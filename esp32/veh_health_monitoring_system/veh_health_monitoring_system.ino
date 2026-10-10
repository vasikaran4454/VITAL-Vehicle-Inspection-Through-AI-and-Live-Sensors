/*
  ESP32-S3 Multi-Sensor Monitor -> 0.96" OLED (SSD1306 128x64, I2C)
  SLIDE DISPLAY VERSION 5
    - DS18B20 temperature: EACH SENSOR ON ITS OWN PIN (separate 1-Wire buses)
    - 3S battery pack: individual CELL voltage reading
    - Buzzer alert output
    - Insulation test input

  Screens (auto-rotate every SLIDE_INTERVAL_MS):
    1. CELL VOLT  : C1, C2, C3 + pack voltage     (NORMAL / HIGH / LOW)
    2. TEMPERATURE: T1, T2, T3  (DS18B20)         (NORMAL / HIGH / ERR)
    3. CURRENT    : INA219 bus V, mA, mW          (NORMAL / HIGH)
    4. IMU        : roll, pitch, tilt, vibration  (OK / TILT, OK / HIGH)
    5. NOISE      : dB level + bar graph          (NORMAL / HIGH)
    6. OIL PURITY : ADC 0-4095 -> 0-100 %         (GOOD / FAIR / POOR)
    7. METAL      : digital input                 (HIGH = METAL DETECTED,
                                                   LOW  = METAL NOT-DETECTED)
    8. INSULATION : input pullup, pin to GND      (LOW  = Insulation OK,
                                                   HIGH = NO Insulation)

  Sensors / pins:
   - 3x DS18B20 temperature, ONE PIN PER SENSOR:
        T1 -> GPIO1
        T2 -> GPIO2
        T3 -> GPIO10
        Each data pin needs its OWN 4.7k pull-up to 3.3V.
   - 3x Arduino voltage sensor      (ADC1: GPIO4, GPIO5, GPIO6)  [0-25V module, 5:1 divider]
        3S battery pack wiring (all sensor "-" to pack B-, common with ESP32 GND):
          Sensor 1 (GPIO4) "+" -> B1  (after cell 1)   -> reads Cell 1
          Sensor 2 (GPIO5) "+" -> B2  (after cell 2)   -> reads Cell 1 + 2
          Sensor 3 (GPIO6) "+" -> B+  (pack positive)  -> reads full pack
        Cell 1 = tap1, Cell 2 = tap2 - tap1, Cell 3 = tap3 - tap2
   - Oil conductivity (analog)      (ADC1: GPIO7)
   - Metal detection sensor (digital) (GPIO14)
   - Buzzer output (active buzzer)  (GPIO15)
   - Insulation test input          (GPIO16)  (INPUT_PULLUP)
   - INA219 current/bus voltage     (I2C 0x40)
   - ADXL345 tilt + vibration       (I2C 0x53)
   - INMP441 I2S MEMS microphone    (BCLK GPIO12, WS GPIO11, SD GPIO13)

  I2C bus (shared): SDA = GPIO8, SCL = GPIO9
    OLED 0x3C, INA219 0x40, ADXL345 0x53

  Libraries (Library Manager):
   - Adafruit SSD1306, Adafruit GFX
   - Adafruit INA219
   - Adafruit ADXL345, Adafruit Unified Sensor
   - OneWire (Paul Stoffregen), DallasTemperature (Miles Burton)
   - ESP_I2S is built into ESP32 Arduino core 3.x (use core 3.0.0 or newer)

  NOTE: Arduino IDE needs the .ino inside a folder with the same name.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_INA219.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_ADXL345_U.h>
#include <ESP_I2S.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

// ---------------- Pins ----------------
#define I2C_SDA 8
#define I2C_SCL 9

// DS18B20 data pins: one separate 1-Wire bus per sensor
const uint8_t DS18B20_PINS[3] = {1, 2, 10};

const uint8_t VOLT_PINS[3] = {4, 5, 6};

#define OIL_PIN    7     // oil conductivity sensor (analog, ADC1)
#define METAL_PIN  14    // metal detection sensor (digital)

#define BUZZER_PIN 15    // buzzer output
#define INSUL_PIN  16    // insulation test input (INPUT_PULLUP, connect to GND = OK)

#define I2S_BCLK 12   // INMP441 SCK
#define I2S_WS   11   // INMP441 WS
#define I2S_DIN  13   // INMP441 SD   (INMP441 L/R pin -> GND = left channel)

// ---------------- Slide settings ----------------
#define SLIDE_COUNT        8
#define SLIDE_INTERVAL_MS  3000    // time each screen stays visible

// ---------------- ADC settling / sampling settings ----------------
#define ADC_DUMMY_READS    12
#define ADC_SETTLE_MS      10
#define ADC_PARK_PIN       VOLT_PINS[0]

#define VOLT_SAMPLES       40     // max 64
#define OIL_SAMPLES        40     // max 64

// ---------------- DS18B20 settings ----------------
#define DS18B20_RESOLUTION 12      // 9..12 bit (12 bit = 750 ms conversion)
#define DS18B20_CONV_MS    800     // wait time before reading a conversion
#define DS18B20_MAX_FAILS  5       // show ERR only after this many failed reads in a row
const float DS18B20_CAL[3] = {0.0, 0.0, 0.0};   // offset in degC per channel

// ---------------- Voltage sensor calibration ----------------
const float VOLT_RATIO = 5.0;
const float VOLT_CAL[3] = {1.0, 1.0, 1.0};   // tune each tap against a multimeter (B1, B2, B+)

// ---------------- Oil purity settings ----------------
// Purity % = ADC raw (0..4095) mapped to 0..100 %
#define OIL_ADC_MAX     4095.0
#define OIL_INVERT      1        // 1 = purity = 100 - mapped value (if your sensor works reversed)
const float OIL_GOOD_MIN = 70.0; // >= 70 %  -> GOOD
const float OIL_FAIR_MIN = 40.0; // >= 40 %  -> FAIR, below -> POOR (alarm)

// ---------------- Metal sensor settings ----------------
#define METAL_DETECT_LEVEL  HIGH   // pin level that means "metal detected"
// Use INPUT_PULLDOWN / INPUT_PULLUP only if your module needs it (e.g. open-collector output)
#define METAL_PIN_MODE      INPUT

// ---------------- Buzzer settings ----------------
#define BUZZER_ON_LEVEL     HIGH   // use LOW if your buzzer module is active-LOW
#define BUZZER_OFF_LEVEL    (!BUZZER_ON_LEVEL)

// ---------------- Insulation test settings ----------------
// Pin is INPUT_PULLUP. Connected to GND (LOW) = Insulation OK. Pin HIGH = NO Insulation.
#define INSUL_OK_LEVEL      LOW

// ---------------- Alarm / status thresholds ----------------
// Voltage limits are per CELL (3S Li-ion). For LiFePO4 use about 3.65 / 2.5.
const float VOLT_HIGH_LIMIT = 4.25;    // V  -> "HIGH" above this (cell overcharge)
const float VOLT_LOW_LIMIT  = 3.00;    // V  -> "LOW" below this (cell over-discharge)
const float TEMP_LIMIT_C    = 40.0;    // degC
const float CURRENT_LIMIT_MA = 1000.0; // mA
const float TILT_LIMIT_DEG  = 30.0;    // degrees from vertical
const float VIB_LIMIT_MS2   = 2.0;     // RMS vibration m/s^2
const float NOISE_LIMIT_DB  = 85.0;    // approx dB SPL

// ---------------- Objects ----------------
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Adafruit_INA219 ina219(0x40);
Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified(12345);
I2SClass i2s;

// One OneWire bus + one DallasTemperature per DS18B20 (created in setup)
OneWire           *oneWire[3]  = {nullptr, nullptr, nullptr};
DallasTemperature *ds18b20[3]  = {nullptr, nullptr, nullptr};

bool oledOK = false, inaOK = false, adxlOK = false, micOK = false;

// ---------------- Running (moving) average ----------------
#define AVG_WINDOW 8

class MovingAvg {
  float buf[AVG_WINDOW];
  float sum = 0;
  int idx = 0, count = 0;
public:
  float add(float v) {
    if (count < AVG_WINDOW) {
      buf[idx] = v; sum += v; count++;
    } else {
      sum -= buf[idx];
      buf[idx] = v;
      sum += v;
    }
    idx = (idx + 1) % AVG_WINDOW;
    return sum / count;
  }
};

MovingAvg tempAvg[3];
MovingAvg voltAvg[3];
MovingAvg oilAvg;

// ---------------- Readings ----------------
float tempC[3] = {0, 0, 0};
bool  tempOK[3] = {false, false, false};   // false = no valid reading
DeviceAddress tempAddr[3];                 // ROM address of each DS18B20
bool  tempPresent[3] = {false, false, false}; // sensor detected on its pin
uint8_t tempFail[3] = {0, 0, 0};           // consecutive failed reads

float volts[3];          // CELL 1, 2, 3 voltages
float tapV[3];           // cumulative tap voltages: B1, B2, B+
float packV = 0;         // total pack voltage

float busV = 0, current_mA = 0, power_mW = 0;
float roll = 0, pitch = 0, tilt = 0, vib = 0;
float noiseDb = 0;
float oilRaw = 0, oilPurity = 0;
bool  metalDetected = false;
bool  insulationOK = false;

// ---------------- Status strings ----------------
const char *voltStatus[3];
const char *tempStatus[3];
const char *curStatus   = "NORMAL";
const char *tiltStatus  = "OK";
const char *vibStatus   = "OK";
const char *noiseStatus = "NORMAL";
const char *oilStatus   = "GOOD";
const char *metalStatus = "NORMAL";
const char *insulStatus = "OK";
bool anyAlarm = false;

// ---------------- ADC helpers ----------------
void parkADC() {
  for (int i = 0; i < 4; i++) analogReadMilliVolts(ADC_PARK_PIN);
}

// Settle on the channel, then trimmed mean.
// rawCounts = false -> returns millivolts, true -> returns raw ADC counts (0..4095)
float readSettled(uint8_t pin, int samples, bool rawCounts = false) {
  if (samples > 64) samples = 64;

  parkADC();
  for (int i = 0; i < ADC_DUMMY_READS; i++) analogReadMilliVolts(pin);
  delay(ADC_SETTLE_MS);
  for (int i = 0; i < 4; i++) analogReadMilliVolts(pin);

  uint32_t s[64];
  for (int i = 0; i < samples; i++) {
    s[i] = rawCounts ? (uint32_t)analogRead(pin) : analogReadMilliVolts(pin);
    delayMicroseconds(500);
  }

  for (int i = 1; i < samples; i++) {
    uint32_t key = s[i];
    int j = i - 1;
    while (j >= 0 && s[j] > key) { s[j + 1] = s[j]; j--; }
    s[j + 1] = key;
  }

  int lo = samples / 5;
  int hi = samples - lo;
  uint32_t sum = 0;
  for (int i = lo; i < hi; i++) sum += s[i];
  return sum / (float)(hi - lo);
}

// ---------------- DS18B20 (one bus per sensor) ----------------
// Look for a sensor on bus i. Returns true if found and addressed.
bool detectTemp(int i) {
  ds18b20[i]->begin();
  if (ds18b20[i]->getDeviceCount() < 1) return false;
  if (!ds18b20[i]->getAddress(tempAddr[i], 0)) return false;
  ds18b20[i]->setResolution(tempAddr[i], DS18B20_RESOLUTION);
  return true;
}

// Non-blocking. A conversion is requested on all three buses, and results are
// read on a later call once DS18B20_CONV_MS has passed (so the loop never stalls).
// A failed read keeps the last good value; ERR is shown only after
// DS18B20_MAX_FAILS failed reads in a row.
// A sensor that was not found is re-scanned every cycle (hot-plug / loose wire).
void readTemps() {
  static bool requested = false;
  static uint32_t reqTime = 0;

  if (!requested) {
    for (int i = 0; i < 3; i++) ds18b20[i]->requestTemperatures();
    reqTime = millis();
    requested = true;
    return;
  }

  if (millis() - reqTime < DS18B20_CONV_MS) return;   // conversion not ready

  for (int i = 0; i < 3; i++) {
    if (!tempPresent[i]) {
      tempPresent[i] = detectTemp(i);        // try to find it again
      tempOK[i] = false;
      continue;
    }

    float t = ds18b20[i]->getTempC(tempAddr[i]);
    // 85.0 is the DS18B20 power-on default (conversion not valid), -127 = no reply
    bool bad = (t == DEVICE_DISCONNECTED_C || t < -55.0 || t > 125.0 || t == 85.0);

    if (bad) {
      if (tempFail[i] < 255) tempFail[i]++;
      if (tempFail[i] >= DS18B20_MAX_FAILS) {
        tempOK[i] = false;
        tempPresent[i] = false;              // force re-detect next cycle
      }
      continue;                              // keep the last good value meanwhile
    }

    tempFail[i] = 0;
    tempOK[i] = true;
    tempC[i] = tempAvg[i].add(t + DS18B20_CAL[i]);
  }

  for (int i = 0; i < 3; i++) ds18b20[i]->requestTemperatures();   // next conversion
  reqTime = millis();
}

// 3S battery pack: sensors read cumulative taps (B1, B2, B+),
// cell voltage = difference between neighbouring taps.
void readVoltages() {
  for (int i = 0; i < 3; i++) {
    float mv = readSettled(VOLT_PINS[i], VOLT_SAMPLES);
    float raw = (mv / 1000.0) * VOLT_RATIO * VOLT_CAL[i];
    tapV[i] = voltAvg[i].add(raw);          // smoothed tap voltage
  }

  volts[0] = tapV[0];                       // Cell 1 = B1
  volts[1] = tapV[1] - tapV[0];             // Cell 2 = B2 - B1
  volts[2] = tapV[2] - tapV[1];             // Cell 3 = B+ - B2
  for (int i = 0; i < 3; i++) if (volts[i] < 0) volts[i] = 0;

  packV = tapV[2];
}

// Oil purity: raw ADC 0..4095 -> 0..100 %
void readOil() {
  float raw = readSettled(OIL_PIN, OIL_SAMPLES, true);
  oilRaw = raw;

  float pct = (raw / OIL_ADC_MAX) * 100.0;
  pct = constrain(pct, 0.0, 100.0);
#if OIL_INVERT
  pct = 100.0 - pct;
#endif
  oilPurity = oilAvg.add(pct);
}

// Metal sensor: majority vote of 5 quick reads (simple debounce)
void readMetal() {
  int hits = 0;
  for (int i = 0; i < 5; i++) {
    if (digitalRead(METAL_PIN) == METAL_DETECT_LEVEL) hits++;
    delay(2);
  }
  metalDetected = (hits >= 3);
}

// Insulation test: majority vote of 5 quick reads (simple debounce)
// LOW (pin connected to GND) = Insulation OK, HIGH = NO Insulation
void readInsulation() {
  int hits = 0;
  for (int i = 0; i < 5; i++) {
    if (digitalRead(INSUL_PIN) == INSUL_OK_LEVEL) hits++;
    delay(2);
  }
  insulationOK = (hits >= 3);
}

// ---------------- Other sensors ----------------
void readINA() {
  if (!inaOK) return;
  busV       = ina219.getBusVoltage_V();
  current_mA = ina219.getCurrent_mA();
  power_mW   = ina219.getPower_mW();
}

void readADXL() {
  if (!adxlOK) return;
  const int N = 32;
  float xs[N], ys[N], zs[N];
  float mx = 0, my = 0, mz = 0;

  for (int i = 0; i < N; i++) {
    sensors_event_t e;
    adxl.getEvent(&e);
    xs[i] = e.acceleration.x;
    ys[i] = e.acceleration.y;
    zs[i] = e.acceleration.z;
    mx += xs[i]; my += ys[i]; mz += zs[i];
    delay(3);
  }
  mx /= N; my /= N; mz /= N;

  roll  = atan2(my, mz) * 180.0 / PI;
  pitch = atan2(-mx, sqrt(my * my + mz * mz)) * 180.0 / PI;
  float mag = sqrt(mx * mx + my * my + mz * mz);
  tilt = (mag > 0.1) ? acos(constrain(mz / mag, -1.0, 1.0)) * 180.0 / PI : 0;

  float acc = 0;
  for (int i = 0; i < N; i++) {
    float dx = xs[i] - mx, dy = ys[i] - my, dz = zs[i] - mz;
    acc += dx * dx + dy * dy + dz * dz;
  }
  vib = sqrt(acc / N);
}

void readMic() {
  if (!micOK) return;
  const int SAMPLES = 512;
  static int32_t buf[SAMPLES];

  size_t bytes = i2s.readBytes((char *)buf, sizeof(buf));
  int n = bytes / sizeof(int32_t);
  if (n <= 0) return;

  double mean = 0;
  for (int i = 0; i < n; i++) { buf[i] >>= 8; mean += buf[i]; }
  mean /= n;

  double sumSq = 0;
  for (int i = 0; i < n; i++) {
    double s = buf[i] - mean;
    sumSq += s * s;
  }
  double rms = sqrt(sumSq / n);
  if (rms < 1) rms = 1;

  double dbfs = 20.0 * log10(rms / 8388608.0);
  noiseDb = (float)(dbfs + 120.0);
  if (noiseDb < 0) noiseDb = 0;
}

// ---------------- Status evaluation ----------------
void updateStatus() {
  anyAlarm = false;

  for (int i = 0; i < 3; i++) {
    if (volts[i] > VOLT_HIGH_LIMIT)      { voltStatus[i] = "HIGH";  }
    else if (volts[i] < VOLT_LOW_LIMIT)  { voltStatus[i] = "LOW";    }
    else                                 { voltStatus[i] = "NORMAL"; }

    if (!tempOK[i])                      { tempStatus[i] = "ERR";    }
    else if (tempC[i] > TEMP_LIMIT_C)    { tempStatus[i] = "HIGH";   anyAlarm = true; }
    else                                 { tempStatus[i] = "NORMAL"; }
  }

  if (inaOK && fabs(current_mA) > CURRENT_LIMIT_MA) { curStatus = "HIGH";  }
  else                                              { curStatus = "NORMAL"; }

  if (adxlOK && tilt > TILT_LIMIT_DEG) { tiltStatus = "TILT";  }
  else                                 { tiltStatus = "OK"; }

  if (adxlOK && vib > VIB_LIMIT_MS2)   { vibStatus = "HIGH"; }
  else                                 { vibStatus = "OK"; }

  if (micOK && noiseDb > NOISE_LIMIT_DB) { noiseStatus = "HIGH"; }
  else                                   { noiseStatus = "NORMAL"; }

  if (oilPurity >= OIL_GOOD_MIN)       { oilStatus = "GOOD"; }
  else if (oilPurity >= OIL_FAIR_MIN)  { oilStatus = "FAIR"; }
  else                                 { oilStatus = "POOR"; }

  if (metalDetected) { metalStatus = "ALERT";  anyAlarm = true;  }
  else               { metalStatus = "NORMAL"; }

  if (insulationOK)  { insulStatus = "OK"; }
  else               { insulStatus = "FAIL"; anyAlarm = true; }
}

// ---------------- Buzzer ----------------
// Buzzer ON whenever any alert is active, OFF otherwise
void updateBuzzer() {
  digitalWrite(BUZZER_PIN, anyAlarm ? BUZZER_ON_LEVEL : BUZZER_OFF_LEVEL);
}

// ---------------- Display helpers ----------------
void drawHeader(int slide, const char *title) {
  display.fillRect(0, 0, SCREEN_W, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(4, 2);
  display.printf("%d/%d  %s", slide + 1, SLIDE_COUNT, title);
  display.setTextColor(SSD1306_WHITE);
}

// Page dots (left) + alarm flag (right)
void drawFooter(int slide) {
  for (int i = 0; i < SLIDE_COUNT; i++) {
    int x = 4 + i * 8;
    if (i == slide) display.fillCircle(x, 60, 2, SSD1306_WHITE);
    else            display.drawCircle(x, 60, 2, SSD1306_WHITE);
  }
  if (anyAlarm) {
    display.setCursor(86, 56);
    display.print("!ALARM");
  }
}

void drawVoltageSlide(int s) {
  drawHeader(s, "CELL VOLTAGE");
  for (int i = 0; i < 3; i++) {
    display.setCursor(0, 14 + i * 10);
    display.printf("C%d:%5.2fV %s", i + 1, volts[i], voltStatus[i]);
  }
  display.setCursor(0, 44);
  display.printf("Pack:%5.2fV", packV);
}

void drawTempSlide(int s) {
  drawHeader(s, "TEMPERATURE");
  for (int i = 0; i < 3; i++) {
    display.setCursor(0, 15 + i * 12);
    if (tempOK[i]) display.printf("T%d:%5.1fC %s", i + 1, tempC[i], tempStatus[i]);
    else           display.printf("T%d:  ---  %s", i + 1, tempStatus[i]);
  }
}

void drawCurrentSlide(int s) {
  drawHeader(s, "CURRENT");
  if (!inaOK) {
    display.setCursor(0, 24);
    display.print("INA219 not found");
    return;
  }
  display.setCursor(0, 14); display.printf("Bus:%.2f V", busV);
  display.setCursor(0, 24); display.printf("Cur:%.1f mA", current_mA);
  display.setCursor(0, 34); display.printf("Pwr:%.0f mW", power_mW);
  display.setCursor(0, 45); display.printf("Status: %s", curStatus);
}

void drawImuSlide(int s) {
  drawHeader(s, "IMU DATA");
  if (!adxlOK) {
    display.setCursor(0, 24);
    display.print("ADXL345 not found");
    return;
  }
  display.setCursor(0, 14); display.printf("Roll:%.0f Pitch:%.0f", roll, pitch);
  display.setCursor(0, 24); display.printf("Tilt:%.0f deg  %s", tilt, tiltStatus);
  display.setCursor(0, 34); display.printf("Vib:%.2f m/s2", vib);
  display.setCursor(0, 44); display.printf("Vib status: %s", vibStatus);
}

void drawNoiseSlide(int s) {
  drawHeader(s, "NOISE LEVEL");
  if (!micOK) {
    display.setCursor(0, 24);
    display.print("INMP441 error");
    return;
  }
  display.setTextSize(2);
  display.setCursor(0, 15);
  display.printf("%.0f dB", noiseDb);
  display.setTextSize(1);

  display.setCursor(80, 20);
  display.print(noiseStatus);

  // Bar graph 0..120 dB with a marker at the alarm limit
  int w = (int)constrain(noiseDb / 120.0 * 126.0, 0, 126);
  display.drawRect(0, 36, 128, 9, SSD1306_WHITE);
  display.fillRect(1, 37, w, 7, SSD1306_WHITE);
  int lim = (int)(NOISE_LIMIT_DB / 120.0 * 126.0) + 1;
  display.drawFastVLine(lim, 33, 15, SSD1306_WHITE);
}

void drawOilSlide(int s) {
  drawHeader(s, "OIL PURITY");

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.printf("%.1f%%", oilPurity);
  display.setTextSize(1);

  display.setCursor(88, 18);
  display.print(oilStatus);

  // Bar graph 0..100 %
  int w = (int)constrain(oilPurity / 100.0 * 126.0, 0, 126);
  display.drawRect(0, 33, 128, 9, SSD1306_WHITE);
  display.fillRect(1, 34, w, 7, SSD1306_WHITE);

  display.setCursor(0, 45);
  display.printf("ADC:%4.0f / 4095", oilRaw);
}

void drawMetalSlide(int s) {
  drawHeader(s, "METAL DETECTION");

  // Banner: filled = detected, outlined = not detected
  if (metalDetected) {
    display.fillRect(0, 16, 128, 18, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(17, 21);
    display.print("METAL DETECTED");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.drawRect(0, 16, 128, 18, SSD1306_WHITE);
    display.setCursor(7, 21);
    display.print("METAL NOT-DETECTED");
  }

  display.setCursor(0, 40);
  display.printf("Pin: %s", digitalRead(METAL_PIN) ? "HIGH" : "LOW");
  display.setCursor(0, 49);
  display.printf("Status: %s", metalStatus);
}

void drawInsulationSlide(int s) {
  drawHeader(s, "INSULATION TEST");

  // Banner: outlined = OK, filled = NO Insulation
  if (insulationOK) {
    display.drawRect(0, 16, 128, 18, SSD1306_WHITE);
    display.setCursor(22, 21);
    display.print("Insulation OK");
  } else {
    display.fillRect(0, 16, 128, 18, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(22, 21);
    display.print("NO Insulation");
    display.setTextColor(SSD1306_WHITE);
  }

  display.setCursor(0, 40);
  display.printf("Pin: %s", digitalRead(INSUL_PIN) ? "HIGH" : "LOW");
  display.setCursor(0, 49);
  display.printf("Status: %s", insulStatus);
}

void updateDisplay() {
  if (!oledOK) return;

  static int slide = 0;
  static uint32_t lastSlide = 0;
  if (millis() - lastSlide >= SLIDE_INTERVAL_MS) {
    lastSlide = millis();
    slide = (slide + 1) % SLIDE_COUNT;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  switch (slide) {
    case 0: drawVoltageSlide(slide);    break;
    case 1: drawTempSlide(slide);       break;
    case 2: drawCurrentSlide(slide);    break;
    case 3: drawImuSlide(slide);        break;
    case 4: drawNoiseSlide(slide);      break;
    case 5: drawOilSlide(slide);        break;
    case 6: drawMetalSlide(slide);      break;
    case 7: drawInsulationSlide(slide); break;
  }

  drawFooter(slide);
  display.display();
}

void printSerial() {
  for (int i = 0; i < 3; i++) {
    if (tempOK[i]) Serial.printf("T%d=%.1fC ", i + 1, tempC[i]);
    else           Serial.printf("T%d=ERR ", i + 1);
  }
  Serial.printf("| Cells: %.2f %.2f %.2f V (Pack %.2f V) | "
                "Bus: %.2f V, I: %.1f mA, P: %.0f mW | Roll: %.1f Pitch: %.1f Tilt: %.1f | "
                "Vib: %.2f m/s2 | Noise: %.1f dB | Oil: %.1f %% (ADC %.0f) | Metal: %s | "
                "Insulation: %s | Buzzer: %s\n",
                volts[0], volts[1], volts[2], packV,
                busV, current_mA, power_mW,
                roll, pitch, tilt, vib, noiseDb,
                oilPurity, oilRaw,
                metalDetected ? "DETECTED" : "NOT-DETECTED",
                insulationOK ? "OK" : "NO INSULATION",
                anyAlarm ? "ON" : "OFF");
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  // Buzzer: make sure it is OFF at boot
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  analogReadResolution(12);
  for (int i = 0; i < 3; i++) {
    analogSetPinAttenuation(VOLT_PINS[i], ADC_11db);
  }
  analogSetPinAttenuation(OIL_PIN, ADC_11db);

  pinMode(METAL_PIN, METAL_PIN_MODE);
  pinMode(INSUL_PIN, INPUT_PULLUP);

  // DS18B20 init: one OneWire bus per sensor (non-blocking, ROM addressed)
  for (int i = 0; i < 3; i++) {
    oneWire[i]  = new OneWire(DS18B20_PINS[i]);
    ds18b20[i]  = new DallasTemperature(oneWire[i]);
    ds18b20[i]->begin();
    ds18b20[i]->setWaitForConversion(false);
    tempPresent[i] = detectTemp(i);
    Serial.printf("DS18B20 T%d on GPIO%d: %s\n", i + 1, DS18B20_PINS[i],
                  tempPresent[i] ? "found" : "NOT found");
  }

  for (int i = 0; i < 3; i++) { voltStatus[i] = "NORMAL"; tempStatus[i] = "NORMAL"; }

  oledOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (!oledOK) Serial.println("OLED not found!");
  else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("ESP32-S3 Monitor");
    display.println("Initializing...");
    display.display();
  }

  inaOK = ina219.begin();
  if (!inaOK) Serial.println("INA219 not found!");

  adxlOK = adxl.begin(0x53);
  if (!adxlOK) Serial.println("ADXL345 not found!");
  else {
    adxl.setRange(ADXL345_RANGE_4_G);
    adxl.setDataRate(ADXL345_DATARATE_800_HZ);
  }

  i2s.setPins(I2S_BCLK, I2S_WS, -1, I2S_DIN);
  micOK = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
                    I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
  if (!micOK) Serial.println("INMP441 I2S init failed!");

  webSetup();   // Wi-Fi + web API (see web_api.ino)
  delay(800);
}

void loop() {
  readTemps();
  readVoltages();
  readOil();
  readMetal();
  readInsulation();
  readINA();
  readADXL();
  readMic();

  updateStatus();
  updateBuzzer();
  updateDisplay();
  printSerial();

  for (int i = 0; i < 10; i++) { webLoop(); delay(10); }   // serve web requests (~100 ms)
}



// /*
//   ESP32-S3 Multi-Sensor Monitor -> 0.96" OLED (SSD1306 128x64, I2C)
//   SLIDE DISPLAY VERSION 4
//     - DS18B20 temperature (robust read, ROM addressed, ERR only after repeated fails)
//     - 3S battery pack: individual CELL voltage reading
//     - Buzzer alert output
//     - Insulation test input

//   Screens (auto-rotate every SLIDE_INTERVAL_MS):
//     1. CELL VOLT  : C1, C2, C3 + pack voltage     (NORMAL / HIGH / LOW)
//     2. TEMPERATURE: T1, T2, T3  (DS18B20)         (NORMAL / HIGH / ERR)
//     3. CURRENT    : INA219 bus V, mA, mW          (NORMAL / HIGH)
//     4. IMU        : roll, pitch, tilt, vibration  (OK / TILT, OK / HIGH)
//     5. NOISE      : dB level + bar graph          (NORMAL / HIGH)
//     6. OIL PURITY : ADC 0-4095 -> 0-100 %         (GOOD / FAIR / POOR)
//     7. METAL      : digital input                 (HIGH = METAL DETECTED,
//                                                    LOW  = METAL NOT-DETECTED)
//     8. INSULATION : input pullup, pin to GND      (LOW  = Insulation OK,
//                                                    HIGH = NO Insulation)

//   Sensors / pins:
//    - 3x DS18B20 temperature (1-Wire bus, GPIO1)
//         All 3 sensors share ONE data wire on GPIO1 with a 4.7k pull-up to 3.3V
//    - 3x Arduino voltage sensor      (ADC1: GPIO4, GPIO5, GPIO6)  [0-25V module, 5:1 divider]
//         3S battery pack wiring (all sensor "-" to pack B-, common with ESP32 GND):
//           Sensor 1 (GPIO4) "+" -> B1  (after cell 1)   -> reads Cell 1
//           Sensor 2 (GPIO5) "+" -> B2  (after cell 2)   -> reads Cell 1 + 2
//           Sensor 3 (GPIO6) "+" -> B+  (pack positive)  -> reads full pack
//         Cell 1 = tap1, Cell 2 = tap2 - tap1, Cell 3 = tap3 - tap2
//    - Oil conductivity (analog)      (ADC1: GPIO7)
//    - Metal detection sensor (digital) (GPIO14)
//    - Buzzer output (active buzzer)  (GPIO15)
//    - Insulation test input          (GPIO16)  (INPUT_PULLUP)
//    - INA219 current/bus voltage     (I2C 0x40)
//    - ADXL345 tilt + vibration       (I2C 0x53)
//    - INMP441 I2S MEMS microphone    (BCLK GPIO12, WS GPIO11, SD GPIO13)

//   I2C bus (shared): SDA = GPIO8, SCL = GPIO9
//     OLED 0x3C, INA219 0x40, ADXL345 0x53

//   Libraries (Library Manager):
//    - Adafruit SSD1306, Adafruit GFX
//    - Adafruit INA219
//    - Adafruit ADXL345, Adafruit Unified Sensor
//    - OneWire (Paul Stoffregen), DallasTemperature (Miles Burton)
//    - ESP_I2S is built into ESP32 Arduino core 3.x (use core 3.0.0 or newer)

//   NOTE: Arduino IDE needs the .ino inside a folder with the same name.
// */

