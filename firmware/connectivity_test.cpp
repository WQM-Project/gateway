#include <Arduino.h>
#include <stdio.h>
#include "config.h"
#include "ModemAT.h"

HardwareSerial modem(2);

bool registered(const String &response) {
  const int position = response.indexOf("+CEREG:");
  if (position < 0) return false;
  int mode = -1, status = -1;
  if (sscanf(response.substring(position).c_str(), "+CEREG: %d,%d", &mode, &status) != 2)
    return false;
  return status == 1 || status == 5; // home or roaming
}

void runConnectivityTest() {
  Serial.println("\nWQM cellular connectivity test");
  if (!sendAt(modem, "AT").success) {
    Serial.println("[FAIL] Check modem power, UART routing, baud and RX/TX wiring");
    return;
  }
  if (!sendAt(modem, "ATE0").success) return;
  sendAt(modem, "ATI");
  const AtResult sim = sendAt(modem, "AT+CPIN?");
  if (!sim.success || sim.response.indexOf("+CPIN: READY") < 0) {
    Serial.println("[FAIL] SIM not ready. Check card insertion/PIN status");
    return;
  }
  Serial.println("[PASS] SIM ready");
  sendAt(modem, "AT+CSQ"); // diagnostic; signal alone is not internet
  sendAt(modem, "AT+CPSI?");
  bool onNetwork = false;
  const unsigned long start = millis();
  while (millis() - start < 120000) {
    const AtResult registration = sendAt(modem, "AT+CEREG?");
    if (registration.success && registered(registration.response)) {
      onNetwork = true;
      break;
    }
    delay(3000);
  }
  if (!onNetwork) {
    Serial.println("[FAIL] LTE registration timed out. Check antenna, SIM and coverage");
    return;
  }
  Serial.println("[PASS] Registered on LTE network");
  const String apn = SIM_APN;
  if (!apn.length() || apn == "CHANGE_ME" || apn.indexOf('"') >= 0 ||
      apn.indexOf('\r') >= 0 || apn.indexOf('\n') >= 0) {
    Serial.println("[FAIL] Set your SIM operator APN in firmware/config.h");
    return;
  }
  if (!sendAt(modem, "AT+CGDCONT=1,\"IP\",\"" + apn + "\"").success) return;
  if (!sendAt(modem, "AT+CGATT=1", 60000).success) return;
  // Clear any HTTP session left by a previous interrupted test.
  sendAt(modem, "AT+HTTPTERM"); // ERROR is normal if no session exists
  if (!sendAt(modem, "AT+HTTPINIT", 60000).success) return;
  const String url = CONNECTIVITY_URL;
  if (!url.startsWith("http://") || url.indexOf('"') >= 0 ||
      url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0) {
    Serial.println("[FAIL] Configure a public HTTP probe URL");
    sendAt(modem, "AT+HTTPTERM");
    return;
  }
  if (!sendAt(modem, "AT+HTTPPARA=\"URL\",\"" + url + "\"").success) {
    sendAt(modem, "AT+HTTPTERM");
    return;
  }
  // HTTPACTION first returns OK; only the later URC contains the HTTP outcome.
  const AtResult action = sendAt(modem, "AT+HTTPACTION=0", 60000, "+HTTPACTION:");
  int method = -1, status = -1, bytes = -1;
  const int position = action.response.indexOf("+HTTPACTION:");
  const bool parsed = position >= 0 &&
    sscanf(action.response.substring(position).c_str(), "+HTTPACTION: %d,%d,%d", &method, &status, &bytes) == 3;
  if (action.success && parsed && method == 0 && status >= 200 && status < 300 && bytes > 0) {
    Serial.printf("[PASS] Mobile internet: HTTP %d, %d response bytes\n", status, bytes);
    Serial.println("HTTPS certificate validation and backend upload are separate tests");
  } else {
    Serial.printf("[FAIL] HTTP probe: status=%d bytes=%d; check APN/data plan/endpoint\n", status, bytes);
  }
  sendAt(modem, "AT+HTTPTERM");
}

void setup() {
  Serial.begin(115200);
  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(3000);
  runConnectivityTest();
  Serial.println("Test finished. Send r to rerun after correcting setup.");
}

void loop() {
  if (Serial.available() && Serial.read() == 'r') runConnectivityTest();
  delay(10);
}
