/*
 *   OSSM-Stroke on ESP32-SvelteKit
 *
 *   The motion code is the OSSM-RackandPinion StrokeEngine (linear speed, mm/s).
 *   The network, the web server, the REST API and the WebSocket event bus come
 *   from ESP32-SvelteKit. Everything the original firmware did with a cable
 *   remote, an OLED or an ESP-NOW remote can be driven from a web browser.
 *
 *   Division of work:
 *     core 0: ESP32-SvelteKit (WiFi, HTTP, WebSocket, MQTT), speed ramp, sync
 *     core 1: motion. StrokeEngine starts its own pattern and homing tasks.
 */

#include <Arduino.h>

#include <ESP32SvelteKit.h>
#include <NotificationService.h>
#include <PsychicHttpServer.h>

#include <FastAccelStepperMotionDriver.h>
#include <StrokeEngine.h>

#include "OSSM_Config.h"
#include "OSSM_PinDEF.h"
#include "StrokeSettingsService.h"
#include "StrokeStateService.h"

#define SERIAL_BAUD_RATE 115200

// The framework needs to know how many HTTP endpoints exist in total, because
// the ESP-IDF server allocates them statically. Count every server.on() in the
// framework, in the front end data and in this project, then add a few spare.
#define NUMBER_OF_ENDPOINTS 60

PsychicHttpServer server;
ESP32SvelteKit esp32sveltekit(&server, NUMBER_OF_ENDPOINTS);

StrokeSettingsService strokeSettingsService(&server, &esp32sveltekit);
StrokeStateService strokeStateService(&server, &esp32sveltekit);

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

StrokeEngine Stroker;
FastAccelStepperMotionDriver stepperDriver;

static machineGeometry strokingMachine;
static motorProperties servoMotor;
static endstopProperties endstop = {
    .homeToBack = true,      // end stop sits at the rear of the machine
    .activeLow = true,       // switch is wired active low
    .endstopPin = SERVO_ENDSTOP,
    .pinMode = INPUT_PULLUP  // pinmode INPUT with an external pull-up resistor
};
static sensorlessHomeProperties sensorless = {
    .currentPin = SERVO_PED_PIN,
    .currentLimit = 2.5f
};

float userSpeedLimit = OSSM_USER_SPEEDLIMIT_MM_S;

// Speed ramp: the slider and the patterns must not fight each other. Hands off
// the running move, apply the new speed on the next pattern segment.
#define SPEED_MIN_MM_PER_SECOND 1.0f
#define SPEED_DEADBAND_MM_PER_SECOND 0.5f
#define SPEED_SLEW_MM_PER_SECOND_PER_SECOND 80.0f
#define SPEED_CONTROL_INTERVAL_MS 100

float appliedSpeedMmPerSecond = SPEED_MIN_MM_PER_SECOND;

// Range the engine really allows in mm, published for the state updater
// (see StrokeStateService.h). Written at boot, after homing and by the sync task;
// 32 bit aligned reads and writes, so no lock is needed for a single value.
volatile float strokeUsableTravelMm = 0.0f;

// Measured speed, computed here from the driver position with microsecond
// timing. A client cannot do this: the 10 Hz WebSocket stream jitters with the
// network, so every slope a client computes is noise. This number is the real
// one, over one synchronisation period.
static volatile float measuredSpeedMmPerSecond = 0.0f;
static float lastSampleMm = 0.0f;
static int64_t lastSampleUs = 0;

// Written by the engine telemetry callback
volatile float telemetryPositionMm = 0.0f;
volatile float telemetrySpeedMmPerSecond = 0.0f;
volatile bool telemetryClipping = false;

TaskHandle_t speedTaskHandle = nullptr;
TaskHandle_t strokeSyncHandle = nullptr;

float clampSpeedMmPerSecond(float requestedSpeed);
static float usableTravel();
static const char *servoStateName(ServoState state);
static void homingNotification(bool isHomed);
static void telemetryCallback(float positionMm, float speedMmPerSecond, bool clipping);
static void loadSettings();
static void startHoming();
static void applyStrokeStateToEngine(const String &originId);
void syncStrokeStateTask(void *pvParameters);
void speedCommandTask(void *pvParameters);
void emitPatternList();

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

float clampSpeedMmPerSecond(float requestedSpeed)
{
    if (!std::isfinite(requestedSpeed))
    {
        return SPEED_MIN_MM_PER_SECOND;
    }
    // StrokeEngine works with a linear actuator speed in mm/s, never with a raw
    // stroke rate. Clamp to what the machine and the user allow.
    return constrain(requestedSpeed, SPEED_MIN_MM_PER_SECOND, userSpeedLimit);
}

