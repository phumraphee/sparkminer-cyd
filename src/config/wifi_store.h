/*
 * SparkMiner - Saved WiFi networks
 * Up to WIFI_STORE_MAX networks in their own NVS namespace, most recently
 * used first. The miner joins the strongest saved network in range.
 */

#ifndef WIFI_STORE_H
#define WIFI_STORE_H

#include <Arduino.h>

#define WIFI_STORE_MAX 8

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_entry_t;

void wifi_store_init();             // Loads; migrates the legacy single SSID on first run
int wifi_store_count();
const wifi_entry_t* wifi_store_get(int i);
const wifi_entry_t* wifi_store_find(const char *ssid);
bool wifi_store_put(const char *ssid, const char *pass);   // Add/update, moves to the front
bool wifi_store_remove(const char *ssid);

#endif // WIFI_STORE_H
