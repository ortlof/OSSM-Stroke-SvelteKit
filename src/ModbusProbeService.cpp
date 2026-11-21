#include <ModbusProbeService.h>

namespace
{

// Candidate line settings, tried in this order. The iHSV57 is usually shipped at
// 57600 8E1; the rest are cheap to try and cost one timeout each.
struct ProbeLine
{
    uint32_t baud;
    uint32_t cfg;
    const char *name;     // "57600 8E1", for the log
    const char *framing;  // "8E1", for the result fields
};

const ProbeLine PROBE_LINES[] = {
    {57600, SERIAL_8E1, "57600 8E1", "8E1"},
    {9600, SERIAL_8E1, "9600 8E1", "8E1"},
    {19200, SERIAL_8E1, "19200 8E1", "8E1"},
    {25600, SERIAL_8E1, "25600 8E1", "8E1"},
    {115200, SERIAL_8E1, "115200 8E1", "8E1"},
    {57600, SERIAL_8N1, "57600 8N1", "8N1"},
};
const size_t PROBE_LINE_COUNT = sizeof(PROBE_LINES) / sizeof(PROBE_LINES[0]);

// Read a reply with a short deadline. Stops as soon as the frame announced by
// the byte count is complete, otherwise gives up after timeoutMs. The 3.5
// character silent interval is not modelled; the caller flushes before every
// request and keeps one request in flight at a time.
size_t readReply(HardwareSerial &port, uint8_t *buf, size_t maxLen, uint32_t timeoutMs)
{
    size_t got = 0;
    uint32_t start = millis();
    while (millis() - start < timeoutMs)
    {
        while (port.available() && got < maxLen)
        {
            buf[got++] = (uint8_t)port.read();
        }
        if (got >= 5)
        {
            // An exception reply is always five bytes. A function 3 reply
            // announces its data length in the byte-count field; a function 6
            // reply is a fixed eight-byte echo. Stop as soon as it is complete.
            size_t expected;
            if (buf[1] & 0x80)
            {
                expected = 5;
            }
            else if ((buf[1] & 0x7F) == 0x03)
            {
                expected = 3 + (size_t)buf[2] + 2;
            }
            else
            {
                expected = 8;  // function 6 echo
            }
            if (expected > maxLen)
            {
                expected = maxLen;
            }
            if (got >= expected)
            {
                break;
            }
        }
        vTaskDelay(1);  // one tick between polls; the UART buffers the frame
    }
    return got;
}

// Short text for the log line. Exception codes get their meaning appended.
String resultText(modbus_rtu::ReadReply rr, uint8_t exc)
{
    switch (rr)
    {
    case modbus_rtu::REPLY_OK:
        return "ok";
    case modbus_rtu::REPLY_BAD_CRC:
        return "crc";
    case modbus_rtu::REPLY_BAD_UNIT:
        return "unit";
    case modbus_rtu::REPLY_BAD_FUNCTION:
        return "func";
    case modbus_rtu::REPLY_BAD_ECHO:
        return "echo";
    case modbus_rtu::REPLY_EXCEPTION:
        return String("exc ") + String(exc, HEX) + " " + modbus_rtu::exceptionText(exc);
    case modbus_rtu::REPLY_TOO_SHORT:
    default:
        return "timeout";
    }
}

}  // namespace

ModbusProbeService::ModbusProbeService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit) : _httpEndpoint(
                                                                                                    ModbusProbeState::read,
                                                                                                    ModbusProbeState::update,
                                                                                                    this,
                                                                                                    server,
                                                                                                    MODBUS_PROBE_ENDPOINT_PATH,
                                                                                                    sveltekit->getSecurityManager(),
                                                                                                    AuthenticationPredicates::IS_AUTHENTICATED)
{
}

void ModbusProbeService::begin()
{
    _httpEndpoint.begin();

    // One job at a time, on core 0 next to the rest of the application logic.
    // Priority 2 stays below the network and the motion tasks; 4 kB is enough for
    // the short String log this task builds.
    xTaskCreatePinnedToCore(taskEntry, "modbusProbe", 4096, this, 2, &_probeTaskHandle, 0);
}

void ModbusProbeService::taskEntry(void *param)
{
    static_cast<ModbusProbeService *>(param)->probeLoop();
}

// The single owner of Serial2. Open the port with the line that answered, then
// drop anything a previous request left in the receive buffer.
void ModbusProbeService::openPort()
{
    Serial2.begin(_lineBaud, _lineConfig, MODBUS_PROBE_RX_PIN, MODBUS_PROBE_TX_PIN);
    vTaskDelay(1);  // let the UART settle before the first byte
    while (Serial2.available())
    {
        Serial2.read();
    }
}

