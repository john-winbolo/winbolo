#!/usr/bin/env bash
#
# sign_macos.sh — Sign, notarize, and staple WinBolo's macOS app bundles.
#
# Prerequisites (one-time setup):
#
#   1. Apple Developer Program membership ($99/yr).
#
#   2. "Developer ID Application: <Your Name> (TEAMID)" certificate in
#      your login keychain. Verify with:
#          security find-identity -p codesigning -v
#      You should see at least one "Developer ID Application:" line.
#
#   3. App-specific password for notarytool stored as a keychain profile.
#      Generate the password at appleid.apple.com → Sign-In and Security
#      → App-Specific Passwords, then run:
#          xcrun notarytool store-credentials winbolo-notary \
#              --apple-id   "your@apple.id" \
#              --team-id    "YOURTEAMID" \
#              --password   "xxxx-xxxx-xxxx-xxxx"
#      The profile name "winbolo-notary" matches APPLE_NOTARY_PROFILE
#      below.
#
# Usage:
#   scripts/sign_macos.sh [BUILD_DIR]
#
# BUILD_DIR defaults to cmake-build-release. Override via the positional
# argument or the BUILD_DIR env var.
#
# Environment overrides:
#   APPLE_DEVELOPER_ID_APPLICATION  Force a specific signing identity.
#                                   Default: auto-detect from keychain.
#   APPLE_NOTARY_PROFILE            Keychain profile name to use for
#                                   notarytool. Default: winbolo-notary.

set -euo pipefail

BUILD_DIR="${1:-${BUILD_DIR:-cmake-build-release}}"
NOTARY_PROFILE="${APPLE_NOTARY_PROFILE:-winbolo-notary}"
ENTITLEMENTS="src/gui/sdl3/platform/winbolo.entitlements"

# Resolve the signing identity. Allow override; otherwise pick the first
# "Developer ID Application" identity from the codesigning keychain view.
CERT_NAME="${APPLE_DEVELOPER_ID_APPLICATION:-}"
if [[ -z "$CERT_NAME" ]]; then
    CERT_NAME=$(security find-identity -p codesigning -v 2>/dev/null \
                | grep "Developer ID Application" \
                | head -n1 \
                | sed -E 's/.*"([^"]+)".*/\1/')
fi

if [[ -z "$CERT_NAME" ]]; then
    cat >&2 <<'EOF'
ERROR: No "Developer ID Application" certificate found.

Install one from developer.apple.com (Certificates → Developer ID
Application), then re-run. Verify the install with:
    security find-identity -p codesigning -v
EOF
    exit 1
fi
echo "Signing identity: $CERT_NAME"

if [[ ! -f "$ENTITLEMENTS" ]]; then
    echo "ERROR: Entitlements file not found at $ENTITLEMENTS" >&2
    exit 1
fi
echo "Entitlements:     $ENTITLEMENTS"
echo "Notary profile:   $NOTARY_PROFILE"
echo "Build directory:  $BUILD_DIR"
echo ""

# All three apps the build produces.
APPS=(
    "$BUILD_DIR/WinBolo.app"
    "$BUILD_DIR/MapEditor.app"
    "$BUILD_DIR/Log Viewer.app"
)

ANY_PROCESSED=0
for APP in "${APPS[@]}"; do
    if [[ ! -d "$APP" ]]; then
        echo "Skipping $APP (not built)"
        continue
    fi
    ANY_PROCESSED=1

    echo ""
    echo "=== $APP ==="

    echo "  [1/4] codesign (hardened runtime + entitlements + deep)"
    codesign --force --deep --options runtime --timestamp \
             --entitlements "$ENTITLEMENTS" \
             --sign "$CERT_NAME" \
             "$APP"

    echo "  [2/4] codesign --verify"
    codesign --verify --deep --strict --verbose=2 "$APP" 2>&1 | sed 's/^/        /'

    echo "  [3/4] notarytool submit --wait (this can take several minutes)"
    ZIP="${APP%.app}.zip"
    ditto -c -k --keepParent "$APP" "$ZIP"
    if ! xcrun notarytool submit "$ZIP" \
              --keychain-profile "$NOTARY_PROFILE" \
              --wait; then
        rm -f "$ZIP"
        cat >&2 <<EOF

ERROR: Notarization failed for $APP.

Common causes:
  - Profile "$NOTARY_PROFILE" not set up. Run:
      xcrun notarytool store-credentials $NOTARY_PROFILE \\
          --apple-id "your@apple.id" --team-id "TEAMID" \\
          --password "app-specific-password"
  - Bundle has unsigned components. Re-run; the verify step above
    would have caught this already.
  - Network / Apple notary service issue. Retry in a few minutes.

Inspect the latest submission log with:
    xcrun notarytool log <submission-id> --keychain-profile $NOTARY_PROFILE
EOF
        exit 1
    fi
    rm -f "$ZIP"

    echo "  [4/4] stapler staple + spctl assess"
    xcrun stapler staple "$APP"
    xcrun stapler validate "$APP" | sed 's/^/        /'
    spctl --assess --type execute --verbose "$APP" 2>&1 | sed 's/^/        /' || true

    echo "  ✓ $APP signed, notarized, and stapled"
done

# Dedicated-server CLI binary. It's a bare Mach-O, not a .app, so we sign
# it with hardened runtime + entitlements but skip individual notarization
# (you can't staple a tickets onto a bare binary). Notarization happens at
# the DMG level in package_macos.sh — that single notarytool submission
# covers every signed binary inside the DMG.
SERVER_BIN="$BUILD_DIR/WinBoloDS"
if [[ -f "$SERVER_BIN" ]]; then
    ANY_PROCESSED=1
    echo ""
    echo "=== $SERVER_BIN ==="

    echo "  [1/2] codesign (hardened runtime + entitlements)"
    codesign --force --options runtime --timestamp \
             --entitlements "$ENTITLEMENTS" \
             --sign "$CERT_NAME" \
             "$SERVER_BIN"

    echo "  [2/2] codesign --verify"
    codesign --verify --strict --verbose=2 "$SERVER_BIN" 2>&1 | sed 's/^/        /'

    echo "  ✓ $SERVER_BIN signed (notarized via DMG)"
else
    echo "Skipping $SERVER_BIN (not built)"
fi

if [[ "$ANY_PROCESSED" -eq 0 ]]; then
    echo ""
    echo "WARNING: No bundles or WinBoloDS found in $BUILD_DIR — nothing was signed." >&2
    echo "Build first (e.g. 'cmake --build $BUILD_DIR'), then re-run." >&2
    exit 1
fi

echo ""
echo "All processed targets are signed. Run package_macos for a notarized DMG."
