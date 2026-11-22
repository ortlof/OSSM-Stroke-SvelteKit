#!/bin/bash
# Deploy osdb inside the container: packages, database, secrets, schema, service,
# prune timer. Idempotent - re-running updates the app and leaves the data alone.
#
# scp app.py, schema.sql and ossm-ingest.service to /root/osdb-src on the
# container, then run this there.
set -euo pipefail

SRC=${SRC:-/root/osdb-src}
APP_DIR=/opt/osdb
ETC_DIR=/etc/osdb
DB_NAME=ossm
DB_USER=ossm

echo "== packages =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq postgresql python3-psycopg2 python3-flask gunicorn curl openssl >/dev/null

echo "== service account and directories =="
id -u osdb >/dev/null 2>&1 || useradd --system --home "$APP_DIR" --shell /usr/sbin/nologin osdb
install -d -m 0755 "$APP_DIR"
install -d -m 0750 -o root -g osdb "$ETC_DIR"

echo "== secrets (created once, never overwritten) =="
# alphanumeric only, so nothing needs quoting in an EnvironmentFile or a DSN
[ -s "$ETC_DIR/ingest.key" ]     || openssl rand -hex 32 > "$ETC_DIR/ingest.key"
[ -s "$ETC_DIR/dashboard.pass" ] || openssl rand -base64 48 | tr -dc 'A-Za-z0-9' | cut -c1-24 > "$ETC_DIR/dashboard.pass"
[ -s "$ETC_DIR/db.pass" ]        || openssl rand -base64 48 | tr -dc 'A-Za-z0-9' | cut -c1-24 > "$ETC_DIR/db.pass"
chown root:osdb "$ETC_DIR"/*.key "$ETC_DIR"/*.pass
chmod 0640 "$ETC_DIR"/*.key "$ETC_DIR"/*.pass

DB_PASS=$(cat "$ETC_DIR/db.pass")

echo "== database role and schema =="
# postgres runs as its own user and /root is 0700, so hand it a readable copy
install -m 0644 "$SRC/schema.sql" /tmp/osdb-schema.sql
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -q <<SQL
DO \$\$ BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = '$DB_USER') THEN
    CREATE ROLE $DB_USER LOGIN PASSWORD '$DB_PASS';
  ELSE
    ALTER ROLE $DB_USER LOGIN PASSWORD '$DB_PASS';
  END IF;
END \$\$;
SQL
runuser -u postgres -- psql -tAc "SELECT 1 FROM pg_database WHERE datname='$DB_NAME'" | grep -q 1 \
  || runuser -u postgres -- createdb -O "$DB_USER" "$DB_NAME"
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -q -d "$DB_NAME" -f /tmp/osdb-schema.sql
runuser -u postgres -- psql -v ON_ERROR_STOP=1 -q -d "$DB_NAME" \
  -c "GRANT USAGE ON SCHEMA public TO $DB_USER;" \
  -c "GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO $DB_USER;" \
  -c "GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA public TO $DB_USER;"
rm -f /tmp/osdb-schema.sql

echo "== application =="
install -m 0644 "$SRC/app.py" "$APP_DIR/app.py"
install -m 0644 "$SRC/serial-console.html" "$APP_DIR/serial-console.html"
install -m 0644 "$SRC/ossm-ingest.service" /etc/systemd/system/ossm-ingest.service
# Single-quoted: systemd's EnvironmentFile and a shell `.` both stop at the first
# unquoted space, which would leave the DSN as just "dbname=ossm".
printf "OSSM_DSN='dbname=%s user=%s password=%s host=127.0.0.1'\n" "$DB_NAME" "$DB_USER" "$DB_PASS" > "$ETC_DIR/env"
chown root:osdb "$ETC_DIR/env"; chmod 0640 "$ETC_DIR/env"

echo "== prune job (30-day retention, as a timer so no package is needed) =="
cat > /usr/local/bin/osdb-prune <<'PRUNE'
#!/bin/bash
# Retention is a decision, not a trigger: deleting inside the ingest path would
# stall the device. Runs daily; samples cascade through the foreign key.
set -eu
runuser -u postgres -- psql -q -d ossm -c \
  "DELETE FROM sessions WHERE coalesce(ended_at, started_at) < now() - interval '30 days';"
PRUNE
chmod 0755 /usr/local/bin/osdb-prune
cat > /etc/systemd/system/osdb-prune.service <<'UNIT'
[Unit]
Description=osdb 30-day retention prune
[Service]
Type=oneshot
ExecStart=/usr/local/bin/osdb-prune
UNIT
cat > /etc/systemd/system/osdb-prune.timer <<'UNIT'
[Unit]
Description=run the osdb prune daily
[Timer]
OnCalendar=*-*-* 04:17:00
Persistent=true
[Install]
WantedBy=timers.target
UNIT

echo "== start =="
systemctl daemon-reload
systemctl enable --now osdb-prune.timer >/dev/null 2>&1
systemctl enable ossm-ingest >/dev/null 2>&1
systemctl restart ossm-ingest
sleep 3
echo "service: $(systemctl is-active ossm-ingest)   timer: $(systemctl is-active osdb-prune.timer)"
curl -s -o /dev/null -w "health: %{http_code}\n" http://127.0.0.1:8080/ingest/health || true
echo
echo "secrets live in $ETC_DIR (0640 root:osdb)"
