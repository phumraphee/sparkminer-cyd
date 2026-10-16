/*
 * SparkMiner - Touch screen calibration (CYD)
 */

#include "touch_cal.h"

#if defined(ESP32_2432S028) && USE_DISPLAY

#include <TFT_eSPI.h>
#include <board_config.h>
#include "../display/display.h"
#include "../display/touch_cyd.h"
#include "../config/nvs_config.h"
#include "../extras/extras.h"
#include "wifi_ui.h"

#define TOUCH_IRQ 36
#define INSET     24      // Crosshair distance from the screen edge (px)
#define TEST_MS   30000   // Unsaved calibration is dropped after this

static volatile bool s_active = false;
static TFT_eSPI *tft;
static int W, H;

static void crosshair(int x, int y, uint16_t c) {
    tft->drawFastHLine(x - 12, y, 25, c);
    tft->drawFastVLine(x, y - 12, 25, c);
    tft->drawCircle(x, y, 6, c);
}

static void centerText(const char *t, int y, uint16_t c, int size = 2) {
    tft->setTextDatum(MC_DATUM);
    tft->setTextSize(size);
    tft->setTextColor(c, TFT_BLACK);
    tft->drawString(t, W / 2, y);
}

// Wait for one complete press; average its pressure-checked samples
static bool readPress(uint16_t *rx, uint16_t *ry, uint32_t timeoutMs) {
    uint32_t start = millis();
    while (digitalRead(TOUCH_IRQ) == LOW) {               // Finger still down from before
        if (millis() - start > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (digitalRead(TOUCH_IRQ) != LOW) {
        if (millis() - start > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    uint32_t sx = 0, sy = 0, n = 0, down = millis();
    while (digitalRead(TOUCH_IRQ) == LOW) {
        uint16_t x, y;
        if (millis() - down > 40 && touch_read_raw(&x, &y)) { sx += x; sy += y; n++; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (n < 3) return false;
    *rx = sx / n;
    *ry = sy / n;
    return true;
}

static void calTask(void *param) {
    vTaskDelay(pdMS_TO_TICKS(300));   // Let a closing WiFi menu hand the screen back first
    display_take(true);
    tft = display_tft();
    W = tft->width();
    H = tft->height();
    uint8_t rot = nvs_config_get()->rotation;

    const int tx[4] = {INSET, W - INSET, W - INSET, INSET};
    const int ty[4] = {INSET, INSET, H - INSET, H - INSET};
    uint16_t raw[4][2];
    float panel[4][2];
    bool saved = false;

    for (int attempt = 0; attempt < 3 && !saved; attempt++) {
        bool ok = true;
        for (int i = 0; i < 4 && ok; i++) {
            display_lock();
            tft->fillScreen(TFT_BLACK);
            centerText("Touch calibration", H / 2 - 30, TFT_ORANGE);
            centerText("Tap the center of the +", H / 2, TFT_WHITE, 1);
            centerText("Use a fingernail or stylus", H / 2 + 14, TFT_DARKGREY, 1);
            char step[16];
            snprintf(step, sizeof(step), "%d / 4", i + 1);
            centerText(step, H / 2 + 34, TFT_DARKGREY, 1);
            crosshair(tx[i], ty[i], TFT_ORANGE);
            display_unlock();
            if (!readPress(&raw[i][0], &raw[i][1], 30000)) { ok = false; break; }
            touch_screen_to_panel((float)tx[i] / W, (float)ty[i] / H, rot, &panel[i][0], &panel[i][1]);
        }
        if (!ok) break;   // Nobody tapped: keep the old calibration

        float cal[6];
        float err = touch_fit_calibration(raw, panel, 4, cal);
        if (err > 0.06f) {   // Points don't fit one mapping: a tap probably missed
            display_lock();
            tft->fillScreen(TFT_BLACK);
            centerText("Taps didn't line up", H / 2 - 10, TFT_RED);
            centerText("Let's try again", H / 2 + 14, TFT_WHITE, 1);
            display_unlock();
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        // Try it out: a dot follows taps; Save / Redo buttons use the new mapping
        touch_set_calibration(cal, false);
        int bw = W / 2 - 12, bh = 44, by = H - bh - 8;
        auto drawTest = [&]() {
            display_lock();
            tft->fillScreen(TFT_BLACK);
            centerText("Tap around to test", 24, TFT_ORANGE);
            centerText("The dot should land under your tap", 46, TFT_DARKGREY, 1);
            tft->fillRoundRect(8, by, bw, bh, 6, 0x0320);
            tft->fillRoundRect(W - 8 - bw, by, bw, bh, 6, 0x2124);
            tft->setTextDatum(MC_DATUM);
            tft->setTextSize(2);
            tft->setTextColor(TFT_WHITE, 0x0320);
            tft->drawString("Save", 8 + bw / 2, by + bh / 2);
            tft->setTextColor(TFT_WHITE, 0x2124);
            tft->drawString("Redo", W - 8 - bw / 2, by + bh / 2);
            display_unlock();
        };
        drawTest();
        uint32_t testStart = millis();
        bool redo = false;
        while (millis() - testStart < TEST_MS) {
            uint16_t rx, ry;
            if (!readPress(&rx, &ry, TEST_MS - (millis() - testStart))) break;
            float fx, fy;
            touch_screen_xy(rx, ry, rot, &fx, &fy);
            int px = fx * W, py = fy * H;
            if (py >= by && py < by + bh && px < W / 2) { touch_set_calibration(cal, true); saved = true; break; }
            if (py >= by && py < by + bh && px >= W / 2) { redo = true; break; }
            display_lock();
            tft->fillCircle(px, py, 4, TFT_GREEN);
            display_unlock();
            testStart = millis();   // Still being tested: keep the window open
        }
        if (!saved && !redo) break;   // Timed out without Save: discard
    }

    if (!saved) touch_reload_calibration();   // Back to the previous mapping
    display_lock();
    tft->fillScreen(TFT_BLACK);
    centerText(saved ? "Calibration saved" : "Calibration unchanged", H / 2, saved ? TFT_GREEN : TFT_DARKGREY);
    display_unlock();
    vTaskDelay(pdMS_TO_TICKS(1200));
    display_take(false);
    s_active = false;
    vTaskDelete(NULL);
}

void touch_cal_start() {
    if (s_active) return;
    if (wifi_ui_is_open()) wifi_ui_close();
    extras_wake();
    s_active = true;
    if (xTaskCreatePinnedToCore(calTask, "TouchCal", 6144, NULL, 2, NULL, 0) != pdPASS) s_active = false;
}

bool touch_cal_active() {
    return s_active;
}

#else

void touch_cal_start() {}
bool touch_cal_active() { return false; }

#endif
