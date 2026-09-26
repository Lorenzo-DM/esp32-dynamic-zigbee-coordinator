/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 */
#pragma once

#include "esp_zigbee_core.h"
#include "switch_driver.h"

/* Zigbee configuration (see Kconfig.projbuild for tunable values) */
#define MAX_CHILDREN CONFIG_VALVE_ZB_MAX_CHILDREN            /* the max amount of connected devices */
#define INSTALLCODE_POLICY_ENABLE false                      /* enable the install code policy for security */
#define HA_VALVE_REGULATOR_ENDPOINT CONFIG_VALVE_ZB_ENDPOINT /* valve regulator device endpoint */

/* Basic manufacturer information */
#define ESP_MANUFACTURER_NAME "\x09" \
                              "ESPRESSIF"             /* Customized manufacturer name */
#define ESP_MODEL_IDENTIFIER "\x07" CONFIG_IDF_TARGET /* Customized model identifier */

#define ESP_ZB_ZC_CONFIG()                                \
    {                                                     \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,    \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE, \
        .nwk_cfg.zczr_cfg = {                             \
            .max_children = MAX_CHILDREN,                 \
        },                                                \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()       \
    {                                       \
        .radio_mode = ZB_RADIO_MODE_NATIVE, \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                          \
    {                                                         \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }
