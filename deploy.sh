#!/usr/bin/env bash
# Build SparkMiner for the CYD (2-USB / ST7789), publish a signed GitHub
# release (miners anywhere pick it up within 6 h, or via "Check for update"),
# and push it straight to the miner if it is on this network.
#
#   ./deploy.sh                 # build + release + LAN push
#   ./deploy.sh --lan-only      # build + LAN push, no release
#   ./deploy.sh --no-build      # LAN push of the last build only
#   MINER=192.168.1.27 ./deploy.sh
#
# Secrets (macOS Keychain):
#   sparkminer-pin      web UI admin PIN (or $SPARKMINER_PIN)
#   sparkminer-signing  release signing key, base64 DER (ECDSA P-256)
set -euo pipefail
cd "$(dirname "$0")"

ENV=esp32-2432s028-2usb
MINER="${MINER:-sparkminer-b820.local}"
PIO="${PIO:-$HOME/sparkminer-src/venv/bin/pio}"
BIN=".pio/build/$ENV/firmware.bin"
MODE="${1:-}"

REPO=$(sed -n 's/.*-D UPDATE_REPO=\\"\([^"\\]*\)\\".*/\1/p' platformio.ini | head -1)

# ---------- build ----------
if [[ "$MODE" != "--no-build" ]]; then
    BUILD=$(date +%s)
    export PLATFORMIO_BUILD_FLAGS="-DFW_BUILD=$BUILD ${EXTRA_BUILD_FLAGS:-}"
    "$PIO" run -e "$ENV"
    echo "$BUILD" > ".pio/build/$ENV/build_number"
fi
BUILD=$(cat ".pio/build/$ENV/build_number" 2>/dev/null || echo 0)

# ---------- signed GitHub release ----------
if [[ "$MODE" == "" && -n "$REPO" ]]; then
    TAG="build-$(date -r "$BUILD" +%Y%m%d-%H%M%S)"
    SHA=$(shasum -a 256 "$BIN" | cut -d' ' -f1)
    SIZE=$(stat -f%z "$BIN")
    KEY=$(mktemp)
    trap 'rm -f "$KEY"' EXIT
    security find-generic-password -s sparkminer-signing -w | base64 -d > "$KEY"
    SIG=$(printf '%s:%s' "$BUILD" "$SHA" | openssl dgst -sha256 -keyform DER -sign "$KEY" | base64)
    rm -f "$KEY"
    OUT=".pio/build/$ENV/release"
    mkdir -p "$OUT"
    cp "$BIN" "$OUT/firmware.bin"
    FILE="fw-$BUILD.bin"
    printf '{"build":%s,"tag":"%s","file":"%s","size":%s,"sha256":"%s","sig":"%s"}\n' \
        "$BUILD" "$TAG" "$FILE" "$SIZE" "$SHA" "$SIG" > "$OUT/manifest.json"
    git push -q origin HEAD:main 2>/dev/null || true
    gh release create "$TAG" "$OUT/firmware.bin" "$OUT/manifest.json" \
        --repo "$REPO" --title "$TAG" --notes "$(git log -1 --pretty=%s)" --latest >/dev/null
    # What the miners actually download: a one-commit "ota" branch on raw.githubusercontent.com
    OTA=$(mktemp -d)
    cp "$OUT/manifest.json" "$OTA/manifest.json"
    cp "$BIN" "$OTA/$FILE"
    (
        cd "$OTA"
        git init -q -b ota
        git add .
        git -c user.email=github@phumraphee.me -c user.name="Phumraphee Sae-tang" commit -q -m "OTA $TAG"
        git push -q -f "https://github.com/$REPO.git" ota:ota
    )
    rm -rf "$OTA"
    echo "Released $TAG on github.com/$REPO"
fi

# ---------- LAN push (skipped quietly if the miner is elsewhere) ----------
PIN="${SPARKMINER_PIN:-$(security find-generic-password -s sparkminer-pin -w 2>/dev/null || true)}"

HOST="$MINER"
if [[ "$MINER" == *.local ]]; then
    IP=$(dscacheutil -q host -a name "$MINER" 2>/dev/null | awk '/ip_address/ {print $2; exit}')
    [[ -n "$IP" ]] && HOST="$IP"
fi

if ! curl -fsS -m 5 "http://$HOST/api/status" >/dev/null 2>&1; then
    echo "Miner not on this network - it will install the release by itself."
    exit 0
fi
echo "Uploading $(du -h "$BIN" | cut -f1) to $MINER ($HOST)..."
curl -fsS -m 120 -H "X-PIN: $PIN" -F "firmware=@$BIN" "http://$HOST/api/update"; echo

# Wait for the reboot, then for it to come back
went_down=0
for i in $(seq 1 40); do
    sleep 2
    if s=$(curl -fsS -m 3 "http://$HOST/api/status" 2>/dev/null); then
        up=$(sed -n 's/.*"uptime":\([0-9]*\).*/\1/p' <<<"$s")
        b=$(sed -n 's/.*"build":\([0-9]*\).*/\1/p' <<<"$s")
        if [[ $went_down == 1 && -n "$up" ]]; then
            echo "Back online: build $b, uptime ${up}s"
            exit 0
        fi
    else
        went_down=1
    fi
done
echo "Miner did not come back within 80s - check the screen" >&2
exit 1
