# `ble_central` — reading BLE values without a phone

## The problem

The `bluetooth` project (UT-4) scans and counts advertising reports. To read
an actual *value* off a BLE device, a phone application was still needed. That
is the usual conclusion, and it is wrong — but for three specific reasons, not
one.

A phone contains three things that a bare scanner build does not:

| Missing piece | What it costs you | Supplied here by |
|---|---|---|
| The **central / GATT client** role | Can see advertisements, cannot connect or read | `gatt_client_init()`, `MAX_NR_GATT_CLIENTS` |
| The **assigned numbers** | `0x2A6E` is just a number; `2B 09` is just two bytes | `ble_names.c` lookup tables and decoders |
| The **security manager** | Encrypted characteristics refuse every read | `sm_init()`, Just Works pairing, bonding |

None of the three is a hardware limitation of the RP2350 or the CYW43439. The
`bluetooth` project lacked all three because a scanner does not need them.
This project adds them and the phone becomes unnecessary.

A fourth reason exists and cannot be fixed in firmware: a vendor's
**proprietary payload format**. Manufacturer-specific advertising data and
custom 128-bit characteristics are undocumented on purpose, and the vendor's
app is the only thing that knows the encoding. This program prints those bytes
raw and names the company that owns the format, which is the most that can
honestly be done without reverse engineering.

## What it does

```
scan (active, 8 s)                  decode every AD structure as it arrives
  -> summary table                  address, RSSI, sightings, connectable, name
  -> choose a target                by name, by address, or strongest named device
  -> connect
  -> discover primary services      the full GATT tree, named
  -> discover characteristics       with ATT properties spelled out
  -> read every readable value      decoded per the SIG specification
       -> on ATT 0x05 / 0x0F        pair (Just Works), bond, retry
  -> subscribe to the first         notifications streamed live
     NOTIFY / INDICATE char
  -> on disconnect, rescan
```

Active scanning matters: it transmits `SCAN_REQ` and collects `SCAN_RSP`,
which is where most peripherals put their full name. UT-4 scanned passively
and therefore saw fewer names than a phone did, for reasons that had nothing
to do with the radio.

## Files

| File | Contents |
|---|---|
| `ble_central.c` | The state machine: scan, choose, connect, discover, read, subscribe |
| `ble_names.c` / `.h` | Assigned-number tables, ATT property names, per-UUID value decoders, advertising-payload printer |
| `btstack_config.h` | Central + GATT client + pairing. Note `ENABLE_LE_PERIPHERAL` is *absent* — this build never advertises |

## Build

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.1
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
export PATH=$PICO_TOOLCHAIN_PATH/bin:$PATH
cmake -S . -B build -G Ninja -DPICO_PLATFORM=rp2350-arm-s -DCMAKE_BUILD_TYPE=Debug
ninja -C build
```

Pick a target at configure time, or leave both unset to take the strongest
connectable device that advertises a name:

```bash
cmake -S . -B build -G Ninja -DTARGET_NAME="Pico2W"
cmake -S . -B build -G Ninja -DTARGET_ADDR="A4:C1:38:11:22:33"
```

Console is the Debug Probe UART bridge, 115200 8N1, as for every other
project here.

## Verification

### VT-1 — Build and size — **PASS**

Both translation units compile without a single warning against Pico SDK
2.3.1 / arm-none-eabi-gcc 15.2.1.

```
   text	   data	    bss	    dec	    hex	filename
 354260	      0	   9308	 363568	  58c30	build/ble_central.elf
```

Against the scanner-only build measured in UT-4:

| Project | text | bss | role |
|---|---|---|---|
| `bluetooth` | 338,956 | 6,668 | scanner + advertiser |
| `ble_central` | **354,260** | **9,308** | central + GATT client + SM + decoders |
| difference | **+15,304** | +2,640 | |

**The finding.** Everything that separates "counts advertising reports" from
"replaces the phone application" costs 15,304 bytes of flash — under 0.4 % of
the Pico 2 W's 4 MiB. The 225 KB CYW43439 firmware blob dwarfs it. Whatever
the reason for reaching for a phone, it is not that the microcontroller is too
small.

### VT-2 — Advertising decode — *pending hardware run*

**Objective.** Confirm that the AD structures printed by the firmware match
what a reference scanner application reports for the same device.

**Procedure.** Flash and scan with a known peripheral in range. Use the
`bluetooth` project on a second board as the known-good target — its payload
is fixed and already documented in UT-4 as `02 01 06 07 09 "Pico2W"`.
Cross-check against nRF Connect on a phone.

**Expected.** `Flags 0x06 LE-general-discoverable BR/EDR-not-supported` and
`Local name  Pico2W`, with an RSSI within a few dB of the phone's reading.

**Observed.** _(record console output here)_

### VT-3 — GATT discovery and read — *pending hardware run*

**Objective.** Confirm the full service/characteristic walk and the decoded
values.

**Procedure.** Target a device with a Device Information Service and a Battery
Service. Compare every handle, UUID, property flag and decoded value against
the same device opened in nRF Connect.

**Expected.** Identical service and characteristic handle ranges; `0x2A19`
rendered as a percentage matching the app; `0x2A29`/`0x2A24` matching the
app's manufacturer and model strings.

**Observed.** _(record console output here)_

### VT-4 — Pairing on an encrypted characteristic — *pending hardware run*

**Objective.** Confirm the firmware recovers from an encryption-required read
rather than failing it.

**Procedure.** Target a peripheral with at least one characteristic marked
encrypted-read. Watch for `ATT error 0x05`/`0x0F`, the pairing attempt, and
the retry.

**Expected.** `needs encryption; pairing...` then `Paired and bonded.` then the
value. A second run should read it without pairing again, the keys having been
bonded.

**Observed.** _(record console output here)_

### VT-5 — Notifications — *pending hardware run*

**Objective.** Confirm the CCCD write actually starts the stream.

**Procedure.** Target a device with a NOTIFY characteristic (a heart-rate belt,
or any sensor with a measurement characteristic). Observe repeated `notify`
lines.

**Expected.** Values arriving unprompted at the peripheral's own rate.

**Observed.** _(record console output here)_

## Known limits, stated honestly

- **128-bit vendor UUIDs are printed as `0x0000`.** The firmware names 16-bit
  SIG-assigned UUIDs only. A characteristic with a custom 128-bit UUID is
  discovered, listed and read, but cannot be named — because nothing public
  names it.
- **Manufacturer data is not interpreted.** Company identifier and raw bytes
  only. See the fourth reason above.
- **Just Works pairing only.** No display, no keypad, so no passkey entry and
  no numeric comparison. A peripheral demanding MITM protection will refuse.
- **Bonding keys do not survive a power cycle.** `NVM_NUM_LINK_KEYS` is set,
  but the Pico port keeps the LE device database in RAM. Persisting it to
  flash is the natural next step, and the thing that would let the board
  re-identify a device using a resolvable private address across reboots.
- **One connection at a time.** `MAX_NR_HCI_CONNECTIONS 1`.

## Where this sits in the V-model

`bluetooth` (UT-4) verified the radio and both GAP roles. This project is the
next unit up: it verifies the **GATT client** and the **security manager** as
subsystems. It is still unit-level — one board, one peripheral, no network
involved. Combining it with the `webserver` (IT-1) to republish a BLE sensor's
readings over HTTP would be the integration-level case, and the obvious
follow-on.
