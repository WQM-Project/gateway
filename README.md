# WQM gateway hardware diagnostics

Target: ESP32-WROOM-32 development board plus Waveshare SIM7600X HAT fitted
with SIM7600G-H, part of the WQM-Project ecosystem. Earlier planning listed
ESP8266 + SIM7600E; these diagnostics target the supplied ESP32 + SIM7600G-H.

## Files and scope

- `firmware/modem_bridge.cpp`: manually send modem commands through the ESP32.
- `firmware/connectivity_test.cpp`: check AT communication, SIM readiness,
  LTE registration, APN/data attachment and a real public HTTP response.
- `firmware/config.h`: proposed UART pins, modem baud, operator APN and probe URL.
- `platformio.ini`: separate build/upload targets for both programs.

These are diagnostic programs, not the complete LoRa receiver/upload firmware.
The HTTP probe sends no readings or authentication keys. HTTPS/TLS and backend
POST tests still need integration. The backend work belongs to
[wqm-web-dashboard](https://github.com/WQM-Project/wqm-web-dashboard).

## Before wiring

Use the HAT's labelled UART header and verify its routing/jumper configuration
against the manual for this board revision. Do not assume the current jumpers
connect that header to the modem. Use 3.3 V UART logic for the ESP32.

Proposed signal connections after verification:

| ESP32 | HAT |
| --- | --- |
| GPIO16 (RX) | TXD |
| GPIO17 (TX) | RXD |
| GND | GND |

Power the HAT using a supply appropriate to its documented requirements;
do not power a cellular modem from the ESP32's 3.3 V pin. Connect the LTE
antenna to MAIN and insert the SIM while unpowered. Power on the modem using
the HAT's documented PWRKEY procedure before running the test. These programs
do not control PWRKEY or change jumper settings. Never connect ESP GPIO to 5 V.

## Run with PlatformIO

Install the PlatformIO extension in VS Code if needed, open this folder,
and edit `SIM_APN` in `firmware/config.h` using your operator's actual APN.
The public probe defaults to `http://example.com/`; it is configurable.

Start with the serial bridge:

```text
pio run -e modem_bridge
pio run -e modem_bridge -t upload
pio device monitor --baud 115200 --echo --eol CRLF
```

Send `AT`. Continue only once it replies `OK`. Then send `ATI`, `AT+CPIN?`,
`AT+CSQ`, `AT+CEREG?`, `AT+CPSI?` individually to inspect the modem.
Close the serial monitor before uploading another program.

```text
pio run -e connectivity_test
pio run -e connectivity_test -t upload
pio device monitor --baud 115200
```

The connectivity test runs once and prints PASS/FAIL. Send `r` to repeat.
Registration is allowed up to two minutes; data attachment/HTTP can each take
up to one minute. A SIM requiring an APN username/password needs additional
operator-specific authentication configuration; that is not implemented here.

## What counts as passing

- `AT` replies `OK`: UART/modem communication.
- `+CPIN: READY`: SIM available.
- `+CEREG` status 1 or 5: LTE registration.
- `+HTTPACTION: 0,200,<positive length>` (or another 2xx): actual HTTP data access.
- `OK` after `AT+HTTPACTION=0` alone is not success. The code waits for its
  asynchronous result and reports modem error statuses as failures.

An HTTP-only probe does not verify secure HTTPS connectivity or storage.
To complete the assignment, add packet reception and authenticated HTTPS
upload using the backend contract, then test with the real radios and server.

## Verification status

Source prepared without hardware. No PlatformIO/Arduino compiler was available
in this workspace when written, so compilation and physical operation remain
unverified. Save serial logs and modem firmware identity during the first run.

Command reference: SIMCom SIM7500/SIM7600 HTTP AT command manual:
https://simcom.ee/documents/SIM7X00/SIM7500_SIM7600_SIM7800%20Series_HTTP_AT%20Command%20Manual_V1.00.pdf
