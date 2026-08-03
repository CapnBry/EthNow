#include "web.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_id.h"
#include "config.h"
#include "eth.h"
#include "mqtt.h"

static const char *TAG = "web";

#define PUMP_STACK  4096
#define PUMP_PRIO   4               /* below the ESP-NOW dispatch task */

/* Status heartbeat. Also the interval at which a stalled send is retried. */
#define PUMP_TICK_MS 1000

/*
 * One websocket text frame. The largest is a message frame: a 250-byte
 * ESP-NOW payload where every byte escapes to \uXXXX, plus the fixed fields.
 */
#define JSON_MAX    (6 * ESP_NOW_MAX_DATA_LEN + 256)

/* The embedded status page (EMBED_TXTFILES in CMakeLists.txt). */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

typedef struct {
    uint32_t seq;                   /* 1-based, never reused */
    uint32_t ms;                    /* device uptime at receive, milliseconds */
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t len;
    uint8_t  data[ESP_NOW_MAX_DATA_LEN];
} entry_t;

/*
 * History ring. Entry @seq lives at s_hist[(seq - 1) % CONFIG_WEB_HISTORY];
 * s_seq is the sequence number of the newest entry, so the ring holds
 * (s_seq - CONFIG_WEB_HISTORY, s_seq]. Written by the ESP-NOW dispatch task,
 * read by the pump task, both under s_lock -- held for a memcpy and nothing
 * else, so the dispatch task is never parked behind network I/O.
 */
static entry_t          s_hist[CONFIG_WEB_HISTORY];
static uint32_t         s_seq;
static SemaphoreHandle_t s_lock;

/* Connected browsers, by socket descriptor. -1 is a free slot. */
static int  s_fd[CONFIG_WEB_WS_CLIENTS];
static bool s_snapshot[CONFIG_WEB_WS_CLIENTS];   /* still owes a full replay */

static httpd_handle_t s_server;
static TaskHandle_t   s_pump;

/* ==========================================================================
 * JSON
 * ========================================================================== */

/*
 * Append @len bytes as the body of a JSON string. ESP-NOW payloads are
 * arbitrary bytes, so anything outside printable ASCII becomes \uXXXX -- the
 * frame stays parseable whatever a sender puts on the air. Returns the
 * characters written, or -1 if the result did not fit.
 */
static int json_escape(char *out, size_t cap, const uint8_t *data, size_t len)
{
    size_t n = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        const char *lit = NULL;

        if (c == '"') {
            lit = "\\\"";
        } else if (c == '\\') {
            lit = "\\\\";
        }

        if (lit) {
            if (n + 2 >= cap) {
                return -1;
            }
            out[n++] = lit[0];
            out[n++] = lit[1];
        } else if (c >= 0x20 && c < 0x7f) {
            if (n + 1 >= cap) {
                return -1;
            }
            out[n++] = (char)c;
        } else {
            if (n + 6 >= cap) {
                return -1;
            }
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        }
    }

    out[n] = '\0';
    return (int)n;
}

static int fmt_message(char *out, size_t cap, const entry_t *e)
{
    int n = snprintf(out, cap,
                     "{\"t\":\"m\",\"seq\":%lu,\"ms\":%lu,"
                     "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                     "\"rssi\":%d,\"len\":%u,\"pl\":\"",
                     (unsigned long)e->seq, (unsigned long)e->ms,
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                     e->rssi, (unsigned)e->len);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }

    int esc = json_escape(out + n, cap - (size_t)n, e->data, e->len);
    if (esc < 0) {
        return -1;
    }
    n += esc;

    int tail = snprintf(out + n, cap - (size_t)n, "\"}");
    return (tail > 0 && (size_t)(n + tail) < cap) ? n + tail : -1;
}

static int fmt_status(char *out, size_t cap)
{
    char ip[16];
    bool dhcp = false;
    eth_ip_str(ip, sizeof(ip), &dhcp);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    int n = snprintf(out, cap,
                     "{\"t\":\"s\",\"host\":\"%s\",\"ip\":\"%s\",\"dhcp\":%s,"
                     "\"mqtt\":%s,\"broker\":\"" CONFIG_MQTT_URI "\","
                     "\"rx\":%lu,\"drop\":%lu,\"ch\":%d,\"lr\":%s,"
                     "\"up\":%lu,\"hist\":%d,\"ver\":\"%s\",\"slot\":\"%s\"}",
                     app_hostname(), ip, dhcp ? "true" : "false",
                     mqtt_is_connected() ? "true" : "false",
                     (unsigned long)espnow_received(), (unsigned long)espnow_dropped(),
                     CONFIG_ESPNOW_CHANNEL,
#if defined(CONFIG_ESPNOW_LR) && CONFIG_ESPNOW_LR
                     "true",
#else
                     "false",
#endif
                     (unsigned long)(esp_timer_get_time() / 1000),
                     CONFIG_WEB_HISTORY, app->version, running->label);

    return (n > 0 && (size_t)n < cap) ? n : -1;
}

/* ==========================================================================
 * Client list
 * ========================================================================== */

static void client_add(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
        if (s_fd[i] == fd) {
            slot = i;                       /* reconnect on a recycled fd */
            break;
        }
        if (s_fd[i] < 0 && slot < 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        s_fd[slot] = fd;
        s_snapshot[slot] = true;
    }

    xSemaphoreGive(s_lock);

    if (slot < 0) {
        ESP_LOGW(TAG, "ws client limit reached, fd %d not tracked", fd);
    } else {
        ESP_LOGI(TAG, "ws client on fd %d", fd);
    }
    xTaskNotifyGive(s_pump);
}

