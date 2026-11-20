#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <sys/stat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Packets waiting for upload, one LittleFS file each: /q/<sequence>.
// Record = "<packet uuid>\n<json>". head = oldest unsent, tail = next free.
// A packet is acknowledged to the STM32 only after its file is complete.
class PacketQueue {
 public:
  // Small on purpose: a long outage should not pile up a huge backlog. When
  // full, new packets are refused (the STM32 gets "NAK storage") until uploads
  // drain the queue or it is cleared with 'c' on the USB serial monitor.
  static constexpr uint32_t CAPACITY = 50;

  bool begin() {
    lock = xSemaphoreCreateMutex();
    if (!lock || !LittleFS.begin(true)) return false;  // formats on first boot
    if (!present("/q") && !LittleFS.mkdir("/q")) return false;
    if (!counters.begin("wqm-queue", false)) return false;
    head = counters.getUInt("head", 0);
    tail = counters.getUInt("tail", 0);
    // Power loss between rename and counter save leaves a complete file at tail.
    while (present(path(tail))) ++tail;
    if (head > tail) head = tail;
    save();
    Serial.printf("[Queue] %lu packet(s) waiting from before reboot\n", static_cast<unsigned long>(size()));
    return true;
  }

  bool push(const String &record) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (tail - head >= CAPACITY) {
      // Full: keep what is already queued and refuse the new packet.
      xSemaphoreGive(lock);
      Serial.printf("[Queue] Full (%lu); new packet refused. Send 'c' to clear.\n", static_cast<unsigned long>(CAPACITY));
      return false;
    }
    File file = LittleFS.open("/q/tmp", "w");
    const bool written = file && file.print(record) == record.length();
    if (file) file.close();
    const bool stored = written && LittleFS.rename("/q/tmp", path(tail));
    if (stored) { ++tail; save(); }
    xSemaphoreGive(lock);
    return stored;
  }

  // Oldest waiting record, skipping any file lost to corruption.
  bool peek(String &record) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool found = false;
    while (!found && head < tail) {
      File file = LittleFS.open(path(head), "r");
      if (file) { record = file.readString(); file.close(); found = record.length() > 37; }
      if (!found) { LittleFS.remove(path(head)); ++head; save(); }
    }
    xSemaphoreGive(lock);
    return found;
  }

  void pop() {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (head < tail) { LittleFS.remove(path(head)); ++head; save(); }
    xSemaphoreGive(lock);
  }

  // Wipe every queued packet and reset counters to zero.
  void clear() {
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t removed = 0;
    while (head < tail) { LittleFS.remove(path(head)); ++head; ++removed; }
    head = 0; tail = 0; save();
    xSemaphoreGive(lock);
    Serial.printf("[Queue] Cleared %lu packet(s)\n", static_cast<unsigned long>(removed));
  }

  uint32_t size() { return tail - head; }

 private:
  static String path(uint32_t sequence) { return "/q/" + String(sequence); }
  // LittleFS.exists() logs an error for every missing file; a missing file is
  // the normal answer here, so ask the VFS directly and quietly.
  static bool present(const String &file) {
    struct stat info;
    return stat(("/littlefs" + file).c_str(), &info) == 0;
  }
  void save() { counters.putUInt("head", head); counters.putUInt("tail", tail); }
  SemaphoreHandle_t lock = nullptr;
  Preferences counters;
  uint32_t head = 0, tail = 0;
};
