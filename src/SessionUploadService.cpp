#include "SessionUploadService.h"

#include "osdb_ca.h"

#include <time.h>

/*
 *   Session archive uploader. See the header for the design; this file is the
 *   mechanics and the traps.
 *
 *   Three things here are deliberate and were paid for once already in this
 *   project:
 *     1. Nothing blocking happens in an engine callback. This is its own task on
 *        core 0, next to the framework's network work, and it touches the engine
 *        only through the state service.
 *     2. No heap churn in the capture path. The sample ring is a fixed array of
 *        plain structs and the POST body is one fixed buffer, because the body
 *        varies in length every batch and a String would fragment the heap a few
 *        hundred times an hour.
 *     3. Timestamps are clock-at-session-start plus monotonic uptime, so the
 *        trace has millisecond resolution (time() has only seconds) and an NTP
 *        step mid-session cannot put a jump in it.
 */

// The POST body for one batch. Fixed, so it costs BSS instead of heap. Only the
// capture task touches it, and that task is single-threaded by construction.
static char osdbBody[SESSION_UPLOAD_BATCH_MAX * 200 + 256];

static int extractSessionId(const String &json)
{
    int at = json.indexOf("\"session_id\"");
    if (at < 0)
    {
        return -1;
    }
    at = json.indexOf(':', at);
    if (at < 0)
    {
        return -1;
    }
    // toInt() skips leading spaces and stops at the first non-digit, which is
    // exactly the grammar of the one field this needs to read.
    return json.substring(at + 1).toInt();
}

static void iso8601FromMs(int64_t epochMs, char *out, size_t len)
{
    time_t seconds = (time_t)(epochMs / 1000);
    struct tm utc;
    gmtime_r(&seconds, &utc);
    strftime(out, len, "%Y-%m-%dT%H:%M:%S", &utc);
    size_t used = strlen(out);
    if (used + 6 < len)
    {
        snprintf(out + used, len - used, ".%03dZ", (int)(epochMs % 1000));
    }
}

SessionUploadService::SessionUploadService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit,
                                           StrokeStateService *strokeState,
                                           StrokeSettingsService *machineSettings,
                                           ModbusProbeService *probe) : _httpEndpoint(
                                                                            SessionUploadSettings::read,
                                                                            SessionUploadSettings::update,
                                                                            this,
                                                                            server,
                                                                            SESSION_UPLOAD_ENDPOINT_PATH,
                                                                            sveltekit->getSecurityManager(),
                                                                            AuthenticationPredicates::IS_AUTHENTICATED),
                                                                        _fsPersistence(
                                                                            SessionUploadSettings::readForFile,
                                                                            SessionUploadSettings::update,
                                                                            this,
                                                                            sveltekit->getFS(),
                                                                            SESSION_UPLOAD_FILE_PATH),
                                                                        _strokeState(strokeState),
                                                                        _machineSettings(machineSettings),
                                                                        _probe(probe)
{
}

void SessionUploadService::begin()
{
    _fsPersistence.readFromFS();
    _httpEndpoint.begin();

    // 16 kB, and this number is a compromise the hardware forces: the mbedTLS
    // handshake needs BOTH a deep stack AND one large contiguous heap allocation,
    // and this task's stack comes out of that same DRAM. Measured: at 24576 the
    // largest contiguous block fell from 42,996 to 34,804 and the handshake
    // failed with "X509 - Certificate verification failed" even though the CA was
    // correct; at 16384 it succeeds. At 16384 the task's own watermark is about
    // 5 kB used, and it survives a stationary session. The ring buffer and the
    // POST body are kept small for the same reason - every kilobyte of static
    // buffer is a kilobyte the handshake cannot have.
    xTaskCreatePinnedToCore(taskEntry, "sessionUpload", 16384, this, 1, &_taskHandle, 0);
}

void SessionUploadService::taskEntry(void *param)
{
    static_cast<SessionUploadService *>(param)->captureLoop();
    vTaskDelete(nullptr);
}

bool SessionUploadService::clockReady()
{
    return time(nullptr) >= (time_t)SESSION_UPLOAD_CLOCK_MIN_EPOCH;
}

