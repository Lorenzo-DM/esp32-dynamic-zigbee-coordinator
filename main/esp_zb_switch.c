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
#include "esp_zb_switch.h"
#include "secrets.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "config_loader.h"

static const char *TAG = "CRONOTERMOSTATO_C6";

// Bit dell'EventGroup WiFi
#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t s_wifi_event_group;

// URL completo del config JSON (base_url dal progetto Gitea)
#define CONFIG_JSON_URL  BASE_URL "/raw/branch/main/config.json"

// ========================================================
// 1. FUNZIONE PER INVIARE LA TEMPERATURA A UNA VALVOLA
// ========================================================
static void imposta_temperatura(uint16_t zb_short_addr, int16_t setpoint, const char *nome) {
    esp_zb_zcl_write_attr_cmd_t write_req;
    write_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    write_req.zcl_basic_cmd.src_endpoint = 1;
    write_req.zcl_basic_cmd.dst_endpoint = 1;  // Sonoff TRV = endpoint 1
    write_req.zcl_basic_cmd.dst_addr_u.addr_short = zb_short_addr;
    write_req.clusterID = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT; // 0x0201

    esp_zb_zcl_attribute_t attr_field;
    attr_field.id = 0x0012; // OccupiedHeatingSetpoint
    attr_field.data.type = ESP_ZB_ZCL_ATTR_TYPE_S16;
    attr_field.data.value = &setpoint;
    attr_field.data.size = sizeof(int16_t);

    write_req.attr_number = 1;
    write_req.attr_field = &attr_field;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_write_attr_cmd_req(&write_req);
    esp_zb_lock_release();

    ESP_LOGI(TAG, "  => [%s] 0x%04x setpoint=%d (%.1f C)",
             nome, (unsigned int)zb_short_addr, setpoint, setpoint / 100.0);
}

// ========================================================
// 2. TASK CRONOTERMOSTATO (controlla orari per tutti i device)
// ========================================================
void task_cronotermostato(void *pvParameters) {
    time_t now;
    struct tm timeinfo;
    int ultimo_minuto = -1;
    int ultima_ora = -1;

    // Aspetta che Zigbee sia pronto e le valvole si siano connesse
    vTaskDelay(20000 / portTICK_PERIOD_MS);

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        // Log periodico ogni 5 minuti
        if (timeinfo.tm_min % 5 == 0 && timeinfo.tm_sec < 12) {
            char buf[32];
            strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "[CRONO] Ora: %s | Dispositivi: %d", buf, g_device_count);
            for (int i = 0; i < g_device_count; i++) {
                device_config_t *d = &g_devices[i];
                if (!d->enabled) continue;
                int16_t target = config_get_current_temp(d, timeinfo.tm_hour, timeinfo.tm_min);
                ESP_LOGI(TAG, "  [%s] connessa=%s addr=0x%04x sched_target=%.1f C",
                         d->name, d->connected ? "SI" : "NO",
                         (unsigned int)d->zb_short_addr, target / 100.0);
            }
        }

        // Ogni nuovo minuto, controlla se bisogna agire
        if (timeinfo.tm_min != ultimo_minuto) {
            
            // Se non è il primo avvio, controlliamo le "transizioni" di fascia
            if (ultimo_minuto != -1 && ultima_ora != -1) {
                for (int i = 0; i < g_device_count; i++) {
                    device_config_t *d = &g_devices[i];
                    if (!d->enabled || !d->connected) continue;

                    int16_t target_ora = config_get_current_temp(d, timeinfo.tm_hour, timeinfo.tm_min);
                    int16_t target_prima = config_get_current_temp(d, ultima_ora, ultimo_minuto);

                    // Scrive la temperatura SOLO se per gli orari è scattato un cambio tra fascia alta e bassa
                    if (target_ora != target_prima) {
                        ESP_LOGI(TAG, "[CRONO] Cambio fascia! '%s' passa da %.1f C a %.1f C", 
                                 d->name, target_prima / 100.0, target_ora / 100.0);
                        imposta_temperatura(d->zb_short_addr, target_ora, d->name);
                    }
                }
            }

            ultimo_minuto = timeinfo.tm_min;
            ultima_ora = timeinfo.tm_hour;
        }

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

