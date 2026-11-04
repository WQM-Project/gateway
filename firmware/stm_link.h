#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "gps.h"

// STM32 -> ESP32 contract: one JSON object per line ('\n'), at most
// STM_MAX_LINE bytes, exactly the sensor fields below. The ESP32 replies
// "ACK\n" once the packet is stored in flash, or "NAK <reason>\n".
constexpr size_t STM_MAX_LINE = 512;
constexpr const char *SENSOR_FIELDS[] = {
  "pH", "turb_ntu", "tds_ppm", "do_mgl", "ec_uscm", "orp_mv", "temp_w1",
  "temp_w2", "depth_cm", "temp_air", "humidity", "pressure_hpa",
};
constexpr size_t SENSOR_FIELD_COUNT = sizeof(SENSOR_FIELDS) / sizeof(SENSOR_FIELDS[0]);

// Validates an STM32 line and returns the upload JSON with GPS added.
// Mirrors the backend schema so bad data is refused at the boat, not after upload.
inline bool buildPacket(const char *line, const GpsSnapshot &gps, String &out, const char *&reason) {
  StaticJsonDocument<1024> in;
  if (deserializeJson(in, line)) { reason = "invalid_json"; return false; }
  JsonObjectConst sensor = in.as<JsonObjectConst>();
  if (sensor.isNull()) { reason = "not_object"; return false; }
  if (sensor.size() != SENSOR_FIELD_COUNT + 2) { reason = "wrong_field_count"; return false; }
  const char *id = sensor["id"];
  const size_t idLength = id ? strlen(id) : 0;
  if (idLength == 0 || idLength > 64) { reason = "bad_id"; return false; }
  for (size_t i = 0; i < idLength; ++i)
    if (!isalnum(static_cast<unsigned char>(id[i])) && id[i] != '_' && id[i] != '-') { reason = "bad_id"; return false; }
  if (!sensor["ts"].is<uint32_t>()) { reason = "bad_ts"; return false; }

  StaticJsonDocument<1024> packet;
  packet["id"] = id;
  packet["ts"] = sensor["ts"].as<uint32_t>();
  for (const char *field : SENSOR_FIELDS) {
    JsonVariantConst value = sensor[field];
    if (!value.is<double>() || !isfinite(value.as<double>())) { reason = field; return false; }
    packet[field] = value;
  }
  // Position as of the moment this packet arrived. Stored once, so every
  // retry of this packet carries identical data and the backend can de-duplicate.
  if (gps.fix) {
    packet["lat"] = serialized(String(gps.lat, 6));  // 6 decimals ~ 0.1 m
    packet["lon"] = serialized(String(gps.lon, 6));
    packet["gps_age_s"] = gps.ageSeconds;
  }
  packet["gps_fix"] = gps.fix;
  out = "";
  serializeJson(packet, out);
  return true;
}

// Collects bytes into lines without blocking. Over-long lines are discarded
// whole, so a lost '\n' can never glue two packets together.
class LineReader {
 public:
  enum Result { NONE, LINE, TOO_LONG };
  Result feed(Stream &port) {
    while (port.available()) {
      const char c = port.read();
      if (c == '\r') continue;
      if (c == '\n') {
        const bool overflowed = overflow;
        buffer[length] = '\0';
        const size_t got = length;
        length = 0; overflow = false;
        if (overflowed) return TOO_LONG;
        if (got) return LINE;
        continue;
      }
      if (length < STM_MAX_LINE) buffer[length++] = c;
      else overflow = true;
    }
    return NONE;
  }
  const char *line() const { return buffer; }
 private:
  char buffer[STM_MAX_LINE + 1] = {};
  size_t length = 0;
  bool overflow = false;
};
