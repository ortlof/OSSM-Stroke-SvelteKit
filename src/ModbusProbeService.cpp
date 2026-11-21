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
            // An exception reply is always 5 bytes; a normal one is
            // unit + function + byte count + data + crc. Stop as soon as we have
            // a whole frame instead of waiting out the deadline.
            size_t expected = (buf[1] & 0x80) ? 5 : (3 + (size_t)buf[2] + 2);
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

    // One probe at a time, on core 0 next to the rest of the application logic.
    // Priority 2 stays below the network and the motion tasks; 4 kB is enough for
    // the short String log this task builds.
    xTaskCreatePinnedToCore(taskEntry, "modbusProbe", 4096, this, 2, &_probeTaskHandle, 0);
}

void ModbusProbeService::taskEntry(void *param)
{
    static_cast<ModbusProbeService *>(param)->probeLoop();
}

void ModbusProbeService::probeLoop()
{
    for (;;)
    {
        bool go = false;
        update([&](ModbusProbeState &s)
               {
            if (!s.run)
            {
                return StateUpdateResult::UNCHANGED;
            }
            // Take the request: clear the flag so a later POST can arm a second
            // probe, and mark the run so a GET shows progress.
            s.run = false;
            s.running = true;
            s.tried = "";
            s.msg = "probing";
            go = true;
            return StateUpdateResult::CHANGED; },
               MODBUS_PROBE_ORIGIN);

        if (go)
        {
            runProbe();
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
            break;  // keep the port open for the follow-up read below
        }

        Serial2.end();
    }

    int torque = -1;
    if (found)
    {
        // The torque-limit register the original firmware wrote to. Read only.
        uint8_t reqLen = modbus_rtu::buildReadRequest(request, MODBUS_PROBE_UNIT, MODBUS_PROBE_TORQUE_REG, 1);
        Serial2.write(request, reqLen);
        Serial2.flush();

        exc = 0;
        size_t got = readReply(Serial2, reply, sizeof(reply), MODBUS_PROBE_TIMEOUT_MS);
        modbus_rtu::ReadReply rr = modbus_rtu::checkReadReply(reply, got, MODBUS_PROBE_UNIT, &exc);

        tried += "reg1FE: ";
        if (rr == modbus_rtu::REPLY_OK)
        {
            torque = (int)(uint16_t)(((uint16_t)reply[3] << 8) | reply[4]);
            tried += torque;
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
        s.treg = MODBUS_PROBE_TORQUE_REG;
        s.tval = torque;
        s.tried = tried;
        s.msg = found ? (String("reply on ") + winName) : String("no reply");
        return StateUpdateResult::CHANGED; },
           MODBUS_PROBE_ORIGIN);
}
