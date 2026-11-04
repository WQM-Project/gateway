#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <TinyGsmClient.h>
#include <SSLClient.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "upload_ca.h"
#include "ModemAT.h"
// TinyGSM fetches up to its FIFO capacity per AT+CIPRXGET=2 call.
// SIM7600 permits at most 1500 raw bytes; oversized reads stall TLS reception.
static_assert(TINY_GSM_RX_BUFFER <= 1500, "SIM7600 raw receive requests must fit its 1500-byte limit");
#if __has_include("upload.local.h")
#include "upload.local.h"
#else
#error "Prepare firmware/upload.local.h with the dedicated gateway key first"
#endif

// TLS runs on ESP32 over the modem's TCP connection. No Wi-Fi or insecure mode.
constexpr const char *UPLOAD_HOST = "scvcufiagvbeixhnwjze.supabase.co";
constexpr const char *UPLOAD_PATH = "/functions/v1/ingest";
constexpr const char *SAMPLE = "{\"id\":\"WQM-001\",\"ts\":12000,\"pH\":7.12,\"turb_ntu\":42.5,\"tds_ppm\":310,\"do_mgl\":6.85,\"ec_uscm\":680.2,\"orp_mv\":215,\"temp_w1\":23.5,\"temp_w2\":23.48,\"depth_cm\":35.2,\"temp_air\":28.6,\"humidity\":65.3,\"pressure_hpa\":1013.25}";
HardwareSerial modemSerial(2);
class ConnectionStatusStream : public Stream {
 public:
  explicit ConnectionStatusStream(HardwareSerial &uart) : uart(uart) {}
  int available() override { return uart.available(); }
  int peek() override { return uart.peek(); }
  void flush() override { uart.flush(); }
  size_t write(uint8_t byte) override { return uart.write(byte); }
  size_t write(const uint8_t *buffer, size_t size) override { return uart.write(buffer, size); }
  int read() override {
    const int byte = uart.read();
    if (byte < 0) return byte;
    if (byte == '\n') {
      // Allow only the modem's socket-open status, never raw command/data logs.
      if (line.startsWith("+CIPOPEN:") || line.startsWith("+IP ERROR:"))
        Serial.println("[Modem socket] " + line);
      line = "";
    } else if (byte != '\r') {
      if (byte >= 32 && byte <= 126 && line.length() < 96) line += static_cast<char>(byte);
      else line = "";
    }
    return byte;
  }
 private:
  HardwareSerial &uart;
  String line;
};
ConnectionStatusStream commands(modemSerial);
TinyGsm modem(commands);
TinyGsmClient probeClient(modem, 0);
String resolvedUploadIp;
// Counts only encrypted transport bytes; never prints their contents.
class DiagnosticTransport : public TinyGsmClient {
 public:
  explicit DiagnosticTransport(TinyGsm &device) : TinyGsmClient(device, 1) {}
  using TinyGsmClient::connect;
  int connect(const char *host, uint16_t port) override {
    const char *target = strcmp(host, UPLOAD_HOST) == 0 && resolvedUploadIp.length()
      ? resolvedUploadIp.c_str() : host;
    Serial.printf("[TCP] Opening %s:%u on socket 1\n", target, port);
    const int result = TinyGsmClient::connect(target, port, 45);
    Serial.printf("[TCP] Open result=%d\n", result);
    return result;
  }
  using TinyGsmClient::write;
  using TinyGsmClient::read;
  uint32_t sent = 0, received = 0, lastReport = 0;
  bool tracing = false;
  size_t write(const uint8_t *buffer, size_t size) override {
    const size_t result = TinyGsmClient::write(buffer, size);
    sent += result;
    if (tracing) Serial.printf("[TLS transport] TX requested=%u sent=%u\n", static_cast<unsigned>(size), static_cast<unsigned>(result));
    return result;
  }
  int read(uint8_t *buffer, size_t size) override {
    const int result = TinyGsmClient::read(buffer, size);
    if (result > 0) {
      received += result;
      if (tracing) Serial.printf("[TLS transport] RX bytes=%d\n", result);
    }
    return result;
  }
  int available() override {
    const int result = TinyGsmClient::available();
    if (tracing && millis() - lastReport >= 10000) {
      Serial.printf("[TLS transport] totals TX=%lu RX=%lu available=%d\n",
        static_cast<unsigned long>(sent), static_cast<unsigned long>(received), result);
      lastReport = millis();
    }
    return result;
  }
  void startTrace() { sent = received = 0; lastReport = millis(); tracing = true; }
};
DiagnosticTransport transport(modem);
SSLClient secure(&transport);
Preferences pending;

