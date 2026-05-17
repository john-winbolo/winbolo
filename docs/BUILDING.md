# Building WinBolo

WinBolo uses CMake and fetches most dependencies automatically via `FetchContent`. A few platform-specific libraries must be installed beforehand.

## Dependencies fetched automatically

These are downloaded and built by CMake — no manual installation needed:

- SDL3
- SDL3_ttf (with vendored FreeType/HarfBuzz)
- Dear ImGui (docking branch)
- Lua 5.4
- zlib
- cJSON
- libmaxminddb (geo-IP lookups)
- libcurl (Windows only — built from source with Schannel SSL)
- ONNX Runtime (optional, for ML brain inference)

## Windows

### Requirements

- Visual Studio 2019 or later (with C/C++ workload), or MinGW
- CMake 3.15+
- Git

No additional libraries are needed. All dependencies including libcurl (using Windows Schannel for SSL) are built from source.

### Build

```bash
cmake -B build -S .
cmake --build build --config Release
```

Or open the project directly in CLion / Visual Studio with CMake support.

### Output

Executables and DLLs are placed in the `build/` directory:
- `WinBolo.exe` — game client
- `WinBoloDS.exe` — dedicated server
- `LogViewer.exe` — log file viewer

## Linux

### Requirements

- GCC or Clang with C99 and C++17 support
- CMake 3.15+
- Git
- System packages:

**Debian / Ubuntu:**
```bash
sudo apt install build-essential cmake git \
    libcurl4-openssl-dev libsdl2-dev pkg-config
```

**Fedora / RHEL:**
```bash
sudo dnf install gcc gcc-c++ cmake git \
    libcurl-devel pkgconfig
```

**Arch Linux:**
```bash
sudo pacman -S base-devel cmake git curl
```

Note: SDL3 is fetched and built by CMake. The `libsdl2-dev` package above is not used directly but ensures X11/Wayland headers and other low-level dependencies are available for SDL3 to build against. On a minimal system you may also need:

```bash
# Debian/Ubuntu - additional headers SDL3 may need
sudo apt install libx11-dev libxext-dev libxrandr-dev libxi-dev \
    libxcursor-dev libxss-dev libwayland-dev libxkbcommon-dev \
    libasound2-dev libpulse-dev libdbus-1-dev libudev-dev
```

### Build

```bash
cmake -B build -S .
cmake --build build -j$(nproc)
```

### Output

Executables are placed in the `build/` directory:
- `WinBolo` — game client
- `WinBoloDS` — dedicated server
- `LogViewer` — log file viewer
- `WinBoloHeadless` — headless server (no GUI)
- `libwinbolo_gym.so` — ML training gym library
- `BrainTest` — brain debug viewer

### Build and run baseline tests

To rebuild and run the `baseline` ctest suite in one command — and only run the tests if the build succeeds:

```bash
if cmake --build ~/linux-build -j$(nproc); then ctest --test-dir ~/linux-build -R baseline -j$(nproc); else echo "build failed"; fi
```

The `if` form gates on the build's exit status, so a build failure prints `build failed` and skips the tests rather than running ctest against stale binaries. A test failure reports through ctest as normal. `-j$(nproc)` runs scenarios in parallel — `--fast` scenarios share no state, and UDP scenarios are serialized per-port via CTest `RESOURCE_LOCK`s so different helpers still run concurrently.

#### Adding a new baseline scenario

Each scenario is registered in **two places**, which must stay in sync:

