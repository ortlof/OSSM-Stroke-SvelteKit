#ifndef ModbusProbeService_h
#define ModbusProbeService_h

/*
 *   Modbus RTU link to the JMC iHSV57 servo drive.
 *
 *   The drive's RS232 port is wired to the OSSM v2.3 board's RS232 header, which
 *   goes through an on-board SP3232E transceiver on GPIO 16 (RX) and GPIO 17
 *   (TX). Those two pins are used nowhere else in this firmware.
 *
 *   Three jobs, all on one link and all on one task:
 *     1. identification probe - walks the candidate line settings and reports
 *        which one answers (read only).
 *     2. range read - reads a block of holding registers (function 3) so the
 *        drive's register map can be identified from the browser.
 *     3. torque limit - writes 0x01FE (forward) and 0x01FF (reverse) with
 *        function 6, and always reads both back. A position monitor that polls
 *        the drive's own encoder position and compares it against the firmware's
 *        step count exposes lost steps.
 *
 *   ONE OWNER OF SERIAL2. A UART has one transmitter. This task is the only
 *   code that ever calls Serial2. The HTTP handler does nothing but set request
 *   flags in the state; the task notices them, performs the serial work in
 *   order, and publishes the answer. Two writers on one UART is how a frame
 *   gets corrupted, and a corrupted frame during a torque write is the mistake
 *   that matters here.
 *
 *   A POST to /rest/modbusProbe carries any of these fields:
 *     {"run": true}                                   identification probe
 *     {"read_reg": N, "read_count": M}                range read, function 3
 *     {"torque_forward": X, "torque_reverse": Y, "apply": true}
 *     {"mb_rebaseline": true}                         re-baseline the monitor
 *     {"mb_position_register": R, "mb_counts_per_mm_milli": C,
 *      "mb_enabled": b, "mb_poll_ms": T}              monitor settings
 *   A GET returns the whole state. Nothing is written to flash, so a reboot
 *   returns to the compiled-in defaults.
 *
 *   SAFETY. The only registers this firmware may ever write are 0x01FE and
 *   0x01FF and only with values the guard in ModbusRtu.h accepts. There is no
 *   generic write path and no function 5 or 15. Values out of range are refused
 *   rather than clamped, so the operator always knows what the drive was told.
 */

#include <Arduino.h>

#include <ESP32SvelteKit.h>
#include <HttpEndpoint.h>
#include <StatefulService.h>

#include "ModbusRtu.h"

#define MODBUS_PROBE_ENDPOINT_PATH "/rest/modbusProbe"
#define MODBUS_PROBE_ORIGIN "modbusProbe"

// One drive on one point-to-point link: unit id 1, addressed directly.
#define MODBUS_PROBE_UNIT 1
#define MODBUS_PROBE_TIMEOUT_MS 150

// RX and TX of the board's RS232 header, behind the SP3232E transceiver.
#define MODBUS_PROBE_RX_PIN 16
#define MODBUS_PROBE_TX_PIN 17

// The line that answered in the original probe; every later operation opens the
// port with these settings.
#define MODBUS_PROBE_DEFAULT_BAUD 57600
#define MODBUS_PROBE_DEFAULT_CONFIG SERIAL_8E1

// Poll interval floor for the position monitor: the drive and the link are not
// here to be hammered, and a lost-step figure wants a machine that has moved.
#define MODBUS_PROBE_POLL_MIN_MS 500
#define MODBUS_PROBE_POLL_DEFAULT_MS 500

// The firmware's own step count and its steps-per-mm, scaled by 1000 so the
// monitor can stay in integer arithmetic. Owned by main.cpp (the motion side);
// read here as plain aligned 32-bit values, so no lock is needed.
// Register25 (bit 13 = AL.501, position deviation too large). Found by diffing a
// latched-alarm snapshot against a cleared one: exactly this bit changed. The bit
// reads 0 while the alarm is ACTIVE and 1 when healthy, so it is a NOT-fault flag.
constexpr uint16_t MODBUS_ALARM_REG = 0x0849;
constexpr uint16_t MODBUS_ALARM_BIT = 0x2000;

