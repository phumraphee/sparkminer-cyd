/*
 * SparkMiner - Extras Implementation
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_system.h>
#include <board_config.h>
#include "extras.h"
#include "fw_update.h"
#include "history.h"
#include "../config/wifi_store.h"
#include "../config/wifi_manager.h"
#include "../ui/wifi_ui.h"
#include "../config/nvs_config.h"
#include "../display/display.h"
#include "../stats/monitor.h"
#include "../stratum/stratum.h"
#include "../mining/miner.h"

#define EXTRAS_NS               "sparkx"
#define WIFI_RESCAN_AFTER_MS    20000               // Let the built-in auto-reconnect try first
#define WIFI_RESCAN_EVERY_MS    60000               // Then look for any saved network every minute
#define HOT_C                   85.0f               // Chip temperature alert threshold
#define HOT_FOR_MS              (5UL * 60 * 1000)
#define OUTAGE_NOTIFY_MS        (10UL * 60 * 1000)  // Outages shorter than this are not reported
#define ROAM_CHECK_MS           (5UL * 60 * 1000)
#define ROAM_RSSI_WEAK          -67                 // Only look for a better AP below this signal
#define ROAM_RSSI_GAIN          8                   // ...and only move for at least this much improvement
#define WAKE_MS                 60000               // Full brightness after a tap at night

// CYD light sensor (LDR divider on an input-only ADC1 pin, fine alongside WiFi)
#if defined(ESP32_2432S028)
    #define HAVE_LIGHT_SENSOR 1
    #define LIGHT_PIN 34
#endif

// CYD discrete RGB LED is wired active-low
#if defined(ESP32_2432S028)
    #define HAVE_RGB_LED 1
    #define LED_ON  LOW
    #define LED_OFF HIGH
#endif

static extras_config_t s_cfg;
static volatile bool s_dimmed = false;
static volatile uint32_t s_wakeUntil = 0;
static volatile uint32_t s_lastActivity = 0;
static volatile bool s_blockAck = false;

// ============================================================
// Settings
// ============================================================

static void loadDefaults(extras_config_t *c) {
    memset(c, 0, sizeof(*c));
    c->notifyDaily = true;
    c->nightEnabled = false;
    c->nightStart = 22;
    c->nightEnd = 7;
    c->nightBright = 5;
    c->ledEnabled = true;
    c->autoUpdate = true;
    c->screenOffMin = 0;
    c->autoBright = false;
    c->darkOff = false;
    c->lightBright = 0;
    c->lightDark = 0;
}

void extras_init() {
    loadDefaults(&s_cfg);
    Preferences p;
    if (p.begin(EXTRAS_NS, true)) {
        p.getString("pin", s_cfg.pin, sizeof(s_cfg.pin));
        p.getString("ntfy", s_cfg.ntfyTopic, sizeof(s_cfg.ntfyTopic));
        s_cfg.notifyDaily  = p.getBool("daily", s_cfg.notifyDaily);
        s_cfg.nightEnabled = p.getBool("night", s_cfg.nightEnabled);
        s_cfg.nightStart   = p.getUChar("n_start", s_cfg.nightStart);
        s_cfg.nightEnd     = p.getUChar("n_end", s_cfg.nightEnd);
        s_cfg.nightBright  = p.getUChar("n_bright", s_cfg.nightBright);
        s_cfg.ledEnabled   = p.getBool("led", s_cfg.ledEnabled);
        s_cfg.autoUpdate   = p.getBool("auto_up", s_cfg.autoUpdate);
        s_cfg.screenOffMin = p.getUChar("scr_off", s_cfg.screenOffMin);
        s_cfg.autoBright   = p.getBool("auto_br", s_cfg.autoBright);
        s_cfg.darkOff      = p.getBool("dark_off", s_cfg.darkOff);
        s_cfg.lightBright  = p.getUShort("l_bright", s_cfg.lightBright);
        s_cfg.lightDark    = p.getUShort("l_dark", s_cfg.lightDark);
        p.end();
    }

    history_init();

    #ifdef HAVE_LIGHT_SENSOR
        analogSetPinAttenuation(LIGHT_PIN, ADC_0db);   // Most sensitive range for the CYD's LDR divider
    #endif

    #ifdef HAVE_RGB_LED
        pinMode(LED_R_PIN, OUTPUT);
        pinMode(LED_G_PIN, OUTPUT);
        pinMode(LED_B_PIN, OUTPUT);
        digitalWrite(LED_R_PIN, LED_OFF);
        digitalWrite(LED_G_PIN, LED_OFF);
        digitalWrite(LED_B_PIN, LED_OFF);
    #endif
}

extras_config_t* extras_get() {
    return &s_cfg;
}

bool extras_save() {
    Preferences p;
    if (!p.begin(EXTRAS_NS, false)) return false;
    p.putString("pin", s_cfg.pin);
    p.putString("ntfy", s_cfg.ntfyTopic);
    p.putBool("daily", s_cfg.notifyDaily);
    p.putBool("night", s_cfg.nightEnabled);
    p.putUChar("n_start", s_cfg.nightStart);
    p.putUChar("n_end", s_cfg.nightEnd);
    p.putUChar("n_bright", s_cfg.nightBright);
    p.putBool("led", s_cfg.ledEnabled);
    p.putBool("auto_up", s_cfg.autoUpdate);
    p.putUChar("scr_off", s_cfg.screenOffMin);
    p.putBool("auto_br", s_cfg.autoBright);
    p.putBool("dark_off", s_cfg.darkOff);
    p.putUShort("l_bright", s_cfg.lightBright);
    p.putUShort("l_dark", s_cfg.lightDark);
    p.end();
    return true;
}

void extras_request_portal() {
    Preferences p;
    if (p.begin(EXTRAS_NS, false)) {
        p.putBool("portal", true);
        p.end();
    }
    Serial.println("[EXTRAS] Rebooting into WiFi setup hotspot");
    delay(200);
    ESP.restart();
}

// Hotspot password: random 8 digits per device, created on first use and shown on
// screen whenever the hotspot is open (the stock "minebitcoin" is public)
const char* extras_ap_password() {
    static char pass[9] = "";
    if (pass[0]) return pass;
    Preferences p;
    if (p.begin(EXTRAS_NS, false)) {
        p.getString("ap_pass", pass, sizeof(pass));
        if (strlen(pass) != 8) {
            snprintf(pass, sizeof(pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));
            p.putString("ap_pass", pass);
        }
        p.end();
    }
    if (strlen(pass) != 8) strcpy(pass, "12345678");
    return pass;
}

bool extras_take_portal_flag() {
    Preferences p;
    if (!p.begin(EXTRAS_NS, false)) return false;
    bool flag = p.getBool("portal", false);
    if (flag) p.remove("portal");
    p.end();
    return flag;
}

bool extras_is_dimmed() {
    return s_dimmed;
}

static volatile float s_light = -1;   // EMA of the raw reading

int extras_light() {
    return s_light < 0 ? -1 : (int)(s_light + 0.5f);
}

void extras_light_calibrate(bool roomIsBright) {
    if (s_light < 0) return;
    if (roomIsBright) s_cfg.lightBright = extras_light();
    else s_cfg.lightDark = extras_light();
    extras_save();
}

static void lightSample() {
    #ifdef HAVE_LIGHT_SENSOR
        int sum = 0;
        for (int i = 0; i < 8; i++) sum += analogRead(LIGHT_PIN);
        float v = sum / 8.0f;
        s_light = s_light < 0 ? v : s_light * 0.8f + v * 0.2f;
    #endif
}

// 0..1 how lit the room is, from the two calibration points (-1 = not calibrated)
static float roomLight() {
    if (s_light < 0 || s_cfg.lightBright == s_cfg.lightDark) return -1;
    float f = (s_light - s_cfg.lightDark) / ((float)s_cfg.lightBright - s_cfg.lightDark);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

void extras_activity() {
    s_lastActivity = millis();
}

void extras_wake() {
    s_lastActivity = millis();
    s_wakeUntil = millis() + WAKE_MS;
    if (s_dimmed) {   // Light up right away rather than on the next 1 s tick
        s_dimmed = false;
        display_set_brightness(nvs_config_get()->brightness);
    }
    // (the next tick settles on the auto-brightness level if that is enabled)
}

void extras_ack_block() {
    s_blockAck = true;
}

// ============================================================
// Notifications (ntfy.sh over plain HTTP - no TLS cost on the miner)
// ============================================================

bool extras_notify(const char *title, const char *message, const char *tags, int priority) {
    if (!s_cfg.ntfyTopic[0] || WiFi.status() != WL_CONNECTED) return false;

    char url[96];
    snprintf(url, sizeof(url), "http://ntfy.sh/%s", s_cfg.ntfyTopic);

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(5000);
    if (!http.begin(client, url)) return false;
    http.addHeader("Title", title);
    if (tags && tags[0]) http.addHeader("Tags", tags);
    http.addHeader("Priority", String(priority));
    int code = http.POST((uint8_t *)message, strlen(message));
    http.end();

    Serial.printf("[NOTIFY] '%s' -> %d\n", title, code);
    return code == 200;
}

static void fmtDuration(char *out, size_t cap, uint32_t seconds) {
    uint32_t d = seconds / 86400, h = seconds % 86400 / 3600, m = seconds % 3600 / 60;
    if (d) snprintf(out, cap, "%lud %luh", (unsigned long)d, (unsigned long)h);
    else if (h) snprintf(out, cap, "%luh %lum", (unsigned long)h, (unsigned long)m);
    else snprintf(out, cap, "%lum", (unsigned long)m);
}

static const char* resetReasonText() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power on";
        case ESP_RST_SW:       return "restart";
        case ESP_RST_PANIC:    return "crash (panic)";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout (weak power supply?)";
        case ESP_RST_DEEPSLEEP:return "deep sleep";
        default:               return "other";
    }
}

// ============================================================
// RGB LED
// ============================================================

static void ledSet(bool r, bool g, bool b) {
    #ifdef HAVE_RGB_LED
        digitalWrite(LED_R_PIN, r ? LED_ON : LED_OFF);
        digitalWrite(LED_G_PIN, g ? LED_ON : LED_OFF);
        digitalWrite(LED_B_PIN, b ? LED_ON : LED_OFF);
    #endif
}

// Called every 100 ms
static void ledUpdate(bool wifiUp, bool poolUp, bool shareFlash, bool blockParty, bool night) {
    static uint8_t tick = 0;
    tick++;
    if (blockParty) {
        // Cycle colors until acknowledged (tap / BOOT click)
        uint8_t phase = (tick / 3) % 6;
        static const bool seq[6][3] = {{1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1},{1,0,1}};
        ledSet(seq[phase][0], seq[phase][1], seq[phase][2]);
        return;
    }
    if (!s_cfg.ledEnabled || night) { ledSet(0, 0, 0); return; }
    if (shareFlash)  { ledSet(0, 1, 0); return; }                  // Green flash: share accepted
    if (!wifiUp)     { ledSet((tick % 10) < 2, 0, 0); return; }    // Red blink: no WiFi
    if (!poolUp)     { ledSet(0, 0, (tick % 10) < 2); return; }    // Blue blink: no pool
    ledSet(0, 0, 0);                                               // Mining normally: dark
}

// ============================================================
// WiFi roaming: move to a clearly stronger AP with the same SSID
// ============================================================

static void roamCheck() {
    if (WiFi.status() != WL_CONNECTED) return;
    int cur = WiFi.RSSI();
    if (cur >= ROAM_RSSI_WEAK) return;

    miner_config_t *config = nvs_config_get();
    String ssid = WiFi.SSID();
    uint8_t curBssid[6];
    memcpy(curBssid, WiFi.BSSID(), 6);

    int n = WiFi.scanNetworks(false, false, false, 120);
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) != ssid) continue;
        if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    }
    if (best >= 0 && memcmp(WiFi.BSSID(best), curBssid, 6) != 0 &&
        WiFi.RSSI(best) >= cur + ROAM_RSSI_GAIN) {
        uint8_t bssid[6];
        memcpy(bssid, WiFi.BSSID(best), 6);
        int ch = WiFi.channel(best);
        Serial.printf("[WIFI] Roaming: %d dBm -> %d dBm (ch %d)\n", cur, WiFi.RSSI(best), ch);
        WiFi.scanDelete();
        const wifi_entry_t *e = wifi_store_find(ssid.c_str());
        WiFi.begin(ssid.c_str(), e ? e->pass : config->wifiPassword, ch, bssid);
        return;
    }
    WiFi.scanDelete();
}

// ============================================================
// Task
// ============================================================

void extras_task(void *param) {
    Serial.printf("[EXTRAS] Task started on core %d\n", xPortGetCoreID());
    s_lastActivity = millis();
    miner_config_t *config = nvs_config_get();

    bool bootNotified = false;
    uint32_t wifiDownSince = 0, poolDownSince = 0, lastRescan = 0;
    uint32_t hotSince = 0;
    bool hotReported = false;
    bool poolOutageReported = false;
    uint32_t lastAccepted = miner_get_stats()->accepted;
    uint32_t lastBlocks = miner_get_stats()->blocks;
    uint32_t shareFlashUntil = 0;
    bool blockParty = false;
    uint32_t lastRoam = millis();
    int lastDailyYday = -1;
    uint32_t lastSlow = 0;
    bool night = false;
    uint32_t zeroHashSince = 0;

    for (;;) {
        uint32_t now = millis();
        bool wifiUp = WiFi.status() == WL_CONNECTED;
        bool poolUp = stratum_is_connected();
        mining_stats_t *ms = miner_get_stats();

        // --- Share / block events (checked every 100 ms) ---
        if (ms->accepted != lastAccepted) {
            lastAccepted = ms->accepted;
            shareFlashUntil = now + 400;
        }
        if (ms->blocks != lastBlocks) {
            lastBlocks = ms->blocks;
            blockParty = true;
            s_blockAck = false;
            char msg[160];
            snprintf(msg, sizeof(msg), "Your miner found a Bitcoin block! Check your wallet: %s", config->wallet);
            extras_notify("BLOCK FOUND!", msg, "tada,moneybag", 5);
        }
        if (blockParty && s_blockAck) blockParty = false;

        ledUpdate(wifiUp, poolUp, (int32_t)(shareFlashUntil - now) > 0, blockParty, night);

        // --- Once per second ---
        if (now - lastSlow >= 1000) {
            lastSlow = now;

            // Boot notification once WiFi is up
            if (!bootNotified && wifiUp) {
                bootNotified = true;
                char msg[160];
                snprintf(msg, sizeof(msg), "Reason: %s\nIP: %s\nSignal: %d dBm",
                         resetReasonText(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
                extras_notify("Miner started", msg, "zap", 2);
            }

            // WiFi outage tracking + watchdog
            if (!wifiUp) {
                if (!wifiDownSince) wifiDownSince = now;
                // Moved somewhere else or the router changed: join the strongest saved
                // network in range (not while the user is driving the WiFi menu)
                if (now - wifiDownSince > WIFI_RESCAN_AFTER_MS && now - lastRescan > WIFI_RESCAN_EVERY_MS &&
                    !wifi_ui_is_open()) {
                    lastRescan = now;
                    wifi_manager_reconnect();
                }
            } else if (wifiDownSince) {
                uint32_t outage = now - wifiDownSince;
                wifiDownSince = 0;
                if (outage > OUTAGE_NOTIFY_MS) {
                    char d[16], msg[96];
                    fmtDuration(d, sizeof(d), outage / 1000);
                    snprintf(msg, sizeof(msg), "WiFi was down for %s, back online now.", d);
                    extras_notify("Miner back online", msg, "white_check_mark", 3);
                }
            }

            // Pool outage (while WiFi works)
            if (wifiUp && !poolUp) {
                if (!poolDownSince) poolDownSince = now;
                if (!poolOutageReported && now - poolDownSince > OUTAGE_NOTIFY_MS) {
                    poolOutageReported = extras_notify("Pool disconnected",
                        "WiFi is fine but the miner has not reached the pool for 10 minutes.", "warning", 4);
                }
            } else if (poolUp && poolDownSince) {
                if (poolOutageReported) {
                    char d[16], msg[96];
                    fmtDuration(d, sizeof(d), (now - poolDownSince) / 1000);
                    snprintf(msg, sizeof(msg), "Pool connection restored after %s.", d);
                    extras_notify("Pool reconnected", msg, "white_check_mark", 3);
                }
                poolDownSince = 0;
                poolOutageReported = false;
            }

            // Overheat: chip sensor above HOT_C for 5 minutes (sensor is rough, +-10 C)
            float tempC = temperatureRead();
            if (tempC > HOT_C) {
                if (!hotSince) hotSince = now;
                if (!hotReported && now - hotSince > HOT_FOR_MS) {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "Chip at %.0f C for 5 minutes. Move it somewhere with more airflow.", tempC);
                    hotReported = extras_notify("Miner running hot", msg, "fire", 4);
                }
            } else if (tempC < HOT_C - 10) {
                hotSince = 0;
                hotReported = false;
            }

            // "Connected but dead": pool link up yet nothing hashing for 90 s -> reconnect
            if (poolUp && monitor_get_data()->hashRate < 1.0) {
                if (!zeroHashSince) zeroHashSince = now;
                if (now - zeroHashSince > 90000) {
                    Serial.println("[EXTRAS] Pool connected but hashrate 0 for 90 s - reconnecting");
                    stratum_reconnect();
                    zeroHashSince = 0;
                }
            } else {
                zeroHashSince = 0;
            }

            // Local time: night mode + daily summary
            struct tm t;
            bool haveTime = getLocalTime(&t, 0);
            if (haveTime) {
                bool inWindow = s_cfg.nightStart <= s_cfg.nightEnd
                    ? (t.tm_hour >= s_cfg.nightStart && t.tm_hour < s_cfg.nightEnd)
                    : (t.tm_hour >= s_cfg.nightStart || t.tm_hour < s_cfg.nightEnd);
                night = s_cfg.nightEnabled && inWindow;

                if (s_cfg.notifyDaily && t.tm_hour == 9 && t.tm_yday != lastDailyYday) {
                    lastDailyYday = t.tm_yday;
                    const display_data_t *d = monitor_get_data();
                    char up[16], msg[200];
                    fmtDuration(up, sizeof(up), d->uptimeSeconds);
                    snprintf(msg, sizeof(msg),
                        "Hashrate: %.0f kH/s\nShares: %lu accepted\nBest difficulty: %.4g\nBlocks: %lu\nUptime: %s\nSignal: %d dBm",
                        d->hashRate / 1000.0, (unsigned long)d->sharesAccepted, d->bestDifficulty,
                        (unsigned long)d->blocksFound, up, WiFi.RSSI());
                    extras_notify("Daily miner report", msg, "bar_chart", 2);
                }
            }

            // Backlight: night level unless woken by a tap
            // Backlight level, most restrictive rule wins:
            //   room light (auto)  -> 10 %..set brightness
            //   dark room / night  -> off / night level   (a tap lights it for a minute)
            //   idle timeout       -> off                 (a tap lights it until the next timeout)
            lightSample();
            bool woken = (int32_t)(s_wakeUntil - now) > 0;
            float room = roomLight();
            int level = config->brightness;
            if (s_cfg.autoBright && room >= 0) level = 10 + (int)((config->brightness - 10) * room);
            bool darkRoom = s_cfg.darkOff && room >= 0 && room < 0.15f;
            bool scheduleDim = false;
            if (darkRoom && !woken) { level = 0; scheduleDim = true; }
            if (night && !woken && level > s_cfg.nightBright) { level = s_cfg.nightBright; scheduleDim = true; }
            if (s_cfg.screenOffMin && now - s_lastActivity > s_cfg.screenOffMin * 60000UL) { level = 0; scheduleDim = true; }
            if (blockParty) { level = config->brightness; scheduleDim = false; }
            s_dimmed = scheduleDim;   // A tap on a dimmed screen only wakes it
            static int applied = -1;
            if (applied < 0 || abs(level - applied) >= 3 || (level == 0) != (applied == 0)) {
                applied = level;
                display_set_brightness(level);
            }

            history_tick();

            // Signed self-update from GitHub Releases (on schedule or when asked)
            if (wifiUp) fw_update_poll(s_cfg.autoUpdate);

            // Roaming between access points with the same name
            if (wifiUp && now - lastRoam >= ROAM_CHECK_MS && !wifi_ui_is_open()) {
                lastRoam = now;
                roamCheck();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
