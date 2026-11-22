-- osdb: OSSM session and torque archive.
--
-- One row per recorded session, one row per telemetry sample. The device only
-- records when the operator arms it, so a test run that nobody asked to record
-- leaves no trace - which is the "real play only, no test data" rule expressed in
-- the schema instead of in a promise.
--
-- Retention is 30 days, enforced by the daily prune job, not by a trigger: a
-- trigger would fire inside the device's ingest path and stall it.

CREATE TABLE IF NOT EXISTS sessions (
    id                  bigserial PRIMARY KEY,
    device              text        NOT NULL,
    started_at          timestamptz NOT NULL DEFAULT now(),
    ended_at            timestamptz,
    pattern             text,
    depth_mm            real,
    stroke_mm           real,
    speed_mms           real,
    sensation           real,
    slow_start          real,
    torque_outward_pct  real,
    torque_inward_pct   real,
    note                text
);

CREATE TABLE IF NOT EXISTS samples (
    id           bigserial PRIMARY KEY,
    session_id   bigint      NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    t            timestamptz NOT NULL DEFAULT now(),
    uptime_ms    bigint,
    position_mm  real,
    speed_mms    real,
    torque_pct   real,      -- signed by direction: + outward, - back to home
    force_n      real,
    direction    smallint,  -- +1 outward, -1 home, 0 holding
    servo_state  text
);

CREATE INDEX IF NOT EXISTS samples_session_t_idx ON samples (session_id, t);
CREATE INDEX IF NOT EXISTS sessions_started_idx   ON sessions (started_at DESC);

-- What the dashboard lists, computed in the database so a page load never scans
-- the sample table in the application.
CREATE OR REPLACE VIEW session_summary AS
SELECT s.id,
       s.device,
       s.started_at,
       s.ended_at,
       s.pattern,
       s.depth_mm,
       s.stroke_mm,
       s.speed_mms,
       s.note,
       count(m.id)                                            AS samples,
       round(avg(m.torque_pct)::numeric, 2)                   AS avg_torque,
       round(max(m.torque_pct)::numeric, 2)                   AS peak_outward_pct,
       round(min(m.torque_pct)::numeric, 2)                   AS peak_home_pct,
       round(max(m.force_n)::numeric, 1)                      AS peak_force_n,
       round(min(m.force_n)::numeric, 1)                      AS min_force_n,
       round(extract(epoch FROM (coalesce(s.ended_at, now()) - s.started_at))::numeric, 0) AS seconds
FROM sessions s
LEFT JOIN samples m ON m.session_id = s.id
GROUP BY s.id
ORDER BY s.started_at DESC;
