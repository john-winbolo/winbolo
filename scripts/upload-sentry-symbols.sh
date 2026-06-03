#!/usr/bin/env bash
#
# Upload debug symbols to Sentry for symbolicated crash reports.
#
# Usage:
#   ./scripts/upload-sentry-symbols.sh --org ORG --project PROJECT --path BUILD_DIR
#
# Requirements:
#   - sentry-cli installed (https://docs.sentry.io/cli/installation/)
#   - SENTRY_AUTH_TOKEN environment variable set
#
# Platform notes:
#   Windows (MSVC) : uploads .pdb files from the build directory
#   macOS          : uploads .dSYM bundles from the build directory
#   Linux          : uploads ELF binaries containing DWARF debug info
#   iOS            : uploads .dSYM bundles from the Xcode build (pass the
#                    derived-data Build/Products path)
#
# Android is handled automatically by the Sentry Gradle plugin during
# assembleRelease — this script is not needed for Android builds.

set -euo pipefail

usage() {
    echo "Usage: $0 --org ORG --project PROJECT --path BUILD_DIR"
    echo ""
    echo "Options:"
    echo "  --org      Sentry organization slug"
    echo "  --project  Sentry project slug"
    echo "  --path     Path to build output (e.g. build/, build-ios/)"
    echo ""
    echo "Environment:"
    echo "  SENTRY_AUTH_TOKEN  Required. Sentry API auth token."
    exit 1
}

ORG=""
PROJECT=""
BUILD_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --org)     ORG="$2";        shift 2 ;;
        --project) PROJECT="$2";    shift 2 ;;
        --path)    BUILD_PATH="$2"; shift 2 ;;
        -h|--help) usage ;;
        *)         echo "Unknown option: $1"; usage ;;
    esac
done

if [[ -z "$ORG" || -z "$PROJECT" || -z "$BUILD_PATH" ]]; then
    echo "Error: --org, --project, and --path are all required."
    usage
fi

if [[ -z "${SENTRY_AUTH_TOKEN:-}" ]]; then
    echo "Error: SENTRY_AUTH_TOKEN environment variable is not set."
    echo "Create one at https://sentry.io/settings/auth-tokens/"
    exit 1
fi

if ! command -v sentry-cli &>/dev/null; then
    echo "Error: sentry-cli not found. Install it from:"
    echo "  https://docs.sentry.io/cli/installation/"
    exit 1
fi

echo "Uploading debug symbols to Sentry..."
echo "  Org:     $ORG"
echo "  Project: $PROJECT"
echo "  Path:    $BUILD_PATH"
echo ""

# Find our own binaries plus third-party PDBs from _deps (SDL3, libcurl,
# onnxruntime, sentry/crashpad, etc.) so vendor frames symbolicate too.
# Excludes specific subtrees that ship test fixtures sentry-cli chokes on:
#   - crashpad's bundled zlib test data (malformed .zip files)
#   - breakpad's testdata PDBs (e.g. kernel32.pdb) which would upload as noise
find "$BUILD_PATH" \
    \( -path "*/_deps/sentry-src/external/crashpad/third_party/zlib*" \
    -o -path "*/_deps/sentry-src/external/breakpad/src/processor/testdata*" \
    -o -path "*/_deps/sentry-src/external/breakpad/src/tools/windows/dump_syms/testdata*" \
    \) -prune -o \( \
    -name "*.pdb" -o \
    -name "*.dSYM" -o \
    -name "WinBolo" -o \
    -name "WinBolo.exe" -o \
    -name "WinBoloDS" -o \
    -name "WinBoloDS.exe" -o \
    -name "WinBoloHeadless" -o \
    -name "WinBoloHeadless.exe" -o \
    -name "LogViewer" -o \
    -name "LogViewer.exe" \
    \) -print0 | xargs -0 sentry-cli upload-dif \
    --org "$ORG" \
    --project "$PROJECT" \
    --include-sources

echo ""
echo "Done. Symbols uploaded successfully."
