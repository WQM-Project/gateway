#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>
#include <time.h>
#include <TinyGsmClient.h>

// Last known SIM7600 GNSS position. The main loop polls the modem; the STM32
// task only reads this cache, so it never touches the modem UART.
struct GpsSnapshot {
  bool fix = false;
  double lat = 0, lon = 0;
  uint32_t ageSeconds = 0;
};

class GpsTracker {
 public:
  // Safe to call again after a modem reset: GNSS turns off with the modem.
  bool begin(TinyGsm &device, Stream &uart) {
    modem = &device;
    port = &uart;
    if (!lock) lock = xSemaphoreCreateMutex();
    streakStart = 0;
    // AT+CGPS=1 returns ERROR when GNSS is already running, e.g. after an
    // ESP32-only reset, so a failed enable is retried on the next poll.
    enabled = modem->enableGPS() || isRunning();
    Serial.println(enabled ? "[GPS] GNSS receiver on; first fix can take 30 s to a few minutes outdoors"
                           : "[GPS] GNSS enable not confirmed; will retry");
    return lock != nullptr;
  }

  // Call from the main loop only, never during an upload. One AT+CGNSSINFO
  // per poll: back-to-back queries can receive each other's late replies.
  void poll() {
    if (!modem || millis() - lastPoll < POLL_INTERVAL_MS) return;
    lastPoll = millis();
    if (!enabled) enabled = modem->enableGPS() || isRunning();
    lastRaw = query();
    double lat = 0, lon = 0, pdop = 0;
    time_t utc = 0;
    if (!parse(lastRaw, lat, lon, pdop, utc)) {
      if (!everFixed && millis() - lastWaitingLog >= 30000) {
        lastWaitingLog = millis();
        Serial.println("[GPS] Searching for satellites (no fix yet)");
      }
      return;
    }
    setClockFrom(utc);
    // A new fix wanders for its first tens of seconds (about 280 m in bench
    // tests), so trust it only after SETTLE_MS of uninterrupted fixes.
    if (!streakStart || millis() - lastGood > STREAK_GAP_MS) {
      streakStart = millis();
      Serial.printf("[GPS] Fix acquired (PDOP %.1f); settling for %lu s before use\n",
        pdop, static_cast<unsigned long>(SETTLE_MS / 1000));
    }
    lastGood = millis();
    if (millis() - streakStart < SETTLE_MS) return;
    if (!everFixed) Serial.printf("[GPS] Position in use: %.6f, %.6f (PDOP %.1f)\n", lat, lon, pdop);
    everFixed = true;
    xSemaphoreTake(lock, portMAX_DELAY);
    last.lat = lat; last.lon = lon; last.fix = true;
    fixAt = millis();
    xSemaphoreGive(lock);
  }

  // Safe from any task. Age is fixed at the moment the packet is stamped.
  GpsSnapshot snapshot() {
    GpsSnapshot copy;
    if (!lock) return copy;
    xSemaphoreTake(lock, portMAX_DELAY);
    copy = last;
    if (copy.fix) copy.ageSeconds = (millis() - fixAt) / 1000;
    xSemaphoreGive(lock);
    return copy;
  }

  // Modem's reply from the latest poll, for diagnostics.
  const String &raw() const { return lastRaw; }

  // True once the ESP32 clock holds a real date (set from network or GPS).
  static bool clockValid() { return time(nullptr) >= 1767225600; }  // 2026-01-01 UTC

 private:
  static constexpr uint32_t POLL_INTERVAL_MS = 5000;
  static constexpr uint32_t SETTLE_MS = 45000;
  // Missing fixes for longer than this restart the settling period.
  static constexpr uint32_t STREAK_GAP_MS = 30000;

  String query() {
    while (port->available()) port->read();  // drop any late reply to an earlier command
    modem->sendAT(GF("+CGNSSINFO"));
    if (modem->waitResponse(5000L, GF("+CGNSSINFO:")) != 1) return "";
    String line = port->readStringUntil('\n');
    modem->waitResponse();
    line.trim();
    return line;
  }

  // <mode>,<gps>,<glonass>,<beidou>,<lat ddmm.mmmmmm>,<N/S>,<lon dddmm.mmmmmm>,<E/W>,
  // <date>,<utc>,<alt>,<speed>,<course>,<pdop>,<hdop>,<vdop>. Empty fields = no fix.
  static bool parse(const String &line, double &lat, double &lon, double &pdop, time_t &utc) {
    String field[16];
    int count = 0, start = 0;
    while (count < 16) {
      const int comma = line.indexOf(',', start);
      field[count++] = comma < 0 ? line.substring(start) : line.substring(start, comma);
      if (comma < 0) break;
      start = comma + 1;
    }
    if (count < 14) return false;
    const int mode = field[0].toInt();
    if (mode != 2 && mode != 3) return false;
    if (!field[4].length() || !field[6].length()) return false;
    lat = toDecimalDegrees(field[4].toDouble()) * (field[5] == "S" ? -1 : 1);
    lon = toDecimalDegrees(field[6].toDouble()) * (field[7] == "W" ? -1 : 1);
    pdop = field[13].toDouble();
    utc = satelliteTime(field[8], field[9]);
    return plausible(lat, lon);
  }

  // ddmm.mmmm -> decimal degrees: 3146.8487 = 31 + 46.8487 / 60.
  static double toDecimalDegrees(double value) {
    const double whole = floor(value / 100);
    return whole + (value - whole * 100) / 60;
  }

  // <date> ddmmyy and <utc> hhmmss.s, from the satellites' atomic clocks.
  static time_t satelliteTime(const String &date, const String &clock) {
    if (date.length() != 6 || clock.length() < 6) return 0;
    struct tm t = {};
    t.tm_mday = date.substring(0, 2).toInt();
    t.tm_mon = date.substring(2, 4).toInt() - 1;
    t.tm_year = date.substring(4, 6).toInt() + 100;
    t.tm_hour = clock.substring(0, 2).toInt();
    t.tm_min = clock.substring(2, 4).toInt();
    t.tm_sec = clock.substring(4, 6).toInt();
    if (t.tm_year < 126 || t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1) return 0;
    setenv("TZ", "UTC0", 1);
    tzset();
    return mktime(&t);
  }

  // Sets the ESP32 clock from GPS when the network clock has not set it yet.
  static void setClockFrom(time_t utc) {
    if (!utc || clockValid()) return;
    struct timeval now = {utc, 0};
    if (settimeofday(&now, nullptr) == 0) Serial.printf("[GPS] UTC clock set from satellites: %s", ctime(&utc));
  }

  // 0,0 is a common "no data" value from receivers, not a real river position.
  static bool plausible(double lat, double lon) {
    return isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180 &&
           !(fabs(lat) < 0.0001 && fabs(lon) < 0.0001);
  }

  bool isRunning() {
    while (port->available()) port->read();
    modem->sendAT(GF("+CGPS?"));
    const bool on = modem->waitResponse(GF("+CGPS: 1")) == 1;
    modem->waitResponse();
    return on;
  }

  TinyGsm *modem = nullptr;
  Stream *port = nullptr;
  SemaphoreHandle_t lock = nullptr;
  GpsSnapshot last;
  String lastRaw;
  uint32_t fixAt = 0, lastPoll = 0, lastWaitingLog = 0, streakStart = 0, lastGood = 0;
  bool enabled = false, everFixed = false;
};