// Range the engine really uses: both keepout zones are subtracted.
static float usableTravel()
{
    float usable = strokingMachine.physicalTravel - (2.0f * strokingMachine.keepoutBoundary);
    return (usable > 0.0f) ? usable : 0.0f;
}

static const char *servoStateName(ServoState state)
{
    switch (state)
    {
    case READY:
        return "READY";
    case PATTERN:
        return "PATTERN";
    case SETUPDEPTH:
        return "SETUPDEPTH";
    case STREAMING:
        return "STREAMING";
    case STOPPING:
        return "STOPPING";
    default:
        return "UNDEFINED";
    }
}

// This callback runs inside the engine homing task, which owns a small stack.
// Float formatting on the serial port goes through malloc, and a WebSocket push
// serialises JSON: both need far more stack than a motion task has. Doing that
// work here crashed the device with "Stack canary watchpoint triggered
// (SensorlessHoming)". Record the result only and let the sync task report it.
static volatile bool homingResultPending = false;
static volatile bool homingResultSuccess = false;

static void homingNotification(bool isHomed)
{
    homingResultSuccess = isHomed;
    homingResultPending = true;
}

static void telemetryCallback(float positionMm, float speedMmPerSecond, bool clipping)
{
    telemetryPositionMm = positionMm;
    telemetrySpeedMmPerSecond = speedMmPerSecond;
    telemetryClipping = clipping;
}

// ---------------------------------------------------------------------------
// Settings: read once, the motion driver is created from them
// ---------------------------------------------------------------------------

