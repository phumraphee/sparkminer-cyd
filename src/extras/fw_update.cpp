/*
 * SparkMiner - Self-update from GitHub Releases
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <board_config.h>
#include "fw_update.h"
#include "extras.h"
#include "history.h"
#include "../mining/miner.h"

#ifdef UPDATE_REPO

#define FIRST_CHECK_MS  (3UL * 60 * 1000)        // After boot
#define CHECK_EVERY_MS  (6UL * 60 * 60 * 1000)   // Then every 6 hours

// Release signing key (private half lives only in the developer's macOS Keychain)
static const char UPDATE_PUBKEY[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE+6XCWDl+54axQKYYOKlNdE6PpSMa\n"
    "+8IAEIAuc/i07ZyhjumELzUCNfXKGZAK/NFqTJQVScdTgsC/F6EzsGQ+IA==\n"
    "-----END PUBLIC KEY-----\n";

#define CONFIRM_AFTER_MS  (3UL * 60 * 1000)   // New firmware must stay up this long with the pool...
#define HEALTHY_WITHIN_MS (10UL * 60 * 1000)  // ...and get there within this, or it is rolled back
#define SHORT_BOOT_MS     (2UL * 60 * 1000)   // A boot that dies sooner counts towards a crash loop
#define CRASH_LOOP_BOOTS  4

// Keep the bootloader's rollback armed after an OTA (Arduino confirms every image at
// boot by default). fw_update_health() confirms it once the pool is reachable;
// any reboot before that makes the bootloader return to the previous firmware.
extern "C" bool verifyRollbackLater() { return true; }

static bool s_pendingVerify = false;
static uint32_t s_badBuild = 0;           // A build that was rolled back - never reinstall it

static volatile bool s_requested = false;
static uint32_t s_lastCheck = 0;
static bool s_checkedOnce = false;
static char s_state[64] = "Not checked yet";

static void setState(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_state, sizeof(s_state), fmt, ap);
    va_end(ap);
    Serial.printf("[UPDATE] %s\n", s_state);
}

void fw_update_request() {
    s_requested = true;
}

const char* fw_update_state() {
    return s_state;
}

static bool httpsGet(HTTPClient &http, WiFiClientSecure &client, const String &url) {
    client.setInsecure();   // Authenticity comes from the signature, not the TLS chain
    client.setTimeout(15000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent("SparkMiner-CYD");
    if (!http.begin(client, url)) return false;
    return http.GET() == HTTP_CODE_OK;
}

static bool verifySignature(uint32_t build, const uint8_t sha[32], const char *sigB64) {
    // Signed message: "<build>:<sha256 hex>" (binds the version, so old builds can't be replayed)
    char msg[80];
    int n = snprintf(msg, sizeof(msg), "%lu:", (unsigned long)build);
    for (int i = 0; i < 32; i++) n += snprintf(msg + n, sizeof(msg) - n, "%02x", sha[i]);

    uint8_t digest[32];
    mbedtls_sha256_ret((const uint8_t *)msg, n, digest, 0);

    uint8_t sig[80];
    size_t sigLen = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen, (const uint8_t *)sigB64, strlen(sigB64)) != 0) {
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const uint8_t *)UPDATE_PUBKEY, sizeof(UPDATE_PUBKEY)) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, sigLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

// Download straight into the spare OTA slot, resuming with HTTP Range after a drop
// (keeps one Update session and one running hash); true once activated
static bool downloadAndInstall(const char *tag, const char *file, uint32_t size, const uint8_t expected[32]) {
    String url = String("https://raw.githubusercontent.com/" UPDATE_REPO "/ota/") + file;
    miner_sha_hold();
    if (!Update.begin(size, U_FLASH)) {
        miner_sha_release();
        setState("Update begin failed: %s", Update.errorString());
        return false;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);
    static uint8_t buf[1024];
    uint32_t got = 0, lastPct = 0;
    bool writeFailed = false;

    int lastCode = 0;
    for (int conn = 0; conn < 15 && got < size && !writeFailed; conn++) {
        if (conn) {
            setState("Connection dropped at %lu%% (HTTP %d) - resuming", (unsigned long)(got * 100 / size), lastCode);
            vTaskDelay(pdMS_TO_TICKS(2000 + conn * 1000));
        }
        WiFiClientSecure client;
        HTTPClient http;
        client.setInsecure();
        client.setTimeout(15000);
        http.setTimeout(15000);
        http.setUserAgent("SparkMiner-CYD");
        if (!http.begin(client, url)) continue;
        if (got) http.addHeader("Range", String("bytes=") + got + "-");
        int code = http.GET();
        lastCode = code;
        if (code != (got ? 206 : 200)) { http.end(); continue; }

        WiFiClient *stream = http.getStreamPtr();
        uint32_t lastData = millis();
        while (got < size && millis() - lastData < 20000) {
            size_t avail = stream->available();
            if (!avail) {
                if (!stream->connected()) break;
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            int r = stream->readBytes(buf, min(avail, min(sizeof(buf), (size_t)(size - got))));
            if (r <= 0) continue;
            lastData = millis();
            mbedtls_sha256_update_ret(&ctx, buf, r);
            if (Update.write(buf, r) != (size_t)r) { writeFailed = true; break; }
            got += r;
            vTaskDelay(1);
            uint32_t pct = got * 100 / size;
            if (pct >= lastPct + 10) { lastPct = pct; setState("Downloading %s: %lu%%", tag, (unsigned long)pct); }
        }
        http.end();
    }

    uint8_t actual[32];
    mbedtls_sha256_finish_ret(&ctx, actual);
    mbedtls_sha256_free(&ctx);

    if (got != size) {
        Update.abort();
        miner_sha_release();
        if (writeFailed) setState("Flash write failed: %s", Update.errorString());
        else setState("Download failed at %lu%%", (unsigned long)(got * 100 / size));
        return false;
    }
    if (memcmp(actual, expected, 32) != 0) {
        Update.abort();
        miner_sha_release();
        setState("Download corrupt - discarded");
        return false;
    }
    bool ok = Update.end(true);
    miner_sha_release();
    if (!ok) setState("Install failed: %s", Update.errorString());
    return ok;
}

static void checkNow() {
    if (WiFi.status() != WL_CONNECTED) { setState("No WiFi"); return; }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next || next == esp_ota_get_running_partition()) {
        setState("No spare firmware slot");
        return;
    }

    // --- Manifest ---
    StaticJsonDocument<512> m;
    {
        WiFiClientSecure client;
        HTTPClient http;
        miner_sha_hold();
        // raw.githubusercontent.com serves files directly (Release downloads go through
        // two redirects to a ~900-char signed URL, which the ESP32 HTTP client can't follow)
        bool ok = httpsGet(http, client, "https://raw.githubusercontent.com/" UPDATE_REPO "/ota/manifest.json");
        String body = ok ? http.getString() : String();
        http.end();
        miner_sha_release();
        if (!ok || deserializeJson(m, body)) { setState("Could not read manifest"); return; }
    }

    uint32_t build = m["build"] | 0;
    const char *tag = m["tag"] | "";
    uint32_t size = m["size"] | 0;
    const char *shaHex = m["sha256"] | "";
    const char *sig = m["sig"] | "";
    const char *file = m["file"] | "";

    if (build == s_badBuild) {
        setState("%s was rolled back - waiting for a newer build", tag);
        return;
    }
    if (build <= FW_BUILD) {
        setState("Up to date (%s)", tag[0] ? tag : AUTO_VERSION);
        return;
    }
    if (!tag[0] || !file[0] || !size || strlen(shaHex) != 64 || !sig[0]) {
        setState("Manifest incomplete");
        return;
    }

    uint8_t expected[32];
    for (int i = 0; i < 32; i++) {
        char b[3] = {shaHex[2 * i], shaHex[2 * i + 1], 0};
        expected[i] = (uint8_t)strtoul(b, nullptr, 16);
    }
    // Check the signature before downloading anything
    miner_sha_hold();   // The digest is computed with the SHA unit the miner drives directly
    bool sigOk = verifySignature(build, expected, sig);
    miner_sha_release();
    if (!sigOk) {
        setState("Bad signature on %s - ignored", tag);
        return;
    }

    // --- Firmware download (miner parked: TLS + image hash use the SHA unit) ---
    if (!downloadAndInstall(tag, file, size, expected)) return;

    {
        Preferences p;
        if (p.begin("sparkupd", false)) { p.putUInt("pending", build); p.end(); }
    }
    setState("Installed %s - restarting", tag);
    char msg[96];
    snprintf(msg, sizeof(msg), "Installed %s (was %s). Restarting now.", tag, AUTO_VERSION);
    extras_notify("Miner updated", msg, "arrow_up", 2);
    history_save();
    delay(500);
    ESP.restart();
}

static volatile bool s_running = false;

// TLS + JSON + ECDSA verification need far more stack than the extras task has
static void updateTask(void *param) {
    // The core-0 miner never blocks; with it and this task both busy at priority 1,
    // IDLE0 can't feed the task watchdog during a download. Pause it meanwhile.
    miner_core0_hold(true);
    checkNow();             // Reboots on success
    miner_core0_hold(false);
    s_running = false;
    vTaskDelete(NULL);
}

static bool s_bootCounted = false;

void fw_update_boot() {
    Preferences p;
    p.begin("sparkupd", false);

    // Crash-loop guard (also covers firmware that was already confirmed): too many
    // short-lived boots in a row -> go back to the other firmware slot if it is valid
    uint8_t shortBoots = p.getUChar("short", 0) + 1;
    p.putUChar("short", shortBoots);
    s_bootCounted = true;
    if (shortBoots >= CRASH_LOOP_BOOTS) {
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        esp_ota_img_states_t ost;
        bool otherOk = other && other != esp_ota_get_running_partition() &&
                       (esp_ota_get_state_partition(other, &ost) != ESP_OK || ost == ESP_OTA_IMG_VALID);
        if (otherOk && esp_ota_set_boot_partition(other) == ESP_OK) {
            p.putUInt("bad", (uint32_t)FW_BUILD);
            p.putUChar("short", 0);
            p.end();
            Serial.println("[UPDATE] Crash loop detected - switching back to the previous firmware");
            delay(200);
            ESP.restart();
        }
    }

    s_badBuild = p.getUInt("bad", 0);
    uint32_t pending = p.getUInt("pending", 0);

    esp_ota_img_states_t st;
    const esp_partition_t *running = esp_ota_get_running_partition();
    s_pendingVerify = esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;

    // Came back from a rollback: the build we tried is bad
    if (!s_pendingVerify && pending && pending != (uint32_t)FW_BUILD && esp_ota_get_last_invalid_partition()) {
        s_badBuild = pending;
        p.putUInt("bad", s_badBuild);
        p.remove("pending");
        setState("Update failed health check - rolled back");
        p.end();
        extras_notify("Update rolled back",
                      "The new firmware didn't reach the pool, so the miner went back to the previous version.",
                      "warning", 4);
        return;
    }
    p.end();
    if (s_pendingVerify) Serial.println("[UPDATE] New firmware on probation until the pool is reached");
}

void fw_update_health(bool poolUp) {
    // Survived long enough: this boot doesn't count towards a crash loop
    if (s_bootCounted && millis() > SHORT_BOOT_MS) {
        s_bootCounted = false;
        Preferences p;
        if (p.begin("sparkupd", false)) { p.putUChar("short", 0); p.end(); }
    }
    if (!s_pendingVerify) return;
    if (poolUp && millis() > CONFIRM_AFTER_MS) {
        esp_ota_mark_app_valid_cancel_rollback();
        s_pendingVerify = false;
        Preferences p;
        if (p.begin("sparkupd", false)) { p.remove("pending"); p.end(); }
        setState("Update confirmed (%s)", AUTO_VERSION);
        Serial.println("[UPDATE] New firmware confirmed");
    } else if (millis() > HEALTHY_WITHIN_MS) {
        Serial.println("[UPDATE] New firmware never reached the pool - rolling back");
        delay(200);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void fw_update_poll(bool autoUpdate) {
    if (s_running) return;
    uint32_t now = millis();
    bool due = autoUpdate && (s_checkedOnce ? now - s_lastCheck >= CHECK_EVERY_MS : now >= FIRST_CHECK_MS);
    if (!s_requested && !due) return;
    s_requested = false;
    s_checkedOnce = true;
    s_lastCheck = now;
    setState("Checking...");
    s_running = true;
    if (xTaskCreatePinnedToCore(updateTask, "FwUpdate", 16384, NULL, 1, NULL, 0) != pdPASS) {
        s_running = false;
        setState("Not enough memory to check");
    }
}

#else  // No repo configured at build time

void fw_update_request() {}
void fw_update_poll(bool) {}
void fw_update_boot() {}
void fw_update_health(bool) {}
const char* fw_update_state() { return "Not configured"; }

#endif
