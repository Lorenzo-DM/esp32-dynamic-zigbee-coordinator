/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 *
 * Host-side unit tests for the pure configuration logic (config_parser.c).
 * No ESP-IDF dependency: built with -DHOST_TEST and run via ctest.
 */

#include "config_parser.h"
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);                 \
        }                                                                      \
    } while (0)

// ── parse_time ────────────────────────────────────────────────────────────
static void test_parse_time(void) {
    printf("test_parse_time\n");
    int h, m;
    CHECK(config_parse_time("07:30", &h, &m) && h == 7 && m == 30);
    CHECK(config_parse_time("00:00", &h, &m) && h == 0 && m == 0);
    CHECK(config_parse_time("23:59", &h, &m) && h == 23 && m == 59);
    CHECK(!config_parse_time("24:00", &h, &m));   // hour out of range
    CHECK(!config_parse_time("12:60", &h, &m));   // minute out of range
    CHECK(!config_parse_time("-1:00", &h, &m));   // negative
    CHECK(!config_parse_time("abc", &h, &m));     // malformed
    CHECK(!config_parse_time("7", &h, &m));       // missing minutes
}

// ── parse_ieee ────────────────────────────────────────────────────────────
static void test_parse_ieee(void) {
    printf("test_parse_ieee\n");
    uint8_t out[8];
    // Little-endian: first string byte (0x00) lands at index 7.
    const uint8_t expect[8] = {0x56, 0x34, 0x12, 0x2a, 0x00, 0x4b, 0x12, 0x00};

    CHECK(config_parse_ieee("00124b002a123456", out) && memcmp(out, expect, 8) == 0);
    CHECK(config_parse_ieee("0x00124b002a123456", out) && memcmp(out, expect, 8) == 0);
    CHECK(config_parse_ieee("00:12:4b:00:2a:12:34:56", out) && memcmp(out, expect, 8) == 0);
    CHECK(!config_parse_ieee("-1", out));          // unknown marker
    CHECK(!config_parse_ieee("1234", out));        // too short
    CHECK(!config_parse_ieee("", out));            // empty
}

// ── config_get_current_temp ────────────────────────────────────────────────
static device_config_t make_dev(void) {
    device_config_t d;
    memset(&d, 0, sizeof(d));
    d.temp_high = 2100;
    d.temp_low  = 1600;
    return d;
}

static void test_current_temp(void) {
    printf("test_current_temp\n");

    // Normal slot 07:00-09:00
    device_config_t d = make_dev();
    d.schedule[0] = (schedule_entry_t){7, 0, 9, 0};
    d.schedule_count = 1;
    CHECK(config_get_current_temp(&d, 8, 0)  == 2100);  // inside
    CHECK(config_get_current_temp(&d, 6, 59) == 1600);  // before
    CHECK(config_get_current_temp(&d, 7, 0)  == 2100);  // boundary: start inclusive
    CHECK(config_get_current_temp(&d, 9, 0)  == 1600);  // boundary: end exclusive
    CHECK(config_get_current_temp(&d, 12, 0) == 1600);  // after

    // Overnight slot 23:00-06:00
    device_config_t n = make_dev();
    n.schedule[0] = (schedule_entry_t){23, 0, 6, 0};
    n.schedule_count = 1;
    CHECK(config_get_current_temp(&n, 23, 30) == 2100);  // late evening
    CHECK(config_get_current_temp(&n, 5, 0)   == 2100);  // early morning
    CHECK(config_get_current_temp(&n, 6, 0)   == 1600);  // boundary end exclusive
    CHECK(config_get_current_temp(&n, 12, 0)  == 1600);  // midday

    // Empty schedule -> always low
    device_config_t e = make_dev();
    CHECK(config_get_current_temp(&e, 8, 0) == 1600);
}

