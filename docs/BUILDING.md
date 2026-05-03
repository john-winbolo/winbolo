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
