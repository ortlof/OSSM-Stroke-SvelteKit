#!/usr/bin/env python3
"""osdb - session and torque archive for the OSSM stroke machine.

Two surfaces, deliberately separated:

  /ingest/*   the machine posts here. Key-authenticated, reachable from the
              internet through the central proxy, because the machine cannot
              reach this network any other way. It accepts a session open, a
              batch of samples, and a session close.

  /           the dashboard. Basic auth AND an IP allowlist (this LAN and the
              tailnet only), because it shows what was done with the machine.

Timestamps are the server's: the device has no clock at all (FT_NTP is off), so
it sends its uptime and this process stamps the wall time on arrival.

Run under gunicorn or Flask's built-in server via systemd; see ossm-ingest.service.
"""

import datetime
import functools
import hmac
import ipaddress
import os

import psycopg2
import psycopg2.extras
from flask import Flask, Response, g, jsonify, request

DSN = os.environ.get("OSSM_DSN", "dbname=ossm user=ossm")
INGEST_KEY_FILE = os.environ.get("OSSM_INGEST_KEY_FILE", "/etc/osdb/ingest.key")
DASH_USER = os.environ.get("OSSM_DASH_USER", "osdb")
DASH_PASS_FILE = os.environ.get("OSSM_DASH_PASS_FILE", "/etc/osdb/dashboard.pass")

# Only these networks may open the dashboard or the serial console. 10.99.10.0/24 is this
# LAN, 100.64.0.0/10 is the CGNAT range Tailscale uses, and 127.0.0.0/8 is loopback.
#
# The operator's own connection is a public address, and the proxy forwards the real
# client address, so his home IP has to be named here or the pages refuse him - the same
# treatment his CrowdSec whitelist gets on npn. Anything further can be added without a
# code change through OSSM_EXTRA_ALLOW (comma separated), so a dynamic address does not
# need an edit and a redeploy.
ALLOWED_NETS = [
    ipaddress.ip_network("10.99.10.0/24"),
    ipaddress.ip_network("127.0.0.0/8"),
    ipaddress.ip_network("100.64.0.0/10"),
    ipaddress.ip_network("84.191.41.89/32"),
]

app = Flask(__name__)

for _extra in (os.environ.get("OSSM_EXTRA_ALLOW") or "").split(","):
    _extra = _extra.strip()
    if _extra:
        try:
            ALLOWED_NETS.append(ipaddress.ip_network(_extra))
        except ValueError:
            app.logger.warning("ignoring unparsable OSSM_EXTRA_ALLOW entry: %r", _extra)


def _secret(path):
    with open(path) as fh:
        return fh.read().strip()


def db():
    if "db" not in g:
        g.db = psycopg2.connect(DSN)
    return g.db


@app.teardown_appcontext
def _close(_exc):
    conn = g.pop("db", None)
    if conn is not None:
        conn.close()


def ingest_auth(fn):
    """Constant-time key check. A plain == on a secret leaks its length by timing."""

    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        sent = request.headers.get("X-OSSM-Key", "")
        if not hmac.compare_digest(sent, _secret(INGEST_KEY_FILE)):
            return jsonify(error="bad key"), 401
        return fn(*args, **kwargs)

    return wrapper


def client_allowed():
    forwarded = request.headers.get("X-Forwarded-For", "")
    addr = (forwarded.split(",")[0].strip() if forwarded else "") or request.remote_addr or ""
    try:
        ip = ipaddress.ip_address(addr)
    except ValueError:
        return False
    return any(ip in net for net in ALLOWED_NETS)


def dashboard_auth(fn):
    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        if not client_allowed():
            return Response("not available from here\n", status=403, mimetype="text/plain")
        auth = request.authorization
        ok = (
            auth is not None
            and hmac.compare_digest(auth.username or "", DASH_USER)
            and hmac.compare_digest(auth.password or "", _secret(DASH_PASS_FILE))
        )
        if not ok:
            return Response(
                "authentication required\n",
                status=401,
                headers={"WWW-Authenticate": 'Basic realm="osdb"'},
                mimetype="text/plain",
            )
        return fn(*args, **kwargs)

    return wrapper


# --------------------------------------------------------------------------- #
# ingest                                                                      #
# --------------------------------------------------------------------------- #


