# WQM Gateway — Modem Connectivity and Upload Setup

Target: ESP32-WROOM-32 development board plus Waveshare SIM7600X HAT fitted
with SIM7600G-H, part of the WQM-Project ecosystem. Earlier planning listed
ESP8266 + SIM7600E; these diagnostics target the supplied ESP32 + SIM7600G-H.

## Files and scope

- `firmware/modem_bridge.cpp`: manually send modem commands through the ESP32.
- `firmware/connectivity_test.cpp`: check AT communication, SIM readiness,
  LTE registration, APN/data attachment and a real public HTTP response.
- `firmware/uart_loopback_test.cpp`: automatically verify the ESP32 UART with
  GPIO17 connected directly to GPIO16 and the modem disconnected.
- `firmware/config.h`: proposed UART pins, modem baud, operator APN and probe URL.
- `platformio.ini`: separate build/upload targets for both programs.

These are diagnostic programs, not the complete LoRa receiver/upload firmware.
The HTTP probe sends no readings or authentication keys. HTTPS/TLS and backend
POST tests still need integration. The backend work belongs to
[wqm-web-dashboard](https://github.com/WQM-Project/wqm-web-dashboard).

## Before wiring

### Equipment required

- ESP32-WROOM-32 development board and Waveshare SIM7600G-H HAT.
- Active compatible SIM with mobile data and the operator's APN.
- Cellular antenna, three jumper wires, and a Micro-USB data cable for ESP32.
- Separate stable regulated 5 V USB supply suitable for powering the HAT.
- VS Code with PlatformIO IDE, or PlatformIO Core.

HAT means “Hardware Attached on Top”: here it is the blue carrier board with
the modem, SIM holder, USB ports and power circuitry. No Raspberry Pi is
required. LoRa hardware is needed later for boat reception, not for this test.

Use the HAT's labelled UART header and verify its routing/jumper configuration
against the manual for this board revision. Do not assume the current jumpers
connect that header to the modem. Use 3.3 V UART logic for the ESP32.

Proposed signal connections after verification:

| ESP32       | HAT |
| ----------- | --- |
| GPIO16 (RX) | TXD |
| GPIO17 (TX) | RXD |
| GND         | GND |

```text
ESP32 GPIO16 (RX) <-------- HAT TXD
ESP32 GPIO17 (TX) --------> HAT RXD
ESP32 GND ---------------- HAT GND
```

TX means transmit and RX means receive, so the signals cross. Use the ESP32
pins labelled `16` and `17`; its pins labelled simply RX/TX are used by its
USB serial connection. Use the HAT's labelled control header, not bare modem pins.

### UART jumpers

The `UART JMP` caps route signals between the modem, onboard CP2102 USB
converter and Raspberry Pi interface. For direct header operation, isolate
the other serial interfaces. On the published G-H HAT arrangement, remove
the two UART routing caps while unpowered and keep them for later use.
Verify the board revision against the
[G-H schematic](https://files.waveshare.com/upload/6/63/SIM7600G-H-4G-HAT-schematic.pdf).
The published revision defaults to translated 3.3 V UART signals; if a labelled
UART voltage selector is present, select 3.3 V. Leave unrelated jumpers alone.

### Power-on sequence

1. Disconnect power before wiring, moving jumpers or inserting the SIM.
2. Insert the SIM underneath the HAT and attach the antenna to **MAIN**.
   AUX and GNSS are unused for this initial diagnostic.
3. Connect the ESP32 Micro-USB port to the laptop using a data cable.
4. Power the HAT separately through its port labelled **USB**, using a suitable
   5 V USB supply. `USB TO UART` is a different serial path, unused here.
5. If the modem is not running, follow its PWRKEY procedure. The HAT-family
   manual specifies holding PWRKEY for about one second, then releasing it.
6. Run the bridge and confirm `AT` returns `OK`.

With separate USB power, do not join the boards' 5 V/3V3 power pins. Leave PWR,
FLIGHT, DTR, RTS, CTS, RI and DCD unconnected for this initial setup.

Power the HAT using a supply appropriate to its documented requirements;
do not power a cellular modem from the ESP32's 3.3 V pin. Connect the LTE
antenna to MAIN and insert the SIM while unpowered. Power on the modem using
the HAT's documented PWRKEY procedure before running the test. These programs
do not control PWRKEY or change jumper settings. Never connect ESP GPIO to 5 V.

## Configuration

The settings in [`firmware/config.h`](firmware/config.h) are:

```cpp
constexpr int MODEM_RX_PIN = 16;
constexpr int MODEM_TX_PIN = 17;
constexpr unsigned long MODEM_BAUD = 115200;
constexpr const char *SIM_APN = "airtelgprs.com";
constexpr const char *CONNECTIVITY_URL = "http://example.com/";
```

The current SIM is Airtel India and the working APN is `airtelgprs.com`.
Change this value to the actual operator APN when switching SIM providers.
Rebuild and upload after configuration changes. The diagnostic configures an
IPv4 PDP profile with APN only. SIM PIN entry, APN username/password, or a
different PDP profile require additional configuration and are not automated.

The probe must be a public HTTP URL returning a body without redirection.
It proves basic cellular internet access; HTTPS certificate validation and
authenticated backend upload are separate integration work. Send no private
readings or credentials to this HTTP probe.

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

| Manual command | What it checks           | Expected indication            |
| -------------- | ------------------------ | ------------------------------ |
| `AT`         | UART/modem communication | `OK`                         |
| `ATI`        | Modem model and firmware | Identity text and`OK`        |
| `AT+CPIN?`   | SIM readiness            | `+CPIN: READY`               |
| `AT+CSQ`     | Signal quality           | `+CSQ: ...`                  |
| `AT+CEREG?`  | LTE registration         | Status 1 (home) or 5 (roaming) |
| `AT+CPSI?`   | Serving network          | Network/system information     |

For example, `+CEREG: 0,1` means registered on the home network. Registration
or signal strength alone does not prove internet connectivity.

If several serial devices are attached, use `pio device list` to find the
ESP32 port. Add `--upload-port COMx` to upload commands or `--port COMx` to
monitor commands, using the actual port. Close the monitor with Ctrl+C before
uploading. In VS Code, run these commands from a PlatformIO terminal.

### Automatic connectivity diagnostic

```text
pio run -e connectivity_test
pio run -e connectivity_test -t upload
pio device monitor --baud 115200
```

The connectivity test runs once and prints PASS/FAIL. Send `r` to repeat.
Registration is allowed up to two minutes; data attachment/HTTP can each take
up to one minute. A SIM requiring an APN username/password needs additional
operator-specific authentication configuration; that is not implemented here.

The program runs this sequence:

1. Check `AT`, disable command echo, and print modem identity.
2. Check SIM readiness; print signal and network details.
3. Poll LTE registration for up to two minutes.
4. Configure the APN and request packet-data attachment.
5. Clear an old HTTP session and initialise a new session.
6. Set the probe URL and issue a GET request.
7. Wait for the asynchronous HTTP result and report PASS/FAIL.
8. Close the HTTP session and wait for lowercase `r` to rerun.

Example successful output, interleaved with modem responses:

```text
[PASS] SIM ready
[PASS] Registered on LTE network
...
+HTTPACTION: 0,200,1256
[PASS] Mobile internet: HTTP 200, 1256 response bytes
HTTPS certificate validation and backend upload are separate tests
Test finished. Send r to rerun after correcting setup.
```

Response length varies. If startup output is missed, reset the ESP32 with the
monitor open. A preliminary `AT+HTTPTERM` error is normal when no previous
HTTP session exists. Redirects, empty responses, timeouts and modem error
statuses do not pass the diagnostic.

## What counts as passing

### Automatic ESP32 UART loopback check

If typed characters cannot be distinguished from local terminal echo, test
the UART independently. With both boards unpowered, disconnect the modem
completely and join ESP32 GPIO17 directly to GPIO16 using one jumper. Power
only the ESP32 through USB and run:

```text
pio run -e uart_loopback_test -t upload
pio device monitor --baud 115200
```

This program sends its own changing message through UART2 and compares received
bytes. No typing is required. A PASS establishes a matching round trip through
GPIO17 and GPIO16; FAIL reports how many bytes arrived. After the test, disconnect
power, remove the loopback jumper, reconnect the modem wiring, and upload
`modem_bridge` again. This check does not establish modem communication.

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

| Deliverable                              | Current state          |
| ---------------------------------------- | ---------------------- |
| Manual modem bridge                      | Compiled, flashed; AT returned OK |
| Automatic ESP32 UART loopback             | Passed; 18 matching bytes received |
| SIM/LTE/HTTP diagnostic                  | Compiled, flashed; cellular HTTP passed |
| Physical wiring and modem operation      | Verified on supplied hardware |
| LoRa packet receiver                     | Pending implementation |
| Secure backend upload                    | Pending implementation |
| Continuous operation, queues and retries | Pending implementation |

### Hardware results — 9 October 2026

The supplied ESP32 and SIM7600G-H were tested using PlatformIO, UART2 on
GPIO16/17 at 115200 baud, an Airtel SIM, and the MAIN cellular antenna:

```text
Automatic UART loopback: 18 bytes matched
AT: OK
AT+CPIN?: +CPIN: READY
AT+CEREG?: +CEREG: 0,5
AT+COPS?: Airtel, access technology 7 (LTE)
AT+HTTPACTION=0: +HTTPACTION: 0,200,577
[PASS] Mobile internet: HTTP 200, 577 response bytes
```

The modem reported firmware `SIM7600G_V2.0.2`. Registration status 5 means
roaming registration. The HTTP result proves that a request reached
`http://example.com/` through the SIM and returned a response. It does not
prove HTTPS certificate validation or delivery to the WQM database.

The bridge and connectivity diagnostic enable an internal RX pull-up through
`driver/gpio.h`, which is included in the ESP32 framework. This keeps a
disconnected RX input at the UART idle-high level. No separate library install
is required. Earlier garbled output suggested floating-input noise, but the
exact source of every earlier corrupted byte was not established.

Upload troubleshooting also established that BOOT held during EN reset can
select download mode. Opening a source file does not change the running
firmware: upload the desired PlatformIO environment. A loopback failure after
removing the loopback jumper is expected; upload `modem_bridge` for modem tests.

The full gateway must eventually receive complete edge packets, validate them,
upload over authenticated HTTPS, check server acknowledgements and retry using
the same packet identity. The current diagnostic runs once and waits; it does
not continuously receive or upload water-quality readings.

Record separate results for compilation, UART communication, SIM registration,
HTTP connectivity, later HTTPS upload/database verification, and the complete
boat-to-server journey. Laptop backend tests do not verify the modem path.

## Planned HTTPS upload through Supabase

The proposed public gateway upload path is:

```text
ESP32 -> UART -> SIM7600G-H -> Airtel mobile internet
      -> HTTPS ingestion Edge Function -> WQM Postgres telemetry_packets
```

WQM project URL:

```text
https://scvcufiagvbeixhnwjze.supabase.co
```

Proposed upload endpoint after deploying an Edge Function named `ingest`:

```text
https://scvcufiagvbeixhnwjze.supabase.co/functions/v1/ingest
```

**The Edge Function and modem HTTPS upload are pending. This endpoint is not
yet deployed or verified.** The existing Next.js `POST /api/ingest` lives in
the dashboard backend; it is not automatically available at the Supabase URL.
The modem cannot access a laptop's `localhost` address over the mobile network.

The WQM database table and `ingest_telemetry(uuid,jsonb)` function have already
been tested from the laptop backend: new uploads, identical retries and
conflicting retries behaved as expected. This is a separate result from the
physical modem connectivity test.

### Upload contract to preserve

- Send the complete 14-field STM32 JSON packet with `Content-Type: application/json`.
- Authenticate using a dedicated gateway upload key.
- Send a new UUID in `X-Packet-Id` for each new reading. Preserve the same UUID
  and unchanged data through retries; do not use boot-relative uptime as the ID.
- Preserve `ts` as node uptime in milliseconds and conductivity in microSiemens/cm.
- Verify the HTTPS server certificate using the appropriate trusted CA and
  correct modem time. Do not disable certificate verification to force a pass.
- Remove a reading from the pending queue only after a valid acknowledgement.

Expected application responses after the public endpoint is implemented:

| HTTP status | Meaning |
| --- | --- |
| 201 | New reading saved |
| 200 | Identical reading already saved |
| 401 / 403 | Wrong gateway credential or unauthorised node |
| 409 | Same packet identity reused with different data |
| 400 / 415 / 422 | Invalid request, format or telemetry |
| 413 | Request too large |
| 503 / network timeout | Retry unchanged data and ID with backoff |

### Credential separation

The Supabase project URL is public. The gateway should hold only its own
limited upload credential. **Never put the database password or Supabase
service-role/secret key on the ESP32.** Those credentials remain on the backend.
Keep device credentials in an ignored local configuration file, not tracked
source or serial logs. Do not place authentication keys in query-string URLs.

### Remaining upload checkpoints

1. Implement and deploy the authenticated `ingest` Edge Function in WQM.
2. Verify its public URL and request/response contract from the laptop.
3. Configure modem TLS trust, time and the HTTPS test program.
4. Upload one known reading through the SIM and verify the exact database row.
5. Retry that same packet and confirm only one row exists.
6. Record results, then integrate LoRa reception and continuous queue/retry logic.

## Troubleshooting

| Symptom                   | Check                                                                 |
| ------------------------- | --------------------------------------------------------------------- |
| ESP32 COM port missing    | Data cable, USB port and board USB-serial driver                      |
| Upload fails / port busy  | Correct COM port; close serial monitors                               |
| No`OK` from `AT`      | Modem power, PWRKEY, common GND, crossed TX/RX, UART routing and baud |
| Garbled text              | Baud settings, common ground, RX pull-up, wiring and modem power |
| SIM not ready             | Card insertion, active SIM and PIN status                             |
| Registration timeout      | MAIN antenna, coverage, SIM service and network compatibility         |
| APN configuration failure | Confirm the APN matches the inserted SIM operator |
| Registered but HTTP fails | Data plan, APN/profile requirements and probe endpoint                |
| HTTP 3xx                  | Redirect; choose an HTTP probe returning a body directly              |
| Repeated modem resets     | Supply capacity, USB cable and connections                            |

Save the full serial log and `ATI` output when debugging. Record the board
revision, operator, APN and wiring. Keep credentials out of shared logs.

## References

- [Waveshare SIM7600G-H HAT schematic](https://files.waveshare.com/upload/6/63/SIM7600G-H-4G-HAT-schematic.pdf)
- [Waveshare HAT-family manual](https://files.waveshare.com/upload/6/6d/SIM7600E-H-4G-HAT-Manual-EN.pdf): older E-H model; verify revision-specific details against the G-H schematic.

Command reference: SIMCom SIM7500/SIM7600 HTTP AT command manual:
https://simcom.ee/documents/SIM7X00/SIM7500_SIM7600_SIM7800%20Series_HTTP_AT%20Command%20Manual_V1.00.pdf
