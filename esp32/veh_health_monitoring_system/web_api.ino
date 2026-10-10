/*
  web_api.ino  -  Wi-Fi + JSON API tab for veh_health_monitoring_system.ino
  Keep this file in the SAME folder as the main .ino (Arduino IDE shows it as a 2nd tab).
  Endpoint:  http://<ESP32-IP>/data   ->  JSON with every sensor reading + status
*/
#include <WiFi.h>
#include <WebServer.h>
#include <stdarg.h>

const char *WIFI_SSID = "VITAL";       // 2.4 GHz only
const char *WIFI_PASS = "VITAL321";

WebServer server(80);
static uint32_t lastWifiTry = 0;

// ---BEGIN JSON---
static char jbuf[1200];
static size_t jlen = 0;

static void jp(const char *fmt, ...) {
  if (jlen >= sizeof(jbuf) - 1) return;
  va_list a;
  va_start(a, fmt);
  int r = vsnprintf(jbuf + jlen, sizeof(jbuf) - jlen, fmt, a);
  va_end(a);
  if (r > 0) jlen += r;
  if (jlen >= sizeof(jbuf)) jlen = sizeof(jbuf) - 1;
}
static void jnum(float v, int d, bool ok = true) {
  if (!ok || isnan(v)) jp("null"); else jp("%.*f", d, v);
}
static void jstr(const char *s, bool ok = true) {
  if (!ok || !s) jp("null"); else jp("\"%s\"", s);
}

static const char *buildJson() {
  jlen = 0; jbuf[0] = 0;
  float mx = volts[0], mn = volts[0];
  for (int i = 1; i < 3; i++) { if (volts[i] > mx) mx = volts[i]; if (volts[i] < mn) mn = volts[i]; }

  jp("{\"cells\":[");
  for (int i = 0; i < 3; i++) { if (i) jp(","); jnum(volts[i], 2); }
  jp("],\"cell_s\":[");
  for (int i = 0; i < 3; i++) { if (i) jp(","); jstr(voltStatus[i]); }
  jp("],\"pack\":"); jnum(packV, 2);
  jp(",\"imbalance\":"); jnum(mx - mn, 2);

  jp(",\"temps\":[");
  for (int i = 0; i < 3; i++) { if (i) jp(","); jnum(tempC[i], 1, tempOK[i]); }
  jp("],\"temp_s\":[");
  for (int i = 0; i < 3; i++) { if (i) jp(","); jstr(tempStatus[i]); }

  jp("],\"bus_v\":");      jnum(busV, 2, inaOK);
  jp(",\"current_ma\":");  jnum(current_mA, 1, inaOK);
  jp(",\"power_mw\":");    jnum(power_mW, 0, inaOK);
  jp(",\"cur_s\":");       jstr(curStatus, inaOK);

  jp(",\"roll\":");  jnum(roll, 1, adxlOK);
  jp(",\"pitch\":"); jnum(pitch, 1, adxlOK);
  jp(",\"tilt\":");  jnum(tilt, 1, adxlOK);
  jp(",\"vib\":");   jnum(vib, 2, adxlOK);
  jp(",\"tilt_s\":"); jstr(tiltStatus, adxlOK);
  jp(",\"vib_s\":");  jstr(vibStatus, adxlOK);

  jp(",\"noise_db\":"); jnum(noiseDb, 1, micOK);
  jp(",\"noise_s\":");  jstr(noiseStatus, micOK);

  jp(",\"oil_pct\":"); jnum(oilPurity, 1);
  jp(",\"oil_raw\":"); jnum(oilRaw, 0);
  jp(",\"oil_s\":");   jstr(oilStatus);

  jp(",\"metal\":%s,\"metal_s\":", metalDetected ? "true" : "false"); jstr(metalStatus);
  jp(",\"insulation\":%s,\"insul_s\":", insulationOK ? "true" : "false"); jstr(insulStatus);
  jp(",\"alarm\":%s", anyAlarm ? "true" : "false");
  jp(",\"ok\":{\"ina\":%s,\"adxl\":%s,\"mic\":%s}", inaOK ? "true" : "false", adxlOK ? "true" : "false", micOK ? "true" : "false");
  jp(",\"uptime_s\":%lu}", (unsigned long)(millis() / 1000));
  return jbuf;
}
// ---END JSON---

static void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Cache-Control", "no-store");
}

static void handleData() {
  sendCors();
  server.send(200, "application/json", buildJson());
}
static void handleRoot() {
  sendCors();
  server.send(200, "text/plain", "VITAL ESP32-S3 OK. Open /data");
}
static void handleNotFound() {
  sendCors();
  if (server.method() == HTTP_OPTIONS) server.send(204);
  else server.send(404, "text/plain", "Not found");
}

void webSetup() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                      // faster HTTP response
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) Serial.printf("\nESP32 IP address: %s\n", WiFi.localIP().toString().c_str());
  else Serial.println("\nWiFi FAILED (check SSID / password). Sensors still work, web API offline.");

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 10000) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }
}
