/*
 * SparkMiner - On-device WiFi menu (CYD touch screen)
 */

#include "wifi_ui.h"

#if defined(ESP32_2432S028) && USE_DISPLAY

#include <WiFi.h>
#include <TFT_eSPI.h>
#include <board_config.h>
#include "../display/display.h"
#include "../config/wifi_store.h"
#include "../config/wifi_manager.h"
#include "../extras/extras.h"
#include "touch_cal.h"

#define C_BG      0x0000
#define C_FG      0xFFFF
#define C_DIM     0x8410
#define C_PANEL   0x18E3
#define C_LINE    0x39E7
#define C_ACCENT  0xFD00
#define C_OK      0x07E0
#define C_BAD     0xF800
#define C_KEY     0x2124

#define IDLE_CLOSE_MS   180000   // Close after 3 min without a tap
#define MULTITAP_MS     1100     // Same key within this time cycles the letter
#define MAX_ITEMS       24
#define MAX_PASS        64

enum State { ST_LIST, ST_ACTION, ST_KEYPAD, ST_SYMBOLS, ST_CONNECTING, ST_RESULT, ST_MORE };
enum Mode { M_LOWER, M_UPPER, M_DIGITS };

struct Item { char ssid[33]; int rssi; bool secure; bool saved; bool inRange; };
struct Tap { float x, y; };

static volatile bool s_open = false;
static QueueHandle_t s_taps = NULL;
static TFT_eSPI *tft = nullptr;
static int W, H;

static State s_state;
static Item s_items[MAX_ITEMS];
static int s_nItems = 0, s_scroll = 0;
static int s_sel = -1;                       // Selected item
static char s_pass[MAX_PASS + 1];
static Mode s_mode = M_LOWER;
static int s_pendKey = -1, s_pendIdx = 0;    // Multi-tap state
static uint32_t s_pendAt = 0;
static bool s_showPass = false;
static bool s_resultOk = false;
static bool s_dirty = true;
static uint32_t s_lastTap = 0;
static int s_drawnState = -1;               // View currently on screen (-1 = repaint all)

// Multi-tap letters per key (index 0..9 = keys 1..9, 0)
static const char *KEYS_LOWER[10] = {".,!?@-_1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", "0 "};
static const char *KEYS_UPPER[10] = {".,!?@-_1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9", "0 "};
static const char *KEY_DIGIT = "1234567890";
static const char SYMBOLS[] = "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~";

// ============================================================
// Drawing helpers
// ============================================================

static void button(int x, int y, int w, int h, const char *label, uint16_t fill, uint16_t text, uint8_t size = 2) {
    tft->fillRoundRect(x + 1, y + 1, w - 2, h - 2, 6, fill);
    tft->drawRoundRect(x + 1, y + 1, w - 2, h - 2, 6, C_LINE);
    tft->setTextDatum(MC_DATUM);
    tft->setTextSize(size);
    tft->setTextColor(text, fill);
    tft->drawString(label, x + w / 2, y + h / 2);
}

static bool hit(const Tap &t, int x, int y, int w, int h) {
    int px = t.x * W, py = t.y * H;
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void title(const char *text, const char *right = nullptr, uint16_t rightColor = C_DIM) {
    tft->fillRect(0, 0, W, 28, C_PANEL);
    tft->setTextDatum(ML_DATUM);
    tft->setTextSize(2);
    tft->setTextColor(C_ACCENT, C_PANEL);
    tft->drawString(text, 8, 14);
    if (right) {
        tft->setTextDatum(MR_DATUM);
        tft->setTextSize(1);
        tft->setTextColor(rightColor, C_PANEL);
        tft->drawString(right, W - 6, 14);
    }
}

static void bars(int x, int y, int rssi) {
    int level = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 3;
        tft->fillRect(x + i * 5, y + 12 - bh, 3, bh, i < level ? C_FG : C_LINE);
    }
}

static void fit(char *out, const char *in, int maxChars) {
    if ((int)strlen(in) <= maxChars) { strcpy(out, in); return; }
    strncpy(out, in, maxChars - 2);
    out[maxChars - 2] = '\0';
    strcat(out, "..");
}

// ============================================================
// Network list
// ============================================================

