/*
 * led.h -- the two status indicators.
 *
 * Both are active low: the pin sinks the LED, so lighting one drives its GPIO
 * low. That inversion lives entirely in led.c -- callers only ever say on or
 * off. A pin configured as -1 is not touched at all, so a board with only one
 * LED fitted needs no other change.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    LED_MQTT,                           /* solid while the broker is connected */
    LED_ESPNOW,                         /* lit after a received frame */
    LED_COUNT,
} led_t;

/* Configure the pins as outputs and leave both dark. */
esp_err_t led_init(void);

void led_set(led_t led, bool on);
