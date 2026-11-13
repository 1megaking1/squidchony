#!/usr/bin/env python3
"""
A stand-in for the chrono's web API, for working on the phone page without the hardware.

    python3 tools/mock_chrono.py [port]        then open http://localhost:8000/?live=1

It serves web/chrono-remote.html and answers /api/state, /api/data, /api/cmd, /api/set,
/api/export.csv and /api/lastshot.wav with the same fields the firmware uses (see
firmware/squidchrony/web.cpp). While "measuring" is on it makes up a shot every two seconds.
Standard library only.
"""
import io
import json
import math
import pathlib
import random
import struct
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "web" / "chrono-remote.html"
FPS = 3.28084

lock = threading.Lock()
state = {
    "rev": 1, "srev": 1, "next_id": 3, "armed_until": 0.0, "lost": 0, "boot_t": time.time(),
    "settings": {"proj": "paintball", "sens": "normal", "units": "fps", "reading": "muzzle",
                 "mount": "downrange", "dist_ft": 3.0, "offset_mm": 40.0, "ahead_mm": 20.0,
                 "auto": False, "wifi": True, "cal": 1.0, "batt_cal": 1.0, "k_paintball": 0.022,
                 "k_dart": 0.098, "backlight_s": 120, "wifi_locked": False},
    "strings": [],
    "deleted": [],
}


def make_shot(mps, t_ms):
    k = 0.022 if state["settings"]["proj"] == "paintball" else 0.098
    d = state["settings"]["dist_ft"] * 0.3048
    return {"muzzle": mps, "chrono": mps * math.exp(-k * d), "snr": random.uniform(22, 38),
            "flags": 0, "t": t_ms, "sigma": 0.03, "time": int(time.time()), "deleted": False}


def seed():
    now = time.time()
    s1 = {"id": 1, "name": "Marker A, before tune", "proj": "paintball", "mount": "downrange",
          "created": int(now - 3600), "shots": [make_shot(random.gauss(85.0, 0.8), i * 7000) for i in range(8)]}
    s2 = {"id": 2, "name": "Marker A", "proj": "paintball", "mount": "downrange",
          "created": int(now - 60), "shots": []}
    t = 0
    for i in range(6):
        s2["shots"].append(make_shot(random.gauss(86.5, 0.4), t))
        t += 85 if i >= 2 else 5000
    state["strings"] = [s1, s2]


def bump(structure=False):
    state["rev"] += 1
    if structure:
        state["srev"] += 1


def current():
    return state["strings"][-1]


def live_shots(s):
    return [x for x in s["shots"] if not x["deleted"]]


def string_json(s):
    shots = []
    t0 = None
    for x in live_shots(s):
        t0 = x["t"] if t0 is None else t0
        shots.append([round(x["muzzle"] * FPS, 2), round(x["chrono"] * FPS, 2), round(x["snr"], 1),
                      x["flags"], x["t"] - t0, round(x["sigma"] * FPS, 2), x["time"]])
    return {"id": s["id"], "name": s["name"], "proj": s["proj"], "mount": s["mount"],
            "created": s["created"], "shots": shots}


