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

#include <cstdlib>
#include <cxxabi.h>
#include <exception>

#include "OSSM_Config.h"
#include "OSSM_PinDEF.h"
#include "ModbusProbeService.h"
#include "SessionUploadService.h"
#include "StrokeSettingsService.h"
#include "StrokeStateService.h"

/*
 *   An uncaught C++ exception aborts this board, and the ESP-IDF panic handler can
 *   only print registers and addresses - the exception TYPE is lost, and a
 *   truncated backtrace names nothing. Measured on this machine: `rst:0xc` with
 *   "abort() was called at PC 0x401b5dab", which resolves to __gxx_personality_v0,
 *   so something threw and nobody caught it.
 *
 *   This handler runs BEFORE the abort and prints what was thrown. It turns "the
 *   device died again" into either `std::bad_alloc` (an allocation failed, i.e.
 *   memory pressure, which is the strong candidate here) or a named logic fault.
 *   It is deliberately tiny: it runs when the machine is already failing, so it
 *   avoids allocation of its own and falls back if the demangle cannot allocate.
 */
static void uncaughtExceptionHandler()
{
    Serial.println();
    Serial.println("!!! UNCAUGHT C++ EXCEPTION - this is what aborts the board !!!");

    std::type_info *info = abi::__cxa_current_exception_type();
    if (info != nullptr)
    {
        // No demangle. __cxa_demangle allocates, and this handler runs precisely
        // when allocation is what failed - an abort inside the abort reporter
        // tells nobody anything, and a demangler frame in the crash backtrace is
        // indistinguishable from the real fault. The mangled name is enough to
        // separate a bad_alloc (memory) from a logic fault.
        Serial.printf("  type: %s\n", info->name());
    }
    else
    {
        Serial.println("  type: unknown (no active exception)");
    }

    try
    {
        throw;
    }
    catch (const std::exception &e)
    {
        Serial.printf("  what(): %s\n", e.what());
    }
    catch (...)
    {
        Serial.println("  what(): (not a std::exception)");
    }

    Serial.println("!!! aborting now, as the runtime wanted !!!");
    Serial.flush();
    abort();
}

#define SERIAL_BAUD_RATE 115200

// The framework needs to know how many HTTP endpoints exist in total, because
// the ESP-IDF server allocates them statically. Count every server.on() in the
// framework, in the front end data and in this project, then add a few spare.
#define NUMBER_OF_ENDPOINTS 64

PsychicHttpServer server;
ESP32SvelteKit esp32sveltekit(&server, NUMBER_OF_ENDPOINTS);

StrokeSettingsService strokeSettingsService(&server, &esp32sveltekit);
StrokeStateService strokeStateService(&server, &esp32sveltekit);
#if FT_TORQUE_TELEMETRY
ModbusProbeService modbusProbeService(&server, &esp32sveltekit);
#endif
// The archive uploader only reads the other services: the stroke state for the
// record control, the machine settings for the torque limits it records at the
// start of a session, and the Modbus probe for the signed torque reading.
SessionUploadService sessionUploadService(&server, &esp32sveltekit, &strokeStateService,
                                          &strokeSettingsService
#if FT_TORQUE_TELEMETRY
                                          , &modbusProbeService
#endif
                                          );

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

// Which interactive setup mode the engine is in. Needed because entering fancy
// mode from normal mode must reach the engine even though both are SETUPDEPTH.
static int appliedSetupMode = 0;

// Feed rate of the interactive setup modes, mm/s (the cable remote used 10).
#define SETUP_SPEED_MM_PER_SECOND 10.0f

// The manual retract speed for the pull-out command. Faster than depth setup
// because it is the "get it out" control and the operator is usually waiting on
// it, still far below the measured envelope of this machine.
#define PULL_OUT_SPEED_MM_PER_SECOND 40.0f

// Range the engine really allows in mm, published for the state updater
// (see StrokeStateService.h). Written at boot, after homing and by the sync task;
// 32 bit aligned reads and writes, so no lock is needed for a single value.
volatile float strokeUsableTravelMm = 0.0f;
volatile float strokeHomeMarginMm = 15.0f;
volatile float strokeFarMarginMm = 15.0f;

