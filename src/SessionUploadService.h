#ifndef SessionUploadService_h
#define SessionUploadService_h

/*
 *   Session archive uploader: recorded sessions leave the machine.
 *
 *   The machine cannot be reached from the network that holds the archive, so
 *   this pushes instead of waiting to be polled. It speaks HTTPS to the small
 *   service in server/osdb, and nothing is sent until the settings enable it AND
 *   the operator arms the record control - which is the "real play, not test
 *   data" rule held by the design instead of by discipline.
 *
 *   One capture tick does one of these:
 *     record armed, no session open  -> POST /ingest/session/start, keep the id
 *     record armed, machine running  -> append a sample to the local ring buffer
 *     buffer full, or 5 s elapsed    -> POST /ingest/samples, drop what was sent
 *     record cleared                 -> final /ingest/samples, then /session/stop
 *
 *   Samples are captured only while the engine runs, so a pause in the middle of
 *   a session does not fill the archive with a flat line, while the session
 *   itself stays open across that pause and remains one recording.
 *
 *   Timestamps come from the device's own clock, which needs NTP: TLS cannot
 *   validate a certificate against 1970. So the task waits for the clock rather
 *   than sending anything it cannot stamp.
 */

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ESP32SvelteKit.h>
#include <FSPersistence.h>
#include <HttpEndpoint.h>
#include <StatefulService.h>

#include "ModbusProbeService.h"
#include "StrokeSettingsService.h"
#include "StrokeStateService.h"

#define SESSION_UPLOAD_ENDPOINT_PATH "/rest/sessionUpload"
#define SESSION_UPLOAD_FILE_PATH "/config/sessionUpload.json"

#define SESSION_UPLOAD_DEVICE_DEFAULT "ossm-stroke"
#define SESSION_UPLOAD_HOST_DEFAULT "osdb.d4rkn3t.de"
#define SESSION_UPLOAD_PORT_DEFAULT 443

// Twenty seconds of capture at 10 Hz. The buffer only has to ride out a flush
// interval or a short network hiccup, and DRAM is the scarce resource here: the
// TLS handshake needs one large contiguous allocation, so every kilobyte of
// static buffer is a kilobyte the handshake cannot have. At 48 bytes per sample
// this array is 9.6 kB, and shrinking it to 600 cost the handshake 29 kB of
// headroom and failed with "SSL - Memory allocation failed".
#define SESSION_UPLOAD_RING 100
#define SESSION_UPLOAD_BATCH_DEFAULT 10
// The body for one POST is built in a fixed buffer, so the batch has a hard
// ceiling: 10 samples at about 170 bytes each is roughly 1.7 kB. Every kilobyte
// of static buffer is a kilobyte the TLS handshake cannot have, and the handshake
// needs one large CONTIGUOUS block - which is the scarce resource on this board.
#define SESSION_UPLOAD_BATCH_MAX 10
#define SESSION_UPLOAD_CAPTURE_MS 100
#define SESSION_UPLOAD_FLUSH_MS 5000
#define SESSION_UPLOAD_TIMEOUT_MS 7000

// A clock earlier than this is not a clock, it is 1970 plus a few seconds of
// uptime. 2024-01-01, so any real sync clears it.
#define SESSION_UPLOAD_CLOCK_MIN_EPOCH 1704067200

struct SessionUploadSample
{
    uint32_t uptimeMs;
    int64_t epochMs;
    float positionMm;
    float speedMm;
    float torquePct;   // signed by direction of travel: + outward, - towards home
    float forceN;      // the same reading as rack force
    int8_t direction;  // +1 outward, -1 home, 0 holding
    char servoState[12];
};

class SessionUploadSettings
{
public:
    // Off by default. Turning this on is a deliberate act by the operator, and
    // the machine is a thing that sits in a room, so the default must not be
    // "sends data somewhere".
    bool enabled = false;

    String device = SESSION_UPLOAD_DEVICE_DEFAULT;  // name recorded against every session
    String host = SESSION_UPLOAD_HOST_DEFAULT;
    int port = SESSION_UPLOAD_PORT_DEFAULT;
    // The ingest key. It is a bearer token for one write-only endpoint, and it is
    // stored in plain text on LittleFS, so treat it as a write credential and
    // nothing more: it cannot read a session back.
    String key = "";
    int batch = SESSION_UPLOAD_BATCH_DEFAULT;  // samples per POST, 1 .. RING

    static void read(SessionUploadSettings &settings, JsonObject &root)
    {
        root["enabled"] = settings.enabled;
        root["device"] = settings.device;
        root["host"] = settings.host;
        root["port"] = settings.port;
        root["batch"] = settings.batch;
        // Deliberately NOT read back: a GET must never hand out the key. The
        // clients only need to know whether one is set.
        root["key_set"] = settings.key.length() > 0;
    }

