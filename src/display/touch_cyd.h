/*
 * SparkMiner - CYD resistive touch (XPT2046) position reader
 * Bit-banged on the CYD's dedicated touch pins; polled from the button task.
 */

#ifndef TOUCH_CYD_H
#define TOUCH_CYD_H

#include <Arduino.h>

#if defined(ESP32_2432S028)
#define HAVE_TOUCH_POSITION 1

void touch_init();

/**
 * Read raw 12-bit X/Y (only meaningful while the panel is pressed)
 */
bool touch_read_raw(uint16_t *x, uint16_t *y);

/**
 * Horizontal tap position as a fraction of the screen width (0 = left edge)
 * for the given TFT rotation
 */
float touch_screen_x(uint16_t rawX, uint16_t rawY, uint8_t rotation);

/**
 * Remember a tap position / read the last 4 (oldest first) for calibration
 */
void touch_record_tap(uint16_t x, uint16_t y);
void touch_history(uint16_t out[4][2]);
#endif

#endif // TOUCH_CYD_H
