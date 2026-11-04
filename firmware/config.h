#pragma once

// Proposed ESP32 UART2 pins. Confirm HAT routing and wiring before powering up.
constexpr int MODEM_RX_PIN = 16; // ESP32 RX <- HAT TXD
constexpr int MODEM_TX_PIN = 17; // ESP32 TX -> HAT RXD
constexpr unsigned long MODEM_BAUD = 115200;

// STM32 sensor board on ESP32 UART1. GPIO 6-11 are the flash chip; avoid them.
constexpr int STM_RX_PIN = 25; // ESP32 RX <- STM32 TX
constexpr int STM_TX_PIN = 26; // ESP32 TX -> STM32 RX (ACK/NAK replies)
constexpr unsigned long STM_BAUD = 115200;

// Airtel India APN for the SIM currently under test. Change when switching operator.
constexpr const char *SIM_APN = "airtelgprs.com";

// Public HTTP probe only: no gateway key or sensor data is sent.
// HTTP success proves basic data connectivity, not authenticated HTTPS upload.
constexpr const char *CONNECTIVITY_URL = "http://example.com/";
