#!/usr/bin/env python3
"""Read the board after the webflash: is it up, which build, what heap, what settings."""
import json
import urllib.request

DEVICE = "192.168.2.115"


def call(path, data=None, token=None, timeout=15):
    req = urllib.request.Request(
        "http://%s%s" % (DEVICE, path),
        data=json.dumps(data).encode() if data else None,
        method="POST" if data else "GET")
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode()
    try:
        return json.loads(raw)
    except Exception:
        return raw


tok = call("/rest/signIn", {"username": "admin", "password": "admin"})["access_token"]
print("  signed in: ok")

s = call("/rest/systemStatus", token=tok)
print("  --- systemStatus (identity + memory) ---")
for k in sorted(s):
    v = s[k]
    if any(t in k.lower() for t in ("sha", "version", "name", "heap", "reset", "firmware", "build", "uptime")):
        print("    %-24s %s" % (k, v))

u = call("/rest/sessionUpload", token=tok)
print("  --- sessionUpload settings (values only, no secrets) ---")
for k in sorted(u):
    v = u[k]
    if "key" in k.lower():
        v = "set (%d chars)" % len(v) if v else "NOT SET"
    print("    %-16s %s" % (k, v))

st = call("/rest/strokeState", token=tok)
print("  --- upload state as the UI sees it ---")
for k in ("uploadState", "uploadError", "uploadSession", "uploadQueued", "uploadSent", "uploadDropped", "uploadStackFree"):
    if k in st:
        print("    %-16s %s" % (k, st[k]))
