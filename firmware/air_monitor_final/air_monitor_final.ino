/**
 * @file air_monitor.ino
 * @brief Indoor Air Quality Monitor — ESP32 + PMS7003 + SCD40 + WeAct 3.7" E-Paper
 *
 * Measures CO2, temperature, humidity (SCD40) and particulate matter
 * PM1.0, PM2.5, PM10 (PMS7003). Results are shown on a 6-cell grid
 * on an e-paper display that refreshes every two minutes.
 *
 * @author  Riccardo Giannini
 * @license MIT
 */

#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <PMS.h>
#include <HardwareSerial.h>
#include "AirMonitorWeb.h"
#include "AirMonitorMqtt.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#include "wifi_secrets.example.h"
#endif

#if __has_include("mqtt_secrets.h")
#include "mqtt_secrets.h"
#else
#include "mqtt_secrets.example.h"
#endif

// =============================================================
// CONFIGURATION
// =============================================================

constexpr unsigned long DISPLAY_INTERVAL_MS = 120000;
constexpr unsigned long PMS_WARMUP_MS = 30000;
constexpr unsigned long SCD40_POLL_MS = 1000;
constexpr unsigned long SAMPLE_MAX_AGE_MS = 15000;

// E-Paper pins
#define EPD_CS              27
#define EPD_DC              26
#define EPD_RST             25
#define EPD_BUSY            33

// PMS7003 UART pins (sensor TX connected to GPIO16, sensor RX unconnected)
#define PMS_RX_PIN          16
#define PMS_TX_PIN          -1

// SCD40 I2C pins
#define I2C_SDA_PIN         21
#define I2C_SCL_PIN         22

// Built-in LED (disabled to save power)
#define LED_PIN             2

// =============================================================
// HARDWARE INSTANCES
// =============================================================

GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT>
  display(GxEPD2_370_GDEY037T03(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

HardwareSerial pmsSerial(2);
PMS            pms(pmsSerial);
PMS::DATA      pmsData;

SensirionI2cScd4x scd4x;
AirMonitorWeb web;
AirMonitorMqtt mqtt;

// =============================================================
// SENSOR DATA
// =============================================================

uint16_t co2         = 0;
float    temperature = 0.0f;
float    humidity    = 0.0f;
uint16_t pm1         = 0;
uint16_t pm25        = 0;
uint16_t pm10        = 0;
bool     scd40_ok    = false;
bool     pms_ok      = false;

bool scd40Started = false;
unsigned long measurementStart = 0;
unsigned long lastScd40Poll = 0;
unsigned long lastScd40Sample = 0;
unsigned long lastPmsSample = 0;
unsigned long lastDisplayRefresh = 0;
bool firstDisplayRefresh = true;

// =============================================================
// AIR QUALITY RATING
// =============================================================

struct Quality {
  const char* label;
  bool        invert; // true = black band with white text (warning)
};

// CO2 thresholds — REHVA / ASHRAE
Quality rateCO2() {
  if (!scd40_ok)  return {"N/A",      false};
  if (co2 < 600)  return {"EXCELLENT",false};
  if (co2 < 800)  return {"GREAT",    false};
  if (co2 < 1000) return {"GOOD",     false};
  if (co2 < 1500) return {"MODERATE", false};
  if (co2 < 2000) return {"POOR",     true};
  return                 {"BAD",      true};
}

// European Environment Agency bands for hourly outdoor concentrations.
// https://airindex.eea.europa.eu/AQI/
constexpr int PM25_EEA_LIMITS[] = {5, 15, 50, 90, 140};
constexpr int PM10_EEA_LIMITS[] = {15, 45, 120, 195, 270};

Quality rateHourlyPM(float average, const int (&limits)[5]) {
  const long rounded = lroundf(average);
  if (rounded <= limits[0]) return {"1h GOOD", false};
  if (rounded <= limits[1]) return {"1h FAIR", false};
  if (rounded <= limits[2]) return {"1h MODERATE", false};
  if (rounded <= limits[3]) return {"1h POOR", true};
  if (rounded <= limits[4]) return {"1h VERY POOR", true};
  return {"1h EXT. POOR", true};
}

// Temperature thresholds (°C)
Quality rateTemperature() {
  if (!scd40_ok)        return {"N/A",   false};
  if (temperature < 16) return {"COLD",  true};
  if (temperature < 20) return {"COOL",  false};
  if (temperature < 26) return {"IDEAL", false};
  if (temperature < 30) return {"WARM",  false};
  return                       {"HOT",   true};
}

// Humidity thresholds (%)
Quality rateHumidity() {
  if (!scd40_ok)     return {"N/A",    false};
  if (humidity < 30) return {"DRY",    true};
  if (humidity < 60) return {"IDEAL",  false};
  if (humidity < 70) return {"HUMID",  false};
  return                    {"V.HUMID",true};
}

// =============================================================
// SENSOR READING
// =============================================================

void pollSCD40(unsigned long now) {
  if (!scd40Started || now - lastScd40Poll < SCD40_POLL_MS) {
    return;
  }
  lastScd40Poll = now;

  bool ready = false;
  int16_t error = scd4x.getDataReadyStatus(ready);
  if (error != 0) {
    Serial.printf("[SCD40] Data-ready error: %d\n", int(error));
    return;
  }
  if (!ready) {
    return;
  }

  uint16_t measuredCO2 = 0;
  float measuredTemperature = 0.0f;
  float measuredHumidity = 0.0f;
  error = scd4x.readMeasurement(measuredCO2, measuredTemperature, measuredHumidity);
  if (error != 0 || measuredCO2 == 0) {
    Serial.printf("[SCD40] No valid sample (error: %d)\n", int(error));
    return;
  }

  co2 = measuredCO2;
  temperature = measuredTemperature;
  humidity = measuredHumidity;
  scd40_ok = true;
  lastScd40Sample = now;
  web.addSCD40(now, co2, temperature, humidity);
  Serial.printf("[SCD40] CO2: %u ppm | Temp: %.1f C | Hum: %.1f%%\n",
                unsigned(co2), temperature, humidity);
}

void pollPMS7003(unsigned long now) {
  while (pmsSerial.available() > 0) {
    if (!pms.read(pmsData) || now - measurementStart < PMS_WARMUP_MS) {
      continue;
    }

    pm1 = pmsData.PM_AE_UG_1_0;
    pm25 = pmsData.PM_AE_UG_2_5;
    pm10 = pmsData.PM_AE_UG_10_0;
    pms_ok = true;
    lastPmsSample = now;
    web.addPMS7003(now, pm1, pm25, pm10);
  }
}

// =============================================================
// DISPLAY
// =============================================================

/**
 * @brief Draws a single cell of the 3x2 grid.
 */
void drawCell(int x, int y, int w, int h,
              const char* topLabel,     const char* bigValue,
              const char* unit,         const char* qualityLabel,
              bool        invertBand) {

  display.drawRect(x, y, w, h, GxEPD_BLACK);

  display.setFont(&FreeMonoBold9pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(x + 6, y + 16);
  display.print(topLabel);
  display.drawLine(x, y + 22, x + w, y + 22, GxEPD_BLACK);

  display.setFont(&FreeMonoBold18pt7b);
  display.setCursor(x + 6, y + 52);
  display.print(bigValue);

  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(x + 6, y + 68);
  display.print(unit);
  if (qualityLabel == nullptr) return;

  const int bandH = 22;
  const int bandY = y + h - bandH;

  if (invertBand) {
    display.fillRect(x + 1, bandY, w - 2, bandH - 1, GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
  } else {
    display.fillRect(x + 1, bandY, w - 2, bandH - 1, GxEPD_WHITE);
    display.drawRect(x + 1, bandY, w - 2, bandH - 1, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
  }

  int16_t tx, ty; uint16_t tw, th;
  display.getTextBounds(qualityLabel, 0, 0, &tx, &ty, &tw, &th);
  display.setCursor(x + (w - tw) / 2, bandY + 15);
  display.print(qualityLabel);
  display.setTextColor(GxEPD_BLACK);
}

/**
 * @brief Shows splash screen on first cold boot.
 */
void showSplashScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(4, 4, 408, 232, GxEPD_BLACK);
    display.drawRect(8, 8, 400, 224, GxEPD_BLACK);
    display.setFont(&FreeMonoBold18pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(52, 90);
    display.print("AIR  MONITOR");
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(100, 130);
    display.print("PMS7003  +  SCD40");
    display.setCursor(70, 155);
    display.print("Taking first reading...");
  } while (display.nextPage());
}

/**
 * @brief Renders the full 3x2 grid with all sensor readings.
 *        Row 1: CO2 | Temperature | Humidity
 *        Row 2: PM1.0 | PM2.5 | PM10
 */
void updateDisplay() {
  const int CW   = 136;
  const int CH   = 116;
  const int XOFF = 4;
  const int YOFF = 4;
  const int GAP  = 1;

  char buf[16];
  float hourlyPM25 = 0.0f;
  float hourlyPM10 = 0.0f;
  const bool hasHourlyPM = web.readHourlyPM(millis(), hourlyPM25, hourlyPM10);
  const Quality waiting = {"1h WAITING", false};
  const Quality p25q = hasHourlyPM ? rateHourlyPM(hourlyPM25, PM25_EEA_LIMITS) : waiting;
  const Quality p10q = hasHourlyPM ? rateHourlyPM(hourlyPM10, PM10_EEA_LIMITS) : waiting;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    Quality cq = rateCO2();
    snprintf(buf, sizeof(buf), scd40_ok ? "%d" : "--", co2);
    drawCell(XOFF, YOFF, CW, CH,
             "CO2", buf, "ppm", cq.label, cq.invert);

    Quality tq = rateTemperature();
    snprintf(buf, sizeof(buf), scd40_ok ? "%.1f" : "--", temperature);
    drawCell(XOFF + CW + GAP, YOFF, CW, CH,
             "TEMPERATURE", buf, "deg C", tq.label, tq.invert);

    Quality hq = rateHumidity();
    snprintf(buf, sizeof(buf), scd40_ok ? "%.0f%%" : "--", humidity);
    drawCell(XOFF + 2*(CW + GAP), YOFF, CW, CH,
             "HUMIDITY", buf, "%", hq.label, hq.invert);

    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm1);
    drawCell(XOFF, YOFF + CH + GAP, CW, CH,
             "PM 1.0", buf, "ug/m3", nullptr, false);

    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm25);
    drawCell(XOFF + CW + GAP, YOFF + CH + GAP, CW, CH,
             "PM 2.5", buf, "ug/m3", p25q.label, p25q.invert);

    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm10);
    drawCell(XOFF + 2*(CW + GAP), YOFF + CH + GAP, CW, CH,
             "PM 10", buf, "ug/m3", p10q.label, p10q.invert);

  } while (display.nextPage());

  Serial.println("[Display] Updated");
}

