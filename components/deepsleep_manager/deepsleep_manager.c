#include "deepsleep_manager.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static const char *TAG = "deepsleep";
static uint64_t interval_ms = 0;
static uint64_t idle_timeout_ms = 0; // how long the device stays active before sleeping
static bool enabled_flag = false; // persisted as third line: 1 or 0
static char storage_root[128];
static TaskHandle_t idle_countdown_task = NULL;

// Idle-countdown task: when enabled, starts a one-shot countdown of
// idle_timeout_ms and triggers deep sleep via maybe_sleep_after_publish().
static void idle_countdown_task_fn(void *arg)
{
    (void)arg;
    uint64_t wait_ms = idle_timeout_ms;
    if (wait_ms == 0) {
        idle_countdown_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "idle_countdown: waiting %llu ms before sleeping", (unsigned long long)wait_ms);
    vTaskDelay(pdMS_TO_TICKS((TickType_t)wait_ms));
    if (enabled_flag) {
        ESP_LOGI(TAG, "idle_countdown expired and deep-sleep is enabled; initiating sleep");
        deepsleep_manager_maybe_sleep_after_publish();
        // If maybe_sleep returns for any reason, clear the task handle here.
        idle_countdown_task = NULL;
    } else {
        ESP_LOGI(TAG, "idle_countdown expired but deep-sleep is disabled; not sleeping");
        idle_countdown_task = NULL;
    }
    vTaskDelete(NULL);
}

static void start_idle_countdown(void)
{
    // If a countdown is already running, cancel it first
    if (idle_countdown_task) {
        vTaskDelete(idle_countdown_task);
        idle_countdown_task = NULL;
    }
    if (!enabled_flag) return;
    if (idle_timeout_ms == 0) {
        ESP_LOGI(TAG, "start_idle_countdown: idle_timeout_ms == 0, not starting countdown");
        return;
    }
    BaseType_t ok = xTaskCreate(idle_countdown_task_fn, "ds_idle_cnt", 2048, NULL, tskIDLE_PRIORITY + 1, &idle_countdown_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create idle_countdown task");
        idle_countdown_task = NULL;
    }
}

static void stop_idle_countdown(void)
{
    if (idle_countdown_task) {
        vTaskDelete(idle_countdown_task);
        idle_countdown_task = NULL;
        ESP_LOGI(TAG, "idle_countdown cancelled");
    }
}

