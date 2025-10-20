/*
 * This local shim is intentionally disabled. The project should use the
 * upstream esp-crt-bundle component provided by ESP-IDF. Leaving the code
 * present but wrapped in #if 0 avoids deleting it from source control while
 * preventing duplicate-symbol link errors when the SDK also provides the
 * same symbols.
 */
#if 0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "esp_crt_bundle";

static char *buf = NULL;

const char *esp_crt_bundle_get(void)
{
    /* implementation omitted */
    return NULL;
}

esp_err_t esp_crt_bundle_set(const char *pem, size_t len)
{
    (void)pem; (void)len; return ESP_ERR_NOT_SUPPORTED;
}
#endif
