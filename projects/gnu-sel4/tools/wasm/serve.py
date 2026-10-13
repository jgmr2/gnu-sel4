#!/usr/bin/env python3
# serve.py <site-dir> [port]: serves the browser demo with the headers SharedArrayBuffer needs
# SPDX-License-Identifier: BSD-2-Clause
import http.server, sys, functools
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "credentialless")
        super().end_headers()
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8099
http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=sys.argv[1])).serve_forever()