// Helper: read whole file into malloc'd buffer, return length via out_len. Caller frees.
static char *read_file_whole(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = '\0';
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/*
 * Historically this module attempted atomic writes using a `.tmp` rename
 * helper. That approach caused failures on some filesystems (errno=22), and
 * the codebase prefers direct overwrite writes now (the user explicitly
 * requested avoiding `.tmp` files). The old atomic helper is intentionally
 * removed to avoid an unused-function warning and to keep behaviour explicit.
 */

bool deepsleep_manager_init(const char *storage_root_path)
{
    if (!storage_root_path) return false;
    snprintf(storage_root, sizeof(storage_root), "%s", storage_root_path);
    char path[256];
    /* Use `sleep.txt` on the data partition; first line holds the interval in ms */
    snprintf(path, sizeof(path), "%s/sleep.txt", storage_root);
    size_t len = 0;
    char *buf = read_file_whole(path, &len);
    if (buf)
    {
        if (len > 0)
        {
            /* parse first line for interval_ms */
            char *line_end = strchr(buf, '\n');
            size_t first_len = line_end ? (size_t)(line_end - buf) : (size_t)len;
            char *tmp = malloc(first_len + 1);
            if (tmp) {
                memcpy(tmp, buf, first_len);
                tmp[first_len] = '\0';
                char *endptr = NULL;
                unsigned long long val = strtoull(tmp, &endptr, 10);
                if (endptr != tmp) {
                    interval_ms = (uint64_t)val;
                    ESP_LOGI(TAG, "Loaded deepsleep interval %llu ms", (unsigned long long)interval_ms);
                }
                free(tmp);
            }

            /* parse second line for idle_timeout_ms (if present) */
            if (line_end) {
                char *second = line_end + 1;
                char *second_end = strchr(second, '\n');
                size_t second_len = second_end ? (size_t)(second_end - second) : strlen(second);
                if (second_len > 0) {
                    char *tmp2 = malloc(second_len + 1);
                    if (tmp2) {
                        memcpy(tmp2, second, second_len);
                        tmp2[second_len] = '\0';
                        char *endptr2 = NULL;
                        unsigned long long val2 = strtoull(tmp2, &endptr2, 10);
                        if (endptr2 != tmp2) {
                            idle_timeout_ms = (uint64_t)val2;
                            ESP_LOGI(TAG, "Loaded idle timeout %llu ms", (unsigned long long)idle_timeout_ms);
                        }
                        free(tmp2);
                    }
                }
                /* parse third line for enabled flag if present */
                if (second_end) {
                    char *third = second_end + 1;
                    char *third_end = strchr(third, '\n');
                    size_t third_len = third_end ? (size_t)(third_end - third) : strlen(third);
                    if (third_len > 0) {
                        char *tmp3 = malloc(third_len + 1);
                        if (tmp3) {
                            memcpy(tmp3, third, third_len);
                            tmp3[third_len] = '\0';
                            if (strcmp(tmp3, "1") == 0) enabled_flag = true;
                            else enabled_flag = false;
                            ESP_LOGI(TAG, "Loaded deep-sleep enabled=%d", enabled_flag ? 1 : 0);
                            free(tmp3);
                        }
                    }
                }
            }
        }
        free(buf);
    }
    else
    {
        ESP_LOGI(TAG, "No deepsleep config found, disabled");
    }
    // Do not start the countdown here. Higher-level code (e.g. network
    // initialization and Telegram initial sync) should call
    // deepsleep_manager_start_idle_countdown() once the system is ready so
    // the idle timer begins only after connectivity is established.
    return true;
}

bool deepsleep_manager_start_idle_countdown(void)
{
    if (!enabled_flag) {
        ESP_LOGI(TAG, "start_idle_countdown requested but deep-sleep disabled");
        return false;
    }
    start_idle_countdown();
    return true;
}

bool deepsleep_manager_set_interval_ms(uint64_t ms)
{
    if (storage_root[0] == '\0') {
        ESP_LOGW(TAG, "storage_root not initialized; cannot persist interval");
        return false;
    }

    char path[256];
    /* Persist interval in the first line of sleep.txt on the data partition */
    snprintf(path, sizeof(path), "%s/sleep.txt", storage_root);
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)ms);
    if (n <= 0) return false;
    ESP_LOGI(TAG, "Persisting deepsleep interval %s to %s", tmp, path);

    /* Read existing file to preserve subsequent lines (if any) */
    size_t existing_len = 0;
    char *existing = read_file_whole(path, &existing_len);
    /* Build new content: <tmp>\n + remainder of existing after first newline (if present) */
    size_t new_cap = (size_t)n + 2 + (existing ? existing_len : 0) + 8;
    char *newbuf = malloc(new_cap);
    if (!newbuf) {
        ESP_LOGE(TAG, "Out of memory while preparing sleep.txt content");
        if (existing) free(existing);
        return false;
    }
    /* write first line */
    int p = snprintf(newbuf, new_cap, "%s\n", tmp);
    if (p < 0) p = 0;
    /* append rest of existing after first newline */
    if (existing) {
        char *nl = strchr(existing, '\n');
        if (nl) {
            /* include everything after the first newline */
            size_t tail_len = existing_len - (size_t)(nl - existing) - 1;
            memcpy(newbuf + p, nl + 1, tail_len);
            p += (int)tail_len;
        } else {
            /* no newline in existing; nothing more to append */
        }
        free(existing);
    }
    /* ensure null termination is not necessary for write */
    /* User requested avoiding temporary files like sleep.txt.tmp; perform
     * a direct write to sleep.txt (overwriting). This is not atomic but
     * avoids issues where creating/renaming .tmp files fails (errno=22, etc.). */
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing deepsleep interval: errno=%d (%s)", path, errno, strerror(errno));
        free(newbuf);
        return false;
    }
    size_t w = fwrite(newbuf, 1, (size_t)p, f);
    if (fflush(f) == 0) { int fd = fileno(f); if (fd >= 0) fsync(fd); }
    fclose(f);
    if (w != (size_t)p) {
        ESP_LOGE(TAG, "Direct write('%s') failed: wrote=%zu expected=%zu", path, w, (size_t)p);
        free(newbuf);
        return false;
    }
    ESP_LOGI(TAG, "Direct persist succeeded for %s", path);
    free(newbuf);
    interval_ms = ms;
    ESP_LOGI(TAG, "New deepsleep interval set to %llu ms", (unsigned long long)interval_ms);
    return true;
}