// ── config_parse_json ──────────────────────────────────────────────────────
static void test_parse_json_valid(void) {
    printf("test_parse_json_valid\n");
    const char *json =
        "{\"devices\":[{"
        "\"name\":\"Living Room\","
        "\"ieee\":\"00124b002a123456\","
        "\"enabled\":true,"
        "\"config\":{\"temp_high\":2100,\"temp_low\":1700,"
        "\"schedule\":[{\"start\":\"07:00\",\"end\":\"09:00\"},"
        "{\"start\":\"18:00\",\"end\":\"22:00\"}]}}]}";

    CHECK(config_parse_json(json, (int)strlen(json)) == true);
    CHECK(g_device_count == 1);
    CHECK(strcmp(g_devices[0].name, "Living Room") == 0);
    CHECK(g_devices[0].ieee_known == true);
    CHECK(g_devices[0].enabled == true);
    CHECK(g_devices[0].temp_high == 2100);
    CHECK(g_devices[0].temp_low == 1700);
    CHECK(g_devices[0].schedule_count == 2);
    CHECK(g_devices[0].schedule[1].start_hour == 18 && g_devices[0].schedule[1].end_hour == 22);
}

static void test_parse_json_invalid_temp(void) {
    printf("test_parse_json_invalid_temp\n");
    // temp_high above range and temp_low below range -> fall back to defaults.
    const char *json =
        "{\"devices\":[{"
        "\"name\":\"Bad\",\"ieee\":\"-1\",\"enabled\":false,"
        "\"config\":{\"temp_high\":9999,\"temp_low\":100,\"schedule\":[]}}]}";

    CHECK(config_parse_json(json, (int)strlen(json)) == true);
    CHECK(g_device_count == 1);
    CHECK(g_devices[0].ieee_known == false);       // "-1" -> unknown
    CHECK(g_devices[0].temp_high == 2100);         // default kept
    CHECK(g_devices[0].temp_low == 1600);          // default kept
}

static void test_parse_json_bad_slot(void) {
    printf("test_parse_json_bad_slot\n");
    // First slot has an out-of-range start and must be skipped.
    const char *json =
        "{\"devices\":[{"
        "\"name\":\"Skip\",\"ieee\":\"00124b002a123456\",\"enabled\":true,"
        "\"config\":{\"temp_high\":2000,\"temp_low\":1500,"
        "\"schedule\":[{\"start\":\"25:00\",\"end\":\"09:00\"},"
        "{\"start\":\"18:00\",\"end\":\"22:00\"}]}}]}";

    CHECK(config_parse_json(json, (int)strlen(json)) == true);
    CHECK(g_device_count == 1);
    CHECK(g_devices[0].schedule_count == 1);       // bad slot dropped
    CHECK(g_devices[0].schedule[0].start_hour == 18);
}

static void test_parse_json_no_devices(void) {
    printf("test_parse_json_no_devices\n");
    const char *json = "{\"foo\":42}";
    CHECK(config_parse_json(json, (int)strlen(json)) == false);
    CHECK(g_device_count == 0);
}

static void test_parse_json_nested_unknown_object(void) {
    printf("test_parse_json_nested_unknown_object\n");
    // An unknown field holding an object must be skipped as a whole subtree.
    const char *json =
        "{\"devices\":[{"
        "\"name\":\"A\",\"meta\":{\"room\":\"x\",\"floor\":1},"
        "\"ieee\":\"00124b002a123456\",\"enabled\":true,"
        "\"config\":{\"temp_high\":2200,\"temp_low\":1500,"
        "\"schedule\":[{\"start\":\"07:00\",\"end\":\"09:00\"}]}},"
        "{\"name\":\"B\",\"ieee\":\"00124b002a654321\",\"enabled\":true,"
        "\"config\":{\"temp_high\":2000,\"temp_low\":1600}}]}";

    CHECK(config_parse_json(json, (int)strlen(json)) == true);
    CHECK(g_device_count == 2);
    CHECK(g_devices[0].enabled == true);
    CHECK(g_devices[0].ieee_known == true);
    CHECK(g_devices[0].temp_high == 2200);
    CHECK(g_devices[0].schedule_count == 1);
    CHECK(strcmp(g_devices[1].name, "B") == 0);
    CHECK(g_devices[1].temp_high == 2000);
}

