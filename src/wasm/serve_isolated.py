#!/usr/bin/env python3
# Serve a web build locally with the headers a pthreads build needs.
#
# The page only gets SharedArrayBuffer (and so threads) when it is
# cross-origin isolated, which takes COOP and COEP headers that
# `python3 -m http.server` cannot send.
#
# Usage:
#   python3 src/wasm/serve_isolated.py <build-dir> [port]
#   then open http://localhost:8000/winbolo.html
#
# Port defaults to 8000. Every response also carries Cache-Control: no-cache
# so a rebuilt winbolo.js / .wasm / .data is always fetched again.

import functools
import http.server
import sys


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()


def main():
    if len(sys.argv) < 2 or len(sys.argv) > 3:
        print("usage: serve_isolated.py <build-dir> [port]", file=sys.stderr)
        sys.exit(2)
    directory = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) == 3 else 8000

    handler = functools.partial(IsolatedHandler, directory=directory)
    server = http.server.ThreadingHTTPServer(("", port), handler)
    print(f"Serving {directory} at http://localhost:{port}/winbolo.html")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
