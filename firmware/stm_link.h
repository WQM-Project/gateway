#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "gps.h"

// STM32 -> ESP32 contract: one JSON object per line ('\n'), at most
// STM_MAX_LINE bytes. Required active STM32 measurements must be present;
// reserved/optional measurements and STM32 GNSS/IMU fields are forwarded when
// present and finite. The ESP32 replies "ACK\n" once stored, or "NAK <reason>\n".
constexpr size_t STM_MAX_LINE = 768;
constexpr const char *REQUIRED_SENSOR_FIELDS[] = {
  "pH", "tds_ppm", "do_mgl", "orp_mv", "temp_w1", "temp_air", "pressure_hpa",
};
constexpr const char *OPTIONAL_SENSOR_FIELDS[] = {
  "turb_ntu", "ec_uscm", "temp_w2", "depth_cm", "humidity",
  "alt_m", "sog_knots", "cog_deg", "heading", "pitch", "roll",
};
constexpr const char *PASSTHROUGH_META_FIELDS[] = {
  "id", "ts", "lat", "lon", "gps_fix", "sats",
};

inline bool containsKey(const char *const *list, size_t count, const char *key) {
  for (size_t i = 0; i < count; ++i)
    if (strcmp(list[i], key) == 0) return true;
  return false;
}

inline bool isAllowedStmKey(const char *key) {
  return containsKey(REQUIRED_SENSOR_FIELDS, sizeof(REQUIRED_SENSOR_FIELDS) / sizeof(REQUIRED_SENSOR_FIELDS[0]), key) ||
         containsKey(OPTIONAL_SENSOR_FIELDS, sizeof(OPTIONAL_SENSOR_FIELDS) / sizeof(OPTIONAL_SENSOR_FIELDS[0]), key) ||
         containsKey(PASSTHROUGH_META_FIELDS, sizeof(PASSTHROUGH_META_FIELDS) / sizeof(PASSTHROUGH_META_FIELDS[0]), key);
}

// Validates an STM32 line and returns the upload JSON with GNSS coordinates added.
// Mirrors the backend schema so bad data is refused at the boat, not after upload.
inline bool buildPacket(const char *line, const GpsSnapshot &gps, String &out, const char *&reason) {
  StaticJsonDocument<1536> in;
  if (deserializeJson(in, line)) { reason = "invalid_json"; return false; }
  JsonObjectConst sensor = in.as<JsonObjectConst>();
  if (sensor.isNull()) { reason = "not_object"; return false; }

  for (JsonPairConst kv : sensor) {
    if (!isAllowedStmKey(kv.key().c_str())) { reason = "unknown_field"; return false; }
  }

  const char *id = sensor["id"];
  const size_t idLength = id ? strlen(id) : 0;
  if (idLength == 0 || idLength > 64) { reason = "bad_id"; return false; }
  for (size_t i = 0; i < idLength; ++i)
    if (!isalnum(static_cast<unsigned char>(id[i])) && id[i] != '_' && id[i] != '-') { reason = "bad_id"; return false; }
  if (!sensor["ts"].is<uint32_t>()) { reason = "bad_ts"; return false; }

  StaticJsonDocument<1536> packet;
  packet["id"] = id;
  packet["ts"] = sensor["ts"].as<uint32_t>();
  for (const char *field : REQUIRED_SENSOR_FIELDS) {
    JsonVariantConst value = sensor[field];
    if (!value.is<double>() || !isfinite(value.as<double>())) { reason = field; return false; }
    packet[field] = value;
  }
  for (const char *field : OPTIONAL_SENSOR_FIELDS) {
    if (!sensor.containsKey(field)) continue;
    JsonVariantConst value = sensor[field];
    if (!value.is<double>() || !isfinite(value.as<double>())) { reason = field; return false; }
    packet[field] = value;
  }
  if (sensor.containsKey("sats")) {
    JsonVariantConst sats = sensor["sats"];
    if (!sats.is<uint32_t>()) { reason = "sats"; return false; }
    packet["sats"] = sats.as<uint32_t>();
  }

  // Prefer the gateway's SIM7600 GNSS fix when valid; otherwise accept a valid
  // STM32 NEO-M8N fix. Omit (0, 0) unfixed placeholders so maps stay clean.
  bool stmFix = false;
  if (sensor.containsKey("gps_fix")) {
    JsonVariantConst fixVar = sensor["gps_fix"];
    if (fixVar.is<bool>()) stmFix = fixVar.as<bool>();
    else if (fixVar.is<int>()) stmFix = fixVar.as<int>() > 0;
    else { reason = "gps_fix"; return false; }
  }
  const bool stmHasCoords = sensor.containsKey("lat") && sensor.containsKey("lon") &&
                            sensor["lat"].is<double>() && sensor["lon"].is<double>() &&
                            isfinite(sensor["lat"].as<double>()) && isfinite(sensor["lon"].as<double>());
  const double stmLat = stmHasCoords ? sensor["lat"].as<double>() : 0.0;
  const double stmLon = stmHasCoords ? sensor["lon"].as<double>() : 0.0;
  const bool stmCoordsValid = stmFix && stmHasCoords &&
                              stmLat >= -90.0 && stmLat <= 90.0 &&
                              stmLon >= -180.0 && stmLon <= 180.0 &&
                              (stmLat != 0.0 || stmLon != 0.0);

  if (gps.fix) {
    packet["lat"] = serialized(String(gps.lat, 6));  // 6 decimals ~ 0.1 m
    packet["lon"] = serialized(String(gps.lon, 6));
    packet["gps_age_s"] = gps.ageSeconds;
    packet["gps_fix"] = true;
  } else if (stmCoordsValid) {
    packet["lat"] = serialized(String(stmLat, 6));
    packet["lon"] = serialized(String(stmLon, 6));
    packet["gps_fix"] = true;
  } else {
    packet["gps_fix"] = false;
  }
  // Arrival time in Unix seconds, only once network or GPS has set the clock.
  // Unlike the server's recorded_at, it survives hours of queueing offline.
  if (GpsTracker::clockValid()) packet["utc_s"] = static_cast<uint32_t>(time(nullptr));
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
