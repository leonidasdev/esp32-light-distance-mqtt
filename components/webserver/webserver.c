/**
 * webserver.c
 *
 * Small HTTP server used to present an index page and accept a config POST
 * (ssid/password). The implementation favours clarity and safe error
 * handling (no crashing asserts on malformed requests).
 */

#include "webserver.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "persistence.h"

static const char *TAG = "webserver";

static esp_err_t webserver_index_handler(httpd_req_t *req);
static esp_err_t webserver_update_handler(httpd_req_t *req);

struct webserver_handle *webserver_start(const char *index_path, const char *config_path)
{
    if (index_path == NULL || config_path == NULL) {
        ESP_LOGE(TAG, "webserver_start called with NULL path");
        return NULL;
    }

    ESP_LOGI(TAG, "Starting webserver...");

    httpd_handle_t server = NULL;
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();

    esp_err_t err = httpd_start(&server, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %d", err);
        return NULL;
    }

    struct webserver_handle *webserver_handle = calloc(1, sizeof(*webserver_handle));
    if (webserver_handle == NULL) {
        httpd_stop(server);
        return NULL;
    }

    webserver_handle->httpd_handle = server;
    webserver_handle->index_path = strdup(index_path);
    webserver_handle->config_path = strdup(config_path);
    webserver_handle->event_group = xEventGroupCreate();

    if (webserver_handle->index_path == NULL || webserver_handle->config_path == NULL || webserver_handle->event_group == NULL) {
        ESP_LOGE(TAG, "Allocation failed in webserver_start");
        if (webserver_handle->index_path) free(webserver_handle->index_path);
        if (webserver_handle->config_path) free(webserver_handle->config_path);
        if (webserver_handle->event_group) vEventGroupDelete(webserver_handle->event_group);
        free(webserver_handle);
        httpd_stop(server);
        return NULL;
    }

    httpd_uri_t get_handler = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = webserver_index_handler,
        .user_ctx = webserver_handle,
    };
    httpd_uri_t post_handler = {
        .uri = "/change_config",
        .method = HTTP_POST,
        .handler = webserver_update_handler,
        .user_ctx = webserver_handle,
    };

    httpd_register_uri_handler(server, &get_handler);
    httpd_register_uri_handler(server, &post_handler);

    ESP_LOGI(TAG, "Webserver started");
    return webserver_handle;
}

void webserver_stop(struct webserver_handle *handle)
{
    if (handle == NULL) return;
    if (handle->httpd_handle) httpd_stop(handle->httpd_handle);
    if (handle->index_path) free(handle->index_path);
    if (handle->config_path) free(handle->config_path);
    if (handle->event_group) vEventGroupDelete(handle->event_group);
    free(handle);
}

static esp_err_t webserver_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    struct webserver_handle *ctx = req->user_ctx;
    if (ctx == NULL || ctx->index_path == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *index_file = fopen(ctx->index_path, "r");
    if (index_file == NULL)
    {
        ESP_LOGE(TAG, "Error reading index file '%s'", ctx->index_path);
        httpd_resp_send_500(req);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(index_file, 0, SEEK_END) != 0) {
        fclose(index_file);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    long index_file_size = ftell(index_file);
    if (index_file_size < 0) {
        fclose(index_file);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    rewind(index_file);

    char *index = malloc((size_t)index_file_size + 1);
    if (index == NULL) {
        fclose(index_file);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(index, 1, (size_t)index_file_size, index_file);
    fclose(index_file);
    if (read != (size_t)index_file_size) {
        free(index);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    index[index_file_size] = '\0';

    ESP_LOGI(TAG, "GET %s", req->uri);
    httpd_resp_send(req, index, (int)index_file_size);

    free(index);
    return ESP_OK;
}

static esp_err_t webserver_update_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    struct webserver_handle *ctx = req->user_ctx;
    if (ctx == NULL || ctx->config_path == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_INVALID_ARG;
    }

    /* Guard against malicious or bogus Content-Length header */
    if (req->content_len <= 0 || req->content_len > 4096) {
        ESP_LOGW(TAG, "Rejected POST with content_len=%d", req->content_len);
        httpd_resp_set_status(req, "413 Content Too Large");
        httpd_resp_send(req, "413", HTTPD_RESP_USE_STRLEN);
        return ESP_ERR_INVALID_ARG;
    }

    char *buffer = malloc((size_t)req->content_len + 1);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "Error allocating buffer of size %d for request", req->content_len);
        httpd_resp_set_status(req, "413 Content Too Large");
        httpd_resp_send(req, "413", HTTPD_RESP_USE_STRLEN);
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < (size_t)req->content_len)
    {
        ssize_t ret = httpd_req_recv(req, buffer + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            ESP_LOGE(TAG, "Timed out while reading POST request");
            free(buffer);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';

    char *ssid = strstr(buffer, "ssid=");
    char *password = strstr(buffer, "password=");
    if (ssid == NULL || password == NULL)
    {
        free(buffer);
        ESP_LOGI(TAG, "POST parameters missing");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "400", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ssid += strlen("ssid=");
    password += strlen("password=");

    ssid[strcspn(ssid, "\r\n")] = '\0';
    password[strcspn(password, "\r\n")] = '\0';

    struct persistence_config config = {
        .ssid = ssid,
        .password = password};
    bool result = persistence_save_config(ctx->config_path, &config);
    free(buffer);

    if (!result)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_status(req, "302");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/?ok");
    httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Configuration saved, signalling event group");

    xEventGroupSetBits(ctx->event_group, WEBSERVER_POST_EVENT);

    return ESP_OK;
}