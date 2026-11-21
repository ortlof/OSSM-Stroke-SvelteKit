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

// The drive's own ceiling: 4200 rpm maximum through a 20:1 gearbox and a 60 mm
// pitch pinion. Commanding a faster step train does not make the machine faster,
// it makes the drive lose position, and the carriage then carries its momentum
// into whatever is in front of it.
#define DRIVE_MAX_SPEED_MM_PER_SECOND 660.0f


#include <ESP32SvelteKit.h>
#include <FSPersistence.h>
#include <HttpEndpoint.h>
#include <StatefulService.h>

#define STROKE_SETTINGS_ENDPOINT_PATH "/rest/strokeSettings"
#define STROKE_SETTINGS_FILE_PATH "/config/strokeSettings.json"

#ifndef OSSM_STEP_PER_MM
#define OSSM_STEP_PER_MM 84.88f
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

    // How far the near end of a sweep keeps away from the home position. The
    // engine declares the home position keepout away from the stall that homing
    // finds, so a sweep that starts at 0 returns to the homing point on every
    // stroke. If homing stops a little late against the end of the rail, that
    // point is already too far out, so keep a margin of its own.
    float home_margin = 15.0;  // mm, 0 .. 50
    float far_margin = 15.0;   // mm, 0 .. 50

    // How much of max_acceleration a direction change may use. A geared rack and
    // pinion with backlash slams its gear train at a full acceleration reversal,
    // and on a stroking machine every move reverses, so this factor decides both
    // the noise and the peak speed a short stroke can reach.
    float reversal_acceleration_factor = 0.35;  // 0.05 .. 1.0

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
        root["reversal_acceleration_factor"] = settings.reversal_acceleration_factor;
        root["home_margin"] = settings.home_margin;
        root["far_margin"] = settings.far_margin;
    }

    static StateUpdateResult update(JsonObject &root, StrokeSettings &settings, const String &originId)
    {
        float physical_travel = root["physical_travel"] | settings.physical_travel;
        float keepout = root["keepout"] | settings.keepout;
        float steps_per_mm = root["steps_per_mm"] | settings.steps_per_mm;
        float max_speed = root["max_speed"] | settings.max_speed;
        max_speed = constrain(max_speed, 1.0f, DRIVE_MAX_SPEED_MM_PER_SECOND);
        float max_acceleration = root["max_acceleration"] | settings.max_acceleration;
        float user_speed_limit = root["user_speed_limit"] | settings.user_speed_limit;
        bool invert_direction = root["invert_direction"] | settings.invert_direction;
        bool sensorless_homing = root["sensorless_homing"] | settings.sensorless_homing;
        float reversal = root["reversal_acceleration_factor"] | settings.reversal_acceleration_factor;
        reversal = constrain(reversal, 0.05f, 1.0f);
        float homeMargin = root["home_margin"] | settings.home_margin;
        homeMargin = constrain(homeMargin, 0.0f, 50.0f);
        float farMargin = root["far_margin"] | settings.far_margin;
        farMargin = constrain(farMargin, 0.0f, 50.0f);

        if (physical_travel == settings.physical_travel && keepout == settings.keepout &&
            steps_per_mm == settings.steps_per_mm && max_speed == settings.max_speed &&
            max_acceleration == settings.max_acceleration &&
            user_speed_limit == settings.user_speed_limit &&
            invert_direction == settings.invert_direction &&
            sensorless_homing == settings.sensorless_homing &&
            reversal == settings.reversal_acceleration_factor &&
            homeMargin == settings.home_margin &&
            farMargin == settings.far_margin)
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
        settings.reversal_acceleration_factor = reversal;
        settings.home_margin = homeMargin;
        settings.far_margin = farMargin;
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