// ========================================================
// 3. GESTIONE EVENTI ZIGBEE
// ========================================================

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p   = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "=> Zigbee: Inizializzazione stack...");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "=> Zigbee avviato (%s)",
                         esp_zb_bdb_is_factory_new() ? "NUOVA RETE" : "RETE ESISTENTE");
                if (esp_zb_bdb_is_factory_new()) {
                    ESP_LOGI(TAG, "=> Avvio formazione rete...");
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
                } else {
                    ESP_LOGI(TAG, "=> Rete esistente, apro accoppiamento (180s)");
                    esp_zb_bdb_open_network(180);
                }
            } else {
                ESP_LOGE(TAG, "=> ERRORE avvio Zigbee: %s", esp_err_to_name(err_status));
            }
            break;

        case ESP_ZB_BDB_SIGNAL_FORMATION:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "=> Rete FORMATA! PAN: 0x%04x, Canale: %d",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel());
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGW(TAG, "=> Formazione fallita: %s. Riprovo in 1s...", esp_err_to_name(err_status));
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "=> Coordinatore PRONTO! Rete aperta per pairing.");
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            esp_zb_zdo_signal_device_annce_params_t *annce =
                (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);

            uint16_t short_addr = annce->device_short_addr;

            // Ricaviamo l'IEEE short dai primi 2 byte (per matching con il JSON)
            // Il JSON usa 2 byte (es. "0x424c") che corrispondono ai byte [0:1] dell'IEEE addr
            esp_zb_ieee_addr_t ieee;
            esp_zb_ieee_address_by_short(short_addr, ieee);

            ESP_LOGI(TAG, "================================================");
            ESP_LOGI(TAG, "=> Dispositivo annunciato: 0x%04x (IEEE: %02x%02x%02x%02x%02x%02x%02x%02x)",
                     (unsigned int)short_addr, 
                     ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);

            // Cerca nel config e associa
            device_config_t *found = config_find_and_connect(ieee, short_addr);
            if (found) {
                ESP_LOGI(TAG, "=> MATCHING: '%s' -> 0x%04x", found->name, (unsigned int)short_addr);
                // Applica subito la temperatura corretta per l'ora attuale
                time_t now; struct tm ti;
                time(&now); localtime_r(&now, &ti);
                int16_t target = config_get_current_temp(found, ti.tm_hour, ti.tm_min);
                ESP_LOGI(TAG, "=> Imposto subito %.1f C", target / 100.0);
                imposta_temperatura(short_addr, target, found->name);
            } else {
                ESP_LOGW(TAG, "=> Dispositivo 0x%04x NON trovato nel config JSON!", (unsigned int)short_addr);
                ESP_LOGW(TAG, "   Aggiorna 'ieee' nel config con: \"%02x%02x%02x%02x%02x%02x%02x%02x\"", 
                         ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);
            }
            ESP_LOGI(TAG, "================================================");
            break;
        }

        case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
            if (err_status == ESP_OK) {
                uint8_t *duration = (uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
                if (*duration) {
                    ESP_LOGI(TAG, "=> Rete APERTA per %d secondi (metti la valvola in pairing!)", *duration);
                } else {
                    ESP_LOGW(TAG, "=> Rete CHIUSA. Accoppiamento non permesso.");
                }
            }
            break;

        default:
            ESP_LOGD(TAG, "Segnale Zigbee: 0x%x, stato: %s", sig_type, esp_err_to_name(err_status));
            break;
    }
}

