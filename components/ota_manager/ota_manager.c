#include "ota_manager.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "nvs_flash.h"
#include <cJSON.h>
#include <stddef.h>
// Forward-declare the minimal MQTT manager symbol we need here to avoid
// relying on component include paths from other components.
extern const char *mqtt_get_access_token(void);
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "mbedtls/md.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>

static const char *TAG = "ota_manager";
// Forward declaration so retry callback can call it before the definition appears
static bool ota_manager_thingsboard_preflight(const char *tb_base_url, const char *title, const char *version);

// Pending OTA request state (simple single-slot queue)
typedef struct {
    bool present;
    char tb_base_url[128];
    char title[64];
    char version[64];
    char checksum[128];
    char algo[32];
} pending_ota_t;

static pending_ota_t s_pending = { 0 };

// Retry timer to attempt preflight again when it failed previously
static TimerHandle_t s_retry_timer = NULL;
static TaskHandle_t s_ota_task = NULL;
static void ota_retry_timer_cb(TimerHandle_t xTimer);
static void ota_retry_task(void *pvParameters);

// Helper to schedule retry in seconds (creates timer lazily)
static void schedule_ota_retry(int seconds)
{
    if (!s_pending.present) return;
    if (!s_retry_timer) {
        s_retry_timer = xTimerCreate("ota_retry", pdMS_TO_TICKS(seconds * 1000), pdFALSE, NULL, ota_retry_timer_cb);
        if (!s_retry_timer) {
            ESP_LOGW(TAG, "Failed to create OTA retry timer");
            return;
        }
    }
    xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(seconds * 1000), 0);
    xTimerStart(s_retry_timer, 0);
}

static void ota_retry_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "OTA retry timer fired; notifying ota_retry_task");
    if (s_ota_task) {
        xTaskNotifyGive(s_ota_task);
    } else {
        // If task not created yet, create it now
        BaseType_t rc = xTaskCreate(ota_retry_task, "ota_retry", 6*1024, NULL, tskIDLE_PRIORITY+3, &s_ota_task);
        if (rc == pdPASS) {
            xTaskNotifyGive(s_ota_task);
        } else {
            ESP_LOGW(TAG, "Failed to create ota_retry_task");
        }
    }
}


static void ota_retry_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ota_retry_task started");
    for (;;) {
        // Wait for notification from timer or external notify
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_pending.present) continue;
        ESP_LOGI(TAG, "ota_retry_task: running preflight for %s@%s", s_pending.title, s_pending.version);
        if (ota_manager_thingsboard_preflight(s_pending.tb_base_url, s_pending.title, s_pending.version)) {
            ESP_LOGI(TAG, "Preflight succeeded in ota_retry_task; starting OTA");
            ota_manager_download_and_apply_by_title(s_pending.tb_base_url, s_pending.title, s_pending.version, s_pending.checksum[0] ? s_pending.checksum : NULL, s_pending.algo[0] ? s_pending.algo : NULL);
            s_pending.present = false;
            if (s_retry_timer) xTimerStop(s_retry_timer, 0);
        } else {
            ESP_LOGW(TAG, "ota_retry_task: preflight failed; scheduling retry");
            schedule_ota_retry(60);
        }
    }
}

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

// Try to load a runtime CA PEM from the mounted filesystem.
// Returns malloc'd buffer which the caller must free, or NULL if not found.
static char *load_ca_pem(void)
{
    static const char *pem_candidates[] = { "/filesystem/ca_root.pem", "/filesystem/ca-root.pem", "/filesystem/cacert.pem", NULL };
    for (const char **pp = pem_candidates; *pp; ++pp) {
        char *buf = read_file_to_buffer(*pp);
        if (buf) {
            ESP_LOGI(TAG, "Loaded CA PEM from %s", *pp);
            return buf;
        }
    }
    ESP_LOGW(TAG, "No CA PEM found under /filesystem; will try global CA store if available");
    return NULL;
}

static int s_poll_minutes = 5; // default poll interval in minutes

