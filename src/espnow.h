/*
 * espnow.h -- always-on ESP-NOW listener.
 *
 * Wi-Fi is brought up in STA mode but never associates: it parks on
 * CONFIG_ESPNOW_CHANNEL with power save off so the radio is always listening.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_now.h"

/*
 * A received frame, owned by a fixed pool. Handed to the consumer by
 * pointer -- the payload is copied exactly once, out of the Wi-Fi driver's
 * transient buffer, and is not touched again.
 */
typedef struct {
    uint8_t  mac[6];                    /* sender's address */
    int8_t   rssi;                      /* dBm, from the frame's RX control block */
    uint16_t len;                       /* payload length, never 0 */
    uint8_t  data[ESP_NOW_MAX_DATA_LEN];
} espnow_msg_t;

/*
 * Called from the ESP-NOW dispatch task -- not the Wi-Fi task -- so blocking
 * on the network here is fine. @msg is valid only for the duration of the
 * call; the slot is recycled on return.
 */
typedef void (*espnow_msg_cb_t)(const espnow_msg_t *msg, void *ctx);

/* Start Wi-Fi, ESP-NOW and the dispatch task. */
esp_err_t espnow_start(espnow_msg_cb_t cb, void *ctx);

/* Frames dropped because the pool was exhausted (consumer fell behind). */
uint32_t espnow_dropped(void);