// ========================================================
// 3b. BOTTONE BOOT – cicla tra temp_high e temp_low
//     di tutti i device connessi
// ========================================================
static void zb_buttons_handler(switch_func_pair_t *button_func_pair) {
    static bool toggle_high = false;
    if (button_func_pair->func != SWITCH_ONOFF_TOGGLE_CONTROL) return;

    toggle_high = !toggle_high;
    ESP_LOGI(TAG, "*** BOOT premuto: forzo %s su tutti i device connessi ***",
             toggle_high ? "ALTA" : "BASSA");

    for (int i = 0; i < g_device_count; i++) {
        device_config_t *d = &g_devices[i];
        if (!d->enabled || !d->connected) continue;
        int16_t target = toggle_high ? d->temp_high : d->temp_low;
        imposta_temperatura(d->zb_short_addr, target, d->name);
    }
}

// ========================================================
// 4. WIFI
// ========================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "=> WiFi STA avviato, connessione in corso...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "=> WiFi disconnesso (motivo: %d), riprovo...", event->reason);
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=> WiFi Connesso! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Segnala che l'IP è pronto
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

    // Forza la configurazione WiFi in RAM ignorando quella corrotta in flash (NVS)
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

// ========================================================
// 5. TASK ZIGBEE (task dedicato obbligatorio)
// ========================================================
static void esp_zb_task(void *pvParameters) {
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *basic_attr = esp_zb_basic_cluster_create(NULL);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint       = 1,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id  = ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
    esp_zb_device_register(ep_list);

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "=> Zigbee stack avviato. Entro nel main loop...");
    esp_zb_stack_main_loop();
}

// ========================================================
// 6. MAIN
// ========================================================
void app_main(void) {
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, " CRONOTERMOSTATO ZIGBEE - ESP32-C6");
    ESP_LOGI(TAG, "=============================================");

    // FASE 1: NVS
    ESP_LOGI(TAG, "[1/6] Inizializzazione NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // FASE 2: WiFi
    ESP_LOGI(TAG, "[2/6] Avvio WiFi...");
    wifi_init_sta();

    // Aspetta IP con EventGroup (max 20s)
    ESP_LOGI(TAG, "   Attesa IP...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(20000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "   IP ottenuto. Attendo 1s per DNS...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // lascia stabilizzare il DNS
    } else {
        ESP_LOGW(TAG, "   WiFi timeout! Procedo comunque...");
    }

    // FASE 3: Download config JSON
    ESP_LOGI(TAG, "[3/6] Download config JSON da: %s", CONFIG_JSON_URL);
    bool cfg_ok = config_load_from_url(CONFIG_JSON_URL);
    if (cfg_ok) {
        ESP_LOGI(TAG, "=> Caricati %d dispositivi dal JSON", g_device_count);
    } else {
        ESP_LOGW(TAG, "=> Download JSON fallito! Continuio senza config.");
    }

    // FASE 4: SNTP
    ESP_LOGI(TAG, "[4/6] Sincronizzazione SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now; struct tm timeinfo;
    int retry = 0;
    while (retry++ < 30) {
        time(&now); localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2024 - 1900)) break;
        ESP_LOGI(TAG, "   Attesa SNTP... (%d/30)", retry);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    time(&now); localtime_r(&now, &timeinfo);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%d/%m/%Y %H:%M:%S", &timeinfo);
    if (timeinfo.tm_year > (2024 - 1900)) {
        ESP_LOGI(TAG, "=> ORA SINCRONIZZATA: %s", tbuf);
    } else {
        ESP_LOGW(TAG, "=> SNTP non riuscito. Ora: %s", tbuf);
    }

    // FASE 5: Spegni WiFi completamente (libera radio per Zigbee)
    ESP_LOGI(TAG, "[5/6] Spegnimento WiFi (libero radio per Zigbee)...");
    esp_sntp_stop();
    esp_wifi_stop();
    esp_wifi_deinit();

    // FASE 6: Avvio Zigbee
    ESP_LOGI(TAG, "[6/6] Avvio Zigbee Coordinator...");
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    static switch_func_pair_t button_func_pair[] = {
        {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}
    };
    switch_driver_init(button_func_pair, PAIR_SIZE(button_func_pair), zb_buttons_handler);

    xTaskCreate(task_cronotermostato, "task_crono",  4096, NULL, 5, NULL);
    xTaskCreate(esp_zb_task,          "Zigbee_main", 4096, NULL, 5, NULL);
}