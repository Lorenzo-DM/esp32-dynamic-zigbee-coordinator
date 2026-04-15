#include "config_loader.h"
#include "jsmn.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "CONFIG_LOADER";

// ── Global State ──────────────────────────────────────────────────────────
device_config_t g_devices[MAX_DEVICES];
int             g_device_count = 0;

// ── HTTP Buffer ────────────────────────────────────────────────────────────
#define HTTP_BUF_SIZE 4096
static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                int to_copy = evt->data_len;
                if (s_http_len + to_copy >= HTTP_BUF_SIZE - 1)
                    to_copy = HTTP_BUF_SIZE - 1 - s_http_len;
                memcpy(s_http_buf + s_http_len, evt->data, to_copy);
                s_http_len += to_copy;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ── jsmn Helpers ──────────────────────────────────────────────────────────

// Copies a token's text into a C-string buffer
static void tok_str(const char *json, const jsmntok_t *t, char *out, size_t out_len) {
    int len = t->end - t->start;
    if (len >= (int)out_len) len = (int)out_len - 1;
    memcpy(out, json + t->start, len);
    out[len] = '\0';
}

// Compares a token with a string literal
static int tok_eq(const char *json, const jsmntok_t *t, const char *s) {
    int len = t->end - t->start;
    return (int)strlen(s) == len && strncmp(json + t->start, s, len) == 0;
}

// ── "HH:MM" Time Slot Parser ──────────────────────────────────────────
static bool parse_time(const char *s, int *hour, int *min) {
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return false;
    *hour = h; *min = m;
    return true;
}

// ── IEEE String Parser (from "00124b..." to little-endian uint8_t[8]) ─────────
static bool parse_ieee(const char *str, uint8_t *ieee_out) {
    if (strcmp(str, "-1") == 0) return false;
    
    int nibble_idx = 0;
    uint8_t out[8] = {0};
    
    for (int i = 0; str[i] && nibble_idx < 16; i++) {
        char c = str[i];
        if (c == '0' && str[i+1] == 'x' && nibble_idx == 0) { i++; continue; }
        if (c == ':') continue;
        
        int val = -1;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        
        if (val != -1) {
            // Little endian: the first byte of the string (MSB) goes to index 7
            int byte_idx = 7 - (nibble_idx / 2); 
            if (nibble_idx % 2 == 0) {
                out[byte_idx] = (val << 4);
            } else {
                out[byte_idx] |= val;
            }
            nibble_idx++;
        }
    }
    
    if (nibble_idx == 16) {
        memcpy(ieee_out, out, 8);
        return true;
    }
    return false;
}

// ── Main JSON Parser ────────────────────────────────────────────────
static bool parse_json(const char *json, int len) {
    #define MAX_TOK 512
    static jsmntok_t toks[MAX_TOK];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, len, toks, MAX_TOK);
    if (r < 0) {
        ESP_LOGE(TAG, "jsmn parse error: %d", r);
        return false;
    }

    g_device_count = 0;

    // Search for "devices" key at root level
    int devices_arr = -1;
    for (int i = 1; i < r - 1; i++) {
        if (toks[i].type == JSMN_STRING && tok_eq(json, &toks[i], "devices")) {
            if (toks[i + 1].type == JSMN_ARRAY) {
                devices_arr = i + 1;
                break;
            }
        }
    }
    if (devices_arr < 0) {
        ESP_LOGE(TAG, "'devices' field not found in JSON");
        return false;
    }

    int num_devices = toks[devices_arr].size;
    int idx = devices_arr + 1; // first element of the array

    for (int d = 0; d < num_devices && g_device_count < MAX_DEVICES; d++) {
        if (toks[idx].type != JSMN_OBJECT) { idx++; continue; }

        device_config_t *dev = &g_devices[g_device_count];
        memset(dev, 0, sizeof(*dev));
        dev->temp_high = 2100;
        dev->temp_low  = 1600;

        int obj_size = toks[idx].size;
        idx++; // enter the object

        for (int f = 0; f < obj_size; f++) {
            if (idx >= r) break;
            if (toks[idx].type != JSMN_STRING) { idx += 2; continue; }

            // ── name ──
            if (tok_eq(json, &toks[idx], "name")) {
                idx++;
                tok_str(json, &toks[idx], dev->name, MAX_NAME_LEN);
                idx++;
            }
            // ── ieee ──
            else if (tok_eq(json, &toks[idx], "ieee")) {
                idx++;
                char ieee_s[32];
                tok_str(json, &toks[idx], ieee_s, sizeof(ieee_s));
                dev->ieee_known = parse_ieee(ieee_s, dev->ieee_addr);
                idx++;
            }
            // ── enabled ──
            else if (tok_eq(json, &toks[idx], "enabled")) {
                idx++;
                dev->enabled = tok_eq(json, &toks[idx], "true");
                idx++;
            }
            // ── config (nested object) ──
            else if (tok_eq(json, &toks[idx], "config")) {
                idx++;
                if (toks[idx].type != JSMN_OBJECT) { idx++; continue; }
                int cfg_size = toks[idx].size;
                idx++;
                for (int cf = 0; cf < cfg_size; cf++) {
                    if (idx >= r) break;
                    if (toks[idx].type != JSMN_STRING) { idx += 2; continue; }

                    // temp_high
                    if (tok_eq(json, &toks[idx], "temp_high")) {
                        idx++;
                        char tmp[16]; tok_str(json, &toks[idx], tmp, sizeof(tmp));
                        dev->temp_high = (int16_t)atoi(tmp);
                        idx++;
                    }
                    // temp_low
                    else if (tok_eq(json, &toks[idx], "temp_low")) {
                        idx++;
                        char tmp[16]; tok_str(json, &toks[idx], tmp, sizeof(tmp));
                        dev->temp_low = (int16_t)atoi(tmp);
                        idx++;
                    }
                    // schedule (array of {start, end})
                    else if (tok_eq(json, &toks[idx], "schedule")) {
                        idx++;
                        if (toks[idx].type != JSMN_ARRAY) { idx++; continue; }
                        int sched_count = toks[idx].size;
                        idx++;
                        for (int s = 0; s < sched_count && dev->schedule_count < MAX_SCHEDULES; s++) {
                            if (toks[idx].type != JSMN_OBJECT) { idx++; continue; }
                            int sobj_size = toks[idx].size;
                            idx++;
                            schedule_entry_t *e = &dev->schedule[dev->schedule_count];
                            for (int sf = 0; sf < sobj_size; sf++) {
                                if (idx >= r) break;
                                char tval[16];
                                if (tok_eq(json, &toks[idx], "start")) {
                                    idx++;
                                    tok_str(json, &toks[idx], tval, sizeof(tval));
                                    parse_time(tval, &e->start_hour, &e->start_min);
                                    idx++;
                                } else if (tok_eq(json, &toks[idx], "end")) {
                                    idx++;
                                    tok_str(json, &toks[idx], tval, sizeof(tval));
                                    parse_time(tval, &e->end_hour, &e->end_min);
                                    idx++;
                                } else {
                                    idx += 2;
                                }
                            }
                            dev->schedule_count++;
                        }
                    }
                    else {
                        idx += 2; // skip unknown field
                    }
                }
            }
            // ── type (skip) ──
            else {
                idx += 2;
            }
        }

        ESP_LOGI(TAG, "  [%d] '%s' ieee=%02x%02x%02x%02x%02x%02x%02x%02x enabled=%s temp_high=%d temp_low=%d scheds=%d",
                 g_device_count, dev->name, 
                 dev->ieee_addr[7], dev->ieee_addr[6], dev->ieee_addr[5], dev->ieee_addr[4],
                 dev->ieee_addr[3], dev->ieee_addr[2], dev->ieee_addr[1], dev->ieee_addr[0],
                 dev->enabled ? "YES" : "NO",
                 dev->temp_high, dev->temp_low, dev->schedule_count);

        g_device_count++;
    }

    return g_device_count > 0;
}

