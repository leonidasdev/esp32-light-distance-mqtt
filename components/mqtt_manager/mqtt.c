/*
 * mqtt.c
 *
 * Thin wrapper around ESP-IDF's MQTT client to connect to a ThingsBoard
 * instance and publish telemetry. The module keeps a single global client
 * handle and exposes a small API used by the rest of the application.
 */
#include "mqtt.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t client = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

void mqtt_app_stop(void)
{
    if (client)
    {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = NULL;
        ESP_LOGI(TAG, "mqtt client stopped");
    }
}

void mqtt_app_start(const char *uri, const char *access_token)
{
    if (client)
    {
        ESP_LOGW(TAG, "mqtt client already running");
        return;
    }

    if (uri == NULL || access_token == NULL) {
        ESP_LOGE(TAG, "mqtt_app_start called with NULL uri or access_token");
        return;
    }

    esp_mqtt_client_config_t cfg = {0};
    /* populate nested fields according to esp-mqtt layout in ESP-IDF v5.x */
    cfg.broker.address.uri = uri;
    cfg.credentials.username = access_token;
    cfg.session.keepalive = 60;

    client = esp_mqtt_client_init(&cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "failed to init mqtt client");
        return;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to start mqtt client: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "mqtt client started (uri=%s)", uri);
    }
}

bool mqtt_app_start_from_file(const char *uri, const char *token_file_path)
{
    if (uri == NULL || token_file_path == NULL) return false;

    FILE *f = fopen(token_file_path, "r");
    if (!f)
    {
        ESP_LOGW(TAG, "token file not found: %s", token_file_path);
        return false;
    }

    char token[128] = {0};
    if (fgets(token, sizeof(token), f) == NULL)
    {
        fclose(f);
        ESP_LOGW(TAG, "empty token file: %s", token_file_path);
        return false;
    }
    fclose(f);

    /* strip newline */
    size_t len = strlen(token);
    while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r'))
    {
        token[len - 1] = '\0';
        len--;
    }

    if (len == 0)
    {
        ESP_LOGW(TAG, "token file contained only whitespace: %s", token_file_path);
        return false;
    }

    mqtt_app_start(uri, token);
    return true;
}

void mqtt_publish_telemetry(const char *json_payload)
{
    if (!client)
    {
        ESP_LOGW(TAG, "cannot publish, mqtt client not started");
        return;
    }
    if (!json_payload) {
        ESP_LOGW(TAG, "mqtt_publish_telemetry called with NULL payload");
        return;
    }

    const char *topic = "v1/devices/me/telemetry";
    int msg_id = esp_mqtt_client_publish(client, topic, json_payload, 0, 1, 0);
    ESP_LOGI(TAG, "published telemetry (msg_id=%d): %s", msg_id, json_payload);
}