static void scan() {
    s_drawnState = -1;
    display_lock();
    tft->fillScreen(C_BG);
    title("WiFi");
    tft->setTextDatum(MC_DATUM);
    tft->setTextSize(2);
    tft->setTextColor(C_DIM, C_BG);
    tft->drawString("Scanning...", W / 2, H / 2);
    display_unlock();

    int n = WiFi.scanNetworks(false, false);
    s_nItems = 0;
    for (int i = 0; i < n && s_nItems < MAX_ITEMS; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        int found = -1;
        for (int j = 0; j < s_nItems; j++) if (ssid == s_items[j].ssid) found = j;
        if (found >= 0) {
            if (WiFi.RSSI(i) > s_items[found].rssi) s_items[found].rssi = WiFi.RSSI(i);
            continue;
        }
        Item &it = s_items[s_nItems++];
        strlcpy(it.ssid, ssid.c_str(), sizeof(it.ssid));
        it.rssi = WiFi.RSSI(i);
        it.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        it.saved = wifi_store_find(it.ssid) != nullptr;
        it.inRange = true;
    }
    WiFi.scanDelete();
    // Strongest first
    for (int i = 1; i < s_nItems; i++) {
        Item t = s_items[i];
        int j = i - 1;
        while (j >= 0 && s_items[j].rssi < t.rssi) { s_items[j + 1] = s_items[j]; j--; }
        s_items[j + 1] = t;
    }
    // Saved networks that are not around (so they can still be forgotten)
    for (int k = 0; k < wifi_store_count() && s_nItems < MAX_ITEMS; k++) {
        const wifi_entry_t *e = wifi_store_get(k);
        bool seen = false;
        for (int j = 0; j < s_nItems; j++) if (strcmp(e->ssid, s_items[j].ssid) == 0) seen = true;
        if (seen) continue;
        Item &it = s_items[s_nItems++];
        strlcpy(it.ssid, e->ssid, sizeof(it.ssid));
        it.rssi = -127;
        it.secure = e->pass[0] != '\0';
        it.saved = true;
        it.inRange = false;
    }
    s_scroll = 0;
}

static int listRows() { return (H - 32 - 46) / 30; }

static void drawList(bool full) {
    if (full) tft->fillScreen(C_BG);
    else tft->fillRect(0, 28, W, H - 28 - 44, C_BG);
    char right[40];
    if (WiFi.status() == WL_CONNECTED) {
        char s[20];
        fit(s, WiFi.SSID().c_str(), 14);
        snprintf(right, sizeof(right), "%s %ddBm", s, WiFi.RSSI());
        title("WiFi", right, C_OK);
    } else {
        title("WiFi", "Not connected", C_BAD);
    }

    int rows = listRows();
    int maxChars = (W - 70) / 6;
    for (int r = 0; r < rows && s_scroll + r < s_nItems; r++) {
        const Item &it = s_items[s_scroll + r];
        int y = 32 + r * 30;
        bool current = WiFi.status() == WL_CONNECTED && WiFi.SSID() == it.ssid;
        tft->fillRoundRect(4, y, W - 8, 27, 4, current ? 0x0320 : C_PANEL);
        char name[40];
        fit(name, it.ssid, maxChars / 2 + 4);
        tft->setTextDatum(ML_DATUM);
        tft->setTextSize(it.inRange ? 2 : 1);
        tft->setTextColor(it.inRange ? C_FG : C_DIM, current ? 0x0320 : C_PANEL);
        tft->drawString(it.inRange ? name : (String(it.ssid) + "  (not in range)").c_str(), 10, y + 14);
        int x = W - 30;
        if (it.inRange) bars(x, y + 7, it.rssi);
        tft->setTextSize(1);
        tft->setTextDatum(MR_DATUM);
        tft->setTextColor(it.saved ? C_OK : C_DIM, current ? 0x0320 : C_PANEL);
        tft->drawString(it.saved ? "saved" : (it.secure ? "lock" : "open"), x - 4, y + 14);
    }
    if (s_nItems == 0) {
        tft->setTextDatum(MC_DATUM);
        tft->setTextSize(1);
        tft->setTextColor(C_DIM, C_BG);
        tft->drawString("No networks found - tap Scan", W / 2, 80);
    }

    int bw = W / 5, y = H - 44;
    button(0, y, bw, 44, "^", s_scroll > 0 ? C_KEY : C_BG, C_FG);
    button(bw, y, bw, 44, "v", s_scroll + rows < s_nItems ? C_KEY : C_BG, C_FG);
    button(2 * bw, y, bw, 44, "Scan", C_KEY, C_FG, 1);
    button(3 * bw, y, bw, 44, "More", C_KEY, C_FG, 1);
    button(4 * bw, y, W - 4 * bw, 44, "Close", C_KEY, C_ACCENT, 1);
}

