# Unit-level verification — Raspberry Pi Pico 2 W

Verification of individual subsystems on a Raspberry Pi Pico 2 W (RP2350),
following the **V-model**. Each project here exercises **one** subsystem in
isolation, verified against its module design.

A unit here is a peripheral or driver module, which is the smallest element of the
architectural decomposition. A failure at this level has exactly one place to
look, which is the reason for testing at this level before combining anything.

## Test environment

| Item | Detail |
| --- | --- |
| Target | Raspberry Pi Pico 2 W — RP2350 rev 2, Cortex-M33, 4 MiB W25Q32 flash |
| Radio | CYW43439 — WiFi 2.4 GHz and Bluetooth |
| Debug probe | Raspberry Pi Debug Probe (CMSIS-DAP), firmware 1.0.1 |
| Console | Probe UART bridge, `/dev/cu.usbmodem114202`, 115200 8N1 |
| Toolchain | Pico SDK 2.3.0/2.3.1, arm-none-eabi-gcc 15.2.1, OpenOCD 0.12.0+dev |
| Host | macOS, Apple silicon, VS Code + Raspberry Pi Pico extension |
| Build | `PICO_PLATFORM=rp2350-arm-s`, `CMAKE_BUILD_TYPE=Debug` (`-Og -g`) |

All results were measured on this hardware. Where a value was read directly out
of the running target, the GDB command used is given alongside it.

---

## UT-1 — `blink` — GPIO / LED via CYW43

**Subsystem.** GPIO output, and the toolchain and debug path themselves.

**Objective.** Verify that the build toolchain, SWD flashing and source-level
debugging function, and that the onboard LED can be driven.

**Procedure.**

1. Configure a Debug build (`-Og -g`) and compile.
2. Flash over SWD through the Debug Probe.
3. Attach GDB, set a breakpoint at `main`, continue.
4. Observe the LED.

**Expected result.** Flash verifies; the debugger halts at `main`; the LED
alternates on a 2 s cycle.

**Observed result.**

```
** Programming Finished **
** Verify Started **
** Verified OK **
** Resetting Target **

Thread 1 "rp2350.cm0" hit Breakpoint 1, main () at blink.c:29
29          return cyw43_arch_init();
```

**Verdict: PASS** — see [blink/README.md](blink/README.md).

**Observation.** 263,436 bytes of text for a blinking LED. The Pico 2 W has no
LED attached to the RP2350; it hangs off a GPIO on the WiFi chip, so the 225 KB
CYW43439 firmware blob must be linked in order to light it. This is a property
of the board, not of the code.

---

## UT-2 — `internal_temperature` — ADC

**Subsystem.** 12-bit ADC and the on-die temperature sensor on channel 4.

**Objective.** Verify the sensor and the datasheet conversion
`T = 27 - (V - 0.706) / 0.001721`.

**Procedure.**

1. `adc_init()`, `adc_set_temp_sensor_enabled(true)`, select
   `ADC_TEMPERATURE_CHANNEL_NUM`.
2. Average 16 conversions per reading to suppress LSB jitter.
3. Convert and print once per second.
4. Halt via GDB mid-loop and read the intermediate values.

**Expected result.** A plausible die temperature — above ambient, within
20–40 degC — stable across consecutive samples.

**Observed result.**

```
(gdb) break internal_temperature.c:78
(gdb) continue
Thread 1 "rp2350.cm0" hit Breakpoint 1, main () at internal_temperature.c:78

(gdb) print celsius
$1 = 27.2262268
(gdb) print fahrenheit
$2 = 81.0072098
(gdb) print raw_avg
$3 = 875.8125
(gdb) print voltage
$4 = 0.705610633
```

**Verdict: PASS** — see
[internal_temperature/README.md](internal_temperature/README.md).

**Corroboration.** The measured 0.7056 V sits essentially on the datasheet's
nominal 0.706 V at 27 degC, so the conversion is independently confirmed by the
sensor's own calibration point rather than only by the result looking sensible.

**Limitations.** The sensor is uncalibrated — part-to-part variation is a few
degrees. `Vref` is assumed to be exactly 3.3 V, and real rail variation feeds
straight into the result. Absolute accuracy is roughly ±2–3 degC; relative
changes are considerably better. This measures **die** temperature, not ambient.

**Observation.** 21,660 bytes — 12x smaller than `blink`, because no radio is
linked.

---

## UT-3 — `wifi` — CYW43439 WiFi + lwIP

**Subsystem.** WiFi station mode and DHCP.

**Objective.** Verify association with a WPA2 network and address assignment.

**Procedure.**

1. `cyw43_arch_init_with_country()`, then `cyw43_arch_enable_sta_mode()`.
2. `cyw43_arch_wifi_connect_timeout_ms()` with a 30 s timeout.
3. Print the address obtained from `netif_default`.

**Expected result.** Association completes and a DHCP lease is granted within
the timeout.

**Observed result.**

```txt
Connecting to 'iPhone13'
connect status: joining
connect status: no ip
connect status: link up
Connected to Wi-Fi
IP address: 172.20.10.11
```

