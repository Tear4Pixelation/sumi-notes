#!/usr/bin/env python3
"""Live playground: draw in playground.html and see what the recognizer makes of each stroke the moment
the pen lifts.

    python3 playground.py [port]     # default 8001, then open the printed address (works from an iPad)

Builds recognize_cli from the current sources whenever they are newer than the binary, then keeps one
copy running and feeds it strokes, so there is no compile or process start per stroke.  Edit
shaperec.cpp and restart this script to try a change.  Anyone on the network can reach it while it runs.
"""

import glob
import http.server
import json
import os
import socket
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
CLI = os.path.join(HERE, "recognize_cli")
SOURCES = ["recognize_cli.cpp", "shaperec.cpp"]


def build():
    inputs = [os.path.join(HERE, name) for name in SOURCES] + glob.glob(os.path.join(HERE, "*.h"))
    if os.path.exists(CLI) and os.path.getmtime(CLI) >= max(os.path.getmtime(path) for path in inputs):
        return
    print("building recognize_cli ...")
    subprocess.run(["g++", "-O2", "-std=c++17", "-o", CLI] + SOURCES, cwd=HERE, check=True)


class Recognizer:
    def __init__(self):
        self.proc = subprocess.Popen([CLI], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.lock = threading.Lock()

    def recognize(self, points):
        line = " ".join(f"{value:.2f}" for value in points) + "\n"
        with self.lock:
            self.proc.stdin.write(line)
            self.proc.stdin.flush()
            return self.proc.stdout.readline()


class Handler(http.server.SimpleHTTPRequestHandler):
    recognizer = None

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HERE, **kwargs)

    def log_message(self, fmt, *args):
        pass  # one line per stroke would bury the startup address

    def do_GET(self):
        if self.path in ("/", ""):
            self.path = "/playground.html"
        super().do_GET()

    def do_POST(self):
        if self.path != "/recognize":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0 or length > 5 * 1024 * 1024:
            self.send_error(400)
            return
        try:
            points = [float(value) for value in json.loads(self.rfile.read(length))["points"]]
        except (ValueError, KeyError, TypeError):
            self.send_error(400, "expected {\"points\": [x, y, x, y, ...]}")
            return
        reply = self.recognizer.recognize(points).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def local_ip():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(("192.168.0.1", 1))
            return probe.getsockname()[0]
        except OSError:
            return "localhost"


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8001
    build()
    Handler.recognizer = Recognizer()
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    # by address, not "localhost": that tries IPv6 first and costs ~100 ms per stroke against this
    #  IPv4-only server
    print(f"open http://{local_ip()}:{port}/  (or http://127.0.0.1:{port}/ on this PC; Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
