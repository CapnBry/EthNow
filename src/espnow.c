#include "espnow.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config.h"

static const char *TAG = "espnow";

#define DISPATCH_STACK  4096
#define DISPATCH_PRIO   5

/*
 * Two queues over one static pool: slots move free -> ready -> free and are
 * never allocated, freed or copied between. The RX callback owns a slot for
 * exactly one memcpy, the dispatch task owns it for the duration of the
 * consumer callback.
 */
static espnow_msg_t  s_pool[CONFIG_ESPNOW_QUEUE_LEN];
static QueueHandle_t s_free;
static QueueHandle_t s_ready;

static espnow_msg_cb_t s_cb;
static void           *s_cb_ctx;
static uint32_t        s_dropped;

#ifdef CONFIG_ESPNOW_MAC
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;                          /* fold A-F onto a-f */
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/*
 * Parse "xx:xx:xx:xx:xx:xx" (or '-' separated) into six bytes. Hand-rolled
 * rather than sscanf: this runs once at boot and sscanf would pull the whole
 * formatted-input machinery into the image for it.
 */
static esp_err_t mac_parse(const char *s, uint8_t out[6])
{
    for (int i = 0; i < 6; i++) {
        int hi = hex_nibble(s[0]);
        int lo = hi < 0 ? -1 : hex_nibble(s[1]);
        if (lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        s += 2;

        if (i < 5) {
            if (*s != ':' && *s != '-') {
                return ESP_ERR_INVALID_ARG;
            }
            s++;
        }
    }
    return *s == '\0' ? ESP_OK : ESP_ERR_INVALID_ARG;
}
#endif

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > (int)sizeof(((espnow_msg_t *)0)->data)) {
        return;
    }

    espnow_msg_t *msg;
    if (xQueueReceive(s_free, &msg, 0) != pdTRUE) {
        /* Never block the Wi-Fi task -- shed the frame instead. */
        s_dropped++;
        return;
    }

    memcpy(msg->mac, info->src_addr, sizeof(msg->mac));
    msg->rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    msg->len = (uint16_t)len;
    memcpy(msg->data, data, (size_t)len);

    if (xQueueSend(s_ready, &msg, 0) != pdTRUE) {
        /* Cannot happen: both queues are pool-sized. Recycle defensively. */
        xQueueSend(s_free, &msg, 0);
        s_dropped++;
    }
}

static void dispatch_task(void *arg)
{
    for (;;) {
        espnow_msg_t *msg;
        if (xQueueReceive(s_ready, &msg, portMAX_DELAY) == pdTRUE) {
            /* Logged here rather than in the consumer so frames still show up
             * when the consumer drops them, e.g. while MQTT is disconnected. */
            ESP_LOGI(TAG, "%02x:%02x:%02x:%02x:%02x:%02x %ddBm len %u",
                     msg->mac[0], msg->mac[1], msg->mac[2],
                     msg->mac[3], msg->mac[4], msg->mac[5],
                     msg->rssi, (unsigned)msg->len);

            s_cb(msg, s_cb_ctx);
            xQueueSend(s_free, &msg, 0);
        }
    }
}

static esp_err_t wifi_listen_only(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");

    /* Nothing to persist: no SSID, no credentials, and the channel is fixed
     * in config.h. Keeps NVS out of the boot path. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");

#ifdef CONFIG_ESPNOW_MAC
    /* Must happen while the interface is still stopped. */
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(mac_parse(CONFIG_ESPNOW_MAC, mac), TAG,
                        "CONFIG_ESPNOW_MAC is not a valid MAC address");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mac(WIFI_IF_STA, mac), TAG, "set_mac");
#endif

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    /* Constant listen: no modem sleep, no association, fixed channel. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set_ps");

#if defined(CONFIG_ESPNOW_LR) && CONFIG_ESPNOW_LR
    /* LR is added alongside b/g/n rather than selected alone, matching the IDF
     * ESP-NOW example: senders that do not use LR are still received. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocol(WIFI_IF_STA,
                                              WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                              WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR),
                        TAG, "set_protocol");
#else
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocol(WIFI_IF_STA,
                                              WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                              WIFI_PROTOCOL_11N),
                        TAG, "set_protocol");
#endif

    ESP_RETURN_ON_ERROR(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE),
                        TAG, "set_channel");
    return ESP_OK;
}

esp_err_t espnow_start(espnow_msg_cb_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "no callback");
    s_cb = cb;
    s_cb_ctx = ctx;

    s_free = xQueueCreate(CONFIG_ESPNOW_QUEUE_LEN, sizeof(espnow_msg_t *));
    s_ready = xQueueCreate(CONFIG_ESPNOW_QUEUE_LEN, sizeof(espnow_msg_t *));
    ESP_RETURN_ON_FALSE(s_free && s_ready, ESP_ERR_NO_MEM, TAG, "queue alloc");

    for (size_t i = 0; i < CONFIG_ESPNOW_QUEUE_LEN; i++) {
        espnow_msg_t *slot = &s_pool[i];
        xQueueSend(s_free, &slot, 0);
    }

    ESP_RETURN_ON_ERROR(wifi_listen_only(), TAG, "wifi");
    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "now_init");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(on_recv), TAG, "recv_cb");

    ESP_RETURN_ON_FALSE(
        xTaskCreate(dispatch_task, "espnow_rx", DISPATCH_STACK, NULL, DISPATCH_PRIO, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "task create");

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "listening on channel %d (%s) as %02x:%02x:%02x:%02x:%02x:%02x",
             CONFIG_ESPNOW_CHANNEL,
#if defined(CONFIG_ESPNOW_LR) && CONFIG_ESPNOW_LR
             "11b/g/n+LR",
#else
             "11b/g/n",
#endif
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

uint32_t espnow_dropped(void)
{
    return s_dropped;
}
