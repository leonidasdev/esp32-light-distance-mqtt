#include "telegram.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* Deepsleep manager API (persisted sleep interval/idle timeout) */
#include "deepsleep_manager.h"

/*
 * telegram_manager
 * ----------------
 * Small, self-contained Telegram long-poll client used in this project.
 * Responsibilities:
 *  - Load bot token and persisted last_update_id from `tele.txt` (third line)
 *  - Perform a network/TLS preflight (SNTP, getMe) and a short initial
 *    getUpdates sync to advance the cursor without replying to historical
 *    messages.
 *  - Long-poll getUpdates and dispatch textual messages to a registered
 *    handler (or handle a set of built-in commands implemented below).
 *  - Send messages using sendMessage (percent-encoded query param).
 *
 * Design notes:
 *  - JSON parsing is intentionally minimal (string/integer extraction only)
 *    to avoid bringing in a JSON dependency on the device.
 *  - TLS certificate PEM is loaded at runtime from the mounted data
 *    partition (candidates defined below). This keeps the binary small and
 *    allows shipping CA bundles via the filesystem.
 */

/* Filesystem root used for runtime CA PEM discovery. Duplicate of the
 * definition in main.c; keeping here makes this TU self-contained. */
#ifndef FILESYSTEM_ROOT
#define FILESYSTEM_ROOT "/filesystem"
#endif

/* PEM candidate paths (attempted in order). */
#ifndef PEM_CANDIDATE1
#define PEM_CANDIDATE1 FILESYSTEM_ROOT "/ca_root.pem"
#endif
#ifndef PEM_CANDIDATE2
#define PEM_CANDIDATE2 FILESYSTEM_ROOT "/ca-root.pem"
#endif
#ifndef PEM_CANDIDATE3
#define PEM_CANDIDATE3 FILESYSTEM_ROOT "/cacert.pem"
#endif

/* Local logging tag for this component */
static const char *TAG = "telegram";

/* Persistence and state variables used by the module */
static char bot_token[256] = "";
static char tele_file_path[512] = ""; /* path to tele.txt for persistence */
static int64_t last_update_id = 0;

/* Optional message handler registered by the application */
static void (*msg_handler)(int64_t, const char *, void *) = NULL;
static void *msg_ctx = NULL;

/* SNTP initialization flag */
static bool sntp_inited = false;

/* Minimal network helpers used as preflight checks. These are simple
 * implementations that avoid introducing additional dependencies; they
 * intentionally only provide basic behaviour needed by telegram.c. */
