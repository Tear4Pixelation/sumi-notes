#!/usr/bin/env python3
# Serve the web build (EmRelease/ by default) to other devices on the LAN, e.g. an iPad:
#   python3 wasm/serve.py [dir] [port]   then open http://<this machine's IP>:<port>/Kaku.html
# Plain http.server would do, except that Safari caches Kaku.wasm heuristically and keeps running
#  a stale build after a rebuild; no-cache makes it revalidate every load (a 304 when unchanged).
import http.server
import os
import socket
import sys

class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
  def end_headers(self):
    self.send_header("Cache-Control", "no-cache")
    super().end_headers()

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "EmRelease")
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8321
os.chdir(root)
# the address other devices reach us on: the interface holding the default route
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
  probe.connect(("192.0.2.1", 1))  # TEST-NET, nothing is sent
  lanAddr = probe.getsockname()[0]
except OSError:
  lanAddr = "localhost"
finally:
  probe.close()
print(f"Serving {os.getcwd()} at http://{lanAddr}:{port}/Kaku.html", flush=True)
http.server.ThreadingHTTPServer(("", port), NoCacheHandler).serve_forever()