// Which way the carriage is travelling, in the machine's own frame: +1 outward
// (deeper, a growing position), -1 back towards home, 0 standing still. The Modbus
// probe's JSON updater reads it to sign the torque by direction; see
// ModbusProbeService.h for why it is a plain global.
volatile int strokeTravelDirection = 0;

// Direction detection for that readout. Position is reported to 0.1 mm, so the
// deadband sits just under one reported step, and the direction is held for a
// moment after motion stops because the probe polls and the browser reads at
// different rates: without the hold, a reading taken in the gap between two ticks
// would say "holding" and be read as the wrong direction.
#define STROKE_DIRECTION_DEADBAND_MM 0.05f
#define STROKE_DIRECTION_HOLD_MS 800U

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

// Published for ModbusProbeService's lost-step monitor: the firmware's own step
// count and its steps-per-mm scaled by 1000, so the monitor can stay in integer
// arithmetic. Plain aligned 32-bit values, read by the Modbus task without a
// lock. The drive's closed loop knows its own encoder position; this is the only
// place the firmware exposes what it thinks it commanded.
volatile int32_t modbusFirmwareSteps = 0;
volatile int32_t modbusFirmwareStepsPerMmX1000 = (int32_t)(OSSM_STEP_PER_MM * 1000.0f);

TaskHandle_t speedTaskHandle = nullptr;
TaskHandle_t strokeSyncHandle = nullptr;

float clampSpeedMmPerSecond(float requestedSpeed);
static float usableTravel();
static const char *servoStateName(ServoState state);
static void homingNotification(bool isHomed);
static void telemetryCallback(float positionMm, float speedMmPerSecond, bool clipping);
static void loadSettings();
static void applyMotionTuning();
#if FT_TORQUE_TELEMETRY
static void applyTorqueLimits();
#endif
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
    modbusFirmwareStepsPerMmX1000 = (int32_t)(settings.steps_per_mm * 1000.0f + 0.5f);
    servoMotor.invertDirection = settings.invert_direction;
    servoMotor.enableActiveLow = true;
    servoMotor.stepPin = SERVO_PULSE;
    servoMotor.directionPin = SERVO_DIR;
    servoMotor.enablePin = SERVO_ENABLE;

    userSpeedLimit = min(settings.user_speed_limit, settings.max_speed);

    strokeUsableTravelMm = settings.physical_travel - (2.0f * settings.keepout);
    strokeHomeMarginMm = settings.home_margin;
    strokeFarMarginMm = settings.far_margin;

    strokeStateService.update([&](StrokeState &state)
                              {
        state.physicalTravel = settings.physical_travel;
        state.speed = clampSpeedMmPerSecond(state.speed);
        return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);


}

// ---------------------------------------------------------------------------
// Motion tuning: engine setters that must be applied AFTER begin(), because
// begin() resets them to the compiled defaults. Safe to call again at runtime,
// so a settings change takes effect without a restart.
// ---------------------------------------------------------------------------

static void applyMotionTuning()
{
    StrokeSettings settings;
    strokeSettingsService.read([&settings](StrokeSettings &s)
                               { settings = s; });

    strokeHomeMarginMm = settings.home_margin;
    strokeFarMarginMm = settings.far_margin;
    Stroker.setReversalAccelerationFactor(settings.reversal_acceleration_factor);
    Stroker.setMaxAcceleration(settings.max_acceleration);
    userSpeedLimit = min(settings.user_speed_limit, settings.max_speed);
}

// ---------------------------------------------------------------------------
// Direction-specific torque limits (P05-10 forward / P05-11 reverse)
//
// Outward is the direction that extends the carriage, inward retracts it. The
// drive stores the forward limit positive and the reverse limit negative, and
// it does not keep either across a power cycle, so they are written here at
// boot and again whenever the setting changes. Nothing writes them while the
// engine runs: a pattern must not move the safety limit under the operator.
// ---------------------------------------------------------------------------