extern volatile bool modbusProbeInvert;   // mirror of mbInvert, for the poll task
extern volatile bool modbusParamWritePending;
extern volatile int32_t modbusParamWriteReg;
extern volatile int32_t modbusParamWriteValue;

// Torque limits queued by the settings path (main.cpp). The values are
// magnitudes in 0.1 % of rated, 0 .. 3000, and they are meant for the two
// direction-specific limit registers, not for one of them. Set through
// ModbusProbeService::setTorqueLimits(), which is safe to call from any task.
extern volatile bool modbusTorqueLimitsPending;
extern volatile int32_t modbusTorqueLimitsForward;  // magnitude for 0x01FE
extern volatile int32_t modbusTorqueLimitsReverse;  // magnitude for 0x01FF
extern volatile int32_t modbusFirmwareSteps;

extern volatile int32_t modbusFirmwareStepsPerMmX1000;

class ModbusProbeState
{
public:
    // --- requests (writable from a POST; the task consumes them) ---
    bool run = false;          // POST {"run": true} requests one probe
    bool readPending = false;  // a range read has been requested
    int readReg = -1;          // requested start register, -1 = none
    int readCount = 0;         // requested register count
    bool torquePending = false;// a torque write has been requested and validated
    int torqueForwardReq = 0;  // requested forward value (0.1 %)
    int torqueReverseReq = 0;  // requested reverse value (0.1 %)
    bool rebaseline = false;   // transient: re-baseline the position reference

    // --- position monitor settings (writable from a POST) ---
    // Identified on this machine: 0x0836/0x0837 is the feedback pulse sum (d01),
    // 32-bit high word first, in the same pulse units the firmware counts.
    int mbPositionRegister = 0x0836;  // the register's HIGH word; 0xFFFF disables
    int mbCountsPerMmMilli = 84880;   // drive counts per mm x 1000; 0 disables. 84.88 here.
    bool mbInvert = true;             // the drive counts down as the carriage goes up
    bool mbEnabled = true;            // polling on by default now that it is proven
    int mbPollMs = MODBUS_PROBE_POLL_DEFAULT_MS;  // poll interval, floored at 500 ms

    // --- status (owned by the task) ---
    bool running = false;  // a probe is executing right now
    bool done = false;     // a probe has finished since the request
    bool ok = false;       // the last finished probe got a valid reply
    int baud = 0;          // line speed that answered, 0 if nothing did
    String cfg = "";       // framing that answered, "8E1" or "8N1"
    int unit = 0;          // unit id that answered
    int val = 0;           // value of holding register 0x0000
    int treg = 0;          // torque-limit register (0x01FE)
    int tval = -1;         // its raw value, -1 if the read failed
    String tried = "";     // one "baud cfg addrN: result" note per attempt

    // --- range-read result ---
    int rreg = -1;         // register the last read asked for
    int rcount = 0;        // registers the last read asked for
    int rfunc = modbus_rtu::MODBUS_FUNC_READ_HOLDING;  // 3 holding, 4 input
    bool rok = false;      // the read answered with valid data
    int rn = 0;            // number of values published
    int rvals[32] = {0};   // the values, published as a decimal array
    String rhex = "";      // the same values as space-separated hex words
    int rerr = 0;          // Modbus exception code from the read, 0 if none

    // --- torque result ---
    int tfreq = 0;         // forward value last requested
    int trreq = 0;         // reverse value last requested
    int tfread = -1;       // forward value read back, -1 if unknown
    int trread = -1;       // reverse value read back, -1 if unknown
    bool twrote = false;   // both function 6 writes got a valid echo
    bool tok = false;      // both registers were read back successfully