// #include <Wire.h>
// #include <Adafruit_GFX.h>
// #include <Adafruit_SSD1306.h>
// #include <Adafruit_INA219.h>
// #include <Adafruit_Sensor.h>
// #include <Adafruit_ADXL345_U.h>
// #include <ESP_I2S.h>
// #include <OneWire.h>
// #include <DallasTemperature.h>
// #include <math.h>

// // ---------------- Pins ----------------
// #define I2C_SDA 8
// #define I2C_SCL 9

// // DS18B20 1-Wire data pin (all sensors on the same bus)
// #define DS18B20_PIN 1

// const uint8_t VOLT_PINS[3] = {4, 5, 6};

// #define OIL_PIN    7     // oil conductivity sensor (analog, ADC1)
// #define METAL_PIN  14    // metal detection sensor (digital)

// #define BUZZER_PIN 15    // buzzer output
// #define INSUL_PIN  16    // insulation test input (INPUT_PULLUP, connect to GND = OK)

// #define I2S_BCLK 12   // INMP441 SCK
// #define I2S_WS   11   // INMP441 WS
// #define I2S_DIN  13   // INMP441 SD   (INMP441 L/R pin -> GND = left channel)

// // ---------------- Slide settings ----------------
// #define SLIDE_COUNT        8
// #define SLIDE_INTERVAL_MS  3000    // time each screen stays visible

