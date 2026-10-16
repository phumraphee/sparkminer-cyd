/*
 * SparkMiner - CYD resistive touch (XPT2046) position reader
 */

#include "touch_cyd.h"
#include <Preferences.h>

#ifdef HAVE_TOUCH_POSITION

// CYD touch controller pins (separate from the display SPI bus)
#define T_CLK   25
#define T_MOSI  32
#define T_MISO  39
#define T_CS    33

// Raw range per axis, from the corner taps below (edges slightly padded)
#define RAW_X_MIN 600
#define RAW_X_MAX 3650
#define RAW_Y_MIN 350
#define RAW_Y_MAX 3800

// Portrait (rotation 0) calibration from corner taps on the real board:
// TL [3480,402]  TR [648,433]  BL [3611,3750]  BR [658,3692]
// -> raw X runs right-to-left along the width, raw Y top-to-bottom
#define CAL_SWAP_XY  0
#define CAL_INVERT_X 1
#define CAL_INVERT_Y 0

#define TOUCH_Z_MIN  120   // Minimum pressure (Z1) for a trustworthy position

// Affine calibration from raw to panel-portrait fractions:
//   px = c[0]*rx + c[1]*ry + c[2],  py = c[3]*rx + c[4]*ry + c[5]
static float s_cal[6];
static bool s_calValid = false;

static uint16_t s_hist[4][2];
static uint8_t s_histPos = 0;

void touch_init() {
    Preferences p;
    if (p.begin("sparktouch", true)) {
        s_calValid = p.getBytes("cal", s_cal, sizeof(s_cal)) == sizeof(s_cal);
        p.end();
    }
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

static uint16_t median5(uint16_t *v) {
    for (int i = 1; i < 5; i++) {
        uint16_t t = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; }
        v[j + 1] = t;
    }
    return v[2];
}

bool touch_read_raw(uint16_t *x, uint16_t *y) {
    uint16_t xs[5], ys[5];
    digitalWrite(T_CS, LOW);
    // Pressure: Z1 is near 0 when nothing (or only a light graze) is on the panel,
    // which is exactly when X/Y readings drift - reject those samples
    uint16_t z1 = xfer(0xB0);
    xfer(0xD0);  // Settle
    for (int i = 0; i < 5; i++) {
        xs[i] = xfer(0xD0);   // X position
        ys[i] = xfer(0x90);   // Y position
    }
    uint16_t z1b = xfer(0xB0);
    digitalWrite(T_CS, HIGH);
    if (z1 < TOUCH_Z_MIN || z1b < TOUCH_Z_MIN) return false;
    *x = median5(xs);
    *y = median5(ys);
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

static float norm(uint16_t v, int lo, int hi) {
    float f = (float)((int)v - lo) / (hi - lo);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

static void rawToPanel(uint16_t rawX, uint16_t rawY, float *px, float *py) {
    if (s_calValid) {
        *px = s_cal[0] * rawX + s_cal[1] * rawY + s_cal[2];
        *py = s_cal[3] * rawX + s_cal[4] * rawY + s_cal[5];
    } else {
        float nx = norm(rawX, RAW_X_MIN, RAW_X_MAX), ny = norm(rawY, RAW_Y_MIN, RAW_Y_MAX);
        *px = CAL_SWAP_XY ? ny : nx;
        *py = CAL_SWAP_XY ? nx : ny;
        if (CAL_INVERT_X) *px = 1 - *px;
        if (CAL_INVERT_Y) *py = 1 - *py;
    }
    *px = *px < 0 ? 0 : (*px > 1 ? 1 : *px);
    *py = *py < 0 ? 0 : (*py > 1 ? 1 : *py);
}

void touch_screen_to_panel(float sx, float sy, uint8_t rotation, float *px, float *py) {
    // Inverse of the rotation mapping in touch_screen_xy
    switch (rotation & 3) {
        case 0:  *px = sx;     *py = sy;     break;
        case 1:  *px = 1 - sy; *py = sx;     break;
        case 2:  *px = 1 - sx; *py = 1 - sy; break;
        default: *px = sy;     *py = 1 - sx; break;
    }
}

// Least-squares affine fit of n raw points to panel fractions; returns max error
static bool solve3(double m[3][4], double out[3]) {
    for (int c = 0; c < 3; c++) {
        int piv = c;
        for (int r = c + 1; r < 3; r++) if (fabs(m[r][c]) > fabs(m[piv][c])) piv = r;
        if (fabs(m[piv][c]) < 1e-9) return false;
        for (int k = 0; k < 4; k++) { double t = m[c][k]; m[c][k] = m[piv][k]; m[piv][k] = t; }
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            double f = m[r][c] / m[c][c];
            for (int k = c; k < 4; k++) m[r][k] -= f * m[c][k];
        }
    }
    for (int i = 0; i < 3; i++) out[i] = m[i][3] / m[i][i];
    return true;
}

float touch_fit_calibration(const uint16_t raw[][2], const float panel[][2], int n, float outCal[6]) {
    for (int axis = 0; axis < 2; axis++) {
        double m[3][4] = {{0}};
        for (int i = 0; i < n; i++) {
            double r[3] = {(double)raw[i][0], (double)raw[i][1], 1.0};
            for (int a = 0; a < 3; a++) {
                for (int b = 0; b < 3; b++) m[a][b] += r[a] * r[b];
                m[a][3] += r[a] * panel[i][axis];
            }
        }
        double sol[3];
        if (!solve3(m, sol)) return 1e9;
        for (int k = 0; k < 3; k++) outCal[axis * 3 + k] = (float)sol[k];
    }
    float worst = 0;
    for (int i = 0; i < n; i++) {
        float ex = outCal[0] * raw[i][0] + outCal[1] * raw[i][1] + outCal[2] - panel[i][0];
        float ey = outCal[3] * raw[i][0] + outCal[4] * raw[i][1] + outCal[5] - panel[i][1];
        worst = max(worst, max(fabsf(ex), fabsf(ey)));
    }
    return worst;
}

void touch_set_calibration(const float cal[6], bool save) {
    memcpy(s_cal, cal, sizeof(s_cal));
    s_calValid = true;
    if (save) {
        Preferences p;
        if (p.begin("sparktouch", false)) { p.putBytes("cal", s_cal, sizeof(s_cal)); p.end(); }
    }
}

void touch_reload_calibration() {
    Preferences p;
    s_calValid = false;
    if (p.begin("sparktouch", true)) {
        s_calValid = p.getBytes("cal", s_cal, sizeof(s_cal)) == sizeof(s_cal);
        p.end();
    }
}

// Rotation mapping follows TFT_eSPI's ILI9341 MADCTL table:
// r0 MX, r1 MV, r2 MY, r3 MX|MY|MV (see TFT_Drivers/ILI9341_Rotation.h)
void touch_screen_xy(uint16_t rawX, uint16_t rawY, uint8_t rotation, float *sx, float *sy) {
    // Panel coordinates in its native portrait orientation (0..1)
    float px, py;
    rawToPanel(rawX, rawY, &px, &py);
    switch (rotation & 3) {
        case 0:  *sx = px;     *sy = py;     break;
        case 1:  *sx = py;     *sy = 1 - px; break;
        case 2:  *sx = 1 - px; *sy = 1 - py; break;
        default: *sx = 1 - py; *sy = px;     break;
    }
}

float touch_screen_x(uint16_t rawX, uint16_t rawY, uint8_t rotation) {
    float sx, sy;
    touch_screen_xy(rawX, rawY, rotation, &sx, &sy);
    return sx;
}

#endif
