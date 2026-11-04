#pragma once
#include <Arduino.h>
#include <sys/time.h>
#include <time.h>
#include <TinyGsmClient.h>
#include <SSLClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "upload_ca.h"
#include "ModemAT.h"
#if __has_include("upload.local.h")
#include "upload.local.h"
#else
#error "Prepare firmware/upload.local.h with the dedicated gateway key first"
#endif

// TinyGSM fetches up to its FIFO capacity per AT+CIPRXGET=2 call.
// SIM7600 permits at most 1500 raw bytes; oversized reads stall TLS reception.
static_assert(TINY_GSM_RX_BUFFER <= 1500, "SIM7600 raw receive requests must fit its 1500-byte limit");

constexpr const char *UPLOAD_HOST = "scvcufiagvbeixhnwjze.supabase.co";
constexpr const char *UPLOAD_PATH = "/functions/v1/ingest";

// TCP goes to the modem-resolved IP; TLS still verifies UPLOAD_HOST and sends SNI.
class ResolvedTransport : public TinyGsmClient {
 public:
  explicit ResolvedTransport(TinyGsm &device) : TinyGsmClient(device, 1) {}
  using TinyGsmClient::connect;
  int connect(const char *host, uint16_t port) override {
    const char *target = strcmp(host, UPLOAD_HOST) == 0 && ip.length() ? ip.c_str() : host;
    return TinyGsmClient::connect(target, port, 45);
  }
  String ip;
};

class Uploader {
 public:
  enum Result { SAVED, DUPLICATE, REJECTED, RETRY };

  Uploader(TinyGsm &device, HardwareSerial &uart)
    : modem(device), modemSerial(uart), transport(device), secure(&transport) {}

  void begin() {
    secure.setCACert(UPLOAD_CA);
    // SSLClient uses a positive read timeout as its handshake timeout too.
    // Polling avoids waiting for a full TLS-sized read on partial records.
    secure.setTimeout(0);
    secure.setHandshakeTimeout(90);
  }

  // Forget the data session after a modem reset; ensureOnline() rebuilds it.
  void reset() {
    secure.stop();
    transport.ip = "";
  }

  // Brings up registration, data, clock and DNS as needed. Blocks while working.
  bool ensureOnline() {
    if (!modem.isNetworkConnected()) {
      Serial.println("[Net] Waiting for mobile network registration...");
      if (!modem.waitForNetwork(60000L)) { Serial.println("[Net] No network"); return false; }
    }
    if (!modem.isGprsConnected()) {
      Serial.println("[Net] Connecting mobile data...");
      if (!modem.gprsConnect(SIM_APN, "", "")) { Serial.println("[Net] Data connection failed"); return false; }
      transport.ip = "";  // new data session; resolve again
    }
    if (!clockSet && !(clockSet = setClock())) return false;
    if (!transport.ip.length() && !resolve()) return false;
    return true;
  }

  Result send(const String &id, const String &payload) {
    secure.stop();
    if (!secure.connect(UPLOAD_HOST, 443)) {
      Serial.println("[Upload] TLS connection failed");
      transport.ip = "";  // the server's address may have changed
      return RETRY;
    }
    HttpClient http(secure, UPLOAD_HOST, 443);
    http.connectionKeepAlive();
    http.setHttpResponseTimeout(30000);
    http.beginRequest();
    if (http.post(UPLOAD_PATH) != 0) { secure.stop(); return RETRY; }
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", payload.length());
    http.sendHeader("Authorization", String("Bearer ") + GATEWAY_UPLOAD_KEY);
    http.sendHeader("X-Packet-Id", id);
    http.sendHeader("Connection", "close");
    http.beginBody();
    if (http.print(payload) != payload.length()) { secure.stop(); return RETRY; }
    http.endRequest();
    const int status = http.responseStatusCode();
    if (status != 200 && status != 201) {
      secure.stop();
      // Data the server will never accept: drop it instead of blocking the queue.
      // 401/403/503 are configuration or outages, so those packets wait.
      if (status == 400 || status == 409 || status == 413 || status == 422) {
        Serial.printf("[Upload] HTTP %d: server rejected packet permanently\n", status);
        return REJECTED;
      }
      Serial.printf("[Upload] HTTP %d: will retry\n", status);
      return RETRY;
    }
    const bool duplicate = readAcknowledgement(http, id);
    secure.stop();
    if (!acknowledged) return RETRY;
    if ((status == 201) == duplicate) return RETRY;  // status and body disagree
    return duplicate ? DUPLICATE : SAVED;
  }

