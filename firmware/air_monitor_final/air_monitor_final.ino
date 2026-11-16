/**
 * @file air_monitor.ino
 * @brief Indoor Air Quality Monitor — ESP32 + PMS7003 + SCD40 + WeAct 3.7" E-Paper
 *
 * Measures CO2, temperature, humidity (SCD40) and particulate matter
 * PM1.0, PM2.5, PM10 (PMS7003). Results are shown on a 6-cell grid
 * on an e-paper display. The ESP32 uses deep sleep between measurements.
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
#include "esp_sleep.h"

// =============================================================
// CONFIGURATION
// =============================================================

#define SLEEP_DURATION_US   (10ULL * 60ULL * 1000000ULL)  // 10 minutes
#define PMS_WARMUP_MS       30000                           // PMS7003 fan warmup
#define SCD40_TIMEOUT_S     15                              // max wait for SCD40 data

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

// PM2.5 thresholds — WHO 2021
Quality ratePM25() {
  if (!pms_ok)    return {"N/A",      false};
  if (pm25 <= 5)  return {"EXCELLENT",false};
  if (pm25 <= 15) return {"GOOD",     false};
  if (pm25 <= 25) return {"MODERATE", false};
  if (pm25 <= 50) return {"POOR",     true};
  return                 {"BAD",      true};
}

// PM10 thresholds — WHO 2021
Quality ratePM10() {
  if (!pms_ok)     return {"N/A",      false};
  if (pm10 <= 15)  return {"EXCELLENT",false};
  if (pm10 <= 45)  return {"GOOD",     false};
  if (pm10 <= 75)  return {"MODERATE", false};
  if (pm10 <= 150) return {"POOR",     true};
  return                  {"BAD",      true};
}

// PM1.0 thresholds
Quality ratePM1() {
  if (!pms_ok)   return {"N/A",      false};
  if (pm1 <= 5)  return {"EXCELLENT",false};
  if (pm1 <= 12) return {"GOOD",     false};
  if (pm1 <= 20) return {"MODERATE", false};
  if (pm1 <= 35) return {"POOR",     true};
  return                {"BAD",      true};
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

/**
 * @brief Starts SCD40 periodic measurement, waits for data,
 *        then reads CO2, temperature, and humidity.
 */
void readSCD40() {
  Serial.println("[SCD40] Starting...");

  scd4x.stopPeriodicMeasurement();
  delay(600);

  uint16_t err = scd4x.startPeriodicMeasurement();
  if (err != 0) {
    Serial.printf("[SCD40] Start error: %d\n", err);
    scd40_ok = false;
    return;
  }

  bool ready = false;
  for (int i = 0; i < SCD40_TIMEOUT_S; i++) {
    delay(1000);
    scd4x.getDataReadyStatus(ready);
    if (ready) break;
  }

  if (!ready) {
    Serial.println("[SCD40] Timeout");
    scd40_ok = false;
    return;
  }

  err      = scd4x.readMeasurement(co2, temperature, humidity);
  scd40_ok = (err == 0 && co2 > 0);

  if (scd40_ok)
    Serial.printf("[SCD40] CO2: %d ppm | Temp: %.1f C | Hum: %.1f%%\n",
                  co2, temperature, humidity);
  else
    Serial.printf("[SCD40] Read error: %d\n", err);
}

/**
 * @brief Waits for stabilization, then reads a fresh PMS7003 frame in active mode.
 */
void readPMS7003() {
  Serial.println("[PMS7003] Warming up...");
  delay(PMS_WARMUP_MS);

  // Discard samples that arrived during the warm-up wait.
  while (pmsSerial.available() > 0) {
    pmsSerial.read();
  }
  pms_ok = pms.readUntil(pmsData, 3000);

  if (pms_ok) {
    pm1  = pmsData.PM_AE_UG_1_0;
    pm25 = pmsData.PM_AE_UG_2_5;
    pm10 = pmsData.PM_AE_UG_10_0;
    Serial.printf("[PMS7003] PM1: %d | PM2.5: %d | PM10: %d ug/m3\n",
                  pm1, pm25, pm10);
  } else {
    Serial.println("[PMS7003] Read error");
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

    Quality p1q = ratePM1();
    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm1);
    drawCell(XOFF, YOFF + CH + GAP, CW, CH,
             "PM 1.0", buf, "ug/m3", p1q.label, p1q.invert);

    Quality p25q = ratePM25();
    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm25);
    drawCell(XOFF + CW + GAP, YOFF + CH + GAP, CW, CH,
             "PM 2.5", buf, "ug/m3", p25q.label, p25q.invert);

    Quality p10q = ratePM10();
    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm10);
    drawCell(XOFF + 2*(CW + GAP), YOFF + CH + GAP, CW, CH,
             "PM 10", buf, "ug/m3", p10q.label, p10q.invert);

  } while (display.nextPage());

  Serial.println("[Display] Updated");
}

// =============================================================
// ENTRY POINT
// =============================================================

/**
 * @brief Runs on every wake-up (cold boot or timer).
 *        Sequence: init → measure → display → deep sleep
 */
void setup() {
  Serial.begin(115200);

  // Disable built-in LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Check wake-up reason
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
  bool isFirstBoot = (wakeupReason != ESP_SLEEP_WAKEUP_TIMER);

  // Init display
  display.init(115200, isFirstBoot, 50, false);
  display.setRotation(1);

  if (isFirstBoot) {
    Serial.println("[System] Cold boot");
    showSplashScreen();
    delay(2000);
  } else {
    Serial.println("[System] Wake from deep sleep");
  }

  // Init SCD40
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  scd4x.begin(Wire, SCD40_I2C_ADDR_62);
  delay(100);
  scd4x.stopPeriodicMeasurement();
  delay(600);

  // Init PMS7003
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);

  // Measure and update display
  Serial.println("[System] === START ===");
  readSCD40();
  readPMS7003();
  updateDisplay();
  Serial.println("[System] === END ===");

  // Deep sleep pauses the ESP32. The sensors remain on the powered rails.
  Serial.printf("[System] Sleeping for %llu min...\n",
                SLEEP_DURATION_US / 60000000ULL);
  Serial.flush();
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_deep_sleep_start();
}

// Never reached with deep sleep
void loop() {}
