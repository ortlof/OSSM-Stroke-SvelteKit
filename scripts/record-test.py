#!/usr/bin/env python3
"""
Exercise the upload path with NO MOTION.

Recording opens a TLS session and closes it. Samples are only captured while the
engine runs, so leaving run=false means the session is empty - but the code paths
this build changes are all hit: the handshake, the heap pre-flight, the retry
gate, the give-up latch. The carriage does not move, so a fault cannot drop the
load.

The ingest key is read from a file and never printed.
"""
import json
import time
import urllib.request

DEVICE = "192.168.2.115"
KEYFILE = "/root/.hermes/cache/scratch/ossm-ingest-key"


def call(path, data=None, token=None, timeout=20):
    req = urllib.request.Request(
        "http://%s%s" % (DEVICE, path),
        data=json.dumps(data).encode() if data else None,
        method="POST" if data else "GET")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def upstate(tok):
    try:
        s = call("/rest/strokeState", token=tok)
    except Exception as e:
        return {"_error": str(e)}
    return {k: s.get(k) for k in
            ("uploadState", "uploadError", "uploadSession", "uploadQueued",
             "uploadSent", "uploadDropped", "uploadStackFree", "homed", "run")}


key = open(KEYFILE).read().strip()
tok = call("/rest/signIn", {"username": "admin", "password": "admin"})["access_token"]
print("signed in")

# 1. Make sure the uploader is armed with the key. The HTTP reader hides the key
#    from GETs, so this is write-only from here.
print("settings before:", {k: v for k, v in call("/rest/sessionUpload", token=tok).items() if k != "key"})
call("/rest/sessionUpload", {"enabled": True, "key": key}, token=tok)
print("settings after :", {k: v for k, v in call("/rest/sessionUpload", token=tok).items() if k != "key"})

print("heap before    :", {k: call("/rest/systemStatus", token=tok)[k]
                           for k in ("free_heap", "max_alloc_heap")})

# 2. Press Record. No run=true, so nothing moves.
print("\n--- pressing Record (no motion) ---")
call("/rest/strokeState", {"record": True}, token=tok)
t0 = time.time()
opened = False
while time.time() - t0 < 14:
    st = upstate(tok)
    print("  t+%2ds state=%-10s session=%-4s err=%s"
          % (int(time.time() - t0), st.get("uploadState"), st.get("uploadSession"),
             (st.get("uploadError") or "")[:70]))
    if st.get("uploadState") == "recording":
        opened = True
    time.sleep(3)

# 3. Release Record: flush, then close the session.
print("--- releasing Record ---")
call("/rest/strokeState", {"record": False}, token=tok)
t0 = time.time()
while time.time() - t0 < 15:
    st = upstate(tok)
    print("  t+%2ds state=%-10s sent=%s dropped=%s stack_free=%s err=%s"
          % (int(time.time() - t0), st.get("uploadState"), st.get("uploadSent"),
             st.get("uploadDropped"), st.get("uploadStackFree"),
             (st.get("uploadError") or "")[:70]))
    time.sleep(3)

print("\nheap after     :", {k: call("/rest/systemStatus", token=tok)[k]
                            for k in ("free_heap", "max_alloc_heap", "uptime")})
print("RESULT: session opened = %s" % opened)
