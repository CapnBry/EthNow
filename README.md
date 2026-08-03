# EthNow

ESP-NOW to MQTT bridge for the ESP32-C3 ETH01-EVO

Converts incoming ESP-NOW from the wireless C3 to Ethernet over MQTT. Could this be done with espnhome? Probably, but when I had the idea it couldn't and I didn't count on those nerds being as incredibly productive as they are.

Ethernet is DM9051 over SPI. Config is in `src/config.h`

## Topics

Senders prefix the frame as `key\tpayload`, where key is the topic suffix. Example `status\t{"v":123}` from `a4cf12000000` publishes `{"v":123}` to `espnow/a4cf12000000/status`.

| Topic | Payload |
|---|---|
| `${CONFIG_MQTT_BASE}${MAC}/rssi` | RSSI in dBm, decimal |
| `${CONFIG_MQTT_BASE}${MAC}/${KEY}` | Frame payload following the tab |

A frame with no tab in it or with a key longer than 32 bytes falls back to the un-suffixed `${CONFIG_MQTT_BASE}${MAC}` and published without removing any key.

Payloads over 250 bytes (the ESP-NOW v1.0 limit) are not accepted.

## LEDs

Two indicators, both active low. Use `-1` to disable.

| Config | Default | Description |
|---|---|---|
| `CONFIG_LED_MQTT_GPIO` | 2 | Enabled if MQTT connected |
| `CONFIG_LED_ESPNOW_GPIO` | 5 | Stays on for a few seconds after ESP-NOW received |

## Status page

`http://<host>/` is a live status page, streaming over websocket, with the last `CONFIG_WEB_HISTORY` received frames.

## Building

Edit `src/config.h` setting `CONFIG_MQTT_URI` with the MQTT server. Only unencrypted connections are available by the default build.

```sh
pio run -e uart -t upload     # first flash, over UART
pio run -t upload             # every flash after that, over ethernet
pio device monitor            # console on UART0, 115200
```

The console is on UART0 at with default pins (19,20) and is mirrored to USB-serial D+/D-.

Set the OTA target with `custom_ota_host` in `platformio.ini` (device IP or `<hostname>.local`). Upload is a plain `curl` POST of `firmware.bin` to `http://<host>/update`.

## Notes

* **No access restriction on OTA or the status page.**
* AI disclosure: I started writing this but after getting it building and connecting to ethernet/mqtt, I just wanted to get it done and let Claude and Qwen take the wheel.