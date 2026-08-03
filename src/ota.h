/*
 * ota.h -- unauthenticated firmware upload over the LAN.
 *
 * Serves POST ${CONFIG_OTA_PATH} on port CONFIG_OTA_PORT. The request body is
 * the raw firmware.bin, streamed straight into the inactive OTA slot; the
 * device reboots into it once the image verifies. This is what the
 * platformio "ota" environment drives via curl.
 *
 * There is no authentication -- keep the device on a trusted network.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Start/stop the HTTP server. Both are idempotent; drive from the link state. */
esp_err_t ota_set_online(bool online);
