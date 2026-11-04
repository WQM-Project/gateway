#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <TinyGsmClient.h>
#include "config.h"
#include "gps.h"
#include "stm_link.h"
#include "packet_queue.h"
#include "uploader.h"

// WQM gateway: STM32 JSON (UART1) -> validate + GPS stamp -> flash queue
// -> SIM7600 LTE -> HTTPS ingest. The main loop owns the modem UART (GPS
// polls and uploads); a separate task owns the STM32 UART.
HardwareSerial modemSerial(2);
HardwareSerial stmSerial(1);
TinyGsm modem(modemSerial);
GpsTracker gps;
PacketQueue packets;
Uploader uploader(modem, modemSerial);

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

// Replies go back on the port the line came from.
void handleLine(LineReader &reader, LineReader::Result got, Stream &port) {
  if (got == LineReader::TOO_LONG) {
    port.print("NAK too_long\n");
    Serial.println("[STM] Line over 512 bytes discarded");
  } else if (got == LineReader::LINE) {
    String packet;
    const char *reason = "";
    if (!buildPacket(reader.line(), gps.snapshot(), packet, reason)) {
      port.printf("NAK %s\n", reason);
      Serial.printf("[STM] Rejected packet: %s\n", reason);
    } else if (!packets.push(newPacketId() + "\n" + packet)) {
      port.print("NAK storage\n");
      Serial.println("[STM] Flash write failed");
    } else {
      port.print("ACK\n");
      Serial.printf("[STM] Queued (%lu waiting): %s\n", static_cast<unsigned long>(packets.size()), packet.c_str());
    }
  }
}

void stmTask(void *) {
  LineReader stm, usb;
  for (;;) {
    handleLine(stm, stm.feed(stmSerial), stmSerial);
#ifdef WQM_USB_PACKETS
    // Bench build only: packets sent over USB take the same path as the STM32's.
    handleLine(usb, usb.feed(Serial), Serial);
#endif
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// Self-healing limits. Backoff reaches 8 failures after roughly 15 minutes.
constexpr uint32_t WATCHDOG_S = 600;                  // longest blocking modem step is ~5 min
constexpr uint8_t FAILURES_BEFORE_MODEM_RESET = 8;
constexpr uint32_t STUCK_RESTART_MS = 60UL * 60000;   // queue not moving for 1 h -> reboot ESP32
constexpr uint32_t MODEM_DOWN_RESTART_MS = 10UL * 60000;

bool modemReady = false;
uint8_t failures = 0;
uint32_t nextUpload = 0, backoffMs = 0, nextModemTry = 0, lastStatus = 0;
uint32_t lastProgress = 0, modemDownSince = 0;

// Wrap-safe "has this millis() deadline passed" (millis() wraps after 49 days).
bool due(uint32_t deadline) { return static_cast<int32_t>(millis() - deadline) >= 0; }

void rebootEsp(const char *why) {
  Serial.printf("[Recovery] %s; restarting ESP32 (queued packets are kept in flash)\n", why);
  delay(200);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  modemSerial.setRxBufferSize(4096);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  gpio_set_pull_mode(static_cast<gpio_num_t>(MODEM_RX_PIN), GPIO_PULLUP_ONLY);
  stmSerial.setRxBufferSize(2048);
  stmSerial.begin(STM_BAUD, SERIAL_8N1, STM_RX_PIN, STM_TX_PIN);
  Serial.println("\nWQM gateway starting");
  if (!packets.begin()) { Serial.println("[FAIL] Flash queue unavailable; halting"); for (;;) delay(1000); }
  uploader.begin();
  // Accept STM32 packets immediately, even before the modem is up.
  xTaskCreatePinnedToCore(stmTask, "stm", 8192, nullptr, 2, nullptr, 0);
  // Hardware watchdog: if loop() stops running (a hang, not a slow modem
  // step), the chip reboots itself after WATCHDOG_S.
  esp_task_wdt_init(WATCHDOG_S, true);
  esp_task_wdt_add(nullptr);
  delay(3000);
}

void loop() {
  esp_task_wdt_reset();
  if (!modemReady) {
    if (!modemDownSince) modemDownSince = millis();
    if (millis() - modemDownSince > MODEM_DOWN_RESTART_MS) rebootEsp("Modem silent for 10 min");
    if (!due(nextModemTry)) { delay(50); return; }
    nextModemTry = millis() + 10000;
    if (!modem.init()) { Serial.println("[Modem] Not responding; retrying in 10 s"); return; }
    Serial.println("[Modem] Ready");
    gps.begin(modem, modemSerial);
    modemReady = true;
    modemDownSince = 0;
  }
  gps.poll();

  if (!packets.size()) lastProgress = millis();  // nothing waiting is not "stuck"
  else if (millis() - lastProgress > STUCK_RESTART_MS) rebootEsp("No successful upload for 1 h");

  if (packets.size() && due(nextUpload)) {
    String record;
    Uploader::Result result = Uploader::RETRY;
    if (packets.peek(record) && uploader.ensureOnline()) {
      const String id = record.substring(0, 36);
      result = uploader.send(id, record.substring(37));
      if (result != Uploader::RETRY) { packets.pop(); lastProgress = millis(); }
      if (result == Uploader::SAVED) Serial.printf("[Upload] Saved %s (%lu left)\n", id.c_str(), static_cast<unsigned long>(packets.size()));
      if (result == Uploader::DUPLICATE) Serial.printf("[Upload] Already saved %s (%lu left)\n", id.c_str(), static_cast<unsigned long>(packets.size()));
    }
    if (result == Uploader::RETRY) {
      // 5 s, 10 s, 20 s ... capped at 5 min, so a dead network is not hammered.
      backoffMs = backoffMs ? min<uint32_t>(backoffMs * 2, 300000) : 5000;
      nextUpload = millis() + backoffMs;
      Serial.printf("[Upload] Retrying in %lu s\n", static_cast<unsigned long>(backoffMs / 1000));
      if (++failures >= FAILURES_BEFORE_MODEM_RESET) {
        // Like switching a phone off and on: clears a stuck radio or data session.
        Serial.println("[Recovery] Uploads failing repeatedly; resetting modem (AT+CRESET)");
        failures = 0;
        uploader.reset();
        modem.restart();  // ~35 s; GNSS turns off and is re-enabled on the next loop
        modemReady = false;
      }
    } else {
      backoffMs = 0;
      failures = 0;
    }
  }

  if (millis() - lastStatus >= 60000) {
    lastStatus = millis();
    const GpsSnapshot fix = gps.snapshot();
    Serial.printf("[Status] up=%lus queue=%lu gps=%s age=%lus failures=%u clock=%s\n",
      static_cast<unsigned long>(millis() / 1000), static_cast<unsigned long>(packets.size()),
      fix.fix ? "fix" : "none", static_cast<unsigned long>(fix.ageSeconds), failures,
      GpsTracker::clockValid() ? "set" : "unset");
  }
  delay(10);
}
