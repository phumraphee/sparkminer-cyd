/*
 * SparkMiner - Monitor Task
 * Coordinates display updates and live stats fetching
 */

#ifndef MONITOR_H
#define MONITOR_H

#include <Arduino.h>

/**
 * Initialize monitor subsystem
 */
void monitor_init();

/**
 * Monitor task (runs on Core 0)
 * Updates display and fetches live stats periodically
 */
void monitor_task(void *param);

/**
 * Latest display data snapshot (updated once per second by monitor task)
 */
struct display_data_s;
const struct display_data_s* monitor_get_data();

#endif // MONITOR_H
