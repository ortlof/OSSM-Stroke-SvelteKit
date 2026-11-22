#!/usr/bin/env python3
"""Verify the operator-presence dead-man on real hardware.

What it proves: if the operator's client goes away while the machine runs, the
firmware stops the motion by itself. It does that by playing the operator with a
raw WebSocket client, starting a slow sweep, closing the socket properly, and
watching for the firmware to stop the machine.

Why each step is shaped the way it is:

  - The socket is opened BEFORE the run starts. The dead-man arms on the run
    transition and only if a client was present at that moment, so connecting
    afterwards would arm nothing and the test would "pass" for the wrong reason.
  - The socket is closed with a real WebSocket close frame (opcode 0x8), not just
    a dropped TCP connection. The event socket notices the close frame at once
    and the client count drops immediately; a bare TCP close would only be
    noticed by the ping handling, which is much slower.
  - The sweep runs at low speed inside a hard position window, with run=false in
    a finally, because this moves a real machine.

Run it only while the operator is present and the machine is clear.

    /root/.pio-venv/bin/python scripts/deadman-test.py

Exit code 0 means the alarm fired. Anything else means it did not.
"""

import base64
import json
import os
import socket
import sys
import time
import urllib.request

HOST = "192.168.2.115"
USER = "admin"
PASSWORD = "admin"

# Low speed, small sweep, well inside the usable travel.
DEPTH_MM = 40.0
STROKE_MM = 30.0          # so the sweep runs 10 .. 40 mm
SPEED_MM_S = 5.0
WINDOW_MM = (-5.0, 70.0)  # hard limit: leave this and the test aborts
RUN_FOR_S = 12            # how long to let it sweep before the client leaves
WAIT_FOR_ALARM_S = 12     # grace period is 3 s, so this is generous


def request(path, data=None, token=None):
    req = urllib.request.Request(
        "http://%s%s" % (HOST, path),
        data=json.dumps(data).encode() if data else None,
        method="POST" if data else "GET",
    )
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read().decode())


def main():
    token = request("/rest/signIn", {"username": USER, "password": PASSWORD})["access_token"]

    def state():
        return request("/rest/strokeState", token=token)

    start = state()
    if not start.get("homed"):
        print("NOT HOMED - refusing to run this test")
        return 2
    print("homed=%s servo=%s position=%.1f" % (start.get("homed"), start.get("servo_state"), start.get("position", 0)))

    ws = None
    fired = False
    try:
        # Play the operator.
        key = base64.b64encode(os.urandom(16)).decode()
        ws = socket.create_connection((HOST, 80), timeout=10)
        ws.sendall((
            "GET /ws/events?access_token=%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
            % (token, HOST, key)).encode())
        print("operator socket: %s" % ws.recv(1024).split(b"\r\n")[0].decode(errors="replace"))
        time.sleep(2)

        request("/rest/strokeState",
                {"depth": DEPTH_MM, "stroke": STROKE_MM, "speed": SPEED_MM_S, "run": True},
                token)
        print("sweep started: depth %.0f stroke %.0f speed %.1f mm/s"
              % (DEPTH_MM, STROKE_MM, SPEED_MM_S))

        t0 = time.time()
        while time.time() - t0 < RUN_FOR_S:
            d = state()
            p = d.get("position", 0.0)
            print("  t=%4.1fs run=%-5s pos=%6.2f safe_stop=%-5s clients=%s"
                  % (time.time() - t0, d.get("run"), p, d.get("safe_stop"), d.get("ws_clients")))
            if not (WINDOW_MM[0] <= p <= WINDOW_MM[1]):
                print("POSITION LEFT THE WINDOW - emergency stop")
                request("/rest/strokeState", {"run": False}, token)
                return 3
            time.sleep(2)

        print("operator closes the socket (proper close frame)")
        ws.sendall(b"\x88\x82" + os.urandom(4) + b"\x03\xe8")
        time.sleep(1)
        ws.close()
        ws = None

        t1 = time.time()
        while time.time() - t1 < WAIT_FOR_ALARM_S:
            d = state()
            print("  +%4.1fs run=%-5s pos=%6.2f safe_stop=%-5s clients=%s"
                  % (time.time() - t1, d.get("run"), d.get("position", 0), d.get("safe_stop"), d.get("ws_clients")))
            if d.get("run") is False and d.get("safe_stop") is True:
                fired = True
                print("ALARM FIRED: the firmware stopped the motion on its own")
                break
            time.sleep(2)
    finally:
        request("/rest/strokeState", {"run": False}, token)
        if ws is not None:
            ws.close()
        time.sleep(2)
        d = state()
        print("final: run=%s safe_stop=%s position=%.2f servo=%s"
              % (d.get("run"), d.get("safe_stop"), d.get("position", 0), d.get("servo_state")))

    if not fired:
        print("NO ALARM within %d s - the dead-man did not fire" % WAIT_FOR_ALARM_S)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
