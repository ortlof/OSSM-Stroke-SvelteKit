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

class StrokeState
{
public:
    // --- commands ---
    bool run = false;        // true: run the pattern, false: stop the motion
    bool home = false;       // transient: re-run the homing procedure, cleared by the firmware
    float speed = 10.0f;     // linear actuator speed in mm/s
    float depth = 0.0f;      // stroke depth in mm
    float stroke = 0.0f;     // stroke length in mm
    float sensation = 0.0f;  // pattern sensation, -100 .. 100
    int pattern = 0;         // pattern index

    // --- telemetry ---
    String servoState = "disabled";  // UNDEFINED / READY / PATTERN / SETUPDEPTH / STREAMING / STOPPING
    String patternName = "";
    bool homed = false;
    float position = 0.0f;         // current position in mm
    float physicalTravel = 0.0f;   // usable travel in mm, updated after homing
    bool clipping = false;         // engine had to clip a commanded motion

    static void read(StrokeState &state, JsonObject &root)
    {
        root["run"] = state.run;
        root["home"] = state.home;
        root["speed"] = state.speed;
        root["depth"] = state.depth;
        root["stroke"] = state.stroke;
        root["sensation"] = state.sensation;
        root["pattern"] = state.pattern;

        root["servo_state"] = state.servoState;
        root["pattern_name"] = state.patternName;
        root["homed"] = state.homed;
        root["position"] = state.position;
        root["physical_travel"] = state.physicalTravel;
        root["clipping"] = state.clipping;
    }

    static StateUpdateResult update(JsonObject &root, StrokeState &state, const String &originId)
    {
        // A client may post a single field, so every field falls back to the
        // value that is already in the state.
        bool run = root["run"] | state.run;
        bool home = root["home"] | state.home;
        float speed = root["speed"] | state.speed;
        float depth = root["depth"] | state.depth;
        float stroke = root["stroke"] | state.stroke;
        float sensation = root["sensation"] | state.sensation;
        int pattern = root["pattern"] | state.pattern;

        if (run == state.run && home == state.home && speed == state.speed && depth == state.depth &&
            stroke == state.stroke && sensation == state.sensation && pattern == state.pattern)
        {
            return StateUpdateResult::UNCHANGED;
        }

        state.run = run;
        state.home = home;
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
