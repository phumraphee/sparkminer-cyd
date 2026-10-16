/*
 * SparkMiner - 24 h history for the web UI charts
 * One sample every 5 minutes (hashrate, chip temperature, WiFi signal, pool state),
 * timestamped with wall-clock time and saved to NVS hourly so reboots keep it.
 */

#ifndef HISTORY_H
#define HISTORY_H

#include <Arduino.h>

void history_init();
void history_tick();            // Call about once a second
void history_json(String &out); // {"interval":300,"t":[...],"hr":[...],"temp":[...],"rssi":[...],"pool":[...]}
void history_save();            // Persist now (e.g. before a planned restart)

#endif // HISTORY_H