// optional MQTT telemetry publish function (implemented in mqtt_manager)
extern void mqtt_publish_telemetry(const char *json_payload);

int ota_manager_get_poll_minutes(void) { return s_poll_minutes; }

void ota_manager_init(const char *manifest_url)
{
    ESP_LOGI(TAG, "ota_manager_init called (manifest_url=%s)", manifest_url ? manifest_url : "(none)");
    // For now we don't persist manifest_url; future work: read /filesystem/ota_config.json
}

// Ensure system time is sane before attempting TLS certificate validation.
// Returns true if system time appears valid (year >= 2020) or becomes valid
// within max_wait_seconds after starting SNTP. This is a best-effort helper
// to avoid certificate verification failures on devices with incorrect RTC.
static bool ensure_sane_time(int max_wait_seconds)
{
    time_t now = time(NULL);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2020) return true;

    ESP_LOGW(TAG, "system time looks incorrect (year=%d). Attempting SNTP sync before TLS attempts.", tm_now.tm_year + 1900);

    static bool sntp_inited = false;
    if (!sntp_inited) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_setservername(2, "time.cloudflare.com");
        esp_sntp_init();
        sntp_inited = true;
        ESP_LOGI(TAG, "SNTP initialized (servers: pool.ntp.org, time.google.com, time.cloudflare.com)");
    }

    int waited = 0;
    const int step_ms = 2000;
    while (waited < max_wait_seconds * 1000) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
        now = time(NULL);
        gmtime_r(&now, &tm_now);
        if (tm_now.tm_year + 1900 >= 2020) {
            ESP_LOGI(TAG, "system time after wait (UTC): %04d-%02d-%02d %02d:%02d:%02d",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
            return true;
        }
        ESP_LOGW(TAG, "still waiting for valid time (attempt=%d) year=%d", (waited / step_ms), tm_now.tm_year + 1900);
    }
    ESP_LOGW(TAG, "SNTP wait finished; system time still appears invalid (year=%d)", tm_now.tm_year + 1900);
    return false;
}

// Forward declaration: preflight check for ThingsBoard firmware API (defined later)
static bool ota_manager_thingsboard_preflight(const char *tb_base_url, const char *title, const char *version);



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
    // Attempt to load CA PEM from filesystem; fall back to global CA store
    char *pem_buf = load_ca_pem();
    esp_http_client_config_t ota_http_cfg = {
        .url = url->valuestring,
        .use_global_ca_store = (pem_buf == NULL),
        .cert_pem = (const char *)pem_buf,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &ota_http_cfg,
    };
    // Ensure system time is sane before attempting TLS handshake
    if (!ensure_sane_time(30)) {
        ESP_LOGW(TAG, "Proceeding with OTA attempt even though system time may be invalid");
    }
    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (pem_buf) free(pem_buf);
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

