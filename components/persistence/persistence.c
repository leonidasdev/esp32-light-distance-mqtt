#include "persistence.h"

#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "FILESYSTEM";

/*
 * fat32_mount
 * Mount a FAT filesystem on the provided mountpoint. This helper wraps the
 * esp-vfs fat mount call with a small, documented configuration. The
 * function will assert on error via ESP_ERROR_CHECK so callers should ensure
 * the environment (NVS, flash partition) is prepared.
 */
void fat32_mount(const char *mountpoint, const char *partition)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = false,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat = false,
    };

    wl_handle_t s_wl_handle;
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(mountpoint, partition, &mount_config, &s_wl_handle));
    ESP_LOGI(TAG, "Mounted FAT32 `%s' on `%s'", partition, mountpoint);
}

/*
 * persistence_read_config
 * Reads a simple two-line file containing SSID and password. Allocates
 * two buffers on success and stores them into the provided `config`.
 * The caller must free them using persistence_config_free().
 */
bool persistence_read_config(const char *path, struct persistence_config *config)
{
    ESP_LOGI(TAG, "Reading config file `%s'...", path);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Error opening config file `%s' for reading", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "Failed to seek end of file `%s'", path);
        fclose(file);
        return false;
    }
    long ft = ftell(file);
    if (ft < 0) {
        ESP_LOGE(TAG, "ftell failed for `%s'", path);
        fclose(file);
        return false;
    }
    size_t file_size = (size_t)ft;
    if (fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to rewind file `%s'", path);
        fclose(file);
        return false;
    }

    /* allocate reasonable buffers based on file size */
    char *ssid = calloc(1, file_size + 1);
    char *password = calloc(1, file_size + 1);
    if (!ssid || !password) {
        ESP_LOGE(TAG, "Out of memory allocating config buffers");
        fclose(file);
        free(ssid);
        free(password);
        return false;
    }

    if (fgets(ssid, (int)file_size + 1, file) == NULL || fgets(password, (int)file_size + 1, file) == NULL) {
        ESP_LOGE(TAG, "Error reading config file `%s', file may be corrupted or too short", path);
        fclose(file);
        free(ssid);
        free(password);
        return false;
    }

    fclose(file);

    /* strip trailing newline characters */
    ssid[strcspn(ssid, "\r\n")] = '\0';
    password[strcspn(password, "\r\n")] = '\0';

    config->ssid = ssid;
    config->password = password;

    ESP_LOGI(TAG, "Successfully read config file `%s'", path);
    return true;
}

/*
 * persistence_save_config
 * Persist a two-line config file with SSID and password. Overwrites the
 * target file. Returns true on success.
 */
bool persistence_save_config(const char *path, struct persistence_config *config)
{
    if (!config || !config->ssid || !config->password) {
        ESP_LOGE(TAG, "Invalid config provided to persistence_save_config");
        return false;
    }
    ESP_LOGI(TAG, "Saving config file `%s'", path);
    ESP_LOGI(TAG, "\tSSID: %s", config->ssid);

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        ESP_LOGE(TAG, "Error opening config file `%s' for writing", path);
        return false;
    }
    /* write SSID and password on separate lines */
    if (fprintf(file, "%s\n%s\n", config->ssid, config->password) < 0) {
        ESP_LOGE(TAG, "Failed to write to `%s'", path);
        fclose(file);
        return false;
    }
    fflush(file);
    fclose(file);

    ESP_LOGI(TAG, "New configuration saved to `%s'", path);
    return true;
}

/*
 * persistence_config_free
 * Frees buffers allocated by persistence_read_config.
 */
void persistence_config_free(struct persistence_config *config)
{
    if (!config) return;
    free(config->ssid);
    free(config->password);
    config->ssid = NULL;
    config->password = NULL;
}