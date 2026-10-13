# `setup_portal` — configure the board over BLE, then over the web

Three ways in, in the order you need them:

| # | Path | Available when | How |
|---|---|---|---|
| 1 | **BLE** | always | Provisioning GATT service, always advertising as `Pico2W-Setup`. Write SSID and password, send the join command, watch the status characteristic. Needs no network of any kind. |
| 2 | **Access point** | until credentials are saved | The board raises its own WPA2 network `Pico2W-Setup`, runs a DHCP server, and serves the settings page at `http://192.168.4.1/`. |
| 3 | **Station** | once joined | The same settings page, on the real network. |

The BLE radio does two jobs throughout: advertising the setup service as a
**peripheral**, and passively scanning as a **central** so the page can show
nearby sensors. One pinned device (matched by advertised name) gets a card
with its live reading; everything else in range goes in a table.

---

## eduroam will not work, and this is not a configuration problem

You asked for eduroam. It cannot be done on this hardware with this SDK, so
the portal is built to say so clearly rather than fail with a timeout.

**The evidence.** The Pico SDK's CYW43 driver exposes exactly four
authorisation modes, documented at `lib/cyw43-driver/src/cyw43.h:457-460`:

```
CYW43_AUTH_OPEN           | Use an open access point with no authorisation required
CYW43_AUTH_WPA_TKIP_PSK   | Use WPA authorisation
CYW43_AUTH_WPA2_AES_PSK   | Use WPA2 (preferred)
CYW43_AUTH_WPA2_MIXED_PSK | Use WPA2/WPA mixed
```

All four are **pre-shared key**. A search of the entire driver and the SDK's
WiFi layer for `enterprise`, `EAP`, `802.1x`, `PEAP`, `MSCHAP` or `RADIUS`
returns two hits, both comments in `cyw43_ll.c:575,579`:

```c
// TODO need to parse the IE to check for TKIP, AES and enterprise modes
```

There is no EAP state machine, no TLS supplicant, no certificate store and no
identity handling anywhere in the stack. eduroam is WPA2-Enterprise
(802.1X, normally PEAP-MSCHAPv2 or EAP-TTLS), so there is nothing to
configure — the capability is absent, not disabled.

**What the portal does instead.** `wifi_join()` refuses a known-enterprise
SSID up front with an explanation, rather than calling the driver and waiting
for the generic `-2` that WiFi defect #3 and #4 both produced. The scan table
marks those networks, and the page carries a standing note. Diagnosing this
once cost real time; the firmware now diagnoses it every time.

**Workarounds, in order of convenience:**

1. **Phone hotspot** — what UT-3 used (`iPhone13`). Forced to 2.4 GHz.
2. **A WPA2-PSK device network.** Many universities run a separate SSID for
   equipment that cannot do 802.1X, usually with MAC registration. Worth
   asking Saxion ICT whether one exists before assuming it does not.
3. **A travel router** that authenticates to eduroam upstream and presents
   WPA2-PSK downstream. This is the only option that puts the board on the
   campus network itself.

A fourth option exists and is not recommended here: the full Infineon WHD
with a supplicant component supports enterprise auth on some CYW chips, but
it is not the driver the Pico SDK ships, and swapping it out is a project in
itself rather than a setting.

---

## Files

| File | Contents |
|---|---|
| `setup_portal.c` | Startup, mode selection, the action loop |
| `config_store.c` | Settings in flash, behind a magic number and a CRC |
| `wifi_manager.c` | Scan, asynchronous join with *distinguishable* failures, AP mode |
| `dhcpserver.c` | Minimal DHCP server — lwIP ships a client only |
| `http_portal.c` | Asset serving, form handling, `/api/json`, the `/classic` page |
| `ble_portal.c` | Provisioning GATT server **and** scanner, both roles at once |
| `portal_actions.c` | The queue where the two front ends meet |
| `portal_gatt.gatt` | Provisioning service definition, compiled at build time |
| `web/` | The real `index.html`, `app.css`, `app.js` |
| `tools/embed_assets.py` | Gzips `web/` into `assets.c` at build time |

`assets.c` and `assets.h` are generated into `build/generated/web_assets/` —
not checked in, not edited by hand.

### Four design decisions worth the words

**Nothing acts inside a callback.** A BLE characteristic write runs inside
BTstack's packet handling; an HTTP request runs inside an lwIP callback.
Joining a network or erasing a flash sector from either is a good way to find
out what a watchdog reset looks like. Both front ends call
`portal_queue_action()`; `main()` executes.

**The configuration sector is the fourth from the end**, not the last:

```
-1  reserved  — RP2350 errata E10 workaround wants it free, and the SDK's own
                BTstack flash bank deliberately skips it
-2  \  BTstack link-key store (PICO_FLASH_BANK_TOTAL_SIZE = 2 sectors),
-3  /  claimed whenever pico_btstack_flash_bank is linked
-4  <- this configuration
```

The obvious choice — the last sector — is the one the errata workaround wants
free. Sector -4 is safe today and still safe if bonding keys are persisted
later.