// External notification that HTTPS/TLS is operational (e.g., another
// component successfully performed an HTTPS request). Trigger immediate
// retry of any pending preflight-deferred OTA.
void ota_manager_notify_https_ready(void)
{
    ESP_LOGI(TAG, "ota_manager_notify_https_ready called");
    if (!s_pending.present) return;
    // Ensure ota task exists
    if (!s_ota_task) {
        BaseType_t rc = xTaskCreate(ota_retry_task, "ota_retry", 6*1024, NULL, tskIDLE_PRIORITY+3, &s_ota_task);
        if (rc != pdPASS) {
            ESP_LOGW(TAG, "ota_manager_notify_https_ready: failed to create ota task");
            schedule_ota_retry(30);
            return;
        }
    }
    xTaskNotifyGive(s_ota_task);
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
        cJSON_HasObjectItem(payload, "fw_checksum_algorithm"))
    {
        // If fw_url is present, use URL-based OTA
        if (cJSON_HasObjectItem(payload, "fw_url")) {
            ota_manager_apply_fota_from_attributes(payload);
        } else {
            // ThingsBoard often provides only title/version/checksum; use TB v1 firmware API
            const char *tb_host = "https://demo.thingsboard.io";
            // allow overriding TB base url via attribute 'tb_base_url'
            cJSON *tb_base = cJSON_GetObjectItemCaseSensitive(payload, "tb_base_url");
            if (cJSON_IsString(tb_base)) tb_host = tb_base->valuestring;

            const cJSON *title_item = cJSON_GetObjectItemCaseSensitive(payload, "fw_title");
            const cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(payload, "fw_version");
            const cJSON *checksum_item = cJSON_GetObjectItemCaseSensitive(payload, "fw_checksum");
            const cJSON *algo_item = cJSON_GetObjectItemCaseSensitive(payload, "fw_checksum_algorithm");

            if (!cJSON_IsString(title_item) || (ver_item == NULL) ) {
                ESP_LOGW(TAG, "OTA attributes missing title or version fields (unexpected types)");
                cJSON_Delete(root);
                return;
            }

            const char *title = title_item->valuestring;
            char version_buf[64] = {0};
            const char *version = NULL;
            if (cJSON_IsString(ver_item)) {
                version = ver_item->valuestring;
            } else if (cJSON_IsNumber(ver_item)) {
                /* convert numeric version to string for comparison and URL building */
                snprintf(version_buf, sizeof(version_buf), "%.15g", ver_item->valuedouble);
                version = version_buf;
            }
            const char *checksum = cJSON_IsString(checksum_item) ? checksum_item->valuestring : NULL;
            const char *algo = cJSON_IsString(algo_item) ? algo_item->valuestring : NULL;

            ESP_LOGI(TAG, "Initiating ThingsBoard firmware download by title=%s version=%s", title, version ? version : "(null)");

            /* Defensive check: if we already have this version persisted in NVS,
             * skip attempting OTA to avoid update loops when ThingsBoard's
             * attribute sync lags behind. */
            if (version) {
                char nvs_version[64] = {0};
                nvs_handle_t nh;
                if (nvs_open("ota", NVS_READONLY, &nh) == ESP_OK) {
                    size_t nsz = sizeof(nvs_version);
                    if (nvs_get_str(nh, "version", nvs_version, &nsz) == ESP_OK) {
                        if (nvs_version[0] != '\0' && strcmp(nvs_version, version) == 0) {
                            ESP_LOGI(TAG, "Already running version %s per NVS; ignoring OTA", nvs_version);
                            nvs_close(nh);
                            cJSON_Delete(root);
                            return;
                        }
                    }
                    nvs_close(nh);
                }
            }
            // Perform a lightweight TLS/auth preflight before attempting the full download.
            if (!ota_manager_thingsboard_preflight(tb_host, title, version)) {
                ESP_LOGW(TAG, "ThingsBoard preflight failed; deferring OTA until TLS/auth is ready and scheduling retry");
                // Store pending OTA metadata for retry
                s_pending.present = true;
                strncpy(s_pending.tb_base_url, tb_host, sizeof(s_pending.tb_base_url)-1);
                strncpy(s_pending.title, title, sizeof(s_pending.title)-1);
                strncpy(s_pending.version, version, sizeof(s_pending.version)-1);
                if (checksum) strncpy(s_pending.checksum, checksum, sizeof(s_pending.checksum)-1); else s_pending.checksum[0]=0;
                if (algo) strncpy(s_pending.algo, algo, sizeof(s_pending.algo)-1); else s_pending.algo[0]=0;
                // schedule first retry in 60 seconds
                schedule_ota_retry(60);
            } else {
                bool ok = ota_manager_download_and_apply_by_title(tb_host, title, version, checksum, algo);
                if (!ok) {
                    ESP_LOGE(TAG, "ThingsBoard firmware download by title failed");
                }
            }
        }
    }
    else
    {
        ESP_LOGW(TAG, "OTA attribute update missing required FOTA fields; ignoring");
    }
    cJSON_Delete(root);
}

