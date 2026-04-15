#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "ha_valve_regulator.h"
#include "secrets.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "driver/gpio.h"
#include "config_loader.h"


static const char *TAG = DEVICE_NAME;

// WiFi EventGroup Bits
#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;

// Full JSON config URL
#define CONFIG_JSON_URL  BASE_URL "/raw/branch/main/config.json"

#define BOARD_LED_GPIO   GPIO_NUM_15 

// 0. HELPER: BLINK LED
static void blink_led(int times, int ms_on, int ms_off) {
    for (int i = 0; i < times; i++) {
        gpio_set_level(BOARD_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(ms_on));
        gpio_set_level(BOARD_LED_GPIO, 0);
        if (i < times - 1 && ms_off > 0) {
            vTaskDelay(pdMS_TO_TICKS(ms_off));
        }
    }
}

// 0b. HELPER: RESTORE CACHED CONNECTIONS
static void zb_restore_connections() {
    ESP_LOGI(TAG, "=> Checking for already paired devices in stack cache...");
    int restored = 0;
    for (int i = 0; i < g_device_count; i++) {
        device_config_t *d = &g_devices[i];
        
        // Query the stack for the short address associated with this IEEE
        uint16_t short_addr = esp_zb_address_short_by_ieee(d->ieee_addr);
        
        // 0xFFFE is unknown/missing, 0xFFFF is broadcast/error
        if (short_addr < 0xFFFE) {
            ESP_LOGI(TAG, "   => Found cached device '%s' at 0x%04x. Restoring connection...", d->name, short_addr);
            config_find_and_connect(d->ieee_addr, short_addr);
            restored++;
        }
    }
    if (restored > 0) {
        blink_led(restored, 200, 200); // Visual feedback for restored devices
    }
}

// 0c. HELPER: GET CURRENT TIMESTAMP STRING
static void get_timestamp(char *buf, size_t len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year < (2024 - 1900)) {
        snprintf(buf, len, "[--- NOT SYNCED ---]");
    } else {
        strftime(buf, len, "[%d/%m/%Y %H:%M:%S]", &timeinfo);
    }
}

// 1. FUNCTION TO SEND TEMPERATURE TO A VALVE
static void set_temperature(uint16_t zb_short_addr, int16_t setpoint, const char *name) {
    esp_zb_zcl_write_attr_cmd_t write_req;
    write_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    write_req.zcl_basic_cmd.src_endpoint = HA_VALVE_REGULATOR_ENDPOINT;
    write_req.zcl_basic_cmd.dst_endpoint = 1;  // Sonoff TRV = endpoint 1
    write_req.zcl_basic_cmd.dst_addr_u.addr_short = zb_short_addr;
    write_req.clusterID = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT; // 0x0201

    // IMPORTANT: In the ESP-Zigbee SDK, write_attr is asynchronous.
    // We must use a static variable to ensure the pointer remains valid 
    // until the stack actually sends the packet.
    static int16_t setpoint_val;
    setpoint_val = setpoint;

    esp_zb_zcl_attribute_t attr_field;
    attr_field.id = ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID; // 0x0012
    attr_field.data.type = ESP_ZB_ZCL_ATTR_TYPE_S16;
    attr_field.data.value = &setpoint_val;
    attr_field.data.size = sizeof(int16_t);

    write_req.attr_number = 1;
    write_req.attr_field = &attr_field;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_write_attr_cmd_req(&write_req);
    esp_zb_lock_release();

    char tbuf[64];
    get_timestamp(tbuf, sizeof(tbuf));
    ESP_LOGI(TAG, "%s => [%s] 0x04%x setpoint=%d (%.1f C)",
             tbuf, name, (unsigned int)zb_short_addr, setpoint, setpoint / 100.0);
    
    // A small delay to avoid saturating the Zigbee stack when sending to multiple valves
    vTaskDelay(pdMS_TO_TICKS(100));
}