#if FT_TORQUE_TELEMETRY
static void applyTorqueLimits()
{
    StrokeSettings settings;
    strokeSettingsService.read([&settings](StrokeSettings &s)
                               { settings = s; });

    int outwardRaw = (int)lroundf(constrain(settings.torque_outward_pct, 0.0f, 300.0f) * 10.0f);
    int inwardRaw = (int)lroundf(constrain(settings.torque_inward_pct, 0.0f, 300.0f) * 10.0f);

    // Which register each one lands in is the wiring question, and that is what
    // torque_outward_forward answers.
    int forwardMag = settings.torque_outward_forward ? outwardRaw : inwardRaw;
    int reverseMag = settings.torque_outward_forward ? inwardRaw : outwardRaw;

    modbusProbeService.setTorqueLimits(forwardMag, reverseMag);
}
#endif

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

// The engine keeps its own pattern index and refuses one that is out of range, so
// the state has to be pulled back to what the machine really uses. Without this a
// request with pattern 9 leaves the state claiming 9 ("Invalid") while the engine
// still runs the old pattern, and the UI then shows a pattern the machine is not
// running. Same class as the depth/stroke mismatch this project already fixed.
static void reflectPatternFromEngine()
{
    int enginePattern = Stroker.getPattern();
    strokeStateService.update([&](StrokeState &state)
                              {
        if (state.pattern == enginePattern) {
            return StateUpdateResult::UNCHANGED;
        }
        state.pattern = enginePattern;
        return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);
}

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

    // --- interactive depth setup ---------------------------------------------
    // 0 off, 1 setup depth, 2 setup depth with the sensation slider (fancy).
    // While the engine is in SETUPDEPTH every setDepth() and setSensation() moves
    // the machine at the setup feed rate, so the browser sliders drive it live.
    if (command.setup != appliedSetupMode)
    {
        if (command.setup == 0)
        {
            if (engineState == SETUPDEPTH)
            {
                Stroker.stopMotion();  // leaves the mode, state READY
            }
            appliedSetupMode = 0;
        }
        else
        {
            if (!Stroker.setupDepth(SETUP_SPEED_MM_PER_SECOND, command.setup == 2))
            {
                // Not homed, the engine refuses. Tell the user instead of doing nothing.
                esp32sveltekit.getNotificationService()->pushNotification("Machine is not homed", PUSHWARNING);
                strokeStateService.update([](StrokeState &state)
                                          {
                    state.setup = 0;
                    return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);
                appliedSetupMode = 0;
            }
            else
            {
                appliedSetupMode = command.setup;
            }
        }
    }

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

    if (command.pullOut)
    {
        // Transient, like home: clear the flag first so the command cannot repeat.
        // It also clears run, because a pull-out is meant to end the session, and
        // leaving run set would let the next unrelated command (any depth or
        // pattern change) start the sweep again from wherever it stopped.
        strokeStateService.update([](StrokeState &state)
                                  {
            state.pullOut = false;
            state.run = false;
            return StateUpdateResult::CHANGED; }, STROKE_ENGINE_ORIGIN);

        // moveToMin() stops whatever is running and drives to the home reference
        // at step 0, which is where the homing run parks the machine.
        if (!Stroker.moveToMin(PULL_OUT_SPEED_MM_PER_SECOND))
        {
            esp32sveltekit.getNotificationService()->pushNotification("Machine is not homed", PUSHWARNING);
        }
        return;
    }

    if (command.run && (engineState == PATTERN || engineState == STREAMING))
    {
        // Already running: only take over the pattern parameters.
        Stroker.setDepth(command.depth, false);
        Stroker.setStroke(command.stroke, false);
        Stroker.setSensation(command.sensation, false);
        Stroker.setSlowStart(command.slowStart, false);
        Stroker.setPattern(command.pattern, false);
        reflectPatternFromEngine();
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
        // Non blocking controlled stop. Do NOT return here: the same request may
        // carry depth, stroke, pattern or sensation, and dropping them silently
        // cost an hour of confusion once (a start was then refused because the
        // stroke in that same request had been thrown away).
        Stroker.requestStop();
    }

    Stroker.setDepth(command.depth, false);
    Stroker.setStroke(command.stroke, false);
    Stroker.setSensation(command.sensation, false);
    Stroker.setSlowStart(command.slowStart, false);
    Stroker.setPattern(command.pattern, false);
    reflectPatternFromEngine();

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
        // The firmware's own step count, for the Modbus lost-step monitor.
#if FT_TORQUE_TELEMETRY
        modbusFirmwareSteps = stepperDriver.getCurrentPosition();
#endif
        float positionMm = roundf(rawMm * 10.0f) / 10.0f;

        // --- which way the carriage is going, for the signed torque readout ----
        // A growing position is outward (deeper), a shrinking one is back towards
        // home. Taken from the position delta rather than the drive's signed rpm,
        // because this frame is the one that defines "outward" and the encoder's
        // polarity is not established. The direction is held for a moment after
        // motion stops so a reading taken between two ticks cannot flicker.
        {
            static float lastDirectionPosition = 0.0f;
            static bool directionSeeded = false;
            static uint32_t lastMotionMs = 0;

            if (!directionSeeded)
            {
                lastDirectionPosition = positionMm;
                directionSeeded = true;
            }

            float deltaMm = positionMm - lastDirectionPosition;
            if (fabsf(deltaMm) >= STROKE_DIRECTION_DEADBAND_MM)
            {
                strokeTravelDirection = (deltaMm > 0.0f) ? 1 : -1;
                lastDirectionPosition = positionMm;
                lastMotionMs = millis();
            }
            else if (strokeTravelDirection != 0 &&
                     (uint32_t)(millis() - lastMotionMs) > STROKE_DIRECTION_HOLD_MS)
            {
                strokeTravelDirection = 0;
            }
        }

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

        // A homing run that finds almost no travel leaves every sweep clamped to
        // nothing, and a run is then refused with "Set a stroke length first" -
        // which names the wrong problem and sends the operator looking at the
        // sliders. Say what is actually wrong, once per occurrence.
        //
        // This is not hypothetical: the sensorless homing trips on a CURRENT
        // threshold (a hardcoded 2.5 %, main.cpp), so a load on the carriage can
        // trip it within the first millimetre. Measured: with 3.093 kg on, a
        // post-flash homing reported 5 mm of travel and the machine then refused
        // every run. Integer formatting only, for the stack reason above.
        {
            static bool warnedAboutTravel = false;
            const bool travelTooSmall = (strokeUsableTravelMm <= 0.0f);
            if (travelTooSmall && !warnedAboutTravel)
            {
                warnedAboutTravel = true;
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "Homing found %d mm of travel - re-home before running",
                         (int)travel);
                esp32sveltekit.getNotificationService()->pushNotification(msg, PUSHWARNING);
            }
            else if (!travelTooSmall)
            {
                warnedAboutTravel = false;
            }
        }

        strokeStateService.update([&](StrokeState &state)
                                  {
            bool changed = false;
            if (state.servoState != stateName) { state.servoState = stateName; changed = true; }

            // The engine can leave SETUPDEPTH on its own, for example when a
            // pattern starts. Keep the mode field honest.
            if (state.setup != 0 && engineState != SETUPDEPTH)
            {
                state.setup = 0;
                appliedSetupMode = 0;
                changed = true;
            }
            if (state.run != running) { state.run = running; changed = true; }
            if (state.homed != homed) { state.homed = homed; changed = true; }
            if (state.clipping != clipping) { state.clipping = clipping; changed = true; }
            if (fabsf(state.position - positionMm) >= 0.05f) { state.position = positionMm; changed = true; }
            float speedNow = measuredSpeedMmPerSecond;
            if (fabsf(state.speedMeasured - speedNow) >= 0.5f) { state.speedMeasured = speedNow; changed = true; }
            if (fabsf(state.physicalTravel - travel) >= 0.05f) { state.physicalTravel = travel; changed = true; }

            String name = Stroker.getPatternName(state.pattern);
            if (state.patternName != name) { state.patternName = name; changed = true; }

            // Tell the clients whether the running pattern reads slow_start, so the
            // slider is only offered where it does something.
            bool readsSlowStart = Stroker.patternUsesSlowStart();
            if (state.usesSlowStart != readsSlowStart) { state.usesSlowStart = readsSlowStart; changed = true; }

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

// ---------------------------------------------------------------------------
// Operator-presence dead-man.
//
// If the operator's client goes away while the machine runs, the machine stops.
// The presence signal is the WebSocket client count: a browser or the HUD holds
// a socket while the operator is there, and the socket closes when they leave.
// No heartbeat protocol is needed for that, and a client that freezes is dropped
// by the socket's own ping handling.
//
// Two rules keep it from stopping work that should continue:
//   - it arms only while the machine runs, and only if a client was present when
//     that run started, so a script-driven run with no browser attached is never
//     interrupted;
//   - the count must stay zero for the whole grace period, so a page reload or a
//     brief network blip does not stop the machine.
//
// What it stops is the same path the operator's own stop button uses: run set
// false by an update whose origin is not the engine, so the command applier
// picks it up and stops the motion. A stop is the safe direction on this axis -
// the drive holds position at rest.
#define DEADMAN_GRACE_MS 3000
#define DEADMAN_TICK_MS 500

void deadmanTask(void *pvParameters)
{
    bool seenClientThisRun = false;
    bool prevRun = false;
    uint32_t lastSeenMs = 0;
    TickType_t lastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        const int clients = (int)esp32sveltekit.getSocket()->getConnectedClients();

        bool run = false;
        strokeStateService.read([&run](StrokeState &state)
                                { run = state.run; });

        if (clients > 0)
        {
            seenClientThisRun = true;
            lastSeenMs = millis();
        }

        if (run && !prevRun)
        {
            // A new run. Only one that starts with a client attached can be
            // interrupted by that client leaving.
            seenClientThisRun = (clients > 0);
            lastSeenMs = millis();
        }
        prevRun = run;

        if (run && seenClientThisRun && clients == 0 &&
            (millis() - lastSeenMs) > DEADMAN_GRACE_MS)
        {
            strokeStateService.update([](StrokeState &state)
                                      {
                if (!state.run) { return StateUpdateResult::UNCHANGED; }
                state.run = false;
                state.safeStop = true;
                return StateUpdateResult::CHANGED; }, "watchdog");
            esp32sveltekit.getNotificationService()->pushNotification(
                "Stopped: the operator's client went away", PUSHWARNING);
            Serial.println("dead-man: operator client gone, motion stopped");
        }

        // Publish presence, and clear the dead-man record as soon as a new run
        // starts. Origin is the engine, so this telemetry can never stop or
        // start anything.
        strokeStateService.update([&](StrokeState &state)
                                  {
            bool changed = false;
            if (state.wsClients != clients) { state.wsClients = clients; changed = true; }
            if (state.safeStop && state.run) { state.safeStop = false; changed = true; }
            return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED; },
                                  STROKE_ENGINE_ORIGIN);

        vTaskDelayUntil(&lastWakeTime, DEADMAN_TICK_MS / portTICK_PERIOD_MS);
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

    // First thing after the port is up: if anything below throws and nothing
    // catches it, the type is printed instead of a bare abort.
    std::set_terminate(uncaughtExceptionHandler);

    // --- network, web server, settings ---
    esp32sveltekit.setMDNSAppName("OSSM Stroke");
    esp32sveltekit.begin();
    strokeSettingsService.begin();
    strokeStateService.begin();
#if FT_TORQUE_TELEMETRY
    modbusProbeService.begin();
#endif
    sessionUploadService.begin();

    // --- board ---
    pinMode(SERVO_ALM_PIN, INPUT);
    pinMode(SERVO_PED_PIN, INPUT);

    loadSettings();
    // Motion config must be known before the driver is created
    Stroker.begin(&strokingMachine, &servoMotor, &stepperDriver);
    Stroker.registerTelemetryCallback(telemetryCallback);
    applyMotionTuning();  // after begin(): it resets the factors to the defaults
#if FT_TORQUE_TELEMETRY
    applyTorqueLimits();  // the drive does not keep the limits across a power cycle
#endif

    // A change to the machine settings applies the tuning immediately.
    strokeSettingsService.addUpdateHandler([&](const String &originId)
                                           {
        applyMotionTuning();
#if FT_TORQUE_TELEMETRY
        applyTorqueLimits();
#endif
    },
                                           false);

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
    // Below the motion tasks: this one must never compete with them for time.
    xTaskCreatePinnedToCore(deadmanTask, "deadman", 3072, NULL, 2, NULL, 0);

    Serial.printf("OSSM-Stroke %s on %s ready\n", APP_VERSION, BUILD_TARGET);
}

void loop()
{
    // The framework runs its own network task and the motion has its own tasks.
    // The Arduino loop task stays free for a local display or a cable remote.
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}
