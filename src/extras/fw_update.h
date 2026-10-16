/*
 * SparkMiner - Self-update from GitHub Releases
 *
 * The miner polls manifest.json on the "ota" branch of UPDATE_REPO
 * (raw.githubusercontent.com; deploy.sh force-pushes it with every release).
 * A newer build is downloaded, its SHA-256 checked, and the ECDSA P-256
 * signature over "<build>:<sha256hex>" verified against the public key
 * compiled in below before it is activated. The private key never leaves
 * the developer's Keychain, so a compromised GitHub account cannot push firmware.
 */

#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include <Arduino.h>

#ifndef FW_BUILD
#define FW_BUILD 0
#endif

/**
 * Ask for an update check on the next poll (e.g. from the web UI)
 */
void fw_update_request();

/**
 * Called periodically from the extras task: checks on schedule or on request.
 * Reboots into the new firmware on success.
 */
void fw_update_poll(bool autoUpdate);

/**
 * Human-readable state for the web UI
 */
const char* fw_update_state();

#endif // FW_UPDATE_H