    // --- position monitor result ---
    bool mbok = false;         // the last poll got a valid drive position
    bool alarmKnown = false;   // false: the fault bit is NOT identified yet
    float speedActual = 0.0f;  // d08 motor speed, rpm converted to mm/s
    float speedPeak = 0.0f;    // peak of the last run, mm/s
    float forceN = 0.0f;       // d10 torque feedback as rack force, newtons
    int tqCmd = 0;             // d09 torque command, per mille of rating
    int tqFb = 0;              // d10 torque feedback, per mille
    int tqAvg = 0;             // d11 average torque
    int tqPeak = 0;            // d12 peak torque
    int tqOverload = 0;        // d13 overload load rate
    int tqLimitFwd = 0;        // drive 0x01FE torque limit as a magnitude, 0.1 %
    int tqLimitRev = 0;        // drive 0x01FF torque limit as a magnitude, 0.1 %
    bool tqLimitOk = false;    // the last torque-limit poll got a valid answer
    bool driveStalled = false; // firmware is sending steps the drive is not taking
    int alarmWord = 0;         // raw Register25 value
    int pwReg = -1;            // parameter register the operator asked to write
    int pwVal = 0;             // the value asked for
    int pwReadback = -1;       // what the drive actually holds afterwards
    bool pwOk = false;         // the write was accepted AND read back equal
    int mbReg = -1;            // raw drive register value, -1 if unknown
    int mbFwMmMilli = 0;       // firmware position in milli-mm
    int mbTravelledMilli = 0;  // mm travelled since the reference, milli-mm
    int mbExpected = 0;        // drive counts expected from the mm travelled
    int mbActual = 0;          // drive counts actually seen since the reference
    int mbLost = 0;            // lost counts (expected - actual)
    int mbLostMmMilli = 0;
    bool mbSettled = false;  // the last comparison was taken with the machine at rest     // lost millimetres, milli-mm

    String msg = "";  // short headline for the UI / a rejection reason

    static void read(ModbusProbeState &state, JsonObject &root)
    {
        root["run"] = state.run;
        root["running"] = state.running;
        root["done"] = state.done;
        root["ok"] = state.ok;
        root["baud"] = state.baud;
        root["cfg"] = state.cfg;
        root["unit"] = state.unit;
        root["val"] = state.val;
        root["treg"] = state.treg;
        root["tval"] = state.tval;
        root["tried"] = state.tried;
        root["msg"] = state.msg;

        root["read_reg"] = state.rreg;
        root["read_count"] = state.rcount;
        root["read_func"] = state.rfunc;
        root["read_ok"] = state.rok;
        root["read_err"] = state.rerr;
        root["read_hex"] = state.rhex;
        JsonArray values = root["read_values"].to<JsonArray>();
        for (int i = 0; i < state.rn && i < 32; i++)
        {
            values.add(state.rvals[i]);
        }

        root["torque_forward_requested"] = state.tfreq;
        root["torque_reverse_requested"] = state.trreq;
        root["torque_forward_readback"] = state.tfread;
        root["torque_reverse_readback"] = state.trread;
        root["torque_wrote"] = state.twrote;
        root["torque_ok"] = state.tok;

        root["mb_position_register"] = state.mbPositionRegister;
        root["mb_counts_per_mm_milli"] = state.mbCountsPerMmMilli;
        root["mb_invert"] = state.mbInvert;
        root["mb_settled"] = state.mbSettled;
        root["mb_enabled"] = state.mbEnabled;
        root["mb_poll_ms"] = state.mbPollMs;
        root["mb_ok"] = state.mbok;
        // drive_alarm is deliberately absent: the fault bit in Register25 is not
        // identified. A boolean here would be a guess wearing a warning label.
        root["drive_alarm_known"] = state.alarmKnown;
        root["speed_actual_mm_s"] = state.speedActual;
        root["speed_peak_mm_s"] = state.speedPeak;
        root["force_n"] = state.forceN;
        // The same rack force read as an equivalent hanging mass. This is a unit
        // change, not a weighing: the drive reports an estimated electromagnetic
        // torque, and on a vertical axis the number carries the carriage's own
        // weight and the force to accelerate it as well as any external load.
        root["force_kg"] = state.forceN / 9.80665f;
        root["torque_cmd_pct"] = state.tqCmd / 10.0f;
        root["torque_fb_pct"] = state.tqFb / 10.0f;
        root["torque_avg_pct"] = state.tqAvg / 10.0f;
        root["torque_peak_pct"] = state.tqPeak / 10.0f;
        root["overload_pct"] = state.tqOverload / 10.0f;
        // What the drive actually holds in P05-10 / P05-11 right now, as
        // magnitudes. Read back from the drive, not echoed from the settings.
        root["torque_limit_forward_pct"] = state.tqLimitFwd / 10.0f;
        root["torque_limit_reverse_pct"] = state.tqLimitRev / 10.0f;
        root["torque_limit_ok"] = state.tqLimitOk;
        root["drive_stalled"] = state.driveStalled;
        root["drive_alarm_word"] = state.alarmWord;
        root["param_write_reg"] = state.pwReg;
        root["param_write_value"] = state.pwVal;
        root["param_write_readback"] = state.pwReadback;
        root["param_write_ok"] = state.pwOk;
        root["mb_reg_value"] = state.mbReg;
        root["mb_fw_mm_milli"] = state.mbFwMmMilli;
        root["mb_travelled_mm_milli"] = state.mbTravelledMilli;
        root["mb_expected_counts"] = state.mbExpected;
        root["mb_actual_counts"] = state.mbActual;
        root["mb_lost_counts"] = state.mbLost;
        root["mb_lost_mm_milli"] = state.mbLostMmMilli;
    }

