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
#   scripts/sign_macos.sh [--app NAME]... [BUILD_DIR]
#
# BUILD_DIR defaults to cmake-build-release. Override via the positional
# argument or the BUILD_DIR env var.
#
# --app NAME   Sign only the named target instead of every bundle in
#              BUILD_DIR. Repeat the flag to select several. NAME is a
#              bundle/binary name, e.g. "WinBolo.app", "MapEditor.app",
#              "Log Viewer.app", or "WinBoloDS". Without --app, all
#              targets found in BUILD_DIR are processed.
#
# Environment overrides:
#   APPLE_DEVELOPER_ID_APPLICATION  Force a specific signing identity.
#                                   Default: auto-detect from keychain.
#   APPLE_NOTARY_PROFILE            Keychain profile name to use for
#                                   notarytool. Default: winbolo-notary.

set -euo pipefail

# All targets the build produces, in processing order. The leading entries
# are .app bundles (individually notarized + stapled); WinBoloDS is a bare
# Mach-O signed but not individually notarized. KNOWN_TARGETS drives both
# --app validation and the default "process everything" behavior.
KNOWN_TARGETS=(
    "WinBolo.app"
    "MapEditor.app"
    "Log Viewer.app"
    "WinBoloDS"
)

# Parse optional --app filters and the positional BUILD_DIR.
REQUESTED_APPS=()
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --app|-app)
            [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 1; }
            REQUESTED_APPS+=("$2")
            shift 2
            ;;
        --app=*|-app=*)
            REQUESTED_APPS+=("${1#*app=}")
            shift
            ;;
        -h|--help)
            sed -n '24,38p' "$0"
            exit 0
            ;;
        -*)
            echo "ERROR: unknown option '$1'." >&2
            echo "Run 'scripts/sign_macos.sh --help' for usage." >&2
            exit 1
            ;;
        *)
            POSITIONAL+=("$1")
            shift
            ;;
    esac
done

# Validate any requested names against the known target list so a typo
# fails loudly instead of silently signing nothing.
if [[ ${#REQUESTED_APPS[@]} -gt 0 ]]; then
    for want in "${REQUESTED_APPS[@]}"; do
        ok=0
        for known in "${KNOWN_TARGETS[@]}"; do
            [[ "$want" == "$known" ]] && { ok=1; break; }
        done
        if [[ "$ok" -eq 0 ]]; then
            echo "ERROR: unknown --app target '$want'." >&2
            printf 'Valid targets: %s\n' "${KNOWN_TARGETS[*]}" >&2
            exit 1
        fi
    done
fi

# Returns 0 if the given target basename should be processed: true when no
# --app filter was given, or when the basename matches a requested name.
want_target() {
    [[ ${#REQUESTED_APPS[@]} -eq 0 ]] && return 0
    local base; base=$(basename "$1")
    local want
    for want in "${REQUESTED_APPS[@]}"; do
        [[ "$base" == "$want" ]] && return 0
    done
    return 1
}

BUILD_DIR="${POSITIONAL[0]:-${BUILD_DIR:-cmake-build-release}}"
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
    want_target "$APP" || continue
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
if want_target "$SERVER_BIN" && [[ -f "$SERVER_BIN" ]]; then
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
elif want_target "$SERVER_BIN"; then
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
