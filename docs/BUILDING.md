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

### GeoIP lookups

If `data/dbip-country-lite.mmdb` is present, the server will use it for IP-to-country lookups via libmaxminddb (built automatically).
