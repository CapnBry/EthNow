#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "web.h"

static const char *TAG = "ota";

/* One SPI flash page per write keeps esp_ota_write() on its fast path. */
#define OTA_CHUNK 4096

/* Consecutive recv timeouts tolerated before the upload is declared dead.
 * Multiply by httpd_config_t.recv_wait_timeout for the wall-clock budget. */
#define OTA_MAX_STALLS 3

/* Progress log interval, in bytes. */
#define OTA_REPORT_EVERY (128 * 1024)

static httpd_handle_t s_server;

/*
 * Throughput since @since_us, in bytes per second.
 *
 * Signed 64-bit microseconds, matching esp_timer_get_time(): it cannot wrap in
 * any realistic uptime, so there is no modular-arithmetic reason to prefer
 * unsigned, and a negative interval reads as obviously wrong rather than as a
 * plausible huge positive.
 */
static int rate_bps(int bytes, int64_t since_us)
{
    int64_t elapsed = esp_timer_get_time() - since_us;
    return elapsed > 0 ? (int)((int64_t)bytes * 1000000 / elapsed) : 0;
}

static esp_err_t fail(httpd_req_t *req, httpd_err_code_t code, const char *why)
{
    ESP_LOGE(TAG, "%s", why);
    httpd_resp_send_err(req, code, why);
    return ESP_FAIL;
}

static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
    }
    if (req->content_len <= 0) {
        return fail(req, HTTPD_400_BAD_REQUEST, "empty body");
    }

    ESP_LOGI(TAG, "update started, %d bytes -> %s", req->content_len, target->label);

    /* esp_ota_begin() erases the destination up front -- a second or more of
     * dead time before the first byte can land. Timed so the gap in the log is
     * self-explanatory rather than looking like a stall. */
    const int64_t erase_start_us = esp_timer_get_time();

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "erased in %d ms",
             (int)((esp_timer_get_time() - erase_start_us) / 1000));

    static char buf[OTA_CHUNK];
    const int total = req->content_len;
    int remaining = total;
    int stalls = 0;
    int next_report = OTA_REPORT_EVERY;

    /* Clock starts at the first byte written, so the partition erase inside
     * esp_ota_begin() above is not charged against the transfer rate. */
    int64_t started_us = 0;

    while (remaining > 0) {
        int want = remaining < OTA_CHUNK ? remaining : OTA_CHUNK;
        int got = httpd_req_recv(req, buf, want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            /* Bounded: an upload that stops mid-flight has to surface as an
             * error, not spin here holding the socket open forever. */
            if (++stalls > OTA_MAX_STALLS) {
                esp_ota_abort(handle);
                ESP_LOGE(TAG, "stalled at %d/%d bytes", total - remaining, total);
                return fail(req, HTTPD_408_REQ_TIMEOUT, "upload stalled");
            }
            ESP_LOGW(TAG, "rx stall %d at %d/%d bytes", stalls, total - remaining, total);
            continue;
        }
        if (got <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "recv failed at %d/%d bytes", total - remaining, total);
            return fail(req, HTTPD_400_BAD_REQUEST, "upload truncated");
        }
        stalls = 0;

        if (remaining == total) {           /* nothing written yet */
            started_us = esp_timer_get_time();
        }

        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        }
        remaining -= got;

        if (total - remaining >= next_report) {
            ESP_LOGI(TAG, "%d/%d bytes, %d B/s", total - remaining, total,
                     rate_bps(total - remaining, started_us));
            next_report += OTA_REPORT_EVERY;
        }
    }

    /* Validates the image header and checksum before it can be booted. */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        return fail(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    httpd_resp_sendstr(req, "ok, rebooting\n");
    ESP_LOGW(TAG, "update complete, %d bytes at %d B/s, rebooting into %s",
             total, rate_bps(total, started_us), target->label);

    /* Let the response and the final log line drain before the reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_set_online(bool online)
{
    if (!online) {
        if (s_server) {
            /* Before httpd_stop(): the status page holds this handle for its
             * asynchronous websocket sends. */
            web_detach();
            httpd_stop(s_server);
            s_server = NULL;
        }
        return ESP_OK;
    }
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_OTA_PORT;
    cfg.stack_size = 8192;          /* esp_ota_write() is stack-hungry */
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd_start");

    static const httpd_uri_t update_uri = {
        .uri = CONFIG_OTA_PATH,
        .method = HTTP_POST,
        .handler = update_post,
    };
    httpd_register_uri_handler(s_server, &update_uri);

    esp_err_t err = web_attach(s_server);
    if (err != ESP_OK) {
        /* The status page is not worth losing the update endpoint over. */
        ESP_LOGE(TAG, "status page unavailable: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "listening on port %d, POST " CONFIG_OTA_PATH, CONFIG_OTA_PORT);
    return ESP_OK;
}
