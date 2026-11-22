"""Serve the built SvelteKit front end locally and proxy /rest to the trainer.

This is the fast loop for web interface work: a layout change costs `npm run build` and a
reload instead of a firmware build and an OTA flash. It signs in on the device itself and adds
the token to every proxied call, so it works against the normal, secured firmware - there is
no need to flash the open image just to look at a page.

    cd interface && npm run build
    python3 scripts/ui-preview.py --device http://192.168.2.248 --user admin --password admin
    # then open http://127.0.0.1:8095/ and use a phone viewport in the browser

The credentials come from the command line and are never stored. Without them, only the
device's open image (FT_SECURITY=0) answers the proxied calls.
"""

import argparse
import http.server
import json
import os
import pathlib
import socketserver
import urllib.error
import urllib.request

ROOT = str(pathlib.Path(__file__).resolve().parent.parent / "interface" / "build")

DEVICE = "http://192.168.2.248"
TOKEN = None
USER = None
PASSWORD = None


def device_request(path, method="GET", data=None, content_type=None, token=None, timeout=25):
    request = urllib.request.Request(DEVICE + path, data=data, method=method)
    if content_type:
        request.add_header("Content-Type", content_type)
    if token:
        request.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read(), response.headers.get("Content-Type", "application/json")
    except urllib.error.HTTPError as error:
        return error.code, error.read(), error.headers.get("Content-Type", "application/json")
    except Exception as error:
        return 502, json.dumps({"error": str(error)}).encode(), "application/json"


def sign_in():
    """A token for the device, or None when it has no login (the open debug image)."""
    global TOKEN
    status, body, _ = device_request("/rest/features", timeout=15)
    try:
        features = json.loads(body.decode())
    except Exception:
        features = {}
    if features.get("security") is False or not USER:
        print("device answers without a login")
        return None
    status, body, _ = device_request(
        "/rest/signIn", method="POST",
        data=json.dumps({"username": USER, "password": PASSWORD}).encode(),
        content_type="application/json")
    if status == 200:
        TOKEN = json.loads(body.decode()).get("access_token")
        print("signed in to", DEVICE)
    else:
        print("sign in failed:", status)
    return TOKEN


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def proxy(self, method):
        global TOKEN
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        content_type = self.headers.get("Content-Type")
        status, payload, returned_type = device_request(self.path, method, body, content_type, TOKEN)
        if status == 401:  # tokens expire: take a new one and try the call once more
            if sign_in():
                status, payload, returned_type = device_request(self.path, method, body, content_type, TOKEN)
        # The front end decides from /rest/features whether to ask for a login. This proxy
        # signs in on its behalf, so the browser is told there is no login: otherwise it shows
        # the password form and nothing behind it can be looked at.
        if status == 200 and self.path.split("?")[0] == "/rest/features" and TOKEN:
            try:
                features = json.loads(payload.decode())
                features["security"] = False
                payload = json.dumps(features).encode()
            except Exception:
                pass
        self.send_response(status)
        self.send_header("Content-Type", returned_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path.startswith("/rest/") or self.path.startswith("/_app/env.js"):
            return self.proxy("GET")
        path = self.translate_path(self.path)
        if not os.path.exists(path) or os.path.isdir(path):
            self.path = "/index.html"
        return super().do_GET()

    def do_POST(self):
        if self.path.startswith("/rest/"):
            return self.proxy("POST")
        self.send_error(404)

    def log_message(self, *args):
        pass


def main():
    global DEVICE, USER, PASSWORD
    parser = argparse.ArgumentParser(description="Serve the front end and proxy /rest to the trainer")
    parser.add_argument("--device", default=DEVICE)
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password", default="")
    parser.add_argument("--port", type=int, default=8095)
    args = parser.parse_args()

    DEVICE = args.device.rstrip("/")
    USER = args.user
    PASSWORD = args.password
    if not os.path.isdir(ROOT):
        raise SystemExit(f"no build at {ROOT}: run npm run build in interface/ first")
    # NOT a readiness line: this runs before the port is bound, so it prints even when
    # the bind then fails with "Address already in use". The line that means the server
    # is up is the one inside the `with` below, after bind(). Keep the word "serving"
    # out of this one so nothing can watch for it and be fooled.
    print(f"device {DEVICE}, front end at {ROOT}, port {args.port}")
    sign_in()

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(("127.0.0.1", args.port), Handler) as httpd:
        print(f"serving {ROOT} on http://127.0.0.1:{args.port}")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