void SessionUploadService::captureLoop()
{
    TickType_t lastWakeTime = xTaskGetTickCount();

    for (;;)
    {
        SessionUploadSettings settings;
        read([&settings](SessionUploadSettings &s)
             { settings = s; });

        bool wantRecording = false;
        _strokeState->read([&wantRecording](StrokeState &s)
                           { wantRecording = s.record; });

        // Signed difference, so the millis() wrap does not matter.
        const bool retryDue = ((int32_t)(millis() - _retryAfterMs) >= 0);

        const char *status = "idle";
        const char *error = "";

        if (!settings.enabled)
        {
            // Disabling while a session is open must not leave a session open on
            // the server for ever: close it with whatever is still buffered.
            if (_recording)
            {
                flushSamples(settings, true);
                closeSession(settings);
            }
            _ringCount = 0;
            _ringWrite = 0;
            status = "off";
        }
        else if (settings.key.length() == 0)
        {
            status = "error";
            error = "no ingest key set";
        }
        else if (!clockReady())
        {
            // TLS cannot validate a certificate against 1970. Say so instead of
            // failing the handshake and calling it a network error.
            status = "clock";
        }
        else if (wantRecording && !_recording && retryDue)
        {
            if (openSession(settings))
            {
                _recording = true;
                _consecutiveFailures = 0;
                _retryAfterMs = 0;
                status = "recording";
            }
            else
            {
                status = "error";
                error = (_lastError.length() > 0) ? _lastError.c_str() : "session could not be opened";
                // 2 s, 4 s, 6 s ... capped at 30 s. Each step is CPU work on this
                // core, so a tight loop is both rude to the endpoint and fatal to
                // the idle task that feeds the watchdog.
                _consecutiveFailures++;
                uint32_t waitMs = 2000u * _consecutiveFailures;
                if (waitMs > 30000u)
                {
                    waitMs = 30000u;
                }
                _retryAfterMs = millis() + waitMs;
            }
        }
        else if (wantRecording && !_recording)
        {
            // Inside the backoff window: report the last reason and wait.
            status = "error";
            error = (_lastError.length() > 0) ? _lastError.c_str() : "waiting to retry";
        }
        else if (!wantRecording && _recording)
        {
            flushSamples(settings, true);
            closeSession(settings);
            status = "idle";
        }
        else if (_recording)
        {
            capture(settings);

            const bool batchFull = (_ringCount >= settings.batch);
            const bool timeUp = (uint32_t)(millis() - _lastFlushMs) >= SESSION_UPLOAD_FLUSH_MS;
            if (batchFull || timeUp)
            {
                flushSamples(settings, false);
            }
            status = "recording";
        }

        // The state only emits an event when a value really changed, so an idle
        // machine stays silent. The counters are throttled below: publishing them
        // on every tick doubles the event rate exactly when the engine is already
        // pushing telemetry at 10 Hz, and nobody can read a counter that fast.
        // Counters move slowly enough that once a second is generous; the state
        // and the session id are published the moment they change.
        const bool countersDue = (uint32_t)(millis() - _lastStatusPublishMs) >= 1000u;
        if (countersDue)
        {
            _lastStatusPublishMs = millis();
        }

        _strokeState->update([&](StrokeState &s)
                             {
            bool changed = false;
            String wantedState = status;
            String wantedError = error;
            if (s.uploadState != wantedState) { s.uploadState = wantedState; changed = true; }
            if (s.uploadError != wantedError) { s.uploadError = wantedError; changed = true; }
            if (s.uploadSession != _sessionId) { s.uploadSession = _sessionId; changed = true; }
            if (!countersDue)
            {
                return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
            }
            if (s.uploadQueued != _ringCount) { s.uploadQueued = _ringCount; changed = true; }
            if (s.uploadSent != (int)_sentThisSession) { s.uploadSent = (int)_sentThisSession; changed = true; }
            if (s.uploadDropped != (int)_droppedThisSession) { s.uploadDropped = (int)_droppedThisSession; changed = true; }
            // Measure, do not assume: this is the free stack of THIS task after
            // the work of the tick, including any handshake it just ran.
            const int stackFree = (int)uxTaskGetStackHighWaterMark(nullptr);
            if (s.uploadStackFree != stackFree) { s.uploadStackFree = stackFree; changed = true; }
            return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED; },
                             STROKE_ENGINE_ORIGIN);

        vTaskDelayUntil(&lastWakeTime, SESSION_UPLOAD_CAPTURE_MS / portTICK_PERIOD_MS);
    }
}

