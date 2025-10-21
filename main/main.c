/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_adc/adc_cali.h"

#include "persistence.h"
#include "webserver.h"
#include "wifi.h"
#include "adc_manager.h"
#include "mqtt.h"
#include "telegram.h"
#include "deepsleep_manager.h"
#include "hcsr04.h"
#include "ota_manager.h"
#if __has_include("esp_crt_bundle.h")
#include "esp_crt_bundle.h"
#define HAVE_ESP_CRT_BUNDLE 1
#else
#define HAVE_ESP_CRT_BUNDLE 0
#endif

/*
 * main.c
 *
 * Application entrypoint: mount the data partition, read configuration files,
 * bring up WiFi (station or AP+webserver), start MQTT/Telegram and sensor
 * sampling. The main loop publishes telemetry via MQTT and relies on
 * deepsleep_manager to handle sleeping.
 */

const static char *TAG = "HITO 5";

#define FILESYSTEM_ROOT "/filesystem"
#define FILESYSTEM_PARTITION "storage"
#define INDEX_FILE_PATH (FILESYSTEM_ROOT "/index.htm")
#define MQTT_CREDENTIALS_PATH (FILESYSTEM_ROOT "/mqtt.txt")
#define WIFI_CREDENTIALS_PATH (FILESYSTEM_ROOT "/wifi.txt")

#define AP_SSID "SBC25M02B"
#define AP_PASSWORD "password2B"
#define AP_CHANNEL 1

#define ADC_CHANNEL ADC_CHANNEL_4
#define ADC_ATTEN ADC_ATTEN_DB_12