// 2. CHRONOTHERMOSTAT TASK (checks schedules for all devices)
void chronothermostat_task(void *pvParameters) {
    time_t now;
    struct tm timeinfo;
    int last_minute = -1;
    int last_hour = -1;

    // Wait for Zigbee to be ready and valves to connect
    vTaskDelay(20000 / portTICK_PERIOD_MS);

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        // Periodic log every 5 minutes
        if (timeinfo.tm_min % 5 == 0 && timeinfo.tm_sec < 12) {
            char tbuf[64];
            get_timestamp(tbuf, sizeof(tbuf));
            ESP_LOGI(TAG, "%s [CHRONO] Status | Devices: %d", tbuf, g_device_count);
            for (int i = 0; i < g_device_count; i++) {
                device_config_t *d = &g_devices[i];
                if (!d->enabled) continue;
                int16_t target = config_get_current_temp(d, timeinfo.tm_hour, timeinfo.tm_min);
                ESP_LOGI(TAG, "  [%s] connected=%s addr=0x%04x sched_target=%.1f C",
                         d->name, d->connected ? "YES" : "NO",
                         (unsigned int)d->zb_short_addr, target / 100.0);
            }
        }

        // Every new minute, check if action is needed
        if (timeinfo.tm_min != last_minute) {
            
            // If not the first run, check for "slot transitions"
            if (last_minute != -1 && last_hour != -1) {
                for (int i = 0; i < g_device_count; i++) {
                    device_config_t *d = &g_devices[i];
                    if (!d->enabled || !d->connected) continue;

                    int16_t target_hour = config_get_current_temp(d, timeinfo.tm_hour, timeinfo.tm_min);
                    int16_t target_prev = config_get_current_temp(d, last_hour, last_minute);

                    // Write temperature ONLY if a switch between high and low slots occurred
                    if (target_hour != target_prev) {
                        char tbuf[64];
                        get_timestamp(tbuf, sizeof(tbuf));
                        ESP_LOGI(TAG, "%s [CHRONO] Slot change! '%s' goes from %.1f C to %.1f C", 
                                 tbuf, d->name, target_prev / 100.0, target_hour / 100.0);
                        set_temperature(d->zb_short_addr, target_hour, d->name);
                    }
                }
            }

            last_minute = timeinfo.tm_min;
            last_hour = timeinfo.tm_hour;
        }

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

// 3. ZIGBEE EVENT MANAGEMENT

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p   = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "=> Zigbee: Initializing stack...");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err_status == ESP_OK) {
                bool is_new = esp_zb_bdb_is_factory_new();
                ESP_LOGI(TAG, "=> Zigbee started (%s)",
                         is_new ? "NEW NETWORK" : "EXISTING NETWORK");
                
                if (is_new) {
                    ESP_LOGI(TAG, "=> Starting network formation...");
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
                } else {
                    ESP_LOGI(TAG, "=> Existing network found! Restoring cached states...");
                    zb_restore_connections(); // Try to reconnect devices from stack cache
                    ESP_LOGI(TAG, "=> Opening pairing (30s) for any new devices");
                    esp_zb_bdb_open_network(30);
                }
            } else {
                ESP_LOGE(TAG, "=> Zigbee start ERROR: %s", esp_err_to_name(err_status));
            }
            break;

        case ESP_ZB_BDB_SIGNAL_FORMATION:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "=> Network FORMED! PAN: 0x%04x, Channel: %d",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel());
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGW(TAG, "=> Formation failed: %s. Retrying in 1s...", esp_err_to_name(err_status));
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "=> Coordinator READY! Network open for pairing.");
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            esp_zb_zdo_signal_device_annce_params_t *annce =
                (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);

            uint16_t short_addr = annce->device_short_addr;

            // We derive the short IEEE from the first 2 bytes (for matching with JSON)
            // JSON uses 2 bytes (e.g. "0x424c") corresponding to bytes [0:1] of the IEEE addr
            esp_zb_ieee_addr_t ieee;
            esp_zb_ieee_address_by_short(short_addr, ieee);

            char tbuf[64];
            get_timestamp(tbuf, sizeof(tbuf));
            ESP_LOGI(TAG, "%s ================================================", tbuf);
            ESP_LOGI(TAG, "=> Device announced: 0x%04x (IEEE: %02x%02x%02x%02x%02x%02x%02x%02x)",
                     (unsigned int)short_addr, 
                     ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);

            // Search in config and associate
            device_config_t *found = config_find_and_connect(ieee, short_addr);
            if (found) {
                ESP_LOGI(TAG, "=> MATCHING: '%s' -> 0x%04x", found->name, (unsigned int)short_addr);
                blink_led(2, 80, 80); // Fast double blink on successful connection
                // Apply correct temperature for current time immediately
                time_t now; struct tm ti;
                time(&now); localtime_r(&now, &ti);
                int16_t target = config_get_current_temp(found, ti.tm_hour, ti.tm_min);
                ESP_LOGI(TAG, "=> Setting immediately %.1f C", target / 100.0);
                set_temperature(short_addr, target, found->name);
            } else {
                ESP_LOGW(TAG, "=> Device 0x%04x NOT found in JSON config!", (unsigned int)short_addr);
                ESP_LOGW(TAG, "   Update 'ieee' in config with: \"%02x%02x%02x%02x%02x%02x%02x%02x\"", 
                         ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);
            }
            ESP_LOGI(TAG, "================================================");
            break;
        }

        case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
            if (err_status == ESP_OK) {
                uint8_t *duration = (uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (*duration) {
                    ESP_LOGI(TAG, "=> Network OPEN for %d seconds (put the valve in pairing!)", *duration);
                } else {
                    ESP_LOGW(TAG, "=> Network CLOSED. Pairing not allowed.");
                }
            }
            break;

        default:
            ESP_LOGD(TAG, "Zigbee signal: 0x%x, status: %s", sig_type, esp_err_to_name(err_status));
            break;
    }
}

