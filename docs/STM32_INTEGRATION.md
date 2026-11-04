# STM32 → ESP32 integration guide

This guide is for whoever writes the STM32 firmware. It covers everything the
STM32 must do so the ESP32 gateway accepts its sensor readings and uploads
them to the WQM dashboard. The ESP32 side is finished and tested; if the STM32
follows this guide, nothing else needs to change.

## 1. Big picture

```
STM32 (reads sensors)
  │  one JSON line per reading, over UART
  ▼
ESP32 gateway
  │  checks the packet, adds GPS position + time + unique packet ID,
  │  saves it to flash, replies "ACK"
  ▼
SIM7600 4G modem ──► Supabase database ──► WQM dashboard
```

The STM32 only has to **send one correct line of text per reading and wait
for the reply**. It does **not** deal with GPS, the internet, packet IDs or
retrying uploads; the ESP32 does all of that, including when there is no
mobile signal (packets wait in the ESP32's flash and upload later).

## 2. Words used in this guide

| Term | Meaning |
| --- | --- |
| **UART** | Simple serial link: bytes sent one after another over a TX wire and received on an RX wire. |
| **TX / RX** | Transmit / Receive. One board's TX goes to the other board's RX. |
| **Baud rate** | UART speed in bits per second. Both sides must use the same value. |
| **8N1** | 8 data bits, No parity, 1 stop bit — the standard UART frame. |
| **GND** | Ground, the shared 0 V reference. Boards must share it or signals are garbage. |
| **JSON** | Text data format: `{"name": value, ...}`. |
| **`\n`** | The newline character (byte `0x0A`). It marks the end of one packet. |
| **ACK** | "Acknowledge": the ESP32's reply meaning *received, valid and saved*. |
| **NAK** | "Negative acknowledge": the ESP32's reply meaning *rejected*, followed by a reason. |
| **Uptime** | Time since the STM32 powered on or reset. |

## 3. Wiring

| STM32 | ESP32 | Notes |
| --- | --- | --- |
| UART **TX** | **GPIO25** (ESP32 RX) | Readings STM32 → ESP32 |
| UART **RX** | **GPIO26** (ESP32 TX) | ACK/NAK replies ESP32 → STM32 |
| **GND** | **GND** | Required. Also shared with the modem and the power supply. |

- **3.3 V logic only.** Both chips use 3.3 V signals. Never connect a 5 V UART
  signal to the ESP32.
- Use any free STM32 USART/UART. Do **not** use the UART the STM32 already uses
  for its own debug printing unless you remove that printing.
- Do not connect the boards' 3.3 V or 5 V power pins to each other; power each
  board from the boat's 5 V supply as agreed for the build.

## 4. UART settings

| Setting | Value |
| --- | --- |
| Baud rate | **115200** (one-one-five-two-zero-zero) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Flow control | None |

In STM32CubeMX: USART → Mode *Asynchronous*, Baud Rate *115200 Bits/s*, Word
Length *8 Bits*, Parity *None*, Stop Bits *1*, Hardware Flow Control *Disable*.

## 5. Packet format

Each reading is **one JSON object on one line, ending with `\n`**.

```json
{"id":"WQM-001","ts":12000,"pH":7.12,"turb_ntu":42.5,"tds_ppm":310,"do_mgl":6.85,"ec_uscm":680.2,"orp_mv":215,"temp_w1":23.5,"temp_w2":23.48,"depth_cm":35.2,"temp_air":28.6,"humidity":65.3,"pressure_hpa":1013.25}
```

Rules:

1. **Exactly these 14 keys**, spelled exactly as below (case matters: `pH`).
   No missing keys and no extra keys.
2. Numbers are **JSON numbers**: `7.12`, not `"7.12"`.
3. **No `NaN`, `inf` or empty values.** Every value must be a real number.
4. One line, **at most 512 bytes** including the `\n`. The example is ~210 bytes.
5. Key order does not matter. Spaces are allowed but not needed.
6. Do **not** send `lat`, `lon`, `gps_fix`, `utc_s` or a packet ID. The ESP32
   adds them; a packet that contains them is rejected as `wrong_field_count`.
7. `\r\n` line endings are fine; the ESP32 ignores `\r`.

### Fields

| Key | Type | Unit | Meaning |
| --- | --- | --- | --- |
| `id` | string | – | Boat ID, **`"WQM-001"`**. Letters, digits, `_`, `-` only, max 64 chars. Must be registered on the server; ask before changing it. |
| `ts` | whole number | ms | STM32 uptime in milliseconds, e.g. `HAL_GetTick()`. 0 to 4294967295, no decimals. |
| `pH` | number | – | Water pH |
| `turb_ntu` | number | NTU | Turbidity |
| `tds_ppm` | number | ppm | Total dissolved solids |
| `do_mgl` | number | mg/L | Dissolved oxygen |
| `ec_uscm` | number | µS/cm | Electrical conductivity |
| `orp_mv` | number | mV | Oxidation-reduction potential |
| `temp_w1` | number | °C | Water temperature, probe 1 |
| `temp_w2` | number | °C | Water temperature, probe 2 |
| `depth_cm` | number | cm | Depth |
| `temp_air` | number | °C | Air temperature |
| `humidity` | number | % | Relative humidity |
| `pressure_hpa` | number | hPa | Air pressure |

`ts` is only uptime. The ESP32 attaches the real date and time (`utc_s`), so the
STM32 does not need a real-time clock.

**If a sensor fails:** the packet still needs a real number for that field
(the server currently rejects missing values). Agree with the team what to
send in that case before deploying.

## 6. Reply from the ESP32

After each line the ESP32 answers with one line, normally within a few
milliseconds:

| Reply | Meaning | What the STM32 should do |
| --- | --- | --- |
| `ACK` | Valid and saved in the ESP32's flash. It will upload by itself. | Done; go to the next reading. |
| `NAK storage` | Valid, but the ESP32 could not save it. | Resend the same line once or twice. |
| `NAK invalid_json` | Not valid JSON (missing quote/comma/brace, or `nan`). | Bug in the STM32 code. Log it; don't resend unchanged. |
| `NAK not_object` | Valid JSON but not `{...}`. | Bug; fix the code. |
| `NAK wrong_field_count` | Not exactly 14 keys. | Bug; fix the code. |
| `NAK bad_id` | `id` missing, not a string, or bad characters. | Bug; fix the code. |
| `NAK bad_ts` | `ts` missing, negative, has decimals, or is text. | Bug; fix the code. |
| `NAK <field>`, e.g. `NAK pH` | That field is missing, text, or not finite. | Bug or failed sensor; fix the code. |
| `NAK too_long` | Line over 512 bytes, or the `\n` never arrived. | Bug; fix the code. |
| *(no reply within 1 s)* | Line or reply lost, or the ESP32 is rebooting. | Resend the same line, up to 3 attempts in total. |

Resending after a lost reply can occasionally produce the same reading twice.
That is acceptable; it is far better than losing readings.

## 7. Timing

- Send whenever a reading is ready. **Once every 2–60 seconds** is a good range.
  Faster works but fills the mobile data plan and the dashboard quickly.
- Wait for the reply to one packet before sending the next.
- Start sending straight after power-up. The ESP32 accepts packets about a
  second after boot, long before the 4G connection is ready.
- If the ESP32 restarts (it can recover itself after network problems), the
  STM32 simply sees one missing reply and resends.

## 8. Example STM32 code (STM32 HAL, C)

Replace `huart2` with the UART wired to the ESP32 and fill in the sensor
values from your own sensor code.

```c
#include <stdio.h>
#include <string.h>
#include "main.h"

extern UART_HandleTypeDef huart2;  // UART wired to the ESP32

typedef struct {
  float pH, turb_ntu, tds_ppm, do_mgl, ec_uscm, orp_mv;
  float temp_w1, temp_w2, depth_cm, temp_air, humidity, pressure_hpa;
} Reading;

// Builds one packet line ending in '\n'. Returns its length, or -1 if it
// did not fit. Every value must be a real number (never NaN).
static int build_packet(char *out, size_t size, const Reading *r) {
  int n = snprintf(out, size,
    "{\"id\":\"WQM-001\",\"ts\":%lu,"
    "\"pH\":%.2f,\"turb_ntu\":%.1f,\"tds_ppm\":%.1f,\"do_mgl\":%.2f,"
    "\"ec_uscm\":%.1f,\"orp_mv\":%.1f,\"temp_w1\":%.2f,\"temp_w2\":%.2f,"
    "\"depth_cm\":%.1f,\"temp_air\":%.1f,\"humidity\":%.1f,\"pressure_hpa\":%.2f}\n",
    (unsigned long)HAL_GetTick(),
    r->pH, r->turb_ntu, r->tds_ppm, r->do_mgl, r->ec_uscm, r->orp_mv,
    r->temp_w1, r->temp_w2, r->depth_cm, r->temp_air, r->humidity, r->pressure_hpa);
  return (n > 0 && (size_t)n < size) ? n : -1;
}

// Reads one reply line from the ESP32 into `reply`, waiting at most timeout_ms.
static int read_reply(char *reply, size_t size, uint32_t timeout_ms) {
  uint32_t start = HAL_GetTick();
  size_t len = 0;
  while (HAL_GetTick() - start < timeout_ms) {
    uint8_t c;
    if (HAL_UART_Receive(&huart2, &c, 1, 10) != HAL_OK) continue;
    if (c == '\r') continue;
    if (c == '\n') { reply[len] = '\0'; return 1; }
    if (len < size - 1) reply[len++] = (char)c;
  }
  return 0;  // timed out
}

// Sends one reading. Returns 1 when the ESP32 answered ACK.
int send_reading(const Reading *r) {
  char line[512];
  int len = build_packet(line, sizeof line, r);
  if (len < 0) return 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    uint8_t junk;  // discard any stale reply bytes
    while (HAL_UART_Receive(&huart2, &junk, 1, 0) == HAL_OK) {}
    HAL_UART_Transmit(&huart2, (uint8_t *)line, (uint16_t)len, 100);
    char reply[48];
    if (!read_reply(reply, sizeof reply, 1000)) continue;      // no reply: resend
    if (strcmp(reply, "ACK") == 0) return 1;                   // saved
    if (strcmp(reply, "NAK storage") == 0) continue;           // resend
    // Any other NAK is a data/format bug: resending the same line won't help.
    // Print `reply` on your debug UART here to see which field is wrong.
    return 0;
  }
  return 0;
}
```

### Important: printing floats on STM32

STM32CubeIDE uses a small C library (newlib-nano) where `snprintf` **does not
print `%f` by default**, so values come out empty and the ESP32 replies
`NAK invalid_json`. Enable float printing:

*Project → Properties → C/C++ Build → Settings → MCU Settings →* tick
**"Use float with printf from newlib-nano (-u _printf_float)"**.

Also guard against NaN from a failed sensor read: `snprintf` prints `nan`,
which is not valid JSON.

## 9. How to test

1. **STM32 alone (optional, needs a USB-to-TTL adapter):** STM32 TX → adapter
   RX, GND → GND. Open a serial terminal at 115200 (e.g.
   `pio device monitor -p COMx -b 115200`). You should see one JSON line per
   reading. Paste one into <https://jsonlint.com> to check it.
2. **STM32 → ESP32:** wire as in section 3 with the ESP32 running the `gateway`
   firmware and connected to a laptop by USB. Open the ESP32 log:
   `pio device monitor` (in `gateway-repo`). Each reading should show
   `[STM] Queued (...)`, and the STM32 should receive `ACK`.
   `[STM] Rejected packet: <reason>` names exactly what to fix (section 6).
3. **Full chain:** with the modem, SIM and antennas connected, the log shows
   `[Upload] Saved <packet id>` and the reading appears on the dashboard
   (Gateway history).

## 10. Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Nothing at all in the ESP32 log | TX/RX swapped, no shared GND, wrong pin, or UART not started on the STM32 |
| Random symbols in the log, or `NAK invalid_json` every time | Baud rate not 115200, or float printing not enabled (section 8) |
| `NAK wrong_field_count` | A key missing, misspelled, duplicated, or an extra key such as `lat` |
| `NAK bad_ts` | `ts` printed with decimals (`%f`) or quotes; use `%lu` |
| `NAK <field>` for one sensor | That value is `nan` or missing; check the sensor read |
| `NAK too_long` | No `\n` at the end of the line, or the line is over 512 bytes |
| STM32 never sees `ACK` but the ESP32 log shows `Queued` | ESP32 GPIO26 → STM32 RX wire missing, or the STM32 reads the wrong UART |