bool ota_manager_download_and_apply_by_title(const char *tb_base_url, const char *title, const char *version, const char *expected_checksum, const char *checksum_algo)
{
    if (!tb_base_url || !title || !version) return false;
    const char *token = mqtt_get_access_token();
    if (!token) {
        ESP_LOGW(TAG, "No device token available for TB firmware API");
        return false;
    }

    char url[512];
    // Build URL: http(s)://<host>/api/v1/<ACCESS_TOKEN>/firmware?title=<TITLE>&version=<VERSION>
    snprintf(url, sizeof(url), "%s/api/v1/%s/firmware?title=%s&version=%s", tb_base_url, token, title, version);

    char *pem_buf = load_ca_pem();
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .skip_cert_common_name_check = false,
        .use_global_ca_store = (pem_buf == NULL),
        .cert_pem = (const char *)pem_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init http client for TB firmware API %s", url);
        return false;
    }

     /* mbed TLS MD context and state: initialize early so any goto to cleanup
         won't bypass initialization. md_initialized tracks whether md_setup succeeded. */
    mbedtls_md_context_t md_ctx;
    const mbedtls_md_info_t *md_info = NULL;
    mbedtls_md_init(&md_ctx);

    // No Authorization header necessary: token is in path per TB API
    // Now stream download to OTA (reuse earlier function flow but simpler inline)
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        goto cleanup_err2;
    }

    // prepare SHA
    if (expected_checksum && checksum_algo && strcmp(checksum_algo, "SHA256") == 0) {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        int _md_rc = mbedtls_md_setup(&md_ctx, md_info, 0);
        if (_md_rc == 0) {
            mbedtls_md_starts(&md_ctx);
            /* md setup succeeded */
        } else {
            ESP_LOGW(TAG, "mbedtls_md_setup failed: %d", _md_rc);
            /* leave md_info NULL to indicate digest not available */
            md_info = NULL;
        }
    }

    mqtt_publish_telemetry("{\"fw_state\":\"DOWNLOADING\"}");

    if (!ensure_sane_time(30)) {
        ESP_LOGW(TAG, "Proceeding with HTTP download even though system time may be invalid");
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup_err2;
    }

    /* Fetch headers so we can log content-length and other diagnostics */
    int content_length = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d content_length=%d", http_status, content_length);

    esp_err_t ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        goto cleanup_err2;
    }

    char buffer[1024];
    int read_len = 0;
    size_t total_read = 0;
    unsigned char preview[64];
    size_t preview_len = 0;
    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        ret = esp_ota_write(ota_handle, (const void *)buffer, read_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
            goto cleanup_ota2;
        }
        if (md_info) mbedtls_md_update(&md_ctx, (const unsigned char *)buffer, (size_t)read_len);
        /* capture a small preview of the beginning of the payload for diagnostics */
        if (preview_len < sizeof(preview)) {
            size_t take = (size_t)read_len;
            if (preview_len + take > sizeof(preview)) take = sizeof(preview) - preview_len;
            memcpy(preview + preview_len, buffer, take);
            preview_len += take;
        }
        total_read += (size_t)read_len;
    }
    if (read_len < 0) {
        ESP_LOGE(TAG, "Error reading HTTP response: %d", read_len);
        goto cleanup_ota2;
    }

    ESP_LOGI(TAG, "Total bytes downloaded: %u", (unsigned)total_read);
    if (total_read == 0) {
        ESP_LOGE(TAG, "Download produced zero bytes (empty payload)");
        /* publish a more specific telemetry error */
        mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"empty_download\"}");
        /* include preview (if any) in log as hex for debugging */
        if (preview_len > 0) {
            char h[preview_len*2 + 1];
            for (size_t i = 0; i < preview_len; ++i) snprintf(h + i*2, 3, "%02x", preview[i]);
            ESP_LOGI(TAG, "First %u bytes (hex): %s", (unsigned)preview_len, h);
        }
        goto cleanup_ota2;
    }

    mqtt_publish_telemetry("{\"fw_state\":\"DOWNLOADED\"}");

    if (md_info) {
        unsigned char sha[32];
        mbedtls_md_finish(&md_ctx, sha);
        char sha_hex[65];
        for (int i = 0; i < 32; ++i) snprintf(sha_hex + i*2, 3, "%02x", sha[i]);
        ESP_LOGI(TAG, "Computed SHA256: %s", sha_hex);
        if (expected_checksum && strcasecmp(expected_checksum, sha_hex) != 0) {
            ESP_LOGE(TAG, "Checksum mismatch: expected %s got %s", expected_checksum, sha_hex);
            mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"checksum_mismatch\"}");
            goto cleanup_ota2;
        }
        mqtt_publish_telemetry("{\"fw_state\":\"VERIFIED\"}");
    }

    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
        mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"ota_end_failed\"}");
        goto cleanup_err2;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"set_boot_failed\"}");
        goto cleanup_err2;
    }

    /* Persist version and title into NVS so the device can report the
     * current firmware version on next boot and ThingsBoard can confirm
     * the update. */
    {
        nvs_handle_t nh;
        if (nvs_open("ota", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "version", version);
            nvs_set_str(nh, "title", title);
            // mark as unconfirmed so next boot will send confirmation telemetry
            nvs_set_i32(nh, "confirmed", 0);
            nvs_commit(nh);
            nvs_close(nh);
            ESP_LOGI(TAG, "Persisted OTA version=%s title=%s to NVS (confirmed=0)", version, title);
        } else {
            ESP_LOGW(TAG, "Failed to open NVS to persist OTA version/title");
        }
    }

    char success_payload[128];
    snprintf(success_payload, sizeof(success_payload), "{\"current_fw_title\":\"%s\",\"current_fw_version\":\"%s\",\"fw_state\":\"UPDATED\"}", title, version);
    mqtt_publish_telemetry(success_payload);

    ESP_LOGI(TAG, "OTA applied successfully, restarting");
    esp_restart();
    return true;