// Helper: persist all three fields (first: interval, second: idle timeout, third: enabled flag)
static bool persist_sleep_config(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/sleep.txt", storage_root);
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%llu\n%llu\n%u\n", (unsigned long long)interval_ms, (unsigned long long)idle_timeout_ms, enabled_flag ? 1U : 0U);
    if (n <= 0) return false;
    FILE *f = fopen(path, "w");
    if (!f) { ESP_LOGE(TAG, "Failed to open %s for writing config: errno=%d (%s)", path, errno, strerror(errno)); return false; }
    size_t w = fwrite(buf, 1, (size_t)n, f);
    if (fflush(f) == 0) { int fd = fileno(f); if (fd >= 0) fsync(fd); }
    fclose(f);
    if (w != (size_t)n) { ESP_LOGE(TAG, "Persist write failed for %s: wrote=%zu expected=%d", path, w, n); return false; }
    ESP_LOGI(TAG, "Persisted sleep config to %s (interval=%llu idle=%llu enabled=%u)", path, (unsigned long long)interval_ms, (unsigned long long)idle_timeout_ms, enabled_flag ? 1U : 0U);
    return true;
}

bool deepsleep_manager_set_idle_timeout_ms(uint64_t ms)
{
    if (storage_root[0] == '\0') {
        ESP_LOGW(TAG, "storage_root not initialized; cannot persist idle timeout");
        return false;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/sleep.txt", storage_root);
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)ms);
    if (n <= 0) return false;
    ESP_LOGI(TAG, "Persisting idle timeout %s to %s (second line)", tmp, path);

    /* Read existing file to preserve first line and subsequent lines */
    size_t existing_len = 0;
    char *existing = read_file_whole(path, &existing_len);
    char *newbuf = NULL;
    int p = 0;
    if (!existing) {
        /* no existing file -> create with blank first line and second line = tmp */
        size_t newcap = (size_t)n + 4;
        newbuf = malloc(newcap);
        if (!newbuf) return false;
        p = snprintf(newbuf, newcap, "\n%s\n", tmp);
    } else {
        /* keep first line as-is (if any), replace or insert second line */
        char *nl = strchr(existing, '\n');
        if (!nl) {
            /* single-line existing content: append newline + tmp */
            size_t newcap = existing_len + 2 + (size_t)n + 4;
            newbuf = malloc(newcap);
            if (!newbuf) { free(existing); return false; }
            p = snprintf(newbuf, newcap, "%s\n%s\n", existing, tmp);
        } else {
            /* there is at least one newline; build: firstline\n tmp \n remainder(after second line) */
            char *third = strchr(nl + 1, '\n');
            size_t tail_len = 0;
            if (third) tail_len = existing_len - (size_t)(third - existing) - 1;
            size_t newcap = (size_t)(nl - existing) + 2 + (size_t)n + 2 + tail_len + 8;
            newbuf = malloc(newcap);
            if (!newbuf) { free(existing); return false; }
            /* copy first line */
            int off = snprintf(newbuf, newcap, "%.*s\n", (int)(nl - existing), existing);
            /* write second line (tmp) */
            off += snprintf(newbuf + off, (size_t)newcap - (size_t)off, "%s\n", tmp);
            /* append remainder after second newline if present */
            if (third) {
                size_t remain = existing_len - (size_t)(third - existing) - 1;
                memcpy(newbuf + off, third + 1, remain);
                off += (int)remain;
            }
            p = off;
            free(existing);
        }
    }

    /* Direct write (no .tmp) per user request */
    FILE *f2 = fopen(path, "w");
    if (!f2) {
        ESP_LOGE(TAG, "Failed to open %s for writing idle timeout: errno=%d (%s)", path, errno, strerror(errno));
        free(newbuf);
        return false;
    }
    size_t w2 = fwrite(newbuf, 1, (size_t)p, f2);
    if (fflush(f2) == 0) { int fd2 = fileno(f2); if (fd2 >= 0) fsync(fd2); }
    fclose(f2);
    if (w2 != (size_t)p) { ESP_LOGE(TAG, "Direct write failed for idle timeout: wrote=%zu expected=%zu", w2, (size_t)p); free(newbuf); return false; }
    ESP_LOGI(TAG, "Direct persist succeeded for idle timeout to %s", path);
    free(newbuf);
    idle_timeout_ms = ms;
    ESP_LOGI(TAG, "New idle timeout set to %llu ms", (unsigned long long)idle_timeout_ms);
    // persist full config (so enabled flag remains in sync) and restart countdown if enabled
    persist_sleep_config();
    if (enabled_flag) start_idle_countdown();
    return true;
}

