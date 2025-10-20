#include "ota_manager.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "nvs_flash.h"
#include <cJSON.h>

// small helper to read a file from mounted filesystem into a newly allocated buffer
static char *read_file_to_buffer(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long s = ftell(f);
    if (s < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)s + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)s, f);
    buf[r] = '\0';
    fclose(f);
    return buf;
}

static const char *TAG = "ota_manager";

static int s_poll_minutes = 5; // default poll interval in minutes

// optional MQTT telemetry publish function (implemented in mqtt_manager)
extern void mqtt_publish_telemetry(const char *json_payload);

int ota_manager_get_poll_minutes(void) { return s_poll_minutes; }

void ota_manager_init(const char *manifest_url)
{
    ESP_LOGI(TAG, "ota_manager_init called (manifest_url=%s)", manifest_url ? manifest_url : "(none)");
    // For now we don't persist manifest_url; future work: read /filesystem/ota_config.json
}

// Poll task state
static TaskHandle_t s_poller_task = NULL;

static void ota_poller_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "OTA poller task started");
    while (1) {
        int minutes = ota_manager_get_poll_minutes();
        if (minutes <= 0) minutes = 5;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)minutes * 60u * 1000u));
        ESP_LOGI(TAG, "OTA poller waking to check for updates (interval=%d minutes)", minutes);
        ota_manager_check_and_update();
    }
}

void ota_manager_start_poller(void)
{
    if (s_poller_task) {
        ESP_LOGW(TAG, "OTA poller already running");
        return;
    }
    BaseType_t r = xTaskCreate(ota_poller_task, "ota_poller", 4096, NULL, tskIDLE_PRIORITY + 1, &s_poller_task);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ota poller task");
        s_poller_task = NULL;
    }
}

void ota_manager_stop_poller(void)
{
    if (!s_poller_task) return;
    vTaskDelete(s_poller_task);
    s_poller_task = NULL;
    ESP_LOGI(TAG, "OTA poller stopped");
}

bool ota_manager_check_and_update(void)
{
    ESP_LOGI(TAG, "Checking for OTA updates...");

    // Look for a manifest URL in /filesystem/ota_config.json or default to a hardcoded URL
    const char *default_manifest = "https://raw.githubusercontent.com/leonidasdev/firmware-repo/main/manifest.json";
    char *cfg = read_file_to_buffer("/filesystem/ota.cfg");
    const char *manifest_url = default_manifest;
    if (cfg) {
        cJSON *json = cJSON_Parse(cfg);
        if (json) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(json, "manifest_url");
            if (cJSON_IsString(m) && (m->valuestring != NULL)) {
                manifest_url = strdup(m->valuestring);
            }
            cJSON_Delete(json);
        }
        free(cfg);
    }

    ESP_LOGI(TAG, "Fetching manifest: %s", manifest_url);
    esp_http_client_config_t http_cfg = {
        .url = manifest_url,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init http client for manifest");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch manifest: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    int content_length = esp_http_client_get_content_length(client);
    char *body = malloc((size_t)content_length + 1);
    if (!body) { esp_http_client_cleanup(client); return false; }
    int read_len = esp_http_client_read_response(client, body, content_length + 1);
    if (read_len <= 0) {
        ESP_LOGE(TAG, "Empty manifest response");
        free(body);
        esp_http_client_cleanup(client);
        return false;
    }
    body[read_len] = '\0';
    esp_http_client_cleanup(client);

    cJSON *manifest = cJSON_Parse(body);
    free(body);
    if (!manifest) {
        ESP_LOGE(TAG, "Invalid JSON manifest");
        return false;
    }

    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(manifest, "url");
    cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(manifest, "version");
    cJSON *sha_item = cJSON_GetObjectItemCaseSensitive(manifest, "sha256");
    if (!cJSON_IsString(url_item) || !cJSON_IsString(ver_item)) {
        ESP_LOGE(TAG, "Manifest missing url or version");
        cJSON_Delete(manifest);
        return false;
    }

    const char *remote_ver = ver_item->valuestring;
    const char *bin_url = url_item->valuestring;
    const char *expected_sha = cJSON_IsString(sha_item) ? sha_item->valuestring : NULL;

    // Compare with local last version stored in NVS
    esp_err_t nerr;
    nvs_handle_t h;
    nerr = nvs_open("ota", NVS_READWRITE, &h);
    char last_version[64] = {0};
    if (nerr == ESP_OK) {
        size_t sz = sizeof(last_version);
        nvs_get_str(h, "version", last_version, &sz);
    }

    if (last_version[0] != '\0' && strcmp(last_version, remote_ver) == 0) {
        ESP_LOGI(TAG, "Device already at version %s; nothing to do", remote_ver);
        if (nerr == ESP_OK) nvs_close(h);
        cJSON_Delete(manifest);
        return false;
    }

    ESP_LOGI(TAG, "New firmware available: %s -> %s", last_version[0] ? last_version : "(none)", remote_ver);

    esp_http_client_config_t ota_cfg = {
        .url = bin_url,
        // In production provide .cert_pem or ensure CA present in esp_crt_bundle
    };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA applied successfully, saving version and restarting");
        if (nerr == ESP_OK) {
            nvs_set_str(h, "version", remote_ver);
            nvs_commit(h);
            nvs_close(h);
        }
        cJSON_Delete(manifest);
        // Restart to boot into the newly flashed partition
        esp_restart();
        return true; // usually not reached
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        if (nerr == ESP_OK) nvs_close(h);
        cJSON_Delete(manifest);
        return false;
    }
}