The three `connect status` lines come from the CYW43 driver and mark the
association stages: `joining` → associating, `no ip` → awaiting DHCP,
`link up` → lease granted.

**Verdict: PASS** — see [wifi/README.md](wifi/README.md).

**Defect found and resolved.** The first execution returned
`Failed to connect to Wi-Fi: -2` (`PICO_ERROR_TIMEOUT`). Root cause: the access
point was operating on 5 GHz, which the CYW43439 cannot receive — it has no
5 GHz radio, so the SSID was never visible to the board. Resolved by forcing the
access point to 2.4 GHz. Note that an invisible SSID and an incorrect password
produce the same error code, so `-2` alone does not discriminate between them.

---

## UT-4 — `bluetooth` — CYW43439 Bluetooth + BTstack

**Subsystem.** Bluetooth Low Energy, in both roles.

**Objective.** Verify LE central operation (scanning for advertisements) and LE
peripheral operation (being discoverable and connectable).

**Procedure.**

1. Bring up BTstack over the CYW43 HCI transport.
2. Advertise as `Pico2W` with `ADV_IND` (connectable, undirected).
3. Scan passively and count received advertising reports.
4. Discover and connect from a phone using a BLE scanner application.

**Expected result.** Advertising reports accumulate; the board is discoverable
and connectable from an external device.

**Observed result.**

```txt
(gdb) print report_count
$1 = 1006
(gdb) print adv_data
$2 = "\002\001\006\a\tPico2W"
```

`adv_data` decodes as `02 01 06` — flags, LE general discoverable — followed by
`07 09 "Pico2W"` — complete local name. From an iOS BLE scanner: `Pico2W`
discovered at **-45 dBm** (~0.2 m), then **CONNECTED / BONDED**.

**Verdict: PASS** — see [bluetooth/README.md](bluetooth/README.md).

**Defect found and resolved.** The board was initially not discoverable.
Root cause: scanning and advertising are opposite roles — an LE central only
receives and transmits nothing, so it cannot be discovered. Resolved by adding
`ENABLE_LE_PERIPHERAL` and an advertising payload. Both roles then ran
concurrently.

**Observation.** iOS Settings does not list bare BLE peripherals; it shows only
Classic devices and those offering recognised pairing profiles. A scanner
application is required to observe the result. The identifier such an app
displays is a CoreBluetooth UUID, not a hardware address — iOS never exposes
Bluetooth MAC addresses to applications.

The service list appears empty after connecting. This is correct: the firmware
advertises and accepts connections but implements no GATT services.

---

## Summary

| ID | Project | Subsystem | Key measurement | Verdict |
| --- | --- | --- | --- | --- |
| UT-1 | `blink` | GPIO via CYW43 | breakpoint hit at `main` | PASS |
| UT-2 | `internal_temperature` | ADC | 27.23 degC, 0.7056 V | PASS |
| UT-3 | `wifi` | WiFi + lwIP | 172.20.10.11 | PASS |
| UT-4 | `bluetooth` | Bluetooth + BTstack | 1006 reports; bonded at -45 dBm | PASS |

## Defects found during unit verification

| # | Test | Defect | Root cause | Resolution |
| --- | --- | --- | --- | --- |
| 1 | UT-3 | `Failed to connect: -2` | Access point on 5 GHz; CYW43439 is 2.4 GHz only | Force the AP to 2.4 GHz |
| 2 | UT-3 | `-DWIFI_SSID=""` compiled in | Undefined CMake variables expand to empty, silently | `include(... OPTIONAL)` plus a runtime guard |
| 3 | UT-3 | `lwipopts.h: No such file` | lwIP ships no default configuration | Supply the file |
| 4 | UT-4 | Board not discoverable | Scanning is a receive-only role | Add `ENABLE_LE_PERIPHERAL` and advertise |

## Environment defects

Not faults in the units under test, but they blocked verification and each one
recurs:

| # | Defect | Root cause | Resolution |
| --- | --- | --- | --- |
| E1 | Cortex-M33 cores reported unavailable | `ARCHSEL` latched to RISC-V by an earlier session; survives reset | Write `0x0` to `0x40120158` via the RISC-V core |
| E2 | `clang` invoked with `-mthumb` | CMake Tools configured with a host kit; CMake caches compiler paths permanently | Delete `build/` entirely — "Clean CMake" leaves `CMakeCache.txt` |
| E3 | GDB lands in `isr_hardfault` | Halting a running target and detaching leaves it wedged | `monitor reset init` before `continue` |

## Method notes

**Debugger readout over console output.** Several results were taken by halting
the target in GDB and printing variables, rather than from the serial console.
This is more reliable when the console is held by another process, and it
observes the program's actual state rather than what it chose to print.

**Not automated.** These are manual verification procedures with recorded
evidence, not an automated regression suite. The pure functions — notably
`adc_to_celsius()` in UT-2 — are host-testable and would be the natural
starting point for automation.
