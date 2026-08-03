/*
 * web.h -- status page and its websocket feed.
 *
 * GET /    the page itself, one self-contained document embedded in the image
 * GET /ws  a websocket that pushes a status snapshot and every received
 *          ESP-NOW frame as JSON text
 *
 * The last CONFIG_WEB_HISTORY frames are kept in RAM so a browser that
 * connects late still sees recent traffic. Everything is unauthenticated,
 * same as the OTA endpoint it shares a server with.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include "espnow.h"

/*
 * Add the handlers to an already-started server, and start the broadcast task
 * on first use. Undone by web_detach(), which must be called before the
 * server is stopped -- the handle is retained for asynchronous sends.
 */
esp_err_t web_attach(httpd_handle_t server);
void web_detach(void);

/*
 * Record a received frame and wake the broadcast task.
 *
 * Called from the ESP-NOW dispatch task and deliberately does no network I/O:
 * it copies the frame into the history ring and returns, so a slow or stalled
 * websocket client cannot back up the receive queue. If the browser falls
 * behind by more than the ring, it simply misses frames.
 */
void web_on_message(const espnow_msg_t *msg);