def shooter():
    while True:
        time.sleep(2.0)
        with lock:
            armed = state["settings"]["auto"] or time.time() < state["armed_until"]
            if armed:
                if random.random() < 0.1:
                    state["lost"] += 1
                    continue
                t = int((time.time() - state["boot_t"]) * 1000)
                current()["shots"].append(make_shot(random.gauss(86.5, 0.5), t))
                bump()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        self.do_GET()

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query, keep_blank_values=True).items()}
        with lock:
            if url.path == "/":
                return self.send(200, PAGE.read_bytes(), "text/html; charset=utf-8")
            if url.path == "/api/state":
                cur = current()
                armed = state["settings"]["auto"] or time.time() < state["armed_until"]
                return self.send(200, json.dumps({
                    "rev": state["rev"], "srev": state["srev"], "fw": "mock", "ssid": "CHRONO-M0CK",
                    "armed": armed, "held": False, "web_armed": time.time() < state["armed_until"],
                    "busy": False, "clients": 1, "time_set": True, "batt_v": 7.7, "batt_pct": 62,
                    "batt_low": False,
                    "dsp": {"noise_db": 11.8, "peak_db": 3.1, "peak_hz": 0, "frames": 1000, "shots": 0,
                            "lost": state["lost"], "rejected": 0, "gaps": 0, "dropped": 0, "lag": 0},
                    "cur": {"id": cur["id"], "name": cur["name"], "proj": cur["proj"], "mount": cur["mount"],
                            "n": len(live_shots(cur))},
                    "settings": state["settings"]}))
            if url.path == "/api/data":
                strings = [current()] if q.get("cur") == "1" else list(reversed(state["strings"]))
                return self.send(200, json.dumps({"rev": state["rev"], "srev": state["srev"],
                                                  "cur": current()["id"], "strings": [string_json(s) for s in strings]}))
            if url.path == "/api/cmd":
                return self.send(200, json.dumps(self.cmd(q)))
            if url.path == "/api/set":
                bad = []
                for k, v in q.items():
                    if k not in state["settings"]:
                        bad.append(k)
                        continue
                    old = state["settings"][k]
                    state["settings"][k] = (v in ("1", "true")) if isinstance(old, bool) else (
                        float(v) if isinstance(old, float) else v)
                    if k in ("proj", "mount") and live_shots(current()):
                        self.new_string()
                    elif k in ("proj", "mount"):
                        current()[k] = v
                        bump(True)
                bump()
                return self.send(200, json.dumps({"ok": not bad, "bad": bad, "settings": state["settings"]}))
            if url.path == "/api/export.csv":
                rows = ["string,projectile,mount,shot,muzzle_fps,at_chrono_fps,sigma_fps,signal_db,flags,time"]
                for s in state["strings"]:
                    for i, x in enumerate(live_shots(s)):
                        rows.append(f'{s["name"]},{s["proj"]},{s["mount"]},{i + 1},{x["muzzle"] * FPS:.2f},'
                                    f'{x["chrono"] * FPS:.2f},{x["sigma"] * FPS:.2f},{x["snr"]:.1f},{x["flags"]},')
                return self.send(200, "\n".join(rows) + "\n", "text/csv")
            if url.path == "/api/lastshot.wav":
                return self.send(200, fake_wav(), "audio/wav")
        self.send(302, "", "text/plain", {"Location": "/"})

    def new_string(self):
        sid = state["next_id"]
        state["next_id"] += 1
        state["strings"].append({"id": sid, "name": f"String {sid}", "proj": state["settings"]["proj"],
                                 "mount": state["settings"]["mount"], "created": int(time.time()), "shots": []})
        bump(True)

    def cmd(self, q):
        c, v = q.get("c", ""), q.get("v", "")
        cur = current()
        if c == "new":
            self.new_string()
        elif c == "del":
            live = live_shots(cur)
            if not live:
                return {"ok": False, "msg": "No shots to delete."}
            live[-1]["deleted"] = True
            state["deleted"].append(live[-1])
            bump()
        elif c == "undel":
            if not state["deleted"]:
                return {"ok": False, "msg": "Nothing to undo."}
            state["deleted"].pop()["deleted"] = False
            bump()
        elif c == "name":
            sid = int(q.get("id", cur["id"]) or cur["id"])
            for s in state["strings"]:
                if s["id"] == sid:
                    s["name"] = v[:31]
            bump(True)
        elif c == "arm":
            state["armed_until"] = time.time() + 60 if v == "1" else 0
        elif c == "time":
            pass
        elif c == "cal":
            live = [x["muzzle"] if state["settings"]["reading"] == "muzzle" else x["chrono"] for x in live_shots(cur)]
            if len(live) < 3:
                return {"ok": False, "msg": "Need 3+ shots in this string, and a reference within 10%."}
            ratio = float(v) / (sum(live) / len(live))
            if abs(ratio - 1) > 0.1:
                return {"ok": False, "msg": "Need 3+ shots in this string, and a reference within 10%."}
            state["settings"]["cal"] = round(state["settings"]["cal"] * ratio, 5)
        else:
            return {"ok": False, "msg": "Unknown command."}
        return {"ok": True, "rev": state["rev"], "cal": state["settings"]["cal"], "msg": ""}


def fake_wav():
    rate, n = 50000, 20000
    buf = io.BytesIO()
    samples = [int(800 * math.sin(2 * math.pi * 14000 * i / rate) * math.exp(-((i - n / 2) / 4000) ** 2))
               for i in range(n)]
    data = struct.pack(f"<{n}h", *samples)
    buf.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
              struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data)) + data)
    return buf.getvalue()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    seed()
    threading.Thread(target=shooter, daemon=True).start()
    print(f"mock chrono on http://localhost:{port}/?live=1")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
