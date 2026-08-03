#include "app_id.h"

#include <stdio.h>

#include "esp_mac.h"

#include "config.h"

const char *app_hostname(void)
{
#ifdef CONFIG_HOSTNAME
    return CONFIG_HOSTNAME;
#else
    /* sizeof includes the NUL, +1 for the '-', +6 hex digits. */
    static char s_hostname[sizeof(CONFIG_HOSTNAME_PREFIX) + 7];

    if (s_hostname[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_hostname, sizeof(s_hostname), CONFIG_HOSTNAME_PREFIX "-%02x%02x%02x",
                 mac[3], mac[4], mac[5]);
    }
    return s_hostname;
#endif
}