// ── Public API ───────────────────────────────────────────────────────────

bool config_load_from_url(const char *url) {
    ESP_LOGI(TAG, "Download config: %s", url);
    s_http_len = 0;
    memset(s_http_buf, 0, sizeof(s_http_buf));

    esp_http_client_config_t cfg = {
        .url            = url,
        .event_handler  = http_event_handler,
        .timeout_ms     = 10000,
        .skip_cert_common_name_check = true,  // for HTTPS with self-signed cert
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

    ESP_LOGI(TAG, "Received %d bytes. Parsing...", s_http_len);
    return parse_json(s_http_buf, s_http_len);
}

int16_t config_get_current_temp(const device_config_t *dev, int hour, int min) {
    int cur = hour * 60 + min;
    for (int i = 0; i < dev->schedule_count; i++) {
        const schedule_entry_t *e = &dev->schedule[i];
        int start = e->start_hour * 60 + e->start_min;
        int end   = e->end_hour   * 60 + e->end_min;
        bool in_range;
        if (end > start) {
            // normal interval (e.g. 05:00–08:00)
            in_range = cur >= start && cur < end;
        } else {
            // overnight interval (e.g. 23:00–00:30)
            in_range = cur >= start || cur < end;
        }
        if (in_range) return dev->temp_high;
    }
    return dev->temp_low;
}

device_config_t *config_find_and_connect(const uint8_t *ieee_addr, uint16_t zb_short_addr) {
    ESP_LOGI(TAG, "--- DEBUG MATCHING ---");
    ESP_LOGI(TAG, "Cerco target IEEE: %02x%02x%02x%02x%02x%02x%02x%02x", 
             ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
             ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);
    ESP_LOGI(TAG, "Dispositivi in JSON: %d", g_device_count);

    for (int i = 0; i < g_device_count; i++) {
        ESP_LOGI(TAG, "  -> [%d] '%s' -- known=%d -- IEEE conf: %02x%02x%02x%02x%02x%02x%02x%02x",
                 i, g_devices[i].name, g_devices[i].ieee_known,
                 g_devices[i].ieee_addr[7], g_devices[i].ieee_addr[6], g_devices[i].ieee_addr[5], g_devices[i].ieee_addr[4],
                 g_devices[i].ieee_addr[3], g_devices[i].ieee_addr[2], g_devices[i].ieee_addr[1], g_devices[i].ieee_addr[0]);

        if (g_devices[i].ieee_known && memcmp(g_devices[i].ieee_addr, ieee_addr, 8) == 0) {
            ESP_LOGI(TAG, "  -> MATCH TROVATO!");
            g_devices[i].zb_short_addr = zb_short_addr;
            g_devices[i].connected = true;
            return &g_devices[i];
        }
    }
    ESP_LOGI(TAG, "----------------------");
    return NULL;
}
