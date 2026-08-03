# EthNow

ESP-NOW → MQTT bridge for the ESP32-C3 ETH01-EVO. ESP-IDF, no Arduino.

The DM9051 carries all IP traffic. The C3 radio never associates — it sits in
permanent ESP-NOW receive on a fixed channel.

Ethernet is a DM9051 over SPI; the pin assignments live in `src/config.h`.

## Topics

For every received frame, with `MAC` formatted `%02x%02x%02x%02x%02x%02x`:

| Topic | Payload | Order |
|---|---|---|
| `${CONFIG_MQTT_BASE}${MAC}/rssi` | RSSI in dBm, decimal | first |
| `${CONFIG_MQTT_BASE}${MAC}` | the ESP-NOW payload, byte for byte | second |

Payloads over 250 bytes (the ESP-NOW v1.0 limit) are not accepted.

## LEDs

Two indicators, both active low (the pin sinks the LED), with their GPIOs set
in `src/config.h` — `-1` for either one that is not fitted:

| Config | Default | Meaning |
|---|---|---|
| `CONFIG_LED_MQTT_GPIO` | 2 | solid while the broker connection is up |
| `CONFIG_LED_ESPNOW_GPIO` | 5 | lit on a received frame, out after a period of silence |

## Status page

`http://<host>/` is a live status page: hostname, address (and whether it was
leased), MQTT connection, ESP-NOW packet count, channel and long-range state,
plus the last `CONFIG_WEB_HISTORY` received frames with their uptime stamp,
sender, RSSI and payload. Click a row to keep it — a kept row stays visible
after it ages out of the device's ring, sinking to the bottom of the table —
and press <kbd>Esc</kbd> to clear the selection.

Everything arrives over a websocket on `/ws`; the page itself is a single
document embedded in the firmware, with no external requests of any kind.

## Building

Edit `src/config.h` first — at minimum `CONFIG_MQTT_URI`.

```sh
pio run -e uart -t upload     # first flash, over UART
pio run -t upload             # every flash after that, over ethernet
pio device monitor            # console on UART0, 115200
```

Both environments build byte-identical firmware; they differ only in how it is
delivered. The console is on UART0 at its default pins and is mirrored to
USB-serial-JTAG, so it shows up on whichever the host is attached to.

Set the OTA target with `custom_ota_host` in `platformio.ini` (device IP or
`<hostname>.local`). Upload is a plain `curl` POST of `firmware.bin` to
`http://<host>/update`; the status page footer reports the running version and
slot. The device logs transfer progress and throughput as it writes.

If the UART upload is unreliable on your cable, lower `upload_speed` in
`[env:uart]`.

## Notes

- **No authentication on OTA or the status page.** Keep the device on a
  trusted network.
- The status page never blocks reception: the ESP-NOW dispatch task only copies
  each frame into a ring buffer, and a separate task does the websocket writes.
  A stalled browser costs it frames, not the device.
- Subsystem startup failures are logged, not fatal — a wiring fault leaves the
  device up and complaining on the console instead of boot looping.
- Dual OTA slots in a 4 MB flash; sizes are in `partitions.csv`. A UART flash
  also rewrites the OTA selector, so it always boots what you just flashed.
- ESP-NOW frames land in a fixed pool of `CONFIG_ESPNOW_QUEUE_LEN` slots. The
  payload is copied exactly once, out of the Wi-Fi driver's transient buffer;
  the dispatch task and MQTT publish work from that same slot. If publishing
  falls behind, frames are dropped rather than blocking the Wi-Fi task, and a
  warning is logged.
- Log tags name their subsystem. Only ERROR/WARN/INFO are used, and
  DEBUG/VERBOSE are compiled out of the image.
- The ESP-IDF release is pinned by URL in `platformio.ini`, and the managed
  component versions in `dependencies.lock`. The platform itself is not pinned.
- mDNS comes from the `espressif/mdns` managed component (`src/idf_component.yml`)
  and only exists so `<hostname>.local` resolves for OTA. Delete that file to
  build offline.
