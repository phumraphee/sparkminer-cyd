/*
 * SparkMiner - 24 h history for the web UI charts
 */

#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "history.h"
#include "../display/display.h"
#include "../stats/monitor.h"
#include "../stratum/stratum.h"

#define SAMPLE_SEC   300
#define SLOTS        288                 // 24 h
#define SAVE_MS      (60UL * 60 * 1000)  // Hourly NVS write (wear stays negligible)

typedef struct __attribute__((packed)) {
    uint32_t t;      // Unix time
    uint16_t khs;    // Hashrate in kH/s
    int8_t tempC;
    int8_t rssi;     // 0 when WiFi was down
    uint8_t pool;    // 1 = pool connected
} sample_t;

static sample_t s_buf[SLOTS];
static uint16_t s_head = 0, s_count = 0;   // s_head = next write slot
static uint32_t s_lastSampleT = 0, s_lastSave = 0;
static SemaphoreHandle_t s_mutex = NULL;

void history_init() {
    s_mutex = xSemaphoreCreateMutex();
    Preferences p;
    if (p.begin("sparkhist", true)) {
        size_t len = p.getBytesLength("buf");
        if (len == sizeof(s_buf)) {
            p.getBytes("buf", s_buf, sizeof(s_buf));
            s_head = p.getUShort("head", 0) % SLOTS;
            s_count = min((int)p.getUShort("count", 0), SLOTS);
        }
        p.end();
    }
    s_lastSave = millis();
}

void history_save() {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    Preferences p;
    if (p.begin("sparkhist", false)) {
        p.putBytes("buf", s_buf, sizeof(s_buf));
        p.putUShort("head", s_head);
        p.putUShort("count", s_count);
        p.end();
    }
    xSemaphoreGive(s_mutex);
    s_lastSave = millis();
}

void history_tick() {
    time_t now = time(nullptr);
    if (now < 1700000000) return;                    // Wait for NTP
    if (s_lastSampleT == 0 && s_count) {
        s_lastSampleT = s_buf[(s_head + SLOTS - 1) % SLOTS].t;   // Resume the restored series
    }
    if (millis() < 120000) return;                   // Skip the boot ramp-up (no job / hashrate yet)
    if ((uint32_t)now - s_lastSampleT >= SAMPLE_SEC) {
        const display_data_t *d = monitor_get_data();
        sample_t smp;
        smp.t = now;
        smp.khs = (uint16_t)min(65535.0, d->hashRate / 1000.0);
        smp.tempC = (int8_t)constrain((int)temperatureRead(), -40, 127);
        smp.rssi = WiFi.status() == WL_CONNECTED ? (int8_t)WiFi.RSSI() : 0;
        smp.pool = stratum_is_connected() ? 1 : 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_buf[s_head] = smp;
        s_head = (s_head + 1) % SLOTS;
        if (s_count < SLOTS) s_count++;
        xSemaphoreGive(s_mutex);
        s_lastSampleT = now;
    }
    if (millis() - s_lastSave > SAVE_MS) history_save();
}

void history_json(String &out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out.reserve(out.length() + s_count * 26 + 80);
    const char *names[] = {"t", "hr", "temp", "rssi", "pool"};
    out += "{\"interval\":";
    out += SAMPLE_SEC;
    for (int f = 0; f < 5; f++) {
        out += ",\"";
        out += names[f];
        out += "\":[";
        for (int i = 0; i < s_count; i++) {
            const sample_t &s = s_buf[(s_head + SLOTS - s_count + i) % SLOTS];
            if (i) out += ',';
            switch (f) {
                case 0: out += s.t; break;
                case 1: out += s.khs; break;
                case 2: out += s.tempC; break;
                case 3: out += s.rssi; break;
                default: out += s.pool; break;
            }
        }
        out += "]";
    }
    out += "}";
    xSemaphoreGive(s_mutex);
}