cleanup_ota2:
    esp_ota_end(ota_handle);
cleanup_err2:
    /* Free md context unconditionally because md_ctx was initialized early */
    mbedtls_md_free(&md_ctx);
    esp_http_client_cleanup(client);
    if (pem_buf) free(pem_buf);
    return false;
}

// Perform a lightweight HEAD request to the ThingsBoard firmware API to verify
// TLS/auth before attempting a full download. Returns true if the endpoint is
// reachable and returns a 2xx-3xx status.
static bool ota_manager_thingsboard_preflight(const char *tb_base_url, const char *title, const char *version)
{
    if (!tb_base_url || !title || !version) return false;
    const char *token = mqtt_get_access_token();
    if (!token) {
        ESP_LOGW(TAG, "No device token available for TB preflight");
        return false;
    }
    if (!ensure_sane_time(30)) {
        ESP_LOGW(TAG, "Preflight: system time may be invalid; SNTP attempted");
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/api/v1/%s/firmware?title=%s&version=%s", tb_base_url, token, title, version);
    char *pem = load_ca_pem();
    // Diagnostics: log basic info about the PEM we loaded
    if (pem) {
        size_t pem_len = strlen(pem);
        int begin_count = 0;
        for (size_t i = 0; i + 15 < pem_len; ++i) {
            if (memcmp(pem + i, "-----BEGIN CERT", 15) == 0) begin_count++;
        }
        ESP_LOGI(TAG, "Preflight: loaded PEM length=%u bytes, BEGIN CERT count=%d", (unsigned)pem_len, begin_count);
        if (pem_len > 80) {
            char preview[81];
            memcpy(preview, pem, 80);
            preview[80] = '\0';
            ESP_LOGI(TAG, "Preflight: PEM preview: %s", preview);
        } else {
            ESP_LOGI(TAG, "Preflight: PEM preview: %s", pem);
        }
        if (begin_count == 0) {
            ESP_LOGW(TAG, "Preflight: PEM appears to contain no 'BEGIN CERTIFICATE' markers; it may be malformed");
        }
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_HEAD,
        .skip_cert_common_name_check = false,
        .use_global_ca_store = (pem == NULL),
        .cert_pem = (const char *)pem,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (pem) free(pem);
        ESP_LOGW(TAG, "Preflight: failed to init http client");
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = 0;
    if (err == ESP_OK) status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (pem) free(pem);
    if (err == ESP_OK && status >= 200 && status < 400) {
        ESP_LOGI(TAG, "Preflight OK: %s returned HTTP %d", url, status);
        return true;
    }
    ESP_LOGW(TAG, "Preflight failed: err=%s status=%d", esp_err_to_name(err), status);
    return false;
}

bool ota_manager_probe_thingsboard_firmware(const char *tb_base_url, const char *package_id)
{
    if (!tb_base_url || !package_id) return false;
    const char *token = mqtt_get_access_token();
    if (!token) {
        ESP_LOGW(TAG, "No device token available for ThingsBoard probe");
        return false;
    }

    // Try a list of common endpoints
    const char *paths[] = {
        "/api/firmware/",            // e.g. /api/firmware/{id}/download
        "/api/plugins/firmware/",    // e.g. /api/plugins/firmware/{id}/download
        NULL
    };

    for (const char **p = paths; *p; ++p) {
        char url[256];
        snprintf(url, sizeof(url), "%s%s%s/download", tb_base_url, *p, package_id);
        char *pem_buf = load_ca_pem();
        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_HEAD,
            .skip_cert_common_name_check = false,
            .use_global_ca_store = (pem_buf == NULL),
            .cert_pem = (const char *)pem_buf,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            if (pem_buf) free(pem_buf);
            continue;
        }
        // set Authorization header with token
        char auth[256];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "Probe URL %s returned HTTP %d", url, status);
            esp_http_client_cleanup(client);
            if (pem_buf) free(pem_buf);
            if (status >= 200 && status < 400) return true;
            continue;
        } else {
            ESP_LOGW(TAG, "Probe URL %s failed: %s", url, esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
        if (pem_buf) free(pem_buf);
    }
    ESP_LOGW(TAG, "No ThingsBoard firmware endpoint reachable for package %s", package_id);
    return false;
}

bool ota_manager_download_and_apply_from_thingsboard(const char *tb_base_url, const char *package_id, const char *expected_checksum, const char *checksum_algo)
{
    if (!tb_base_url || !package_id) return false;
    const char *token = mqtt_get_access_token();
    if (!token) {
        ESP_LOGW(TAG, "No device token available for TB download");
        return false;
    }

    char url[256];
    // prefer plugin endpoint first
    snprintf(url, sizeof(url), "%s/api/plugins/firmware/%s/download", tb_base_url, package_id);

    char *pem_buf = load_ca_pem();
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .skip_cert_common_name_check = false,
        .use_global_ca_store = (pem_buf == NULL),
        .cert_pem = (const char *)pem_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init http client for %s", url);
        return false;
    }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth);

    // Start OTA
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        goto cleanup_err;
    }

    // compute SHA256 on the fly
    mbedtls_md_context_t md_ctx;
    const mbedtls_md_info_t *md_info = NULL;
    mbedtls_md_init(&md_ctx);
    if (expected_checksum && checksum_algo && strcmp(checksum_algo, "SHA256") == 0) {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        int _md_rc = mbedtls_md_setup(&md_ctx, md_info, 0);
        if (_md_rc == 0) {
            mbedtls_md_starts(&md_ctx);
            /* md setup succeeded */
        } else {
            ESP_LOGW(TAG, "mbedtls_md_setup failed: %d", _md_rc);
            md_info = NULL;
        }
    }

    // Signal start
    mqtt_publish_telemetry("{\"fw_state\":\"DOWNLOADING\"}");

    if (!ensure_sane_time(30)) {
        ESP_LOGW(TAG, "Proceeding with HTTP download even though system time may be invalid");
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        goto cleanup_err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d content_length=%d", http_status, content_length);

    esp_err_t ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        goto cleanup_err;
    }

    // Read loop
    char buffer[1024];
    int read_len = 0;
    size_t total_read = 0;
    unsigned char preview[64];
    size_t preview_len = 0;
    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        ret = esp_ota_write(ota_handle, (const void *)buffer, read_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
            goto cleanup_ota;
        }
        if (md_info) {
            mbedtls_md_update(&md_ctx, (const unsigned char *)buffer, (size_t)read_len);
        }
        if (preview_len < sizeof(preview)) {
            size_t take = (size_t)read_len;
            if (preview_len + take > sizeof(preview)) take = sizeof(preview) - preview_len;
            memcpy(preview + preview_len, buffer, take);
            preview_len += take;
        }
        total_read += (size_t)read_len;
    }

    if (read_len < 0) {
        ESP_LOGE(TAG, "Error reading HTTP response: %d", read_len);
        goto cleanup_ota;
    }

    ESP_LOGI(TAG, "Total bytes downloaded: %u", (unsigned)total_read);
    if (total_read == 0) {
        ESP_LOGE(TAG, "Download produced zero bytes (empty payload)");
        mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"empty_download\"}");
        if (preview_len > 0) {
            char h[preview_len*2 + 1];
            for (size_t i = 0; i < preview_len; ++i) snprintf(h + i*2, 3, "%02x", preview[i]);
            ESP_LOGI(TAG, "First %u bytes (hex): %s", (unsigned)preview_len, h);
        }
        goto cleanup_ota;
    }

    mqtt_publish_telemetry("{\"fw_state\":\"DOWNLOADED\"}");

    // finish SHA
    unsigned char sha[32];
    if (md_info) {
        mbedtls_md_finish(&md_ctx, sha);
        char sha_hex[65];
        for (int i = 0; i < 32; ++i) snprintf(sha_hex + i*2, 3, "%02x", sha[i]);
        ESP_LOGI(TAG, "Computed SHA256: %s", sha_hex);
        if (expected_checksum && strcasecmp(expected_checksum, sha_hex) != 0) {
            ESP_LOGE(TAG, "Checksum mismatch: expected %s got %s", expected_checksum, sha_hex);
            mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"checksum_mismatch\"}");
            goto cleanup_ota;
        }
        mqtt_publish_telemetry("{\"fw_state\":\"VERIFIED\"}");
    }

    // complete OTA
    ret = esp_ota_end(ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
        mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"ota_end_failed\"}");
        goto cleanup_err;
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        mqtt_publish_telemetry("{\"fw_state\":\"FAILED\",\"fw_error\":\"set_boot_failed\"}");
        goto cleanup_err;
    }

    // publish updated info before restart
    /* Persist a best-effort representation of the firmware identity in NVS
     * so that on next boot the device can report it back to ThingsBoard. If
     * a human-friendly version string isn't available, use the package id or
     * expected checksum as the version. */
    {
        const char *store_ver = expected_checksum ? expected_checksum : package_id;
        const char *store_title = package_id;
        nvs_handle_t nh;
        if (nvs_open("ota", NVS_READWRITE, &nh) == ESP_OK) {
            nvs_set_str(nh, "version", store_ver);
            nvs_set_str(nh, "title", store_title);
            nvs_set_i32(nh, "confirmed", 0);
            nvs_commit(nh);
            nvs_close(nh);
            ESP_LOGI(TAG, "Persisted OTA version=%s title=%s to NVS (confirmed=0)", store_ver, store_title);
        } else {
            ESP_LOGW(TAG, "Failed to open NVS to persist OTA version/title");
        }
    }

    char success_payload[128];
    snprintf(success_payload, sizeof(success_payload), "{\"current_fw_title\":\"%s\",\"current_fw_version\":\"%s\",\"fw_state\":\"UPDATED\"}", package_id, expected_checksum ? expected_checksum : "");
    mqtt_publish_telemetry(success_payload);

    ESP_LOGI(TAG, "OTA applied successfully, restarting");
    if (pem_buf) free(pem_buf);
    esp_restart();
    return true; // not reached

cleanup_ota:
    esp_ota_end(ota_handle);
cleanup_err:
    /* md_ctx was initialized early; free unconditionally */
    mbedtls_md_free(&md_ctx);
    esp_http_client_cleanup(client);
    if (pem_buf) free(pem_buf);
    return false;
}
