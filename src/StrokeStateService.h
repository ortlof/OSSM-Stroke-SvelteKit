#ifndef StrokeStateService_h
#define StrokeStateService_h

/*
 *   Live stroke control and telemetry for the OSSM on ESP32-SvelteKit.
 *
 *   This service carries two kinds of data and keeps them apart on purpose:
 *     - commands: run, speed, depth, stroke, sensation, pattern
 *     - telemetry: servo_state, pattern_name, homed, position, physical_travel, clipping
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

    // --- telemetry ---
    String servoState = "disabled";  // UNDEFINED / READY / PATTERN / SETUPDEPTH / STREAMING / STOPPING
    String patternName = "";
    bool homed = false;
    float position = 0.0f;         // current position in mm
    float speedMeasured = 0.0f;    // measured speed in mm/s, from the driver position
    float physicalTravel = 0.0f;   // measured travel in mm between the hard stops
    bool clipping = false;         // engine had to clip a commanded motion


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

        bool run = root["run"] | state.run;
        bool home = root["home"] | state.home;
        bool keepStrokeStart = root["keep_stroke_start"] | state.keepStrokeStart;
        int setup = root["setup"] | state.setup;
        float speed = root["speed"] | state.speed;
        float depth = depthSent ? depthIn : state.depth;
        float stroke = strokeSent ? strokeIn : state.stroke;
        float sensation = root["sensation"] | state.sensation;
        int pattern = root["pattern"] | state.pattern;

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

        if (run == state.run && home == state.home && keepStrokeStart == state.keepStrokeStart &&
            setup == state.setup &&
            speed == state.speed && depth == state.depth &&
            stroke == state.stroke && sensation == state.sensation && pattern == state.pattern)
        {
            return StateUpdateResult::UNCHANGED;
        }

        state.run = run;
        state.home = home;
        state.keepStrokeStart = keepStrokeStart;
        state.setup = setup;
        state.speed = speed;
        state.depth = depth;
        state.stroke = stroke;
        state.sensation = sensation;
        state.pattern = pattern;
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