// 3b. BOOT BUTTON - cycles between temp_high and temp_low
// for all connected devices
static void zb_buttons_handler(switch_func_pair_t *button_func_pair) {
    static bool toggle_high = false;
    if (button_func_pair->func != SWITCH_ONOFF_TOGGLE_CONTROL) return;

    toggle_high = !toggle_high;
    ESP_LOGI(TAG, "*** BOOT pressed: forcing %s on all connected devices ***",
             toggle_high ? "HIGH" : "LOW");

    for (int i = 0; i < g_device_count; i++) {
        device_config_t *d = &g_devices[i];
        if (!d->enabled || !d->connected) continue;
        int16_t target = toggle_high ? d->temp_high : d->temp_low;
        set_temperature(d->zb_short_addr, target, d->name);
    }
}

// 4. WIFI
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "=> WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "=> WiFi disconnected (reason: %d), retrying...", event->reason);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=> WiFi Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Signal that IP is ready
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Force WiFi config in RAM, ignoring corrupted flash (NVS) one
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.rssi = -127,
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// 5. ZIGBEE TASK (dedicated mandatory task)
static void esp_zb_task(void *pvParameters) {
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    
    // Server clusters (Basic)
    esp_zb_attribute_list_t *basic_attr = esp_zb_basic_cluster_create(NULL);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Client clusters (Thermostat)
    // Most TRVs require the coordinator to have the Thermostat cluster (Client role)
    // to accept writes on their Thermostat Server attributes.
    esp_zb_attribute_list_t *thermo_attr = esp_zb_thermostat_cluster_create(NULL);
    esp_zb_cluster_list_add_thermostat_cluster(cluster_list, thermo_attr, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint       = HA_VALVE_REGULATOR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id  = ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
    esp_zb_device_register(ep_list);

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "=> Zigbee stack started. Entering main loop...");
    esp_zb_stack_main_loop();
}

// 6. MAIN
void app_main(void) {
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, " ZIGBEE CHRONOTHERMOSTAT - ESP32-C6");
    ESP_LOGI(TAG, "=============================================");

    // Initialize LED early
    gpio_reset_pin(BOARD_LED_GPIO);
    gpio_set_direction(BOARD_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LED_GPIO, 0);

    // PHASE 1: NVS
    ESP_LOGI(TAG, "[1/6] Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // PHASE 2: WiFi
    ESP_LOGI(TAG, "[2/6] Starting WiFi...");
    wifi_init_sta();

    // Wait for IP via EventGroup (max 20s)
    ESP_LOGI(TAG, "   Waiting for IP...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "   IP obtained. Waiting 1s for DNS...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // allow DNS to stabilize
    } else {
        ESP_LOGW(TAG, "   WiFi timeout! Proceeding anyway...");
    }

    // PHASE 3: Download JSON config
    ESP_LOGI(TAG, "[3/6] Downloading JSON config from: %s", CONFIG_JSON_URL);
    bool cfg_ok = config_load_from_url(CONFIG_JSON_URL);
    if (cfg_ok) {
        ESP_LOGI(TAG, "=> Loaded %d devices from JSON", g_device_count);
    } else {
        ESP_LOGW(TAG, "=> JSON download failed! Continuing without config.");
    }

    // Visual feedback: blink X times (X = devices found in config)
    if ((bits & WIFI_CONNECTED_BIT) && cfg_ok) {
        ESP_LOGI(TAG, "=> WiFi & Config OK: Blinking %d times...", g_device_count);
        blink_led(g_device_count, 150, 150);
    }

    // PHASE 4: SNTP
    ESP_LOGI(TAG, "[4/6] SNTP Synchronization...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now; struct tm timeinfo;
    int retry = 0;
    while (retry++ < 30) {
        time(&now); localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2024 - 1900)) break;
        ESP_LOGI(TAG, "   Waiting for SNTP... (%d/30)", retry);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time(&now); localtime_r(&now, &timeinfo);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%d/%m/%Y %H:%M:%S", &timeinfo);
    if (timeinfo.tm_year > (2024 - 1900)) {
        ESP_LOGI(TAG, "=> TIME SYNCHRONIZED: %s", tbuf);
        blink_led(1, 1000, 0); // 1s blink for time sync
    } else {
        ESP_LOGW(TAG, "=> SNTP failed. Time: %s", tbuf);
    }

    // PHASE 5: Turn off WiFi completely (free radio for Zigbee)
    ESP_LOGI(TAG, "[5/6] Turning off WiFi (free radio for Zigbee)...");
    esp_sntp_stop();
    esp_wifi_stop();
    esp_wifi_deinit();

    // PHASE 6: Start Zigbee
    ESP_LOGI(TAG, "[6/6] Starting Zigbee Coordinator...");
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    static switch_func_pair_t button_func_pair[] = {
        {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}
    };
    switch_driver_init(button_func_pair, PAIR_SIZE(button_func_pair), zb_buttons_handler);

    xTaskCreate(chronothermostat_task, "task_crono",  4096, NULL, 5, NULL);
    xTaskCreate(esp_zb_task,          "Zigbee_main", 4096, NULL, 5, NULL);
}