void SessionUploadService::capture(const SessionUploadSettings &settings)
{
    bool running = false;
    float positionMm = 0.0f;
    float speedMm = 0.0f;
    String servoState = "UNDEFINED";

    // Extract only what is needed: copying the whole state would allocate for
    // its Strings ten times a second for nothing.
    _strokeState->read([&](StrokeState &s)
                       {
        running = s.run;
        positionMm = s.position;
        speedMm = s.speedMeasured;
        servoState = s.servoState; });

    // Samples are taken only while the engine runs. A pause in the middle of a
    // session stays one session and does not fill the archive with a flat line.
    if (!running)
    {
        return;
    }

    int torqueFeedback = 0;
    _probe->read([&torqueFeedback](ModbusProbeState &p)
                 { torqueFeedback = p.tqFb; });

    // The same signing the probe publishes: outward positive, towards home
    // negative, and the drive's own sign while standing still (on a vertical axis
    // that is negative, because the drive is holding the carriage up).
    const float rawPct = torqueFeedback / 10.0f;
    const float directedPct = (strokeTravelDirection == 0) ? rawPct
                                                     : (strokeTravelDirection > 0) ? fabsf(rawPct)
                                                                                   : -fabsf(rawPct);

    SessionUploadSample &sample = _ring[_ringWrite];
    sample.uptimeMs = millis();
    sample.epochMs = _baseEpochMs + (int64_t)(millis() - _baseUptimeMs);
    sample.positionMm = positionMm;
    sample.speedMm = speedMm;
    sample.torquePct = directedPct;
    sample.forceN = directedPct * 3.8f;  // 100 % of rating is 380 N
    sample.direction = (int8_t)strokeTravelDirection;
    // Guard the copy: this runs while the machine moves and a session is open,
    // which is when the heap is tightest, and strncpy() with a null source on an
    // exhausted String is a crash rather than an error.
    if (servoState.length() > 0)
    {
        strncpy(sample.servoState, servoState.c_str(), sizeof(sample.servoState) - 1);
        sample.servoState[sizeof(sample.servoState) - 1] = '\0';
    }
    else
    {
        strncpy(sample.servoState, "?", sizeof(sample.servoState));
    }

    _ringWrite = (_ringWrite + 1) % SESSION_UPLOAD_RING;
    if (_ringCount < SESSION_UPLOAD_RING)
    {
        _ringCount++;
    }
    else
    {
        // The buffer is full and the oldest unsent sample has just been
        // overwritten. Count that instead of hiding it.
        _droppedThisSession++;
    }
}

bool SessionUploadService::postJson(const char *path, const char *body, size_t bodyLength, String &response)
{
    SessionUploadSettings settings;
    read([&settings](SessionUploadSettings &s)
         { settings = s; });

    char url[192];
    snprintf(url, sizeof(url), "https://%s:%d%s", settings.host.c_str(), settings.port, path);

    if (!_clientStarted)
    {
        // One pinned root: the proxy serves Let's Encrypt certificates, whose
        // chain ends at ISRG Root X1. No certificate bundle needed.
        _client.setCACert(OSDB_ROOT_CA);
        _client.setHandshakeTimeout(15);
        _clientStarted = true;
    }

    // The TLS handshake is CPU work with no yield point inside it, and this task
    // runs on core 0. With the idle-task watchdog armed (CONFIG_ESP_TASK_WDT_
    // TIMEOUT_S=5 and CHECK_IDLE_TASK_CPU0=1) the machine RESET mid-handshake
    // instead of reporting a network error, which also hid the error itself. The
    // request is bounded by the HTTP timeout, so the window is bounded, and it is
    // closed again on every path out of this function.
    const bool wdtWasArmed = disableCore0WDT();

    bool ok = false;
    if (!_https.begin(_client, url))
    {
        _lastError = "bad url";
        response = _lastError;
    }
    else
    {
        _https.setTimeout(SESSION_UPLOAD_TIMEOUT_MS);
        _https.setReuse(true);
        _https.addHeader("Content-Type", "application/json");
        _https.addHeader("X-OSSM-Key", settings.key);

        int code = _https.POST((uint8_t *)body, bodyLength);
        if (code > 0)
        {
            response = _https.getString();
        }
        else
        {
            // A negative code is not an HTTP status at all: the request never made
            // it to the server. Ask the TLS stack why, because "not connected"
            // covers everything from a bad certificate to a heap that could not
            // fit the handshake, and those need different answers.
            char tlsReason[96] = {0};
            const int sslCode = _client.lastError(tlsReason, sizeof(tlsReason));
            _lastError = HTTPClient::errorToString(code);
            if (sslCode != 0 && tlsReason[0] != '\0')
            {
                _lastError += " / ";
                _lastError += tlsReason;
            }
            response = _lastError;
        }
        _https.end();

        if (code >= 200 && code < 300)
        {
            _lastError = "";
            ok = true;
        }
        else
        {
            if (code > 0)
            {
                // The server answered and refused. Its own message is the useful part.
                _lastError = "http ";
                _lastError += code;
                if (response.length() > 0)
                {
                    _lastError += " ";
                    _lastError += response.substring(0, 60);
                }
            }
            // A failed request may leave a half-open TLS session behind; drop it so
            // the next attempt starts from a clean handshake instead of reusing a
            // connection the server has already closed.
            _client.stop();
        }
    }

    if (wdtWasArmed)
    {
        enableCore0WDT();
    }
    return ok;
}

