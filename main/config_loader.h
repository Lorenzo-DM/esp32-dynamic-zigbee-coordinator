/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_DEVICES    8
#define MAX_SCHEDULES  8
#define MAX_NAME_LEN   32

// A time slot: HIGH heating from "start" to "end"
typedef struct {
    int start_hour;
    int start_min;
    int end_hour;
    int end_min;
} schedule_entry_t;

// A valve read from JSON
typedef struct {
    char     name[MAX_NAME_LEN];
    uint8_t  ieee_addr[8];        // 64-bit IEEE address (MAC address)
    bool     ieee_known;          // false if ieee == "-1"
    bool     enabled;
    int16_t  temp_high;           // high temperature (active slot)
    int16_t  temp_low;            // low temperature (outside slots)
    schedule_entry_t schedule[MAX_SCHEDULES];
    int      schedule_count;
    uint16_t zb_short_addr;       // set when the valve connects
    bool     connected;
} device_config_t;

// Global array of devices loaded from JSON
extern device_config_t g_devices[MAX_DEVICES];
extern int             g_device_count;

/**
 * Downloads JSON from URL and populates g_devices / g_device_count.
 * To be called while WiFi is active.
 * @return true if at least one device was loaded.
 */
bool config_load_from_url(const char *url);

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
