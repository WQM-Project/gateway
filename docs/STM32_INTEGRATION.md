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
{"id":"WQM-001","ts":166519,"pH":7.12,"tds_ppm":310.0,"do_mgl":8.84,"orp_mv":91.6,"temp_w1":23.62,"temp_air":28.60,"pressure_hpa":1013.25,"lat":0.000000,"lon":0.000000,"alt_m":0.0,"sog_knots":0.0,"cog_deg":0.0,"gps_fix":0,"sats":0,"heading":0.0,"pitch":0.0,"roll":0.0}
```

Rules:

1. **Required active keys** must always be present (`id`, `ts`, `pH`, `tds_ppm`, `do_mgl`, `orp_mv`, `temp_w1`, `temp_air`, `pressure_hpa`). Case matters (`pH`).
   Reserved sensors (`turb_ntu`, `ec_uscm`, `temp_w2`, `depth_cm`, `humidity`) and STM32 GNSS/IMU fields (`lat`, `lon`, `alt_m`, `sog_knots`, `cog_deg`, `gps_fix`, `sats`, `heading`, `pitch`, `roll`) are optional pass-through fields.
2. Numbers are **JSON numbers**: `7.12`, not `"7.12"`.
3. **No `NaN`, `inf` or empty values.** Every value must be a real number.
4. One line, **at most 768 bytes** including the `\n`. The active STM32 line is ~280 bytes.
5. Key order does not matter. Spaces are allowed but not needed.
6. The gateway prioritizes its own SIM7600 GNSS fix when valid; unfixed STM32 `(0, 0)` coordinates (`gps_fix: 0`) are stripped before upload so the map stays accurate. Wait for `ACK`/`NAK` from ESP32 on every packet.
7. `\r\n` line endings are fine; the ESP32 ignores `\r`.

### Fields

| Key | Type | Unit | Status | Meaning |
| --- | --- | --- | --- | --- |
| `id` | string | – | Required | Boat ID, **`"WQM-001"`**. Letters, digits, `_`, `-` only, max 64 chars. Must be registered on the server. |
| `ts` | whole number | ms | Required | STM32 uptime in milliseconds, e.g. `HAL_GetTick()`. 0 to 4294967295, no decimals. |
| `pH` | number | – | Required | Water pH (`PA3`, SEN0161 V1 on 5V w/ 2:1 divider; `PH_SCALE=4.942f`, `PH_OFFSET=-2.713f`) |
| `tds_ppm` | number | ppm | Required | Total dissolved solids (`PA5`, SEN0244 w/ 2:1 divider; `TDS_CAL_K=1.036f`) |
| `do_mgl` | number | mg/L | Required | Dissolved oxygen (`PA4`, SEN0237-A w/ 2:1 divider, 7.5% KCl, BMP280 pressure comp) |
| `orp_mv` | number | mV | Required | Oxidation-reduction potential (`PC3`, SEN0165 w/ 2:1 divider, 20-sample trimmed mean) |
| `temp_w1` | number | °C | Required | Water temperature (`PE6`, DS18B20 1-Wire) |
| `temp_air` | number | °C | Required | Air temperature (`PB8/PB9`, BMP280 `0x76`) |
| `pressure_hpa` | number | hPa | Required | Barometric pressure (`PB8/PB9`, BMP280 `0x76`) |
| `turb_ntu` | number | NTU | Optional | Turbidity (`PC0`, reserved/unfitted in current STM32 build) |
| `ec_uscm` | number | µS/cm | Optional | Electrical conductivity (`PA6`, reserved/unfitted in current STM32 build) |
| `temp_w2` | number | °C | Optional | Backup water temperature probe (reserved) |
| `depth_cm` | number | cm | Optional | Ultrasonic depth (reserved) |
| `humidity` | number | % | Optional | Relative humidity (omitted — BMP280 has no RH sensor) |
| `lat` / `lon` | number | deg | Optional | NEO-M8N GNSS coordinates (overridden by SIM7600 GNSS if fixed) |
| `alt_m` / `sog_knots` / `cog_deg` | number | m / kn / deg | Optional | NEO-M8N altitude, speed over ground, course over ground |
| `gps_fix` / `sats` | bool or int | – | Optional | Fix quality & satellite count |
| `heading` / `pitch` / `roll` | number | deg | Optional | BNO085 9-DOF IMU Euler orientation |

`ts` is only uptime. The ESP32 attaches the real date and time (`utc_s`), so the
STM32 does not need a real-time clock.

**If a sensor fails:** keep finite real numbers in required fields; unfitted
optional sensors should simply be omitted from the JSON rather than sent as `0`
if not installed.

## 6. Reply from the ESP32

After each line the ESP32 answers with one line, normally within a few
milliseconds:

| Reply | Meaning | What the STM32 should do |
| --- | --- | --- |
| `ACK` | Valid and saved in the ESP32's flash. It will upload by itself. | Done; go to the next reading. |
| `NAK storage` | Valid, but the ESP32 could not save it. | Resend the same line once or twice. |
| `NAK invalid_json` | Not valid JSON (missing quote/comma/brace, or `nan`). | Bug in the STM32 code. Log it; don't resend unchanged. |
| `NAK not_object` | Valid JSON but not `{...}`. | Bug; fix the code. |
| `NAK unknown_field` | Contains an unrecognized JSON key outside the telemetry schema. | Remove typo/extra field in firmware. |
| `NAK bad_id` | `id` missing, not a string, or bad characters. | Bug; fix the code. |
| `NAK bad_ts` | `ts` missing, negative, has decimals, or is text. | Bug; fix the code. |
| `NAK <field>`, e.g. `NAK pH` | That required or optional field is missing (if required), non-numeric, or non-finite. | Bug or failed sensor; fix the code. |
| `NAK too_long` | Line over 768 bytes, or the `\n` never arrived. | Bug; fix the code. |
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
  float pH, tds_ppm, do_mgl, orp_mv;
  float temp_w1, temp_air, pressure_hpa;
  float lat, lon, alt_m, sog_knots, cog_deg;
  int gps_fix, sats;
  float heading, pitch, roll;
} Reading;

// Builds one packet line ending in '\n'. Returns its length, or -1 if it
// did not fit. Every value must be a real number (never NaN).
static int build_packet(char *out, size_t size, const Reading *r) {
  int n = snprintf(out, size,
    "{\"id\":\"WQM-001\",\"ts\":%lu,"
    "\"pH\":%.2f,\"tds_ppm\":%.1f,\"do_mgl\":%.2f,\"orp_mv\":%.1f,"
    "\"temp_w1\":%.2f,\"temp_air\":%.2f,\"pressure_hpa\":%.2f,"
    "\"lat\":%.6f,\"lon\":%.6f,\"alt_m\":%.1f,\"sog_knots\":%.1f,\"cog_deg\":%.1f,"
    "\"gps_fix\":%d,\"sats\":%d,\"heading\":%.1f,\"pitch\":%.1f,\"roll\":%.1f}\n",
    (unsigned long)HAL_GetTick(),
    r->pH, r->tds_ppm, r->do_mgl, r->orp_mv,
    r->temp_w1, r->temp_air, r->pressure_hpa,
    r->lat, r->lon, r->alt_m, r->sog_knots, r->cog_deg,
    r->gps_fix, r->sats, r->heading, r->pitch, r->roll);
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
  char line[768];
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
| `NAK unknown_field` | A misspelled or unsupported JSON key outside the telemetry schema |
| `NAK bad_ts` | `ts` printed with decimals (`%f`) or quotes; use `%lu` |
| `NAK <field>` for one sensor | That value is `nan` or missing; check the sensor read |
| `NAK too_long` | No `\n` at the end of the line, or the line is over 768 bytes |
| STM32 never sees `ACK` but the ESP32 log shows `Queued` | ESP32 GPIO26 → STM32 RX wire missing, or the STM32 reads the wrong UART |
