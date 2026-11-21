#ifndef ModbusProbeService_h
#define ModbusProbeService_h

/*
 *   Read-only Modbus RTU probe for the JMC iHSV57 servo drive.
 *
 *   The drive's RS232 port is wired to the OSSM v2.3 board's RS232 header, which
 *   goes through an on-board SP3232E transceiver on GPIO 16 (RX) and GPIO 17
 *   (TX). Those two pins are used nowhere else in this firmware.
 *
 *   This service only ever reads. It exists to prove the link before anyone
 *   writes a single register to the drive: every frame it sends is a function 3
 *   (read holding registers) request, so it cannot change anything on the drive.
 *
 *   A POST of {"run": true} to /rest/modbusProbe asks for one probe. A task
 *   notices the flag, walks the candidate line settings one at a time, and
 *   publishes the result here so the browser can read it back with a GET. A
 *   later POST arms a second probe; nothing is written to flash, so a reboot
 *   simply returns to the compiled-in defaults.
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
// The register the original firmware wrote its torque limit to. Read here, never
// written.
#define MODBUS_PROBE_TORQUE_REG 0x01FE

// RX and TX of the board's RS232 header, behind the SP3232E transceiver.
#define MODBUS_PROBE_RX_PIN 16
#define MODBUS_PROBE_TX_PIN 17

class ModbusProbeState
{
public:
    // --- command ---
    bool run = false;  // POST {"run": true} requests one probe

    // --- status ---
    bool running = false;  // a probe is executing right now
    bool done = false;     // a probe has finished since the request
    bool ok = false;       // the last finished probe got a valid reply

    // --- result ---
    int baud = 0;       // line speed that answered, 0 if nothing did
    String cfg = "";    // framing that answered, "8E1" or "8N1"
    int unit = 0;       // unit id that answered
    int val = 0;        // value of holding register 0x0000
    int treg = 0;       // torque-limit register (0x01FE)
    int tval = -1;      // its raw value, -1 if the read failed
    String tried = "";  // one "baud cfg addrN: result" note per attempt
    String msg = "";    // short headline for the UI

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
    }

    static StateUpdateResult update(JsonObject &root, ModbusProbeState &state, const String &originId)
    {
        // Only the request flag is writable. The task owns every other field, so
        // a client cannot fake a result.
        bool run = root["run"] | state.run;
        if (run == state.run)
        {
            return StateUpdateResult::UNCHANGED;
        }

        state.run = run;
        if (run)
        {
            // A fresh request: drop the previous answer so the UI cannot show a
            // stale result while the new probe runs.
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
        return StateUpdateResult::CHANGED;
    }
};

class ModbusProbeService : public StatefulService<ModbusProbeState>
{
public:
    ModbusProbeService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit);

    void begin();

private:
    static void taskEntry(void *param);

    void probeLoop();
    void runProbe();

    HttpEndpoint<ModbusProbeState> _httpEndpoint;
    TaskHandle_t _probeTaskHandle = nullptr;
};

#endif  // ModbusProbeService_h
