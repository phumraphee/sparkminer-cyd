/*
 * SparkMiner - On-device WiFi menu (CYD touch screen)
 *
 * Hold the screen 3 s to open. Lists nearby networks (saved ones marked),
 * joins saved networks with one tap, and takes new passwords on an old-phone
 * style multi-tap keypad (big keys suit the resistive panel). The setup
 * hotspot is still available from here for emergencies. Mining continues.
 */

#ifndef WIFI_UI_H
#define WIFI_UI_H

#include <Arduino.h>

void wifi_ui_open();
void wifi_ui_close();
bool wifi_ui_is_open();

/**
 * Tap at (fx, fy) as fractions of the screen (0,0 = top-left)
 */
void wifi_ui_touch(float fx, float fy);

#endif // WIFI_UI_H
