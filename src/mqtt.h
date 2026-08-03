/*
 * mqtt.h -- broker connection and publish path.
 *
 * The client is created once at boot and started/stopped as the ethernet
 * link comes and goes; esp-mqtt owns the reconnect backoff from there.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Create the client. Does not touch the network. */
esp_err_t mqtt_init(void);

/* Follow the link state. Both are idempotent. */
void mqtt_set_online(bool online);

/* True once CONNACK has been seen. */
bool mqtt_is_connected(void);

/*
 * Publish @len bytes of @data to @topic. The payload is handed to esp-mqtt
 * as-is -- no NUL termination required and no interpretation applied.
 * Returns ESP_OK if the message was queued, ESP_ERR_INVALID_STATE while
 * offline.
 */
esp_err_t mqtt_publish(const char *topic, const void *data, size_t len);
