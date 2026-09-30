# Pico 2 W — `webserver`

An HTTP server running on the board itself. Joins a WiFi network, listens on
port 80, and serves a status page showing the RP2350's on-die temperature,
uptime, and how many requests it has handled. The page refreshes every 5
seconds.

No cloud, no broker, no host PC — the board is the web server.

Built on macOS with VS Code, the Raspberry Pi Pico extension, and a Raspberry Pi
Debug Probe.

## Result

![The status page served by the Pico 2 W](docs/images/browser-status-page.png)

Live chip temperature, IP address, uptime, request count and chip details —
generated and served by the board itself, with no host involved.

Verified independently with `curl` from a machine on the same network:

```
$ curl -D - http://172.20.10.11/

HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
Content-Length: 991
Connection: close
```

The body measured **exactly 991 bytes** against the declared `Content-Length:
991` — nothing truncated, which confirms the connection is closed only after
all data is acknowledged.

Values from that response:

```
25.73 degC
IP address        172.20.10.11
Uptime            0 h 1 m 19 s
Requests served   23
Chip              RP2350 (Cortex-M33)
WiFi              CYW43439
```

And the counter read straight off the chip afterwards, having climbed further
from the page's own 5-second auto-refresh:

```
(gdb) print request_count
$1 = 29
```

Console output:

```
Starting Wi-Fi...
Connecting to 'iPhone13'...
Connected. IP address: 172.20.10.11
HTTP server listening on http://172.20.10.11/
Open that address in a browser on the same network.

Request 1 -> 1187 bytes
Request 2 -> 1187 bytes
```

## Seeing it work

**The browser must be on the same network as the board.** This is the one thing
that catches people out — a Pico on a phone hotspot is not reachable from a
laptop on a different WiFi network. There is no routing between them. The
screenshot above was taken after joining the laptop to the same hotspot the
board is on.

### Option A — phone hotspot

The phone providing the hotspot is on the same subnet as the board, so it can
reach it directly.

1. Turn the hotspot on, with **Maximize Compatibility** enabled (2.4 GHz — the
   CYW43439 has no 5 GHz radio).
2. Reset the board and read the IP from the console.
3. Open `http://172.20.10.11/` in the phone's browser.

### Option B — join the same network as your computer

Put that network's credentials in `wifi_secrets.cmake`, rebuild, and the board
gets an address on the same subnet:

```cmake
set(WIFI_SSID     "YourHomeNetwork")
set(WIFI_PASSWORD "...")
```

Then from the computer:

```bash
curl -v http://<pico-ip>/
```

This is the better option for development — it lets you test with `curl`,
browser dev tools, and load-testing tools.

## How it works

lwIP's **raw TCP API**, not sockets. There is no operating system and no thread
to block, so the server is a set of callbacks that lwIP invokes from a
background interrupt.

```c
struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
tcp_bind(pcb, IP_ANY_TYPE, 80);
struct tcp_pcb *listener = tcp_listen_with_backlog(pcb, 4);
tcp_accept(listener, http_accept);
```

Four callbacks carry the whole server:

| Callback | Fires when | Does |
|---|---|---|
| `http_accept` | a client connects | allocates per-connection state, registers the rest |
| `http_recv` | request bytes arrive | builds the page, `tcp_write` + `tcp_output` |
| `http_sent` | the client ACKs data | closes and frees once everything is acknowledged |
| `http_err` | the connection fails | frees state; lwIP has already freed the pcb |

Three details that matter:

**`tcp_recved(pcb, p->tot_len)`** — tells lwIP you have consumed the data so it
can reopen the receive window. Omit it and the connection stalls after the first
segment.

**`pbuf_free(p)`** — lwIP's buffers are reference-counted and yours to release.
Leaking them exhausts `PBUF_POOL_SIZE` and the server dies after a few requests.

**Close in `http_sent`, not in `http_recv`.** Closing right after `tcp_write`
discards unacknowledged data, and the browser gets a truncated page. Wait until
`sent >= len`.

The request itself is never parsed — any request gets the same page. That is
deliberate: it keeps the example to one job. Parsing the path is the natural
next step.

### Why `Connection: close`

Each response sets `Content-Length` and `Connection: close`, so the browser
knows where the body ends and does not hold the socket open. With only a handful
of TCP PCBs configured in `lwipopts.h`, keep-alive connections would exhaust
them quickly.

### Temperature on the page

Same sensor as the `internal_temperature` project — ADC channel 4, 16 samples
averaged, `T = 27 - (V - 0.706) / 0.001721`. It is read fresh on every request,
so reloading the page shows the chip actually warming and cooling.

## Setup

### CMakeLists.txt

```cmake
target_link_libraries(webserver
    pico_stdlib
    hardware_adc
    pico_cyw43_arch_lwip_threadsafe_background   # WiFi + lwIP on a background IRQ
)
```

`threadsafe_background` is the right choice: lwIP is serviced from an interrupt,
so `main()` is free and the TCP callbacks fire on their own. With
`..._lwip_poll` you would have to call `cyw43_arch_poll()` continuously or the
server would never respond.

### lwipopts.h

Required — lwIP ships no defaults. Copied from the `wifi` project; `LWIP_TCP`,
`LWIP_DHCP` and the `TCP_*` buffer sizes are what matter here.

### Credentials

Same gitignored pattern as the `wifi` project:

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/wifi_secrets.cmake OPTIONAL)
target_compile_definitions(webserver PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
)
```

## Build and flash

```bash
cd /2.integration-testing/webserver

~/.pico-sdk/cmake/v4.3.4/bin/cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DPICO_BOARD=pico2_w -DPICO_PLATFORM=rp2350-arm-s .

~/.pico-sdk/ninja/v1.13.2/ninja -C build

~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
  -c "adapter speed 5000" \
  -c "program build/webserver.elf verify reset exit"
```

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Console stops after `Connecting to '...'` | network down, 5 GHz only, or wrong password — see the `wifi` project's README |
| `Failed to connect` then a reboot loop | returning from `main()` restarts the runtime; expected on failure |
| Browser times out | your computer is on a different network from the board |
| Page loads once, then never again | pbufs leaked, or the connection closed before data was ACKed |
| Page is truncated | closed in `http_recv` instead of `http_sent` |
| `undefined reference to tcp_new_ip_type` | wrong cyw43 arch library — needs an `_lwip_` variant |

Check what the board thinks its address is, without the console:

```bash
~/.pico-sdk/toolchain/15_2_Rel1/bin/arm-none-eabi-gdb build/webserver.elf -q -batch \
  -ex "target extended-remote 127.0.0.1:3333" -ex "monitor halt" \
  -ex "print/x netif_default->ip_addr.addr" -ex "print request_count"
```

`0x0` means DHCP has not completed — the board is not on the network yet. The
address is little-endian, so `0x0b0a14ac` is `172.20.10.11`.

## Next steps

- **Parse the request path** and serve more than one page.
- **Add a JSON endpoint** (`/api/temp`) so other programs can poll the board.
- **Control an LED or GPIO** from a link or form — the classic IoT demo.
- **Serve from flash** with lwIP's `httpd` and `makefsdata` for real static
  files.
- **Add mDNS** so it answers to `pico.local` instead of a bare IP.
