/*
 * SparkMiner - Web UI
 * Status page + settings editor served on the LAN while mining
 */

#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>

/**
 * Start HTTP server + mDNS (call after WiFi is connected)
 */
void web_ui_start();

/**
 * FreeRTOS task that services HTTP clients (runs on Core 0)
 */
void web_ui_task(void *param);

#endif // WEB_UI_H
