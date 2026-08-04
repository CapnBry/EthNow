/*
 * info.h -- device and build detail page.
 *
 * GET /info       the page itself, embedded in the image like the status page
 * GET /info.json  everything the page shows, as one flat JSON object
 *
 * Separate from the status page on purpose: almost nothing here changes while
 * the device is running, so it is fetched on demand rather than pushed, and
 * none of it belongs in the once-a-second websocket frame.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * Add the two handlers to an already-started server. Called by web_attach(),
 * and like it, safe to call again after the server has been restarted.
 */
esp_err_t info_attach(httpd_handle_t server);
