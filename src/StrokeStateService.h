#ifndef StrokeStateService_h
#define StrokeStateService_h

/*
 *   Live stroke control and telemetry for the OSSM on ESP32-SvelteKit.
 *
 *   This service carries two kinds of data and keeps them apart on purpose:
 *     - commands: run, speed, depth, stroke, sensation, pattern, record
 *     - telemetry: servo_state, pattern_name, homed, position, physical_travel,
 *       clipping, upload_state
 *
 *   Telemetry is written by the engine synchronisation loop only. That loop uses
 *   the origin "strokeEngine", and the command applier ignores updates with that
 *   origin, so a telemetry push can never re-trigger a motion command.
 */

#include <Arduino.h>

#include <ESP32SvelteKit.h>
#include <EventEndpoint.h>
#include <EventSocket.h>
#include <HttpEndpoint.h>
#include <StatefulService.h>

#define STROKE_STATE_ENDPOINT_PATH "/rest/strokeState"
#define STROKE_STATE_EVENT "stroke"
#define STROKE_ENGINE_ORIGIN "strokeEngine"

// The range the engine really allows, in mm. main.cpp keeps this current: the
// machine settings at boot, the measured travel after homing. The JSON updater
// needs it, and that updater runs under the state mutex, so it arrives as a
// plain global instead of a parameter.
extern volatile float strokeUsableTravelMm;
extern volatile float strokeHomeMarginMm;
extern volatile float strokeFarMarginMm;

class StrokeState
{
public:
    // --- commands ---
    bool run = false;        // true: run the pattern, false: stop the motion
    bool home = false;       // transient: re-run the homing procedure, cleared by the firmware
    bool keepStrokeStart = false;  // when the depth changes, hold the near endpoint and grow the stroke
    int setup = 0;           // interactive depth setup: 0 off, 1 depth, 2 depth with sensation (fancy)
    float speed = 10.0f;     // commanded linear actuator speed in mm/s
    float depth = 0.0f;      // stroke depth in mm
    float stroke = 0.0f;     // stroke length in mm
    float sensation = 0.0f;  // pattern sensation, -100 .. 100
    int pattern = 0;         // pattern index
    // Where the slow part of a pattern starts, as a share of the active stroke.
    // Only the patterns that report uses_slow_start read it.
    float slowStart = 50.0f;
    // Transient: retract fully to the home reference. Cleared by the firmware,
    // like home. It also leaves the pattern, because it is the "get it out"
    // control and leaving run set would let the next command restart the sweep.
    bool pullOut = false;
    // Arm the session archive. This is the operator's record button: while it is
    // set the firmware opens a session on the server, streams samples while the
    // machine runs, and closes the session when it is cleared. Off by default,
    // so nothing is recorded that nobody asked to record - that is the "real
    // play, not test data" rule, held by the design rather than by discipline.
    bool record = false;

    // --- telemetry ---
    String servoState = "disabled";  // UNDEFINED / READY / PATTERN / SETUPDEPTH / STREAMING / STOPPING
    String patternName = "";
    bool homed = false;
    float position = 0.0f;         // current position in mm
    float speedMeasured = 0.0f;    // measured speed in mm/s, from the driver position
    float physicalTravel = 0.0f;   // measured travel in mm between the hard stops
    bool clipping = false;         // engine had to clip a commanded motion
    // True when the running pattern reads slow_start. The clients hide the slider
    // when it is false, instead of offering a control that does nothing.
    bool usesSlowStart = false;

    // --- operator-presence dead-man, written by the firmware in main.cpp ---
    // True when the firmware stopped the motion because the operator's client
    // went away. Cleared when the operator commands a run again. Visible in
    // /rest/strokeState so a stop can always be explained.
    bool safeStop = false;
    // Connected WebSocket clients. This is the presence signal the dead-man
    // uses: a browser or the HUD holds a socket while the operator is there,
    // and the socket closes when they leave.
    int wsClients = 0;

    // --- archive status, written by SessionUploadService ---
    // "off"      disabled in the settings, nothing will be sent
    // "idle"     enabled and connected-ready, no session open
    // "clock"    waiting for NTP, because TLS cannot validate a certificate in 1970
    // "opening"  session open request in flight
    // "recording" session open, samples are being captured and sent
    // "error"    the last request failed, see upload_error
    String uploadState = "off";
    int uploadSession = 0;     // server session id, 0 when none
    int uploadQueued = 0;      // samples waiting in the buffer
    int uploadSent = 0;        // samples accepted by the server this session
    int uploadDropped = 0;     // samples dropped because the buffer was full
    String uploadError = "";   // last error, empty when the last request worked
    // Free stack of the uploader task, from uxTaskGetStackHighWaterMark. This is
    // the measurement that decides whether the task stack is big enough, and a
    // stack overflow here is invisible from outside (it resets the device).
    int uploadStackFree = 0;


    // usable_travel and sweep_start are derived, so they are not stored: a stored
    // copy would lag one synchronisation tick behind depth and stroke, and the
    // browser would briefly show a sweep the machine is not doing.

