#include "info.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_id.h"
#include "config.h"
#include "espnow.h"
#include "eth.h"
#include "mqtt.h"

/* After config.h, not with the other IDF headers: the guard is one of ours. */
#if CONFIG_INFO_TEMPERATURE
#include "driver/temperature_sensor.h"
#endif

/* CONFIG_ETH_SPI_HOST is an spi_host_device_t, not a number. */
#include "driver/spi_common.h"

static const char *TAG = "info";

/*
 * The whole document, built in one buffer and sent in one call. Comfortably
 * larger than the ~2 KB it actually takes; the buffer is heap and lives only
 * for the duration of a request, and running out mid-object would produce
 * JSON the page cannot parse at all.
 */
#define JSON_CAP 3072

/* The embedded info page (EMBED_TXTFILES in CMakeLists.txt). */
extern const char info_html_start[] asm("_binary_info_html_start");
extern const char info_html_end[]   asm("_binary_info_html_end");

/* ==========================================================================
 * JSON builder
 * ========================================================================== */

/*
 * Append-only, overflow-latching. Once a write does not fit, `ovf` stays set
 * and every later write is a no-op, so the caller can emit the entire document
 * and check once at the end rather than after every field.
 */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   ovf;
} jbuf_t;

static void jb(jbuf_t *j, const char *fmt, ...)
{
    if (j->ovf) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= j->cap - j->len) {
        j->ovf = true;
        return;
    }
    j->len += (size_t)n;
}

static void jb_mac(jbuf_t *j, const char *key, const uint8_t mac[6])
{
    jb(j, "\"%s\":\"%02x:%02x:%02x:%02x:%02x:%02x\",",
       key, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ==========================================================================
 * Sources
 * ========================================================================== */

/*
 * Bytes of @part actually occupied by the app image.
 *
 * Walks the image's segment headers the way the bootloader does, which costs a
 * handful of 8-byte reads -- esp_image_verify() would give the same answer but
 * re-hashes the entire megabyte to do it. Returns 0 if the partition does not
 * hold something that looks like an image.
 */
static uint32_t image_size(const esp_partition_t *part)
{
    esp_image_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        return 0;
    }

    uint32_t off = sizeof(hdr);
    for (int i = 0; i < hdr.segment_count && i < ESP_IMAGE_MAX_SEGMENTS; i++) {
        esp_image_segment_header_t seg;
        if (esp_partition_read(part, off, &seg, sizeof(seg)) != ESP_OK) {
            return 0;
        }
        off += sizeof(seg) + seg.data_len;
        if (off > part->size) {
            return 0;               /* header garbage, or a truncated image */
        }
    }

    /* One checksum byte, then padded out to a 16-byte boundary, then the
     * optional whole-image SHA256 -- the same arithmetic esp_image_format.c
     * does when it records image_len. */
    off = (off + 1 + 15) & ~15U;
    if (hdr.hash_appended) {
        off += 32;
    }
    return off > part->size ? 0 : off;
}

static const char *flash_mode_str(void)
{
    /* What the flash is being driven as right now, which is not necessarily
     * what was configured: the ROM bootloader takes the mode out of the image
     * header, and esptool writes QIO images with a DIO header on this family
     * because the ROM cannot boot QIO directly. */
    if (!esp_flash_default_chip) {
        return "unknown";
    }
    switch (esp_flash_default_chip->read_mode) {
    case SPI_FLASH_SLOWRD: return "slow read";
    case SPI_FLASH_FASTRD: return "fast read";
    case SPI_FLASH_DOUT:   return "DOUT";
    case SPI_FLASH_DIO:    return "DIO";
    case SPI_FLASH_QOUT:   return "QOUT";
    case SPI_FLASH_QIO:    return "QIO";
    default:               return "unknown";
    }
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
    }
}

static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default:           return "unknown";
    }
}

#if CONFIG_INFO_TEMPERATURE
/*
 * Installed on first use and left installed: the sensor needs a moment to
 * settle after being enabled, so cycling it per request would report a number
 * that is wrong in a way nothing on the page could explain. Only ever reached
 * from an httpd handler, and httpd runs its handlers one at a time, so the
 * lazy install needs no lock.
 */
static temperature_sensor_handle_t s_tsens;

static bool temp_celsius(float *out)
{
    if (!s_tsens) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
            return false;
        }
        if (temperature_sensor_enable(s_tsens) != ESP_OK) {
            temperature_sensor_uninstall(s_tsens);
            s_tsens = NULL;
            return false;
        }
    }
    return temperature_sensor_get_celsius(s_tsens, out) == ESP_OK;
}
#endif

/* ==========================================================================
 * Document
 * ========================================================================== */