 private:
  bool readAcknowledgement(HttpClient &http, const String &id) {
    acknowledged = false;
    if (http.skipResponseHeaders() != 0 || http.contentLength() > 1024) return false;
    const int declaredLength = http.contentLength();
    String body;
    const uint32_t start = millis();
    // HttpClient decodes chunked responses. Bound both decoded body and duration.
    while (millis() - start < 30000 && !http.endOfBodyReached()) {
      if (http.available()) {
        const int byte = http.read();
        if (byte >= 0) body += static_cast<char>(byte);
        if (body.length() > 1024) return false;
      } else {
        if (!http.connected()) break;
        delay(10);
      }
    }
    // Chunked/close-delimited replies have no Content-Length in this library.
    const bool complete = declaredLength >= 0 ? http.endOfBodyReached()
      : (!http.connected() && http.available() == 0);
    StaticJsonDocument<512> reply;
    if (!complete || deserializeJson(reply, body) || reply["saved"] != true ||
        !reply["duplicate"].is<bool>() || reply["packet_id"].as<String>() != id) {
      Serial.println("[Upload] Missing/invalid acknowledgement; packet kept");
      return false;
    }
    acknowledged = true;
    return reply["duplicate"].as<bool>();
  }

  bool resolve() {
    // No credential is present in this command or response.
    const AtResult dns = sendAt(modemSerial, String("AT+CDNSGIP=\"") + UPLOAD_HOST + "\"", 30000);
    const int position = dns.response.indexOf("+CDNSGIP:");
    char hostname[96] = {}, address[48] = {};
    int status = 0;
    IPAddress parsed;
    if (!dns.success || position < 0 ||
        sscanf(dns.response.substring(position).c_str(), "+CDNSGIP: %d,\"%95[^\"]\",\"%47[^\"]\"", &status, hostname, address) != 3 ||
        status != 1 || strcmp(hostname, UPLOAD_HOST) != 0 || !parsed.fromString(address)) {
      Serial.println("[Net] DNS lookup failed");
      return false;
    }
    transport.ip = address;
    return true;
  }

  // TLS certificate checks need the real date; take UTC from the network clock.
  bool setClock() {
    setenv("TZ", "UTC0", 1);
    tzset();
    for (int attempt = 0; attempt < 6; ++attempt) {
      int y, m, d, h, minute, s;
      float zone;
      if (modem.getNetworkTime(&y, &m, &d, &h, &minute, &s, &zone) &&
          y >= 2026 && y <= 2035 && m >= 1 && m <= 12 && d >= 1 && d <= 31 &&
          h >= 0 && h <= 23 && minute >= 0 && minute <= 59 && s >= 0 && s <= 59 &&
          zone >= -12 && zone <= 14) {
        struct tm clock = {};
        clock.tm_year = y - 1900; clock.tm_mon = m - 1; clock.tm_mday = d;
        clock.tm_hour = h; clock.tm_min = minute; clock.tm_sec = s;
        const time_t epoch = mktime(&clock) - static_cast<long>(zone * 3600);
        struct timeval now = {epoch, 0};
        if (settimeofday(&now, nullptr) != 0) return false;
        Serial.printf("[Net] UTC clock set: %s", ctime(&epoch));
        return true;
      }
      delay(2000);
    }
    Serial.println("[Net] Network clock missing; certificate validation stays enabled");
    return false;
  }

  TinyGsm &modem;
  HardwareSerial &modemSerial;
  ResolvedTransport transport;
  SSLClient secure;
  bool clockSet = false, acknowledged = false;
};
