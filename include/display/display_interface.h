/*
 * SparkMiner - Display Driver Interface
 * Abstract interface for pluggable display drivers
 *
 * Supports: TFT (CYD, T-Display), LED (headless), Serial (fallback)
 *
 * GPL v3 License
 */

#ifndef DISPLAY_INTERFACE_H
#define DISPLAY_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>

// Forward declaration - actual struct defined in display.h
typedef struct display_data_s display_data_t;

// ============================================================
// Display Driver Interface
// ============================================================

/**
 * Display driver function table
 * Each display type (TFT, LED, Serial) implements this interface
 */
typedef struct {
    /**
     * Initialize the display hardware
     * @param rotation Screen rotation (0-3, display-specific)
     * @param brightness Brightness level (0-100)
     */
    void (*init)(uint8_t rotation, uint8_t brightness);

    /**
     * Update display with current mining data
     * @param data Pointer to display data structure
     */
    void (*update)(const display_data_t *data);

    /**
     * Set display brightness
     * @param brightness Brightness level (0-100)
     */
    void (*set_brightness)(uint8_t brightness);

    /**
     * Cycle to next screen (if multi-screen support)
     */
    void (*next_screen)(void);

    /**
     * Show AP configuration screen
     * @param ssid AP SSID to display
     * @param password AP password to display
     * @param ip IP address to connect to
     */
    void (*show_ap_config)(const char *ssid, const char *password, const char *ip);

    /**
     * Show boot splash screen
     */
    void (*show_boot)(void);

    /**
     * Show factory reset countdown
     * @param seconds Seconds remaining
     */
    void (*show_reset_countdown)(int seconds);

    /**
     * Show reset complete message
     */
    void (*show_reset_complete)(void);

    /**
     * Force a full redraw on next update
     */
    void (*redraw)(void);

    /**
     * Flip/rotate display orientation
     * @return New rotation value
     */
    uint8_t (*flip_rotation)(void);

    /**
     * Set color inversion
     * @param inverted true to invert colors
     */
    void (*set_inverted)(bool inverted);

    /**
     * Get display width in pixels
     * @return Width (0 for non-pixel displays like LED)
     */
    uint16_t (*get_width)(void);

    /**
     * Get display height in pixels
     * @return Height (0 for non-pixel displays like LED)
     */
    uint16_t (*get_height)(void);

    /**
     * Check if display is in portrait orientation
     * @return true if portrait
     */
    bool (*is_portrait)(void);

    /**
     * Get current screen index
     * @return Current screen (0-based)
     */
    uint8_t (*get_screen)(void);

    /**
     * Set current screen
     * @param screen Screen index to set
     */
    void (*set_screen)(uint8_t screen);

    /**
     * Driver name for debugging
     */
    const char *name;

} DisplayDriver;

// ============================================================
// Screen Constants
// ============================================================

#define SCREEN_MINING   0
#define SCREEN_STATS    1
#define SCREEN_CLOCK    2
#define SCREEN_COUNT    3

// Note: display_data_t is defined in src/display/display.h
// to maintain backward compatibility with existing code

// ============================================================
// Global Driver Access
// ============================================================

/**
 * Get the active display driver
 * Returns NULL if no display configured
 */
DisplayDriver* display_get_driver(void);

/**
 * Register a display driver
 * Called during driver initialization
 */
void display_register_driver(DisplayDriver *driver);

#endif // DISPLAY_INTERFACE_H