bool checkPlainTcp() {
  Serial.println("Checking modem TCP send/receive with a public HTTP probe (no key)...");
  if (!probeClient.connect("example.com", 80, 30)) {
    Serial.println("[FAIL] Plain TCP connection; TLS test stopped"); return false;
  }
  const char *probe = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
  if (probeClient.print(probe) != strlen(probe)) {
    Serial.println("[FAIL] Plain TCP send"); probeClient.stop(); return false;
  }
  String line;
  const uint32_t start = millis();
  while (millis() - start < 30000 && line.length() < 120) {
    if (probeClient.available()) {
      const int byte = probeClient.read();
      if (byte == '\n') break;
      if (byte >= 0 && byte != '\r') line += static_cast<char>(byte);
    } else { delay(10); }
  }
  probeClient.stop();
  if (!line.startsWith("HTTP/1.")) { Serial.println("[FAIL] Plain TCP response missing"); return false; }
  Serial.println("[PASS] Modem TCP send/receive: " + line);
  return true;
}

bool resolveUploadHost() {
  Serial.println("Checking modem DNS for the WQM hostname...");
  // No credential is present in this command or response.
  const AtResult dns = sendAt(modemSerial, String("AT+CDNSGIP=\"") + UPLOAD_HOST + "\"", 30000);
  const int position = dns.response.indexOf("+CDNSGIP:");
  char hostname[96] = {}, address[48] = {};
  int status = 0;
  if (!dns.success || position < 0 ||
      sscanf(dns.response.substring(position).c_str(), "+CDNSGIP: %d,\"%95[^\"]\",\"%47[^\"]\"", &status, hostname, address) != 3 ||
      status != 1 || strcmp(hostname, UPLOAD_HOST) != 0) {
    Serial.println("[FAIL] WQM hostname DNS lookup; no credential sent"); return false;
  }
  IPAddress ip;
  if (!ip.fromString(address)) { Serial.println("[FAIL] DNS did not return IPv4"); return false; }
  resolvedUploadIp = address;
  Serial.println("[PASS] Modem resolved WQM IPv4: " + resolvedUploadIp);
  // Only TCP uses the resolved IP. TLS still checks UPLOAD_HOST and sends SNI.
  return true;
}

String newPacketId() {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  char id[37];
  snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return String(id);
}

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
      Serial.printf("[PASS] ESP32 UTC clock: %s", ctime(&epoch));
      return true;
    }
    delay(2000);
  }
  Serial.println("[FAIL] Modem clock missing/invalid; certificate validation stays enabled");
  return false;
}

// Return 1 for a new save, 2 for a duplicate acknowledgement, 0 for failure.
int upload(const String &id, const String &payload) {
  secure.stop();
  transport.startTrace();
  Serial.println("Checking HTTPS certificate and hostname...");
  if (!secure.connect(UPLOAD_HOST, 443)) {
    transport.tracing = false;
    Serial.printf("[TLS transport] final TX=%lu RX=%lu\n",
      static_cast<unsigned long>(transport.sent), static_cast<unsigned long>(transport.received));
    Serial.println("[FAIL] TLS/TCP connection. Check clock, trust anchor, data and power");
    return 0;
  }
  transport.tracing = false;
  Serial.println("[PASS] Verified TLS connection; sending sample packet");
  HttpClient http(secure, UPLOAD_HOST, 443);
  // Reuse the TLS connection we just verified; request still explicitly closes it.
  http.connectionKeepAlive();
  http.setHttpResponseTimeout(30000);
  http.beginRequest();
  const int started = http.post(UPLOAD_PATH);
  if (started != 0) { secure.stop(); return 0; }
  http.sendHeader("Content-Type", "application/json");
  http.sendHeader("Content-Length", payload.length());
  http.sendHeader("Authorization", String("Bearer ") + GATEWAY_UPLOAD_KEY);
  http.sendHeader("X-Packet-Id", id);
  http.sendHeader("Connection", "close");
  http.beginBody();
  if (http.print(payload) != payload.length()) { secure.stop(); return 0; }
  http.endRequest();
  const int status = http.responseStatusCode();
  if (status != 200 && status != 201) {
    Serial.printf("[FAIL] HTTP %d; pending sample kept unchanged\n", status);
    secure.stop();
    return 0;
  }
  if (http.skipResponseHeaders() != 0 || http.contentLength() > 1024) {
    Serial.println("[FAIL] Invalid/oversized response headers");
    secure.stop(); return 0;
  }
  const int declaredLength = http.contentLength();
  String body;
  const uint32_t start = millis();
  // HttpClient decodes chunked responses. Bound both decoded body and duration.
  while (millis() - start < 30000 && !http.endOfBodyReached()) {
    if (http.available()) {
      const int byte = http.read();
      if (byte >= 0) body += static_cast<char>(byte);
      if (body.length() > 1024) { secure.stop(); return 0; }
    } else {
      if (!http.connected()) break;
      delay(10);
    }
  }
  // Chunked/close-delimited replies have no Content-Length in this library.
  const bool complete = declaredLength >= 0 ? http.endOfBodyReached()
    : (!http.connected() && http.available() == 0);
  secure.stop();
  StaticJsonDocument<512> reply;
  if (!complete || deserializeJson(reply, body) || !reply["saved"].is<bool>() ||
      reply["saved"].as<bool>() != true || !reply["duplicate"].is<bool>() ||
      reply["packet_id"].as<String>() != id) {
    Serial.println("[FAIL] Missing/invalid acknowledgement; pending sample kept");
    return 0;
  }
  const bool duplicate = reply["duplicate"].as<bool>();
  if ((status == 201 && duplicate) || (status == 200 && !duplicate)) return 0;
  Serial.printf("[PASS] HTTP %d: %s\n", status, duplicate ? "same sample already saved" : "new sample saved");
  return duplicate ? 2 : 1;
}

