/*
 * app_id.h -- device identity shared by the ethernet and OTA layers.
 */
#pragma once

/*
 * Hostname for this device: CONFIG_HOSTNAME verbatim if defined, otherwise
 * "<CONFIG_HOSTNAME_PREFIX>-xxxxxx" built from the low three bytes of the
 * base MAC. The returned pointer is static and valid for the process
 * lifetime; safe to hand to esp_netif_set_hostname() without copying.
 */
const char *app_hostname(void);
