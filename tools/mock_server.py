#!/usr/bin/env python3
"""
Local mock of the MatrixBoard web server, for testing the control page
without the hardware.

It pulls INDEX_HTML straight out of src/main.cpp (so you always test the
exact page the firmware serves) and emulates the firmware's endpoints:

  GET  /            -> control page
  GET  /api/state   -> {"msg":..,"bri":..,"spd":..,"mode":..}
  POST /set         -> same fields/clamping/sanitising as handleSet()

Each update prints a rough 32x8 preview of what the matrix would show.

Usage:
  python3 tools/mock_server.py            # http://localhost:8080
  python3 tools/mock_server.py --port 9000
"""

import argparse
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

ROOT = Path(__file__).resolve().parent.parent
MAIN_CPP = ROOT / "src" / "main.cpp"

MSG_MAX_LEN = 160
SPEED_MIN, SPEED_MAX = 1, 10
MODE_SCROLL, MODE_STATIC = 0, 1

state = {
    "msg": 'Join Wi-Fi "MatrixBoard" to edit me!',
    "bri": 4,
    "spd": 5,
    "mode": MODE_SCROLL,
}


def load_html() -> bytes:
    """Extract the PROGMEM raw string literal from main.cpp."""
    src = MAIN_CPP.read_text(encoding="utf-8")
    m = re.search(r'INDEX_HTML\[\]\s+PROGMEM\s*=\s*R"rawliteral\((.*?)\)rawliteral"',
                  src, re.S)
    if not m:
        raise SystemExit(f"Could not find INDEX_HTML in {MAIN_CPP}")
    return m.group(1).encode("utf-8")


def sanitize(text: str) -> str:
    """Mirror of sanitizeMessage() in main.cpp."""
    out = []
    for b in text.encode("utf-8"):
        if len(out) >= MSG_MAX_LEN:
            break
        if 0x20 <= b <= 0x7E:
            out.append(chr(b))
        elif b in (0x0A, 0x0D, 0x09):
            if out and out[-1] != " ":
                out.append(" ")
        elif b >= 0xC0:          # UTF-8 lead byte -> one '?'
            out.append("?")
    return "".join(out)


def to_int(v: str, default: int) -> int:
    """Arduino String::toInt() semantics: leading integer, else 0."""
    m = re.match(r"\s*([-+]?\d+)", v)
    return int(m.group(1)) if m else default


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def preview():
    frame_ms = 160 - state["spd"] * 14
    mode = "STATIC" if state["mode"] == MODE_STATIC else f"SCROLL {frame_ms}ms/frame"
    bar = "#" * (state["bri"] + 1) + "." * (15 - state["bri"])
    text = state["msg"]
    if state["mode"] == MODE_STATIC:
        # ~6 px per glyph on a 32 px display -> roughly 5-6 characters fit
        shown = text if len(text) <= 5 else text[:5] + "…(clipped)"
        text = shown.center(5)
    print(f"  ┌{'─' * 34}┐")
    print(f"  │ {text[:32]:<32} │  {mode}")
    print(f"  └{'─' * 34}┘  bri {state['bri']:>2} [{bar}]")


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body=b"", ctype="text/plain", headers=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _state_json(self):
        self._send(200, json.dumps(state, separators=(",", ":")).encode(),
                   "application/json")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            # Re-read on every request so edits to main.cpp show up on refresh.
            self._send(200, load_html(), "text/html; charset=utf-8")
        elif path == "/api/state":
            self._state_json()
        else:
            # Firmware redirects to http://192.168.4.1/; locally go to "/".
            self._send(302, headers={"Location": "/"})

    def do_POST(self):
        if self.path.split("?", 1)[0] != "/set":
            self._send(302, headers={"Location": "/"})
            return
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8", "replace")
        ctype = self.headers.get("Content-Type", "")
        if "multipart/form-data" in ctype:
            self._send(400, b"Expected urlencoded body (firmware would also ignore this)")
            return
        args = {k: v[0] for k, v in parse_qs(raw, keep_blank_values=True).items()}

        if "msg" in args:
            state["msg"] = sanitize(args["msg"])
        if "mode" in args:
            state["mode"] = MODE_STATIC if to_int(args["mode"], 0) == MODE_STATIC else MODE_SCROLL
        if "bri" in args:
            state["bri"] = clamp(to_int(args["bri"], 0), 0, 15)
        if "spd" in args:
            state["spd"] = clamp(to_int(args["spd"], 0), SPEED_MIN, SPEED_MAX)

        print(f"[web] POST /set {sorted(k for k in args if k != 'js')}")
        preview()

        if "js" in args:
            self._state_json()
        else:
            self._send(303, headers={"Location": "/"})

    def log_message(self, fmt, *a):  # quieter default logging
        if not self.path.startswith("/api/state"):
            print(f"[http] {self.command} {self.path} -> {a[1] if len(a) > 1 else ''}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # show logs live even when piped
    load_html()  # fail fast if the page can't be extracted
    print(f"MatrixBoard mock serving {MAIN_CPP.relative_to(ROOT)} page at "
          f"http://localhost:{a.port}/  (Ctrl+C to stop)")
    preview()
    ThreadingHTTPServer((a.host, a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