static void client_drop(int fd)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
        if (s_fd[i] == fd) {
            s_fd[i] = -1;
            s_snapshot[i] = false;
        }
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "ws client on fd %d closed", fd);
}

/*
 * Send one text frame. Async because the pump task is not an httpd task; the
 * fd is re-checked first so a descriptor closed (and possibly reused) since we
 * copied the client list cannot be written to as if it were still a socket.
 */
static bool ws_send(int fd, const char *json, size_t len)
{
    if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        return false;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = len,
    };
    return httpd_ws_send_frame_async(s_server, fd, &frame) == ESP_OK;
}

/* ==========================================================================
 * Broadcast
 * ========================================================================== */

/*
 * Owns the only JSON buffer and the only socket writes, so nothing here is
 * on the ESP-NOW path. Wakes on a new frame, and on a tick to keep the status
 * card live.
 */
static void pump_task(void *arg)
{
    static char json[JSON_MAX];         /* static: too large for a task stack */
    uint32_t sent = 0;                  /* highest sequence already broadcast */

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PUMP_TICK_MS));

        if (!s_server) {
            sent = s_seq;               /* offline: nothing to catch up on */
            continue;
        }

        int  fds[CONFIG_WEB_WS_CLIENTS];
        bool snap[CONFIG_WEB_WS_CLIENTS];
        uint32_t newest;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(fds, s_fd, sizeof(fds));
        memcpy(snap, s_snapshot, sizeof(snap));
        memset(s_snapshot, 0, sizeof(s_snapshot));
        newest = s_seq;
        xSemaphoreGive(s_lock);

        bool any = false;
        for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
            any = any || fds[i] >= 0;
        }
        if (!any) {
            sent = newest;
            continue;
        }

        /* Status first: it carries the history depth the page sizes itself to. */
        int len = fmt_status(json, sizeof(json));
        if (len > 0) {
            for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
                if (fds[i] >= 0 && !ws_send(fds[i], json, (size_t)len)) {
                    client_drop(fds[i]);
                    fds[i] = -1;
                }
            }
        }

        /*
         * A client that just connected gets the whole ring; everyone else gets
         * only what is new. Frames older than the ring are gone -- the page
         * shows a gap rather than the dispatch task waiting on a slow socket.
         */
        uint32_t oldest = newest > CONFIG_WEB_HISTORY ? newest - CONFIG_WEB_HISTORY + 1 : 1;
        if (sent < oldest - 1) {
            sent = oldest - 1;
        }

        for (uint32_t seq = oldest; seq <= newest; seq++) {
            bool wanted = false;
            for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
                wanted = wanted || (fds[i] >= 0 && (snap[i] || seq > sent));
            }
            if (!wanted) {              /* nobody is owed it -- do not format it */
                continue;
            }

            entry_t e;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            e = s_hist[(seq - 1) % CONFIG_WEB_HISTORY];
            xSemaphoreGive(s_lock);

            if (e.seq != seq) {
                continue;               /* overwritten while we were sending */
            }

            len = fmt_message(json, sizeof(json), &e);
            if (len < 0) {
                continue;
            }

            for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
                if (fds[i] < 0 || (!snap[i] && seq <= sent)) {
                    continue;
                }
                if (!ws_send(fds[i], json, (size_t)len)) {
                    client_drop(fds[i]);
                    fds[i] = -1;
                }
            }
        }
        sent = newest;
    }
}

void web_on_message(const espnow_msg_t *msg)
{
    if (!s_pump) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    entry_t *e = &s_hist[s_seq % CONFIG_WEB_HISTORY];
    e->seq = ++s_seq;
    e->ms = msg->ms;
    memcpy(e->mac, msg->mac, sizeof(e->mac));
    e->rssi = msg->rssi;
    e->len = msg->len;
    memcpy(e->data, msg->data, msg->len);
    xSemaphoreGive(s_lock);

    xTaskNotifyGive(s_pump);
}

/* ==========================================================================
 * Handlers
 * ========================================================================== */

static esp_err_t root_get(httpd_req_t *req)
{
    /* EMBED_TXTFILES appends a NUL that is not part of the document. */
    const size_t len = (size_t)(index_html_end - index_html_start) - 1;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t ws_get(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake. The reply is sent by the server; the snapshot is left to
         * the pump task so every send stays on one task with one buffer. */
        client_add(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    /* The page never sends anything, but a frame that does arrive has to be
     * drained or the rest of the stream is misread as a header. */
    uint8_t scratch[64];
    httpd_ws_frame_t frame = { .payload = scratch };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, sizeof(scratch));
    if (err != ESP_OK) {
        client_drop(httpd_req_to_sockfd(req));
    }
    return err;
}

esp_err_t web_attach(httpd_handle_t server)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
        for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
            s_fd[i] = -1;
        }
    }

    /* Before the handlers: a client can arrive the instant they are
     * registered, and connecting one notifies the pump task. */
    if (!s_pump) {
        ESP_RETURN_ON_FALSE(
            xTaskCreate(pump_task, "web_pump", PUMP_STACK, NULL, PUMP_PRIO, &s_pump) == pdPASS,
            ESP_ERR_NO_MEM, TAG, "task create");
    }

    s_server = server;

    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get,
    };
    static const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_get,
        .is_websocket = true,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root_uri), TAG, "root");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &ws_uri), TAG, "ws");
    return ESP_OK;
}

void web_detach(void)
{
    /* Clear the handle before the caller stops the server: the pump task
     * dereferences it from outside any httpd task. */
    s_server = NULL;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < CONFIG_WEB_WS_CLIENTS; i++) {
            s_fd[i] = -1;
            s_snapshot[i] = false;
        }
        xSemaphoreGive(s_lock);
    }
}