static void wait_for_ip(int timeout_seconds)
{
    /* In this codebase the networking stack will be brought up elsewhere.
     * Here we simply wait a small amount of time (up to timeout_seconds)
     * so that higher-level connection attempts have a chance to succeed.
     */
    for (int i = 0; i < timeout_seconds; ++i) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void dns_connect_test(const char *host, const char *port)
{
    /* Stub diagnostic: nothing heavy here, just log intent. A more
     * thorough implementation could attempt a socket connect to surface
     * low-level errno values, but that's unnecessary for compilation. */
    (void)host; (void)port;
    ESP_LOGI(TAG, "dns_connect_test: (stub) host=%s port=%s", host, port);
}

bool telegram_init_from_file(const char *token_file_path)
{
    FILE *f = fopen(token_file_path, "r");
    if (!f) return false;
    if (fgets(bot_token, sizeof(bot_token), f) == NULL)
    {
        fclose(f);
        return false;
    }
    // Try to read an optional third line that contains the persisted last_update_id
    // token file layout (lines):
    // 1: bot token
    // 2: (optional) admin chat id or comment
    // 3: (optional) persisted last_update_id
    // Copy the token file path for future writes
    strncpy(tele_file_path, token_file_path, sizeof(tele_file_path) - 1);
    // consume second line if present
    char buf[128];
    if (fgets(buf, sizeof(buf), f) != NULL) {
        // second line present; attempt to read third line
        if (fgets(buf, sizeof(buf), f) != NULL) {
            // trim newline and parse as int64
            buf[strcspn(buf, "\r\n")] = '\0';
            if (buf[0] != '\0') {
                int64_t persisted = 0;
                // use sscanf to allow large numbers
                if (sscanf(buf, "%lld", (long long *)&persisted) == 1) {
                    last_update_id = persisted;
                    ESP_LOGI(TAG, "Loaded persisted last_update_id=%lld from %s", (long long)last_update_id, token_file_path);
                } else {
                    ESP_LOGI(TAG, "No valid persisted last_update_id in %s (third line)", token_file_path);
                }
            }
        }
    }
    fclose(f);
    // remove trailing newline
    bot_token[strcspn(bot_token, "\r\n")] = '\0';
    ESP_LOGI(TAG, "Telegram token loaded (len=%d)", (int)strlen(bot_token));
    return true;
}

void telegram_register_message_handler(void (*handler)(int64_t, const char *, void *), void *user_ctx)
{
    msg_handler = handler;
    msg_ctx = user_ctx;
}

static bool http_get(const char *url, char **out, int *out_len)
{
    // Configure HTTP client. Prefer the compiled-in certificate bundle when
    // available (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE). Otherwise, attempt to
    // obtain a PEM buffer via esp_crt_bundle_get() as a runtime fallback.
    esp_http_client_config_t config;
    memset(&config, 0, sizeof(config));
    config.url = url;
    config.method = HTTP_METHOD_GET;
    // default timeout (10s) — may be overridden for long-polling requests
    config.timeout_ms = 10000;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;

    // If URL contains a Telegram long-polling 'timeout=<seconds>' parameter,
    // make the HTTP client's timeout slightly larger so the client does not
    // abort the request before the server returns (telegram uses long polling).
    char *tpos = strstr(url, "timeout=");
    if (tpos) {
        int server_timeout = atoi(tpos + strlen("timeout="));
        if (server_timeout > 0) {
            // add a small margin (5s) and convert to ms
            int new_ms = (server_timeout + 5) * 1000;
            // cap to a sensible maximum (e.g., 120s)
            if (new_ms > 120000) new_ms = 120000;
            config.timeout_ms = new_ms;
            ESP_LOGI(TAG, "http_get: detected timeout=%d, setting client timeout_ms=%d", server_timeout, config.timeout_ms);
        }
    }

    /* Load CA PEM from the mounted data partition. We try a few
     * candidate filenames and use the first one found. The returned buffer
     * is malloc'd and must be freed after esp_http_client_cleanup(). */
    static const char *pem_candidates[] = { PEM_CANDIDATE1, PEM_CANDIDATE2, PEM_CANDIDATE3, NULL };
    char *pem_buf = NULL;
    size_t pem_len = 0;
    for (const char **pp = pem_candidates; *pp; ++pp) {
        FILE *f = fopen(*pp, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long s = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (s <= 0) { fclose(f); continue; }
        pem_buf = malloc((size_t)s + 1);
        if (!pem_buf) { fclose(f); break; }
        size_t r = fread(pem_buf, 1, (size_t)s, f);
        pem_buf[r] = '\0';
        fclose(f);
        pem_len = r;
        ESP_LOGI(TAG, "Loaded CA PEM from %s (bytes=%d)", *pp, (int)pem_len);
        /* NOTE: runtime x509 parsing removed per user request. We simply
         * load the PEM and hand it to esp_http_client via config.cert_pem. */
        break;
    }
    if (!pem_buf) {
        ESP_LOGE(TAG, "No CA PEM found under %s; cannot perform TLS requests", FILESYSTEM_ROOT);
        return false;
    }
    config.cert_pem = pem_buf;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    // Set a common User-Agent and Accept header; some servers vary responses by UA.
    esp_http_client_set_header(client, "User-Agent", "curl/7.88.1");
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");

    // Use explicit open/fetch_headers/read instead of esp_http_client_perform()
    // because some servers and transport layers behave differently when the
    // perform() helper consumes the body via event callbacks. Opening and
    // fetching headers gives us control to read the body with esp_http_client_read().
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http_get open failed for %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(pem_buf);
        return false;
    }

    // Enable verbose TLS logs here to capture per-request handshake details
    esp_log_level_set("esp_tls", ESP_LOG_DEBUG);
    esp_log_level_set("esp_tls_mbedtls", ESP_LOG_DEBUG);

    int fetch_ret = esp_http_client_fetch_headers(client);
    int content_len = fetch_ret;
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "http_get: url=%s fetch_ret=%d status=%d content_length=%d", url, fetch_ret, status, content_len);

    // If fetch_headers returned an error or content/status look invalid, dump some headers
    if (fetch_ret < 0 || status <= 0 || content_len <= 0) {
        const char *hdrs_to_check[] = { "Content-Type", "Content-Length", "Transfer-Encoding", "Connection", "Server", "Content-Encoding", "Location", NULL };
        for (const char **hp = hdrs_to_check; *hp; ++hp) {
            char *val = NULL;
            esp_err_t r = esp_http_client_get_header(client, *hp, &val);
            if (r == ESP_OK && val) ESP_LOGW(TAG, "Response header: %s: %s", *hp, val);
            else ESP_LOGW(TAG, "Response header: %s: <absent> (err=%d)", *hp, r);
        }
        // Also log any transport-layer error code if available via esp_err_t
        if (fetch_ret < 0) ESP_LOGW(TAG, "esp_http_client_fetch_headers returned %d (%s)", fetch_ret, esp_err_to_name(fetch_ret));
    }

    // Read response in a loop to support chunked or unknown content-length
    const size_t chunk = 512;
    size_t cap = chunk;
    char *buf = malloc(cap + 1);
    if (!buf) { esp_http_client_cleanup(client); return false; }
    size_t total = 0;
    while (1) {
        int r = esp_http_client_read(client, buf + total, (int)(cap - total));
        if (r > 0) {
            total += (size_t)r;
            // expand if we're full
            if (cap - total < chunk) {
                size_t newcap = cap + chunk;
                char *nb = realloc(buf, newcap + 1);
                if (!nb) { free(buf); esp_http_client_close(client); esp_http_client_cleanup(client); return false; }
                buf = nb; cap = newcap;
            }
            continue;
        }
        // r == 0 -> no more data available; r < 0 -> error
        if (r < 0) {
            ESP_LOGW(TAG, "http_get read error (%d) for %s", r, url);
            free(buf); esp_http_client_close(client); esp_http_client_cleanup(client); return false;
        }
        if (r == 0) {
            // no more data available. If total == 0, log content length and headers
            if (total == 0) {
                ESP_LOGI(TAG, "http_get: read returned 0 bytes (no body). content_length=%d", content_len);
            }
        }
        break;
    }
    buf[total] = '\0';
    *out = buf;
    if (out_len) *out_len = (int)total;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(pem_buf);
    return true;
}