**Credentials are never compile-time.** There is no `wifi_secrets.cmake` in
this project, deliberately. Defect #4 was an undefined CMake variable
expanding silently to `-DWIFI_SSID=""`; a runtime store cannot reproduce it.

**The page is a file in flash, not a string in a function.** `web/` holds real
HTML, CSS and JavaScript. `tools/embed_assets.py` gzips each file at build time
and emits it as a `const unsigned char[]`:

```
embed_assets: 3 file(s), 18,546 B -> 6,230 B in flash (12,316 B saved)
  /app.css         7,212 B ->   2,134 B  gzip
  /app.js          7,846 B ->   2,896 B  gzip
  /index.html      3,491 B ->   1,200 B  gzip
```

`const` puts them in `.rodata`, and flash is memory-mapped, so `pump()` hands
lwIP the pointer **without** `TCP_WRITE_FLAG_COPY`:

```c
if (tcp_write(pcb, c->body + c->body_sent, chunk, 0) != ERR_OK) break;
```

lwIP holds the reference until the bytes are acknowledged, which is safe only
because flash never moves — and it means a large asset costs no more RAM than
a small one. It also ties into the rule above: `flash_range_program` disables
XIP while it runs, so writing settings while one of these sends is in flight
would read garbage. Settings are only ever written from `main()`, after the
response has drained.

The live values arrive as JSON from `/api/json`, polled every 3 s. The
server-rendered page is kept at **`/classic`**: it needs no JavaScript, renders
the scan lists itself, and is what every captive-portal probe receives.

**`sscanf` is not used for request parsing.** Pulling one path out of a request
line with `sscanf("%*s %159s")` links newlib's `__ssvfscanf_r`, `_strtod_l` and
its float tables — **49,188 bytes** of flash, measured by removing it. A
twelve-line `request_path()` does the same job.

---

## Build

```bash
export PICO_SDK_PATH=$HOME/.pico-sdk/sdk/2.3.1
export PICO_TOOLCHAIN_PATH=$HOME/.pico-sdk/toolchain/15_2_Rel1
export PATH=$PICO_TOOLCHAIN_PATH/bin:$PATH
cmake -S . -B build -G Ninja -DPICO_PLATFORM=rp2350-arm-s -DCMAKE_BUILD_TYPE=Debug
ninja -C build
```

## Use

**First boot, over WiFi:** join `Pico2W-Setup` / `picosetup`, open
`http://192.168.4.1/`, press *Scan for networks*, fill in SSID and password,
*Save and connect*. The console prints the new address.

**First boot, over BLE:** connect to `Pico2W-Setup` with any GATT client
(nRF Connect, or the `ble_central` project on another board). Service
`4A7F0000-1C2B-4F7A-9D3E-0A1B2C3D4E5F`:

| Characteristic | Access | Meaning |
|---|---|---|
| `…0001` | read / write | WiFi SSID |
| `…0002` | **write only** | WiFi password — never readable back |
| `…0003` | write | Command: 1 join, 2 save, 3 scan, 4 start AP, 5 factory reset |
| `…0004` | read / notify | Status text |
| `…0005` | read | Portal URL once there is an address |
| `…0006` | read | Scan results, one network per read |

Write the SSID, write the password, write `0x01`. Subscribe to `…0004`
first and the join progress is notified to you.

**JSON** for anything that is not a browser:

```bash
curl http://<address>/api/json
```

---

## Verification

### VT-1 — Build, both boards — **PASS**

Clean configure and build from an empty tree, Pico SDK 2.3.1 /
arm-none-eabi-gcc 15.2.1, zero warnings across all seven translation units.

```
   text	   data	    bss	    dec	    hex	filename
 415548	      0	  71164	 486712	  76d38	setup_portal.elf
 347688	      0	   6752	 354440	  56888	ble_sensor.elf
```

In the context of the other projects on this board:

| Project | text | bss | What it links |
|---|---|---|---|
| `internal_temperature` | 21,660 | 792 | ADC only |
| `blink` | 263,436 | 3,216 | + CYW43 firmware blob |
| `wifi` | 312,528 | 48,388 | + lwIP |
| `webserver` | 321,816 | 48,768 | + TCP server |
| `bluetooth` | 338,956 | 6,668 | BTstack, scanner + advertiser |
| `ble_sensor` | 347,688 | 6,752 | BTstack, GATT server |
| `ble_central` | 354,260 | 9,308 | BTstack, GATT client + SM |
| **`setup_portal`** | **415,548** | **71,164** | **both radios, both BLE roles, HTTP, DHCP, flash store** |

**The finding.** Running WiFi and Bluetooth simultaneously, in both LE roles,
with a web server and a DHCP server, costs 415 KB of flash and 71 KB of RAM —
about 10 % of the 4 MiB flash and 14 % of the 520 KB SRAM. The bss figure is
the interesting one: 71 KB against the webserver's 49 KB, and almost all of
the difference is lwIP buffers plus BTstack's HCI buffers coexisting. Neither
radio is close to crowding the other out.

### VT-2 — Access-point mode and DHCP — *pending hardware run*

