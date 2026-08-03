file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    # The status page is served straight out of flash -- no filesystem, and
    # nothing to keep in sync with the firmware image.
    EMBED_TXTFILES "index.html"
    REQUIRES
        nvs_flash
        esp_event
        esp_netif
        esp_eth
        esp_wifi
        esp_timer
        esp_http_server
        app_update
        driver
        mqtt
)
