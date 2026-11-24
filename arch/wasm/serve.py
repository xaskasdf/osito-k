#!/usr/bin/env python3
"""
serve.py — local dev server for OsitoK WASM that adds the headers needed
to enable SharedArrayBuffer (Cross-Origin-Opener-Policy +
Cross-Origin-Embedder-Policy). Without these the worker can't use
Atomics.wait, which the oi_chat sync bridge depends on.

Defaults to serving the current directory on port 8000.

Usage:
    cd arch/wasm/build && python3 ../serve.py [port]
"""
import sys
import http.server
import socketserver


class COIHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        # `credentialless` is more lenient than `require-corp`: cross-origin
        # fetches without credentials (our R2 toolchain fetches) don't need
        # a `Cross-Origin-Resource-Policy` header on the response. Same
        # cross-origin-isolation guarantees → SharedArrayBuffer is available.
        self.send_header("Cross-Origin-Embedder-Policy", "credentialless")
        super().end_headers()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    with socketserver.ThreadingTCPServer(("", port), COIHandler) as httpd:
        httpd.allow_reuse_address = True
        print(f"Serving with COOP/COEP on http://localhost:{port}/")
        print("SharedArrayBuffer should be available; check window.crossOriginIsolated.")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down.")


if __name__ == "__main__":
    main()
