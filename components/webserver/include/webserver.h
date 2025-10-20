/*
 * webserver.h
 *
 * Minimal HTTP server API used by the application.
 */

#ifndef MAIN_WEBSERVER_H_
#define MAIN_WEBSERVER_H_

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WEBSERVER_POST_EVENT BIT0

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Handle returned by webserver_start(). Caller owns and must free it via
 * webserver_stop().
 */
struct webserver_handle {
    httpd_handle_t httpd_handle; /* HTTP server handle */
    char *index_path;            /* path to index.html served for GET / */
    char *config_path;           /* path to persist wifi config */
    EventGroupHandle_t event_group; /* event group to signal POST completion */
};

struct webserver_handle* webserver_start(const char *index_path, const char *config_path);
void webserver_stop(struct webserver_handle *handle);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_WEBSERVER_H_ */