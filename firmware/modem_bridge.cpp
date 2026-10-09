#include <Arduino.h>
#include "config.h"

HardwareSerial modem(2);

void setup() {
  Serial.begin(115200);
  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(1500);
  Serial.println("WQM ESP32 -> SIM7600G-H serial bridge");
  Serial.println("Use CR+LF line endings. Send AT; expect OK.");
  Serial.println("Then try ATI, AT+CPIN?, AT+CSQ, AT+CEREG?, AT+CPSI?");
}

void loop() {
  while (Serial.available()) modem.write(Serial.read());
  while (modem.available()) Serial.write(modem.read());
  delay(1);
}
