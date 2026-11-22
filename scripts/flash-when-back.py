#!/usr/bin/env python3
"""
Flash the fix as soon as the board answers again.

Why this exists: the running image contains the defect that kills the core-0
watchdog, so when its heap runs out the HTTP server wedges and nothing in the
firmware can reset it out of that state - only a power cycle. Once Basti power
cycles it, the heap is healthy for the first minute or two, and that is the only
reliable window for an OTA upload over this board's weak radio.

So: poll cheaply, flash on the first sign of life, then read the board back.
"""
import json
import subprocess
import sys
import time
import urllib.request

DEVICE = "192.168.2.115"
BIN = "/root/ossm-refactor/OSSM-SvelteKit/build/release/OSSM-Stroke_esp32dev_0-7-0.bin"
LOG = "/root/.hermes/cache/scratch/ossm-flash-watch.log"
DEADLINE_S = 45 * 60


def log(msg):
    line = "%s  %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(LOG, "a") as fh:
        fh.write(line + "\n")


def get(path, timeout=6):
    with urllib.request.urlopen("http://%s%s" % (DEVICE, path), timeout=timeout) as r:
        return json.loads(r.read().decode())


def alive():
    try:
        get("/rest/features")
        return True
    except Exception:
        return False


def main():
    log("watcher started; waiting for the board to answer (power cycle it now)")
    started = time.time()
    while time.time() - started < DEADLINE_S:
        if alive():
            log("board is UP - flashing immediately")
            break
        time.sleep(5)
    else:
        log("gave up: the board did not answer within %d min" % (DEADLINE_S // 60))
        return 2

    # Flash. This is the one window where the heap can still fit the upload.
    proc = subprocess.run(
        ["timeout", "300", "/root/.pio-venv/bin/python",
         "/root/ossm-refactor/OSSM-SvelteKit/scripts/ota-upload.py",
         "--device", DEVICE, BIN],
        capture_output=True, text=True)
    log("ota-upload exit=%d" % proc.returncode)
    for line in (proc.stdout + proc.stderr).strip().splitlines()[-3:]:
        log("  | %s" % line)

    # Wait for it to come back and read what it says about itself.
    log("waiting for the board to restart")
    for _ in range(40):
        time.sleep(5)
        try:
            s = get("/rest/systemStatus", timeout=8)
            log("  firmware=%s heap_free=%s max_alloc=%s reset=%s" % (
                s.get("app_version"), s.get("free_heap"),
                s.get("max_alloc_heap"), s.get("cpu_reset_reason")))
            log("RESULT: flashed and alive - check the serial console for the "
                "'task_wdt: task not found' flood, which must now be ABSENT")
            return 0
        except Exception:
            continue
    log("flashed, but the board has not answered since. Check the serial console.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