// Minimal JSON extraction helpers (not robust but small): find "text":"..." and "id":<num>
static char *extract_json_string(const char *buf, const char *key)
{
    char *p = strstr(buf, key);
    if (!p) return NULL;
    p = strchr(p, ':');
    if (!p) return NULL;
    p++;
    // skip spaces
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    const char *start = p;
    // find closing quote (not handling escapes)
    while (*p && *p != '"') p++;
    if (*p != '"') return NULL;
    size_t len = p - start;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int64_t extract_json_int(const char *buf, const char *key)
{
    char *p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    int64_t v = 0;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    return v * sign;
}

// Extract chat id by finding the "chat" object and then the "id" within it.
static int64_t extract_chat_id_from_update(const char *update_buf)
{
    // Try message.chat.id
    char *p = strstr((char *)update_buf, "\"message\"");
    if (p) {
        char *chatp = strstr(p, "\"chat\"");
        if (chatp) {
            char *idp = strstr(chatp, "\"id\"");
            if (idp) return extract_json_int(idp, "\"id\"");
        }
    }
    // Try channel_post.chat.id
    p = strstr((char *)update_buf, "\"channel_post\"");
    if (p) {
        char *chatp = strstr(p, "\"chat\"");
        if (chatp) {
            char *idp = strstr(chatp, "\"id\"");
            if (idp) return extract_json_int(idp, "\"id\"");
        }
    }
    // fallback: any chat id
    char *chatp = strstr((char *)update_buf, "\"chat\"");
    if (chatp) {
        char *idp = strstr(chatp, "\"id\"");
        if (idp) return extract_json_int(idp, "\"id\"");
    }
    // my_chat_member events have chat at top-level under "my_chat_member":{"chat":{...}}
    char *mcmp = strstr((char *)update_buf, "\"my_chat_member\"");
    if (mcmp) {
        char *chatp2 = strstr(mcmp, "\"chat\"");
        if (chatp2) {
            char *idp2 = strstr(chatp2, "\"id\"");
            if (idp2) return extract_json_int(idp2, "\"id\"");
        }
    }
    return 0;
}

// Forward-declare command handler which is defined later in this file.
static void handle_incoming_message(int64_t chat_id, const char *text);

// Process an array of update position pointers found in the response.
// Updates are parsed and message handler / command handler invoked.
static void process_updates(char **update_positions, int update_count, bool ignore_last_cursor, int64_t *pmax_processed_uid)
{
    for (int ui = 0; ui < update_count; ++ui) {
        char *upd = update_positions[ui];
        int64_t uid = extract_json_int(upd, "\"update_id\"");
        if (!ignore_last_cursor && uid <= last_update_id) {
            continue;
        }
        // extract and handle message text for this update
        char *text = extract_json_string(upd, "\"text\"");
        int64_t chat_id = extract_chat_id_from_update(upd);
        // Debug: log raw update snippet, chat id and text (if any) to help diagnose why commands might not be recognized
        if (text) {
            ESP_LOGI(TAG, "update_id=%lld chat=%lld text='%s'", (long long)uid, (long long)chat_id, text);
        } else {
            // log a short preview of the update payload for non-text updates
            char preview[128] = {0};
            int pshow = 0;
            if (upd) {
                const char *start = upd;
                while (*start && *start != '{') start++; // try to align to JSON start
                if (start) {
                    const char *end = start;
                    while (*end && *end != '\n' && (size_t)(end - start) < 120) end++;
                    pshow = (int)(end - start);
                    if (pshow > 0) memcpy(preview, start, (size_t)pshow);
                    preview[pshow] = '\0';
                }
            }
            ESP_LOGI(TAG, "update_id=%lld chat=%lld (no text), preview='%s'", (long long)uid, (long long)chat_id, preview);
        }
        if (!text) {
            ESP_LOGW(TAG, "No text found in update_id=%lld chat=%lld", (long long)uid, (long long)chat_id);
        }
        ESP_LOGI(TAG, "processing update_id=%lld chat=%lld (oldest->newest)", (long long)uid, (long long)chat_id);
        if (text) {
            handle_incoming_message(chat_id, text);
            free(text);
        } else {
            // no text: still allow message handler to inspect update (if registered)
            if (msg_handler) msg_handler(chat_id, NULL, msg_ctx);
        }
        if (uid > *pmax_processed_uid) *pmax_processed_uid = uid;
    }
}

// Persist the highest processed update id into the third line of tele.txt (atomic tmp+rename).
static bool persist_last_update_id(int64_t new_last_update_id)
{
    if (tele_file_path[0] == '\0') return false;
    FILE *fr = fopen(tele_file_path, "r");
    char first[256] = ""; char second[256] = "";
    if (fr) {
        if (fgets(first, sizeof(first), fr) == NULL) first[0] = '\0';
        if (fgets(second, sizeof(second), fr) == NULL) second[0] = '\0';
        fclose(fr);
    }

    /* Direct write to tele_file_path (user requested no .tmp usage). This
     * overwrites the file with up-to-date first/second lines and the new
     * persisted id on the third line. This is not atomic but avoids issues
     * with creating temporary files on some filesystems. */
    FILE *fw = fopen(tele_file_path, "w");
    if (!fw) {
        ESP_LOGW(TAG, "Failed to open %s for writing persisted last_update_id (errno=%d %s)", tele_file_path, errno, strerror(errno));
        return false;
    }
    // ensure first line ends with newline
    if (first[0] != '\0' && first[strlen(first)-1] != '\n') fprintf(fw, "%s\n", first);
    else if (first[0] != '\0') fprintf(fw, "%s", first);
    // second line
    if (second[0] != '\0' && second[strlen(second)-1] != '\n') fprintf(fw, "%s\n", second);
    else if (second[0] != '\0') fprintf(fw, "%s", second);
    // write third line as the persisted id
    fprintf(fw, "%lld\n", (long long)new_last_update_id);
    fclose(fw);
    ESP_LOGI(TAG, "Persisted last_update_id=%lld to %s (direct write)", (long long)new_last_update_id, tele_file_path);
    return true;
}

// Centralized handler for incoming text messages (keeps telegram_task concise)
static void handle_incoming_message(int64_t chat_id, const char *text)
{
    // Debug entry log to confirm handler invocation
    ESP_LOGI(TAG, "handle_incoming_message invoked for chat=%lld text='%s'", (long long)chat_id, text ? text : "(null)");
    if (!text) return;
    // Commands start with '/'
    if (text[0] != '/') {
        if (msg_handler) msg_handler(chat_id, text, msg_ctx);
        else telegram_send_message(chat_id, "Not a valid command");
        return;
    }

    // /setdeepsleepduration
    if (strncasecmp(text, "/setdeepsleepduration", strlen("/setdeepsleepduration")) == 0) {
        const char *arg = text + strlen("/setdeepsleepduration"); while (*arg == ' ') arg++;
        if (*arg == '\0') {
            telegram_send_message(chat_id, "Usage: /setdeepsleepduration <milliseconds>");
            return;
        }
        char *endptr = NULL; unsigned long long val = strtoull(arg, &endptr, 10);
        if (endptr == arg || val < 1000ULL || val > 604800000ULL) {
            telegram_send_message(chat_id, "Invalid value. Provide milliseconds between 1000 and 604800000.");
            return;
        }
        if (deepsleep_manager_set_interval_ms((uint64_t)val)) {
            char rsp[128]; snprintf(rsp, sizeof(rsp), "deepsleep interval set to %llu ms", (unsigned long long)val);
            telegram_send_message(chat_id, rsp);
        } else {
            telegram_send_message(chat_id, "Failed to persist deepsleep interval.");
        }
        return;
    }

    // /setdeepsleepdelay
    if (strncasecmp(text, "/setdeepsleepdelay", strlen("/setdeepsleepdelay")) == 0) {
        const char *arg = text + strlen("/setdeepsleepdelay"); while (*arg == ' ') arg++;
        if (*arg == '\0') {
            telegram_send_message(chat_id, "Usage: /setdeepsleepdelay <milliseconds>");
            return;
        }
        char *endptr = NULL; unsigned long long val = strtoull(arg, &endptr, 10);
        if (endptr == arg || val < 100ULL || val > 86400000ULL) {
            telegram_send_message(chat_id, "Invalid value. Provide milliseconds between 100 and 86400000.");
            return;
        }
        if (deepsleep_manager_set_idle_timeout_ms((uint64_t)val)) {
            char rsp[128]; snprintf(rsp, sizeof(rsp), "idle timeout set to %llu ms", (unsigned long long)val);
            telegram_send_message(chat_id, rsp);
        } else {
            telegram_send_message(chat_id, "Failed to persist idle timeout.");
        }
        return;
    }

    // /toggledeepsleep on|off
    if (strncasecmp(text, "/toggledeepsleep", strlen("/toggledeepsleep")) == 0) {
        const size_t cmdlen = strlen("/toggledeepsleep");
        const char *arg = text + cmdlen; while (*arg == ' ') arg++;
        if (strncasecmp(arg, "off", 3) == 0) {
            if (deepsleep_manager_set_enabled(false)) telegram_send_message(chat_id, "deepsleep disabled");
            else telegram_send_message(chat_id, "Failed to disable deepsleep.");
            return;
        } else if (strncasecmp(arg, "on", 2) == 0) {
            uint64_t ms = deepsleep_manager_get_interval_ms();
            if (ms == 0) {
                telegram_send_message(chat_id, "No interval set. Use /setdeepsleepduration <ms> first.");
            } else {
                if (deepsleep_manager_set_enabled(true)) {
                    char rsp2[128]; snprintf(rsp2, sizeof(rsp2), "deepsleep enabled (interval: %llu ms)", (unsigned long long)ms);
                    telegram_send_message(chat_id, rsp2);
                } else {
                    telegram_send_message(chat_id, "Failed to enable deepsleep.");
                }
            }
            return;
        } else {
            telegram_send_message(chat_id, "Usage: /toggledeepsleep on|off");
            return;
        }
    }

    // /getdeepsleepstatus
    if (strncasecmp(text, "/getdeepsleepstatus", strlen("/getdeepsleepstatus")) == 0) {
        uint64_t ms = deepsleep_manager_get_interval_ms(); uint64_t idle = deepsleep_manager_get_idle_timeout_ms();
        bool en = deepsleep_manager_is_enabled();
        char buf[256];
        if (ms == 0) snprintf(buf, sizeof(buf), "deepsleep interval not set; enabled=%d; idle timeout=%llu ms", en ? 1 : 0, (unsigned long long)idle);
        else snprintf(buf, sizeof(buf), "deepsleep interval=%llu ms; enabled=%d; idle timeout=%llu ms", (unsigned long long)ms, en ? 1 : 0, (unsigned long long)idle);
        telegram_send_message(chat_id, buf);
        return;
    }

    // /getid
    if (strncasecmp(text, "/getid", strlen("/getid")) == 0) {
        char idbuf[64]; snprintf(idbuf, sizeof(idbuf), "%lld", (long long)chat_id);
        telegram_send_message(chat_id, idbuf);
        return;
    }

    // /deepsleep -> immediately attempt to sleep (subject to enabled & interval)
    if (strncasecmp(text, "/deepsleep", strlen("/deepsleep")) == 0) {
        uint64_t ms = deepsleep_manager_get_interval_ms();
        if (ms == 0) {
            telegram_send_message(chat_id, "No deepsleep interval set. Use /setdeepsleepduration <ms> first.");
            return;
        }
        if (!deepsleep_manager_is_enabled()) {
            telegram_send_message(chat_id, "Deep-sleep is currently disabled. Use /toggledeepsleep on to enable, or use /deepsleep to force immediate sleep after enabling.");
            return;
        }
        // Confirm to user and enter deep sleep
        char rsp[128]; snprintf(rsp, sizeof(rsp), "Entering deep sleep for %llu ms", (unsigned long long)ms);
        telegram_send_message(chat_id, rsp);
        // allow a short delay so the message can be sent before sleeping
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!deepsleep_manager_force_sleep()) {
            telegram_send_message(chat_id, "Failed to force deep sleep (check enabled flag and interval)");
        }
        return;
    }

    // Unknown command
    telegram_send_message(chat_id, "Unknown command");
}

