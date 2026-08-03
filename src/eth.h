/*
 * eth.h -- SPI ethernet link, the device's only IP interface.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Invoked from the default event loop task when the interface becomes usable
 * (an address is bound) and again when it goes away. Never invoked
 * re-entrantly; safe to start/stop other services from inside it.
 */
typedef void (*eth_state_cb_t)(bool online, void *ctx);

/*
 * Bring up SPI, the MAC/PHY driver and the netif, then start the link.
 * Requires nvs_flash, the default event loop and esp_netif to be initialised.
 * Returns once the driver is running -- the link itself comes up
 * asynchronously and is reported through @cb.
 */
esp_err_t eth_start(eth_state_cb_t cb, void *ctx);

/*
 * Current address as a dotted quad, "0.0.0.0" while the link has none, and
 * whether it was leased rather than configured. @dhcp may be NULL.
 *
 * Reads a snapshot taken when the address was bound rather than calling into
 * esp_netif, so it is safe from any task.
 */
void eth_ip_str(char *out, size_t cap, bool *dhcp);
