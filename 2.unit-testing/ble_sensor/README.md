# `ble_sensor` — the known-good peripheral

A reference target for the other two BLE projects. It reads the RP2350's
on-die temperature once a second and publishes it **twice**, by the two
different mechanisms a BLE device has:

| Mechanism | Payload | Read by | Needs |
|---|---|---|---|
| **Broadcast** | Environmental Sensing (`0x181A`) service data in every advertisement | `setup_portal` | nothing — no connection, no GATT client, no pairing |
| **Connection** | GATT Temperature characteristic `0x2A6E`, read or notify | `ble_central` | a central with a GATT client |

Both carry the same number in the same encoding — `sint16`, centi-degrees
Celsius — so if the two readers disagree, the fault is in a reader, not in
the source. That is what makes this a verification instrument rather than a
demonstration.

It also exposes Device Information (`Saxion` / `Pico 2 W` / `1.0`) and
Battery Service, which gives `ble_central` several known strings and a known
percentage to decode.

## The advertising payload, byte for byte

```
02 01 06                           flags: LE general discoverable, no BR/EDR
05 16 1A 18 <lo> <hi>              service data, UUID 0x181A, sint16 centi-degC
0E 09 "Pico2W-Sensor"              complete local name
```

25 bytes, inside the 31-byte limit. The two temperature bytes are patched in
place on every update and the payload handed back to the controller, so no
reassembly happens on the hot path.

Worked example: 21.50 degC is 2150 = `0x0866`, transmitted little-endian as
`66 08`. The console prints the hex form of each reading so the two can be
compared directly against a scanner's output.

## Build and run

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.1
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
export PATH=$PICO_TOOLCHAIN_PATH/bin:$PATH
cmake -S . -B build -G Ninja -DPICO_PLATFORM=rp2350-arm-s -DCMAKE_BUILD_TYPE=Debug
ninja -C build
```

Flash it to the **second** board. Console, as everywhere else, is the Debug
Probe UART bridge at 115200 8N1.

```
[1] 27.23 degC  (advertised as 0x0AA3)
Central connected
Temperature notifications on
```

## Verification

**VT-1 — Build — PASS.** Clean build, no warnings.

```
   text	   data	    bss	    dec	    hex	filename
 347688	      0	   6752	 354440	  56888	build/ble_sensor.elf
```

8,732 bytes more than the `bluetooth` scanner, which is the cost of a GATT
server and its generated attribute database.

**Cross-check to run on hardware.** The whole point of this board is that
three independent readers should agree on one number:

| Reader | Route | Expected |
|---|---|---|
| This board's console | direct ADC | the reference value |
| `setup_portal` pinned card | advertisement service data | same, within one update period |
| `ble_central` | GATT read of `0x2A6E` | same, within one update period |
| nRF Connect | either | same |

Any disagreement larger than the 1 s update interval localises the fault to
one reader.

**Cross-check against UT-2.** The conversion is the one verified in the
`internal_temperature` project — 16 samples averaged, then
`T = 27 - (V - 0.706) / 0.001721`. A value consistent with UT-2's 27.23 degC
means the sensor path is unchanged and only the transport is new.
