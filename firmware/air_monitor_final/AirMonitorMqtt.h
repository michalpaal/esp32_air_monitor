#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <math.h>

class AirMonitorMqtt {
 public:
  void begin(const char* host, uint16_t port, const char* username, const char* password) {
    enabled = host[0] != '\0';
    if (!enabled) return;
    user = username;
    pass = password;
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    deviceId = "air_monitor_" + mac;
    baseTopic = "airmonitor/" + deviceId;
    client.setServer(host, port);
    enabled = client.setBufferSize(1536);
    client.setSocketTimeout(1);
    client.setKeepAlive(30);
    client.setCallback([this](char* topic, uint8_t* payload, unsigned int length) {
      if (strcmp(topic, "homeassistant/status") == 0 && length == 6 &&
          memcmp(payload, "online", 6) == 0) {
        discoveryPending = true;
        statePending = true;
      }
    });
    Serial.printf("[MQTT] Broker %s:%u%s\n", host, unsigned(port),
                  enabled ? "" : " (buffer allocation failed)");
  }

  void poll(unsigned long now, const float (&values)[6]) {
    if (!enabled) return;
    if (WiFi.status() != WL_CONNECTED) {
      // Drop the transport so the next Wi-Fi connection starts a fresh MQTT session.
      network.stop();
      return;
    }
    if (!client.connected()) {
      if (attempted && now - lastAttempt < RETRY_MS) return;
      attempted = true;
      lastAttempt = now;
      const String availability = baseTopic + "/availability";
      const char* login = user[0] ? user : nullptr;
      const char* password = user[0] ? pass : nullptr;
      if (!client.connect(deviceId.c_str(), login, password,
                          availability.c_str(), 1, true, "offline")) {
        Serial.printf("[MQTT] Connection failed (%d), retrying in 30 s\n", client.state());
        return;
      }
      if (!client.subscribe("homeassistant/status") ||
          !client.publish(availability.c_str(), "online", true)) {
        network.stop();
        return;
      }
      discoveryPending = true;
      statePending = true;
      Serial.println("[MQTT] Connected");
    }
    client.loop();
    if (discoveryPending) {
      if (!publishDiscovery()) {
        network.stop();
        return;
      }
      discoveryPending = false;
    }
    if (statePending || now - lastPublish >= PUBLISH_MS) {
      String state = "{";
      for (size_t i = 0; i < 6; ++i) {
        if (i) state += ',';
        state += '"';
        state += metricKey(i);
        state += "\":";
        state += isfinite(values[i]) ? String(values[i], 1) : String("null");
      }
      state += '}';
      if (client.publish((baseTopic + "/state").c_str(), state.c_str(), false)) {
        lastPublish = now;
        statePending = false;
      } else {
        network.stop();
      }
    }
  }

 private:
  static constexpr unsigned long RETRY_MS = 30000;
  static constexpr unsigned long PUBLISH_MS = 30000;
  WiFiClient network;
  PubSubClient client{network};
  const char* user = "";
  const char* pass = "";
  String deviceId;
  String baseTopic;
  bool enabled = false;
  bool attempted = false;
  bool discoveryPending = false;
  bool statePending = false;
  unsigned long lastAttempt = 0;
  unsigned long lastPublish = 0;

  const char* metricKey(size_t index) const {
    static const char* const keys[] = {"co2", "temperature", "humidity", "pm1", "pm25", "pm10"};
    return keys[index];
  }

  bool publishDiscovery() {
    static const char* const names[] = {"CO2", "Temperature", "Humidity", "PM1.0", "PM2.5", "PM10"};
    static const char* const classes[] = {"carbon_dioxide", "temperature", "humidity", "pm1", "pm25", "pm10"};
    static const char* const units[] = {"ppm", "°C", "%", "µg/m³", "µg/m³", "µg/m³"};
    for (size_t i = 0; i < 6; ++i) {
      const String key = metricKey(i);
      String config;
      config.reserve(900);
      config = "{\"name\":\"";
      config += names[i];
      config += "\",\"unique_id\":\"" + deviceId + "_" + key;
      config += "\",\"state_topic\":\"" + baseTopic + "/state";
      config += "\",\"availability_topic\":\"" + baseTopic + "/availability";
      config += "\",\"value_template\":\"{{ value_json." + key + " }}";
      config += "\",\"device_class\":\"";
      config += classes[i];
      config += "\",\"unit_of_measurement\":\"";
      config += units[i];
      config += "\",\"state_class\":\"measurement\",\"expire_after\":90";
      config += ",\"device\":{\"identifiers\":[\"" + deviceId;
      config += "\"],\"name\":\"Air Monitor\",\"manufacturer\":\"DIY\",";
      config += "\"model\":\"ESP32 SCD40 PMS7003\",\"configuration_url\":\"http://";
      config += WiFi.localIP().toString();
      config += "/\"}}";
      const String topic = "homeassistant/sensor/" + deviceId + "/" + key + "/config";
      if (!client.publish(topic.c_str(), config.c_str(), true)) return false;
    }
    return true;
  }
};
