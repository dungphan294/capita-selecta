# System-level verification — Raspberry Pi Pico 2 W

Verification that the combined subsystems behave as a **deliverable system**,
following the **V-model**. This level tests against the system design: not
whether each unit works, and not whether the interfaces between a pair of them
are correct, but whether the whole thing does the job someone would actually
pick it up to do.

```
 Requirements ─────────────────────────────► Acceptance test
   \                                              /
    System design ──────────────────► SYSTEM TEST  ◄── you are here
      \                                        /
       Architectural design ──────► Integration test
         \                                  /
          Module design ──► Unit test
            \                   /
             Implementation
```

Integration-level verification is in
[`../3.integration-testing/`](../3.integration-testing/) and unit-level in
[`../2.unit-testing/`](../2.unit-testing/). Both are preconditions: every
subsystem combined here carries a recorded PASS at unit level, and the
WiFi + TCP + ADC + HTTP combination carries one at integration level.

## What distinguishes this level

The `webserver` project at integration level answers "do WiFi, lwIP, TCP, the
ADC and HTTP work when combined?" That is an interface question, and it assumes
someone has already got the board onto a network.

`setup_portal` answers the question that comes before it: **you have a board
and no network it knows about — now what?** Which means it has to work when
nothing else does. That brings in whole concerns integration testing never
touches:

| Concern | Why it is a system-level question |
|---|---|
| Three ways in — BLE, access point, station | Whichever is available must work, and the choice is the system's to make, not the user's |
| Settings that survive power loss | A system is not usable if it must be reconfigured each boot |
| Honest failure reporting | "Could not join" is useless; "wrong password" versus "5 GHz" versus "WPA2-Enterprise" is actionable |
| Never becoming unreachable | There must be no state in which the board can neither be reached nor told where to go |
| Two radio roles at once | BLE peripheral and central together, on one shared front end, alongside WiFi |

## Projects

| Project | Combines | Status |
|---|---|---|
| [`setup_portal`](setup_portal/README.md) | BLE peripheral + BLE central + WiFi station + access point + DHCP + HTTP + flash settings | **builds, not yet verified on hardware** |

## Status, stated plainly

`setup_portal` builds cleanly at **423,652 bytes** of flash — 20.2% of the
2 MB budget — and parts of it are checked on the host. **No system-level
procedure has been carried out on hardware, so this level has no recorded
result yet.**

What the host checks do cover, and they are worth something: every embedded web
asset gunzips to a byte-identical copy of its source and every ETag recomputes;
every element id referenced by `app.js` exists in the HTML; every form action
matches a route the server handles. That eliminates a class of mistake before
it reaches the board. It establishes nothing about radio behaviour, timing,
coexistence or the flash store.

The full list of claims awaiting hardware is in
[`../1.document/verification-record.md`](../1.document/verification-record.md),
under **Not yet verified**.

## Test environment

Identical to the other levels:

| Item | Detail |
|---|---|
| Target | Raspberry Pi Pico 2 W — RP2350 rev 2, Cortex-M33, 4 MiB W25Q32 flash |
| Radio | CYW43439 — WiFi 2.4 GHz and Bluetooth |
| Debug probe | Raspberry Pi Debug Probe (CMSIS-DAP), firmware 1.0.1 |
| Console | Probe UART bridge, `/dev/cu.usbmodem114202`, 115200 8N1 |
| Toolchain | Pico SDK 2.3.1, arm-none-eabi-gcc 15.2.1, OpenOCD 0.12.0+dev |
| Host | macOS, Apple silicon, VS Code + Raspberry Pi Pico extension |
| Build | `PICO_PLATFORM=rp2350-arm-s`, `CMAKE_BUILD_TYPE=Debug` (`-Og -g`) |