static void connectTo(const char *ssid, const char *pass);

static void tapList(const Tap &t) {
    int rows = listRows();
    int bw = W / 5, y = H - 44;
    if (hit(t, 0, y, bw, 44)) { if (s_scroll > 0) s_scroll -= rows - 1; if (s_scroll < 0) s_scroll = 0; return; }
    if (hit(t, bw, y, bw, 44)) { if (s_scroll + rows < s_nItems) s_scroll += rows - 1; return; }
    if (hit(t, 2 * bw, y, bw, 44)) { scan(); return; }
    if (hit(t, 3 * bw, y, bw, 44)) { s_state = ST_MORE; return; }
    if (hit(t, 4 * bw, y, W - 4 * bw, 44)) { wifi_ui_close(); return; }
    for (int r = 0; r < rows && s_scroll + r < s_nItems; r++) {
        if (!hit(t, 4, 32 + r * 30, W - 8, 27)) continue;
        s_sel = s_scroll + r;
        const Item &it = s_items[s_sel];
        if (it.saved) {
            s_state = ST_ACTION;
        } else if (!it.secure) {
            connectTo(it.ssid, "");
        } else {
            s_pass[0] = '\0';
            s_pendKey = -1;
            s_mode = M_LOWER;
            s_showPass = false;
            s_state = ST_KEYPAD;
        }
        return;
    }
}

// ============================================================
// More: setup hotspot, touch calibration
// ============================================================

static void drawMore() {
    tft->fillScreen(C_BG);
    title("More");
    int bh = 52, y = 48, x = 16, w = W - 32;
    button(x, y, w, bh, "Setup hotspot", C_KEY, C_FG);
    tft->setTextDatum(TC_DATUM);
    tft->setTextSize(1);
    tft->setTextColor(C_DIM, C_BG);
    char pw[40];
    snprintf(pw, sizeof(pw), "Password: %s", extras_ap_password());
    tft->drawString(pw, W / 2, y + bh + 4);
    button(x, y + bh + 24, w, bh, "Calibrate touch", C_KEY, C_FG);
    button(x, y + 2 * bh + 48, w, bh, "Back", C_KEY, C_ACCENT);
}

static void tapMore(const Tap &t) {
    int bh = 52, y = 48, x = 16, w = W - 32;
    if (hit(t, x, y, w, bh)) extras_request_portal();   // Reboots into the hotspot
    else if (hit(t, x, y + bh + 24, w, bh)) { wifi_ui_close(); touch_cal_start(); }
    else if (hit(t, x, y + 2 * bh + 48, w, bh)) s_state = ST_LIST;
}

// ============================================================
// Saved network actions
// ============================================================

static void drawAction() {
    tft->fillScreen(C_BG);
    char name[40];
    fit(name, s_items[s_sel].ssid, W / 12);
    title(name);
    int bh = 48, y = 44, x = 16, w = W - 32;
    bool inRange = s_items[s_sel].inRange;
    button(x, y, w, bh, "Connect", inRange ? 0x0320 : C_PANEL, inRange ? C_FG : C_DIM);
    button(x, y + bh + 10, w, bh, "Change password", C_KEY, C_FG);
    button(x, y + 2 * (bh + 10), w, bh, "Forget", 0x5000, C_FG);
    button(x, y + 3 * (bh + 10), w, bh, "Back", C_KEY, C_ACCENT);
}

static void tapAction(const Tap &t) {
    int bh = 48, y = 44, x = 16, w = W - 32;
    const Item &it = s_items[s_sel];
    if (hit(t, x, y, w, bh) && it.inRange) {
        const wifi_entry_t *e = wifi_store_find(it.ssid);
        connectTo(it.ssid, e ? e->pass : "");
    } else if (hit(t, x, y + bh + 10, w, bh)) {
        s_pass[0] = '\0';
        s_pendKey = -1;
        s_mode = M_LOWER;
        s_showPass = false;
        s_state = ST_KEYPAD;
    } else if (hit(t, x, y + 2 * (bh + 10), w, bh)) {
        wifi_store_remove(it.ssid);
        s_items[s_sel].saved = false;
        if (!it.inRange) {   // Drop it from the list entirely
            memmove(&s_items[s_sel], &s_items[s_sel + 1], (s_nItems - s_sel - 1) * sizeof(Item));
            s_nItems--;
        }
        s_state = ST_LIST;
    } else if (hit(t, x, y + 3 * (bh + 10), w, bh)) {
        s_state = ST_LIST;
    }
}

