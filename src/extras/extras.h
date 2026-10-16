/*
 * SparkMiner - Extras
 * Phone notifications (ntfy), night dimming, CYD RGB status LED,
 * WiFi watchdog + roaming, and the web UI admin PIN.
 *
 * Settings live in their own NVS namespace so the main config
 * struct (and its checksum) stays compatible with stock firmware.
 */

#ifndef EXTRAS_H
#define EXTRAS_H

#include <Arduino.h>

typedef struct {
    char pin[9];            // Web UI admin PIN (empty = no PIN)
    char ntfyTopic[65];     // ntfy.sh topic (empty = notifications off)
    bool notifyDaily;       // Daily summary at 09:00 local time
    bool nightEnabled;
    uint8_t nightStart;     // Hour (0-23) the screen dims
    uint8_t nightEnd;       // Hour (0-23) the screen returns to normal
    uint8_t nightBright;    // Backlight % during the night (0 = off)
    bool ledEnabled;        // RGB status LED
    bool autoUpdate;        // Install signed releases from GitHub automatically
    uint8_t screenOffMin;   // Turn the backlight off after this many minutes without a tap (0 = never)
    bool autoBright;        // Follow room light (light sensor), between 10 % and the set brightness
    bool darkOff;           // Screen off while the room is dark
    uint16_t lightBright;   // Sensor reading with the room lit   (calibrated from the web UI)
    uint16_t lightDark;     // Sensor reading with the room dark  (calibrated from the web UI)
} extras_config_t;

void extras_init();
extras_config_t* extras_get();
bool extras_save();

/**
 * Background task: notifications, night mode, LED, WiFi watchdog (Core 0)
 */
void extras_task(void *param);

/**
 * True while night mode has the screen dimmed/off
 */
bool extras_is_dimmed();

/**
 * Screen was tapped / button pressed while dark: light it up again
 * (for a minute at night, until the idle timeout otherwise)
 */
void extras_wake();

/**
 * Any tap or button press: restarts the screen-off timer
 */
void extras_activity();

/**
 * Smoothed light sensor reading (raw ADC), and calibration from the current value
 */
int extras_light();
void extras_light_calibrate(bool roomIsBright);

/**
 * Acknowledge the block-found LED celebration
 */
void extras_ack_block();

/**
 * Reboot straight into the WiFi setup hotspot (keeps all other settings)
 */
void extras_request_portal();

/**
 * Consume the "open portal on this boot" flag (called once at boot)
 */
bool extras_take_portal_flag();

/**
 * Setup hotspot password (random per device, shown on screen while the hotspot is open)
 */
const char* extras_ap_password();

/**
 * Send a notification now (no-op without a topic). Returns true on HTTP 200.
 */
bool extras_notify(const char *title, const char *message, const char *tags, int priority);

#endif // EXTRAS_H
