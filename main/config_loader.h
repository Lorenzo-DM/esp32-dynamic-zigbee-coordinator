#pragma once
#include <stdint.h>
#include <stdbool.h>

#define MAX_DEVICES    8
#define MAX_SCHEDULES  8
#define MAX_NAME_LEN   32

// Una fascia oraria: riscaldamento ALTO dalle "start" alle "end"
typedef struct {
    int start_hour;
    int start_min;
    int end_hour;
    int end_min;
} schedule_entry_t;

// Una valvola letta dal JSON
typedef struct {
    char     name[MAX_NAME_LEN];
    uint8_t  ieee_addr[8];        // indirizzo IEEE a 64 bit (mac address)
    bool     ieee_known;          // false se ieee == "-1"
    bool     enabled;
    int16_t  temp_high;           // temperatura alta (fascia attiva)
    int16_t  temp_low;            // temperatura bassa (fuori fascia)
    schedule_entry_t schedule[MAX_SCHEDULES];
    int      schedule_count;
    uint16_t zb_short_addr;       // impostato quando la valvola si connette
    bool     connected;
} device_config_t;

// Array globale dei dispositivi caricati dal JSON
extern device_config_t g_devices[MAX_DEVICES];
extern int             g_device_count;

/**
 * Scarica il JSON dall'URL e popola g_devices / g_device_count.
 * Da chiamare mentre il WiFi è attivo.
 * @return true se almeno un dispositivo è stato caricato.
 */
bool config_load_from_url(const char *url);

/**
 * Data l'ora corrente, restituisce la temperatura corretta per un dispositivo.
 * Se l'ora cade in una fascia → temp_high, altrimenti → temp_low.
 */
int16_t config_get_current_temp(const device_config_t *dev, int hour, int min);

/**
 * Cerca un dispositivo per ieee_short e aggiorna zb_short_addr + connected.
 * Chiamata dall'handler DEVICE_ANNCE.
 * @return puntatore al device trovato, o NULL.
 */
device_config_t *config_find_and_connect(const uint8_t *ieee_addr, uint16_t zb_short_addr);
