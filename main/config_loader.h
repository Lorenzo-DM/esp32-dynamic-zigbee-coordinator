/*
 * Copyright (c) 2026 Lorenzo-DM
 * Licensed under the MIT License. See LICENSE file in the project root for full license information.
 */

#pragma once

// Device types, globals and the pure parsing/matching API.
#include "config_parser.h"

/**
 * Downloads JSON from URL and populates g_devices / g_device_count.
 * To be called while WiFi is active.
 * @return true if at least one device was loaded.
 */
bool config_load_from_url(const char *url);