1. **`tests/baseline/run.sh`** — add a branch to the `dispatch_scenario` case statement mapping the scenario name to the helper invocation (`run`, `run_ds`, `run_events_fast`, `run_events_udp`, `run_events_cmd_fast`, `run_events_cmd_udp`, `run_events_cmd_udp_server_only`, or `run_events_cmd_udp_two_clients`) with its arguments (map, brain, command file, etc.). Also add the name to the appropriate `for` loop in the manual-mode block at the bottom so `run.sh` with no `--scenario` flag still exercises it.
2. **`CMakeLists.txt`** — add the scenario name to one of the lists near the `enable_testing()` block:
   - `_baseline_fast` for `--fast` scenarios (no UDP, parallelize freely).
   - `_baseline_udp_<port>` matching the hardcoded port in the helper the scenario uses (`run_ds` → 50001, `run_events_udp` → 50002, `run_events_cmd_udp` → 50003, `run_events_cmd_udp_server_only` → 50004, `run_events_cmd_udp_two_clients` → 50005). Same-port scenarios share a `RESOURCE_LOCK` and serialize; different-port scenarios stay concurrent.

Why two places: `run.sh` owns the per-scenario helper args (which CMake doesn't need to know), and `CMakeLists.txt` owns the CTest properties — timeout, resource lock — that `run.sh` can't express. A shared manifest would only deduplicate the name list, at the cost of a parser on both sides, so the names are kept as a small intentional duplication.

After adding a scenario, run it once standalone to capture the golden output under `tests/baseline/expected/`. The exact form depends on the helper:

```bash
# --fast scenarios capture to expected/<name>.json or expected/<name>.jsonl
~/linux-build/WinBoloHeadless --fast --map "tests/baseline/maps/<map>" \
    --brain tests/brains/<brain>.lua --ticks 500 --seed 42 \
    --log-state tests/baseline/expected/<name>.json --quiet
```

Re-run `cmake -B ~/linux-build` to pick up the new CTest entry, then `ctest -R baseline.<name>` to verify.

## macOS

### Requirements

- Xcode command line tools (or full Xcode)
- CMake 3.15+
- Git
- libcurl (included with macOS, but headers may need Xcode CLT)

```bash
xcode-select --install
brew install cmake
```

No other dependencies are needed — macOS ships with libcurl.

### Build

```bash
cmake -B build -S .
cmake --build build -j$(sysctl -n hw.ncpu)
```

By default, the macOS build is **universal2** (`arm64;x86_64`) so the resulting `.app` runs natively on both Apple Silicon and Intel Macs. Build time and binary size are roughly double a single-arch build. To produce a single-arch build (faster, smaller), override on the command line:

```bash
# Apple Silicon only
cmake -B build -S . -DCMAKE_OSX_ARCHITECTURES=arm64

# Intel only
cmake -B build -S . -DCMAKE_OSX_ARCHITECTURES=x86_64
```

`CMAKE_OSX_ARCHITECTURES` is sticky in the build directory's CMake cache once configured — re-running `cmake -B build -S .` without an override keeps whatever was set the first time. To switch a single-arch build dir back to universal2, either wipe it (`rm -rf build && cmake -B build -S .`) or pass `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` explicitly on the next configure. FetchContent dependencies built from source (SDL3, SDL3_ttf, crashpad) inherit whatever the cache holds.

Verify a finished build is universal with `lipo`:

```bash
lipo -info build/WinBolo.app/Contents/MacOS/WinBolo
# Architectures in the fat file: ... are: x86_64 arm64
```

The minimum macOS deployment target is 11.0 (Big Sur).

## iOS

### Requirements

- macOS with Xcode 14+
- CMake 3.15+

### Build

Generate an Xcode project and build from there:

```bash
cmake -B build-ios -S . -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
```

Then open `build-ios/winbolo.xcodeproj` in Xcode, select the `WinBoloIOS` target, and build.

Note: The iOS build does not use libcurl (network features use platform stubs).

## WebAssembly (WASM)

There are two WASM builds: the game client and the log viewer. Each has its own standalone CMakeLists.txt.

### Requirements

- Emscripten SDK

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### Build (game client)

```bash
emcmake cmake -B build-wasm-game -S src/wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm-game -j$(nproc)
```

Or use the convenience script:
```bash
./src/wasm/build.sh
```

### Build (log viewer)

```bash
emcmake cmake -B build-wasm-logviewer -S src/logviewer/wasm -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm-logviewer -j$(nproc)
```

### Testing locally

```bash
python3 -m http.server -d build-wasm-game 8080
# Open http://localhost:8080/winbolo.html
```

Note: The WASM builds do not use libcurl (network features use platform stubs).

## Build targets

| Target | Description | Platforms |
|--------|-------------|-----------|
| `WinBolo` | Game client with GUI | Windows, Linux, macOS |
| `WinBoloDS` | Dedicated server | Windows, Linux, macOS |
| `LogViewer` | Log file viewer | Windows, Linux, macOS |
| `WinBoloHeadless` | Headless server (no GUI) | Windows, Linux, macOS |
| `winbolo_gym` | ML training gym (shared lib) | Windows, Linux, macOS |
| `BrainTest` | Brain debug viewer | Windows, Linux, macOS |
| `WinBoloIOS` | iOS app bundle | iOS |
| `dist` | Distribution zip | All desktop |
| `sign_macos` | Sign + notarize + staple app bundles and the WinBoloDS binary | macOS |
| `package_macos` | Bundle signed apps + WinBoloDS into a notarized DMG | macOS |

## Optional features

### ONNX Runtime (ML brain inference)

Enabled by default. ONNX Runtime binaries are downloaded automatically. To disable:

```bash
cmake -B build -S . -DENABLE_ONNXRUNTIME=OFF
```

### OpenMP (parallel gym stepping)

The `winbolo_gym` library uses OpenMP to step multiple game instances in parallel. GCC includes OpenMP support (`libgomp`) by default — no extra packages needed. If using Clang, install the OpenMP runtime:

```bash
# Debian/Ubuntu (Clang only)
sudo apt install libomp-dev
```

OpenMP is detected automatically by CMake. If not found, the gym library still builds but `winbolo_step_batch()` will run sequentially.

### GeoIP lookups

If `data/dbip-country-lite.mmdb` is present, the server will use it for IP-to-country lookups via libmaxminddb (built automatically).

### Sentry crash reporting

Enabled by default. Pass a DSN to activate crash reporting:

```bash
cmake -B build -S . -DSENTRY_DSN="https://key@o0.ingest.sentry.io/0"
```

To disable entirely:

```bash
cmake -B build -S . -DENABLE_SENTRY=OFF
```

Users can also disable crash reporting at runtime with the `-nocrashreporting` flag.

## Release builds and debug symbols

Release builds should include debug information so that Sentry crash reports contain symbolicated stack traces. The key is to build with optimizations **and** debug symbols, then upload those symbols to Sentry.

### Windows (MSVC)

Use the `RelWithDebInfo` configuration to get optimized binaries with `.pdb` files:

```bash
cmake -B build -S . -DSENTRY_DSN="https://..."
cmake --build build --config RelWithDebInfo
```

This produces `.exe` files alongside `.pdb` debug symbol files in `build/RelWithDebInfo/`.

### Linux

Build with `RelWithDebInfo` to embed DWARF debug info in the ELF binaries:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSENTRY_DSN="https://..."
cmake --build build -j$(nproc)
```

The resulting binaries in `build/` contain embedded DWARF sections that `sentry-cli` can extract.

### macOS

Build with `RelWithDebInfo` to generate `.dSYM` bundles:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSENTRY_DSN="https://..."
cmake --build build -j$(sysctl -n hw.ncpu)
```

The `.dSYM` bundles are generated alongside the binaries in `build/`.

### iOS

Generate an Xcode project and build a Release or RelWithDebInfo archive:

```bash
cmake -B build-ios -S . -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
```

Build in Xcode with the Archive action or a Release configuration. The `.dSYM` files will be in the Xcode derived-data `Build/Products/` directory.

### Android

No special build type configuration is needed. The Sentry Gradle plugin (`io.sentry.android.gradle`) automatically uploads native debug symbols (`.so` files for each ABI) during `assembleRelease`:

```bash
cd android
./gradlew assembleRelease
```

Ensure `SENTRY_AUTH_TOKEN` is set in the environment or in `~/.sentryclirc` so the plugin can authenticate.

## Signing and notarization (macOS)

For a build that runs on machines other than the one that built it, the three macOS app bundles (`WinBolo.app`, `MapEditor.app`, `Log Viewer.app`) and the `WinBoloDS` dedicated-server CLI binary all need to be code-signed with a Developer ID Application certificate, notarized by Apple, and stapled so Gatekeeper accepts them offline. The `sign_macos` target handles all signing; the `package_macos` target builds a single notarized DMG containing everything.

### One-time setup

1. **Apple Developer Program membership** ($99/yr).

2. **Install a "Developer ID Application" certificate** in your login keychain (Apple Developer portal → Certificates → Developer ID Application). Verify with:

   ```bash
   security find-identity -p codesigning -v
   ```

   You should see at least one `Developer ID Application: <Your Name> (TEAMID)` entry.

3. **Generate an app-specific password** at [appleid.apple.com](https://appleid.apple.com/) → *Sign-In and Security* → *App-Specific Passwords*. Then stash it in the keychain as a `notarytool` profile:

   ```bash
   xcrun notarytool store-credentials winbolo-notary \
       --apple-id "your@apple.id" \
       --team-id  "YOURTEAMID" \
       --password "xxxx-xxxx-xxxx-xxxx"
   ```

   The profile name `winbolo-notary` matches the script's default; override with `APPLE_NOTARY_PROFILE` if you want a different name.

### Signing a build

After a normal build, run:

```bash
cmake --build build --target sign_macos
```

Or invoke the script directly with an alternate build directory:

```bash
scripts/sign_macos.sh build
```

For each `.app` in the build directory, the script:

1. Runs `codesign` with `--options runtime` (hardened runtime), `--timestamp`, `--deep`, and the entitlements at `src/gui/sdl3/platform/winbolo.entitlements`.
2. Verifies the signature with `codesign --verify --deep --strict`.
3. Zips the bundle and submits it to Apple via `xcrun notarytool submit --wait` (typically 1–5 minutes per bundle).
4. Staples the notarization ticket with `xcrun stapler staple` and runs `spctl --assess` to confirm Gatekeeper accepts it.

The `WinBoloDS` CLI binary is also signed (hardened runtime + entitlements + timestamp). A bare Mach-O cannot have a notarization ticket stapled to it, so its notarization is deferred to the DMG-level submission in `package_macos` — that single submission covers every signed binary inside the DMG.

The script auto-detects the first `Developer ID Application` identity from the keychain. To force a specific identity, set:

```bash
export APPLE_DEVELOPER_ID_APPLICATION="Developer ID Application: Your Name (TEAMID)"
```

### Entitlements

The bundles request only two entitlements (defined in `src/gui/sdl3/platform/winbolo.entitlements`):

- `com.apple.security.network.client` — outbound connections to trackers and peers.
- `com.apple.security.network.server` — listening UDP socket for inbound peer traffic.

No audio-input, file-access, or JIT entitlements are requested.

### Troubleshooting

- **"No Developer ID Application certificate found"** — the certificate isn't installed, or it's in a non-default keychain. Re-check `security find-identity -p codesigning -v`.
- **Notarization rejected** — fetch the detailed log from Apple's notary service:

  ```bash
  xcrun notarytool log <submission-id> --keychain-profile winbolo-notary
  ```

  The submission ID is printed at the top of the `notarytool submit` output. Common causes are unsigned dylibs inside the bundle (the `codesign --verify` step normally catches these first) or a missing hardened-runtime flag on a nested binary.
- **`spctl --assess` fails after stapling** — the staple succeeded but Gatekeeper still rejects. Almost always means the bundle was modified after stapling; rebuild and re-run `sign_macos`.

## Packaging a distribution DMG (macOS)

The `package_macos` target builds a single Gatekeeper-clean `WinBolo.dmg` installer containing all three apps, the `WinBoloDS` dedicated-server binary, and an `/Applications` drop-link. End users drag `WinBolo.app`, `MapEditor.app`, and `Log Viewer.app` into `Applications`; server operators copy `WinBoloDS` wherever they prefer (e.g. `/usr/local/bin`) and run it from a terminal or under launchd.

### Prerequisite

Install `create-dmg` once:

```bash
brew install create-dmg
```

Signing prerequisites are the same as `sign_macos` above — the same Developer ID certificate and `winbolo-notary` keychain profile are reused.

### Building the DMG

After a normal Release build:

```bash
cmake --build build --target package_macos
```

`package_macos` depends on `sign_macos`, so all four targets are signed first if they aren't already. The script then:

1. Stages the signed apps and `WinBoloDS` into a temporary directory.
2. Runs `create-dmg` to build `WinBolo.dmg` with an icon-arranged window and `/Applications` drop-link.
3. Signs the DMG with `codesign --timestamp`.
4. Submits the DMG to Apple via `xcrun notarytool submit --wait`. A single notarization covers every signed binary inside, including `WinBoloDS`.
5. Staples the ticket onto the DMG and runs `spctl --assess --type install` to confirm Gatekeeper accepts it.

The finished `WinBolo.dmg` is placed in the build directory.

### Shipping the dedicated server

`WinBoloDS` is shipped as a bare CLI binary inside the DMG rather than wrapped in a `.app`. Server operators normally:

```bash
hdiutil attach WinBolo.dmg
cp /Volumes/WinBolo/WinBoloDS /usr/local/bin/
hdiutil detach /Volumes/WinBolo
```

Because the binary was signed with the hardened runtime and the DMG was notarized by Apple, the copied binary launches without Gatekeeper prompts on first run — the quarantine attribute resolves against the stapled DMG ticket.

## Uploading debug symbols to Sentry

After building with debug info, upload symbols so Sentry can symbolicate crash reports.

### Prerequisites

1. Install `sentry-cli`: https://docs.sentry.io/cli/installation/
2. Set `SENTRY_AUTH_TOKEN` in your environment (create one at https://sentry.io/settings/auth-tokens/)

### Using the upload script

A convenience script is provided at `scripts/upload-sentry-symbols.sh`:

```bash
./scripts/upload-sentry-symbols.sh \
    --org your-org \
    --project winbolo \
    --path build/
```

This runs `sentry-cli upload-dif --include-sources` on the given path. It works for all desktop platforms — the CLI auto-detects the symbol format:

| Platform | What gets uploaded | Build path |
|----------|-------------------|------------|
| Windows (MSVC) | `.pdb` files | `build/RelWithDebInfo/` |
| macOS | `.dSYM` bundles | `build/` |
| Linux | ELF binaries with DWARF | `build/` |
| iOS | `.dSYM` bundles | Xcode derived-data `Build/Products/` path |

### Manual upload

You can also run `sentry-cli` directly:

```bash
sentry-cli upload-dif \
    --org your-org \
    --project winbolo \
    --include-sources \
    build/
```

### Android

Android symbol uploads are automatic. The Sentry Gradle plugin handles native symbol upload during `assembleRelease` — no manual `sentry-cli` invocation is needed.

### iOS

Upload the `.dSYM` files from your Xcode build:

```bash
./scripts/upload-sentry-symbols.sh \
    --org your-org \
    --project winbolo \
    --path ~/Library/Developer/Xcode/DerivedData/winbolo-*/Build/Products/Release-iphoneos/
```

Alternatively, add a Run Script build phase in Xcode to automate this on every archive build.
