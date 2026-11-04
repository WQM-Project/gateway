#include <Arduino.h>
#include <driver/gpio.h>
#include "config.h"

HardwareSerial testUart(2);
unsigned long sequence = 0;

void setup() {
  Serial.begin(115200);
  testUart.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  gpio_set_pull_mode(static_cast<gpio_num_t>(MODEM_RX_PIN), GPIO_PULLUP_ONLY);
  delay(1500);
  Serial.println("WQM automatic ESP32 UART loopback test");
  Serial.println("Disconnect modem completely. Jumper ESP32 GPIO17 to GPIO16.");
  Serial.println("No typing needed. Results repeat every few seconds.");
}

void loop() {
  while (testUart.available()) testUart.read();
  const String expected = String("WQM_UART_CHECK_") + (++sequence) + "\r\n";
  String received;
  testUart.print(expected);
  const unsigned long started = millis();
  while (millis() - started < 1000 && received.length() < expected.length()) {
    while (testUart.available() && received.length() < expected.length())
      received += static_cast<char>(testUart.read());
    delay(1);
  }
  if (received == expected) {
    Serial.printf("[PASS] Test %lu: UART17 -> jumper -> UART16; %u bytes matched\n",
                  sequence, static_cast<unsigned>(received.length()));
  } else {
    Serial.printf("[FAIL] Test %lu: expected %u bytes, received %u\n", sequence,
                  static_cast<unsigned>(expected.length()), static_cast<unsigned>(received.length()));
    Serial.println("Check jumper is on ESP32 pins 16 and 17 and makes good contact.");
  }
  delay(4000);
}
