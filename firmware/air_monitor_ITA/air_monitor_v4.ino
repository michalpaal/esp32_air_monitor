/*
 * RILEVATORE D'ARIA - ESP32  v4
 * PMS7003 + SCD40 + WeAct 3.7" E-Paper
 * Deep sleep ESP32 tra le misurazioni (10 minuti)
 * I sensori si spengono automaticamente col deep sleep
 *
 * PIN E-PAPER:  VCC→3.3V | GND→GND | SDA→GPIO23 | SCL→GPIO18
 *               CS→GPIO5 | D/C→GPIO17 | RES→GPIO16 | BUSY→GPIO4
 * PIN PMS7003:  VCC→VIN(5V) | GND→GND | TX→GPIO13 | RX→GPIO14
 * PIN SCD40:    VCC→3.3V (condiviso) | GND→GND | SDA→GPIO21 | SCL→GPIO22
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

// ── E-Paper ──────────────────────────────────────
#define EPD_CS   5
#define EPD_DC   17
#define EPD_RST  16
#define EPD_BUSY 4

GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT>
  display(GxEPD2_370_GDEY037T03(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ── PMS7003 ───────────────────────────────────────
HardwareSerial pmsSerial(2);
PMS pms(pmsSerial);
PMS::DATA pmsData;

// ── SCD40 ─────────────────────────────────────────
SensirionI2cScd4x scd4x;

// ── LED built-in ──────────────────────────────────
#define LED_PIN 2

// ── Dati sensori ──────────────────────────────────
uint16_t co2 = 0;
float temperature = 0;
float humidity = 0;
uint16_t pm1 = 0, pm25 = 0, pm10 = 0;
bool scd40_ok = false, pms_ok = false;

// ── Deep sleep: 10 minuti in microsecondi ─────────
#define SLEEP_DURATION_US  (10ULL * 60ULL * 1000000ULL)

// ── Scale qualità aria OMS 2021 ───────────────────
struct Quality { const char* label; bool invert; };

Quality co2Quality() {
  if (!scd40_ok)  return {"N/D",        false};
  if (co2 < 600)  return {"ECCELLENTE", false};
  if (co2 < 800)  return {"OTTIMO",     false};
  if (co2 < 1000) return {"BUONO",      false};
  if (co2 < 1500) return {"DISCRETO",   false};
  if (co2 < 2000) return {"SCARSO",     true};
  return                 {"PESSIMO",    true};
}

Quality pm25Quality() {
  if (!pms_ok)     return {"N/D",        false};
  if (pm25 <= 5)   return {"ECCELLENTE", false};
  if (pm25 <= 15)  return {"BUONO",      false};
  if (pm25 <= 25)  return {"DISCRETO",   false};
  if (pm25 <= 50)  return {"SCARSO",     true};
  return                  {"PESSIMO",    true};
}

Quality pm10Quality() {
  if (!pms_ok)     return {"N/D",        false};
  if (pm10 <= 15)  return {"ECCELLENTE", false};
  if (pm10 <= 45)  return {"BUONO",      false};
  if (pm10 <= 75)  return {"DISCRETO",   false};
  if (pm10 <= 150) return {"SCARSO",     true};
  return                  {"PESSIMO",    true};
}

Quality pm1Quality() {
  if (!pms_ok)    return {"N/D",        false};
  if (pm1 <= 5)   return {"ECCELLENTE", false};
  if (pm1 <= 12)  return {"BUONO",      false};
  if (pm1 <= 20)  return {"DISCRETO",   false};
  if (pm1 <= 35)  return {"SCARSO",     true};
  return                 {"PESSIMO",    true};
}

Quality tempQuality() {
  if (!scd40_ok)          return {"N/D",        false};
  if (temperature < 16)   return {"FREDDO",     true};
  if (temperature < 20)   return {"FRESCO",     false};
  if (temperature < 26)   return {"IDEALE",     false};
  if (temperature < 30)   return {"CALDO",      false};
  return                         {"MOLT.CALDO", true};
}

Quality humQuality() {
  if (!scd40_ok)     return {"N/D",        false};
  if (humidity < 30) return {"SECCO",      true};
  if (humidity < 60) return {"IDEALE",     false};
  if (humidity < 70) return {"UMIDO",      false};
  return                    {"MOLT.UMIDO", true};
}

// ── Lettura SCD40 ─────────────────────────────────
void leggiSCD40() {
  Serial.println("Leggo SCD40...");

  scd4x.stopPeriodicMeasurement();
  delay(600);
  uint16_t startErr = scd4x.startPeriodicMeasurement();
  if (startErr != 0) {
    Serial.printf("Errore avvio SCD40: %d\n", startErr);
    scd40_ok = false;
    return;
  }

  bool ready = false;
  for (int i = 0; i < 15; i++) {
    delay(1000);
    scd4x.getDataReadyStatus(ready);
    if (ready) break;
  }

  if (!ready) {
    Serial.println("SCD40: timeout");
    scd40_ok = false;
    return;
  }

  uint16_t err = scd4x.readMeasurement(co2, temperature, humidity);
  scd40_ok = (err == 0 && co2 > 0);
  if (scd40_ok)
    Serial.printf("CO2:%d ppm  T:%.1f C  H:%.1f%%\n", co2, temperature, humidity);
  else
    Serial.printf("Errore lettura SCD40: %d\n", err);
}

// ── Lettura PMS7003 ───────────────────────────────
void leggiPMS7003() {
  Serial.println("Sveglio PMS7003...");
  pms.wakeUp();
  delay(30000); // riscaldamento 30 sec

  pms.requestRead();
  pms_ok = pms.readUntil(pmsData, 3000);

  if (pms_ok) {
    pm1  = pmsData.PM_AE_UG_1_0;
    pm25 = pmsData.PM_AE_UG_2_5;
    pm10 = pmsData.PM_AE_UG_10_0;
    Serial.printf("PM1:%d  PM2.5:%d  PM10:%d µg/m3\n", pm1, pm25, pm10);
  } else {
    Serial.println("Errore lettura PMS7003");
  }
}

// ── Disegna cella scacchiera ──────────────────────
void drawCell(int x, int y, int w, int h,
              const char* topLabel, const char* bigValue,
              const char* unit, const char* qualityLabel,
              bool invertQuality) {

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

  int bandH = 22;
  int bandY = y + h - bandH;
  if (invertQuality) {
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

// ── Schermata avvio ───────────────────────────────
void showStartupScreen() {
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
    display.print("Prima misura in corso...");
  } while (display.nextPage());
}

// ── Aggiorna display scacchiera ───────────────────
void aggiornaDisplay() {
  const int CW = 136, CH = 116, XOFF = 4, YOFF = 4, GAP = 1;
  char buf[16];

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    auto cq = co2Quality();
    snprintf(buf, sizeof(buf), scd40_ok ? "%d" : "--", co2);
    drawCell(XOFF, YOFF, CW, CH, "CO2", buf, "ppm", cq.label, cq.invert);

    auto tq = tempQuality();
    snprintf(buf, sizeof(buf), scd40_ok ? "%.1f" : "--", temperature);
    drawCell(XOFF + CW + GAP, YOFF, CW, CH, "TEMPERATURA", buf, "gradi C", tq.label, tq.invert);

    auto hq = humQuality();
    snprintf(buf, sizeof(buf), scd40_ok ? "%.0f%%" : "--", humidity);
    drawCell(XOFF + 2*(CW+GAP), YOFF, CW, CH, "UMIDITA'", buf, "%", hq.label, hq.invert);

    auto p1q = pm1Quality();
    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm1);
    drawCell(XOFF, YOFF + CH + GAP, CW, CH, "PM 1.0", buf, "ug/m3", p1q.label, p1q.invert);

    auto p25q = pm25Quality();
    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm25);
    drawCell(XOFF + CW + GAP, YOFF + CH + GAP, CW, CH, "PM 2.5", buf, "ug/m3", p25q.label, p25q.invert);

    auto p10q = pm10Quality();
    snprintf(buf, sizeof(buf), pms_ok ? "%d" : "--", pm10);
    drawCell(XOFF + 2*(CW+GAP), YOFF + CH + GAP, CW, CH, "PM 10", buf, "ug/m3", p10q.label, p10q.invert);

  } while (display.nextPage());
  Serial.println("Display aggiornato!");
}

// ── Setup (eseguito ad ogni wake-up) ──────────────
void setup() {
  Serial.begin(115200);

  // Spegni LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Mostra schermata avvio solo al primo boot (non dal deep sleep)
  esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
  bool isFirstBoot = (wakeup != ESP_SLEEP_WAKEUP_TIMER);

  // Init display
  display.init(115200, isFirstBoot, 2, false);
  display.setRotation(1);

  if (isFirstBoot) {
    showStartupScreen();
    delay(2000);
  }

  // Init SCD40
  Wire.begin(21, 22);
  scd4x.begin(Wire, SCD40_I2C_ADDR_62);
  delay(100);
  scd4x.stopPeriodicMeasurement();
  delay(600);

  // Init PMS7003
  pmsSerial.begin(9600, SERIAL_8N1, 13, 14);
  pms.passiveMode();

  // Misura
  Serial.println("\n=== MISURAZIONE ===");
  leggiSCD40();
  leggiPMS7003();
  aggiornaDisplay();
  Serial.println("=== FINE ===");

  // Vai in deep sleep per 10 minuti
  // Durante il deep sleep tutto è spento: sensori, LED, WiFi
  Serial.println("Deep sleep per 10 minuti...");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_deep_sleep_start();
}

// ── Loop non viene mai eseguito con deep sleep ────
void loop() {}
