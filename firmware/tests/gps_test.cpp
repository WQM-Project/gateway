#include <Arduino.h>
#include <driver/gpio.h>
#include <TinyGsmClient.h>
#include "config.h"
#include "gps.h"

// GNSS diagnostic: no cellular data, no backend, no credentials.
// Prints the modem's latest GNSS reply and the decoded position every 5 seconds.
HardwareSerial modemSerial(2);
TinyGsm modem(modemSerial);
GpsTracker gps;
uint32_t started = 0, lastPrint = 0;

void setup() {
  Serial.begin(115200);
  modemSerial.setRxBufferSize(1024);
  modemSerial.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  gpio_set_pull_mode(static_cast<gpio_num_t>(MODEM_RX_PIN), GPIO_PULLUP_ONLY);
  delay(3000);
  Serial.println("\nWQM SIM7600 GNSS test. Put the antenna outdoors, label side up.");
  // init() only checks AT/SIM; GNSS does not need a SIM or mobile data.
  if (!modem.testAT(10000)) { Serial.println("[FAIL] Modem not answering AT; check wiring/power"); return; }
  Serial.println("[PASS] Modem answering AT");
  gps.begin(modem, modemSerial);
  started = millis();
}

void loop() {
  if (!started) { delay(1000); return; }
  gps.poll();
  if (millis() - lastPrint < 5000) return;
  lastPrint = millis();
  const uint32_t t = (millis() - started) / 1000;
  // Empty fields (",,,,") mean no fix yet; this is normal for the first minutes.
  Serial.printf("[%4lus] raw: %s\n", static_cast<unsigned long>(t), gps.raw().c_str());
  const GpsSnapshot now = gps.snapshot();
  if (now.fix)
    Serial.printf("        FIX %.6f, %.6f  age %lus  map: https://maps.google.com/?q=%.6f,%.6f\n",
      now.lat, now.lon, static_cast<unsigned long>(now.ageSeconds), now.lat, now.lon);
  else
    Serial.println("        no fix yet");
}
