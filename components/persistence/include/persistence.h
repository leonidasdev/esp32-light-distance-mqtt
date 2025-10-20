/*
 * persistence.h
 *
 * Simple helpers to mount the data partition and read/write a tiny
 * WiFi configuration (ssid/password). The APIs favour minimal heap use and
 * make ownership explicit: when `persistence_read_config` allocates strings
 * they must be freed with `persistence_config_free`.
 */

#ifndef MAIN_PERSISTENCE_H_
#define MAIN_PERSISTENCE_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct persistence_config {
    /* NUL-terminated strings. When returned from persistence_read_config the
     * caller owns these pointers and must call persistence_config_free(). */
    char *ssid;
    char *password;
};

/**
 * Mount a FAT32 partition provided by the partition label. `mountpoint` is the
 * path where the partition will be mounted (e.g. "/filesystem"). Returns on
 * success or logs/aborts on failure (behaviour mirrors previous implementation).
 */
void fat32_mount(const char *mountpoint, const char *partition);

/**
 * Read a persisted config from `path`. On success returns true and fills
 * `config` with allocated strings which must be freed by
 * `persistence_config_free()`; on failure returns false and leaves `config`
 * untouched.
 */
bool persistence_read_config(const char *path, struct persistence_config *config);

/**
 * Save a config to `path`. The function reads values from `config` and does
 * not take ownership. Returns true on success and false on failure.
 */
bool persistence_save_config(const char *path, struct persistence_config *config);

/**
 * Free strings allocated by persistence_read_config(). Safe to call on a
 * partially populated structure (it will free non-NULL members).
 */
void persistence_config_free(struct persistence_config *config);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_PERSISTENCE_H_ */