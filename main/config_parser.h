/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 */

/*
 * Pure, portable configuration logic: JSON parsing, IEEE/time parsing,
 * schedule evaluation and device matching. Depends only on jsmn + app_log,
 * so it can be unit-tested on a host machine without the ESP-IDF.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_DEVICES 8
#define MAX_SCHEDULES 8
#define MAX_NAME_LEN 32

/* Accepted setpoint range (1/100 °C). Values outside are rejected as invalid. */
#define TEMP_MIN_CENTI 500  /*  5.0 °C */
#define TEMP_MAX_CENTI 3500 /* 35.0 °C */

// A time slot: HIGH heating from "start" to "end"
typedef struct {
    int start_hour;
    int start_min;
    int end_hour;
    int end_min;
} schedule_entry_t;

// A valve read from JSON
typedef struct {
    char name[MAX_NAME_LEN];
    uint8_t ieee_addr[8]; // 64-bit IEEE address (MAC address)
    bool ieee_known;      // false if ieee == "-1"
    bool enabled;
    int16_t temp_high; // high temperature (active slot)
    int16_t temp_low;  // low temperature (outside slots)
    schedule_entry_t schedule[MAX_SCHEDULES];
    int schedule_count;
    uint16_t zb_short_addr; // set when the valve connects
    bool connected;
} device_config_t;

// Global array of devices loaded from JSON
extern device_config_t g_devices[MAX_DEVICES];
extern int g_device_count;

/**
 * Parses "HH:MM" into hour/min. Rejects out-of-range values (hour 0-23, min 0-59).
 * @return true on success.
 */
bool config_parse_time(const char *s, int *hour, int *min);

/**
 * Parses an IEEE address string ("00124b...", optional "0x" prefix or ':' separators)
 * into a little-endian uint8_t[8]. "-1" means "unknown".
 * @return true if a full 64-bit address was parsed.
 */
bool config_parse_ieee(const char *str, uint8_t *ieee_out);

/**
 * Parses the device list JSON and populates g_devices / g_device_count.
 * @return true if at least one device was loaded.
 */
bool config_parse_json(const char *json, int len);

/**
 * Given the current time, returns the correct temperature for a device.
 * If the time falls within a slot -> temp_high, otherwise -> temp_low.
 */
int16_t config_get_current_temp(const device_config_t *dev, int hour, int min);

/**
 * Searches for a device by IEEE address and updates zb_short_addr + connected.
 * Called from the DEVICE_ANNCE handler.
 * @return pointer to the found device, or NULL.
 */
device_config_t *config_find_and_connect(const uint8_t *ieee_addr, uint16_t zb_short_addr);