bool SessionUploadService::openSession(const SessionUploadSettings &settings)
{
    StrokeState state;
    _strokeState->read([&state](StrokeState &s)
                       { state = s; });

    StrokeSettings machine;
    _machineSettings->read([&machine](StrokeSettings &s)
                           { machine = s; });

    // Pattern names come from a compile time table of plain ASCII, so they cannot
    // carry a quote that would break this body.
    char body[512];
    int length = snprintf(body, sizeof(body),
                          "{\"device\":\"%s\",\"pattern\":\"%s\",\"depth_mm\":%.1f,\"stroke_mm\":%.1f,"
                          "\"speed_mms\":%.1f,\"sensation\":%.1f,\"slow_start\":%.1f,"
                          "\"torque_outward_pct\":%.1f,\"torque_inward_pct\":%.1f}",
                          settings.device.c_str(), state.patternName.c_str(),
                          state.depth, state.stroke, state.speed, state.sensation, state.slowStart,
                          machine.torque_outward_pct, machine.torque_inward_pct);
    if (length <= 0 || (size_t)length >= sizeof(body))
    {
        return false;
    }

    String response;
    if (!postJson("/ingest/session/start", body, (size_t)length, response))
    {
        return false;
    }

    const int id = extractSessionId(response);
    if (id <= 0)
    {
        return false;
    }

    _sessionId = id;
    _sentThisSession = 0;
    _droppedThisSession = 0;
    _ringCount = 0;
    _ringWrite = 0;
    _baseEpochMs = (int64_t)time(nullptr) * 1000;
    _baseUptimeMs = millis();
    _lastFlushMs = millis();
    return true;
}

bool SessionUploadService::flushSamples(const SessionUploadSettings &settings, bool force)
{
    if (_ringCount == 0)
    {
        return true;
    }

    // Send the oldest first. `force` is the session-close path: everything that
    // is still buffered goes in one body, a few batches at a time if it is more
    // than one batch will hold.
    int remaining = _ringCount;
    int oldest = (_ringWrite - _ringCount + SESSION_UPLOAD_RING) % SESSION_UPLOAD_RING;

    while (remaining > 0)
    {
        const int take = (remaining < settings.batch) ? remaining : settings.batch;

        int written = snprintf(osdbBody, sizeof(osdbBody), "{\"session_id\":%d,\"samples\":[", _sessionId);
        for (int i = 0; i < take; i++)
        {
            const SessionUploadSample &s = _ring[(oldest + i) % SESSION_UPLOAD_RING];
            char stamp[32];
            iso8601FromMs(s.epochMs, stamp, sizeof(stamp));
            written += snprintf(osdbBody + written, sizeof(osdbBody) - written,
                                "%s{\"t\":\"%s\",\"uptime_ms\":%lu,\"position_mm\":%.2f,"
                                "\"speed_mms\":%.2f,\"torque_pct\":%.2f,\"force_n\":%.2f,"
                                "\"direction\":%d,\"servo_state\":\"%s\"}",
                                (i == 0) ? "" : ",", stamp, (unsigned long)s.uptimeMs,
                                s.positionMm, s.speedMm, s.torquePct, s.forceN,
                                (int)s.direction, s.servoState);
        }
        written += snprintf(osdbBody + written, sizeof(osdbBody) - written, "]}");

        if (written <= 0 || (size_t)written >= sizeof(osdbBody))
        {
            // A batch that does not fit is a bug in the sizing, not a network
            // problem. Drop it loudly rather than send a truncated body.
            _droppedThisSession += take;
            oldest = (oldest + take) % SESSION_UPLOAD_RING;
            remaining -= take;
            _ringCount -= take;
            continue;
        }

        String response;
        if (!postJson("/ingest/samples", osdbBody, (size_t)written, response))
        {
            // Keep the samples: the next tick tries again. Losing a trace because
            // of one dropped packet would be the wrong trade.
            return false;
        }

        _sentThisSession += take;
        oldest = (oldest + take) % SESSION_UPLOAD_RING;
        remaining -= take;
        _ringCount -= take;

        // On a forced flush a slow link would keep this task busy for a long
        // time; give the rest of the system a slot between batches.
        if (force && remaining > 0)
        {
            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
    }

    _lastFlushMs = millis();
    return true;
}

void SessionUploadService::closeSession(const SessionUploadSettings &settings)
{
    if (_sessionId <= 0)
    {
        _recording = false;
        return;
    }

    char body[64];
    int length = snprintf(body, sizeof(body), "{\"session_id\":%d}", _sessionId);
    String response;
    postJson("/ingest/session/stop", body, (size_t)length, response);

    _sessionId = 0;
    _recording = false;
    _ringCount = 0;
    _ringWrite = 0;
    _sentThisSession = 0;
    _droppedThisSession = 0;
}