void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    nvs_flash_init();
    fat32_mount(FILESYSTEM_ROOT, FILESYSTEM_PARTITION);

    // Log presence of common CA PEM filenames in the mounted filesystem so we
    // can quickly diagnose whether the data partition contains the expected
    // CA bundle that TLS needs.
    const char *pem_candidates[] = {FILESYSTEM_ROOT "/ca_root.pem", FILESYSTEM_ROOT "/ca-root.pem", FILESYSTEM_ROOT "/cacert.pem", NULL};
    for (const char **pp = pem_candidates; *pp; ++pp) {
        FILE *f = fopen(*pp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long s = ftell(f);
            fclose(f);
            ESP_LOGI(TAG, "Found CA PEM candidate: %s (bytes=%ld)", *pp, s);
        } else {
            ESP_LOGI(TAG, "CA PEM candidate not found: %s", *pp);
        }
    }
    // List directory contents to help debug which files are present on the data partition
    DIR *d = opendir(FILESYSTEM_ROOT);
    if (d) {
        struct dirent *ent;
        ESP_LOGI(TAG, "Listing %s:", FILESYSTEM_ROOT);
        while ((ent = readdir(d)) != NULL) {
            ESP_LOGI(TAG, "  %s", ent->d_name);
        }
        closedir(d);
    } else {
        ESP_LOGW(TAG, "Failed to open directory %s for listing", FILESYSTEM_ROOT);
    }

    // Read and log tele.txt (masked token + persisted id) to help debug Telegram issues
    {
        const char *tele_path = FILESYSTEM_ROOT "/tele.txt";
        FILE *tf = fopen(tele_path, "r");
        if (tf) {
            char line1[256] = "";
            char line2[256] = "";
            char line3[256] = "";
            if (fgets(line1, sizeof(line1), tf) == NULL) line1[0] = '\0';
            if (fgets(line2, sizeof(line2), tf) == NULL) line2[0] = '\0';
            if (fgets(line3, sizeof(line3), tf) == NULL) line3[0] = '\0';
            fclose(tf);
            // trim newlines
            line1[strcspn(line1, "\r\n")] = '\0';
            line2[strcspn(line2, "\r\n")] = '\0';
            line3[strcspn(line3, "\r\n")] = '\0';
            if (line1[0] != '\0') {
                size_t L = strlen(line1);
                char masked[64];
                if (L <= 12) {
                    int show = (int)L;
                    snprintf(masked, sizeof(masked), "<redacted:%.*s>", show, line1);
                } else {
                    // show first 6 + last 6
                    snprintf(masked, sizeof(masked), "%.*s...%.*s", 6, line1, 6, line1 + L - 6);
                }
                long long persisted = 0;
                if (line3[0] != '\0') sscanf(line3, "%lld", &persisted);
                ESP_LOGI(TAG, "Found %s (masked token: %s, persisted_last_update_id=%lld)", tele_path, masked, persisted);
            } else {
                ESP_LOGI(TAG, "%s exists but token line is empty", tele_path);
            }
        } else {
            ESP_LOGI(TAG, "%s not present on data partition", FILESYSTEM_ROOT "/tele.txt");
        }
    }

    // Check for a CA PEM directly on the mounted filesystem and, if found,
    // register it with the upstream esp_crt_bundle via esp_crt_bundle_set().
    const char *pem_candidates2[] = {FILESYSTEM_ROOT "/ca_root.pem", FILESYSTEM_ROOT "/ca-root.pem", FILESYSTEM_ROOT "/cacert.pem", NULL};
    bool pem_found = false;
    for (const char **pp = pem_candidates2; *pp; ++pp) {
        FILE *f = fopen(*pp, "rb");
        if (!f) continue;
        // find file size
        fseek(f, 0, SEEK_END);
        long s = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (s <= 0) { fclose(f); continue; }
        char *full = malloc((size_t)s + 1);
        if (!full) { fclose(f); break; }
        size_t r = fread(full, 1, (size_t)s, f);
        full[r] = '\0';
        fclose(f);
        ESP_LOGI(TAG, "Found PEM at %s (bytes=%d)", *pp, (int)r);
        #if defined(HAVE_ESP_CRT_BUNDLE)
            /* esp_crt_bundle_set expects 'const uint8_t *' (unsigned char pointer).
             * Our `full` buffer is a char*, so cast to avoid signedness warnings
             * which are treated as errors (-Werror=pointer-sign). */
            if (esp_crt_bundle_set((const uint8_t *)full, r) == ESP_OK) {
                ESP_LOGI(TAG, "Registered filesystem PEM with esp_crt_bundle");
            } else {
                ESP_LOGW(TAG, "Failed to register filesystem PEM with esp_crt_bundle");
            }
        #else
            ESP_LOGI(TAG, "esp_crt_bundle not available at compile time; skipping registration");
        #endif
        free(full);
        pem_found = true;
        break;
    }
    if (!pem_found) {
        ESP_LOGW(TAG, "No PEM file found under %s", FILESYSTEM_ROOT);
    }

    init_wifi_module();

    // OTA manager is attribute-driven; OTA initialization is handled when
    // MQTT is connected and attributes are retrieved.

    // Log partition table layout for OTA debugging
    ESP_LOGI(TAG, "Partition table layout (4MB flash):");
    ESP_LOGI(TAG, "  nvs      @ 0x9000   size 0x6000");
    ESP_LOGI(TAG, "  phy_init @ 0xf000   size 0x1000");
    ESP_LOGI(TAG, "  otadata  @ 0x10000  size 0x2000");
    ESP_LOGI(TAG, "  factory  @ 0x12000  size 0x100000");
    ESP_LOGI(TAG, "  ota_0    @ 0x112000 size 0x100000");
    ESP_LOGI(TAG, "  ota_1    @ 0x212000 size 0x100000");
    ESP_LOGI(TAG, "  storage  @ 0x312000 size 0xEE000");

    struct persistence_config wifi_network_config;
    if (!persistence_read_config(WIFI_CREDENTIALS_PATH, &wifi_network_config) ||
        !set_station(wifi_network_config.ssid, wifi_network_config.password))
    {
        persistence_config_free(&wifi_network_config);

        set_ap(AP_SSID, AP_PASSWORD, AP_CHANNEL);
            struct webserver_handle *webserver = webserver_start(INDEX_FILE_PATH, WIFI_CREDENTIALS_PATH);
            if (webserver == NULL) {
                ESP_LOGE(TAG, "Failed to start webserver; cannot continue in AP setup mode");
                return;
            }

            xEventGroupWaitBits(webserver->event_group,
                                WEBSERVER_POST_EVENT,
                                pdFALSE,
                                pdFALSE,
                                portMAX_DELAY);

        ESP_LOGI(TAG, "Configuration file updated, restarting...");
        vTaskDelay(pdMS_TO_TICKS(3000));

        webserver_stop(webserver);
        esp_restart();
    }
    persistence_config_free(&wifi_network_config);

    /* Start MQTT only after station is configured and connected */
    if (!mqtt_app_start_from_file("mqtt://demo.thingsboard.io", MQTT_CREDENTIALS_PATH)) {
        ESP_LOGW(TAG, "MQTT not started from file %s", MQTT_CREDENTIALS_PATH);
    }

    // initialize deepsleep manager (reads stored interval)
    deepsleep_manager_init(FILESYSTEM_ROOT);

    // Optional: start Telegram bot if token file present
    if (telegram_init_from_file(FILESYSTEM_ROOT "/tele.txt"))
    {
        // simple handler that replies 'Not a valid command' for anything not starting with '/'
        void tg_handler(int64_t chat_id, const char *text, void *user_ctx)
        {
            (void)user_ctx;
            if (!text) return;
            if (text[0] == '/')
            {
                // you can extend command handling here; for now reply unknown command
                telegram_send_message(chat_id, "Unknown command");
            }
            else
            {
                telegram_send_message(chat_id, "Not a valid command");
            }
        }
        telegram_register_message_handler(tg_handler, NULL);
        telegram_start();
    }

    // Initialize ADC for LDR readings
    adc_manager_handle_t *adc_handle = adc_manager_init(ADC_CHANNEL, ADC_ATTEN);
    if (adc_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize ADC");
        return;
    }

    // Initialize HC-SR04 sensor: trigger GPIO4, echo GPIO5 per user request
    if (!hcsr04_init(4, 5)) {
        ESP_LOGW(TAG, "HC-SR04 initialization failed; distance will be unavailable");
    }

    int adc_raw, voltage;

    while (1)
    {
        if (adc_manager_read_raw(adc_handle, &adc_raw) == ESP_OK)
        {
            ESP_LOGI(TAG, "ADC Raw Data: %d", adc_raw);

            if (adc_manager_read_voltage(adc_handle, &voltage) == ESP_OK)
            {
                int resistance = adc_manager_calc_ohm(adc_raw);
                ESP_LOGI(TAG, "Voltage: %d mV, Resistance: %.3f kOhm", voltage, resistance / 1000.0);

                // read HC-SR04 distance (optional)
                uint32_t distance_mm = 0;
                bool have_distance = hcsr04_read_mm(&distance_mm);

                // publish telemetry JSON to ThingsBoard
                char payload[192];
                int len;
                if (have_distance) {
                    /* cast to unsigned long for portability with printf format */
                    len = snprintf(payload, sizeof(payload), "{\"voltage_mV\":%d,\"ohms\":%d,\"distance_mm\":%lu}", voltage, resistance, (unsigned long)distance_mm);
                } else {
                    len = snprintf(payload, sizeof(payload), "{\"voltage_mV\":%d,\"ohms\":%d}", voltage, resistance);
                }
                    if (len > 0 && len < sizeof(payload))
                    {
                        mqtt_publish_telemetry(payload);
                        // after publishing, do not immediately enter deep sleep here.
                        // Deep-sleep will be triggered by the idle countdown started
                        // after the Telegram initial sync, or by an explicit /deepsleep
                        // command which uses deepsleep_manager_force_sleep().
                    }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // Clean up (not reached in this case)
    adc_manager_deinit(adc_handle);
}