// // ---------------- ADC settling / sampling settings ----------------
// #define ADC_DUMMY_READS    12
// #define ADC_SETTLE_MS      10
// #define ADC_PARK_PIN       VOLT_PINS[0]

// #define VOLT_SAMPLES       40     // max 64
// #define OIL_SAMPLES        40     // max 64

// // ---------------- DS18B20 settings ----------------
// #define DS18B20_RESOLUTION 12      // 9..12 bit (12 bit = 750 ms conversion)
// #define DS18B20_CONV_MS    800     // wait time before reading a conversion
// #define DS18B20_MAX_FAILS  5       // show ERR only after this many failed reads in a row
// const float DS18B20_CAL[3] = {0.0, 0.0, 0.0};   // offset in degC per channel

// // ---------------- Voltage sensor calibration ----------------
// const float VOLT_RATIO = 5.0;
// const float VOLT_CAL[3] = {1.0, 1.0, 1.0};   // tune each tap against a multimeter (B1, B2, B+)

// // ---------------- Oil purity settings ----------------
// // Purity % = ADC raw (0..4095) mapped to 0..100 %
// #define OIL_ADC_MAX     4095.0
// #define OIL_INVERT      1        // 1 = purity = 100 - mapped value (if your sensor works reversed)
// const float OIL_GOOD_MIN = 70.0; // >= 70 %  -> GOOD
// const float OIL_FAIR_MIN = 40.0; // >= 40 %  -> FAIR, below -> POOR (alarm)

