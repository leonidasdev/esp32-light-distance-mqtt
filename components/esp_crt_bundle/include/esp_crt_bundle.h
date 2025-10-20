#pragma once
#include "esp_err.h"

// Returns a NULL-terminated PEM string containing CA roots, or NULL if unavailable.
// If you have the official esp-crt-bundle component, prefer that. This project
// provides a lightweight runtime loader that reads /filesystem/ca_root.pem when
// present and returns it as a pointer. Caller must not free the returned pointer.
const char *esp_crt_bundle_get(void);

// Register a runtime PEM buffer with the local bundle. The function will copy
// the provided buffer into an internal static allocation and return ESP_OK on
// success. This is a convenience to ensure the runtime bundle is available to
// other components that may call esp_crt_bundle_get().
esp_err_t esp_crt_bundle_set(const char *pem, size_t len);
