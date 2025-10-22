/*
 * mqtt.h
 *
 * Lightweight MQTT helper for connecting to ThingsBoard and publishing
 * telemetry. The module uses the esp-mqtt client from ESP-IDF v5.x.
 */

#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the MQTT client with explicit broker URI and access token.
 * uri example: "mqtt://demo.thingsboard.io"
 */
void mqtt_app_start(const char *uri, const char *access_token);

/**
 * Start the MQTT client reading the access token from a file on the filesystem.
 * Returns true when the file was read and client start was attempted.
 */
bool mqtt_app_start_from_file(const char *uri, const char *token_file_path);

/** Stop and cleanup the MQTT client. */
void mqtt_app_stop(void);

/** Publish a telemetry JSON payload to ThingsBoard v1/devices/me/telemetry. */
void mqtt_publish_telemetry(const char *json_payload);

/** Publish client attributes JSON to ThingsBoard v1/devices/me/attributes. */
void mqtt_publish_attributes(const char *json_payload);

/** Return the access token used to start the MQTT client (not NULL once started). */
const char *mqtt_get_access_token(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_H
