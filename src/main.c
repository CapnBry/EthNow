/*
 * EthNow -- ESP-NOW to MQTT bridge.
 *
 * The ethernet port carries all IP traffic; the C3's radio never associates
 * and sits in permanent ESP-NOW receive. Every received frame is republished as:
 *
 *     ${CONFIG_MQTT_BASE}${MAC}/rssi   <- link quality, published first
 *     ${CONFIG_MQTT_BASE}${MAC}/${KEY} <- the payload after the tab, byte for byte
 *
 * Senders prefix the payload with "key\t"; the key becomes the last topic
 * level and is not otherwise interpreted. A frame with no usable key goes out
 * whole on ${CONFIG_MQTT_BASE}${MAC}, as before.
 */
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#if __has_include("mdns.h")
#include "mdns.h"
#define HAVE_MDNS 1
#endif

#include "app_id.h"
#include "config.h"
#include "espnow.h"
#include "eth.h"
#include "led.h"
#include "mqtt.h"
#include "ota.h"
#include "web.h"

static const char *TAG = "main";

#define BASE_LEN  (sizeof(CONFIG_MQTT_BASE) - 1)
#define MAC_LEN   12
#define SUFFIX    "/rssi"

/* A key longer than this is not a key, it is a payload that happens to have a
 * tab in it. Such a frame is published whole, on the MAC topic. */
#define KEY_MAX   32

/*
 * One topic buffer, written in place: the MAC is rendered once and everything
 * after it -- "/rssi", then "/${KEY}" -- is rewritten per publish. Touched
 * only from the ESP-NOW dispatch task, so no locking is needed.
 */
static char s_topic[BASE_LEN + MAC_LEN + 1 + KEY_MAX + 1] = CONFIG_MQTT_BASE;

static uint32_t s_dropped_seen;

static void write_mac_hex(char *dst, const uint8_t mac[6])
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < 6; i++) {
        *dst++ = hex[mac[i] >> 4];
        *dst++ = hex[mac[i] & 0x0f];
    }
}

static void on_espnow_msg(const espnow_msg_t *msg, void *ctx)
{
    /* First, and unconditionally: the status page is most useful precisely
     * when MQTT is down. It only copies the frame -- no I/O happens here. */
    web_on_message(msg);

    if (!mqtt_is_connected()) {
        return;
    }

    write_mac_hex(&s_topic[BASE_LEN], msg->mac);
    memcpy(&s_topic[BASE_LEN + MAC_LEN], SUFFIX, sizeof(SUFFIX));

    char rssi[5];
    int rssi_len = snprintf(rssi, sizeof(rssi), "%d", msg->rssi);
    mqtt_publish(s_topic, rssi, (size_t)rssi_len);

    /*
     * "key\tpayload". Only the tab is looked for -- what follows it is the
     * sender's business and goes out byte for byte.
     */
    char *suffix = &s_topic[BASE_LEN + MAC_LEN];
    const uint8_t *tab = memchr(msg->data, '\t', msg->len);
    size_t key_len = tab ? (size_t)(tab - msg->data) : 0;

    if (key_len > 0 && key_len <= KEY_MAX) {
        *suffix = '/';
        memcpy(suffix + 1, msg->data, key_len);
        suffix[1 + key_len] = '\0';
        mqtt_publish(s_topic, tab + 1, msg->len - key_len - 1);
    } else {
        *suffix = '\0';
        mqtt_publish(s_topic, msg->data, msg->len);
    }

    uint32_t dropped = espnow_dropped();
    if (dropped != s_dropped_seen) {
        ESP_LOGW(TAG, "%lu frames dropped, publish is not keeping up",
                 (unsigned long)(dropped - s_dropped_seen));
        s_dropped_seen = dropped;
    }
}

static void on_link_state(bool online, void *ctx)
{
    mqtt_set_online(online);
    ota_set_online(online);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "ethnow starting as %s", app_hostname());

    /* Before the subsystems that drive them, so neither can light an LED that
     * has not been configured as an output yet. */
    if ((err = led_init()) != ESP_OK) {
        ESP_LOGE(TAG, "led init failed: %s", esp_err_to_name(err));
    }

#ifdef HAVE_MDNS
    /* Lets the platformio "ota" environment target <hostname>.local. */
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(app_hostname());
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_OTA_PORT, NULL, 0);
#endif

    /*
     * Subsystem failures are logged, not fatal. A bad SPI pin or an absent
     * PHY used to abort inside ESP_ERROR_CHECK and reboot, which turned a
     * wiring fault into a boot loop that scrolls its own diagnosis off the
     * console. Staying up keeps the log readable.
     */
    if ((err = mqtt_init()) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt init failed: %s", esp_err_to_name(err));
    }
    if ((err = eth_start(on_link_state, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "ethernet failed to start: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "check the ethernet pin block in config.h -- no LAN, no MQTT, no OTA");
    }
    if ((err = espnow_start(on_espnow_msg, NULL)) != ESP_OK) {
        ESP_LOGE(TAG, "esp-now failed to start: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "bridging to " CONFIG_MQTT_BASE "<mac>");
}
