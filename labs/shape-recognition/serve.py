#!/usr/bin/env python3
"""Serve recorder.html to other devices on the network and save what they record into fixtures/.

    python3 serve.py [port]        # default 8000, then open http://<this-pc>:8000/recorder.html

GET serves files from this folder; POST /save writes the request body to fixtures/<name>.strokes.
Anyone on the network can reach it while it runs - stop it with Ctrl+C when done.
"""

import http.server
import os
import re
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")
MAX_BODY = 20 * 1024 * 1024


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=HERE, **kwargs)

    def do_POST(self):
        if not self.path.startswith("/save"):
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0 or length > MAX_BODY:
            self.send_error(413 if length > 0 else 400)
            return
        body = self.rfile.read(length)
        if not body.startswith(b"# shape-recognition strokes"):
            self.send_error(400, "not a .strokes file")
            return
        # the name comes from the client, so only a plain file name is accepted
        requested = self.path.partition("name=")[2]
        stem = re.sub(r"[^A-Za-z0-9_.-]", "", requested).removesuffix(".strokes").lstrip(".")[:80]
        stem = stem or time.strftime("recorded-%Y%m%d-%H%M%S")
        os.makedirs(FIXTURES, exist_ok=True)
        path = os.path.join(FIXTURES, stem + ".strokes")
        with open(path, "wb") as out:
            out.write(body)
        count = body.count(b"\nend\n")
        print(f"saved {count} strokes to {os.path.relpath(path, HERE)}")
        reply = f"fixtures/{stem}.strokes".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def local_ip():
    # the address other devices reach this PC on; no packet is actually sent
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        try:
            probe.connect(("192.168.0.1", 1))
            return probe.getsockname()[0]
        except OSError:
            return "localhost"


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"open http://{local_ip()}:{port}/recorder.html  (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
