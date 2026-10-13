# Raspberry Pi Pico 2 W — verification record

Eight embedded projects on a Raspberry Pi Pico 2 W (RP2350), structured and
verified according to the **V-model**.

**Five carry a recorded verification result. Three do not yet** — see
[Not yet verified](#not-yet-verified) at the end. Nothing in this document is
inferred; an entry exists only where the procedure was actually carried out on
the hardware named below.

Each project on the left-hand side of the V has a corresponding verification
activity on the right. The four peripheral projects are **unit-level**
verification — one subsystem each, in isolation. The web server is
**integration-level** — several subsystems combined, where interface faults
first become possible.

```
 Requirements ─────────────────────────────► Acceptance test
   \                                              /
    System design ──────────────────────► SYSTEM TEST
      \                                        /     setup_portal
       Architectural design ──────► INTEGRATION TEST
         \                                  /        webserver
          Module design ──► UNIT TEST
            \                   /    blink · wifi · bluetooth
             Implementation          internal_temperature
                                     ble_sensor · ble_central
```

## Test environment

| Item | Detail |
|---|---|
| Target | Raspberry Pi Pico 2 W — RP2350 rev 2, Cortex-M33, 4 MiB W25Q32 flash |
| Radio | CYW43439 — WiFi 2.4 GHz and Bluetooth |
| Debug probe | Raspberry Pi Debug Probe (CMSIS-DAP), firmware 1.0.1 |
| Console | Probe UART bridge, `/dev/cu.usbmodem114202`, 115200 8N1 |
| Toolchain | Pico SDK 2.3.0/2.3.1, arm-none-eabi-gcc 15.2.1, OpenOCD 0.12.0+dev |
| Host | macOS, Apple silicon, VS Code + Raspberry Pi Pico extension |
| Build | `PICO_PLATFORM=rp2350-arm-s`, `CMAKE_BUILD_TYPE=Debug` (`-Og -g`) |

Every result below was measured on this hardware. Where a value was read
directly out of the running target, the GDB command is given.

---

# Unit-level verification

One subsystem per project, exercised in isolation. A failure at this level has
exactly one place to look.

## UT-1 — `blink` — GPIO / LED via CYW43

**Objective.** Verify the toolchain, SWD flashing, and source-level debugging;
confirm the onboard LED can be driven.

**Procedure.** Build Debug, flash over SWD, set a breakpoint at `main`, step.

**Expected.** Flash verifies; the debugger halts at `main`; LED alternates on a
2 s cycle.

**Observed.**

```
** Programming Finished **
** Verify Started **
** Verified OK **

Thread 1 "rp2350.cm0" hit Breakpoint 1, main () at blink.c:29
29          return cyw43_arch_init();
```

**Verdict: PASS.** See [blink/README.md](../2.unit-testing/blink/README.md).

**Note.** 263,436 bytes of text for a blinking LED. The Pico 2 W has no LED on
the RP2350 — it hangs off the WiFi chip, so the 225 KB CYW43439 firmware blob
must be linked to light it.

## UT-2 — `internal_temperature` — ADC

**Objective.** Verify the on-die temperature sensor on ADC channel 4 and the
datasheet conversion `T = 27 - (V - 0.706) / 0.001721`.

**Procedure.** Enable the sensor, average 16 conversions, convert, print once
per second. Read the live values through the debugger.

**Expected.** A plausible die temperature — above ambient, in the 20–40 degC
band — stable across samples.

**Observed.**

```
(gdb) print celsius
$1 = 27.2262268
(gdb) print fahrenheit
$2 = 81.0072098
(gdb) print raw_avg
$3 = 875.8125
(gdb) print voltage
$4 = 0.705610633
```

**Verdict: PASS.** 27.23 degC. The measured 0.7056 V sits essentially on the
datasheet's nominal 0.706 V at 27 degC, so the conversion is corroborated by
the sensor's own calibration point.
See [internal_temperature/README.md](../2.unit-testing/internal_temperature/README.md).

**Note.** 21,660 bytes — 12x smaller than `blink`, because no radio is linked.

## UT-3 — `wifi` — CYW43439 WiFi + lwIP

**Objective.** Verify association with a WPA2 network and DHCP address
assignment.

**Procedure.** Connect in station mode with a 30 s timeout; print the address
obtained from `netif_default`.

**Expected.** Association completes and a DHCP lease is granted inside the
timeout.

**Observed.**

```
Connecting to 'iPhone13'
connect status: joining
connect status: no ip
connect status: link up
Connected to Wi-Fi
IP address: 172.20.10.11
```

**Verdict: PASS.** See [wifi/README.md](../2.unit-testing/wifi/README.md).

**Defect found and resolved.** The first attempt returned
`Failed to connect to Wi-Fi: -2` (`PICO_ERROR_TIMEOUT`). Root cause: the access
point was operating on 5 GHz, which the CYW43439 cannot receive — it has no
5 GHz radio, so the SSID was never visible. Resolved by forcing the hotspot to
2.4 GHz. Note that an invisible SSID and a wrong password produce the same
error code.

## UT-4 — `bluetooth` — CYW43439 Bluetooth + BTstack

**Objective.** Verify both BLE roles: scanning for advertisements (LE central)
and being discoverable (LE peripheral).

**Procedure.** Bring up BTstack, advertise as `Pico2W`, scan passively, count
received advertising reports. Discover and connect from a phone.

**Expected.** Advertising reports accumulate; the board is discoverable and
connectable from a BLE scanner application.

**Observed.**

```
(gdb) print report_count
$1 = 1006
(gdb) print adv_data
$2 = "\002\001\006\a\tPico2W"
```

`adv_data` decodes as `02 01 06` (flags: LE general discoverable) then
`07 09 "Pico2W"` (complete local name). From an iOS BLE scanner: `Pico2W`
discovered at **-45 dBm**, then **CONNECTED / BONDED**.

**Verdict: PASS.** See [bluetooth/README.md](../2.unit-testing/bluetooth/README.md).

**Note.** Scanning and advertising are opposite roles. A scanner transmits
nothing and is therefore undiscoverable; `ENABLE_LE_PERIPHERAL` and an
advertising payload are what make the board visible. iOS Settings does not list
bare BLE peripherals — a scanner application is required.

---

# Integration-level verification

## IT-1 — `webserver` — WiFi + lwIP + TCP + ADC + HTTP

**Objective.** Verify that the independently-verified subsystems operate
together: the board joins a network, accepts TCP connections, samples the ADC
during request handling, and returns a well-formed HTTP response.

**Interfaces under test.**

| Interface | Between |
|---|---|
| DHCP address → server bind | UT-3 → TCP listener |
| ADC sample → HTTP body | UT-2 → response builder |
| lwIP background IRQ → TCP callbacks | CYW43 driver → application |
| `Content-Length` → transmitted body | response builder → TCP send path |

**Procedure.** Connect to WiFi, listen on port 80, serve a status page
containing a freshly-sampled temperature. Request it from a browser and
independently with `curl`. Read the request counter off the target.

**Expected.** HTTP/1.1 200 with a body whose length matches the declared
`Content-Length`; a live temperature consistent with UT-2; the request counter
incrementing per request.

**Observed.**

```
$ curl -D - http://172.20.10.11/

HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: 991
Connection: close
```

Body measured **exactly 991 bytes**. Content of that response:

```
25.73 degC
IP address        172.20.10.11
Uptime            0 h 1 m 19 s
Requests served   23
Chip              RP2350 (Cortex-M33)
WiFi              CYW43439
```

Counter read from the target afterwards, having advanced further through the
page's 5 s auto-refresh:

```
(gdb) print request_count
$1 = 29
```

**Verdict: PASS.** See [webserver/README.md](../3.integration-testing/webserver/README.md).

**Why the length check is the meaningful assertion.** Declared and actual body
lengths matching proves the connection is closed only once all data has been
acknowledged. Closing in the receive callback instead of the send callback
discards unacknowledged bytes — the page still renders in a browser, so the
fault is invisible to visual inspection and only a length comparison catches it.

**Temperature cross-check.** The served 25.73 degC is consistent with UT-2's
27.23 degC; the difference reflects genuine die cooling between runs, not a
conversion error in the integration path.

---

# Summary

| ID | Level | Project | Subsystem(s) | Result |
|---|---|---|---|---|
| UT-1 | Unit | `blink` | GPIO via CYW43 | PASS |
| UT-2 | Unit | `internal_temperature` | ADC | PASS — 27.23 degC |
| UT-3 | Unit | `wifi` | WiFi + lwIP | PASS — 172.20.10.11 |
| UT-4 | Unit | `bluetooth` | Bluetooth + BTstack | PASS — 1006 reports, bonded |
| IT-1 | Integration | `webserver` | WiFi + TCP + ADC + HTTP | PASS — 200, 991 bytes |
| UT-5 | Unit | `ble_sensor` | BLE peripheral, GATT + service data | **not recorded** |
| UT-6 | Unit | `ble_central` | BLE central, GATT client | **not recorded** |
| ST-1 | System | `setup_portal` | BLE + WiFi + HTTP + flash settings | **not recorded** |

The IT-1 figures describe the `webserver` as it stood when that test was run.
It has since gained Wi-Fi provisioning, access-point fallback and
flash-resident web assets, so the 991-byte response no longer exists and IT-1
would need re-running to describe the current build.

Binary sizes, which show what each project actually links:

| Project | text | bss | of 2 MB |
|---|---|---|---|
| `internal_temperature` | 21,660 | 792 | 1.0% |
| `blink` | 266,012 | 3,212 | 12.7% |
| `wifi` | 312,528 | 48,388 | 14.9% |
| `bluetooth` | 338,956 | 6,668 | 16.2% |
| `webserver` | 340,356 | 56,732 | 16.2% |
| `ble_sensor` | 347,688 | 6,752 | 16.6% |
| `ble_central` | 354,260 | 9,308 | 16.9% |
| `setup_portal` | 423,652 | 71,164 | 20.2% |

Measured with `arm-none-eabi-size` on the current tree. Two figures moved since
the original record and the reasons are worth keeping:

- `webserver` went 321,816 -> 389,552 as provisioning and the asset pipeline
  went in, then **down to 340,356** when `sscanf` was removed from request
  parsing. One `sscanf("%7s %159s")` links newlib's `__ssvfscanf_r`,
  `_strtod_l` and its float tables: **49,364 bytes**, measured by deletion.
  `setup_portal` carried the same cost and the same fix.
- `blink` went 263,436 -> 266,012 across the SDK 2.3.0 to 2.3.1 move.

## Defects found during verification

Recorded because each cost real time and each recurs:

| # | Level | Defect | Root cause | Resolution |
|---|---|---|---|---|
| 1 | Environment | Cortex-M33 cores reported unavailable | `ARCHSEL` latched to RISC-V by an earlier session; survives reset | Write `0x0` to `0x40120158` via the RISC-V core |
| 2 | Environment | `clang` compiling with `-mthumb` | CMake Tools configured with a host kit; CMake caches compiler paths permanently | Delete `build/` entirely — "Clean CMake" leaves `CMakeCache.txt` |
| 3 | UT-3 | `Failed to connect: -2` | Access point on 5 GHz; CYW43439 is 2.4 GHz only | Force the AP to 2.4 GHz |
| 4 | UT-3 | `-DWIFI_SSID=""` | Undefined CMake variables expand to empty, silently | `include(... OPTIONAL)` plus a runtime guard |
| 5 | UT-3 | `lwipopts.h: No such file` | lwIP ships no default configuration | Supply the file |
| 6 | UT-4 | Board not discoverable | Scanning is a receive-only role | Add `ENABLE_LE_PERIPHERAL` and advertise |
| 7 | Tooling | GDB lands in `isr_hardfault` | Halting and detaching leaves the target wedged | `monitor reset init` before `continue` |

## Verification method notes

**Debugger readout over console output.** Several results were taken with GDB
rather than from the serial console, by halting the target and printing
variables. This is more reliable when the console is held by another process,
and it observes the program's actual state rather than what it chose to print.

**Independent confirmation.** IT-1 was verified two ways — a browser rendering
the page, and `curl` measuring the response. The second gives a numeric
assertion rather than a visual impression.

**Not automated.** These are manual verification procedures with recorded
evidence, not an automated regression suite. The pure functions — notably
`adc_to_celsius()` — are host-testable and would be the natural starting point
if automation is wanted.

## Not yet verified

Three projects and one set of changes have **no recorded hardware result**.
They build cleanly and parts of them are checked on the host, but neither is a
verification result and neither is reported as one here.

| Item | Built | Host-checked | On hardware |
|---|---|---|---|
| `ble_sensor` | yes, 347,688 B | — | **no** |
| `ble_central` | yes, 354,260 B | — | **no** |
| `setup_portal` | yes, 423,652 B | assets, element ids, routes, JSON keys | **no** |
| `webserver` provisioning + assets | yes, 340,356 B | assets, element ids, form fields | **no** |

What "host-checked" covers, for the two with web front ends: every embedded
asset gunzips to a byte-identical copy of its source and every ETag recomputes;
every element id referenced by `app.js` exists in the HTML; every form action
matches a route the server handles; every asset referenced by the page
resolves. That catches whole classes of mistake before they reach the board —
it establishes nothing about radio behaviour, timing or coexistence.

The specific claims that only hardware can settle:

- Whether the CYW43439 returns BLE scan results while also beaconing an access
  point. The driver permits the call; the single shared radio front end is the
  question.
- Whether three parallel asset fetches complete on first page load, which is
  what `MEMP_NUM_TCP_PCB = 12` is there to allow.
- Whether `Content-Encoding: gzip` is matched correctly to the body a browser
  receives.
- Whether the relocated credential sector reads back correctly after a power
  cycle. The offset moved from the last sector to the fourth from the end —
  the last sector is reserved by the RP2350 E10 errata workaround, which the
  original `webserver` store silently sat on top of. The first boot after
  flashing will therefore find no stored credentials and fall back, which is
  the designed behaviour and not a fault.

Until a procedure below exists for each of these with an observed result, the
honest status is "builds, untested".

## Reference

| Document | Location |
|---|---|
| Getting Started with Raspberry Pi Pico | `RP-008276-DS-2-getting-started-with-pico.pdf` |
| RP2350 datasheet | `RP-008373-DS-2-rp2350-datasheet.pdf` |