void ModbusProbeService::probeLoop()
{
    for (;;)
    {
        // Pick up whatever the HTTP task queued, under the state lock, and clear
        // the flags. Nothing here touches Serial2; that happens below, in order.
        bool doProbe = false;
        bool doRead = false;
        int readReg = 0;
        int readCount = 0;
        bool doTorque = false;
        int forwardRaw = 0;
        int reverseRaw = 0;
        bool doRebaseline = false;
        int posReg = 0xFFFF;
        int countsPerMm = 0;
        bool mbEnabled = false;
        int pollMs = MODBUS_PROBE_POLL_DEFAULT_MS;

        update([&](ModbusProbeState &s)
               {
            bool changed = false;

            if (s.run)
            {
                // Take the request: clear the flag so a later POST can arm a
                // second probe, and mark the run so a GET shows progress.
                s.run = false;
                s.running = true;
                s.tried = "";
                s.msg = "probing";
                doProbe = true;
                changed = true;
            }
            if (s.readPending)
            {
                s.readPending = false;
                readReg = s.rreg;
                readCount = s.rcount;
                doRead = true;
                changed = true;
            }
            if (s.torquePending)
            {
                s.torquePending = false;
                forwardRaw = s.torqueForwardReq;
                reverseRaw = s.torqueReverseReq;
                doTorque = true;
                changed = true;
            }
            if (s.rebaseline)
            {
                s.rebaseline = false;
                doRebaseline = true;
                changed = true;
            }

            posReg = s.mbPositionRegister;
            countsPerMm = s.mbCountsPerMmMilli;
            mbEnabled = s.mbEnabled;
            pollMs = s.mbPollMs;

            return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED; },
               MODBUS_PROBE_ORIGIN);

        // Every serial operation runs on this one task, one at a time.
        if (doProbe)
        {
            runProbe();
        }
        if (doRead)
        {
            runRangeRead(readReg, readCount);
        }
        if (doTorque)
        {
            runTorqueWrite(forwardRaw, reverseRaw);
        }
        if (doRebaseline)
        {
            _posRefValid = false;
        }

        // Position monitor: one poll on this task, never faster than the floor.
        if (mbEnabled && posReg != 0xFFFF && countsPerMm > 0)
        {
            if (posReg != _posAppliedReg || countsPerMm != _posAppliedCounts)
            {
                // New settings: start a fresh cycle and take the reference on
                // the first successful poll.
                _posAppliedReg = posReg;
                _posAppliedCounts = countsPerMm;
                _posRefValid = false;
                _lastPollMs = millis() - (uint32_t)pollMs;  // poll now
            }
            uint32_t now = millis();
            if ((uint32_t)(now - _lastPollMs) >= (uint32_t)pollMs)
            {
                runPositionPoll(posReg, countsPerMm, modbusProbeInvert);
                _lastPollMs = millis();
            }
        }
        else if (_posAppliedReg != -1)
        {
            // Monitor off: forget the cycle so a later enable re-baselines.
            _posAppliedReg = -1;
            _posAppliedCounts = -1;
            _posRefValid = false;
        }

        // Plain non-blocking loop: never sit on the UART or the state lock.
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void ModbusProbeService::runProbe()
{
    String tried;
    uint8_t request[MODBUS_RTU_REQUEST_LEN];
    uint8_t reply[64];
    uint8_t exc = 0;

    bool found = false;
    uint16_t value = 0;
    int winBaud = 0;
    const char *winFraming = "";
    String winName;

    for (size_t i = 0; i < PROBE_LINE_COUNT; i++)
    {
        const ProbeLine &line = PROBE_LINES[i];

        Serial2.begin(line.baud, line.cfg, MODBUS_PROBE_RX_PIN, MODBUS_PROBE_TX_PIN);
        vTaskDelay(1);  // let the UART settle before the first byte
        while (Serial2.available())
        {
            Serial2.read();  // drop whatever the previous line setting left behind
        }

        uint8_t reqLen = modbus_rtu::buildReadRequest(request, MODBUS_PROBE_UNIT, 0x0000, 2);
        Serial2.write(request, reqLen);
        Serial2.flush();

        size_t got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
        modbus_rtu::ReadReply rr = modbus_rtu::checkReadReply(reply, got, MODBUS_PROBE_UNIT, &exc);

        tried += line.name;
        tried += " addr";
        tried += MODBUS_PROBE_UNIT;
        tried += ": ";
        tried += resultText(rr, exc);
        tried += "; ";

        if (rr == modbus_rtu::REPLY_OK)
        {
            // A function 3 reply carries count*2 data bytes after the byte count.
            value = (uint16_t)(((uint16_t)reply[3] << 8) | reply[4]);
            found = true;
            winBaud = (int)line.baud;
            winFraming = line.framing;
            winName = line.name;
            // Remember the line for every later operation.
            _lineBaud = line.baud;
            _lineConfig = line.cfg;
            break;  // keep the port open for the follow-up read below
        }

        Serial2.end();
    }

    int torque = -1;
    int torqueRev = -1;
    if (found)
    {
        // The two torque-limit registers the original firmware wrote to. Read
        // both here so the state always shows what the drive currently holds,
        // even when no write is requested; the write path lives in
        // runTorqueWrite().
        uint8_t reqLen = modbus_rtu::buildReadRequest(request, MODBUS_PROBE_UNIT, MODBUS_TORQUE_REG_FORWARD, 2);
        Serial2.write(request, reqLen);
        Serial2.flush();

        exc = 0;
        size_t got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
        modbus_rtu::ReadReply rr = modbus_rtu::checkReadReply(reply, got, MODBUS_PROBE_UNIT, &exc);

        tried += "reg1FE: ";
        if (rr == modbus_rtu::REPLY_OK)
        {
            torque = (int)(uint16_t)(((uint16_t)reply[3] << 8) | reply[4]);
            torqueRev = (int)(uint16_t)(((uint16_t)reply[5] << 8) | reply[6]);
            tried += torque;
            tried += "/";
            tried += torqueRev;
        }
        else
        {
            tried += resultText(rr, exc);
        }
        tried += "; ";

        Serial2.end();
    }

    // Publish the outcome. running goes back to false and done to true on every
    // path, including a total miss, so the UI never waits forever.
    update([&](ModbusProbeState &s)
           {
        s.running = false;
        s.done = true;
        s.ok = found;
        s.baud = winBaud;
        s.cfg = winFraming;
        s.unit = found ? MODBUS_PROBE_UNIT : 0;
        s.val = (int)value;
        s.treg = MODBUS_TORQUE_REG_FORWARD;
        s.tval = torque;
        // What the drive holds right now, whether or not a write was requested.
        s.tfread = torque;
        s.trread = torqueRev;
        s.tried = tried;
        s.msg = found ? (String("reply on ") + winName) : String("no reply");
        return StateUpdateResult::CHANGED; },
           MODBUS_PROBE_ORIGIN);
}

// Read a block of holding registers with function 3 and publish every value as a
// decimal in an array plus the same words as hex. Read only: no register state
// on the drive changes.
void ModbusProbeService::runRangeRead(int reg, int count)
{
    // The function comes from the request (3 holding or 4 input). Read only,
    // either way: no register on the drive changes.
    int func = modbus_rtu::MODBUS_FUNC_READ_HOLDING;
    update([&](ModbusProbeState &s)
           { func = s.rfunc; return StateUpdateResult::UNCHANGED; },
           MODBUS_PROBE_ORIGIN);
    openPort();

    uint8_t request[MODBUS_RTU_REQUEST_LEN];
    uint8_t reply[MODBUS_RTU_READ_REPLY_MAX];
    uint8_t exc = 0;

    uint8_t reqLen = modbus_rtu::buildReadRequest(request, MODBUS_PROBE_UNIT, (uint16_t)reg, (uint16_t)count, (uint8_t)func);
    // Discard anything left in the receive buffer before asking, so what we parse
    // is the reply to THIS request. Without it a moving machine leaves stale frames
    // behind and a read silently returns the previous answer - which reads as a
    // plausible number and is the worst kind of wrong.
    while (Serial2.available())
    {
        Serial2.read();
    }
    Serial2.write(request, reqLen);
    Serial2.flush();

    size_t got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
    modbus_rtu::ReadReply rr = modbus_rtu::checkReadReply(reply, got, MODBUS_PROBE_UNIT, &exc, (uint8_t)func);

    int values[32] = {0};
    int n = 0;
    bool ok = false;
    int err = 0;
    String hex;
    hex.reserve(32 * 5);

    if (rr == modbus_rtu::REPLY_OK)
    {
        int byteCount = reply[2];
        n = byteCount / 2;
        if (n > count) n = count;
        if (n > MODBUS_READ_COUNT_MAX) n = MODBUS_READ_COUNT_MAX;
        char word[8];
        for (int i = 0; i < n; i++)
        {
            values[i] = ((int)reply[3 + 2 * i] << 8) | (int)reply[4 + 2 * i];
            snprintf(word, sizeof(word), "%04X", values[i]);
            if (i)
            {
                hex += ' ';
            }
            hex += word;
        }
        ok = true;
    }
    else
    {
        err = exc;
    }

    update([&](ModbusProbeState &s)
           {
        s.rreg = reg;
        s.rcount = count;
        s.rok = ok;
        s.rn = n;
        for (int i = 0; i < n; i++)
        {
            s.rvals[i] = values[i];
        }
        s.rhex = hex;
        s.rerr = err;
        if (ok)
        {
            char head[48];
            snprintf(head, sizeof(head), "read %d reg @0x%04X", n, reg);
            s.msg = head;
        }
        else
        {
            s.msg = String("read failed: ") + resultText(rr, err);
        }
        return StateUpdateResult::CHANGED; },
           MODBUS_PROBE_ORIGIN);
}

// Write the two torque-limit registers with function 6, then always read both
// back. The values arrived already validated and inside [100, 3000]; this
// function cannot be reached with anything else.
void ModbusProbeService::runTorqueWrite(int forwardRaw, int reverseRaw)
{
    openPort();

    uint8_t request[MODBUS_RTU_REQUEST_LEN];
    uint8_t reply[16];
    uint8_t exc = 0;

    // --- forward, 0x01FE ---
    uint8_t reqLen = modbus_rtu::buildWriteRequest(request, MODBUS_PROBE_UNIT, MODBUS_TORQUE_REG_FORWARD, (uint16_t)forwardRaw);
    // Discard anything left in the receive buffer before asking, so what we parse
    // is the reply to THIS request. Without it a moving machine leaves stale frames
    // behind and a read silently returns the previous answer - which reads as a
    // plausible number and is the worst kind of wrong.
    while (Serial2.available())
    {
        Serial2.read();
    }
    Serial2.write(request, reqLen);
    Serial2.flush();
    size_t got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
    modbus_rtu::ReadReply rrF = modbus_rtu::checkWriteReply(reply, got, MODBUS_PROBE_UNIT,
                                                            MODBUS_TORQUE_REG_FORWARD, (uint16_t)forwardRaw, &exc);

    // --- reverse, 0x01FF ---
    exc = 0;
    reqLen = modbus_rtu::buildWriteRequest(request, MODBUS_PROBE_UNIT, MODBUS_TORQUE_REG_REVERSE, (uint16_t)reverseRaw);
    // Discard anything left in the receive buffer before asking, so what we parse
    // is the reply to THIS request. Without it a moving machine leaves stale frames
    // behind and a read silently returns the previous answer - which reads as a
    // plausible number and is the worst kind of wrong.
    while (Serial2.available())
    {
        Serial2.read();
    }
    Serial2.write(request, reqLen);
    Serial2.flush();
    got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
    modbus_rtu::ReadReply rrR = modbus_rtu::checkWriteReply(reply, got, MODBUS_PROBE_UNIT,
                                                            MODBUS_TORQUE_REG_REVERSE, (uint16_t)reverseRaw, &exc);

    // --- read both back, whatever the writes answered ---
    int fread = -1;
    int rread = -1;
    exc = 0;
    reqLen = modbus_rtu::buildReadRequest(request, MODBUS_PROBE_UNIT, MODBUS_TORQUE_REG_FORWARD, 2);
    // Discard anything left in the receive buffer before asking, so what we parse
    // is the reply to THIS request. Without it a moving machine leaves stale frames
    // behind and a read silently returns the previous answer - which reads as a
    // plausible number and is the worst kind of wrong.
    while (Serial2.available())
    {
        Serial2.read();
    }
    Serial2.write(request, reqLen);
    Serial2.flush();
    got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
    modbus_rtu::ReadReply rrBack = modbus_rtu::checkReadReply(reply, got, MODBUS_PROBE_UNIT, &exc);
    if (rrBack == modbus_rtu::REPLY_OK)
    {
        fread = ((int)reply[3] << 8) | (int)reply[4];
        rread = ((int)reply[5] << 8) | (int)reply[6];
    }

    bool wrote = (rrF == modbus_rtu::REPLY_OK) && (rrR == modbus_rtu::REPLY_OK);
    bool readbackOk = (rrBack == modbus_rtu::REPLY_OK);

    update([&](ModbusProbeState &s)
           {
        s.tfreq = forwardRaw;
        s.trreq = reverseRaw;
        s.tfread = fread;
        s.trread = rread;
        s.twrote = wrote;
        s.tok = readbackOk;
        if (readbackOk)
        {
            char head[56];
            snprintf(head, sizeof(head), "torque now %d/%d", fread, rread);
            s.msg = head;
        }
        else
        {
            s.msg = wrote ? String("torque written, read-back failed") : String("torque write failed");
        }
        return StateUpdateResult::CHANGED; },
           MODBUS_PROBE_ORIGIN);
}

// Poll the drive's own encoder position and compare it against the firmware's
// step count. Both are integer counts: the difference is the lost steps nothing
// else in this project can see. All arithmetic is integer; no float formatting.
volatile bool modbusProbeInvert = true;

void ModbusProbeService::runPositionPoll(int positionRegister, int countsPerMm, bool invert)
{
    openPort();

    uint8_t request[MODBUS_RTU_REQUEST_LEN];
    uint8_t reply[16];
    uint8_t exc = 0;

    // The position monitors are 32-bit values served as two registers, high word
    // first. Reading one register would wrap every 327 mm of travel.
    uint8_t reqLen = modbus_rtu::buildReadRequest(request, MODBUS_PROBE_UNIT, (uint16_t)positionRegister, 2);
    // Discard anything left in the receive buffer before asking, so what we parse
    // is the reply to THIS request. Without it a moving machine leaves stale frames
    // behind and a read silently returns the previous answer - which reads as a
    // plausible number and is the worst kind of wrong.
    while (Serial2.available())
    {
        Serial2.read();
    }
    Serial2.write(request, reqLen);
    Serial2.flush();

    size_t got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
    // Sample the firmware's step count as close to the reply as possible.
    int32_t fwSteps = modbusFirmwareSteps;
    modbus_rtu::ReadReply rr = modbus_rtu::checkReadReply(reply, got, MODBUS_PROBE_UNIT, &exc);

    bool ok = (rr == modbus_rtu::REPLY_OK);
    int32_t drive = ok ? (int32_t)(((uint32_t)reply[3] << 24) | ((uint32_t)reply[4] << 16) |
                                   ((uint32_t)reply[5] << 8) | (uint32_t)reply[6]) : -1;

    int32_t stepsPerMmX1000 = modbusFirmwareStepsPerMmX1000;
    if (stepsPerMmX1000 < 1)
    {
        stepsPerMmX1000 = 1;
    }

    // milli-mm = steps * 1000 / (stepsPerMmX1000 / 1000) = steps * 1e6 / spmX1000
    int fwMmMilli = (int)(((int64_t)fwSteps * 1000000) / stepsPerMmX1000);
    int travelledMilli = 0;
    int expected = 0;
    int actual = 0;
    int lost = 0;
    int lostMmMilli = 0;

    // Lost steps only mean anything when the machine is standing still. While it
    // moves, the drive legitimately trails the command (that is the following
    // error) and the two counters are sampled a few milliseconds apart, so a
    // comparison made mid-move measures the motion, not a fault. Two polls that
    // read identical values from both sources mean rest.
    bool atRest = _haveLast && (fwSteps == _lastFwSteps) && (drive == _lastDrive);
    _lastFwSteps = fwSteps;
    _lastDrive = drive;
    _haveLast = ok;

    if (ok && atRest && !_posRefValid)
    {
        // A new poll cycle starts: this drive reading and the firmware step
        // count at this instant become the reference everything is measured
        // against until a re-baseline or a settings change.
        _posRefValid = true;
        _posRefSteps = fwSteps;
        _posRefDrive = drive;
    }

    if (ok && atRest && _posRefValid)
    {
        int32_t stepsSinceRef = fwSteps - _posRefSteps;
        travelledMilli = (int)(((int64_t)stepsSinceRef * 1000000) / stepsPerMmX1000);
        // countsPerMm is carried in thousandths, so 84.88 counts per mm is 84880
        // and the arithmetic stays exact in integers.
        expected = (int)(((int64_t)travelledMilli * countsPerMm) / 1000000);
        // The drive counts down as the carriage goes up on this machine, so the
        // reference is subtracted the other way round when mbInvert is set.
        actual = invert ? (int)(_posRefDrive - drive) : (int)(drive - _posRefDrive);
        lost = expected - actual;
        if (countsPerMm != 0)
        {
            lostMmMilli = (int)(((int64_t)lost * 1000000) / countsPerMm);
        }
    }

    update([&](ModbusProbeState &s)
           {
        s.mbok = ok;
        s.mbReg = ok ? (int)drive : -1;
        s.mbSettled = atRest;
        s.mbFwMmMilli = fwMmMilli;
        s.mbTravelledMilli = travelledMilli;
        s.mbExpected = expected;
        s.mbActual = actual;
        s.mbLost = lost;
        s.mbLostMmMilli = lostMmMilli;
        return StateUpdateResult::CHANGED; },
           MODBUS_PROBE_ORIGIN);
}
