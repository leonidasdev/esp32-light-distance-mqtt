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
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long s = ftell(f);
    if (s < 0)
    {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)s + 1);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
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


// FOTA (ThingsBoard only)
// This function expects all required FOTA metadata to be passed in as a cJSON object.
// It is called from ota_manager_handle_attribute_update().
bool ota_manager_apply_fota_from_attributes(cJSON *root)
{
    // Required fields: fw_title, fw_version, fw_size, fw_checksum, fw_checksum_algorithm, fw_url
    cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "fw_title");
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "fw_version");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "fw_size");
    cJSON *checksum = cJSON_GetObjectItemCaseSensitive(root, "fw_checksum");
    cJSON *algo = cJSON_GetObjectItemCaseSensitive(root, "fw_checksum_algorithm");
    cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "fw_url");
    if (!cJSON_IsString(title) || !cJSON_IsString(version) || !cJSON_IsNumber(size) ||
        !cJSON_IsString(checksum) || !cJSON_IsString(algo) || !cJSON_IsString(url))
    {
        ESP_LOGE(TAG, "FOTA attribute missing required fields");
        return false;
    }

    // Compare with local last version stored in NVS
    esp_err_t nerr;
    nvs_handle_t h;
    nerr = nvs_open("ota", NVS_READWRITE, &h);
    char last_version[64] = {0};
    if (nerr == ESP_OK)
    {
        size_t sz = sizeof(last_version);
        nvs_get_str(h, "version", last_version, &sz);
    }
    if (last_version[0] != '\0' && strcmp(last_version, version->valuestring) == 0)
    {
        ESP_LOGI(TAG, "Device already at version %s; nothing to do", version->valuestring);
        if (nerr == ESP_OK)
            nvs_close(h);
        return false;
    }

    ESP_LOGI(TAG, "New firmware available: %s -> %s", last_version[0] ? last_version : "(none)", version->valuestring);
    ota_manager_report_status("download_start", url->valuestring);

    // Download firmware to flash using OTA API
    esp_http_client_config_t ota_http_cfg = {
        .url = url->valuestring,
        .use_global_ca_store = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &ota_http_cfg,
    };
    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "OTA applied successfully, saving version and restarting");
        if (nerr == ESP_OK)
        {
            nvs_set_str(h, "version", version->valuestring);
            nvs_commit(h);
            nvs_close(h);
        }
        ota_manager_report_status("update_success", version->valuestring);
        esp_restart();
        return true; // usually not reached
    }
    else
    {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
        if (nerr == ESP_OK)
            nvs_close(h);
        ota_manager_report_status("update_failed", esp_err_to_name(ret));
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
    if (!json_payload)
        return;
    ESP_LOGI(TAG, "ota attribute update: %s", json_payload);
    cJSON *root = cJSON_Parse(json_payload);
    if (!root)
    {
        ESP_LOGE(TAG, "Invalid OTA attribute JSON");
        return;
    }
    // ThingsBoard attribute responses can be shaped several ways:
    //  - plain attributes object: {"fw_version":...}
    //  - wrapped response: {"clientToken":"..","data":{...}}
    //  - shared attributes: {"shared":{...}}
    // Prefer 'data' if present, then 'shared', otherwise use the root object.
    cJSON *payload = root;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        payload = data;
        ESP_LOGI(TAG, "Using 'data' object as payload for OTA attributes");
    } else {
        cJSON *shared = cJSON_GetObjectItemCaseSensitive(root, "shared");
        if (cJSON_IsObject(shared)) {
            payload = shared;
            ESP_LOGI(TAG, "Using 'shared' object as payload for OTA attributes");
        } else {
            ESP_LOGI(TAG, "Using top-level object as payload for OTA attributes");
        }
    }

    // If all required FOTA fields are present in the payload, trigger OTA
    if (cJSON_HasObjectItem(payload, "fw_title") && cJSON_HasObjectItem(payload, "fw_version") &&
        cJSON_HasObjectItem(payload, "fw_size") && cJSON_HasObjectItem(payload, "fw_checksum") &&
        cJSON_HasObjectItem(payload, "fw_checksum_algorithm") && cJSON_HasObjectItem(payload, "fw_url"))
    {
        ota_manager_apply_fota_from_attributes(payload);
    }
    else
    {
        ESP_LOGW(TAG, "OTA attribute update missing required FOTA fields; ignoring");
    }
    cJSON_Delete(root);
}
