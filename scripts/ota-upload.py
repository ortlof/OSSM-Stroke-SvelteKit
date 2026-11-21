#!/usr/bin/env python3
"""OTA update for the OSSM over the ESP32-SvelteKit HTTP endpoint.

This firmware has no espota/ArduinoOTA. For an update over the network the
device accepts a firmware image at

    POST /rest/uploadFirmware      multipart, form field "file", JWT required

writes it to the next OTA partition and reboots. Upload progress arrives on the
WebSocket event "otastatus". That is the same path the browser uses on the
System -> Update page.

usage:
    scripts/ota-upload.py --device 192.168.2.115 build/release/OSSM-Stroke_esp32dev_0-7-0.bin
    scripts/ota-upload.py --device 192.168.2.115 --check          # login and report only

Options: --user, --password (factory defaults admin/admin), --no-progress.
"""

import argparse
import json
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

try:
    import msgpack
except ImportError:                              # pragma: no cover
    sys.exit("msgpack is required for the progress display: python3 -m pip install msgpack")

try:
    from websockets.sync.client import connect as ws_connect   # websockets >= 13
except ImportError:                              # pragma: no cover
    ws_connect = None


def api(device, path, data=None, token=None, timeout=15):
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(
        "http://%s%s" % (device, path), data=body,
        method="POST" if data is not None else "GET")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def sign_in(device, user, password):
    return api(device, "/rest/signIn", {"username": user, "password": password})["access_token"]


def wait_for_device(device, timeout=90):
    """Return systemStatus once the device answers again, None on timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            token = sign_in(device, args.user, args.password)
            return api(device, "/rest/systemStatus", token=token)
        except Exception:
            time.sleep(2)
    return None


def watch_progress(device, token, stop):
    """Print upload progress from the 'otastatus' event until the socket dies."""
    try:
        if ws_connect is None:
            return
        # no keepalive pings: the device reboots when the write completes and the
        # library would print a timeout traceback from its own thread
        with ws_connect("ws://%s/ws/events?access_token=%s" % (device, token), ping_interval=None) as ws:
            ws.send(msgpack.packb({"event": "subscribe", "data": "otastatus"}))
            last = -1
            while not stop.is_set():
                try:
                    raw = ws.recv(timeout=2)
                except TimeoutError:
                    continue
                msg = msgpack.unpackb(raw, raw=False, strict_map_key=False)
                if msg.get("event") != "otastatus":
                    continue
                d = msg.get("data", {})
                stage, pct = d.get("status"), d.get("progress")
                if d.get("status") == "progress" and pct is not None:
                    if pct >= last + 10 or pct == 100:
                        sys.stdout.write("  upload %3d%%  (%s / %s bytes)\n"
                                         % (pct, d.get("bytes_written"), d.get("total_bytes")))
                        sys.stdout.flush()
                        last = pct
                else:
                    print("  otastatus: %s" % json.dumps(d))
    except Exception:
        pass          # the device reboots mid-upload and drops the socket


def upload(device, token, binary):
    """POST the image as multipart/form-data."""
    boundary = "----ossm" + uuid.uuid4().hex
    with open(binary, "rb") as fh:
        payload = fh.read()
    name = os.path.basename(binary)
    pre = ("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
           "Content-Type: application/octet-stream\r\n\r\n" % (boundary, name)).encode()
    post = ("\r\n--%s--\r\n" % boundary).encode()
    body = pre + payload + post

    req = urllib.request.Request("http://%s/rest/uploadFirmware" % device, data=body, method="POST")
    req.add_header("Content-Type", "multipart/form-data; boundary=" + boundary)
    req.add_header("Content-Length", str(len(body)))
    req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=180) as resp:
            return resp.status, resp.read().decode()[:400]
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()[:400]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("binary", nargs="?")
    p.add_argument("--device", default="192.168.2.115")
    p.add_argument("--user", default="admin")
    p.add_argument("--password", default="admin")
    p.add_argument("--check", action="store_true", help="login, report the current build, do not upload")
    p.add_argument("--no-progress", action="store_true")
    global args
    args = p.parse_args()

    try:
        token = sign_in(args.device, args.user, args.password)
    except Exception as e:
        sys.exit("login failed: %s" % e)

    before = api(args.device, "/rest/systemStatus", token=token)
    print("device %s: version %s, sketch_size %s, uptime %s s"
          % (args.device, before.get("firmware_version"), before.get("sketch_size"), before.get("uptime")))

    if args.check or not args.binary:
        features = api(args.device, "/rest/features", token=token)
        state = api(args.device, "/rest/strokeState", token=token)
        print("features: %s" % json.dumps(features))
        print("strokeState keys: %s" % ", ".join(sorted(state.keys())))
        print("\ncheck only, nothing uploaded")
        return

    size = os.path.getsize(args.binary)
    print("local image %s: %d bytes" % (os.path.basename(args.binary), size))

    stop = threading.Event()
    watcher = None
    if not args.no_progress and ws_connect is not None:
        watcher = threading.Thread(target=watch_progress, args=(args.device, token, stop), daemon=True)
        watcher.start()
        time.sleep(1)

    print("uploading ...")
    status, text = upload(args.device, token, args.binary)
    stop.set()
    print("upload response: http=%s %s" % (status, text.strip()))
    if status != 200:
        sys.exit("upload rejected")

    print("waiting for the device to reboot ...")
    after = wait_for_device(args.device)
    if after is None:
        sys.exit("device did not come back within 90 s - check the serial log")

    delta = abs(after.get("sketch_size", 0) - size)
    print("\nafter reboot: version %s, sketch_size %s" % (after.get("firmware_version"), after.get("sketch_size")))
    print("local image size differs by %d bytes (image header and padding)" % delta)
    if delta > 2048:
        print("WARNING: the size difference is larger than expected - is the right image flashed?")
    print("uptime %s s, reset reason: %s" % (after.get("uptime"), after.get("cpu_reset_reason")))


if __name__ == "__main__":
    main()
