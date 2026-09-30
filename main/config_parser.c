/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 */

#include "config_parser.h"
#include "jsmn.h"
#include "app_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "CONFIG_PARSER";

// ── Global State ──────────────────────────────────────────────────────────
device_config_t g_devices[MAX_DEVICES];
int g_device_count = 0;

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
bool config_parse_time(const char *s, int *hour, int *min) {
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *hour = h;
    *min = m;
    return true;
}

// ── IEEE String Parser (from "00124b..." to little-endian uint8_t[8]) ─────────
bool config_parse_ieee(const char *str, uint8_t *ieee_out) {
    if (strcmp(str, "-1") == 0) return false;

    int nibble_idx = 0;
    uint8_t out[8] = {0};

    for (int i = 0; str[i] && nibble_idx < 16; i++) {
        char c = str[i];
        if (c == '0' && str[i + 1] == 'x' && nibble_idx == 0) {
            i++;
            continue;
        }
        if (c == ':') continue;

        int val = -1;
        if (c >= '0' && c <= '9')
            val = c - '0';
        else if (c >= 'a' && c <= 'f')
            val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            val = c - 'A' + 10;

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
#define MAX_TOK 512

// Returns the index just past the token at `idx` and its whole subtree.
// jsmn sets `size` to the number of direct children (an object counts its
// keys, and each key has its value as a single child), so summing sizes
// visits every descendant exactly once.
static int tok_skip(const jsmntok_t *toks, int idx, int r) {
    int pending = 1;
    while (pending > 0 && idx < r) {
        pending += toks[idx].size;
        pending--;
        idx++;
    }
    return idx;
}

// Parses an integer setpoint, keeping *out unchanged if it is out of range.
static void parse_temp(const char *json, const jsmntok_t *t, const char *field, const char *dev_name,
                       int16_t *out) {
    char tmp[16];
    tok_str(json, t, tmp, sizeof(tmp));
    int v = atoi(tmp);
    if (v >= TEMP_MIN_CENTI && v <= TEMP_MAX_CENTI) {
        *out = (int16_t)v;
    } else {
        APP_LOGW(TAG, "  '%s': %s=%d out of range [%d,%d], keeping default %d",
                 dev_name, field, v, TEMP_MIN_CENTI, TEMP_MAX_CENTI, *out);
    }
}

// Parses one {"start": "HH:MM", "end": "HH:MM"} object at `obj`.
static bool parse_slot(const char *json, const jsmntok_t *toks, int r, int obj, schedule_entry_t *e) {
    bool have_start = false, have_end = false;
    int idx = obj + 1;
    for (int f = 0; f < toks[obj].size && idx + 1 < r; f++) {
        const jsmntok_t *val = &toks[idx + 1];
        char tval[16];
        if (tok_eq(json, &toks[idx], "start")) {
            tok_str(json, val, tval, sizeof(tval));
            have_start = config_parse_time(tval, &e->start_hour, &e->start_min);
        } else if (tok_eq(json, &toks[idx], "end")) {
            tok_str(json, val, tval, sizeof(tval));
            have_end = config_parse_time(tval, &e->end_hour, &e->end_min);
        }
        idx = tok_skip(toks, idx, r); // key + value subtree
    }
    return have_start && have_end;
}

// Parses the "schedule" array at `arr` into dev->schedule.
static void parse_schedule(const char *json, const jsmntok_t *toks, int r, int arr, device_config_t *dev) {
    int idx = arr + 1;
    for (int s = 0; s < toks[arr].size && idx < r && dev->schedule_count < MAX_SCHEDULES; s++) {
        if (toks[idx].type == JSMN_OBJECT) {
            schedule_entry_t e = {0};
            if (parse_slot(json, toks, r, idx, &e)) {
                dev->schedule[dev->schedule_count++] = e;
            } else {
                APP_LOGW(TAG, "  '%s': skipping schedule slot with invalid/missing time", dev->name);
            }
        }
        idx = tok_skip(toks, idx, r);
    }
}

// Parses the nested "config" object at `obj`.
static void parse_device_config(const char *json, const jsmntok_t *toks, int r, int obj, device_config_t *dev) {
    int idx = obj + 1;
    for (int f = 0; f < toks[obj].size && idx + 1 < r; f++) {
        const jsmntok_t *val = &toks[idx + 1];
        if (tok_eq(json, &toks[idx], "temp_high")) {
            parse_temp(json, val, "temp_high", dev->name, &dev->temp_high);
        } else if (tok_eq(json, &toks[idx], "temp_low")) {
            parse_temp(json, val, "temp_low", dev->name, &dev->temp_low);
        } else if (tok_eq(json, &toks[idx], "schedule") && val->type == JSMN_ARRAY) {
            parse_schedule(json, toks, r, idx + 1, dev);
        }
        idx = tok_skip(toks, idx, r); // key + value subtree (also skips unknown fields)
    }
}

// Parses one device object at `obj` into dev.
static void parse_device(const char *json, const jsmntok_t *toks, int r, int obj, device_config_t *dev) {
    memset(dev, 0, sizeof(*dev));
    dev->temp_high = 2100;
    dev->temp_low = 1600;

    int idx = obj + 1;
    for (int f = 0; f < toks[obj].size && idx + 1 < r; f++) {
        const jsmntok_t *val = &toks[idx + 1];
        if (tok_eq(json, &toks[idx], "name")) {
            tok_str(json, val, dev->name, MAX_NAME_LEN);
        } else if (tok_eq(json, &toks[idx], "ieee")) {
            char ieee_s[32];
            tok_str(json, val, ieee_s, sizeof(ieee_s));
            dev->ieee_known = config_parse_ieee(ieee_s, dev->ieee_addr);
        } else if (tok_eq(json, &toks[idx], "enabled")) {
            dev->enabled = tok_eq(json, val, "true");
        } else if (tok_eq(json, &toks[idx], "config") && val->type == JSMN_OBJECT) {
            parse_device_config(json, toks, r, idx + 1, dev);
        }
        idx = tok_skip(toks, idx, r); // key + value subtree (also skips unknown fields)
    }
}

bool config_parse_json(const char *json, int len) {
    static jsmntok_t toks[MAX_TOK];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, len, toks, MAX_TOK);
    if (r < 0) {
        APP_LOGE(TAG, "jsmn parse error: %d (config too large for MAX_TOK=%d?)", r, MAX_TOK);
        return false;
    }

    g_device_count = 0;

    if (r < 1 || toks[0].type != JSMN_OBJECT) {
        APP_LOGE(TAG, "JSON root is not an object");
        return false;
    }

    // Search for "devices" key among the root object's direct keys only
    int devices_arr = -1;
    int idx = 1;
    for (int f = 0; f < toks[0].size && idx + 1 < r; f++) {
        if (tok_eq(json, &toks[idx], "devices") && toks[idx + 1].type == JSMN_ARRAY) {
            devices_arr = idx + 1;
            break;
        }
        idx = tok_skip(toks, idx, r);
    }
    if (devices_arr < 0) {
        APP_LOGE(TAG, "'devices' field not found in JSON");
        return false;
    }

    idx = devices_arr + 1; // first element of the array
    for (int d = 0; d < toks[devices_arr].size && idx < r && g_device_count < MAX_DEVICES; d++) {
        if (toks[idx].type == JSMN_OBJECT) {
            device_config_t *dev = &g_devices[g_device_count];
            parse_device(json, toks, r, idx, dev);

            APP_LOGI(TAG,
                     "  [%d] '%s' ieee=%02x%02x%02x%02x%02x%02x%02x%02x enabled=%s temp_high=%d temp_low=%d scheds=%d",
                     g_device_count, dev->name,
                     dev->ieee_addr[7], dev->ieee_addr[6], dev->ieee_addr[5], dev->ieee_addr[4],
                     dev->ieee_addr[3], dev->ieee_addr[2], dev->ieee_addr[1], dev->ieee_addr[0],
                     dev->enabled ? "YES" : "NO",
                     dev->temp_high, dev->temp_low, dev->schedule_count);

            g_device_count++;
        }
        idx = tok_skip(toks, idx, r);
    }

    return g_device_count > 0;
}

// ── Schedule evaluation & device matching ─────────────────────────────────

int16_t config_get_current_temp(const device_config_t *dev, int hour, int min) {
    int cur = hour * 60 + min;
    for (int i = 0; i < dev->schedule_count; i++) {
        const schedule_entry_t *e = &dev->schedule[i];
        int start = e->start_hour * 60 + e->start_min;
        int end = e->end_hour * 60 + e->end_min;
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
    APP_LOGD(TAG, "Matching target IEEE %02x%02x%02x%02x%02x%02x%02x%02x against %d configured device(s)",
             ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
             ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0], g_device_count);

    for (int i = 0; i < g_device_count; i++) {
        APP_LOGD(TAG, "  -> [%d] '%s' known=%d ieee=%02x%02x%02x%02x%02x%02x%02x%02x",
                 i, g_devices[i].name, g_devices[i].ieee_known,
                 g_devices[i].ieee_addr[7], g_devices[i].ieee_addr[6], g_devices[i].ieee_addr[5], g_devices[i].ieee_addr[4],
                 g_devices[i].ieee_addr[3], g_devices[i].ieee_addr[2], g_devices[i].ieee_addr[1], g_devices[i].ieee_addr[0]);

        if (g_devices[i].ieee_known && memcmp(g_devices[i].ieee_addr, ieee_addr, 8) == 0) {
            g_devices[i].zb_short_addr = zb_short_addr;
            g_devices[i].connected = true;
            return &g_devices[i];
        }
    }
    return NULL;
}
