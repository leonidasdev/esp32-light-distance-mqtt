#pragma once

#include <stdbool.h>

// Initialize OTA manager. manifest_url may be NULL to use default or filesystem config.
void ota_manager_init(const char *manifest_url);

// Perform a manifest check and, if a newer firmware is found, download and apply it.
// This function blocks until the update completes or fails. Returns true on successful
// update and reboot (note: function will usually not return on success because device
// restarts), false if no update was applied or on failure.
bool ota_manager_check_and_update(void);

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

// Start/stop the internal OTA poller task. The poller will call
// `ota_manager_check_and_update()` periodically using the poll interval
// returned by `ota_manager_get_poll_minutes()`.
void ota_manager_start_poller(void);
void ota_manager_stop_poller(void);
