#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include "DashboardPage.h"

class AirMonitorWeb {
 public:
  void begin(const char* ssid, const char* password) {
    if (!ssid[0]) {
      Serial.println("[Web] Wi-Fi not configured. See wifi_secrets.example.h.");
      return;
    }

    server.on("/", HTTP_GET, [this]() {
      server.send_P(200, "text/html; charset=utf-8", DASHBOARD_PAGE);
    });
    server.on("/api/current", HTTP_GET, [this]() { sendCurrent(); });
    server.on("/api/history", HTTP_GET, [this]() { sendHistory(); });

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
    enabled = true;
    minuteStart = millis();
    Serial.println("[Web] Connecting to Wi-Fi...");
  }

  void addSCD40(unsigned long now, float co2, float temperature, float humidity) {
    if (!enabled) return;
    advanceHistory(now);
    addSample(0, co2, now);
    addSample(1, temperature, now);
    addSample(2, humidity, now);
  }

  void addPMS7003(unsigned long now, float pm1, float pm25, float pm10) {
    if (!enabled) return;
    advanceHistory(now);
    addSample(3, pm1, now);
    addSample(4, pm25, now);
    addSample(5, pm10, now);
  }

  void poll() {
    if (!enabled) return;
    const unsigned long now = millis();
    advanceHistory(now);

    if (WiFi.status() != WL_CONNECTED) {
      if (listening) {
        server.stop();
        listening = false;
      }
      return;
    }

    const bool justConnected = !listening;
    if (justConnected) {
      server.begin();
      listening = true;
    }
    if (justConnected || now - lastAddressPrint >= ADDRESS_PRINT_INTERVAL_MS) {
      Serial.print("[Web] http://");
      Serial.println(WiFi.localIP());
      lastAddressPrint = now;
    }
    server.handleClient();
  }

 private:
  static constexpr size_t METRIC_COUNT = 6;
  static constexpr size_t HISTORY_SIZE = 24 * 60;
  static constexpr unsigned long MINUTE_MS = 60000;
  static constexpr unsigned long LIVE_MAX_AGE_MS = 15000;
  static constexpr unsigned long ADDRESS_PRINT_INTERVAL_MS = 30000;

  WebServer server{80};
  bool enabled = false;
  bool listening = false;
  unsigned long minuteStart = 0;
  unsigned long lastAddressPrint = 0;
  unsigned long latestAt[METRIC_COUNT]{};
  bool hasLatest[METRIC_COUNT]{};
  float latest[METRIC_COUNT]{};
  float totals[METRIC_COUNT]{};
  uint16_t counts[METRIC_COUNT]{};
  float history[HISTORY_SIZE][METRIC_COUNT]{};
  size_t nextRow = 0;
  size_t storedRows = 0;

  void addSample(size_t index, float value, unsigned long now) {
    if (!isfinite(value)) return;
    latest[index] = value;
    latestAt[index] = now;
    hasLatest[index] = true;
    totals[index] += value;
    ++counts[index];
  }

  void advanceHistory(unsigned long now) {
    const unsigned long elapsedMinutes = (now - minuteStart) / MINUTE_MS;
    if (elapsedMinutes == 0) return;

    if (elapsedMinutes > HISTORY_SIZE) {
      nextRow = 0;
      storedRows = 0;
    }

    const size_t rows = elapsedMinutes > HISTORY_SIZE ? HISTORY_SIZE : elapsedMinutes;
    for (size_t row = 0; row < rows; ++row) {
      for (size_t metric = 0; metric < METRIC_COUNT; ++metric) {
        history[nextRow][metric] = elapsedMinutes <= HISTORY_SIZE && row == 0 && counts[metric]
            ? totals[metric] / counts[metric] : NAN;
      }
      nextRow = (nextRow + 1) % HISTORY_SIZE;
      if (storedRows < HISTORY_SIZE) ++storedRows;
    }
    for (size_t metric = 0; metric < METRIC_COUNT; ++metric) {
      totals[metric] = 0;
      counts[metric] = 0;
    }
    minuteStart += elapsedMinutes * MINUTE_MS;
  }

  void sendCurrent() {
    const unsigned long now = millis();
    String json = "[";
    for (size_t metric = 0; metric < METRIC_COUNT; ++metric) {
      if (metric) json += ',';
      if (hasLatest[metric] && now - latestAt[metric] < LIVE_MAX_AGE_MS) {
        json += String(latest[metric], 1);
      } else {
        json += "null";
      }
    }
    json += ']';
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
  }

  void sendHistory() {
    const unsigned long now = millis();
    advanceHistory(now);
    long requested = server.arg("minutes").toInt();
    if (requested <= 0 || requested > long(HISTORY_SIZE)) requested = HISTORY_SIZE;
    const size_t rows = storedRows < size_t(requested) ? storedRows : size_t(requested);
    const size_t first = (nextRow + HISTORY_SIZE - rows) % HISTORY_SIZE;

    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/csv; charset=utf-8", "");
    // Stream batches of rows to keep the response out of RAM.
    String chunk;
    chunk.reserve(1200);
    for (size_t row = 0; row < rows; ++row) {
      const float* sample = history[(first + row) % HISTORY_SIZE];
      for (size_t metric = 0; metric < METRIC_COUNT; ++metric) {
        if (metric) chunk += ',';
        if (isfinite(sample[metric])) chunk += String(sample[metric], 1);
      }
      chunk += '\n';
      if (chunk.length() >= 1000) {
        server.sendContent(chunk);
        chunk = "";
      }
    }
    if (chunk.length()) server.sendContent(chunk);
    server.sendContent("");
  }
};
