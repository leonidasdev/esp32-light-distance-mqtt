#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * deepsleep_manager
 * -----------------
 * High-level API for managing a persisted deep-sleep configuration stored
 * in the data partition as `sleep.txt` (three lines):
 *   1) interval_ms   - deep-sleep wake interval in milliseconds (0 == disabled)
 *   2) idle_timeout  - how long the device remains active before entering sleep
 *   3) enabled_flag  - 1 == enabled, 0 == disabled
 *
 * This module provides helpers to read and persist those values and to
 * coordinate entering deep sleep. The design separates setting the
 * parameters (set_interval/set_idle/set_enabled) from the runtime idle
 * countdown (start_idle_countdown) so higher-level code can decide when
 * to begin the idle timer (for example after network initialization).
 */

// Initialize the deep-sleep manager; reads saved interval from filesystem (`sleep.txt`).
// storage_root should be the mounted data partition root (for example "/filesystem").
bool deepsleep_manager_init(const char *storage_root);

// Set and persist the deep sleep interval (milliseconds)
bool deepsleep_manager_set_interval_ms(uint64_t ms);

// Set and persist the idle timeout (milliseconds)
bool deepsleep_manager_set_idle_timeout_ms(uint64_t ms);

// Query functions
uint64_t deepsleep_manager_get_idle_timeout_ms(void);
uint64_t deepsleep_manager_get_interval_ms(void);

// If the configuration permits, this function will start deep sleep. It is
// intended to be called internally by the idle-countdown task. Ad-hoc callers
// will be ignored to avoid accidental sleeps; use deepsleep_manager_force_sleep()
// to forcibly trigger sleep from other contexts.
void deepsleep_manager_maybe_sleep_after_publish(void);

// Enable/disable deep-sleep without changing the configured interval.
// Persisted as the third line of sleep.txt: '1' == enabled, '0' == disabled.
bool deepsleep_manager_set_enabled(bool enabled);
bool deepsleep_manager_is_enabled(void);

// Start the idle countdown (based on the configured idle timeout) without
// changing persistence. Should be called once the system is network-ready
// to begin the idle timer that will eventually call maybe_sleep.
bool deepsleep_manager_start_idle_countdown(void);

// Force an immediate deep sleep (bypassing the idle countdown). Returns true if
// the code initiated deep sleep (note: the call will not return on success).
bool deepsleep_manager_force_sleep(void);

#ifdef __cplusplus
}
#endif