    static void read(StrokeState &state, JsonObject &root)
    {
        root["run"] = state.run;
        root["home"] = state.home;
        root["keep_stroke_start"] = state.keepStrokeStart;
        root["setup"] = state.setup;
        root["speed"] = state.speed;
        root["depth"] = state.depth;
        root["stroke"] = state.stroke;
        root["sensation"] = state.sensation;
        root["pattern"] = state.pattern;
        root["slow_start"] = state.slowStart;
        root["pull_out"] = state.pullOut;
        root["record"] = state.record;

        root["servo_state"] = state.servoState;
        root["pattern_name"] = state.patternName;
        root["homed"] = state.homed;
        root["position"] = state.position;
        root["speed_measured"] = state.speedMeasured;
        root["physical_travel"] = state.physicalTravel;
        root["usable_travel"] = strokeUsableTravelMm;

        const float sweep = state.depth - state.stroke;
        root["sweep_start"] = (sweep > 0.0f) ? sweep : 0.0f;
        root["clipping"] = state.clipping;
        root["uses_slow_start"] = state.usesSlowStart;

        root["safe_stop"] = state.safeStop;
        root["ws_clients"] = state.wsClients;

        root["upload_state"] = state.uploadState;
        root["upload_session"] = state.uploadSession;
        root["upload_queued"] = state.uploadQueued;
        root["upload_sent"] = state.uploadSent;
        root["upload_dropped"] = state.uploadDropped;
        root["upload_error"] = state.uploadError;
        root["upload_stack_free"] = state.uploadStackFree;
    }

    static StateUpdateResult update(JsonObject &root, StrokeState &state, const String &originId)
    {
        // A client may post a single field, so every field falls back to the
        // value that is already in the state. depth and stroke use -1 as a "not
        // sent" marker, because neither of them can be negative.
        const float depthIn = root["depth"] | -1.0f;
        const float strokeIn = root["stroke"] | -1.0f;
        const bool depthSent = (depthIn >= 0.0f);
        const bool strokeSent = (strokeIn >= 0.0f);
        // slow_start uses -1 as "not sent" for the same reason depth does: 0 is a
        // legal value, so a plain fallback could not tell "set it to 0" from
        // "this client did not mention it".
        const float slowStartIn = root["slow_start"] | -1.0f;

        bool run = root["run"] | state.run;
        bool home = root["home"] | state.home;
        bool pullOut = root["pull_out"] | state.pullOut;
        bool record = root["record"] | state.record;
        bool keepStrokeStart = root["keep_stroke_start"] | state.keepStrokeStart;
        int setup = root["setup"] | state.setup;
        float speed = root["speed"] | state.speed;
        float depth = depthSent ? depthIn : state.depth;
        float stroke = strokeSent ? strokeIn : state.stroke;
        float sensation = root["sensation"] | state.sensation;
        int pattern = root["pattern"] | state.pattern;
        float slowStart = (slowStartIn >= 0.0f) ? constrain(slowStartIn, 0.0f, 100.0f) : state.slowStart;

        // Option "keep the stroke start": when only the depth arrives, hold the
        // near endpoint of the sweep and let the stroke grow instead. The sweep
        // runs between (depth - stroke) and depth, so the whole motion would
        // otherwise slide forward with the depth.
        if (keepStrokeStart && depthSent && !strokeSent)
        {
            const float anchored = depth - (state.depth - state.stroke);
            // A depth below the old near endpoint cannot hold that endpoint, so
            // fall back to the normal behaviour instead of collapsing the stroke
            // to zero, which would stop the machine without saying why.
            if (anchored > 0.0f)
            {
                stroke = anchored;
            }
        }

        // Consistency guard. The engine confines the sweep to [0, usable] and
        // clamps silently, so keep both values inside that range here. Otherwise
        // a slider position and the machine would disagree.
        //
        // The near end of the sweep keeps a margin of its own away from the home
        // position, because a sweep that starts at 0 runs back to the homing
        // point on every stroke, and homing can stop a little late. That also
        // means a stroke can never be as long as the depth.
        const float usable = strokeUsableTravelMm;
        const float margin = (strokeHomeMarginMm < usable) ? strokeHomeMarginMm : usable;
        const float farSpace = (strokeFarMarginMm < usable) ? strokeFarMarginMm : 0.0f;
        float maxDepth = usable - farSpace;
        if (maxDepth < 0.0f) maxDepth = 0.0f;
        depth = constrain(depth, 0.0f, maxDepth);
        float maxStroke = depth - margin;
        if (maxStroke < 0.0f) maxStroke = 0.0f;
        stroke = constrain(stroke, 0.0f, maxStroke);

        if (run == state.run && home == state.home && pullOut == state.pullOut &&
            record == state.record &&
            keepStrokeStart == state.keepStrokeStart &&
            setup == state.setup &&
            speed == state.speed && depth == state.depth &&
            stroke == state.stroke && sensation == state.sensation && pattern == state.pattern &&
            slowStart == state.slowStart)
        {
            return StateUpdateResult::UNCHANGED;
        }

        state.run = run;
        state.home = home;
        state.pullOut = pullOut;
        state.record = record;
        state.keepStrokeStart = keepStrokeStart;
        state.setup = setup;
        state.speed = speed;
        state.depth = depth;
        state.stroke = stroke;
        state.sensation = sensation;
        state.pattern = pattern;
        state.slowStart = slowStart;
        return StateUpdateResult::CHANGED;
    }
};

class StrokeStateService : public StatefulService<StrokeState>
{
public:
    StrokeStateService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit);

    void begin();

private:
    HttpEndpoint<StrokeState> _httpEndpoint;
    EventEndpoint<StrokeState> _eventEndpoint;
};

#endif  // StrokeStateService_h