// // ---------------- Metal sensor settings ----------------
// #define METAL_DETECT_LEVEL  HIGH   // pin level that means "metal detected"
// // Use INPUT_PULLDOWN / INPUT_PULLUP only if your module needs it (e.g. open-collector output)
// #define METAL_PIN_MODE      INPUT

// // ---------------- Buzzer settings ----------------
// #define BUZZER_ON_LEVEL     HIGH   // use LOW if your buzzer module is active-LOW
// #define BUZZER_OFF_LEVEL    (!BUZZER_ON_LEVEL)

// // ---------------- Insulation test settings ----------------
// // Pin is INPUT_PULLUP. Connected to GND (LOW) = Insulation OK. Pin HIGH = NO Insulation.
// #define INSUL_OK_LEVEL      LOW

// // ---------------- Alarm / status thresholds ----------------
// // Voltage limits are per CELL (3S Li-ion). For LiFePO4 use about 3.65 / 2.5.
// const float VOLT_HIGH_LIMIT = 4.25;    // V  -> "HIGH" above this (cell overcharge)
// const float VOLT_LOW_LIMIT  = 3.00;    // V  -> "LOW" below this (cell over-discharge)
// const float TEMP_LIMIT_C    = 50.0;    // degC
// const float CURRENT_LIMIT_MA = 1000.0; // mA
// const float TILT_LIMIT_DEG  = 30.0;    // degrees from vertical
// const float VIB_LIMIT_MS2   = 2.0;     // RMS vibration m/s^2
// const float NOISE_LIMIT_DB  = 85.0;    // approx dB SPL

// // ---------------- Objects ----------------
// #define SCREEN_W 128
// #define SCREEN_H 64
// Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
// Adafruit_INA219 ina219(0x40);
// Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified(12345);
// I2SClass i2s;
// OneWire oneWire(DS18B20_PIN);
// DallasTemperature ds18b20(&oneWire);

// bool oledOK = false, inaOK = false, adxlOK = false, micOK = false;

// // ---------------- Running (moving) average ----------------
// #define AVG_WINDOW 8

// class MovingAvg {
//   float buf[AVG_WINDOW];
//   float sum = 0;
//   int idx = 0, count = 0;
// public:
//   float add(float v) {
//     if (count < AVG_WINDOW) {
//       buf[idx] = v; sum += v; count++;
//     } else {
//       sum -= buf[idx];
//       buf[idx] = v;
//       sum += v;
//     }
//     idx = (idx + 1) % AVG_WINDOW;
//     return sum / count;
//   }
// };

// MovingAvg tempAvg[3];
// MovingAvg voltAvg[3];
// MovingAvg oilAvg;

// // ---------------- Readings ----------------
// float tempC[3] = {0, 0, 0};
// bool  tempOK[3] = {false, false, false};   // false = no valid reading
// DeviceAddress tempAddr[3];                 // ROM address of each DS18B20
// uint8_t tempCount = 0;                     // sensors found on the bus
// uint8_t tempFail[3] = {0, 0, 0};           // consecutive failed reads

// float volts[3];          // CELL 1, 2, 3 voltages
// float tapV[3];           // cumulative tap voltages: B1, B2, B+
// float packV = 0;         // total pack voltage

// float busV = 0, current_mA = 0, power_mW = 0;
// float roll = 0, pitch = 0, tilt = 0, vib = 0;
// float noiseDb = 0;
// float oilRaw = 0, oilPurity = 0;
// bool  metalDetected = false;
// bool  insulationOK = false;

// // ---------------- Status strings ----------------
// const char *voltStatus[3];
// const char *tempStatus[3];
// const char *curStatus   = "NORMAL";
// const char *tiltStatus  = "OK";
// const char *vibStatus   = "OK";
// const char *noiseStatus = "NORMAL";
// const char *oilStatus   = "GOOD";
// const char *metalStatus = "NORMAL";
// const char *insulStatus = "OK";
// bool anyAlarm = false;

// // ---------------- ADC helpers ----------------
// void parkADC() {
//   for (int i = 0; i < 4; i++) analogReadMilliVolts(ADC_PARK_PIN);
// }

// // Settle on the channel, then trimmed mean.
// // rawCounts = false -> returns millivolts, true -> returns raw ADC counts (0..4095)
// float readSettled(uint8_t pin, int samples, bool rawCounts = false) {
//   if (samples > 64) samples = 64;

//   parkADC();
//   for (int i = 0; i < ADC_DUMMY_READS; i++) analogReadMilliVolts(pin);
//   delay(ADC_SETTLE_MS);
//   for (int i = 0; i < 4; i++) analogReadMilliVolts(pin);

//   uint32_t s[64];
//   for (int i = 0; i < samples; i++) {
//     s[i] = rawCounts ? (uint32_t)analogRead(pin) : analogReadMilliVolts(pin);
//     delayMicroseconds(500);
//   }

//   for (int i = 1; i < samples; i++) {
//     uint32_t key = s[i];
//     int j = i - 1;
//     while (j >= 0 && s[j] > key) { s[j + 1] = s[j]; j--; }
//     s[j + 1] = key;
//   }

//   int lo = samples / 5;
//   int hi = samples - lo;
//   uint32_t sum = 0;
//   for (int i = lo; i < hi; i++) sum += s[i];
//   return sum / (float)(hi - lo);
// }

// // DS18B20: non-blocking. A conversion is requested, and the result is read
// // on a later call once DS18B20_CONV_MS has passed (so the loop never stalls).
// // A failed read keeps the last good value; ERR is shown only after
// // DS18B20_MAX_FAILS failed reads in a row.
// void readTemps() {
//   static bool requested = false;
//   static uint32_t reqTime = 0;

//   if (!requested) {
//     ds18b20.requestTemperatures();
//     reqTime = millis();
//     requested = true;
//     return;
//   }

//   if (millis() - reqTime < DS18B20_CONV_MS) return;   // conversion not ready

//   for (int i = 0; i < 3; i++) {
//     if (i >= tempCount) { tempOK[i] = false; continue; }   // sensor not present

//     float t = ds18b20.getTempC(tempAddr[i]);
//     // 85.0 is the DS18B20 power-on default (conversion not valid), -127 = no reply
//     bool bad = (t == DEVICE_DISCONNECTED_C || t < -55.0 || t > 125.0 || t == 85.0);

//     if (bad) {
//       if (tempFail[i] < 255) tempFail[i]++;
//       if (tempFail[i] >= DS18B20_MAX_FAILS) tempOK[i] = false;
//       continue;                       // keep the last good value meanwhile
//     }

//     tempFail[i] = 0;
//     tempOK[i] = true;
//     tempC[i] = tempAvg[i].add(t + DS18B20_CAL[i]);
//   }

//   ds18b20.requestTemperatures();      // start next conversion
//   reqTime = millis();
// }

// // 3S battery pack: sensors read cumulative taps (B1, B2, B+),
// // cell voltage = difference between neighbouring taps.
// void readVoltages() {
//   for (int i = 0; i < 3; i++) {
//     float mv = readSettled(VOLT_PINS[i], VOLT_SAMPLES);
//     float raw = (mv / 1000.0) * VOLT_RATIO * VOLT_CAL[i];
//     tapV[i] = voltAvg[i].add(raw);          // smoothed tap voltage
//   }

//   volts[0] = tapV[0];                       // Cell 1 = B1
//   volts[1] = tapV[1] - tapV[0];             // Cell 2 = B2 - B1
//   volts[2] = tapV[2] - tapV[1];             // Cell 3 = B+ - B2
//   for (int i = 0; i < 3; i++) if (volts[i] < 0) volts[i] = 0;

//   packV = tapV[2];
// }

// // Oil purity: raw ADC 0..4095 -> 0..100 %
// void readOil() {
//   float raw = readSettled(OIL_PIN, OIL_SAMPLES, true);
//   oilRaw = raw;

//   float pct = (raw / OIL_ADC_MAX) * 100.0;
//   pct = constrain(pct, 0.0, 100.0);
// #if OIL_INVERT
//   pct = 100.0 - pct;
// #endif
//   oilPurity = oilAvg.add(pct);
// }

// // Metal sensor: majority vote of 5 quick reads (simple debounce)
// void readMetal() {
//   int hits = 0;
//   for (int i = 0; i < 5; i++) {
//     if (digitalRead(METAL_PIN) == METAL_DETECT_LEVEL) hits++;
//     delay(2);
//   }
//   metalDetected = (hits >= 3);
// }

// // Insulation test: majority vote of 5 quick reads (simple debounce)
// // LOW (pin connected to GND) = Insulation OK, HIGH = NO Insulation
// void readInsulation() {
//   int hits = 0;
//   for (int i = 0; i < 5; i++) {
//     if (digitalRead(INSUL_PIN) == INSUL_OK_LEVEL) hits++;
//     delay(2);
//   }
//   insulationOK = (hits >= 3);
// }

