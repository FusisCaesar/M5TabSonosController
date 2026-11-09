"""Sign this PC into Spotify once and hand the token to the Tab5.

Create a free app at https://developer.spotify.com/dashboard while logged into
the Premium account that owns it. In the app settings add this redirect URI:

    http://127.0.0.1:8888/callback

Leave the client secret empty if Spotify lets you. If the dashboard shows one,
pass it with --client-secret. It is stored on the Tab5, not in this repo.

    python tools/spotify_auth.py --client-id YOUR_ID --port COM12
"""

import argparse
import base64
import hashlib
import json
import secrets
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer

sys.path.insert(0, "tools")
from tab5serial import open_port

REDIRECT = "http://127.0.0.1:8888/callback"
SCOPES = "playlist-read-private playlist-read-collaborative user-read-email"


def pkce():
    verifier = secrets.token_urlsafe(64)
    digest = hashlib.sha256(verifier.encode("ascii")).digest()
    challenge = base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")
    return verifier, challenge


class Once(BaseHTTPRequestHandler):
    code = ""

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        Once.code = urllib.parse.parse_qs(parsed.query).get("code", [""])[0]
        body = b"Signed in. You can close this tab."
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        return


def main():
    parser = argparse.ArgumentParser(description="Sign the Tab5 into Spotify")
    parser.add_argument("--client-id", required=True)
    parser.add_argument("--client-secret", default="")
    parser.add_argument("--port", default="COM12")
    args = parser.parse_args()

    verifier, challenge = pkce()
    state = secrets.token_urlsafe(12)
    query = urllib.parse.urlencode(
        {
            "client_id": args.client_id,
            "response_type": "code",
            "redirect_uri": REDIRECT,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
            "scope": SCOPES,
            "state": state,
        }
    )
    server = HTTPServer(("127.0.0.1", 8888), Once)
    thread = threading.Thread(target=server.handle_request, daemon=True)
    thread.start()
    webbrowser.open("https://accounts.spotify.com/authorize?" + query)
    print("Waiting for Spotify in the browser...")
    thread.join(timeout=180)
    server.server_close()
    if not Once.code:
        sys.exit("No code came back. Check the redirect URI in the Spotify dashboard.")

    form = urllib.parse.urlencode(
        {
            "grant_type": "authorization_code",
            "code": Once.code,
            "redirect_uri": REDIRECT,
            "client_id": args.client_id,
            "code_verifier": verifier,
        }
    ).encode("utf-8")
    headers = {"Content-Type": "application/x-www-form-urlencoded"}
    if args.client_secret:
        basic = base64.b64encode(f"{args.client_id}:{args.client_secret}".encode()).decode()
        headers["Authorization"] = "Basic " + basic
    request = urllib.request.Request("https://accounts.spotify.com/api/token", data=form, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = response.read().decode("utf-8")
    except urllib.error.HTTPError as err:
        sys.exit(f"Token exchange failed ({err.code}): {err.read().decode('utf-8', 'replace')}")
    refresh = json.loads(payload).get("refresh_token", "")
    if not refresh:
        sys.exit("Spotify did not return a refresh token.")

    line = f"S spotify|{args.client_id}|{refresh}|{args.client_secret}\n"
    ser = open_port(args.port, timeout=1.0)
    ser.write(line.encode("utf-8"))
    ser.close()
    print(f"Sent the Spotify sign-in to {args.port}.")


if __name__ == "__main__":
    main()
