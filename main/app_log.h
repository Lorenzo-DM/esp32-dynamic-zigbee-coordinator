/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 */

/*
 * Thin logging shim so that pure, portable logic (config_parser.c) can be
 * compiled and unit-tested on a host machine without the ESP-IDF.
 *
 * On target  : maps to the standard esp_log.h macros.
 * On host    : maps to printf (HOST_TEST defined by the host test CMake).
 */
#pragma once

#ifdef HOST_TEST

#include <stdio.h>

#define APP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define APP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define APP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define APP_LOGD(tag, fmt, ...) printf("D (%s) " fmt "\n", tag, ##__VA_ARGS__)

#else

#include "esp_log.h"

#define APP_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define APP_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define APP_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define APP_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)

#endif
