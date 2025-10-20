/*
 * wifi.c
 *
 * Minimal, well-documented wrapper around ESP-IDF WiFi helpers used by the
 * project. Provides helpers for initializing networking, starting an AP and
 * connecting as a station. The implementations keep behaviour small and
 * predictable to make testing easier.
 */

#include "wifi.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* Event bits used to communicate station state to callers. */
#define STATION_CONNECTED_BIT BIT0
#define STATION_FAIL_BIT BIT1

/* Module log tag */
static const char *TAG = "wifi_manager";

/*
 * Event group used by set_station(). It's created on demand and reused across
 * calls to avoid leaking a handle when set_station() is called multiple times.
 */
static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_retry_num = 0;

/**
 * Initialize networking infra used by this module.
 * This is safe to call multiple times (esp_netif_init is idempotent in IDS).
 */
void init_wifi_module(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
}

/**
 * Start a soft-AP with the provided SSID/password and channel.
 * This is a thin wrapper; it performs minimal validation on inputs.
 */
void set_ap(const char *ssid, const char *password, const int channel)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "set_ap called with empty SSID; aborting");
        return;
    }

    /* Clamp channel to valid 1..14 range to avoid invalid configs */
    int ch = channel;
    if (ch < 1) ch = 1;
    if (ch > 14) ch = 14;

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = (uint8_t)strlen(ssid),
            .channel = ch,
            .max_connection = 16,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };

    /* Copy and ensure NUL-termination */
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';
    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';

    if (password == NULL || strlen(password) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             ssid, password ? password : "", ch);
}

/**
 * Internal event handler for station events. Registered by set_station().
 */
static void station_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < 5)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (attempt %d)", s_retry_num);
        }
        else
        {
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, STATION_FAIL_BIT);
            }
        }
        ESP_LOGI(TAG, "connect to the AP failed");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, STATION_CONNECTED_BIT);
        }
    }
}

/**
 * Configure and connect as a WiFi station. Blocks until connected or failed.
 * Returns true on success, false on failure.
 */
bool set_station(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGW(TAG, "set_station called with empty SSID");
        return false;
    }

    /* Create event group once and reuse it across calls */
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "failed to create event group");
            return false;
        }
    } else {
        /* Clear previous bits and retry counter when reusing */
        xEventGroupClearBits(s_wifi_event_group, STATION_CONNECTED_BIT | STATION_FAIL_BIT);
        s_retry_num = 0;
    }

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &station_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &station_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = { 0 };
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    strncpy((char *)wifi_config.sta.password, password ? password : "", sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "set_station finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           STATION_CONNECTED_BIT | STATION_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);
    if (bits & STATION_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        return true;
    }

    if (bits & STATION_FAIL_BIT)
    {
        ESP_LOGI(TAG, "FAILED to connect to ap SSID:%s", ssid);
        return false;
    }

    return false;
}