// ============================================================
// Multi-tap keypad
// ============================================================

static int headerH() { return H > W ? 66 : 52; }
static int keyH() { return (H - headerH() - 42) / 4; }

static void commitPending() { s_pendKey = -1; }

static void drawKeypad(bool full) {
    if (full) tft->fillScreen(C_BG);
    char name[40];
    fit(name, s_items[s_sel].ssid, (W - 40) / 6);
    tft->fillRect(0, 0, W, headerH(), C_PANEL);
    tft->setTextDatum(TL_DATUM);
    tft->setTextSize(1);
    tft->setTextColor(C_DIM, C_PANEL);
    tft->drawString("Password for", 6, 4);
    tft->setTextColor(C_FG, C_PANEL);
    tft->drawString(name, 6, 14);
    tft->setTextDatum(TR_DATUM);
    tft->setTextColor(C_ACCENT, C_PANEL);
    tft->drawString(s_mode == M_LOWER ? "abc" : s_mode == M_UPPER ? "ABC" : "123", W - 6, 4);

    // Password field: masked except the letter being typed (or all with the eye)
    int fy = headerH() - 30;
    tft->fillRoundRect(4, fy, W - 8, 26, 4, C_BG);
    int len = strlen(s_pass);
    int maxShow = (W - 20) / 12;
    char shown[MAX_PASS + 1];
    for (int i = 0; i < len; i++) {
        bool reveal = s_showPass || (i == len - 1 && s_pendKey >= 0);
        shown[i] = reveal ? s_pass[i] : '*';
    }
    shown[len] = '\0';
    const char *view = len > maxShow ? shown + (len - maxShow) : shown;
    tft->setTextDatum(ML_DATUM);
    tft->setTextSize(2);
    tft->setTextColor(C_FG, C_BG);
    tft->drawString(view, 10, fy + 13);
    // Cursor
    int cx = 10 + strlen(view) * 12;
    tft->fillRect(cx, fy + 5, 2, 16, s_pendKey >= 0 ? C_ACCENT : C_FG);

    // 3x4 keys
    int kw = W / 3, kh = keyH(), y0 = headerH() + 2;
    static const char *labels[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
    for (int i = 0; i < 12; i++) {
        int x = (i % 3) * kw, y = y0 + (i / 3) * kh;
        int key = i < 9 ? i : (i == 10 ? 9 : -1);
        bool pend = key >= 0 && key == s_pendKey;
        uint16_t fill = pend ? 0x7A00 : C_KEY;
        tft->fillRoundRect(x + 2, y + 2, kw - 4, kh - 4, 6, fill);
        tft->setTextDatum(MC_DATUM);
        tft->setTextSize(2);
        tft->setTextColor(C_FG, fill);
        tft->drawString(labels[i], x + kw / 2, y + kh / 2 - (kh > 44 ? 8 : 6));
        const char *sub;
        if (i == 9) sub = "aA1";
        else if (i == 11) sub = "sym";
        else if (s_mode == M_DIGITS) sub = "";
        else {
            static char tmp[10];
            const char *src = (s_mode == M_UPPER ? KEYS_UPPER : KEYS_LOWER)[key];
            strlcpy(tmp, src, sizeof(tmp));
            if (key == 9) strcpy(tmp, "0 space");
            sub = tmp;
        }
        tft->setTextSize(1);
        tft->setTextColor(C_DIM, fill);
        tft->drawString(sub, x + kw / 2, y + kh / 2 + (kh > 44 ? 12 : 9));
    }

    // Actions (only the eye label changes)
    int ay = H - 40, aw = W / 4;
    if (!full) { button(aw, ay, aw, 40, s_showPass ? "hide" : "show", C_KEY, C_FG, 1); return; }
    button(0, ay, aw, 40, "<-", C_KEY, C_FG);
    button(aw, ay, aw, 40, s_showPass ? "hide" : "show", C_KEY, C_FG, 1);
    button(2 * aw, ay, aw, 40, "Cancel", C_KEY, C_ACCENT, 1);
    button(3 * aw, ay, W - 3 * aw, 40, "Join", 0x0320, C_FG);
}

static void pressKey(int key) {
    uint32_t now = millis();
    int len = strlen(s_pass);
    if (s_mode == M_DIGITS) {
        commitPending();
        if (len < MAX_PASS) { s_pass[len] = KEY_DIGIT[key]; s_pass[len + 1] = '\0'; }
        return;
    }
    const char *chars = (s_mode == M_UPPER ? KEYS_UPPER : KEYS_LOWER)[key];
    if (key == s_pendKey && now - s_pendAt < MULTITAP_MS && len > 0) {
        s_pendIdx = (s_pendIdx + 1) % strlen(chars);
        s_pass[len - 1] = chars[s_pendIdx];
    } else if (len < MAX_PASS) {
        s_pendIdx = 0;
        s_pass[len] = chars[0];
        s_pass[len + 1] = '\0';
    }
    s_pendKey = key;
    s_pendAt = now;
}

static void tapKeypad(const Tap &t) {
    int kw = W / 3, kh = keyH(), y0 = headerH() + 2;
    for (int i = 0; i < 12; i++) {
        if (!hit(t, (i % 3) * kw, y0 + (i / 3) * kh, kw, kh)) continue;
        if (i == 9) { commitPending(); s_mode = (Mode)((s_mode + 1) % 3); }
        else if (i == 11) { commitPending(); s_state = ST_SYMBOLS; }
        else pressKey(i < 9 ? i : 9);
        return;
    }
    int ay = H - 40, aw = W / 4;
    if (hit(t, 0, ay, aw, 40)) {
        commitPending();
        int len = strlen(s_pass);
        if (len) s_pass[len - 1] = '\0';
    } else if (hit(t, aw, ay, aw, 40)) {
        s_showPass = !s_showPass;
    } else if (hit(t, 2 * aw, ay, aw, 40)) {
        s_state = ST_LIST;
    } else if (hit(t, 3 * aw, ay, W - 3 * aw, 40)) {
        commitPending();
        connectTo(s_items[s_sel].ssid, s_pass);
    }
}

static void drawSymbols() {
    tft->fillScreen(C_BG);
    title("Symbols");
    int cols = 8, n = strlen(SYMBOLS), rows = (n + cols - 1) / cols;
    int cw = W / cols, ch = (H - 32 - 46) / rows;
    for (int i = 0; i < n; i++) {
        char s[2] = {SYMBOLS[i], 0};
        button((i % cols) * cw, 32 + (i / cols) * ch, cw, ch, s, C_KEY, C_FG);
    }
    button(0, H - 44, W, 44, "Back", C_KEY, C_ACCENT);
}

static void tapSymbols(const Tap &t) {
    int cols = 8, n = strlen(SYMBOLS), rows = (n + cols - 1) / cols;
    int cw = W / cols, ch = (H - 32 - 46) / rows;
    for (int i = 0; i < n; i++) {
        if (!hit(t, (i % cols) * cw, 32 + (i / cols) * ch, cw, ch)) continue;
        int len = strlen(s_pass);
        if (len < MAX_PASS) { s_pass[len] = SYMBOLS[i]; s_pass[len + 1] = '\0'; }
        s_state = ST_KEYPAD;
        return;
    }
    if (hit(t, 0, H - 44, W, 44)) s_state = ST_KEYPAD;
}

// ============================================================
// Connecting / result
// ============================================================

static void message(const char *line1, const char *line2, uint16_t color) {
    s_drawnState = -1;
    display_lock();
    tft->fillScreen(C_BG);
    title("WiFi");
    tft->setTextDatum(MC_DATUM);
    tft->setTextSize(2);
    tft->setTextColor(color, C_BG);
    tft->drawString(line1, W / 2, H / 2 - 14);
    tft->setTextSize(1);
    tft->setTextColor(C_DIM, C_BG);
    tft->drawString(line2, W / 2, H / 2 + 14);
    display_unlock();
}

static void connectTo(const char *ssid, const char *pass) {
    char name[40];
    fit(name, ssid, W / 12);
    message("Connecting...", name, C_ACCENT);
    s_state = ST_CONNECTING;

    String prev = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
    if (wifi_manager_connect(ssid, pass, 15000)) {
        wifi_store_put(ssid, pass);
        s_resultOk = true;
    } else {
        s_resultOk = false;
        // Get back online on whatever saved network is around
        message("Couldn't connect", "Reconnecting to a saved network...", C_BAD);
        wifi_manager_reconnect();
    }
    s_state = ST_RESULT;
}

static void drawResult() {
    tft->fillScreen(C_BG);
    title("WiFi");
    tft->setTextDatum(MC_DATUM);
    if (s_resultOk) {
        tft->setTextSize(2);
        tft->setTextColor(C_OK, C_BG);
        tft->drawString("Connected", W / 2, H / 2 - 40);
        tft->setTextSize(1);
        tft->setTextColor(C_FG, C_BG);
        tft->drawString(WiFi.SSID().c_str(), W / 2, H / 2 - 14);
        tft->drawString(WiFi.localIP().toString().c_str(), W / 2, H / 2 + 2);
        tft->setTextColor(C_DIM, C_BG);
        tft->drawString("Saved - it will join automatically", W / 2, H / 2 + 18);
        button(16, H - 60, W - 32, 48, "Done", 0x0320, C_FG);
    } else {
        tft->setTextSize(2);
        tft->setTextColor(C_BAD, C_BG);
        tft->drawString("Couldn't connect", W / 2, H / 2 - 40);
        tft->setTextSize(1);
        tft->setTextColor(C_DIM, C_BG);
        tft->drawString("Wrong password or weak signal", W / 2, H / 2 - 14);
        tft->drawString(WiFi.status() == WL_CONNECTED ? (String("Back on ") + WiFi.SSID()).c_str() : "Still offline",
                        W / 2, H / 2 + 4);
        button(16, H - 112, W - 32, 48, "Try again", C_KEY, C_FG);
        button(16, H - 60, W - 32, 48, "Back", C_KEY, C_ACCENT);
    }
}

static void tapResult(const Tap &t) {
    if (s_resultOk) {
        if (hit(t, 16, H - 60, W - 32, 48)) wifi_ui_close();
    } else {
        if (hit(t, 16, H - 112, W - 32, 48)) s_state = ST_KEYPAD;   // Password kept
        else if (hit(t, 16, H - 60, W - 32, 48)) { scan(); s_state = ST_LIST; }
    }
}

// ============================================================
// Task
// ============================================================

static void redraw() {
    // Repaint the whole screen only when the view changes; within a view only
    // the changing parts are redrawn (no full-screen flash on every key press)
    bool full = s_state != s_drawnState;
    display_lock();
    switch (s_state) {
        case ST_LIST:    drawList(full); break;
        case ST_ACTION:  drawAction(); break;
        case ST_KEYPAD:  drawKeypad(full); break;
        case ST_SYMBOLS: drawSymbols(); break;
        case ST_RESULT:  drawResult(); break;
        case ST_MORE:    drawMore(); break;
        default: break;
    }
    display_unlock();
    s_drawnState = s_state;
    s_dirty = false;
}

static void uiTask(void *param) {
    s_drawnState = -1;
    display_take(true);
    tft = display_tft();
    W = tft->width();
    H = tft->height();
    s_state = ST_LIST;
    s_lastTap = millis();
    scan();
    s_dirty = true;

    while (s_open) {
        Tap t;
        if (xQueueReceive(s_taps, &t, pdMS_TO_TICKS(100)) == pdTRUE) {
            s_lastTap = millis();
            switch (s_state) {
                case ST_LIST:    tapList(t); break;
                case ST_ACTION:  tapAction(t); break;
                case ST_KEYPAD:  tapKeypad(t); break;
                case ST_SYMBOLS: tapSymbols(t); break;
                case ST_RESULT:  tapResult(t); break;
                case ST_MORE:    tapMore(t); break;
                default: break;
            }
            s_dirty = true;
        }
        // Multi-tap letter timed out: fix it in place (hides it again)
        if (s_state == ST_KEYPAD && s_pendKey >= 0 && millis() - s_pendAt >= MULTITAP_MS) {
            commitPending();
            s_dirty = true;
        }
        if (millis() - s_lastTap > IDLE_CLOSE_MS) s_open = false;
        if (s_open && s_dirty) redraw();
    }

    // Hand the screen back
    memset(s_pass, 0, sizeof(s_pass));
    display_take(false);
    vTaskDelete(NULL);
}

void wifi_ui_open() {
    if (s_open) return;
    if (!s_taps) s_taps = xQueueCreate(8, sizeof(Tap));
    xQueueReset(s_taps);
    s_open = true;
    if (xTaskCreatePinnedToCore(uiTask, "WifiUI", 8192, NULL, 2, NULL, 0) != pdPASS) {
        s_open = false;
    }
}

void wifi_ui_close() {
    s_open = false;
}

bool wifi_ui_is_open() {
    return s_open;
}

void wifi_ui_touch(float fx, float fy) {
    if (!s_open || !s_taps) return;
    Tap t = {fx, fy};
    xQueueSend(s_taps, &t, 0);
}

#else

void wifi_ui_open() {}
void wifi_ui_close() {}
bool wifi_ui_is_open() { return false; }
void wifi_ui_touch(float, float) {}

#endif
