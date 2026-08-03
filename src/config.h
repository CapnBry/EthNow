/*
 * config.h -- compile-time configuration for EthNow.
 *
 * Everything here is a plain #define so unused paths fold away at compile
 * time. Options documented as "undefine to ..." must be #undef / commented
 * out entirely -- defining them to an empty value will not work.
 */
#pragma once

/* ==========================================================================
 * Network
 * ========================================================================== */

/* Fixed IPv4 address for the ethernet interface.
 * Comment out to use DHCP. */
// #define CONFIG_IP           "192.168.1.50"

/* Only used when CONFIG_IP is defined. */
#define CONFIG_NETMASK          "255.255.255.0"
#define CONFIG_GATEWAY          "192.168.1.1"
#define CONFIG_DNS              "192.168.1.1"

/* System hostname, also sent as the DHCP client identifier.
 * Comment out to derive a unique name ("ethnow-a1b2c3") from the MAC. */
#define CONFIG_HOSTNAME         "ethnow"

/* Prefix used when no CONFIG_HOSTNAME is given. */
#define CONFIG_HOSTNAME_PREFIX  "ethnow"

/* ==========================================================================
 * MQTT
 * ========================================================================== */

/* Broker URI: mqtt://host[:port], with optional user:pass@ credentials. */
#define CONFIG_MQTT_URI         "mqtt://streaky.lan:1883"

/* Base topic. Published topics are:
 *   ${CONFIG_MQTT_BASE}${MAC}          <- payload, verbatim
 *   ${CONFIG_MQTT_BASE}${MAC}/rssi     <- RSSI, published first
 * Include the trailing separator. */
#define CONFIG_MQTT_BASE        "espnow_t/"

#define CONFIG_MQTT_QOS         0
#define CONFIG_MQTT_RETAIN      0

/* ==========================================================================
 * ESP-NOW
 * ========================================================================== */

/* Channel the receiver parks on. Must match the senders. */
#define CONFIG_ESPNOW_CHANNEL   13

/* Non-zero adds Espressif's long-range PHY to the protocol bitmap, alongside
 * 802.11b/g/n rather than replacing it -- the combination the IDF ESP-NOW
 * example uses, so LR and standard-rate senders are both received. Zero
 * leaves the radio on 802.11b/g/n, which is also the ESP-IDF default. */
#define CONFIG_ESPNOW_LR        1

/* Override the station MAC used for ESP-NOW reception, so senders keyed to a
 * specific peer address keep working across hardware swaps. Comment out to
 * keep the factory MAC. Six hex pairs separated by ':' or '-'; case does not
 * matter. Byte 0 bit 0 must be clear (a unicast address) or esp_wifi_set_mac()
 * rejects it -- the locally-administered bit is fine to set. */
#define CONFIG_ESPNOW_MAC    "7c:2c:67:c7:5e:6c"

/* Receive queue depth. Each slot is ~260 bytes of static BSS and decouples
 * the Wi-Fi RX callback from MQTT publish latency. */
#define CONFIG_ESPNOW_QUEUE_LEN 8

/* ==========================================================================
 * Status LEDs
 * ========================================================================== */

/* Both are active low -- the pin sinks the LED, so it lights when the GPIO is
 * driven low. Set to -1 to leave a pin alone. */

/* Solid while the MQTT broker connection is up. */
#define CONFIG_LED_MQTT_GPIO    2

/* Lit on every received ESP-NOW frame, dark again after a quiet spell.
 * GPIO2 is a C3 strapping pin and has to read high at reset -- an active-low
 * LED wired to 3V3 holds it there, but do not add a pull-down. */
#define CONFIG_LED_ESPNOW_GPIO  5

/* ==========================================================================
 * DM9051 SPI ethernet wiring (ETH01-EVO)
 * ========================================================================== */
#define CONFIG_ETH_SPI_HOST     SPI2_HOST
#define CONFIG_ETH_PIN_SCLK     7
#define CONFIG_ETH_PIN_MOSI     10
#define CONFIG_ETH_PIN_MISO     3
#define CONFIG_ETH_PIN_CS       9
#define CONFIG_ETH_PIN_INT      8
#define CONFIG_ETH_PIN_RST      6     /* -1 if not wired */
#define CONFIG_ETH_SPI_MHZ      20    /* DM9051 is qualified to 20 MHz */

/* ==========================================================================
 * OTA
 * ========================================================================== */

/* Unauthenticated firmware upload endpoint, reachable on the LAN only. */
#define CONFIG_OTA_PORT         80
#define CONFIG_OTA_PATH         "/update"

/* ==========================================================================
 * Status page
 * ========================================================================== */

/* Received frames kept for the status page, on the same server as OTA. Each
 * slot is ~264 bytes of static BSS. A browser that connects late is replayed
 * this many frames; anything older is gone. */
#define CONFIG_WEB_HISTORY      10

/* Browsers that can watch at once. Each holds an HTTP socket open for as long
 * as its tab is; the rest of the server's sockets stay free for OTA. */
#define CONFIG_WEB_WS_CLIENTS   4