void ota_manager_set_schedule(int hour, int minute)
{
    ESP_LOGI(TAG, "Schedule set to %02d:%02d (not yet implemented)", hour, minute);
}

void ota_manager_enable_on_boot(bool enable)
{
    ESP_LOGI(TAG, "on_boot OTA enabled=%d (not yet implemented)", enable);
}

void ota_manager_report_status(const char *status, const char *detail)
{
    ESP_LOGI(TAG, "OTA status: %s - %s", status, detail ? detail : "");
}

void ota_manager_handle_attribute_update(const char *json_payload)
{
    if (!json_payload) return;
    ESP_LOGI(TAG, "ota attribute update: %s", json_payload);
    cJSON *root = cJSON_Parse(json_payload);
    if (!root) {
        ESP_LOGE(TAG, "Invalid OTA attribute JSON");
        return;
    }

    // If ota_manifest_url present, write to /filesystem/ota.cfg and trigger check
    cJSON *murl = cJSON_GetObjectItemCaseSensitive(root, "ota_manifest_url");
        if (cJSON_IsString(murl) && murl->valuestring) {
        ESP_LOGI(TAG, "Received ota_manifest_url: %s", murl->valuestring);
        // write minimal ota.cfg with manifest_url
        FILE *f = fopen("/filesystem/ota.cfg", "w");
        if (f) {
            fprintf(f, "{\"manifest_url\":\"%s\",\"on_boot\":false}\n", murl->valuestring);
            fclose(f);
                ota_manager_report_status("check_started", murl->valuestring);
                ota_manager_check_and_update();
        } else {
            ESP_LOGE(TAG, "Failed to open /filesystem/ota.cfg for writing");
        }
    }

    // If ota_command present, treat as immediate instruction
    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "ota_command");
    if (cmd && cJSON_IsObject(cmd)) {
        cJSON *url = cJSON_GetObjectItemCaseSensitive(cmd, "url");
        cJSON *version = cJSON_GetObjectItemCaseSensitive(cmd, "version");
        cJSON *sha = cJSON_GetObjectItemCaseSensitive(cmd, "sha256");
        if (cJSON_IsString(url) && url->valuestring) {
            // create a temporary ota.cfg entry with url as direct binary
            FILE *f = fopen("/filesystem/ota.cfg", "w");
            if (f) {
                fprintf(f, "{\"manifest_url\":\"%s\",\"on_boot\":false}\n", url->valuestring);
                fclose(f);
                ota_manager_report_status("download_requested", url->valuestring);
                ota_manager_check_and_update();
            } else {
                ESP_LOGE(TAG, "Failed to open /filesystem/ota.cfg for writing (cmd)");
            }
        } else {
            ESP_LOGW(TAG, "ota_command missing 'url'");
        }
    }

    /* Optional: allow remote attribute to adjust poll interval
       Attribute name in ThingsBoard: ota.poll_minutes (or ota_poll_minutes when delivered in JSON)
    */
    cJSON *poll = cJSON_GetObjectItemCaseSensitive(root, "ota_poll_minutes");
    if (cJSON_IsNumber(poll)) {
        int val = poll->valueint;
        if (val > 0) {
            s_poll_minutes = val;
            ESP_LOGI(TAG, "Updated ota poll minutes to %d", s_poll_minutes);
        }
    }

    cJSON_Delete(root);
}
