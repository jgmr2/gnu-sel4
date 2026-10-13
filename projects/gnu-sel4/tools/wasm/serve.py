#!/usr/bin/env python3
# serve.py <site-dir> [port]: serves the browser demo with the headers SharedArrayBuffer needs.
# A file with a .gz beside it (make-site.sh compresses the packed images) goes out compressed,
# and the browser undoes it (Content-Encoding: gzip).
# SERVE_HOST: the address to listen on (127.0.0.1). SERVE_CERT and SERVE_KEY: HTTPS, which other
# devices need (SharedArrayBuffer only in a secure context: https, or localhost).
# SPDX-License-Identifier: BSD-2-Clause
import http.server, sys, functools, os
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "credentialless")
        super().end_headers()
    def send_head(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path + ".gz"):
            return super().send_head()
        f = open(path + ".gz", "rb")
        self.send_response(200)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(os.fstat(f.fileno()).st_size))
        self.end_headers()
        return f
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8099
srv = http.server.ThreadingHTTPServer((os.environ.get("SERVE_HOST", "127.0.0.1"), port), functools.partial(H, directory=sys.argv[1]))
if os.environ.get("SERVE_CERT"):
    import ssl
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.environ["SERVE_CERT"], os.environ["SERVE_KEY"])
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