// // ---------------- Other sensors ----------------
// void readINA() {
//   if (!inaOK) return;
//   busV       = ina219.getBusVoltage_V();
//   current_mA = ina219.getCurrent_mA();
//   power_mW   = ina219.getPower_mW();
// }

// void readADXL() {
//   if (!adxlOK) return;
//   const int N = 32;
//   float xs[N], ys[N], zs[N];
//   float mx = 0, my = 0, mz = 0;

//   for (int i = 0; i < N; i++) {
//     sensors_event_t e;
//     adxl.getEvent(&e);
//     xs[i] = e.acceleration.x;
//     ys[i] = e.acceleration.y;
//     zs[i] = e.acceleration.z;
//     mx += xs[i]; my += ys[i]; mz += zs[i];
//     delay(3);
//   }
//   mx /= N; my /= N; mz /= N;

//   roll  = atan2(my, mz) * 180.0 / PI;
//   pitch = atan2(-mx, sqrt(my * my + mz * mz)) * 180.0 / PI;
//   float mag = sqrt(mx * mx + my * my + mz * mz);
//   tilt = (mag > 0.1) ? acos(constrain(mz / mag, -1.0, 1.0)) * 180.0 / PI : 0;

//   float acc = 0;
//   for (int i = 0; i < N; i++) {
//     float dx = xs[i] - mx, dy = ys[i] - my, dz = zs[i] - mz;
//     acc += dx * dx + dy * dy + dz * dz;
//   }
//   vib = sqrt(acc / N);
// }

// void readMic() {
//   if (!micOK) return;
//   const int SAMPLES = 512;
//   static int32_t buf[SAMPLES];

//   size_t bytes = i2s.readBytes((char *)buf, sizeof(buf));
//   int n = bytes / sizeof(int32_t);
//   if (n <= 0) return;

//   double mean = 0;
//   for (int i = 0; i < n; i++) { buf[i] >>= 8; mean += buf[i]; }
//   mean /= n;

//   double sumSq = 0;
//   for (int i = 0; i < n; i++) {
//     double s = buf[i] - mean;
//     sumSq += s * s;
//   }
//   double rms = sqrt(sumSq / n);
//   if (rms < 1) rms = 1;

//   double dbfs = 20.0 * log10(rms / 8388608.0);
//   noiseDb = (float)(dbfs + 120.0);
//   if (noiseDb < 0) noiseDb = 0;
// }

// // ---------------- Status evaluation ----------------
// void updateStatus() {
//   anyAlarm = false;

//   for (int i = 0; i < 3; i++) {
//     if (volts[i] > VOLT_HIGH_LIMIT)      { voltStatus[i] = "HIGH";  }
//     else if (volts[i] < VOLT_LOW_LIMIT)  { voltStatus[i] = "LOW";    }
//     else                                 { voltStatus[i] = "NORMAL"; }

//     if (!tempOK[i])                      { tempStatus[i] = "ERR";    }
//     else if (tempC[i] > TEMP_LIMIT_C)    { tempStatus[i] = "HIGH";   anyAlarm = true; }
//     else                                 { tempStatus[i] = "NORMAL"; }
//   }

//   if (inaOK && fabs(current_mA) > CURRENT_LIMIT_MA) { curStatus = "HIGH";  }
//   else                                              { curStatus = "NORMAL"; }

//   if (adxlOK && tilt > TILT_LIMIT_DEG) { tiltStatus = "TILT";  }
//   else                                 { tiltStatus = "OK"; }

//   if (adxlOK && vib > VIB_LIMIT_MS2)   { vibStatus = "HIGH"; }
//   else                                 { vibStatus = "OK"; }

//   if (micOK && noiseDb > NOISE_LIMIT_DB) { noiseStatus = "HIGH"; }
//   else                                   { noiseStatus = "NORMAL"; }

//   if (oilPurity >= OIL_GOOD_MIN)       { oilStatus = "GOOD"; }
//   else if (oilPurity >= OIL_FAIR_MIN)  { oilStatus = "FAIR"; }
//   else                                 { oilStatus = "POOR"; }

//   if (metalDetected) { metalStatus = "ALERT";  }
//   else               { metalStatus = "NORMAL"; }

//   if (insulationOK)  { insulStatus = "OK"; }
//   else               { insulStatus = "FAIL"; anyAlarm = true; }
// }

// // ---------------- Buzzer ----------------
// // Buzzer ON whenever any alert is active, OFF otherwise
// void updateBuzzer() {
//   digitalWrite(BUZZER_PIN, anyAlarm ? BUZZER_ON_LEVEL : BUZZER_OFF_LEVEL);
// }

// // ---------------- Display helpers ----------------
// void drawHeader(int slide, const char *title) {
//   display.fillRect(0, 0, SCREEN_W, 11, SSD1306_WHITE);
//   display.setTextColor(SSD1306_BLACK);
//   display.setTextSize(1);
//   display.setCursor(4, 2);
//   display.printf("%d/%d  %s", slide + 1, SLIDE_COUNT, title);
//   display.setTextColor(SSD1306_WHITE);
// }

// // Page dots (left) + alarm flag (right)
// void drawFooter(int slide) {
//   for (int i = 0; i < SLIDE_COUNT; i++) {
//     int x = 4 + i * 8;
//     if (i == slide) display.fillCircle(x, 60, 2, SSD1306_WHITE);
//     else            display.drawCircle(x, 60, 2, SSD1306_WHITE);
//   }
//   if (anyAlarm) {
//     display.setCursor(86, 56);
//     display.print("!ALARM");
//   }
// }

// void drawVoltageSlide(int s) {
//   drawHeader(s, "CELL VOLTAGE");
//   for (int i = 0; i < 3; i++) {
//     display.setCursor(0, 14 + i * 10);
//     display.printf("C%d:%5.2fV %s", i + 1, volts[i], voltStatus[i]);
//   }
//   display.setCursor(0, 44);
//   display.printf("Pack:%5.2fV", packV);
// }

// void drawTempSlide(int s) {
//   drawHeader(s, "TEMPERATURE");
//   for (int i = 0; i < 3; i++) {
//     display.setCursor(0, 15 + i * 12);
//     if (tempOK[i]) display.printf("T%d:%5.1fC %s", i + 1, tempC[i], tempStatus[i]);
//     else           display.printf("T%d:  ---  %s", i + 1, tempStatus[i]);
//   }
// }

// void drawCurrentSlide(int s) {
//   drawHeader(s, "CURRENT");
//   if (!inaOK) {
//     display.setCursor(0, 24);
//     display.print("INA219 not found");
//     return;
//   }
//   display.setCursor(0, 14); display.printf("Bus:%.2f V", busV);
//   display.setCursor(0, 24); display.printf("Cur:%.1f mA", current_mA);
//   display.setCursor(0, 34); display.printf("Pwr:%.0f mW", power_mW);
//   display.setCursor(0, 45); display.printf("Status: %s", curStatus);
// }

// void drawImuSlide(int s) {
//   drawHeader(s, "IMU DATA");
//   if (!adxlOK) {
//     display.setCursor(0, 24);
//     display.print("ADXL345 not found");
//     return;
//   }
//   display.setCursor(0, 14); display.printf("Roll:%.0f Pitch:%.0f", roll, pitch);
//   display.setCursor(0, 24); display.printf("Tilt:%.0f deg  %s", tilt, tiltStatus);
//   display.setCursor(0, 34); display.printf("Vib:%.2f m/s2", vib);
//   display.setCursor(0, 44); display.printf("Vib status: %s", vibStatus);
// }

// void drawNoiseSlide(int s) {
//   drawHeader(s, "NOISE LEVEL");
//   if (!micOK) {
//     display.setCursor(0, 24);
//     display.print("INMP441 error");
//     return;
//   }
//   display.setTextSize(2);
//   display.setCursor(0, 15);
//   display.printf("%.0f dB", noiseDb);
//   display.setTextSize(1);

//   display.setCursor(80, 20);
//   display.print(noiseStatus);

//   // Bar graph 0..120 dB with a marker at the alarm limit
//   int w = (int)constrain(noiseDb / 120.0 * 126.0, 0, 126);
//   display.drawRect(0, 36, 128, 9, SSD1306_WHITE);
//   display.fillRect(1, 37, w, 7, SSD1306_WHITE);
//   int lim = (int)(NOISE_LIMIT_DB / 120.0 * 126.0) + 1;
//   display.drawFastVLine(lim, 33, 15, SSD1306_WHITE);
// }

// void drawOilSlide(int s) {
//   drawHeader(s, "OIL PURITY");

//   display.setTextSize(2);
//   display.setCursor(0, 14);
//   display.printf("%.1f%%", oilPurity);
//   display.setTextSize(1);

//   display.setCursor(88, 18);
//   display.print(oilStatus);

//   // Bar graph 0..100 %
//   int w = (int)constrain(oilPurity / 100.0 * 126.0, 0, 126);
//   display.drawRect(0, 33, 128, 9, SSD1306_WHITE);
//   display.fillRect(1, 34, w, 7, SSD1306_WHITE);

//   display.setCursor(0, 45);
//   display.printf("ADC:%4.0f / 4095", oilRaw);
// }

// void drawMetalSlide(int s) {
//   drawHeader(s, "METAL DETECTION");

//   // Banner: filled = detected, outlined = not detected
//   if (metalDetected) {
//     display.fillRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setTextColor(SSD1306_BLACK);
//     display.setCursor(17, 21);
//     display.print("METAL DETECTED");
//     display.setTextColor(SSD1306_WHITE);
//   } else {
//     display.drawRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setCursor(7, 21);
//     display.print("METAL NOT-DETECTED");
//   }

//   display.setCursor(0, 40);
//   display.printf("Pin: %s", digitalRead(METAL_PIN) ? "HIGH" : "LOW");
//   display.setCursor(0, 49);
//   display.printf("Status: %s", metalStatus);
// }

// void drawInsulationSlide(int s) {
//   drawHeader(s, "INSULATION TEST");

//   // Banner: outlined = OK, filled = NO Insulation
//   if (insulationOK) {
//     display.drawRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setCursor(22, 21);
//     display.print("Insulation OK");
//   } else {
//     display.fillRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setTextColor(SSD1306_BLACK);
//     display.setCursor(22, 21);
//     display.print("NO Insulation");
//     display.setTextColor(SSD1306_WHITE);
//   }

//   display.setCursor(0, 40);
//   display.printf("Pin: %s", digitalRead(INSUL_PIN) ? "HIGH" : "LOW");
//   display.setCursor(0, 49);
//   display.printf("Status: %s", insulStatus);
// }

// void updateDisplay() {
//   if (!oledOK) return;

//   static int slide = 0;
//   static uint32_t lastSlide = 0;
//   if (millis() - lastSlide >= SLIDE_INTERVAL_MS) {
//     lastSlide = millis();
//     slide = (slide + 1) % SLIDE_COUNT;
//   }