static void loadSettings()
{
    StrokeSettings settings;
    strokeSettingsService.read([&settings](StrokeSettings &s)
                               { settings = s; });

    strokingMachine.physicalTravel = settings.physical_travel;
    strokingMachine.keepoutBoundary = settings.keepout;

    servoMotor.maxSpeed = settings.max_speed;
    servoMotor.maxAcceleration = settings.max_acceleration;
    servoMotor.stepsPerMillimeter = settings.steps_per_mm;
    servoMotor.invertDirection = settings.invert_direction;
    servoMotor.enableActiveLow = true;
    servoMotor.stepPin = SERVO_PULSE;
    servoMotor.directionPin = SERVO_DIR;
    servoMotor.enablePin = SERVO_ENABLE;

    userSpeedLimit = min(settings.user_speed_limit, settings.max_speed);

    strokeUsableTravelMm = settings.physical_travel - (2.0f * settings.keepout);

    strokeStateService.update([&](StrokeState &state)
                              {
        state.physicalTravel = settings.physical_travel;
        state.speed = clampSpeedMmPerSecond(state.speed);
        return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);


}

// ---------------------------------------------------------------------------
// Homing: non blocking, the state service reports the progress
// ---------------------------------------------------------------------------

static void startHoming()
{
    StrokeSettings settings;
    strokeSettingsService.read([&settings](StrokeSettings &s)
                               { settings = s; });

    Stroker.disable();  // back to UNDEFINED, then energise and home again

    if (settings.sensorless_homing)
    {
        Stroker.enableAndSensorlessHome(&sensorless, homingNotification, 20.0);
    }
    else
    {
        Stroker.enableAndHome(&endstop, homingNotification, 20.0);
    }
}

// ---------------------------------------------------------------------------
// Command applier: called for every update that does not come from the engine
// ---------------------------------------------------------------------------

static void applyStrokeStateToEngine(const String &originId)
{
    if (originId == STROKE_ENGINE_ORIGIN)
    {
        return;  // telemetry push, not a command
    }

    StrokeState command;
    strokeStateService.read([&command](StrokeState &state)
                            { command = state; });

    // The state updater already keeps depth and stroke consistent, and it
    // runs under the state mutex, so this applies exactly what the browser sees.
    ServoState engineState = Stroker.getState();

    if (command.home)
    {
        // Clear the transient command first, then run the homing procedure.
        strokeStateService.update([](StrokeState &state)
                                  {
            state.home = false;
            return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);
        startHoming();
        return;
    }

    if (command.run && (engineState == PATTERN || engineState == STREAMING))
    {
        // Already running: only take over the pattern parameters.
        Stroker.setDepth(command.depth, false);
        Stroker.setStroke(command.stroke, false);
        Stroker.setSensation(command.sensation, false);
        Stroker.setPattern(command.pattern, false);
        return;
    }

    if (command.run && engineState == UNDEFINED)
    {
        // Not homed, the servo has no position. Refuse and report.
        esp32sveltekit.getNotificationService()->pushNotification("Machine is not homed", PUSHWARNING);
        strokeStateService.update([](StrokeState &state)
                                  {
            state.run = false;
            return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);
        return;
    }

    if (!command.run && (engineState == PATTERN || engineState == STREAMING))
    {
        Stroker.requestStop();  // non blocking controlled stop
        return;
    }

    Stroker.setDepth(command.depth, false);
    Stroker.setStroke(command.stroke, false);
    Stroker.setSensation(command.sensation, false);
    Stroker.setPattern(command.pattern, false);

    if (command.run)
    {
        // A stroke of zero is a valid command for the engine and it moves
        // nowhere. Refuse it instead of leaving the user with a dead button.
        if (command.stroke <= 0.0f)
        {
            esp32sveltekit.getNotificationService()->pushNotification("Set a stroke length first", PUSHWARNING);
            strokeStateService.update([](StrokeState &state)
                                      {
                state.run = false;
                return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);
            return;
        }

        if (!Stroker.startPattern())
        {
            esp32sveltekit.getNotificationService()->pushNotification("Pattern could not be started", PUSHERROR);
        }
    }
}

// ---------------------------------------------------------------------------
// Telemetry: engine -> state service -> WebSocket clients
// ---------------------------------------------------------------------------

void syncStrokeStateTask(void *pvParameters)
{
    TickType_t lastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        // Result of the homing procedure, reported by the engine homing task
        if (homingResultPending)
        {
            bool success = homingResultSuccess;
            homingResultPending = false;

            if (success)
            {
                // The engine measures the real travel while it homes.
                strokingMachine.physicalTravel = Stroker.getPhysicalTravel();
            }

            // The engine confines depth and stroke to the travel minus both
            // keepout zones. usable_travel is served from this value, see
            // StrokeStateService.h.
            float usable = strokingMachine.physicalTravel - (2.0f * strokingMachine.keepoutBoundary);
            strokeUsableTravelMm = (usable > 0.0f) ? usable : 0.0f;

            strokeStateService.update([&](StrokeState &state)
                                      {
                state.homed = success;
                state.physicalTravel = strokingMachine.physicalTravel;
                return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);

            esp32sveltekit.getNotificationService()->pushNotification(
                success ? "Homed - ready to rumble" : "Homing failed",
                success ? PUSHSUCCESS : PUSHERROR);

            // Integer formatting only, the float path is what blew the stack.
            Serial.printf("Homing %s, physical travel: %d mm\n", success ? "done" : "failed",
                          (int)strokingMachine.physicalTravel);
        }

        ServoState engineState = Stroker.getState();
        const char *stateName = servoStateName(engineState);
        bool running = (engineState == PATTERN || engineState == STREAMING);
        bool homed = (engineState != UNDEFINED);
        bool clipping = telemetryClipping;

        // 0.1 mm resolution: no point in pushing noise over the wire
        float stepsPerMm = (servoMotor.stepsPerMillimeter > 0.0f) ? servoMotor.stepsPerMillimeter : 1.0f;
        float rawMm = float(stepperDriver.getCurrentPosition()) / stepsPerMm;
        float positionMm = roundf(rawMm * 10.0f) / 10.0f;

        // Measured speed over the last period, from the unsmoothed position and
        // the real elapsed microseconds.
        int64_t nowUs = esp_timer_get_time();
        if (lastSampleUs != 0)
        {
            float dt = float(nowUs - lastSampleUs) / 1000000.0f;
            if (dt > 0.02f)
            {
                float v = (rawMm - lastSampleMm) / dt;
                measuredSpeedMmPerSecond = (fabsf(v) < 0.8f) ? 0.0f : v;  // deadband: standing still is not noise
                lastSampleMm = rawMm;
                lastSampleUs = nowUs;
            }
        }
        else
        {
            lastSampleMm = rawMm;
            lastSampleUs = nowUs;
        }
        float travel = strokingMachine.physicalTravel;
        float usable = travel - (2.0f * strokingMachine.keepoutBoundary);
        strokeUsableTravelMm = (usable > 0.0f) ? usable : 0.0f;

        strokeStateService.update([&](StrokeState &state)
                                  {
            bool changed = false;
            if (state.servoState != stateName) { state.servoState = stateName; changed = true; }
            if (state.run != running) { state.run = running; changed = true; }
            if (state.homed != homed) { state.homed = homed; changed = true; }
            if (state.clipping != clipping) { state.clipping = clipping; changed = true; }
            if (fabsf(state.position - positionMm) >= 0.05f) { state.position = positionMm; changed = true; }
            float speedNow = measuredSpeedMmPerSecond;
            if (fabsf(state.speedMeasured - speedNow) >= 0.5f) { state.speedMeasured = speedNow; changed = true; }
            if (fabsf(state.physicalTravel - travel) >= 0.05f) { state.physicalTravel = travel; changed = true; }

            String name = Stroker.getPatternName(state.pattern);
            if (state.patternName != name) { state.patternName = name; changed = true; }

            return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED; },
                                  STROKE_ENGINE_ORIGIN);

        vTaskDelayUntil(&lastWakeTime, 100 / portTICK_PERIOD_MS);
    }
}

void speedCommandTask(void *pvParameters)
{
    const float maxStepPerTick = SPEED_SLEW_MM_PER_SECOND_PER_SECOND * (float(SPEED_CONTROL_INTERVAL_MS) / 1000.0f);
    TickType_t lastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        float target = SPEED_MIN_MM_PER_SECOND;
        strokeStateService.read([&target](StrokeState &state)
                                { target = state.speed; });
        target = clampSpeedMmPerSecond(target);

        float delta = target - appliedSpeedMmPerSecond;

        if (fabsf(delta) > SPEED_DEADBAND_MM_PER_SECOND)
        {
            delta = constrain(delta, -maxStepPerTick, maxStepPerTick);
            appliedSpeedMmPerSecond = clampSpeedMmPerSecond(appliedSpeedMmPerSecond + delta);
            // applyNow = false: the new speed is taken over with the next
            // pattern segment, this keeps the gearbox quiet.
            Stroker.setSpeed(appliedSpeedMmPerSecond, false);
        }

        vTaskDelayUntil(&lastWakeTime, SPEED_CONTROL_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

// The pattern list is static, but the browser needs it for the drop down. It is
// pushed as soon as a client subscribes to the "strokePatterns" event.
void emitPatternList()
{
    JsonDocument doc;
    JsonArray names = doc["names"].to<JsonArray>();
    unsigned int count = Stroker.getNumberOfPattern();
    for (unsigned int i = 0; i < count; i++)
    {
        names.add(Stroker.getPatternName(i));
    }
    doc["count"] = count;

    JsonObject root = doc.as<JsonObject>();
    esp32sveltekit.getSocket()->emitEvent("strokePatterns", root, "");
}

void setup()
{
    Serial.begin(SERIAL_BAUD_RATE);

    // --- network, web server, settings ---
    esp32sveltekit.setMDNSAppName("OSSM Stroke");
    esp32sveltekit.begin();
    strokeSettingsService.begin();
    strokeStateService.begin();

    // --- board ---
    pinMode(SERVO_ALM_PIN, INPUT);
    pinMode(SERVO_PED_PIN, INPUT);

    loadSettings();
    // Motion config must be known before the driver is created
    Stroker.begin(&strokingMachine, &servoMotor, &stepperDriver);
    Stroker.registerTelemetryCallback(telemetryCallback);

    // Command applier: runs on every state change that a client caused
    strokeStateService.addUpdateHandler([&](const String &originId)
                                        { applyStrokeStateToEngine(originId); },
                                        false);

    // Pattern list for the UI
    esp32sveltekit.getSocket()->registerEvent("strokePatterns");
    esp32sveltekit.getSocket()->onSubscribe("strokePatterns", [&](const String &originId)
                                            { emitPatternList(); });

    // Homing is non blocking, the state service reports the progress
    startHoming();

    // Tasks
    // 8192: this task serialises the state to JSON and pushes it over the
    // WebSocket, and it reports the homing result.
    xTaskCreatePinnedToCore(syncStrokeStateTask, "strokeSync", 8192, NULL, 3, &strokeSyncHandle, 0);
    xTaskCreatePinnedToCore(speedCommandTask, "speedCommand", 3072, NULL, 4, &speedTaskHandle, 0);

    Serial.printf("OSSM-Stroke %s on %s ready\n", APP_VERSION, BUILD_TARGET);
}

void loop()
{
    // The framework runs its own network task and the motion has its own tasks.
    // The Arduino loop task stays free for a local display or a cable remote.
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}
