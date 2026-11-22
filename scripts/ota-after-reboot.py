#!/usr/bin/env python3
"""
Flash over OTA on this board - but only the way this board allows it.

Hard-won rule: an ESP32 OTA upload needs a healthy contiguous heap. A completed
TLS handshake leaves max_alloc_heap near 28,660, and at that level the upload
times out part way and the board stays on the old build. So this always reboots
FIRST (POST /rest/restart, admin token), waits for the heap to recover, and only
then uploads. That sequence was measured to work twice; the other order fails.

Usage: ota-after-reboot.py <path-to.bin> [expected-version]
"""
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

DEVICE = "192.168.2.115"
MIN_MAX_ALLOC = 50000


def call(path, data=None, tok=None, timeout=20):
    req = urllib.request.Request(
        "http://%s%s" % (DEVICE, path),
        data=json.dumps(data).encode() if data is not None else None,
        method="POST" if data is not None else "GET")
    req.add_header("Content-Type", "application/json")
    if tok:
        req.add_header("Authorization", "Bearer " + tok)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def token():
    return call("/rest/signIn", {"username": "admin", "password": "admin"})["access_token"]


def status(tok):
    return call("/rest/systemStatus", tok=tok)


def main(bin_path, expect=None):
    tok = token()
    s = status(tok)
    print("before : version=%s uptime=%s max_alloc=%s" % (
        s["firmware_version"], s["uptime"], s["max_alloc_heap"]))

    if s["max_alloc_heap"] < MIN_MAX_ALLOC:
        print("heap too low for an upload (%s < %s) - rebooting first"
              % (s["max_alloc_heap"], MIN_MAX_ALLOC))
        try:
            call("/rest/restart", {}, tok=tok)
            print("  restart accepted")
        except Exception as e:
            print("  restart reply lost while it reset (%s) - normal" % type(e).__name__)
        time.sleep(20)                       # let it drop and rejoin
        for _ in range(30):
            time.sleep(5)
            try:
                tok = token()
                s = status(tok)
                if s["max_alloc_heap"] >= MIN_MAX_ALLOC:
                    print("  back: uptime=%s max_alloc=%s" % (s["uptime"], s["max_alloc_heap"]))
                    break
            except Exception:
                continue
        else:
            print("FAILED: heap did not recover")
            return 1
    else:
        print("heap is healthy, uploading without a reboot")

    print("uploading %s" % bin_path)
    proc = subprocess.run(
        ["timeout", "300", "/root/.pio-venv/bin/python",
         "/root/ossm-refactor/OSSM-SvelteKit/scripts/ota-upload.py",
         "--device", DEVICE, bin_path],
        capture_output=True, text=True)
    for line in (proc.stdout + proc.stderr).strip().splitlines()[-4:]:
        print("  | %s" % line)

    for _ in range(40):
        time.sleep(5)
        try:
            tok = token()
            s = status(tok)
            print("after  : version=%s uptime=%s free=%s max_alloc=%s reset=%s" % (
                s["firmware_version"], s["uptime"], s["free_heap"],
                s["max_alloc_heap"], s["cpu_reset_reason"]))
            if expect and s["firmware_version"] != expect:
                print("FAILED: expected version %s, board says %s"
                      % (expect, s["firmware_version"]))
                return 1
            print("OK: flash verified" if expect else "OK: board is back")
            return 0
        except Exception:
            continue
    print("FAILED: board did not answer after the upload")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None))