static void test_parse_json_nested_unknown_array(void) {
    printf("test_parse_json_nested_unknown_array\n");
    // Unknown arrays/objects at every level: device, config and schedule slot.
    const char *json =
        "{\"devices\":[{"
        "\"tags\":[1,[2,3],{\"k\":\"v\"}],\"name\":\"A\","
        "\"ieee\":\"00124b002a123456\",\"enabled\":true,"
        "\"config\":{\"extra\":{\"a\":[1,2]},\"temp_high\":2300,"
        "\"schedule\":[{\"note\":[\"x\"],\"start\":\"06:00\",\"end\":\"08:00\"}]}}]}";

    CHECK(config_parse_json(json, (int)strlen(json)) == true);
    CHECK(g_device_count == 1);
    CHECK(strcmp(g_devices[0].name, "A") == 0);
    CHECK(g_devices[0].ieee_known == true);
    CHECK(g_devices[0].enabled == true);
    CHECK(g_devices[0].temp_high == 2300);
    CHECK(g_devices[0].schedule_count == 1);
    CHECK(g_devices[0].schedule[0].start_hour == 6);
}

static void test_parse_json_non_object_array_elements(void) {
    printf("test_parse_json_non_object_array_elements\n");
    // Non-object entries (including nested containers) in "devices" and
    // "schedule" are skipped without desynchronizing the following entries.
    const char *json =
        "{\"devices\":[[1,{\"name\":\"X\"}],\"junk\",{"
        "\"name\":\"A\",\"enabled\":true,"
        "\"config\":{\"schedule\":[[\"07:00\"],{\"start\":\"18:00\",\"end\":\"22:00\"}]}}]}";

    CHECK(config_parse_json(json, (int)strlen(json)) == true);
    CHECK(g_device_count == 1);
    CHECK(strcmp(g_devices[0].name, "A") == 0);
    CHECK(g_devices[0].schedule_count == 1);
    CHECK(g_devices[0].schedule[0].start_hour == 18);
}

static void test_parse_json_devices_only_at_root(void) {
    printf("test_parse_json_devices_only_at_root\n");
    // A nested "devices" key must not be mistaken for the root one.
    const char *json =
        "{\"other\":{\"devices\":[{\"name\":\"EVIL\",\"enabled\":true}]},"
        "\"devices\":[{\"name\":\"REAL\"}]}";

    CHECK(config_parse_json(json, (int)strlen(json)) == true);
    CHECK(g_device_count == 1);
    CHECK(strcmp(g_devices[0].name, "REAL") == 0);

    const char *only_nested = "{\"other\":{\"devices\":[{\"name\":\"EVIL\"}]}}";
    CHECK(config_parse_json(only_nested, (int)strlen(only_nested)) == false);
}

// ── config_find_and_connect ────────────────────────────────────────────────
static void test_find_and_connect(void) {
    printf("test_find_and_connect\n");
    const char *json =
        "{\"devices\":[{"
        "\"name\":\"Living Room\",\"ieee\":\"00124b002a123456\",\"enabled\":true,"
        "\"config\":{\"temp_high\":2100,\"temp_low\":1700,\"schedule\":[]}}]}";
    config_parse_json(json, (int)strlen(json));

    const uint8_t ieee[8] = {0x56, 0x34, 0x12, 0x2a, 0x00, 0x4b, 0x12, 0x00};
    device_config_t *m = config_find_and_connect(ieee, 0x1234);
    CHECK(m != NULL);
    CHECK(m->connected == true);
    CHECK(m->zb_short_addr == 0x1234);

    const uint8_t other[8] = {0xff, 0, 0, 0, 0, 0, 0, 0};
    CHECK(config_find_and_connect(other, 0x9999) == NULL);
}

int main(void) {
    test_parse_time();
    test_parse_ieee();
    test_current_temp();
    test_parse_json_valid();
    test_parse_json_invalid_temp();
    test_parse_json_bad_slot();
    test_parse_json_no_devices();
    test_parse_json_nested_unknown_object();
    test_parse_json_nested_unknown_array();
    test_parse_json_non_object_array_elements();
    test_parse_json_devices_only_at_root();
    test_find_and_connect();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
