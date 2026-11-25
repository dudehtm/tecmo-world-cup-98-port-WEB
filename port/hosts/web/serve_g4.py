#!/usr/bin/env python3
"""Minimal local HTTP server for TWC98BO G4 Browser Host (127.0.0.1 only)."""
from __future__ import annotations

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class G4HttpHandler(SimpleHTTPRequestHandler):
    extensions_map = {
        **SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript; charset=utf-8",
        ".mjs": "text/javascript; charset=utf-8",
        ".html": "text/html; charset=utf-8",
        ".css": "text/css; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".bin": "application/octet-stream",
        ".cp": "application/octet-stream",
        ".enc": "application/octet-stream",
    }

    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, format: str, *args) -> None:
        pass


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18980)
    parser.add_argument("--dir", type=str, default=None, help="Directory to serve (e.g. dist/web)")
    args = parser.parse_args()
    default_port_root = Path(__file__).resolve().parents[2]
    root_dir = Path(args.dir).resolve() if args.dir else default_port_root
    handler = partial(G4HttpHandler, directory=str(root_dir))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    entry = "index.html" if (root_dir / "index.html").is_file() else "hosts/web/index.html"
    print(f"TWC98BO Web HTTP server ({root_dir}) -> http://127.0.0.1:{args.port}/{entry}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
