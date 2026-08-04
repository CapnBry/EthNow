/*
 * eth.h -- SPI ethernet link, the device's only IP interface.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Everything the interface knows about itself, as text where the only consumer
 * is a display. Filled from the event task and copied out under no lock -- see
 * eth_get_info().
 */
typedef struct {
    char    ip[16];
    char    netmask[16];
    char    gw[16];
    char    dns[16];                    /* main resolver; "0.0.0.0" if unset */
    uint8_t mac[6];
    bool    dhcp;                       /* leased rather than configured */
    bool    up;                         /* PHY link, independent of the address */
    int     speed_mbps;                 /* 0 while the link is down */
    bool    full_duplex;
} eth_info_t;

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

/*
 * The whole snapshot, for the info page. Same deal as eth_ip_str(): a struct
 * copy of values recorded by the event task, so no esp_netif call happens on
 * the caller's task. Fields describing a link that is down read as zero or
 * "0.0.0.0" rather than as the last thing that was true.
 */
void eth_get_info(eth_info_t *out);
