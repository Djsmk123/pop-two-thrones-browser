#!/usr/bin/env python3
"""Local server for the browser build (this machine only).

Threads in WebAssembly need SharedArrayBuffer, which browsers only enable on cross-origin isolated
pages, so every response carries COOP/COEP headers.

  /            web/ (the page)
  /build/      build_web/www (recomp.js / .wasm from build_web.sh)
  /game/       only with --game DIR: your own install folder, so the page can copy it without the file dialog
               (local development; the loader looks for /game/_list.json)

The page never downloads game files: the player picks their own game folder, which the browser copies
into its private storage.

--lan serves other devices on the same wifi. Browsers only enable WebGPU and SharedArrayBuffer on secure
pages (https, or localhost), so --lan uses https with a throwaway self-signed certificate: the other device
shows a warning once, accept it to continue. With --game, your game files are then readable by anyone on
that network while the server runs.

Usage: python3 web/serve.py [--port 8007] [--game <your game folder>] [--lan]
"""
import argparse
import http.server
import json
import os
import socket
import ssl
import subprocess
import tempfile
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(os.path.dirname(HERE), "build_web", "www")


GAME = None


def game_list():
    out = []
    for root, dirs, files in os.walk(GAME):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for f in files:
            if f.startswith(".") or f == "PrinceSM":
                continue
            full = os.path.join(root, f)
            out.append({"path": os.path.relpath(full, GAME).replace(os.sep, "/"), "size": os.path.getsize(full)})
    return out


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm",
                      ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if GAME and urllib.parse.urlparse(self.path).path == "/game/_list.json":
            body = json.dumps(game_list()).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlparse(path).path)
        if GAME and p.startswith("/game/"):
            return os.path.join(GAME, p[len("/game/"):])
        if p.startswith("/build/"):
            return os.path.join(BUILD, p[len("/build/"):])
        return os.path.join(HERE, p.lstrip("/"))


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))   # no packet is sent; this picks the interface with a route
        return s.getsockname()[0]
    finally:
        s.close()


def self_signed(ip):
    d = tempfile.mkdtemp(prefix="rc-cert-")
    key, crt = os.path.join(d, "key.pem"), os.path.join(d, "cert.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "30", "-keyout", key,
                    "-out", crt, "-subj", "/CN=" + ip, "-addext", f"subjectAltName=IP:{ip},DNS:localhost,IP:127.0.0.1"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return crt, key


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8007)
    ap.add_argument("--game", help="your game folder, served at /game/ for local development")
    ap.add_argument("--lan", action="store_true", help="listen on the local network (https, self-signed)")
    a = ap.parse_args()
    global GAME
    GAME = os.path.abspath(a.game) if a.game else None
    if a.lan:
        ip = lan_ip()
        srv = http.server.ThreadingHTTPServer(("0.0.0.0", a.port), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(*self_signed(ip))
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
        print(f"On the other device (same wifi), open  https://{ip}:{a.port}/  and accept the certificate warning.")
        if GAME:
            print("Warning: your game files are served to this network while this runs.")
    else:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
        print(f"http://localhost:{a.port}/")
    srv.serve_forever()


if __name__ == "__main__":
    main()
