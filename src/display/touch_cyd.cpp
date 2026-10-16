/*
 * SparkMiner - CYD resistive touch (XPT2046) position reader
 */

#include "touch_cyd.h"

#ifdef HAVE_TOUCH_POSITION

// CYD touch controller pins (separate from the display SPI bus)
#define T_CLK   25
#define T_MOSI  32
#define T_MISO  39
#define T_CS    33

// Raw range of the panel (typical CYD values)
#define RAW_MIN 200
#define RAW_MAX 3800

// Portrait (rotation 0) calibration from corner taps on the real board:
// TL [3480,402]  TR [648,433]  BL [3611,3750]  BR [658,3692]
// -> raw X runs right-to-left along the width, raw Y top-to-bottom
#define CAL_SWAP_XY  0
#define CAL_INVERT_X 1
#define CAL_INVERT_Y 0

static uint16_t s_hist[4][2];
static uint8_t s_histPos = 0;

void touch_init() {
    pinMode(T_CLK, OUTPUT);
    pinMode(T_MOSI, OUTPUT);
    pinMode(T_MISO, INPUT);
    pinMode(T_CS, OUTPUT);
    digitalWrite(T_CS, HIGH);
    digitalWrite(T_CLK, LOW);
}

// One 12-bit conversion: 8 command bits out, then the result is shifted out
// MSB first on the falling clock edges. PD bits 00 keep PENIRQ enabled.
static uint16_t xfer(uint8_t cmd) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(T_MOSI, (cmd >> i) & 1);
        digitalWrite(T_CLK, HIGH);
        delayMicroseconds(2);
        digitalWrite(T_CLK, LOW);
        delayMicroseconds(2);
    }
    digitalWrite(T_MOSI, LOW);
    uint16_t v = 0;
    for (int i = 11; i >= 0; i--) {
        digitalWrite(T_CLK, HIGH);
        delayMicroseconds(2);
        digitalWrite(T_CLK, LOW);
        delayMicroseconds(2);
        v |= (uint16_t)digitalRead(T_MISO) << i;
    }
    // Clock out the remaining bits of the 24-clock frame
    for (int i = 0; i < 4; i++) {
        digitalWrite(T_CLK, HIGH);
        delayMicroseconds(2);
        digitalWrite(T_CLK, LOW);
        delayMicroseconds(2);
    }
    return v;
}

bool touch_read_raw(uint16_t *x, uint16_t *y) {
    uint32_t sx = 0, sy = 0;
    digitalWrite(T_CS, LOW);
    xfer(0xD0);  // Discard first sample (settling)
    for (int i = 0; i < 4; i++) {
        sx += xfer(0xD0);   // X position
        sy += xfer(0x90);   // Y position
    }
    digitalWrite(T_CS, HIGH);
    *x = sx / 4;
    *y = sy / 4;
    if (*x < 50 || *y < 50 || *x > 4050 || *y > 4050) return false;
    return true;
}

void touch_record_tap(uint16_t x, uint16_t y) {
    s_hist[s_histPos][0] = x;
    s_hist[s_histPos][1] = y;
    s_histPos = (s_histPos + 1) % 4;
}

void touch_history(uint16_t out[4][2]) {
    // Oldest first
    for (int i = 0; i < 4; i++) {
        out[i][0] = s_hist[(s_histPos + i) % 4][0];
        out[i][1] = s_hist[(s_histPos + i) % 4][1];
    }
}

static float norm(uint16_t v) {
    float f = (float)((int)v - RAW_MIN) / (RAW_MAX - RAW_MIN);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

float touch_screen_x(uint16_t rawX, uint16_t rawY, uint8_t rotation) {
    float nx = norm(rawX), ny = norm(rawY);
    // Panel coordinates in its native portrait orientation (0..1)
    float px = CAL_SWAP_XY ? ny : nx;
    float py = CAL_SWAP_XY ? nx : ny;
    if (CAL_INVERT_X) px = 1 - px;
    if (CAL_INVERT_Y) py = 1 - py;
    switch (rotation & 3) {
        case 0:  return px;
        case 1:  return py;
        case 2:  return 1 - px;
        default: return 1 - py;
    }
}

#endif
