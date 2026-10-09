#pragma once
#include <Arduino.h>

struct AtResult {
  bool success = false;
  String response;
};

// Diagnostic helper: bounded line/response storage and bounded waits.
// Not a general production modem driver or a binary HTTP-body reader.
inline AtResult sendAt(HardwareSerial &modem, const String &command,
                       unsigned long timeoutMs = 5000,
                       const char *completion = "OK") {
  while (modem.available()) modem.read();
  Serial.print("> "); Serial.println(command);
  modem.print(command); modem.print("\r\n");
  AtResult result;
  String line;
  const unsigned long started = millis();
  while (millis() - started < timeoutMs) {
    while (modem.available()) {
      const char c = modem.read();
      if (c == '\r') continue;
      if (c != '\n') {
        if (line.length() >= 512) {
          Serial.println("[FAIL] Modem response line too long");
          return result;
        }
        line += c;
        continue;
      }
      line.trim();
      if (line.length()) {
        Serial.println(line);
        if (result.response.length() + line.length() + 1 > 4096) {
          Serial.println("[FAIL] Modem response too large");
          return result;
        }
        result.response += line + "\n";
        if (line == "ERROR" || line.startsWith("+CME ERROR:") || line.startsWith("+CMS ERROR:"))
          return result;
        if ((String(completion) == "OK" && line == "OK") ||
            (String(completion) != "OK" && line.startsWith(completion))) {
          result.success = true;
          return result;
        }
      }
      line = "";
    }
    delay(1);
  }
  Serial.println("[FAIL] Modem response timed out");
  return result;
}
