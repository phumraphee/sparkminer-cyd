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
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <board_config.h>
#include "fw_update.h"
#include "extras.h"
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

// Returns true only if it rebooted into... (never returns on success)
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
        bool ok = httpsGet(http, client, "https://github.com/" UPDATE_REPO "/releases/latest/download/manifest.json");
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

    if (build <= FW_BUILD) {
        setState("Up to date (%s)", tag[0] ? tag : AUTO_VERSION);
        return;
    }
    if (!tag[0] || !size || strlen(shaHex) != 64 || !sig[0]) {
        setState("Manifest incomplete");
        return;
    }

    uint8_t expected[32];
    for (int i = 0; i < 32; i++) {
        char b[3] = {shaHex[2 * i], shaHex[2 * i + 1], 0};
        expected[i] = (uint8_t)strtoul(b, nullptr, 16);
    }
    // Check the signature before downloading anything
    if (!verifySignature(build, expected, sig)) {
        setState("Bad signature on %s - ignored", tag);
        return;
    }

    // --- Firmware download (miner parked: TLS + image hash use the SHA unit) ---
    setState("Downloading %s", tag);
    WiFiClientSecure client;
    HTTPClient http;
    miner_sha_hold();
    String url = String("https://github.com/" UPDATE_REPO "/releases/download/") + tag + "/firmware.bin";
    if (!httpsGet(http, client, url) || http.getSize() != (int)size) {
        http.end();
        miner_sha_release();
        setState("Download failed");
        return;
    }
    if (!Update.begin(size, U_FLASH)) {
        http.end();
        miner_sha_release();
        setState("Update begin failed");
        return;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts_ret(&ctx, 0);

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[1024];
    uint32_t got = 0, lastPct = 0, lastData = millis();
    while (got < size && millis() - lastData < 20000) {
        size_t avail = stream->available();
        if (!avail) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        int r = stream->readBytes(buf, min(avail, sizeof(buf)));
        if (r <= 0) continue;
        lastData = millis();
        mbedtls_sha256_update_ret(&ctx, buf, r);
        if (Update.write(buf, r) != (size_t)r) break;
        got += r;
        uint32_t pct = got * 100 / size;
        if (pct >= lastPct + 10) { lastPct = pct; setState("Downloading %s: %lu%%", tag, (unsigned long)pct); }
    }
    http.end();

    uint8_t actual[32];
    mbedtls_sha256_finish_ret(&ctx, actual);
    mbedtls_sha256_free(&ctx);

    if (got != size || memcmp(actual, expected, 32) != 0) {
        Update.abort();
        miner_sha_release();
        setState("Download corrupt - discarded");
        return;
    }
    bool ok = Update.end(true);
    miner_sha_release();
    if (!ok) {
        setState("Install failed: %s", Update.errorString());
        return;
    }

    setState("Installed %s - restarting", tag);
    char msg[96];
    snprintf(msg, sizeof(msg), "Installed %s (was %s). Restarting now.", tag, AUTO_VERSION);
    extras_notify("Miner updated", msg, "arrow_up", 2);
    delay(500);
    ESP.restart();
}

void fw_update_poll(bool autoUpdate) {
    uint32_t now = millis();
    bool due = autoUpdate && (s_checkedOnce ? now - s_lastCheck >= CHECK_EVERY_MS : now >= FIRST_CHECK_MS);
    if (!s_requested && !due) return;
    s_requested = false;
    s_checkedOnce = true;
    s_lastCheck = now;
    setState("Checking...");
    checkNow();
}

#else  // No repo configured at build time

void fw_update_request() {}
void fw_update_poll(bool) {}
const char* fw_update_state() { return "Not configured"; }

#endif