//   display.clearDisplay();
//   display.setTextSize(1);
//   display.setTextColor(SSD1306_WHITE);

//   switch (slide) {
//     case 0: drawVoltageSlide(slide);    break;
//     case 1: drawTempSlide(slide);       break;
//     case 2: drawCurrentSlide(slide);    break;
//     case 3: drawImuSlide(slide);        break;
//     case 4: drawNoiseSlide(slide);      break;
//     case 5: drawOilSlide(slide);        break;
//     case 6: drawMetalSlide(slide);      break;
//     case 7: drawInsulationSlide(slide); break;
//   }

//   drawFooter(slide);
//   display.display();
// }

// void printSerial() {
//   for (int i = 0; i < 3; i++) {
//     if (tempOK[i]) Serial.printf("T%d=%.1fC ", i + 1, tempC[i]);
//     else           Serial.printf("T%d=ERR ", i + 1);
//   }
//   Serial.printf("| Cells: %.2f %.2f %.2f V (Pack %.2f V) | "
//                 "Bus: %.2f V, I: %.1f mA, P: %.0f mW | Roll: %.1f Pitch: %.1f Tilt: %.1f | "
//                 "Vib: %.2f m/s2 | Noise: %.1f dB | Oil: %.1f %% (ADC %.0f) | Metal: %s | "
//                 "Insulation: %s | Buzzer: %s\n",
//                 volts[0], volts[1], volts[2], packV,
//                 busV, current_mA, power_mW,
//                 roll, pitch, tilt, vib, noiseDb,
//                 oilPurity, oilRaw,
//                 metalDetected ? "DETECTED" : "NOT-DETECTED",
//                 insulationOK ? "OK" : "NO INSULATION",
//                 anyAlarm ? "ON" : "OFF");
// }

// // ---------------- Setup / Loop ----------------
// void setup() {
//   Serial.begin(115200);
//   delay(300);

//   // Buzzer: make sure it is OFF at boot
//   pinMode(BUZZER_PIN, OUTPUT);
//   digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);

//   Wire.begin(I2C_SDA, I2C_SCL);
//   Wire.setClock(400000);

//   analogReadResolution(12);
//   for (int i = 0; i < 3; i++) {
//     analogSetPinAttenuation(VOLT_PINS[i], ADC_11db);
//   }
//   analogSetPinAttenuation(OIL_PIN, ADC_11db);

//   pinMode(METAL_PIN, METAL_PIN_MODE);
//   pinMode(INSUL_PIN, INPUT_PULLUP);

//   // DS18B20 init (non-blocking conversions, ROM addressed)
//   ds18b20.begin();
//   ds18b20.setResolution(DS18B20_RESOLUTION);
//   ds18b20.setWaitForConversion(false);

//   tempCount = ds18b20.getDeviceCount();
//   if (tempCount > 3) tempCount = 3;
//   for (uint8_t i = 0; i < tempCount; i++) {
//     if (!ds18b20.getAddress(tempAddr[i], i)) {
//       tempCount = i;                  // stop at the first address we can't read
//       break;
//     }
//   }
//   Serial.printf("DS18B20 sensors found: %d\n", tempCount);

//   for (int i = 0; i < 3; i++) { voltStatus[i] = "NORMAL"; tempStatus[i] = "NORMAL"; }

//   oledOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
//   if (!oledOK) Serial.println("OLED not found!");
//   else {
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);
//     display.println("ESP32-S3 Monitor");
//     display.println("Initializing...");
//     display.display();
//   }

//   inaOK = ina219.begin();
//   if (!inaOK) Serial.println("INA219 not found!");

//   adxlOK = adxl.begin(0x53);
//   if (!adxlOK) Serial.println("ADXL345 not found!");
//   else {
//     adxl.setRange(ADXL345_RANGE_4_G);
//     adxl.setDataRate(ADXL345_DATARATE_800_HZ);
//   }

//   i2s.setPins(I2S_BCLK, I2S_WS, -1, I2S_DIN);
//   micOK = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
//                     I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
//   if (!micOK) Serial.println("INMP441 I2S init failed!");

//   delay(800);
// }

// void loop() {
//   readTemps();
//   readVoltages();
//   readOil();
//   readMetal();
//   readInsulation();
//   readINA();
//   readADXL();
//   readMic();

//   updateStatus();
//   updateBuzzer();
//   updateDisplay();
//   printSerial();

//   delay(100);
// }


// /*
//   ESP32-S3 Multi-Sensor Monitor -> 0.96" OLED (SSD1306 128x64, I2C)
//   SLIDE DISPLAY VERSION 3  (DS18B20 + buzzer + insulation test)

//   Screens (auto-rotate every SLIDE_INTERVAL_MS):
//     1. VOLTAGE    : V1, V2, V3                    (NORMAL / HIGH / LOW)
//     2. TEMPERATURE: T1, T2, T3  (DS18B20)         (NORMAL / HIGH / ERR)
//     3. CURRENT    : INA219 bus V, mA, mW          (NORMAL / HIGH)
//     4. IMU        : roll, pitch, tilt, vibration  (OK / TILT, OK / HIGH)
//     5. NOISE      : dB level + bar graph          (NORMAL / HIGH)
//     6. OIL PURITY : ADC 0-4095 -> 0-100 %         (GOOD / FAIR / POOR)
//     7. METAL      : digital input                 (HIGH = METAL DETECTED,
//                                                    LOW  = METAL NOT-DETECTED)
//     8. INSULATION : input pullup, pin to GND      (LOW  = Insulation OK,
//                                                    HIGH = NO Insulation)   <-- NEW

//   Sensors / pins:
//    - 3x DS18B20 temperature (1-Wire bus, GPIO1)  <-- CHANGED (was LM35)
//         All 3 sensors share ONE data wire on GPIO1 with a 4.7k pull-up to 3.3V
//    - 3x Arduino voltage sensor      (ADC1: GPIO4, GPIO5, GPIO6)  [0-25V module, 5:1 divider]
//    - Oil conductivity (analog)      (ADC1: GPIO7)
//    - Metal detection sensor (digital) (GPIO14)
//    - Buzzer output (active buzzer)  (GPIO15)     <-- NEW
//    - Insulation test input          (GPIO16)     <-- NEW (INPUT_PULLUP)
//    - INA219 current/bus voltage     (I2C 0x40)
//    - ADXL345 tilt + vibration       (I2C 0x53)
//    - INMP441 I2S MEMS microphone    (BCLK GPIO12, WS GPIO11, SD GPIO13)

//   I2C bus (shared): SDA = GPIO8, SCL = GPIO9
//     OLED 0x3C, INA219 0x40, ADXL345 0x53

//   Libraries (Library Manager):
//    - Adafruit SSD1306, Adafruit GFX
//    - Adafruit INA219
//    - Adafruit ADXL345, Adafruit Unified Sensor
//    - OneWire (Paul Stoffregen), DallasTemperature (Miles Burton)   <-- NEW
//    - ESP_I2S is built into ESP32 Arduino core 3.x (use core 3.0.0 or newer)

//   NOTE: Arduino IDE needs the .ino inside a folder with the same name.
// */

// #include <Wire.h>
// #include <Adafruit_GFX.h>
// #include <Adafruit_SSD1306.h>
// #include <Adafruit_INA219.h>
// #include <Adafruit_Sensor.h>
// #include <Adafruit_ADXL345_U.h>
// #include <ESP_I2S.h>
// #include <OneWire.h>
// #include <DallasTemperature.h>
// #include <math.h>

// // ---------------- Pins ----------------
// #define I2C_SDA 8
// #define I2C_SCL 9

// // DS18B20 1-Wire data pin (all sensors on the same bus)
// #define DS18B20_PIN 1

// const uint8_t VOLT_PINS[3] = {4, 5, 6};

// #define OIL_PIN    7     // oil conductivity sensor (analog, ADC1)
// #define METAL_PIN  14    // metal detection sensor (digital)

// #define BUZZER_PIN 15    // buzzer output
// #define INSUL_PIN  16    // insulation test input (INPUT_PULLUP, connect to GND = OK)

// #define I2S_BCLK 12   // INMP441 SCK
// #define I2S_WS   11   // INMP441 WS
// #define I2S_DIN  13   // INMP441 SD   (INMP441 L/R pin -> GND = left channel)

// // ---------------- Slide settings ----------------
// #define SLIDE_COUNT        8
// #define SLIDE_INTERVAL_MS  5000    // time each screen stays visible

// // ---------------- ADC settling / sampling settings ----------------
// #define ADC_DUMMY_READS    12
// #define ADC_SETTLE_MS      10
// #define ADC_PARK_PIN       VOLT_PINS[0]

// #define VOLT_SAMPLES       40     // max 64
// #define OIL_SAMPLES        40     // max 64

// // ---------------- DS18B20 settings ----------------
// #define DS18B20_RESOLUTION 12      // 9..12 bit (12 bit = 750 ms conversion)
// #define DS18B20_CONV_MS    800     // wait time before reading a conversion
// const float DS18B20_CAL[3] = {0.0, 0.0, 0.0};   // offset in degC per channel

// // ---------------- Voltage sensor calibration ----------------
// const float VOLT_RATIO = 5.0;
// const float VOLT_CAL[3] = {1.0, 1.0, 1.0};   // tune each tap against a multimeter

// // ---------------- Alarm / status thresholds (per CELL) ----------------
// const float VOLT_HIGH_LIMIT = 4.25;    // V -> "HIGH" above this (cell overcharge)
// const float VOLT_LOW_LIMIT  = 3.00;    // V -> "LOW" below this (cell over-discharge)

// float tapV[3];       // cumulative tap voltages: B1, B2, B+
// float packV = 0;     // total pack voltage
// // volts[0..2] now hold the CELL 1, 2, 3 voltages

// // ---------------- Oil purity settings ----------------
// // Purity % = ADC raw (0..4095) mapped to 0..100 %
// #define OIL_ADC_MAX     4095.0
// #define OIL_INVERT      1        // 1 = purity = 100 - mapped value (if your sensor works reversed)
// const float OIL_GOOD_MIN = 70.0; // >= 70 %  -> GOOD
// const float OIL_FAIR_MIN = 40.0; // >= 40 %  -> FAIR, below -> POOR (alarm)

// // ---------------- Metal sensor settings ----------------
// #define METAL_DETECT_LEVEL  HIGH   // pin level that means "metal detected"
// // Use INPUT_PULLDOWN / INPUT_PULLUP only if your module needs it (e.g. open-collector output)
// #define METAL_PIN_MODE      INPUT

// // ---------------- Buzzer settings ----------------
// #define BUZZER_ON_LEVEL     HIGH   // use LOW if your buzzer module is active-LOW
// #define BUZZER_OFF_LEVEL    (!BUZZER_ON_LEVEL)

// // ---------------- Insulation test settings ----------------
// // Pin is INPUT_PULLUP. Connected to GND (LOW) = Insulation OK. Pin HIGH = NO Insulation.
// #define INSUL_OK_LEVEL      LOW

