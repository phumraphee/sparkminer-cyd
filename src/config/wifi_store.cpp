/*
 * SparkMiner - Saved WiFi networks
 */

#include <Preferences.h>
#include "wifi_store.h"
#include "nvs_config.h"

#define NS "sparkwifi"

static wifi_entry_t s_list[WIFI_STORE_MAX];
static int s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static void saveAll() {
    Preferences p;
    if (!p.begin(NS, false)) return;
    p.putUChar("n", s_count);
    for (int i = 0; i < WIFI_STORE_MAX; i++) {
        char k[8];
        snprintf(k, sizeof(k), "e%d", i);
        if (i < s_count) p.putBytes(k, &s_list[i], sizeof(wifi_entry_t));
        else p.remove(k);
    }
    p.end();
}

void wifi_store_init() {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    Preferences p;
    s_count = 0;
    bool fresh = true;
    if (p.begin(NS, true)) {
        fresh = !p.isKey("n");
        int n = p.getUChar("n", 0);
        for (int i = 0; i < n && i < WIFI_STORE_MAX; i++) {
            char k[8];
            snprintf(k, sizeof(k), "e%d", i);
            if (p.getBytes(k, &s_list[s_count], sizeof(wifi_entry_t)) == sizeof(wifi_entry_t)) {
                s_list[s_count].ssid[32] = '\0';
                s_list[s_count].pass[64] = '\0';
                if (s_list[s_count].ssid[0]) s_count++;
            }
        }
        p.end();
    }
    // First run: bring over the network from the original single-SSID config
    miner_config_t *c = nvs_config_get();
    if (fresh && c->ssid[0]) {
        wifi_store_put(c->ssid, c->wifiPassword);
    }
    Serial.printf("[WIFI] %d saved network(s)\n", s_count);
}

int wifi_store_count() { return s_count; }

const wifi_entry_t* wifi_store_get(int i) {
    return (i >= 0 && i < s_count) ? &s_list[i] : nullptr;
}

const wifi_entry_t* wifi_store_find(const char *ssid) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_list[i].ssid, ssid) == 0) return &s_list[i];
    }
    return nullptr;
}

bool wifi_store_put(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || strlen(pass ? pass : "") > 64) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_entry_t e;
    memset(&e, 0, sizeof(e));
    strlcpy(e.ssid, ssid, sizeof(e.ssid));
    strlcpy(e.pass, pass ? pass : "", sizeof(e.pass));
    // Remove any existing copy, then insert at the front (drops the oldest when full)
    int w = 0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_list[i].ssid, ssid) != 0) s_list[w++] = s_list[i];
    }
    s_count = w;
    if (s_count == WIFI_STORE_MAX) s_count--;
    memmove(&s_list[1], &s_list[0], s_count * sizeof(wifi_entry_t));
    s_list[0] = e;
    s_count++;
    saveAll();
    xSemaphoreGive(s_mutex);
    return true;
}

bool wifi_store_remove(const char *ssid) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int w = 0;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_list[i].ssid, ssid) != 0) s_list[w++] = s_list[i];
    }
    bool removed = w != s_count;
    s_count = w;
    if (removed) saveAll();
    xSemaphoreGive(s_mutex);
    return removed;
}