**Objective.** Confirm a client joining `Pico2W-Setup` is given an address
and reaches the portal.

**Procedure.** Boot with no saved configuration. From a laptop, join the
network, check the address obtained, request the page, and confirm the body
length matches the declared `Content-Length` — the assertion that caught the
premature-close fault in IT-1.

**Expected.** An address in `192.168.4.2-9`, gateway `192.168.4.1`, and
`HTTP/1.1 200` whose body length equals `Content-Length`.

**Observed.** _(record `curl -D - http://192.168.4.1/ | wc -c` here)_

### VT-3 — BLE provisioning with no network present — *pending hardware run*

**Objective.** Confirm the board can be put on a WiFi network entirely over
BLE, with no prior network access of any kind.

**Procedure.** Boot with no saved configuration. Connect with a GATT client,
subscribe to `…0004`, write SSID to `…0001`, password to `…0002`, `0x01` to
`…0003`. Record the notified status sequence and the final value of `…0005`.

**Expected.** `SSID received` → `Password received` → `Joining <ssid>` →
`Connected. http://<address>/`, and `…0005` reading back that URL.

**Observed.** _(record here)_

### VT-4 — Persistence across a power cycle — *pending hardware run*

**Objective.** Confirm settings survive, and that the CRC catches a blank
sector rather than treating it as empty credentials.

**Procedure.** Provision, power-cycle, confirm it rejoins unattended. Then
*Erase saved settings* and power-cycle again.

**Expected.** First boot after provisioning: `config: loaded. SSID '…',
provisioned yes` and an automatic join. After the erase: `config: no saved
configuration (magic 0x…)` and the AP back up.

**Observed.** _(record here)_

### VT-5 — Both BLE roles at once — *pending hardware run*

**Objective.** Confirm advertising and scanning genuinely coexist, rather
than one starving the other.

**Procedure.** With `ble_sensor` running on the second board, open the portal
page with a BLE client *also* connected to the setup service. Confirm the
pinned card updates while the BLE connection is live.

**Expected.** The pinned `Pico2W-Sensor` card showing a temperature that
tracks the sensor board's console, while the BLE status characteristic still
notifies.

**Observed.** _(record here)_

### VT-6 — Failure messages are distinguishable — *pending hardware run*

**Objective.** The point of the whole exercise: three failures that used to
produce one error code must now produce three different sentences.

**Procedure.** Attempt each of: a wrong password; an SSID that is not
present; `eduroam`.

**Expected.**

| Attempt | Expected message |
|---|---|
| Wrong password | `Authentication rejected — wrong password for '…'.` |
| Absent SSID | `'…' was not found. It may be hidden, out of range, or 5 GHz only…` |
| `eduroam` | `'eduroam' is WPA2-Enterprise (802.1X). The CYW43439 driver supports pre-shared keys only — it has no EAP supplicant.` |

Note the third is produced **without touching the radio**, so it appears
immediately rather than after 30 seconds.

**Observed.** _(record here)_

---

## Known limits, stated honestly

- **WPA2-Enterprise is impossible**, as above. The enterprise check is a
  name list (`eduroam`, `saxion`, `govroam`, …), not a measurement — the
  driver reports an `auth_mode` taken from beacon capability bits without
  parsing the RSN information element, so an enterprise network and a
  WPA2-PSK one are indistinguishable in a scan result. Since neither can be
  joined if it is enterprise, naming the common ones beats a measurement
  that cannot be made. Add your own SSIDs to `enterprise_ssids[]`.
- **The setup AP password is hardcoded** (`picosetup`). It is a temporary
  network, but it is still a shared secret in the binary.
- **No captive-portal redirect.** You must type `192.168.4.1` yourself; the
  DHCP server hands out itself as DNS but nothing answers DNS queries.
- **The password characteristic is write-only, but the link is not
  encrypted.** Pairing is not required to write it. On an open BLE link the
  passphrase is recoverable by anyone sniffing during provisioning. Requiring
  encryption on `…0002` is the obvious hardening and is a small change.
- **One HTTP response at a time per connection**, no keep-alive, no TLS.
- **`/api/json` is read-only.** All mutation is form POSTs.

## Where this sits in the V-model

This is **integration level**, and the largest one here: it combines WiFi
(UT-3), the HTTP server (IT-1), Bluetooth in both roles (UT-4), the ADC path
by way of the companion sensor (UT-2), and a new flash-storage subsystem.
The interfaces under test are the ones that only exist once the pieces are
combined:

| Interface | Between |
|---|---|
| Action queue | BLE write callback / HTTP callback → `main()` |
| Shared async context | CYW43 driver → lwIP **and** BTstack run loop |
| Flash store ↔ radio | `flash_safe_execute` → interrupts off while both radios are live |
| Advertising payload → page | `ble_sensor` service data → portal card |
| AP netif → DHCP server | `cyw43_arch_enable_ap_mode` → UDP port 67 |

The `flash_safe_execute` interface is the one most likely to bite: it
disables interrupts, and both radios are serviced by interrupts.