void runUploadTest() {
  Serial.println("\nWQM cellular HTTPS backend upload test (synthetic readings)");
  if (strlen(GATEWAY_UPLOAD_KEY) != 64) { Serial.println("[FAIL] Configure gateway key"); return; }
  if (!modem.init()) { Serial.println("[FAIL] Modem/SIM initialization"); return; }
  Serial.println("[PASS] Modem initialized and SIM ready");
  if (!modem.waitForNetwork(120000L)) { Serial.println("[FAIL] Network registration"); return; }
  Serial.println("[PASS] Registered on network");
  if (!modem.gprsConnect(SIM_APN, "", "")) { Serial.println("[FAIL] Cellular data connection"); return; }
  Serial.println("[PASS] Cellular TCP data ready");
  if (!checkPlainTcp()) return;
  if (!resolveUploadHost()) return;
  if (!setClock()) return;
  if (!pending.begin("wqm-upload", false)) { Serial.println("[FAIL] Pending storage"); return; }
  // Store ID + payload atomically as one NVS value before first transmission.
  String record = pending.getString("sample", "");
  if (!record.length()) {
    record = newPacketId() + "\n" + SAMPLE;
    if (pending.putString("sample", record) == 0) { pending.end(); return; }
  }
  const int split = record.indexOf('\n');
  if (split != 36 || record.length() > 4096) { Serial.println("[FAIL] Invalid pending record"); pending.end(); return; }
  const String id = record.substring(0, split);
  const String payload = record.substring(split + 1);
  Serial.println("Packet ID: " + id);
  // One attempt during diagnosis; use r to retry the same saved sample.
  const int first = upload(id, payload);
  if (first != 0 && upload(id, payload) == 2) {
    if (pending.remove("sample"))
      Serial.println("[PASS] Save + duplicate acknowledgement complete. Verify this ID in telemetry_packets");
    else Serial.println("[FAIL] Pending cleanup failed; rerun will use the same ID");
  } else {
    Serial.println("[FAIL] Test incomplete. Rerun/reboot reuses the same stored ID and data");
  }
  pending.end();
}

void setup() {
  Serial.begin(115200);
  modemSerial.setRxBufferSize(4096);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  gpio_set_pull_mode(static_cast<gpio_num_t>(MODEM_RX_PIN), GPIO_PULLUP_ONLY);
  secure.setCACert(UPLOAD_CA);
  // SSLClient uses a positive read timeout as its handshake timeout too.
  // Polling avoids waiting for a full TLS-sized read when the cellular
  // transport has only a partial record available. Overall limits stay below.
  secure.setTimeout(0);
  secure.setHandshakeTimeout(90);
  delay(3000);
  runUploadTest();
  Serial.println("Finished. Send r to rerun. A completed rerun creates a NEW synthetic sample.");
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') runUploadTest();
  delay(10);
}
