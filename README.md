# VITAL: Vehicle Inspection Through AI and Live Sensors

Bench-top proof of concept for vehicle health monitoring. An **ESP32-S3** reads live sensors and serves them over Wi-Fi to a web dashboard. Physical damage that sensors cannot see (tyre, brake, battery terminal, engine bay, dashboard lights) is checked from a photo using a pretrained AI vision model, and a PDF report is generated.

## Features
- Live dashboard: 8 sensor groups, health score, alerts, 4 live charts, mobile responsive
- ESP32-S3 firmware: local alarm (OLED + buzzer) and a JSON web API (`/data`)
- AI photo inspection for 5 parts: Tyre, Battery, Brake, Engine bay, Dashboard warning lights
- Wrong-part photos are rejected (e.g. brake photo uploaded as "Tyre")
- OpenCV photo quality check (blur, brightness, resolution) before AI analysis
- PDF inspection report with findings, advice and live sensor snapshot
- `mock_esp32.py` to test the dashboard without hardware

## Architecture
```
Sensors > ESP32-S3 > OLED + Buzzer
                   > Wi-Fi  GET /data (JSON, 1 s) > Dashboard (Flask)
Photo > OpenCV quality check > Gemini vision > findings > PDF report
```

## Hardware (ESP32-S3)
| Sensor | Pins | Measures |
|---|---|---|
| 3x voltage input (3S pack) | GPIO 4, 5, 6 | Cell voltages, pack voltage, imbalance |
| 3x DS18B20 | GPIO 1, 2, 10 | Battery / coolant / charge-port temperature |
| INA219 | I2C 0x40 | Current, power, bus voltage |
| ADXL345 | I2C 0x53 | Vibration, tilt |
| INMP441 mic | GPIO 12, 11, 13 | Noise (dB) |
| Oil sensor | GPIO 7 | Oil purity |
| Metal sensor | GPIO 14 | Metal wear debris |
| Insulation input | GPIO 16 | Insulation fault |
| Buzzer / OLED | GPIO 15 / I2C 0x3C | Local alarm / display |

## Setup (Windows)
```
py -m venv venv
venv\Scripts\activate
pip install -r requirements.txt
copy .env.example .env      (then put your GEMINI_API_KEY in .env)
python app.py
```
Open http://localhost:5000

**Test without hardware:** run `python mock_esp32.py` (or `python mock_esp32.py fault`) in a second terminal and connect the dashboard to `127.0.0.1:8080`.

## ESP32-S3 firmware
1. Arduino IDE: install **esp32 by Espressif (3.x)**, board **ESP32S3 Dev Module**
2. Libraries: Adafruit SSD1306, Adafruit GFX, Adafruit INA219, Adafruit ADXL345, Adafruit Unified Sensor, OneWire, DallasTemperature
3. Open `esp32/veh_health_monitoring_system/veh_health_monitoring_system.ino` (`web_api.ino` opens as a second tab)
4. Set `WIFI_SSID` / `WIFI_PASS` in `web_api.ino` (2.4 GHz), upload
5. Serial Monitor (115200) shows the IP; type it in the dashboard and click Connect

## API
| Endpoint | Description |
|---|---|
| `GET http://<esp32-ip>/data` | All sensor values and status (JSON) |
| `POST /api/analyze` | Photo + part, returns AI findings |
| `GET /api/report/<id>` | PDF report |

## Project structure
```
app.py                Flask server
analyzer/             cv_metrics.py (OpenCV), vision.py (Gemini), report.py (PDF)
templates/ static/    Dashboard UI
esp32/                ESP32-S3 firmware (sensor code + web API tab)
mock_esp32.py         Fake ESP32 for testing
```

## Limitations (honest notes)
- Bench-top demo with a small test battery pack; not mounted on a real vehicle
- Sensor fault detection is **threshold based** (limits set in firmware), not machine learning
- Photo inspection uses a **pretrained vision model (Google Gemini) through an API**, not a model trained by us; it needs internet and a free API key
- AI result from one photo does not replace a mechanic's inspection

## Future scope
Own CNN trained on a tyre dataset, anomaly detection on sensor data (autoencoder), FFT vibration analysis, battery SoH prediction, real-vehicle testing.

## Author
Vasikaran V