static void build(jbuf_t *j)
{
    const esp_app_desc_t  *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    jb(j, "{");

    /* ---- identity ---- */
    jb(j, "\"host\":\"%s\",", app_hostname());
    jb(j, "\"project\":\"%s\",", app->project_name);
    jb(j, "\"ver\":\"%s\",", app->version);
    jb(j, "\"built\":\"%s %s\",", app->date, app->time);
    jb(j, "\"idf\":\"%s\",", app->idf_ver);
    jb(j, "\"elf\":\"%s\",", esp_app_get_elf_sha256_str());
    jb(j, "\"up\":%lu,", (unsigned long)(esp_timer_get_time() / 1000));
    jb(j, "\"reset\":\"%s\",", reset_reason_str());
    jb(j, "\"tasks\":%u,", (unsigned)uxTaskGetNumberOfTasks());

    /* ---- chip ---- */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    jb(j, "\"chip\":\"%s\",", chip_model_str(chip.model));
    jb(j, "\"rev\":\"v%d.%d\",", chip.revision / 100, chip.revision % 100);
    jb(j, "\"cores\":%d,", chip.cores);
    jb(j, "\"feat\":\"%s%s%s%s\",",
       chip.features & CHIP_FEATURE_WIFI_BGN ? "802.11b/g/n " : "",
       chip.features & CHIP_FEATURE_BLE ? "BLE " : "",
       chip.features & CHIP_FEATURE_BT ? "BT " : "",
       chip.features & CHIP_FEATURE_EMB_FLASH ? "embedded flash" : "external flash");

    /* The C3 has no separate die serial: the base MAC burned at the factory is
     * what everything else -- including esptool's "Chip ID" -- derives from. */
    uint8_t base[6] = { 0 };
    esp_read_mac(base, ESP_MAC_BASE);
    jb_mac(j, "chipid", base);

    jb(j, "\"cpu_mhz\":%d,", esp_clk_cpu_freq() / 1000000);
    jb(j, "\"cpu_cfg_mhz\":%d,", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    jb(j, "\"xtal_mhz\":%d,", esp_clk_xtal_freq() / 1000000);
    jb(j, "\"apb_mhz\":%d,", esp_clk_apb_freq() / 1000000);

#ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm = { 0 };
    if (esp_pm_get_configuration(&pm) == ESP_OK) {
        jb(j, "\"pm\":true,\"pm_max\":%d,\"pm_min\":%d,\"pm_ls\":%s,",
           pm.max_freq_mhz, pm.min_freq_mhz, pm.light_sleep_enable ? "true" : "false");
    } else {
        jb(j, "\"pm\":true,\"pm_max\":0,\"pm_min\":0,\"pm_ls\":false,");
    }
#else
    jb(j, "\"pm\":false,\"pm_max\":0,\"pm_min\":0,\"pm_ls\":false,");
#endif

#if CONFIG_INFO_TEMPERATURE
    float celsius;
    if (temp_celsius(&celsius)) {
        jb(j, "\"temp\":%.1f,", celsius);
    } else {
        jb(j, "\"temp\":null,");
    }
#else
    jb(j, "\"temp\":null,");
#endif

    /* ---- flash ---- */
    uint32_t flash_id = 0;
    uint32_t flash_size = 0;
    esp_flash_read_id(NULL, &flash_id);
    esp_flash_get_size(NULL, &flash_size);
    jb(j, "\"flash_id\":\"%06lx\",", (unsigned long)(flash_id & 0xffffff));
    jb(j, "\"flash_mfr\":\"%02lx\",", (unsigned long)((flash_id >> 16) & 0xff));
    jb(j, "\"flash_size\":%lu,", (unsigned long)flash_size);
    jb(j, "\"flash_mode\":\"%s\",", flash_mode_str());
    jb(j, "\"flash_mode_cfg\":\"%s\",",
#if defined(CONFIG_ESPTOOLPY_FLASHMODE_QIO)
       "QIO"
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_QOUT)
       "QOUT"
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_DIO)
       "DIO"
#elif defined(CONFIG_ESPTOOLPY_FLASHMODE_DOUT)
       "DOUT"
#else
       "unknown"
#endif
    );
    jb(j, "\"flash_freq\":\"%s\",", CONFIG_ESPTOOLPY_FLASHFREQ);

    /* ---- app partitions ---- */
    uint32_t used = running ? image_size(running) : 0;
    jb(j, "\"slot\":\"%s\",", running ? running->label : "?");
    jb(j, "\"slot_addr\":%lu,", (unsigned long)(running ? running->address : 0));
    jb(j, "\"slot_size\":%lu,", (unsigned long)(running ? running->size : 0));
    jb(j, "\"slot_used\":%lu,", (unsigned long)used);
    jb(j, "\"slot_free\":%lu,",
       (unsigned long)(running && used ? running->size - used : 0));
    jb(j, "\"next_slot\":\"%s\",", next ? next->label : "none");

    /* ---- memory ---- */
    multi_heap_info_t heap;
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL);
    jb(j, "\"heap_free\":%lu,", (unsigned long)heap.total_free_bytes);
    jb(j, "\"heap_used\":%lu,", (unsigned long)heap.total_allocated_bytes);
    jb(j, "\"heap_total\":%lu,",
       (unsigned long)(heap.total_free_bytes + heap.total_allocated_bytes));
    jb(j, "\"heap_block\":%lu,", (unsigned long)heap.largest_free_block);
    jb(j, "\"heap_min\":%lu,", (unsigned long)esp_get_minimum_free_heap_size());

    /* ---- LAN ---- */
    eth_info_t eth;
    eth_get_info(&eth);
    jb_mac(j, "eth_mac", eth.mac);
    jb(j, "\"eth_up\":%s,", eth.up ? "true" : "false");
    jb(j, "\"eth_speed\":%d,", eth.speed_mbps);
    jb(j, "\"eth_duplex\":\"%s\",", eth.full_duplex ? "full" : "half");
    jb(j, "\"ip\":\"%s\",", eth.ip);
    jb(j, "\"netmask\":\"%s\",", eth.netmask);
    jb(j, "\"gw\":\"%s\",", eth.gw);
    jb(j, "\"dns\":\"%s\",", eth.dns);
    jb(j, "\"dhcp\":%s,", eth.dhcp ? "true" : "false");
    jb(j, "\"phy\":\"DM9051 over SPI%d @ %d MHz\",",
       CONFIG_ETH_SPI_HOST + 1, CONFIG_ETH_SPI_MHZ);

    /* ---- radio ---- */
    uint8_t wifi_factory[6] = { 0 };
    uint8_t now_mac[6] = { 0 };
    esp_read_mac(wifi_factory, ESP_MAC_WIFI_STA);
    espnow_get_mac(now_mac);
    jb_mac(j, "wifi_mac", wifi_factory);
    jb_mac(j, "now_mac", now_mac);
