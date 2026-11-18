#pragma once

// Copy to mqtt_secrets.h and use the same login as your MQTT broker.
constexpr char MQTT_HOST[] = "192.168.0.10";
constexpr uint16_t MQTT_PORT = 1883;
constexpr char MQTT_USERNAME[] = "";
constexpr char MQTT_PASSWORD[] = "";
// An empty host disables MQTT. Anonymous access is possible if the broker allows it.