// // ---------------- Alarm / status thresholds ----------------
// // const float VOLT_HIGH_LIMIT = 15.0;    // V  -> "HIGH" above this
// // const float VOLT_LOW_LIMIT  = 0.5;     // V  -> "LOW" below this (set 0.0 to disable)
// const float TEMP_LIMIT_C    = 50.0;    // degC
// const float CURRENT_LIMIT_MA = 1000.0; // mA
// const float TILT_LIMIT_DEG  = 30.0;    // degrees from vertical
// const float VIB_LIMIT_MS2   = 2.0;     // RMS vibration m/s^2
// const float NOISE_LIMIT_DB  = 70.0;    // approx dB SPL

// // ---------------- Objects ----------------
// #define SCREEN_W 128
// #define SCREEN_H 64
// Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
// Adafruit_INA219 ina219(0x40);
// Adafruit_ADXL345_Unified adxl = Adafruit_ADXL345_Unified(12345);
// I2SClass i2s;
// OneWire oneWire(DS18B20_PIN);
// DallasTemperature ds18b20(&oneWire);

// bool oledOK = false, inaOK = false, adxlOK = false, micOK = false;

// // ---------------- Running (moving) average ----------------
// #define AVG_WINDOW 8

// class MovingAvg {
//   float buf[AVG_WINDOW];
//   float sum = 0;
//   int idx = 0, count = 0;
// public:
//   float add(float v) {
//     if (count < AVG_WINDOW) {
//       buf[idx] = v; sum += v; count++;
//     } else {
//       sum -= buf[idx];
//       buf[idx] = v;
//       sum += v;
//     }
//     idx = (idx + 1) % AVG_WINDOW;
//     return sum / count;
//   }
// };

// MovingAvg tempAvg[3];
// MovingAvg voltAvg[3];
// MovingAvg oilAvg;

// // ---------------- Readings ----------------
// float tempC[3] = {0, 0, 0};
// bool  tempOK[3] = {false, false, false};   // false = DS18B20 not found / read error
// float volts[3];
// float busV = 0, current_mA = 0, power_mW = 0;
// float roll = 0, pitch = 0, tilt = 0, vib = 0;
// float noiseDb = 0;
// float oilRaw = 0, oilPurity = 0;
// bool  metalDetected = false;
// bool  insulationOK = false;

// // ---------------- Status strings ----------------
// const char *voltStatus[3];
// const char *tempStatus[3];
// const char *curStatus   = "NORMAL";
// const char *tiltStatus  = "OK";
// const char *vibStatus   = "OK";
// const char *noiseStatus = "NORMAL";
// const char *oilStatus   = "GOOD";
// const char *metalStatus = "NORMAL";
// const char *insulStatus = "OK";
// bool anyAlarm = false;

// // ---------------- ADC helpers ----------------
// void parkADC() {
//   for (int i = 0; i < 4; i++) analogReadMilliVolts(ADC_PARK_PIN);
// }

// // Settle on the channel, then trimmed mean.
// // rawCounts = false -> returns millivolts, true -> returns raw ADC counts (0..4095)
// float readSettled(uint8_t pin, int samples, bool rawCounts = false) {
//   if (samples > 64) samples = 64;

//   parkADC();
//   for (int i = 0; i < ADC_DUMMY_READS; i++) analogReadMilliVolts(pin);
//   delay(ADC_SETTLE_MS);
//   for (int i = 0; i < 4; i++) analogReadMilliVolts(pin);

//   uint32_t s[64];
//   for (int i = 0; i < samples; i++) {
//     s[i] = rawCounts ? (uint32_t)analogRead(pin) : analogReadMilliVolts(pin);
//     delayMicroseconds(500);
//   }

//   for (int i = 1; i < samples; i++) {
//     uint32_t key = s[i];
//     int j = i - 1;
//     while (j >= 0 && s[j] > key) { s[j + 1] = s[j]; j--; }
//     s[j + 1] = key;
//   }

//   int lo = samples / 5;
//   int hi = samples - lo;
//   uint32_t sum = 0;
//   for (int i = lo; i < hi; i++) sum += s[i];
//   return sum / (float)(hi - lo);
// }

// // DS18B20: non-blocking. A conversion is requested, and the result is read
// // on a later call once DS18B20_CONV_MS has passed (so the loop never stalls).
// void readTemps() {
//   static bool requested = false;
//   static uint32_t reqTime = 0;

//   if (!requested) {
//     ds18b20.requestTemperatures();
//     reqTime = millis();
//     requested = true;
//     return;
//   }

//   if (millis() - reqTime < DS18B20_CONV_MS) return;   // conversion not ready yet

//   for (int i = 0; i < 3; i++) {
//     float t = ds18b20.getTempCByIndex(i);
//     if (t == DEVICE_DISCONNECTED_C || t < -55.0 || t > 125.0) {
//       tempOK[i] = false;
//       continue;
//     }
//     tempOK[i] = true;
//     t += DS18B20_CAL[i];
//     tempC[i] = tempAvg[i].add(t);
//   }

//   ds18b20.requestTemperatures();   // start next conversion
//   reqTime = millis();
// }

// void readVoltages() {
//   for (int i = 0; i < 3; i++) {
//     float mv = readSettled(VOLT_PINS[i], VOLT_SAMPLES);
//     float raw = (mv / 1000.0) * VOLT_RATIO * VOLT_CAL[i];
//     tapV[i] = voltAvg[i].add(raw);          // smoothed tap voltage
//   }

//   // Cell voltage = difference between neighbouring taps
//   volts[0] = tapV[0];                       // Cell 1 = B1
//   volts[1] = tapV[1] - tapV[0];             // Cell 2 = B2 - B1
//   volts[2] = tapV[2] - tapV[1];             // Cell 3 = B+ - B2
//   for (int i = 0; i < 3; i++) if (volts[i] < 0) volts[i] = 0;

//   packV = tapV[2];
// }

// // Oil purity: raw ADC 0..4095 -> 0..100 %
// void readOil() {
//   float raw = readSettled(OIL_PIN, OIL_SAMPLES, true);
//   oilRaw = raw;

//   float pct = (raw / OIL_ADC_MAX) * 100.0;
//   pct = constrain(pct, 0.0, 100.0);
// #if OIL_INVERT
//   pct = 100.0 - pct;
// #endif
//   oilPurity = oilAvg.add(pct);
// }

// // Metal sensor: majority vote of 5 quick reads (simple debounce)
// void readMetal() {
//   int hits = 0;
//   for (int i = 0; i < 5; i++) {
//     if (digitalRead(METAL_PIN) == METAL_DETECT_LEVEL) hits++;
//     delay(2);
//   }
//   metalDetected = (hits >= 3);
// }

// // Insulation test: majority vote of 5 quick reads (simple debounce)
// // LOW (pin connected to GND) = Insulation OK, HIGH = NO Insulation
// void readInsulation() {
//   int hits = 0;
//   for (int i = 0; i < 5; i++) {
//     if (digitalRead(INSUL_PIN) == INSUL_OK_LEVEL) hits++;
//     delay(2);
//   }
//   insulationOK = (hits >= 3);
// }

// // ---------------- Other sensors ----------------
// void readINA() {
//   if (!inaOK) return;
//   busV       = ina219.getBusVoltage_V();
//   current_mA = ina219.getCurrent_mA();
//   power_mW   = ina219.getPower_mW();
// }

// void readADXL() {
//   if (!adxlOK) return;
//   const int N = 32;
//   float xs[N], ys[N], zs[N];
//   float mx = 0, my = 0, mz = 0;

//   for (int i = 0; i < N; i++) {
//     sensors_event_t e;
//     adxl.getEvent(&e);
//     xs[i] = e.acceleration.x;
//     ys[i] = e.acceleration.y;
//     zs[i] = e.acceleration.z;
//     mx += xs[i]; my += ys[i]; mz += zs[i];
//     delay(3);
//   }
//   mx /= N; my /= N; mz /= N;

//   roll  = atan2(my, mz) * 180.0 / PI;
//   pitch = atan2(-mx, sqrt(my * my + mz * mz)) * 180.0 / PI;
//   float mag = sqrt(mx * mx + my * my + mz * mz);
//   tilt = (mag > 0.1) ? acos(constrain(mz / mag, -1.0, 1.0)) * 180.0 / PI : 0;

//   float acc = 0;
//   for (int i = 0; i < N; i++) {
//     float dx = xs[i] - mx, dy = ys[i] - my, dz = zs[i] - mz;
//     acc += dx * dx + dy * dy + dz * dz;
//   }
//   vib = sqrt(acc / N);
// }

// void readMic() {
//   if (!micOK) return;
//   const int SAMPLES = 255;  //before 512
//   static int32_t buf[SAMPLES];

//   size_t bytes = i2s.readBytes((char *)buf, sizeof(buf));
//   int n = bytes / sizeof(int32_t);
//   if (n <= 0) return;

//   double mean = 0;
//   for (int i = 0; i < n; i++) { buf[i] >>= 8; mean += buf[i]; }
//   mean /= n;

//   double sumSq = 0;
//   for (int i = 0; i < n; i++) {
//     double s = buf[i] - mean;
//     sumSq += s * s;
//   }
//   double rms = sqrt(sumSq / n);
//   if (rms < 1) rms = 1;

//   double dbfs = 20.0 * log10(rms / 8388608.0);
//   noiseDb = (float)(dbfs + 120.0);
//   if (noiseDb < 0) noiseDb = 0;
// }

// // ---------------- Status evaluation ----------------
// void updateStatus() {
//   anyAlarm = false;

//   for (int i = 0; i < 3; i++) {
//     if (volts[i] > VOLT_HIGH_LIMIT)      { voltStatus[i] = "HIGH";   }
//     else if (volts[i] < VOLT_LOW_LIMIT)  { voltStatus[i] = "LOW";    }
//     else                                 { voltStatus[i] = "NORMAL"; }

//     if (!tempOK[i])                      { tempStatus[i] = "ERR";    }
//     else if (tempC[i] > TEMP_LIMIT_C)    { tempStatus[i] = "HIGH";   anyAlarm = true; }
//     else                                 { tempStatus[i] = "NORMAL"; }
//   }

//   if (inaOK && fabs(current_mA) > CURRENT_LIMIT_MA) { curStatus = "HIGH";  }
//   else                                              { curStatus = "NORMAL"; }

//   if (adxlOK && tilt > TILT_LIMIT_DEG) { tiltStatus = "TILT"; anyAlarm = true; }
//   else                                 { tiltStatus = "OK"; }

//   if (adxlOK && vib > VIB_LIMIT_MS2)   { vibStatus = "HIGH";  anyAlarm = true; }
//   else                                 { vibStatus = "OK"; }

//   if (micOK && noiseDb > NOISE_LIMIT_DB) { noiseStatus = "HIGH";  }
//   else                                   { noiseStatus = "NORMAL"; }

