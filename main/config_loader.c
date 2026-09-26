/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 */

#include "config_loader.h"
#include "config_parser.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CONFIG_LOADER";

// ── HTTP Buffer ────────────────────────────────────────────────────────────
#define HTTP_BUF_SIZE 4096
static char s_http_buf[HTTP_BUF_SIZE];
static int s_http_len = 0;
static bool s_http_truncated = false;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            // With esp_http_client_perform() the body arrives already de-chunked,
            // so chunked and Content-Length responses are handled the same way.
            int to_copy = evt->data_len;
            if (s_http_len + to_copy >= HTTP_BUF_SIZE - 1) {
                to_copy = HTTP_BUF_SIZE - 1 - s_http_len;
                s_http_truncated = true;
            }
            memcpy(s_http_buf + s_http_len, evt->data, to_copy);
            s_http_len += to_copy;
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

// ── Public API ───────────────────────────────────────────────────────────

bool config_load_from_url(const char *url) {
    ESP_LOGI(TAG, "Download config: %s", url);
    s_http_len = 0;
    s_http_truncated = false;
    memset(s_http_buf, 0, sizeof(s_http_buf));

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true, // for HTTPS with self-signed cert
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        return false;
    }
    if (s_http_truncated) {
        // A truncated document is never valid JSON, so don't pretend to parse it
        ESP_LOGE(TAG, "Config exceeds the %d-byte buffer; increase HTTP_BUF_SIZE.", HTTP_BUF_SIZE);
        return false;
    }

    ESP_LOGI(TAG, "Received %d bytes. Parsing...", s_http_len);
    return config_parse_json(s_http_buf, s_http_len);
}
