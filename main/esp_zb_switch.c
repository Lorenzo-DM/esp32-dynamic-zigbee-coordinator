#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

static const char *TAG = "CRONOTERMOSTATO_C6";

// ========================================================
// 1. CONFIGURAZIONE (Orari e Temperature)
// ========================================================

// I setpoint Zigbee sono in centesimi di grado. Es: 2200 = 22.0°C
const int ORA_MATTINA = 7;
const int MIN_MATTINA = 0;
const int TEMP_MATTINA = 2200;

const int ORA_SERA = 23;
const int MIN_SERA = 30;
const int TEMP_SERA = 1800;

// Indirizzo della valvola Sonoff (volatile perché condiviso tra task)
volatile uint16_t trv_short_address = 0;

// ========================================================
// 2. FUNZIONE PER INVIARE LA TEMPERATURA ALLA VALVOLA
// ========================================================
void imposta_temperatura_valvola(uint16_t setpoint_termico) {
    if (trv_short_address == 0) {
        ESP_LOGE(TAG, "Valvola non ancora connessa! Impossibile impostare %d", setpoint_termico);
        return;
    }

    esp_zb_zcl_write_attr_cmd_t write_req;
    write_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    write_req.zcl_basic_cmd.src_endpoint = 1;
    write_req.zcl_basic_cmd.dst_endpoint = 1;  // Sonoff TRV = endpoint 1
    write_req.zcl_basic_cmd.dst_addr_u.addr_short = trv_short_address;
    write_req.clusterID = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT; // 0x0201

    esp_zb_zcl_attribute_t attr_field;
    attr_field.id = 0x0012; // OccupiedHeatingSetpoint
    attr_field.data.type = ESP_ZB_ZCL_ATTR_TYPE_S16;
    attr_field.data.value = &setpoint_termico;
    attr_field.data.size = sizeof(int16_t);

    write_req.attr_number = 1;
    write_req.attr_field = &attr_field;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_write_attr_cmd_req(&write_req);
    esp_zb_lock_release();
    ESP_LOGI(TAG, "=> Comando inviato alla valvola 0x%04x! Setpoint: %d (%.1f C)",
             (unsigned int)trv_short_address, setpoint_termico, setpoint_termico / 100.0);
}

// ========================================================
// 3. IL "CERVELLO" DEGLI ORARI (Task in background)
// ========================================================
void task_cronotermostato(void *pvParameters) {
    time_t now;
    struct tm timeinfo;
    int ultimo_minuto_eseguito = -1;

    // Aspetta che la sincronizzazione SNTP sia avvenuta
    vTaskDelay(15000 / portTICK_PERIOD_MS);

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        // Log periodico ogni 5 minuti per debug
        if (timeinfo.tm_min % 5 == 0 && timeinfo.tm_sec < 12) {
            char buf[32];
            strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
            ESP_LOGI(TAG, "[CRONO] Ora: %s | Valvola: 0x%04x", buf, (unsigned int)trv_short_address);
        }

        if (timeinfo.tm_min != ultimo_minuto_eseguito) {
            // Regola MATTINA
            if (timeinfo.tm_hour == ORA_MATTINA && timeinfo.tm_min == MIN_MATTINA) {
                ESP_LOGI(TAG, "=> Scattata ora MATTINA. Imposto %.1f C", TEMP_MATTINA / 100.0);
                imposta_temperatura_valvola(TEMP_MATTINA);
                ultimo_minuto_eseguito = timeinfo.tm_min;
            }
            // Regola SERA
            if (timeinfo.tm_hour == ORA_SERA && timeinfo.tm_min == MIN_SERA) {
                ESP_LOGI(TAG, "=> Scattata ora SERA. Imposto %.1f C", TEMP_SERA / 100.0);
                imposta_temperatura_valvola(TEMP_SERA);
                ultimo_minuto_eseguito = timeinfo.tm_min;
            }
        }

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

// ========================================================
// 4. GESTIONE EVENTI ZIGBEE
// ========================================================

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode_mask));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
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
                ESP_LOGI(TAG, "=> Zigbee avviato (Stato: %s)",
                         esp_zb_bdb_is_factory_new() ? "NUOVA RETE" : "RETE ESISTENTE");
                if (esp_zb_bdb_is_factory_new()) {
                    ESP_LOGI(TAG, "=> Avvio formazione rete...");
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
                } else {
                    ESP_LOGI(TAG, "=> Rete gia' presente, apro accoppiamento (180s)");
                    esp_zb_bdb_open_network(180);
                }
            } else {
                ESP_LOGE(TAG, "=> ERRORE avvio Zigbee: %s", esp_err_to_name(err_status));
            }
            break;

        case ESP_ZB_BDB_SIGNAL_FORMATION:
            if (err_status == ESP_OK) {
                esp_zb_ieee_addr_t extended_pan_id;
                esp_zb_get_extended_pan_id(extended_pan_id);
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
                ESP_LOGI(TAG, "=> Steering OK. Coordinatore PRONTO per pairing!");
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            esp_zb_zdo_signal_device_annce_params_t *annce_params =
                (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
            trv_short_address = annce_params->device_short_addr;
            ESP_LOGI(TAG, "==================================================");
            ESP_LOGI(TAG, "=> VALVOLA CONNESSA! Indirizzo: 0x%04x", (unsigned int)trv_short_address);
            ESP_LOGI(TAG, "==================================================");
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
// 4b. GESTIONE MANUALE (Bottone BOOT)
// ========================================================
static void zb_buttons_handler(switch_func_pair_t *button_func_pair) {
    static bool toggle_sera = true;
    if (button_func_pair->func == SWITCH_ONOFF_TOGGLE_CONTROL) {
        if (toggle_sera) {
            ESP_LOGI(TAG, "*** BOOT: Forzatura -> SERA (%.1f C) ***", TEMP_SERA / 100.0);
            imposta_temperatura_valvola(TEMP_SERA);
        } else {
            ESP_LOGI(TAG, "*** BOOT: Forzatura -> MATTINA (%.1f C) ***", TEMP_MATTINA / 100.0);
            imposta_temperatura_valvola(TEMP_MATTINA);
        }
        toggle_sera = !toggle_sera;
    }
}

// ========================================================
// 5. WIFI + SNTP (solo per sincronizzare l'ora)
// ========================================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "=> WiFi STA avviato, connessione in corso...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "=> WiFi disconnesso, riprovo...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "=> WiFi Connesso! IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.rssi = -127,
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ========================================================
// 6. TASK ZIGBEE (deve girare nel suo task dedicato!)
// ========================================================
static void esp_zb_task(void *pvParameters) {
    // Inizializzazione Zigbee Coordinator
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    // Creiamo la lista dei cluster (Basic Cluster)
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *basic_attr = esp_zb_basic_cluster_create(NULL);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Creiamo l'Endpoint
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = 1,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_HOME_GATEWAY_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, endpoint_config);

    // Registrazione
    esp_zb_device_register(ep_list);

    // Usa tutti i canali per massima compatibilità
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    // AVVIO + MAIN LOOP (blocca questo task per sempre)
    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "=> Zigbee stack avviato. Entro nel main loop...");
    esp_zb_stack_main_loop();
}

