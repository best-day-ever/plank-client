#!/usr/bin/env python3
"""Local mock of the PLANK broker public API for manual client testing.

Implements bde-linux docs/plank-broker.md section 10.1 closely enough to
exercise the client's Remote (broker) mode UI: sign-in challenge, generic
denial, HTTP 429 rate limiting, host list, connect lease, keepalive, logout.
It does NOT relay a stream or mint real GSSAPI tokens, so "Connect" stops at
the host admission step unless --host-* options point at a real test host.

Usage:
    scripts/mock-plank-broker.py [--port 29000] [--user anna]
                                 [--password secret] [--otp 123456]

It creates a throwaway self-signed ECDSA P-256 certificate (CN/SAN
localhost) in a temporary directory and prints its SPKI SHA-256 pin. In the
client: Settings > Remote Access > broker server "localhost", the port, and
that pin. Requires the `openssl` command line tool. TLS 1.3 only.
"""

import argparse
import base64
import hashlib
import http.server
import json
import os
import secrets
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.parse

HOSTS = [
    {"id": "ws01.example.test", "name": "ws01", "online": True, "in_use_by": None,
     "connectable": True, "reason": None},
    {"id": "ws02.example.test", "name": "ws02", "online": True, "in_use_by": "bob",
     "connectable": False, "reason": "in use"},
    {"id": "ws03.example.test", "name": "ws03", "online": False, "in_use_by": None,
     "connectable": False, "reason": "offline"},
]


def make_certificate(directory):
    key = os.path.join(directory, "broker.key")
    cert = os.path.join(directory, "broker.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
         "-nodes", "-keyout", key, "-out", cert, "-days", "2", "-subj", "/CN=localhost",
         "-addext", "subjectAltName=DNS:localhost"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    public_key = subprocess.run(["openssl", "x509", "-in", cert, "-pubkey", "-noout"],
                                check=True, capture_output=True).stdout
    spki_der = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "der"], input=public_key,
                              check=True, capture_output=True).stdout
    return cert, key, hashlib.sha256(spki_der).hexdigest()


class State:
    def __init__(self, args):
        self.args = args
        self.conversations = {}   # id -> (username, created)
        self.sessions = {}        # token -> username
        self.failures = {}        # (username, ip) -> count
        self.blocked_until = {}   # (username, ip) -> time


def make_handler(state):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "mock-plank-broker"

        def log_message(self, fmt, *args):
            sys.stderr.write("mock-broker: %s %s\n" % (self.address_string(), fmt % args))

        def reply(self, status, obj):
            body = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def body(self):
            length = int(self.headers.get("Content-Length") or 0)
            if length > 65536:
                return None
            try:
                return json.loads(self.rfile.read(length) or b"{}")
            except ValueError:
                return None

        def bearer(self):
            header = self.headers.get("Authorization", "")
            if not header.startswith("Bearer "):
                return None
            return state.sessions.get(header[len("Bearer "):])

        def do_GET(self):
            if self.path != "/v1/hosts":
                return self.reply(404, {"state": "denied"})
            if self.bearer() is None:
                return self.reply(401, {"state": "denied"})
            return self.reply(200, {"hosts": HOSTS})

        def do_POST(self):
            data = self.body()
            if data is None:
                return self.reply(400, {"state": "denied"})
            ip = self.client_address[0]
            path = self.path
            if path == "/v1/auth/start":
                username = data.get("username")
                if not isinstance(username, str) or not username:
                    return self.reply(200, {"state": "denied"})
                if state.blocked_until.get((username, ip), 0) > time.time():
                    retry = int(state.blocked_until[(username, ip)] - time.time()) + 1
                    return self.reply(429, {"state": "denied", "retry_after": retry})
                conversation = secrets.token_urlsafe(24)
                state.conversations[conversation] = (username, time.time())
                return self.reply(200, {
                    "state": "challenge", "conversation_id": conversation,
                    "prompts": [{"id": "password", "style": "secret", "text": "Password"},
                                {"id": "otp", "style": "otp", "text": "Authenticator code"}]})
            if path == "/v1/auth/respond":
                entry = state.conversations.pop(data.get("conversation_id"), None)
                responses = data.get("responses")
                if entry is None or time.time() - entry[1] > 120 or not isinstance(responses, list):
                    return self.reply(200, {"state": "denied"})
                username = entry[0]
                ok = (username == state.args.user and responses == [state.args.password, state.args.otp])
                if not ok:
                    key = (username, ip)
                    state.failures[key] = state.failures.get(key, 0) + 1
                    if state.failures[key] >= 3:
                        state.failures[key] = 0
                        state.blocked_until[key] = time.time() + state.args.block_seconds
                    return self.reply(200, {"state": "denied"})
                token = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip("=")
                state.sessions[token] = username
                return self.reply(200, {"state": "authenticated", "session_token": token,
                                        "expires_in": 36000, "username": username})
            if path == "/v1/logout":
                header = self.headers.get("Authorization", "")
                state.sessions.pop(header[len("Bearer "):], None)
                return self.reply(200, {"state": "logged_out"})
            parts = path.split("/")
            if len(parts) == 5 and parts[1:3] == ["v1", "hosts"] and parts[4] in ("connect", "keepalive"):
                username = self.bearer()
                if username is None:
                    return self.reply(401, {"state": "denied"})
                host_id = urllib.parse.unquote(parts[3])
                host = next((h for h in HOSTS if h["id"] == host_id), None)
                if host is None or not host["connectable"]:
                    return self.reply(403, {"state": "denied"})
                if parts[4] == "keepalive":
                    return self.reply(200, {"state": "ok"})
                endpoint = (self.headers.get("Host") or "localhost").rsplit(":", 1)[0]
                return self.reply(200, {
                    "endpoint": state.args.lease_endpoint or endpoint,
                    "port": state.args.lease_port,
                    "host_cert_sha256": state.args.host_cert_sha256,
                    "username": username,
                    "gssapi_token": base64.b64encode(b"mock-ap-req:" + secrets.token_bytes(16)).decode(),
                    "expires_in": 30})
            return self.reply(404, {"state": "denied"})

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=29000)
    parser.add_argument("--user", default="anna")
    parser.add_argument("--password", default="secret")
    parser.add_argument("--otp", default="123456")
    parser.add_argument("--block-seconds", type=int, default=60)
    parser.add_argument("--lease-endpoint", default=None,
                        help="endpoint returned by connect (default: the Host header name)")
    parser.add_argument("--lease-port", type=int, default=29042)
    parser.add_argument("--host-cert-sha256", default="0" * 64,
                        help="leaf SHA-256 of the test host certificate returned by connect")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="mock-plank-broker-") as directory:
        cert, key, pin = make_certificate(directory)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(cert, key)
        server = http.server.ThreadingHTTPServer((args.bind, args.port), make_handler(State(args)))
        server.socket = context.wrap_socket(server.socket, server_side=True)
        print("mock PLANK broker on https://%s:%d (TLS 1.3, ECDSA P-256)" % (args.bind, args.port))
        print("SPKI SHA-256 pin: %s" % pin)
        print("sign in as %r / %r / code %s" % (args.user, args.password, args.otp), flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
