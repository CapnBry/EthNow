#include "eth.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#if __has_include("esp_eth_mac_spi.h")
#include "esp_eth_mac_spi.h"   /* ETH_W5500_DEFAULT_CONFIG moved here in IDF 5.2 */
#endif
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"

#include "app_id.h"
#include "config.h"

static const char *TAG = "eth";

static esp_netif_t     *s_netif;
static esp_eth_handle_t s_eth;
static eth_state_cb_t   s_cb;
static void            *s_cb_ctx;

#ifdef CONFIG_IP
static esp_err_t apply_static_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip = { 0 };

    if (esp_netif_str_to_ip4(CONFIG_IP, &ip.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_NETMASK, &ip.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_GATEWAY, &ip.gw) != ESP_OK) {
        ESP_LOGE(TAG, "bad static address in config.h");
        return ESP_ERR_INVALID_ARG;
    }

    /* The DHCP client owns the address by default and must release it first. */
    esp_netif_dhcpc_stop(netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip), TAG, "set_ip_info");

    esp_netif_dns_info_t dns = { 0 };
    if (esp_netif_str_to_ip4(CONFIG_DNS, &dns.ip.u_addr.ip4) == ESP_OK) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    return ESP_OK;
}
#endif

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6] = { 0 };
        esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "link up, mac %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        if (s_cb) {
            s_cb(false, s_cb_ctx);
        }
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;

    ESP_LOGI(TAG, "ip " IPSTR " mask " IPSTR " gw " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));

    if (s_cb) {
        s_cb(true, s_cb_ctx);
    }
}

esp_err_t eth_start(eth_state_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    const spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_ETH_PIN_SCLK,
        .mosi_io_num = CONFIG_ETH_PIN_MOSI,
        .miso_io_num = CONFIG_ETH_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(CONFIG_ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize");

    /*
     * esp_eth does not install the GPIO ISR service itself -- the application
     * must, before the MAC is created. Without it the driver's
     * gpio_isr_handler_add() fails, no RX interrupt is ever delivered, and the
     * driver's receive task silently falls back to draining the chip on its
     * 1 s notify timeout. The link still works; it just runs at a few KB/s.
     */
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_ERR_INVALID_STATE) {   /* already installed is fine */
        ESP_RETURN_ON_ERROR(isr_err, TAG, "gpio_install_isr_service");
    }

    /* A DM9051 command frame is a 1-bit read/write flag plus a 7-bit register
     * address; letting the SPI peripheral emit them keeps each driver transfer
     * to a single descriptor over the payload buffer. */
    /* Not const: ETH_DM9051_DEFAULT_CONFIG() stores a mutable pointer to it. */
    spi_device_interface_config_t devcfg = {
        .command_bits = 1,
        .address_bits = 7,
        .mode = 0,
        .clock_speed_hz = CONFIG_ETH_SPI_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_ETH_PIN_CS,
        .queue_size = 20,
    };

    eth_dm9051_config_t dm9051_config = ETH_DM9051_DEFAULT_CONFIG(CONFIG_ETH_SPI_HOST, &devcfg);
    dm9051_config.int_gpio_num = CONFIG_ETH_PIN_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_dm9051(&dm9051_config, &mac_config);

    /* The DM9051's PHY is internal and always answers at address 1. */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = CONFIG_ETH_PIN_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_dm9051(&phy_config);

    ESP_RETURN_ON_FALSE(mac && phy, ESP_ERR_NO_MEM, TAG, "dm9051 driver alloc failed");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth), TAG, "driver_install");

    /* The DM9051 has no burned-in address unless an EEPROM is fitted; take the
     * ETH slot out of the ESP32's own MAC block so it stays stable across
     * reflashes. */
    uint8_t mac_addr[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac_addr, ESP_MAC_ETH), TAG, "read_mac");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, mac_addr), TAG, "set_mac");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_NO_MEM, TAG, "netif_new");

    /* Set before the interface starts so it rides along in the first DISCOVER. */
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_netif, app_hostname()), TAG, "set_hostname");

#ifdef CONFIG_IP
    ESP_RETURN_ON_ERROR(apply_static_ip(s_netif), TAG, "static_ip");
    ESP_LOGI(TAG, "host %s, static " CONFIG_IP, app_hostname());
#else
    ESP_LOGI(TAG, "host %s, dhcp", app_hostname());
#endif

    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)),
                        TAG, "netif_attach");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL, NULL),
                        TAG, "eth_event");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL, NULL),
                        TAG, "ip_event");

    return esp_eth_start(s_eth);
}