// =============================================================
// ENTRY POINT
// =============================================================

void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  display.init(115200, true, 50, false);
  display.setRotation(1);
  showSplashScreen();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  scd4x.begin(Wire, SCD40_I2C_ADDR_62);
  delay(1000);
  scd4x.stopPeriodicMeasurement();
  delay(600);
  int16_t error = scd4x.startPeriodicMeasurement();
  scd40Started = (error == 0);
  if (!scd40Started) {
    Serial.printf("[SCD40] Start error: %d\n", int(error));
  }

  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  web.begin(WIFI_SSID, WIFI_PASSWORD);
  mqtt.begin(MQTT_HOST, MQTT_PORT, MQTT_USERNAME, MQTT_PASSWORD);
  measurementStart = millis();
}

void loop() {
  const unsigned long now = millis();
  pollSCD40(now);
  pollPMS7003(now);

  const bool refreshDue = firstDisplayRefresh
      ? (scd40_ok && pms_ok) || now - measurementStart >= PMS_WARMUP_MS + SAMPLE_MAX_AGE_MS
      : now - lastDisplayRefresh >= DISPLAY_INTERVAL_MS;
  if (refreshDue) {
    scd40_ok = scd40_ok && now - lastScd40Sample < SAMPLE_MAX_AGE_MS;
    pms_ok = pms_ok && now - lastPmsSample < SAMPLE_MAX_AGE_MS;
    if (pms_ok) {
      Serial.printf("[PMS7003] PM1: %u | PM2.5: %u | PM10: %u ug/m3\n",
                    unsigned(pm1), unsigned(pm25), unsigned(pm10));
    }
    lastDisplayRefresh = now;
    firstDisplayRefresh = false;
    updateDisplay();
  }

  web.poll();
  const unsigned long publishNow = millis();
  const bool scdFresh = scd40_ok && publishNow - lastScd40Sample < SAMPLE_MAX_AGE_MS;
  const bool pmsFresh = pms_ok && publishNow - lastPmsSample < SAMPLE_MAX_AGE_MS;
  const float mqttValues[6] = {
    scdFresh ? float(co2) : NAN,
    scdFresh ? temperature : NAN,
    scdFresh ? humidity : NAN,
    pmsFresh ? float(pm1) : NAN,
    pmsFresh ? float(pm25) : NAN,
    pmsFresh ? float(pm10) : NAN
  };
  mqtt.poll(publishNow, mqttValues);
  delay(1);
}