static void telegram_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "telegram_task started");
    while (1)
    {
        // getUpdates with offset
        // build URL dynamically to avoid truncation warnings
        const char *fmt_with_offset = "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=20";
        const char *fmt_no_offset = "https://api.telegram.org/bot%s/getUpdates?timeout=20";
        char *url = NULL;
        if (last_update_id)
        {
            int need = snprintf(NULL, 0, fmt_with_offset, bot_token, (long long)(last_update_id + 1)) + 1;
            url = malloc((size_t)need);
            if (!url) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            snprintf(url, need, fmt_with_offset, bot_token, (long long)(last_update_id + 1));
        }
        else
        {
            int need = snprintf(NULL, 0, fmt_no_offset, bot_token) + 1;
            url = malloc((size_t)need);
            if (!url) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            snprintf(url, need, fmt_no_offset, bot_token);
        }

        char *resp = NULL;
        int resp_len = 0;
        bool made_offset_request = (last_update_id != 0);
        if (!http_get(url, &resp, &resp_len))
        {
            if (url) { free(url); url = NULL; }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // debug: log a short preview of the response body
        if (resp && resp_len > 0) {
            int show = resp_len > 256 ? 256 : resp_len;
            char tmp_preview[257];
            memcpy(tmp_preview, resp, show);
            tmp_preview[show] = '\0';
            ESP_LOGI(TAG, "getUpdates response preview: %s", tmp_preview);
        }

        // Parse all update entries and process them in ascending update_id order
        // If last_update_id is non-zero it acts as the persisted cursor; otherwise
        // we will behave as before (initial sync logic in telegram_start) and
        // process whatever updates appear.
        // Simple parsing: find all occurrences of "update_id" and record the
        // pointer to that place; then process them in the order found (which
        // corresponds to oldest->newest in Telegram's JSON array response).
        #define MAX_UPDATES 64
        char *update_positions[MAX_UPDATES];
        int update_count = 0;
        char *scan = resp;
        while ((scan = strstr(scan, "\"update_id\"")) != NULL && update_count < MAX_UPDATES) {
            update_positions[update_count++] = scan;
            scan++; // advance to avoid infinite loop
        }

        bool ignore_last_cursor = false;
        // If we requested with an offset (last_update_id+1) and got no updates,
        // there may be updates with lower ids present on the server. In that
        // case perform a fallback request without offset and inspect whether
        // the persisted last_update_id exists in the returned set. If it does
        // not, per user request we should start from the first message id
        // received (i.e., process all returned updates) instead of skipping
        // to last_update_id+1.
        if (update_count == 0 && made_offset_request) {
            ESP_LOGI(TAG, "offset query (offset=%lld) returned no updates; trying fallback without offset", (long long)(last_update_id + 1));
            free(resp);
            resp = NULL;
            if (url) { free(url); url = NULL; }
            int need2 = snprintf(NULL, 0, fmt_no_offset, bot_token) + 1;
            url = malloc((size_t)need2);
            if (!url) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            snprintf(url, need2, fmt_no_offset, bot_token);
            if (!http_get(url, &resp, &resp_len)) {
                ESP_LOGW(TAG, "fallback getUpdates without offset failed");
                free(url); url = NULL;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            // rebuild update_positions from the fallback response
            update_count = 0;
            scan = resp;
            while ((scan = strstr(scan, "\"update_id\"")) != NULL && update_count < MAX_UPDATES) {
                update_positions[update_count++] = scan;
                scan++;
            }
            if (update_count > 0) {
                // check whether persisted last_update_id is present among returned updates
                bool found_last = false;
                for (int i = 0; i < update_count; ++i) {
                    int64_t uid = extract_json_int(update_positions[i], "\"update_id\"");
                    if (uid == last_update_id) { found_last = true; break; }
                }
                if (!found_last) {
                    ESP_LOGI(TAG, "persisted last_update_id=%lld not found in fallback response; processing from first returned update_id", (long long)last_update_id);
                    ignore_last_cursor = true;
                } else {
                    ESP_LOGI(TAG, "persisted last_update_id=%lld found in fallback response; skipping <= persisted id", (long long)last_update_id);
                }
            }
        }

        int64_t max_processed_uid = last_update_id;

        // Process updates (delegates to handle_incoming_message/msg_handler)
        process_updates(update_positions, update_count, ignore_last_cursor, &max_processed_uid);

        // Log skipped updates for diagnostics and print the full response when empty
        if (update_count == 0) {
            ESP_LOGI(TAG, "No updates in this poll (last_update_id=%lld), response_len=%d", (long long)last_update_id, resp_len);
            if (resp && resp_len > 0) {
                int show = resp_len > 2048 ? 2048 : resp_len;
                char *full = malloc((size_t)show + 1);
                if (full) {
                    memcpy(full, resp, (size_t)show);
                    full[show] = '\0';
                    ESP_LOGI(TAG, "getUpdates full response (truncated %d/%d): %s", show, resp_len, full);
                    free(full);
                }
            } else {
                ESP_LOGI(TAG, "getUpdates response body empty");
            }
        }

        // After processing all returned updates, persist the highest update_id
        if (max_processed_uid > last_update_id) {
            last_update_id = max_processed_uid;
            if (!persist_last_update_id(last_update_id)) {
                ESP_LOGW(TAG, "persist_last_update_id failed for %lld", (long long)last_update_id);
            }
        }

        if (resp) { free(resp); resp = NULL; }
        if (url) { free(url); url = NULL; }

    // short delay before next poll (getUpdates used with timeout=20 so we loop quickly)
    vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void telegram_start(void)
{
    // Quick sanity check of system time - certificate validation requires a
    // reasonably correct RTC. If the time is far in the past the TLS stack
    // will reject otherwise-valid certs.
    time_t now = time(NULL);
    struct tm tm_now;
    gmtime_r(&now, &tm_now);
    ESP_LOGI(TAG, "system time (UTC): %04d-%02d-%02d %02d:%02d:%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    if (tm_now.tm_year + 1900 < 2020) {
        ESP_LOGW(TAG, "system time looks incorrect (year=%d). Attempting SNTP sync before TLS attempts.", tm_now.tm_year + 1900);
        // initialize SNTP to attempt to update the RTC (only once)
            if (!sntp_inited) {
                esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                // set a few well-known servers; if network blocks NTP you may need a local NTP server
                esp_sntp_setservername(0, "pool.ntp.org");
                esp_sntp_setservername(1, "time.google.com");
                esp_sntp_setservername(2, "time.cloudflare.com");
                esp_sntp_init();
            sntp_inited = true;
            ESP_LOGI(TAG, "SNTP initialized (servers: pool.ntp.org, time.google.com, time.cloudflare.com)");
        }

        // wait up to ~60s for NTP sync
        int tries = 30;
        for (int i = 0; i < tries; ++i) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            now = time(NULL);
            gmtime_r(&now, &tm_now);
            if (tm_now.tm_year + 1900 >= 2020) break;
            ESP_LOGW(TAG, "still waiting for valid time (attempt %d), year=%d", i+1, tm_now.tm_year + 1900);
        }
        ESP_LOGI(TAG, "system time after wait (UTC): %04d-%02d-%02d %02d:%02d:%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }
    // Preflight: try contacting Telegram with getMe to ensure network/TLS is ready.
    // Retry a few times before proceeding. This helps avoid starting the poller
    // when Wi-Fi/DNS/TLS are not yet available.
    {
        // Enable verbose TLS logging for the preflight so we can see mbedTLS errors
        esp_log_level_set("esp_tls", ESP_LOG_DEBUG);
        esp_log_level_set("esp_tls_mbedtls", ESP_LOG_DEBUG);

    // Ensure we have an IP address before attempting connections
    wait_for_ip(30);
    // Quick DNS + TCP connect diagnostic to surface connect errno
    dns_connect_test("api.telegram.org", "443");

    const int max_retries = 5;
        int attempt = 0;
        bool ok = false;
        while (attempt < max_retries) {
            ++attempt;
            const char *fmt_getme = "https://api.telegram.org/bot%s/getMe";
            int need = snprintf(NULL, 0, fmt_getme, bot_token) + 1;
            char *url = malloc((size_t)need);
            if (!url) break;
            snprintf(url, need, fmt_getme, bot_token);
            char *resp = NULL; int rl = 0;
            if (http_get(url, &resp, &rl) && resp) {
                ESP_LOGI(TAG, "telegram_start: getMe success on attempt %d", attempt);
                ok = true;
                free(resp);
                free(url);
                break;
            }
            ESP_LOGW(TAG, "telegram_start: getMe attempt %d failed; retrying...", attempt);
            if (resp) free(resp);
            free(url);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (!ok) {
            ESP_LOGW(TAG, "telegram_start: getMe preflight failed after %d attempts; polling will still start but may fail until network/TLS is ready", max_retries);
        }
    }

    // Perform an initial sync to consume any backlog updates so we don't reply to
    // historical messages in other chats. This advances last_update_id to the
    // highest update seen without processing messages.
    {
        /* Short initial sync to avoid long blocking on wake: request a very
         * short long-poll (1s) and limit=1 so we return quickly. This avoids
         * delaying startup work while still advancing the cursor if needed. */
        const char *fmt_sync = "https://api.telegram.org/bot%s/getUpdates?timeout=1&limit=1";
        int need = snprintf(NULL, 0, fmt_sync, bot_token) + 1;
        char *url = malloc((size_t)need);
        if (url) {
            snprintf(url, need, fmt_sync, bot_token);
            ESP_LOGI(TAG, "telegram_start: performing short initial sync (timeout=1&limit=1)");
            time_t t0 = time(NULL);
            char *resp = NULL; int rl = 0;
            if (http_get(url, &resp, &rl) && resp) {
                time_t t1 = time(NULL);
                ESP_LOGI(TAG, "telegram_start: initial sync returned in %lld ms", (long long)((t1 - t0) * 1000));
                // find highest update_id
                char *p = resp;
                int64_t max_uid = last_update_id;
                while ((p = strstr(p, "\"update_id\"")) != NULL) {
                    int64_t uid = extract_json_int(p, "\"update_id\"");
                    if (uid > max_uid) max_uid = uid;
                    p++;
                }
                if (max_uid > last_update_id) {
                    last_update_id = max_uid;
                    ESP_LOGI(TAG, "telegram_start: skipped backlog up to update_id=%lld", (long long)last_update_id);
                }
            } else {
                ESP_LOGI(TAG, "telegram_start: initial sync returned no response or failed");
            }
            if (resp) free(resp);
            free(url);
        }
    }

    // Start the deep-sleep idle countdown now that the initial Telegram
    // sync and network preflight have completed. This prevents the device
    // from starting the idle timer immediately at boot before we've had a
    // chance to contact Telegram.
    if (deepsleep_manager_is_enabled()) {
        if (deepsleep_manager_start_idle_countdown()) {
            ESP_LOGI(TAG, "Started deep-sleep idle countdown after initial sync");
        }
    }

    xTaskCreate(telegram_task, "telegram_task", 6 * 1024, NULL, tskIDLE_PRIORITY + 1, NULL);
}

bool telegram_send_message(int64_t chat_id, const char *text)
{
    char *url = NULL;
    // conservative percent-encoding for URL query parameter (text)
    // allocate a temporary buffer twice the input length + 1, capped to a reasonable size
    size_t text_len = text ? strlen(text) : 0;
    size_t enc_cap = text_len * 3 + 1; // worst-case every char encoded as %XX
    if (enc_cap > 1024) enc_cap = 1024; // cap to avoid large stack/heap use
    char *encoded = malloc(enc_cap);
    if (!encoded) return false;
    size_t ei = 0;
    for (size_t i = 0; i < text_len && ei + 4 < enc_cap; ++i)
    {
        unsigned char c = (unsigned char)text[i];
        // safe chars per RFC3986 (unreserved) : ALPHA / DIGIT / '-' / '.' / '_' / '~'
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            encoded[ei++] = (char)c;
        } else {
            // percent-encode
            if (ei + 3 < enc_cap) {
                static const char hex[] = "0123456789ABCDEF";
                encoded[ei++] = '%';
                encoded[ei++] = hex[(c >> 4) & 0xF];
                encoded[ei++] = hex[c & 0xF];
            } else {
                break; // no space for encoding
            }
        }
    }
    encoded[ei] = '\0';
    const char *fmt = "https://api.telegram.org/bot%s/sendMessage?chat_id=%lld&text=%s";
    int need = snprintf(NULL, 0, fmt, bot_token, (long long)chat_id, encoded) + 1;
    url = malloc((size_t)need);
    if (!url) { free(encoded); return false; }
    snprintf(url, need, fmt, bot_token, (long long)chat_id, encoded);
    char *tmp = NULL; int tl = 0;
    bool ok = http_get(url, &tmp, &tl);
    if (!ok) {
        ESP_LOGW(TAG, "http_get failed when sending message to chat=%lld", (long long)chat_id);
        if (tmp) free(tmp);
        free(url);
        free(encoded);
        return false;
    }
    // Inspect Telegram API response JSON for 'ok' and optional 'description'
    bool api_ok = false;
    if (tmp) {
        if (strstr(tmp, "\"ok\":true") != NULL) api_ok = true;
        if (!api_ok) {
            char *desc = extract_json_string(tmp, "\"description\"");
            if (desc) {
                ESP_LOGW(TAG, "Telegram API error sending to chat=%lld: %s", (long long)chat_id, desc);
                free(desc);
            } else {
                // log first 512 bytes of response for debugging
                int l = strlen(tmp);
                int show = l > 512 ? 512 : l;
                char preview[513]; memcpy(preview, tmp, show); preview[show] = '\0';
                ESP_LOGW(TAG, "Telegram API returned failure for chat=%lld, response preview: %s", (long long)chat_id, preview);
            }
        } else {
            ESP_LOGI(TAG, "Telegram API sendMessage ok for chat=%lld", (long long)chat_id);
        }
        free(tmp);
    }
    free(url);
    free(encoded);
    return api_ok;
}
