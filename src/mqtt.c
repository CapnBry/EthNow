#include "mqtt.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "mqtt_client.h"

#include "app_id.h"
#include "config.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static atomic_bool              s_connected;
static bool                     s_started;

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        atomic_store(&s_connected, true);
        ESP_LOGI(TAG, "connected to " CONFIG_MQTT_URI);
        break;

    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s_connected, false);
        ESP_LOGW(TAG, "disconnected");
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "transport error, errno %d", event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "error type %d", event->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

esp_err_t mqtt_init(void)
{
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MQTT_URI,
        .credentials.client_id = app_hostname(),
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "client init failed");
        return ESP_FAIL;
    }

    return esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
}

void mqtt_set_online(bool online)
{
    if (!s_client || online == s_started) {
        return;
    }

    if (online) {
        if (esp_mqtt_client_start(s_client) == ESP_OK) {
            s_started = true;
        }
    } else {
        esp_mqtt_client_stop(s_client);
        atomic_store(&s_connected, false);
        s_started = false;
    }
}

bool mqtt_is_connected(void)
{
    return atomic_load(&s_connected);
}

esp_err_t mqtt_publish(const char *topic, const void *data, size_t len)
{
    if (!mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_client, topic, (const char *)data, (int)len,
                                         CONFIG_MQTT_QOS, CONFIG_MQTT_RETAIN);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "publish to %s failed", topic);
        return ESP_FAIL;
    }
    return ESP_OK;
}