    static StateUpdateResult update(JsonObject &root, ModbusProbeState &state, const String &originId)
    {
        // This runs in the HTTP task under the state mutex and must never touch
        // Serial2. It validates, records the request or the rejection, and the
        // probe task picks the request up on its next pass.
        bool changed = false;

        // --- position monitor settings ---
        int posReg = root["mb_position_register"] | state.mbPositionRegister;
        int countsPerMm = root["mb_counts_per_mm_milli"] | state.mbCountsPerMmMilli;
            bool invert = root["mb_invert"] | state.mbInvert;
        bool mbEnabled = root["mb_enabled"] | state.mbEnabled;
        int pollMs = root["mb_poll_ms"] | state.mbPollMs;
        if (pollMs < MODBUS_PROBE_POLL_MIN_MS)
        {
            pollMs = MODBUS_PROBE_POLL_MIN_MS;  // no faster than the floor
        }
        if (posReg < 0) posReg = 0;
        if (posReg > 0xFFFF) posReg = 0xFFFF;
        if (countsPerMm < 0) countsPerMm = 0;
        if (posReg != state.mbPositionRegister || countsPerMm != state.mbCountsPerMmMilli ||
            mbEnabled != state.mbEnabled || pollMs != state.mbPollMs)
        {
            state.mbPositionRegister = posReg;
            state.mbCountsPerMmMilli = countsPerMm;
            state.mbInvert = invert;
            modbusProbeInvert = invert;
            state.mbEnabled = mbEnabled;
            state.mbPollMs = pollMs;
            changed = true;
        }

        // --- range read request ---
        if (!root["read_reg"].isNull() || !root["read_count"].isNull() || !root["read_func"].isNull())
        {
            int reg = root["read_reg"] | -1;
            int count = root["read_count"] | 0;
            int func = root["read_func"] | (int)modbus_rtu::MODBUS_FUNC_READ_HOLDING;
            if (!modbus_rtu::readRangeValid(reg, count) || !modbus_rtu::readFuncValid(func))
            {
                // Refuse, and say why. A range read is only a read, but a
                // request that lies about what it wants must not be acted on.
                state.rreg = reg;
                state.rcount = count;
                state.rok = false;
                state.rn = 0;
                state.rhex = "";
                state.rerr = 0;
                state.msg = "read rejected: register 0x0000..0x1FFF, count 1..32, func 3 or 4";
                changed = true;
            }
            else
            {
                state.rreg = reg;
                state.rcount = count;
                state.rfunc = func;
                state.readPending = true;
                state.msg = "range read queued";
                changed = true;
            }
        }

        // --- torque write request ---
        if (root["apply"] | false)
        {
            bool hasFwd = !root["torque_forward"].isNull();
            bool hasRev = !root["torque_reverse"].isNull();
            int fwd = root["torque_forward"] | 0;
            int rev = root["torque_reverse"] | 0;
            if (!hasFwd || !hasRev)
            {
                state.tok = false;
                state.twrote = false;
                state.msg = "torque rejected: torque_forward and torque_reverse are both required";
                changed = true;
            }
            else if (!modbus_rtu::torqueLimitValid(fwd) || !modbus_rtu::torqueLimitValid(rev))
            {
                // Not clamped. A torque value is a mechanical safety limit and
                // the operator has to know the request was refused.
                state.tok = false;
                state.twrote = false;
                state.msg = "torque rejected: each value must be 100..3000 (10.0..300.0 %)";
                changed = true;
            }
            else
            {
                state.torqueForwardReq = fwd;
                state.torqueReverseReq = rev;
                state.torquePending = true;
                state.msg = "torque write queued";
                changed = true;
            }
        }

        // --- narrow parameter write: one register, range-checked, read back ---
        if (!root["param_write_reg"].isNull() && !root["param_write_value"].isNull())
        {
            int preg = root["param_write_reg"] | -1;
            int pval = root["param_write_value"] | -1;
            if (preg < 0x0000 || preg > 0x07FF)
            {
                // Parameter space only. The monitor block at 0x0800 and above is
                // deliberately unreachable through this endpoint.
                state.pwOk = false;
                state.msg = "parameter write rejected: address must be 0x0000..0x07FF";
            }
            else if (pval < 0 || pval > 65535)
            {
                state.pwOk = false;
                state.msg = "parameter write rejected: value must be 0..65535";
            }
            else
            {
                modbusParamWriteReg = preg;
                modbusParamWriteValue = pval;
                state.pwReg = preg;
                state.pwVal = pval;
                modbusParamWritePending = true;
                state.msg = "parameter write queued";
            }
            changed = true;
        }

        // --- position monitor re-baseline ---
        if (root["mb_rebaseline"] | false)
        {
            state.rebaseline = true;
            changed = true;
        }

        // --- identification probe ---
        bool run = root["run"] | state.run;
        if (run != state.run)
        {
            state.run = run;
            if (run)
            {
                // A fresh request: drop the previous answer so the UI cannot
                // show a stale result while the new probe runs.
                state.running = true;
                state.done = false;
                state.ok = false;
                state.baud = 0;
                state.cfg = "";
                state.unit = 0;
                state.val = 0;
                state.treg = 0;
                state.tval = -1;
                state.tried = "";
                state.msg = "queued";
            }
            changed = true;
        }

        return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
    }
};

