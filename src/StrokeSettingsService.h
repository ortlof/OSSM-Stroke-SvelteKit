#ifndef StrokeSettingsService_h
#define StrokeSettingsService_h

/*
 *   Machine and motor configuration for the OSSM.
 *
 *   These values replace what the original firmware kept in EEPROM
 *   (src/OSSM_Config.h) and they are persisted as JSON on the LittleFS
 *   partition, so they survive a firmware update and can be edited from the
 *   browser. They are read once during start-up, because the motion driver
 *   needs them when it is created. A change therefore needs a restart.
 */

#include <Arduino.h>

#include <ESP32SvelteKit.h>
#include <FSPersistence.h>
#include <HttpEndpoint.h>
#include <StatefulService.h>

#define STROKE_SETTINGS_ENDPOINT_PATH "/rest/strokeSettings"
#define STROKE_SETTINGS_FILE_PATH "/config/strokeSettings.json"

#ifndef OSSM_STEP_PER_MM
#define OSSM_STEP_PER_MM 78.64f
#endif
#ifndef OSSM_MAX_SPEED_MM_S
#define OSSM_MAX_SPEED_MM_S 300.0f
#endif
#ifndef OSSM_MAX_ACCEL_MM_S2
#define OSSM_MAX_ACCEL_MM_S2 10000.0f
#endif
#ifndef OSSM_PHYSICAL_TRAVEL_MM
#define OSSM_PHYSICAL_TRAVEL_MM 450.0f
#endif
#ifndef OSSM_KEEPOUT_MM
#define OSSM_KEEPOUT_MM 15.0f
#endif
#ifndef OSSM_USER_SPEEDLIMIT_MM_S
#define OSSM_USER_SPEEDLIMIT_MM_S 300.0f
#endif

class StrokeSettings
{
public:
    // machine geometry
    float physical_travel = OSSM_PHYSICAL_TRAVEL_MM;  // mm between the hard end stops
    float keepout = OSSM_KEEPOUT_MM;                  // mm of soft end stop at both ends
    // motor and drive train
    float steps_per_mm = OSSM_STEP_PER_MM;
    float max_speed = OSSM_MAX_SPEED_MM_S;            // mm/s, physical limit of the machine
    float max_acceleration = OSSM_MAX_ACCEL_MM_S2;    // mm/s^2
    float user_speed_limit = OSSM_USER_SPEEDLIMIT_MM_S;  // mm/s, ceiling for the UI slider
    bool invert_direction = true;
    bool sensorless_homing = true;  // true: current sensing, false: end stop switch

    static void read(StrokeSettings &settings, JsonObject &root)
    {
        root["physical_travel"] = settings.physical_travel;
        root["keepout"] = settings.keepout;
        root["steps_per_mm"] = settings.steps_per_mm;
        root["max_speed"] = settings.max_speed;
        root["max_acceleration"] = settings.max_acceleration;
        root["user_speed_limit"] = settings.user_speed_limit;
        root["invert_direction"] = settings.invert_direction;
        root["sensorless_homing"] = settings.sensorless_homing;
    }

    static StateUpdateResult update(JsonObject &root, StrokeSettings &settings, const String &originId)
    {
        float physical_travel = root["physical_travel"] | settings.physical_travel;
        float keepout = root["keepout"] | settings.keepout;
        float steps_per_mm = root["steps_per_mm"] | settings.steps_per_mm;
        float max_speed = root["max_speed"] | settings.max_speed;
        float max_acceleration = root["max_acceleration"] | settings.max_acceleration;
        float user_speed_limit = root["user_speed_limit"] | settings.user_speed_limit;
        bool invert_direction = root["invert_direction"] | settings.invert_direction;
        bool sensorless_homing = root["sensorless_homing"] | settings.sensorless_homing;

        if (physical_travel == settings.physical_travel && keepout == settings.keepout &&
            steps_per_mm == settings.steps_per_mm && max_speed == settings.max_speed &&
            max_acceleration == settings.max_acceleration &&
            user_speed_limit == settings.user_speed_limit &&
            invert_direction == settings.invert_direction &&
            sensorless_homing == settings.sensorless_homing)
        {
            return StateUpdateResult::UNCHANGED;
        }

        settings.physical_travel = physical_travel;
        settings.keepout = keepout;
        settings.steps_per_mm = steps_per_mm;
        settings.max_speed = max_speed;
        settings.max_acceleration = max_acceleration;
        settings.user_speed_limit = user_speed_limit;
        settings.invert_direction = invert_direction;
        settings.sensorless_homing = sensorless_homing;
        return StateUpdateResult::CHANGED;
    }
};

class StrokeSettingsService : public StatefulService<StrokeSettings>
{
public:
    StrokeSettingsService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit);

    void begin();

private:
    HttpEndpoint<StrokeSettings> _httpEndpoint;
    FSPersistence<StrokeSettings> _fsPersistence;
};

#endif  // StrokeSettingsService_h
