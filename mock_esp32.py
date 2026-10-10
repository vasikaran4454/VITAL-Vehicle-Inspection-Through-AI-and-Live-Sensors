"""Fake ESP32-S3 for testing the VITAL dashboard WITHOUT hardware (same JSON as the real firmware).
Run:   python mock_esp32.py          (healthy readings)
       python mock_esp32.py fault    (shows alarms: low cell, hot sensor, metal, insulation fail)
Dashboard IP box:  127.0.0.1:8080
"""
import json, math, random, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer

FAULT = len(sys.argv) > 1 and sys.argv[1] == "fault"
T0 = time.time()
r = random.uniform


def reading():
    t = time.time() - T0
    cells = [3.91 + .03 * math.sin(t / 7) + r(-.01, .01), 3.90 + .03 * math.sin(t / 8) + r(-.01, .01), 3.92 + .03 * math.sin(t / 9) + r(-.01, .01)]
    temps = [27 + 2 * math.sin(t / 15) + r(-.2, .2), 29 + 2 * math.sin(t / 12) + r(-.2, .2), 26 + r(-.2, .2)]
    cur = 420 + 120 * math.sin(t / 5) + r(-15, 15)
    vib = abs(0.12 + .05 * math.sin(t / 3) + r(-.02, .02)); noise = 48 + 6 * math.sin(t / 4) + r(-1, 1)
    oil = 82 + r(-.5, .5); metal = False; ins = True
    cs = ["NORMAL"] * 3; ts = ["NORMAL"] * 3; cur_s = "NORMAL"; oil_s = "GOOD"; metal_s = "NORMAL"; ins_s = "OK"
    if FAULT:
        cells[2] = 2.9; cs[2] = "LOW"; temps[0] = 46; ts[0] = "HIGH"; metal = True; metal_s = "ALERT"
        ins = False; ins_s = "FAIL"; oil = 35; oil_s = "POOR"; cur = 1300; cur_s = "HIGH"
    pack = sum(cells)
    d = {"cells": [round(x, 2) for x in cells], "cell_s": cs, "pack": round(pack, 2), "imbalance": round(max(cells) - min(cells), 2),
         "temps": [round(x, 1) for x in temps], "temp_s": ts, "bus_v": round(pack, 2), "current_ma": round(cur, 1),
         "power_mw": round(pack * cur), "cur_s": cur_s, "roll": round(r(-2, 2), 1), "pitch": round(r(-2, 2), 1), "tilt": round(abs(r(0, 3)), 1),
         "vib": round(vib, 2), "tilt_s": "OK", "vib_s": "OK", "noise_db": round(noise, 1), "noise_s": "NORMAL",
         "oil_pct": round(oil, 1), "oil_raw": round((100 - oil) / 100 * 4095), "oil_s": oil_s, "metal": metal, "metal_s": metal_s,
         "insulation": ins, "insul_s": ins_s, "alarm": FAULT, "ok": {"ina": True, "adxl": True, "mic": True}, "uptime_s": int(time.time() - T0)}
    return d


class H(BaseHTTPRequestHandler):
    def _cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Private-Network", "true")

    def do_OPTIONS(self):
        self.send_response(204); self._cors(); self.end_headers()

    def do_GET(self):
        if self.path.split("?")[0] != "/data":
            self.send_response(404); self._cors(); self.end_headers(); return
        body = json.dumps(reading()).encode()
        self.send_response(200); self._cors()
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)

    def log_message(self, *a):
        pass


print("Mock ESP32-S3 on port 8080 (%s). Dashboard IP: 127.0.0.1:8080   Ctrl+C to stop" % ("FAULT mode" if FAULT else "healthy"))
HTTPServer(("0.0.0.0", 8080), H).serve_forever()
