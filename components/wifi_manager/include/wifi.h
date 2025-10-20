/*
 * wifi.h
 *
 * Lightweight wrapper API used by the application for WiFi operations.
 * - init_wifi_module(): prepare network stack
 * - set_ap(...): start a Soft-AP
 * - set_station(...): connect to an AP (blocking)
 */

#ifndef MAIN_WIFI_H_
#define MAIN_WIFI_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize networking infra used by this module. Safe to call multiple times.
 */
void init_wifi_module(void);

/**
 * Start a soft-AP with the provided SSID/password and channel (1..14).
 * Passing an empty password will start an open AP.
 */
void set_ap(const char *ssid, const char *password, const int channel);

/**
 * Configure and connect as a WiFi station. Blocks until connected or failed.
 * Returns true on success, false on failure.
 */
bool set_station(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_WIFI_H_ */