// ========================================================
// 7. MAIN (Sequenza: NVS -> WiFi/SNTP -> WiFi OFF -> Zigbee)
// ========================================================
void app_main(void) {
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, " CRONOTERMOSTATO ZIGBEE - ESP32-C6");
    ESP_LOGI(TAG, "=============================================");

    // --- FASE 1: NVS ---
    ESP_LOGI(TAG, "[1/5] Inizializzazione NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // --- FASE 2: WiFi + SNTP per sincronizzare l'ora ---
    ESP_LOGI(TAG, "[2/5] Avvio WiFi per sincronizzazione ora...");
    wifi_init_sta();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Attendi sincronizzazione SNTP (con verifica reale, non solo timer)
    ESP_LOGI(TAG, "[3/5] Attesa sincronizzazione SNTP...");
    int retry = 0;
    const int max_retry = 30; // 30 secondi max
    time_t now;
    struct tm timeinfo;
    while (retry < max_retry) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2024 - 1900)) {
            break; // Sincronizzato!
        }
        ESP_LOGI(TAG, "   Attesa SNTP... (%d/%d)", retry + 1, max_retry);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        retry++;
    }

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[32];
    strftime(strftime_buf, sizeof(strftime_buf), "%d/%m/%Y %H:%M:%S", &timeinfo);
    if (timeinfo.tm_year > (2024 - 1900)) {
        ESP_LOGI(TAG, "=> ORA SINCRONIZZATA: %s", strftime_buf);
    } else {
        ESP_LOGW(TAG, "=> SNTP NON sincronizzato! Ora: %s (potrebbe essere sbagliata)", strftime_buf);
    }

    // --- FASE 3: Spegnimento COMPLETO WiFi ---
    ESP_LOGI(TAG, "[4/5] Spegnimento completo WiFi (libero radio per Zigbee)...");
    esp_sntp_stop();
    esp_wifi_stop();
    esp_wifi_deinit();  // FONDAMENTALE! Libera completamente la radio per Zigbee

    // --- FASE 4: Avvio Zigbee ---
    ESP_LOGI(TAG, "[5/5] Avvio Zigbee Coordinator...");
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    // Bottone BOOT per controllo manuale
    static switch_func_pair_t button_func_pair[] = {
        {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}
    };
    switch_driver_init(button_func_pair, PAIR_SIZE(button_func_pair), zb_buttons_handler);

    // Task cronotermostato (controlla gli orari)
    xTaskCreate(task_cronotermostato, "task_crono", 4096, NULL, 5, NULL);

    // Task Zigbee (DEVE girare nel suo task dedicato!)
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}