bool deepsleep_manager_set_enabled(bool enabled)
{
    if (storage_root[0] == '\0') {
        ESP_LOGW(TAG, "storage_root not initialized; cannot persist enabled flag");
        return false;
    }

    enabled_flag = enabled;
    if (!persist_sleep_config()) {
        ESP_LOGW(TAG, "Failed to persist enabled flag change");
        return false;
    }
    ESP_LOGI(TAG, "Deep-sleep enabled set to %d", enabled ? 1 : 0);
    if (enabled) start_idle_countdown(); else stop_idle_countdown();
    return true;
}

bool deepsleep_manager_is_enabled(void)
{
    return enabled_flag;
}

uint64_t deepsleep_manager_get_idle_timeout_ms(void)
{
    return idle_timeout_ms;
}

uint64_t deepsleep_manager_get_interval_ms(void)
{
    return interval_ms;
}

void deepsleep_manager_maybe_sleep_after_publish(void)
{
    if (interval_ms == 0) return;
    if (!enabled_flag) { ESP_LOGI(TAG, "Deep-sleep is disabled (enabled_flag=0); skipping sleep"); return; }
    // Diagnostic: ensure only the idle-countdown task triggers the sleep
    void *caller = __builtin_return_address(0);
    ESP_LOGI(TAG, "maybe_sleep called from %p", caller);

    // Only allow the idle_countdown task to trigger this function.
    if (idle_countdown_task == NULL || xTaskGetCurrentTaskHandle() != idle_countdown_task) {
        ESP_LOGW(TAG, "maybe_sleep called from non-idle task; ignoring to prevent accidental sleep");
        return;
    }

    ESP_LOGI(TAG, "Entering deep sleep for %llu ms", (unsigned long long)interval_ms);
    esp_sleep_enable_timer_wakeup(interval_ms * 1000ULL);
    // small delay to let logs flush
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

bool deepsleep_manager_force_sleep(void)
{
    // Allow forcing sleep even if the idle countdown task exists; cancel it
    // to avoid duplicate attempts to call esp_deep_sleep_start().
    stop_idle_countdown();
    if (interval_ms == 0) return false;
    if (!enabled_flag) { ESP_LOGI(TAG, "Force-sleep requested but deep-sleep disabled"); return false; }
    ESP_LOGI(TAG, "Force-sleep: entering deep sleep for %llu ms", (unsigned long long)interval_ms);
    esp_sleep_enable_timer_wakeup(interval_ms * 1000ULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
    return true; // not reached
}
