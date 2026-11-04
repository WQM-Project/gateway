#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>
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
    // Bench testing: packets sent over USB take the same path as the STM32's.
    handleLine(usb, usb.feed(Serial), Serial);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool modemReady = false;
uint32_t nextUpload = 0, backoffMs = 0, nextModemTry = 0, lastStatus = 0;

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
  delay(3000);
}

void loop() {
  if (!modemReady) {
    if (millis() < nextModemTry) { delay(50); return; }
    nextModemTry = millis() + 10000;
    if (!modem.init()) { Serial.println("[Modem] Not responding; retrying in 10 s"); return; }
    Serial.println("[Modem] Ready");
    gps.begin(modem, modemSerial);
    modemReady = true;
  }
  gps.poll();

  if (packets.size() && millis() >= nextUpload) {
    String record;
    Uploader::Result result = Uploader::RETRY;
    if (packets.peek(record) && uploader.ensureOnline()) {
      const String id = record.substring(0, 36);
      result = uploader.send(id, record.substring(37));
      if (result != Uploader::RETRY) packets.pop();
      if (result == Uploader::SAVED) Serial.printf("[Upload] Saved %s (%lu left)\n", id.c_str(), static_cast<unsigned long>(packets.size()));
      if (result == Uploader::DUPLICATE) Serial.printf("[Upload] Already saved %s (%lu left)\n", id.c_str(), static_cast<unsigned long>(packets.size()));
    }
    if (result == Uploader::RETRY) {
      // 5 s, 10 s, 20 s ... capped at 5 min, so a dead network is not hammered.
      backoffMs = backoffMs ? min<uint32_t>(backoffMs * 2, 300000) : 5000;
      nextUpload = millis() + backoffMs;
      Serial.printf("[Upload] Retrying in %lu s\n", static_cast<unsigned long>(backoffMs / 1000));
    } else {
      backoffMs = 0;
    }
  }

  if (millis() - lastStatus >= 60000) {
    lastStatus = millis();
    const GpsSnapshot fix = gps.snapshot();
    Serial.printf("[Status] queue=%lu gps=%s age=%lus\n", static_cast<unsigned long>(packets.size()),
      fix.fix ? "fix" : "none", static_cast<unsigned long>(fix.ageSeconds));
  }
  delay(10);
}
