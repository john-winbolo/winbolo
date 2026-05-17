#!/usr/bin/env bash
#
# package_macos.sh — Bundle the signed apps + dedicated server into a
# single notarized, stapled DMG installer.
#
# Prerequisites:
#   1. All four targets already signed by sign_macos.sh:
#        WinBolo.app, MapEditor.app, Log Viewer.app, WinBoloDS
#   2. create-dmg installed:
#        brew install create-dmg
#   3. Same Developer ID Application certificate + notarytool keychain
#      profile that sign_macos.sh uses (see scripts/sign_macos.sh header).
#
# Usage:
#   scripts/package_macos.sh [BUILD_DIR]
#
# BUILD_DIR defaults to cmake-build-release. Override via the positional
# argument or the BUILD_DIR env var.
#
# Environment overrides (same names as sign_macos.sh):
#   APPLE_DEVELOPER_ID_APPLICATION  Force a specific signing identity.
#   APPLE_NOTARY_PROFILE            Keychain profile name (default:
#                                   winbolo-notary).

set -euo pipefail

BUILD_DIR="${1:-${BUILD_DIR:-cmake-build-release}}"
NOTARY_PROFILE="${APPLE_NOTARY_PROFILE:-winbolo-notary}"

if ! command -v create-dmg >/dev/null 2>&1; then
    cat >&2 <<'EOF'
ERROR: create-dmg not found.

Install with:
    brew install create-dmg
EOF
    exit 1
fi

CERT_NAME="${APPLE_DEVELOPER_ID_APPLICATION:-}"
if [[ -z "$CERT_NAME" ]]; then
    CERT_NAME=$(security find-identity -p codesigning -v 2>/dev/null \
                | grep "Developer ID Application" \
                | head -n1 \
                | sed -E 's/.*"([^"]+)".*/\1/')
fi
if [[ -z "$CERT_NAME" ]]; then
    echo "ERROR: No \"Developer ID Application\" certificate found." >&2
    echo "Install one and re-run scripts/sign_macos.sh first." >&2
    exit 1
fi

echo "Signing identity: $CERT_NAME"
echo "Notary profile:   $NOTARY_PROFILE"
echo "Build directory:  $BUILD_DIR"
echo ""

# Stage the contents — create-dmg's source directory must contain exactly
# what we want to ship and nothing else.
STAGE="$BUILD_DIR/dmg_staging"
DMG="$BUILD_DIR/WinBolo.dmg"

rm -rf "$STAGE"
mkdir -p "$STAGE"

APPS=(
    "$BUILD_DIR/WinBolo.app"
    "$BUILD_DIR/MapEditor.app"
    "$BUILD_DIR/Log Viewer.app"
)
STAGED=0
for APP in "${APPS[@]}"; do
    if [[ -d "$APP" ]]; then
        # cp -R on macOS preserves code-signature xattrs.
        cp -R "$APP" "$STAGE/"
        STAGED=1
    fi
done
if [[ -f "$BUILD_DIR/WinBoloDS" ]]; then
    cp "$BUILD_DIR/WinBoloDS" "$STAGE/"
    STAGED=1
fi

if [[ "$STAGED" -eq 0 ]]; then
    echo "ERROR: nothing to package — neither apps nor WinBoloDS found in $BUILD_DIR." >&2
    rm -rf "$STAGE"
    exit 1
fi

echo "Staged for DMG:"
ls -1 "$STAGE" | sed 's/^/  /'
echo ""

echo "[1/4] Building DMG with create-dmg..."
rm -f "$DMG"
create-dmg \
    --volname "WinBolo" \
    --window-size 640 420 \
    --icon-size 96 \
    --icon "WinBolo.app"     130 180 \
    --icon "MapEditor.app"   290 180 \
    --icon "Log Viewer.app"  450 180 \
    --icon "WinBoloDS"       130 320 \
    --app-drop-link          450 320 \
    "$DMG" \
    "$STAGE"

echo ""
echo "[2/4] codesign DMG (hardened runtime + timestamp)"
codesign --force --timestamp --sign "$CERT_NAME" "$DMG"

echo "[3/4] notarytool submit --wait (this can take several minutes)"
if ! xcrun notarytool submit "$DMG" \
          --keychain-profile "$NOTARY_PROFILE" \
          --wait; then
    cat >&2 <<EOF

ERROR: DMG notarization failed.

Inspect the latest submission log with:
    xcrun notarytool log <submission-id> --keychain-profile $NOTARY_PROFILE
EOF
    rm -rf "$STAGE"
    exit 1
fi

echo "[4/4] stapler staple + spctl assess"
xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG" | sed 's/^/        /'
spctl --assess --type install --verbose "$DMG" 2>&1 | sed 's/^/        /' || true

rm -rf "$STAGE"

echo ""
echo "✓ $DMG is signed, notarized, and stapled."