//   if (oilPurity >= OIL_GOOD_MIN)       { oilStatus = "GOOD"; }
//   else if (oilPurity >= OIL_FAIR_MIN)  { oilStatus = "FAIR"; }
//   else                                 { oilStatus = "POOR"; }

//   if (metalDetected) { metalStatus = "ALERT";  }
//   else               { metalStatus = "NORMAL"; }

//   if (insulationOK)  { insulStatus = "OK"; }
//   else               { insulStatus = "FAIL";}
// }

// // ---------------- Buzzer ----------------
// // Buzzer ON whenever any alert is active, OFF otherwise
// void updateBuzzer() {
//   digitalWrite(BUZZER_PIN, anyAlarm ? BUZZER_ON_LEVEL : BUZZER_OFF_LEVEL);
// }

// // ---------------- Display helpers ----------------
// void drawHeader(int slide, const char *title) {
//   display.fillRect(0, 0, SCREEN_W, 11, SSD1306_WHITE);
//   display.setTextColor(SSD1306_BLACK);
//   display.setTextSize(1);
//   display.setCursor(4, 2);
//   display.printf("%d/%d  %s", slide + 1, SLIDE_COUNT, title);
//   display.setTextColor(SSD1306_WHITE);
// }

// // Page dots (left) + alarm flag (right)
// void drawFooter(int slide) {
//   for (int i = 0; i < SLIDE_COUNT; i++) {
//     int x = 4 + i * 8;
//     if (i == slide) display.fillCircle(x, 60, 2, SSD1306_WHITE);
//     else            display.drawCircle(x, 60, 2, SSD1306_WHITE);
//   }
//   if (anyAlarm) {
//     display.setCursor(86, 56);
//     display.print("!ALARM");
//   }
// }

// void drawVoltageSlide(int s) {
//   drawHeader(s, "CELL VOLTAGE");
//   for (int i = 0; i < 3; i++) {
//     display.setCursor(0, 15 + i * 12);
//     display.printf("C%d:%5.2fV %s", i + 1, volts[i], voltStatus[i]);
//   }
// }

// void drawTempSlide(int s) {
//   drawHeader(s, "TEMPERATURE");
//   for (int i = 0; i < 3; i++) {
//     display.setCursor(0, 15 + i * 12);
//     if (tempOK[i]) display.printf("T%d:%5.1fC %s", i + 1, tempC[i], tempStatus[i]);
//     else           display.printf("T%d:  ---  %s", i + 1, tempStatus[i]);
//   }
// }

// void drawCurrentSlide(int s) {
//   drawHeader(s, "CURRENT");
//   if (!inaOK) {
//     display.setCursor(0, 24);
//     display.print("INA219 not found");
//     return;
//   }
//   display.setCursor(0, 14); display.printf("Bus:%.2f V", busV);
//   display.setCursor(0, 24); display.printf("Cur:%.1f mA", current_mA);
//   display.setCursor(0, 34); display.printf("Pwr:%.0f mW", power_mW);
//   display.setCursor(0, 45); display.printf("Status: %s", curStatus);
// }

// void drawImuSlide(int s) {
//   drawHeader(s, "IMU DATA");
//   if (!adxlOK) {
//     display.setCursor(0, 24);
//     display.print("ADXL345 not found");
//     return;
//   }
//   display.setCursor(0, 14); display.printf("Roll:%.0f Pitch:%.0f", roll, pitch);
//   display.setCursor(0, 24); display.printf("Tilt:%.0f deg  %s", tilt, tiltStatus);
//   display.setCursor(0, 34); display.printf("Vib:%.2f m/s2", vib);
//   display.setCursor(0, 44); display.printf("Vib status: %s", vibStatus);
// }

// void drawNoiseSlide(int s) {
//   drawHeader(s, "NOISE LEVEL");
//   if (!micOK) {
//     display.setCursor(0, 24);
//     display.print("INMP441 error");
//     return;
//   }
//   display.setTextSize(2);
//   display.setCursor(0, 15);
//   display.printf("%.0f dB", noiseDb);
//   display.setTextSize(1);

//   display.setCursor(80, 20);
//   display.print(noiseStatus);

//   // Bar graph 0..120 dB with a marker at the alarm limit
//   int w = (int)constrain(noiseDb / 120.0 * 126.0, 0, 126);
//   display.drawRect(0, 36, 128, 9, SSD1306_WHITE);
//   display.fillRect(1, 37, w, 7, SSD1306_WHITE);
//   int lim = (int)(NOISE_LIMIT_DB / 120.0 * 126.0) + 1;
//   display.drawFastVLine(lim, 33, 15, SSD1306_WHITE);
// }

// void drawOilSlide(int s) {
//   drawHeader(s, "OIL PURITY");

//   display.setTextSize(2);
//   display.setCursor(0, 14);
//   display.printf("%.1f%%", oilPurity);
//   display.setTextSize(1);

//   display.setCursor(88, 18);
//   display.print(oilStatus);

//   // Bar graph 0..100 %
//   int w = (int)constrain(oilPurity / 100.0 * 126.0, 0, 126);
//   display.drawRect(0, 33, 128, 9, SSD1306_WHITE);
//   display.fillRect(1, 34, w, 7, SSD1306_WHITE);

//   display.setCursor(0, 45);
//   display.printf("ADC:%4.0f / 4095", oilRaw);
// }

// void drawMetalSlide(int s) {
//   drawHeader(s, "METAL DETECTION");

//   // Banner: filled = detected, outlined = not detected
//   if (metalDetected) {
//     display.fillRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setTextColor(SSD1306_BLACK);
//     display.setCursor(17, 21);
//     display.print("METAL DETECTED");
//     display.setTextColor(SSD1306_WHITE);
//   } else {
//     display.drawRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setCursor(7, 21);
//     display.print("METAL NOT-DETECTED");
//   }

//   display.setCursor(0, 40);
//   display.printf("Pin: %s", digitalRead(METAL_PIN) ? "HIGH" : "LOW");
//   display.setCursor(0, 49);
//   display.printf("Status: %s", metalStatus);
// }

// void drawInsulationSlide(int s) {
//   drawHeader(s, "INSULATION TEST");

//   // Banner: outlined = OK, filled = NO Insulation
//   if (insulationOK) {
//     display.drawRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setCursor(22, 21);
//     display.print("Insulation OK");
//   } else {
//     display.fillRect(0, 16, 128, 18, SSD1306_WHITE);
//     display.setTextColor(SSD1306_BLACK);
//     display.setCursor(22, 21);
//     display.print("NO Insulation");
//     display.setTextColor(SSD1306_WHITE);
//   }

//   display.setCursor(0, 40);
//   display.printf("Pin: %s", digitalRead(INSUL_PIN) ? "HIGH" : "LOW");
//   display.setCursor(0, 49);
//   display.printf("Status: %s", insulStatus);
// }

// void updateDisplay() {
//   if (!oledOK) return;

//   static int slide = 0;
//   static uint32_t lastSlide = 0;
//   if (millis() - lastSlide >= SLIDE_INTERVAL_MS) {
//     lastSlide = millis();
//     slide = (slide + 1) % SLIDE_COUNT;
//   }

//   display.clearDisplay();
//   display.setTextSize(1);
//   display.setTextColor(SSD1306_WHITE);

//   switch (slide) {
//     case 0: drawVoltageSlide(slide);    break;
//     case 1: drawTempSlide(slide);       break;
//     case 2: drawCurrentSlide(slide);    break;
//     case 3: drawImuSlide(slide);        break;
//     case 4: drawNoiseSlide(slide);      break;
//     case 5: drawOilSlide(slide);        break;
//     case 6: drawMetalSlide(slide);      break;
//     case 7: drawInsulationSlide(slide); break;
//   }

//   drawFooter(slide);
//   display.display();
// }

// void printSerial() {
//   Serial.printf("DS18B20: %.1f %.1f %.1f C | V: %.2f %.2f %.2f V | "
//                 "Bus: %.2f V, I: %.1f mA, P: %.0f mW | Roll: %.1f Pitch: %.1f Tilt: %.1f | "
//                 "Vib: %.2f m/s2 | Noise: %.1f dB | Oil: %.1f %% (ADC %.0f) | Metal: %s | "
//                 "Insulation: %s | Buzzer: %s\n",
//                 tempC[0], tempC[1], tempC[2],
//                 volts[0], volts[1], volts[2],
//                 busV, current_mA, power_mW,
//                 roll, pitch, tilt, vib, noiseDb,
//                 oilPurity, oilRaw,
//                 metalDetected ? "DETECTED" : "NOT-DETECTED",
//                 insulationOK ? "OK" : "NO INSULATION",
//                 anyAlarm ? "ON" : "OFF");
// }

// // ---------------- Setup / Loop ----------------
// void setup() {
//   Serial.begin(115200);
//   delay(300);

//   // Buzzer: make sure it is OFF at boot
//   pinMode(BUZZER_PIN, OUTPUT);
//   digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);

//   Wire.begin(I2C_SDA, I2C_SCL);
//   Wire.setClock(400000);

//   analogReadResolution(12);
//   for (int i = 0; i < 3; i++) {
//     analogSetPinAttenuation(VOLT_PINS[i], ADC_11db);
//   }
//   analogSetPinAttenuation(OIL_PIN, ADC_11db);

//   pinMode(METAL_PIN, METAL_PIN_MODE);
//   pinMode(INSUL_PIN, INPUT_PULLUP);

//   // DS18B20 init (non-blocking conversions)
//   ds18b20.begin();
//   ds18b20.setResolution(DS18B20_RESOLUTION);
//   ds18b20.setWaitForConversion(false);
//   Serial.printf("DS18B20 sensors found: %d\n", ds18b20.getDeviceCount());

//   for (int i = 0; i < 3; i++) { voltStatus[i] = "NORMAL"; tempStatus[i] = "NORMAL"; }

//   oledOK = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
//   if (!oledOK) Serial.println("OLED not found!");
//   else {
//     display.clearDisplay();
//     display.setTextSize(1);
//     display.setTextColor(SSD1306_WHITE);
//     display.setCursor(0, 0);
//     display.println("ESP32-S3 Monitor");
//     display.println("Initializing...");
//     display.display();
//   }

//   inaOK = ina219.begin();
//   if (!inaOK) Serial.println("INA219 not found!");

//   adxlOK = adxl.begin(0x53);
//   if (!adxlOK) Serial.println("ADXL345 not found!");
//   else {
//     adxl.setRange(ADXL345_RANGE_4_G);
//     adxl.setDataRate(ADXL345_DATARATE_800_HZ);
//   }

//   i2s.setPins(I2S_BCLK, I2S_WS, -1, I2S_DIN);
//   micOK = i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT,
//                     I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
//   if (!micOK) Serial.println("INMP441 I2S init failed!");

//   delay(800);
// }

// void loop() {
//   readTemps();
//   readVoltages();
//   readOil();
//   readMetal();
//   readInsulation();
//   readINA();
//   readADXL();
//   readMic();

//   updateStatus();
//   updateBuzzer();
//   updateDisplay();
//   printSerial();

//   delay(100);
// }