@app.post("/ingest/session/start")
@ingest_auth
def session_start():
    b = request.get_json(force=True, silent=True) or {}
    cur = db().cursor()
    cur.execute(
        """INSERT INTO sessions (device, pattern, depth_mm, stroke_mm, speed_mms,
                                 sensation, slow_start, torque_outward_pct,
                                 torque_inward_pct, note)
           VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)
           RETURNING id, started_at""",
        (
            b.get("device") or "ossm",
            b.get("pattern"),
            b.get("depth_mm"),
            b.get("stroke_mm"),
            b.get("speed_mms"),
            b.get("sensation"),
            b.get("slow_start"),
            b.get("torque_outward_pct"),
            b.get("torque_inward_pct"),
            b.get("note"),
        ),
    )
    sid, started = cur.fetchone()
    db().commit()
    return jsonify(session_id=sid, started_at=started.isoformat()), 201


@app.post("/ingest/samples")
@ingest_auth
def ingest_samples():
    b = request.get_json(force=True, silent=True) or {}
    sid = b.get("session_id")
    rows = b.get("samples") or []
    if not sid or not rows:
        return jsonify(error="session_id and samples are required"), 400
    values = [
        (
            sid,
            s.get("t"),
            s.get("uptime_ms"),
            s.get("position_mm"),
            s.get("speed_mms"),
            s.get("torque_pct"),
            s.get("force_n"),
            s.get("direction"),
            s.get("servo_state"),
        )
        for s in rows
    ]
    cur = db().cursor()
    psycopg2.extras.execute_values(
        cur,
        """INSERT INTO samples (session_id, t, uptime_ms, position_mm, speed_mms,
                                 torque_pct, force_n, direction, servo_state)
           VALUES %s""",
        values,
        # t is optional: a sample without a device timestamp is stamped on arrival.
        template="(%s, coalesce(%s::timestamptz, now()), %s, %s, %s, %s, %s, %s, %s)",
    )
    db().commit()
    return jsonify(accepted=len(values)), 202


@app.post("/ingest/session/stop")
@ingest_auth
def session_stop():
    b = request.get_json(force=True, silent=True) or {}
    sid = b.get("session_id")
    if not sid:
        return jsonify(error="session_id is required"), 400
    cur = db().cursor()
    cur.execute("UPDATE sessions SET ended_at = now() WHERE id = %s", (sid,))
    db().commit()
    return jsonify(session_id=sid, closed=cur.rowcount == 1)


@app.get("/ingest/health")
def health():
    """No auth: the proxy and a monitoring check need something cheap to poll."""
    cur = db().cursor()
    cur.execute("SELECT count(*) FROM sessions")
    return jsonify(ok=True, sessions=cur.fetchone()[0])


# --------------------------------------------------------------------------- #
# dashboard                                                                   #
# --------------------------------------------------------------------------- #

PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OSSM sessions</title>
<style>
 :root{color-scheme:light dark}
 body{font:14px/1.45 system-ui,sans-serif;margin:1.2rem;max-width:60rem}
 h1{font-size:1.15rem;margin:0 0 .2rem}
 .muted{opacity:.65}
 table{border-collapse:collapse;width:100%;margin-top:.8rem}
 th,td{text-align:left;padding:.3rem .5rem;border-bottom:1px solid #8883}
 th{font-weight:600;opacity:.75}
 td.num{text-align:right;font-variant-numeric:tabular-nums}
 a{color:inherit}
 svg{width:100%;height:150px;display:block;margin-top:.6rem}
 .card{border:1px solid #8883;border-radius:10px;padding:.8rem 1rem;margin-bottom:.9rem}
</style></head><body>
<h1>OSSM sessions</h1>
<div class="muted">__SUBTITLE__</div>
__BODY__
</body></html>"""


@app.get("/")
@dashboard_auth
def dashboard():
    cur = db().cursor(cursor_factory=psycopg2.extras.RealDictCursor)
    cur.execute("SELECT * FROM session_summary LIMIT 200")
    rows = cur.fetchall()
    if not rows:
        body = '<p class="muted">Nothing recorded yet.</p>'
    else:
        head = (
            "<table><tr><th>#</th><th>started</th><th>pattern</th>"
            "<th class='num'>depth</th><th class='num'>stroke</th><th class='num'>spd</th>"
            "<th class='num'>secs</th><th class='num'>samples</th>"
            "<th class='num'>peak out</th><th class='num'>peak home</th><th class='num'>peak N</th></tr>"
        )
        trs = []
        for r in rows:
            started = r["started_at"].astimezone().strftime("%Y-%m-%d %H:%M")
            trs.append(
                "<tr><td><a href='/session/%s'>%s</a></td><td>%s</td><td>%s</td>"
                "<td class='num'>%s</td><td class='num'>%s</td><td class='num'>%s</td>"
                "<td class='num'>%s</td><td class='num'>%s</td>"
                "<td class='num'>%s</td><td class='num'>%s</td><td class='num'>%s</td></tr>"
                % (
                    r["id"], r["id"], started, r["pattern"] or "-",
                    _n(r["depth_mm"], 0), _n(r["stroke_mm"], 0), _n(r["speed_mms"], 0),
                    _n(r["seconds"], 0), r["samples"],
                    _n(r["peak_outward_pct"], 1), _n(r["peak_home_pct"], 1), _n(r["peak_force_n"], 1),
                )
            )
        body = head + "".join(trs) + "</table>"
    return PAGE.replace("__SUBTITLE__", "%d session(s), 30-day retention" % len(rows)).replace("__BODY__", body)


@app.get("/session/<int:sid>")
@dashboard_auth
def session_detail(sid):
    cur = db().cursor(cursor_factory=psycopg2.extras.RealDictCursor)
    cur.execute("SELECT * FROM sessions WHERE id = %s", (sid,))
    s = cur.fetchone()
    if not s:
        return Response("no such session\n", status=404, mimetype="text/plain")
    cur.execute(
        "SELECT extract(epoch FROM t) AS epoch, position_mm, speed_mms, torque_pct, force_n "
        "FROM samples WHERE session_id = %s ORDER BY t",
        (sid,),
    )
    pts = cur.fetchall()
    meta = (
        "<div class='card'><b>Session %s</b> &middot; %s &middot; %s<br>"
        "<span class='muted'>pattern %s &middot; depth %s mm &middot; stroke %s mm &middot; "
        "speed %s mm/s &middot; %d samples</span></div>"
        % (
            s["id"], s["started_at"].astimezone().strftime("%Y-%m-%d %H:%M:%S"),
            s["device"], s["pattern"] or "-", _n(s["depth_mm"], 0), _n(s["stroke_mm"], 0),
            _n(s["speed_mms"], 0), len(pts),
        )
    )
    return PAGE.replace("__SUBTITLE__", "<a href='/'>back</a>").replace(
        "__BODY__",
        meta
        + _chart(pts, "torque_pct", "torque %, signed (+ outward)")
        + _chart(pts, "position_mm", "position mm"),
    )


@app.get("/serial")
@dashboard_auth
def serial_console():
    """The Web Serial console for the machine's own USB port.

    It is served from here for one reason: a SECURE CONTEXT. A browser hands out
    serial ports to https or localhost and to nothing else, so a page on plain
    http can never open the port however well it is written. Same gate as the
    dashboard - allowlist and password - because this page can drive the board.
    """
    path = os.environ.get("OSSM_SERIAL_PAGE", "serial-console.html")
    with open(path, "rb") as fh:
        return Response(fh.read(), mimetype="text/html")


def _n(v, digits):
    return "-" if v is None else ("%.*f" % (digits, v))


def _chart(points, key, title):
    """An inline SVG trace. No JavaScript, no chart library, no CDN."""
    vals = [p[key] for p in points if p[key] is not None]
    if len(vals) < 2:
        return "<p class='muted'>%s: not enough samples</p>" % title
    lo, hi = min(vals), max(vals)
    if hi - lo < 1e-9:
        hi, lo = lo + 1.0, lo - 1.0
    w, h, pad = 600.0, 150.0, 6.0
    step = (w - 2 * pad) / (len(vals) - 1)
    path = " ".join(
        "%.1f,%.1f" % (pad + i * step, h - pad - (v - lo) / (hi - lo) * (h - 2 * pad))
        for i, v in enumerate(vals)
    )
    zero = ""
    if lo <= 0 <= hi:
        y = h - pad - (0 - lo) / (hi - lo) * (h - 2 * pad)
        zero = "<line x1='%.1f' y1='%.1f' x2='%.1f' y2='%.1f' stroke='#8886' stroke-dasharray='4 3'/>" % (
            pad, y, w - pad, y,
        )
    return (
        "<div class='card'><div class='muted'>%s &nbsp; min %.1f &nbsp; max %.1f</div>"
        "<svg viewBox='0 0 %d %d' preserveAspectRatio='none'>%s"
        "<polyline fill='none' stroke='#4a90d9' stroke-width='1.6' points='%s'/></svg></div>"
        % (title, lo, hi, int(w), int(h), zero, path)
    )


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("OSSM_PORT", "8080")))