    /// The FILE view: the HTTP view plus the secret itself.
    ///
    /// FSPersistence persists by serialising through a read function, so handing
    /// it the one above - which hides the key - also keeps the key OUT OF THE
    /// FILE. The machine then forgets its credentials on every reboot and the
    /// record button answers "no ingest key set", while the settings page still
    /// cheerfully reports enabled. That is exactly what happened the first time.
    /// Two readers, one updater: the secret persists, and it still never leaves
    /// over HTTP.
    static void readForFile(SessionUploadSettings &settings, JsonObject &root)
    {
        read(settings, root);
        root["key"] = settings.key;
    }

    static StateUpdateResult update(JsonObject &root, SessionUploadSettings &settings, const String &originId)
    {
        bool enabled = root["enabled"] | settings.enabled;
        String device = root["device"] | settings.device;
        String host = root["host"] | settings.host;
        int port = root["port"] | settings.port;
        int batch = root["batch"] | settings.batch;
        // The key is write-only: absent means "keep the one you have", so a client
        // can change the host without having to hold the secret.
        String key = root["key"] | settings.key;

        port = constrain(port, 1, 65535);
        batch = constrain(batch, 1, SESSION_UPLOAD_BATCH_MAX);
        if (device.length() > 63) device = device.substring(0, 63);
        if (host.length() > 127) host = host.substring(0, 127);

        if (enabled == settings.enabled && device == settings.device &&
            host == settings.host && port == settings.port && batch == settings.batch &&
            key == settings.key)
        {
            return StateUpdateResult::UNCHANGED;
        }

        settings.enabled = enabled;
        settings.device = device;
        settings.host = host;
        settings.port = port;
        settings.batch = batch;
        settings.key = key;
        return StateUpdateResult::CHANGED;
    }
};

class SessionUploadService : public StatefulService<SessionUploadSettings>
{
public:
    SessionUploadService(PsychicHttpServer *server, ESP32SvelteKit *sveltekit,
                         StrokeStateService *strokeState,
                         StrokeSettingsService *machineSettings,
                         ModbusProbeService *probe);

    void begin();

private:
    static void taskEntry(void *param);

    void captureLoop();
    bool clockReady();
    bool postJson(const char *path, const char *body, size_t bodyLength, String &response);
    bool openSession(const SessionUploadSettings &settings);
    bool flushSamples(const SessionUploadSettings &settings, bool force);
    void closeSession(const SessionUploadSettings &settings);
    void capture(const SessionUploadSettings &settings);
    void publishStatus(const char *state, const char *error);

    HttpEndpoint<SessionUploadSettings> _httpEndpoint;
    FSPersistence<SessionUploadSettings> _fsPersistence;

    StrokeStateService *_strokeState;
    StrokeSettingsService *_machineSettings;
    ModbusProbeService *_probe;

    TaskHandle_t _taskHandle = nullptr;

    // One TLS client, kept alive across requests: a fresh handshake every five
    // seconds costs about two seconds of CPU and a heap churn the rest of the
    // firmware can feel. Dropped and remade whenever a request fails.
    WiFiClientSecure _client;
    HTTPClient _https;
    bool _clientStarted = false;

    // Ring buffer and bookkeeping, owned by the capture task.
    SessionUploadSample _ring[SESSION_UPLOAD_RING];
    int _ringWrite = 0;
    int _ringCount = 0;
    int _sessionId = 0;
    bool _recording = false;
    uint32_t _lastFlushMs = 0;
    uint32_t _sentThisSession = 0;
    uint32_t _droppedThisSession = 0;
    // Why the last request failed, in the server's or the TLS stack's own words.
    // "could not be opened" tells the operator nothing; an mbedTLS reason or an
    // HTTP status tells them what to do next.
    String _lastError = "";
    // Retry backoff. Without it the loop re-attempted a full TLS handshake every
    // capture tick, and the handshake's crypto is CPU-bound: the idle task on
    // this core never ran, the task watchdog fired, and the machine REBOOTED
    // instead of reporting a network error. Rate limiting is also simply correct
    // behaviour towards an endpoint that is failing.
    uint32_t _retryAfterMs = 0;
    uint32_t _consecutiveFailures = 0;
    // The same discipline for the sample flush, and it was MISSING at first: the
    // flush path retried on every capture tick, so a failing handshake was
    // re-attempted ten times a second while the machine ran, starved the idle
    // task and reset the board. Measured: with Record armed the machine ran 12 s
    // and sent 0 samples, then reset.
    uint32_t _flushRetryAfterMs = 0;
    uint32_t _flushFailures = 0;
    // Timestamps are built from the clock once per session plus the monotonic
    // uptime since then: time() only resolves to a second, and a network step of
    // the clock must not put a jump in the middle of a trace.
    int64_t _baseEpochMs = 0;
    uint32_t _baseUptimeMs = 0;
    // Counters are published at most once a second. Sending them on every 100 ms
    // tick adds nothing a human can read, and it doubles the event rate exactly
    // when the engine is already pushing telemetry at 10 Hz - the moment the
    // device is busiest. State transitions are still published immediately.
    uint32_t _lastStatusPublishMs = 0;
};

#endif  // SessionUploadService_h
