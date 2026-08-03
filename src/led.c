#include "led.h"

#include "driver/gpio.h"
#include "esp_check.h"

#include "config.h"

static const char *TAG = "led";

static const int s_gpio[LED_COUNT] = {
    [LED_MQTT]   = CONFIG_LED_MQTT_GPIO,
    [LED_ESPNOW] = CONFIG_LED_ESPNOW_GPIO,
};

esp_err_t led_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < LED_COUNT; i++) {
        if (s_gpio[i] >= 0) {
            mask |= 1ULL << s_gpio[i];
        }
    }
    if (!mask) {
        return ESP_OK;
    }

    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config");

    /* Explicitly dark rather than whatever the pins came out of reset at --
     * on an active-low output that would otherwise read as "connected". */
    for (int i = 0; i < LED_COUNT; i++) {
        led_set((led_t)i, false);
    }
    return ESP_OK;
}

void led_set(led_t led, bool on)
{
    if (s_gpio[led] >= 0) {
        gpio_set_level(s_gpio[led], !on);
    }
}
