#pragma once

// Proposed ESP32 UART2 pins. Confirm HAT routing and wiring before powering up.
constexpr int MODEM_RX_PIN = 16; // ESP32 RX <- HAT TXD
constexpr int MODEM_TX_PIN = 17; // ESP32 TX -> HAT RXD
constexpr unsigned long MODEM_BAUD = 115200;

// Replace with the APN supplied by the SIM operator. No operator is assumed.
constexpr const char *SIM_APN = "CHANGE_ME";

// Public HTTP probe only: no gateway key or sensor data is sent.
// HTTP success proves basic data connectivity, not authenticated HTTPS upload.
constexpr const char *CONNECTIVITY_URL = "http://example.com/";