#ifdef CONFIG_ESPNOW_MAC
    jb(j, "\"now_mac_override\":true,");
#else
    jb(j, "\"now_mac_override\":false,");
#endif
    jb(j, "\"ch\":%d,", CONFIG_ESPNOW_CHANNEL);
#if defined(CONFIG_ESPNOW_LR) && CONFIG_ESPNOW_LR
    jb(j, "\"lr\":true,");
#else
    jb(j, "\"lr\":false,");
#endif
    jb(j, "\"rx\":%lu,", (unsigned long)espnow_received());
    jb(j, "\"drop\":%lu,", (unsigned long)espnow_dropped());
    jb(j, "\"queue\":%d,", CONFIG_ESPNOW_QUEUE_LEN);

    /* ---- services ---- */
    jb(j, "\"broker\":\"" CONFIG_MQTT_URI "\",");
    jb(j, "\"mqtt\":%s,", mqtt_is_connected() ? "true" : "false");
    jb(j, "\"topic\":\"" CONFIG_MQTT_BASE "\",");
    jb(j, "\"qos\":%d,", CONFIG_MQTT_QOS);
    jb(j, "\"retain\":%s,", CONFIG_MQTT_RETAIN ? "true" : "false");
    jb(j, "\"ota_path\":\"" CONFIG_OTA_PATH "\",");
    jb(j, "\"ota_port\":%d,", CONFIG_OTA_PORT);
    jb(j, "\"hist\":%d,", CONFIG_WEB_HISTORY);
    jb(j, "\"ws_clients\":%d,", CONFIG_WEB_WS_CLIENTS);

    /* ---- partition table ---- */
    jb(j, "\"parts\":[");
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (bool first = true; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        jb(j, "%s{\"label\":\"%s\",\"type\":%d,\"sub\":%d,\"addr\":%lu,\"size\":%lu}",
           first ? "" : ",", p->label, p->type, p->subtype,
           (unsigned long)p->address, (unsigned long)p->size);
        first = false;
    }
    /* A no-op as written -- esp_partition_next() releases the iterator when it
     * runs out -- but the loop is one `break` away from needing it. */
    esp_partition_iterator_release(it);
    jb(j, "]}");
}

/* ==========================================================================
 * Handlers
 * ========================================================================== */

static esp_err_t info_get(httpd_req_t *req)
{
    /* EMBED_TXTFILES appends a NUL that is not part of the document. */
    const size_t len = (size_t)(info_html_end - info_html_start) - 1;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, info_html_start, len);
}

static esp_err_t info_json_get(httpd_req_t *req)
{
    /* Heap rather than the handler's stack: 3 KB is most of what httpd gives a
     * handler, and esp_ota_write() on the same task already wants the rest. */
    jbuf_t j = { .buf = malloc(JSON_CAP), .cap = JSON_CAP };
    if (!j.buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    build(&j);

    esp_err_t err;
    if (j.ovf) {
        ESP_LOGE(TAG, "json overflowed %d bytes -- raise JSON_CAP", JSON_CAP);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "info too large");
        err = ESP_FAIL;
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        err = httpd_resp_send(req, j.buf, j.len);
    }

    free(j.buf);
    return err;
}

esp_err_t info_attach(httpd_handle_t server)
{
    static const httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_get,
    };
    static const httpd_uri_t info_json_uri = {
        .uri = "/info.json",
        .method = HTTP_GET,
        .handler = info_json_get,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &info_uri), TAG, "info");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &info_json_uri), TAG, "info.json");
    return ESP_OK;
}
