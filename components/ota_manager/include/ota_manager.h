
#pragma once
#include <stdbool.h>
#include <cJSON.h>

// Initialize OTA manager. manifest_url may be NULL to use default or filesystem config.
void ota_manager_init(const char *manifest_url);

// Apply FOTA update using ThingsBoard attributes (called internally)
// Returns true if update was applied and device will reboot, false otherwise.
bool ota_manager_apply_fota_from_attributes(cJSON *root);

// Configure scheduled update time (24-hour clock)
void ota_manager_set_schedule(int hour, int minute);

// Enable/disable update on boot
void ota_manager_enable_on_boot(bool enable);

// Report current status (string) via MQTT or logging
void ota_manager_report_status(const char *status, const char *detail);

// Handle an attribute update payload (JSON). This will trigger OTA actions
// if the payload contains `ota_manifest_url` or `ota_command` keys.
void ota_manager_handle_attribute_update(const char *json_payload);

// Return configured polling interval in minutes (default 5)
int ota_manager_get_poll_minutes(void);

// Probe ThingsBoard REST firmware endpoints for a given package id using the
// stored device access token. Returns true if a reachable endpoint was found.
bool ota_manager_probe_thingsboard_firmware(const char *tb_base_url, const char *package_id);

// Download firmware package from ThingsBoard REST API and apply OTA. Returns
// true if update applied (device will restart), false otherwise.
bool ota_manager_download_and_apply_from_thingsboard(const char *tb_base_url, const char *package_id, const char *expected_checksum, const char *checksum_algo);

// Download firmware package using ThingsBoard v1 firmware API by title and version
bool ota_manager_download_and_apply_by_title(const char *tb_base_url, const char *title, const char *version, const char *expected_checksum, const char *checksum_algo);

// Notify the OTA manager that HTTPS/TLS is ready (for example when another
// component successfully performed an HTTPS request). This will cause any
// pending preflight-deferred OTA to be retried immediately.
void ota_manager_notify_https_ready(void);

// (Poller API removed; FOTA is now attribute-driven)
