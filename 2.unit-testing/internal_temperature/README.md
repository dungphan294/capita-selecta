# Pico 2 W — `internal_temperature`

Reads the RP2350's on-die temperature sensor once a second and prints the raw
ADC value, the sensor voltage, and the temperature in Celsius and Fahrenheit,
with running minimum and maximum.

Built on macOS with VS Code, the Raspberry Pi Pico extension, and a Raspberry Pi
Debug Probe. Verified on real hardware.

## Result

Output format, one line per reading:

```txt
RP2350 internal temperature sensor
ADC channel 4, 12-bit, Vref 3.3 V, 16 samples averaged

[    1] raw  875.8  0.7056 V   27.23 degC   81.01 degF   (min 27.23 / max 27.23)
[    2] raw  876.2  0.7059 V   27.04 degC   80.67 degF   (min 27.04 / max 27.23)
[    3] raw  875.1  0.7050 V   27.55 degC   81.59 degF   (min 27.04 / max 27.55)
```

Values read directly off the chip with the debugger, mid-loop:

```txt
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

**27.23 degC** at idle. The raw average of 875.8 out of 4095 is 0.7056 V, which
is almost exactly the datasheet's nominal 0.706 V at 27 degC — so this chip is
sitting essentially at the sensor's calibration point.

## How the sensor works

The RP2350 has a temperature sensor built into the die, wired to the **last ADC
channel** — channel 4 on the RP2350A package used by the Pico 2 W. It is not
exposed on any pin.

| Property | Value |
| --- | --- |
| ADC | 12-bit, 0–4095, ~500 ksps |
| Reference | 3.3 V |
| Channel | `ADC_TEMPERATURE_CHANNEL_NUM` = `NUM_ADC_CHANNELS - 1` = **4** |
| Nominal | 0.706 V at 27 degC |
| Slope | **-1.721 mV per degC** — voltage falls as temperature rises |

Conversion, from the RP2350 datasheet:

```txt
T = 27 - (V - 0.706) / 0.001721
```

Use the SDK macro rather than hardcoding `4`. On the RP2350**B** package
`NUM_ADC_CHANNELS` is 9, so the sensor is on channel 8 — the macro tracks that,
a literal does not.

### It measures the chip, not the room

This is die temperature. The RP2350 warms itself, so the reading sits a few
degrees above ambient and climbs under load. It is genuinely useful for
detecting a hot board, thermal throttling, or a runaway workload — and it is
**not** a room thermometer. For ambient, wire up an external sensor.

Accuracy caveats worth knowing:

- **The sensor is uncalibrated.** Part-to-part variation is a few degrees; two
  Picos side by side will disagree.
- **Vref is assumed to be exactly 3.3 V.** The real rail varies with USB supply
  and load, and that error feeds straight into the result.
- **Absolute accuracy is roughly ±2–3 degC; relative changes are much better.**
  Trends and deltas are trustworthy; the absolute number is not a calibrated
  measurement.

## Project layout

```txt
rasp/internal_temperature/
├── CMakeLists.txt
├── internal_temperature.c
├── pico_sdk_import.cmake
├── .vscode/
└── build/                  generated
```

## Setup

### CMakeLists.txt

```cmake
set(PICO_BOARD pico2_w CACHE STRING "Board type")

target_link_libraries(internal_temperature
    pico_stdlib
    hardware_adc      # the ADC block the sensor feeds
)

pico_enable_stdio_uart(internal_temperature 1)
pico_enable_stdio_usb(internal_temperature 0)
```

`hardware_adc` is the only addition beyond `pico_stdlib`.

### This binary is tiny — and that is the point

```txt
   text    data     bss     dec     hex
  21660       0     792   22452    57b4
```

**21 KB**, against 266 KB for the `blink` project on the same board. The
difference is the CYW43439 WiFi firmware blob: `blink` lights the onboard LED,
which hangs off the WiFi chip, so it must link the whole driver. This project
touches no radio, so none of that is pulled in.

That is also why there is no LED heartbeat here — adding one would cost 225 KB
for a blinking light. If you want one, link `pico_cyw43_arch_none` and accept
the size.

## The code

```c
adc_init();                          // power up the ADC block
adc_set_temp_sensor_enabled(true);   // enable the on-die sensor (off by default)
adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
sleep_ms(10);                        // let the sensor settle after enabling
```

`adc_set_temp_sensor_enabled(true)` is easy to forget. Without it you read a
floating input and get noise that looks like a plausible-but-wrong temperature.

Averaging matters:

```c
uint32_t sum = 0;
for (int i = 0; i < SAMPLES; i++) sum += adc_read();
float raw_avg = (float)sum / SAMPLES;
```

A single 12-bit sample jitters by a couple of LSB. At 1.721 mV per degree, one
LSB is about 0.8 mV — nearly half a degree. Averaging 16 samples costs
microseconds and steadies the reading substantially.

## Build and flash

From the IDE: **Compile Project**, then **Flash Project (SWD)**.

```bash
cd /1.unit-testing/internal_temperature

~/.pico-sdk/cmake/v4.3.4/bin/cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350-arm-s .

~/.pico-sdk/ninja/v1.13.2/ninja -C build

~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" \
  -c "program build/internal_temperature.elf verify reset exit"
```

## Serial Monitor

**Panel → Serial Monitor**, port `/dev/tty.usbmodem114202`, baud `115200`,
line ending `None`.

Only one program may hold a serial port:

```bash
lsof /dev/cu.usbmodem114202 /dev/tty.usbmodem114202
screen -ls && screen -X -S <pid> quit
```

### Reading values without the console

Useful when the console is busy or the monitor has frozen:

```bash
~/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-gdb build/internal_temperature.elf -q -batch \
  -ex "target extended-remote 127.0.0.1:3333" \
  -ex "monitor reset init" \
  -ex "break internal_temperature.c:78" \
  -ex "continue" \
  -ex "print celsius" -ex "print raw_avg"
```

`monitor reset init` matters here. Halting a running target and detaching can
leave it wedged, and the next session then lands in `isr_hardfault` — which
looks like a crash in your code but is an artefact of the previous attach.
Resetting first clears it.

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Temperature obviously wrong (e.g. -40 or 120 degC) | `adc_set_temp_sensor_enabled(true)` missing |
| Reading jumps by a degree between samples | increase `SAMPLES` |
| Reads a few degrees high | expected — this is die temperature, not ambient |
| Two boards disagree | expected — the sensor is uncalibrated |
| `undefined reference to adc_init` | `hardware_adc` not in `target_link_libraries` |
| Lands in `isr_hardfault` under GDB | use `monitor reset init` before `continue` |
| Console frozen | check whether the `lsof` offset is growing; restart the monitor |

## Next steps

- **Calibrate** against a known-good thermometer and apply the offset.
- **Compensate Vref** by measuring VSYS on ADC channel 3 instead of assuming
  3.3 V.
- **Log to CSV** over serial and plot the warm-up curve.
- **Watch it under load** — run both cores flat out and see how far it climbs.
- **Expose it over BLE** as a GATT characteristic, combining this with the
  `bluetooth` project so a phone can read the temperature.