class ModbusProbeService : public StatefulService<ModbusProbeState>
{
public:
    ModbusProbeService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit);

    void begin();

    // Queue a torque-limit write for the probe task. Magnitudes in 0.1 % of
    // rated (0 .. 3000). Forward goes to 0x01FE, reverse to 0x01FF with the
    // drive's negative sign applied. Safe from any task: it only sets flags,
    // the one UART owner does the work.
    void setTorqueLimits(int forwardMagnitude, int reverseMagnitude);

private:
    static void taskEntry(void *param);

    void probeLoop();
    void runProbe();
    void runRangeRead(int reg, int count);
    void runTorqueWrite(int forwardRaw, int reverseRaw);
    void runPositionPoll(int positionRegister, int countsPerMm, bool invert);
    void runParamWrite(int reg, int value);
    void pollAlarm();
    void pollTorque();
    void pollTorqueLimits();
    void openPort();

    HttpEndpoint<ModbusProbeState> _httpEndpoint;
    TaskHandle_t _probeTaskHandle = nullptr;

    // Line settings that answered, reused by every operation after the probe.
    uint32_t _lineBaud = MODBUS_PROBE_DEFAULT_BAUD;
    uint32_t _lineConfig = MODBUS_PROBE_DEFAULT_CONFIG;

    // Position monitor reference and poll bookkeeping, owned by the task.
    bool _posRefValid = false;      // a reference has been taken this cycle
    int32_t _posRefSteps = 0;       // firmware step count at the reference
    uint32_t _lastAlarmMs = 0;      // alarm poll cadence
    uint32_t _lastTorqueMs = 0;     // torque poll cadence
    uint32_t _lastLimitMs = 0;      // torque-limit poll cadence
    int32_t _posRefDrive = 0;       // drive register value at the reference
    int32_t _lastFwSteps = 0;       // previous poll's step count, to detect rest
    int32_t _lastDrive = 0;         // previous poll's drive value
    bool _haveLast = false;         // a previous poll exists to compare against
    int _posAppliedReg = -1;        // config the current cycle was started with
    int _posAppliedCounts = -1;
    uint32_t _lastPollMs = 0;
};

#endif  // ModbusProbeService_h
