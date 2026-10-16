/*
 * SparkMiner - WiFi Manager
 * Captive portal for WiFi and pool configuration
 *
 * Uses WiFiManager library for initial setup
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

/**
 * Initialize WiFi manager
 * - Tries to connect to stored WiFi
 * - Falls back to AP mode with captive portal
 * - Saves credentials on successful connection
 */
void wifi_manager_init();

/**
 * Start WiFi manager in blocking mode
 * Won't return until WiFi is connected
 */
void wifi_manager_blocking();

/**
 * Start WiFi manager in non-blocking mode
 * Returns immediately, check wifi_manager_is_connected()
 */
void wifi_manager_start();

/**
 * Open the setup hotspot now (5 min), save any new WiFi, then reboot
 */
void wifi_manager_portal();

/**
 * Join one network (saves it as current on success)
 */
bool wifi_manager_connect(const char *ssid, const char *pass, uint32_t timeoutMs,
                          int32_t channel = 0, const uint8_t *bssid = nullptr);

/**
 * Scan and join the strongest saved network in range
 */
bool wifi_manager_connect_best(uint32_t perTryMs);

/**
 * connect_best, then a direct join of the last-used network as a fallback
 */
bool wifi_manager_reconnect();

/**
 * Process WiFi manager events
 * Call periodically in non-blocking mode
 */
void wifi_manager_process();

/**
 * Check if WiFi is connected
 */
bool wifi_manager_is_connected();

/**
 * Force configuration portal
 * Opens AP even if credentials exist
 */
void wifi_manager_reset();

/**
 * Get current IP address as string
 */
const char* wifi_manager_get_ip();

#endif // WIFI_MANAGER_H
