cmake_minimum_required(VERSION 3.28)

# Allow vendored dependencies with older cmake_minimum_required calls
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "")

# CMP0168 (CMake 3.30+): Use direct population for FetchContent instead of a
# sub-build.  The sub-build approach fails with the Xcode generator when
# cross-compiling for iOS because the populate target inherits iOS toolchain
# settings and cannot run the download scripts.
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

# Use vcpkg if installed at C:/vcpkg and no toolchain file was specified
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND EXISTS "C:/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(winbolo C)

include(${CMAKE_SOURCE_DIR}/cmake/bolo_lib.cmake)

# Enable Objective-C for macOS pinch-to-zoom support (not iOS)
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJC)
endif()

# On macOS, ensure CMAKE_OSX_SYSROOT is set so that dependencies (e.g.
# crashpad) can find system .defs files under the SDK path instead of the
# non-existent /usr/include.
if(APPLE AND NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

# Build universal2 on macOS so the same .app runs natively on Apple Silicon
# and Intel Macs. Override on the command line with -DCMAKE_OSX_ARCHITECTURES=arm64
# for a single-arch build. iOS sets its own architectures via the toolchain.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_ARCHITECTURES)
    # FORCE because CLion / some IDE configures write an empty cache entry
    # for CMAKE_OSX_ARCHITECTURES, which would otherwise block this default.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS build architectures" FORCE)
endif()

include(cmake/bolo_version.cmake)

# Global macOS bundle metadata. Set once here so every MACOSX_BUNDLE target
# (WinBolo, Log Viewer, Map Editor) picks up matching version/copyright keys
# in its Info.plist, and so the deployment target applies to all of them.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # FORCE for the same reason as CMAKE_OSX_ARCHITECTURES above: CLion / some
    # IDE configures write an EMPTY cache entry for CMAKE_OSX_DEPLOYMENT_TARGET,
    # which without FORCE would win and leave the target unset. An unset target
    # makes clang default to the host SDK version (e.g. 26.x), baking minos 26.0
    # into the Mach-O load command so dyld refuses to launch on older macOS.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
    string(TIMESTAMP _WB_CURRENT_YEAR "%Y")
    # Marketing/human-facing strings carry the display suffix (e.g. "2.02a")
    # so Finder and the About panel match the rest of the UI. CFBundleVersion
    # is the build number macOS treats as monotonic, so keep it numeric.
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${WINBOLO_DISPLAY_VERSION}")
    set(MACOSX_BUNDLE_BUNDLE_VERSION       "${WINBOLO_VERSION}")
    set(MACOSX_BUNDLE_LONG_VERSION_STRING  "${WINBOLO_DISPLAY_VERSION}")
    set(MACOSX_BUNDLE_COPYRIGHT            "Copyright 1998-${_WB_CURRENT_YEAR} John Morrison")
endif()

set(CMAKE_C_STANDARD 99)

# On Linux, embed $ORIGIN in RPATH so installed/dist binaries find .so files in
# the same directory, while keeping the default build-tree RPATH for development.
if(NOT WIN32 AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()
# On macOS (not iOS), use @executable_path so binaries find .dylib files next to them.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CMAKE_INSTALL_RPATH "@executable_path")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
endif()

# On Linux, libm is a separate library that must be linked explicitly.
# On macOS/iOS it is part of libSystem (always linked), so adding -lm is
# redundant and causes duplicate-library linker warnings.
if(UNIX AND NOT APPLE)
    set(MATH_LIB m)
else()
    set(MATH_LIB)
endif()

# ---------------------------------------------------------------------------
# Dependencies (FetchContent)
# ---------------------------------------------------------------------------

include(FetchContent)
FetchContent_Declare(
    sdl3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    # Must stay >= 3.4.12, which fixes a use-after-free in the audio-device
    # disconnect path (libsdl-org/SDL#14856, commits 39aff4c9 + 6a854e03): a
    # queued main-thread callback held a raw SDL_AudioDevice pointer, and a
    # stale run could free the device while its audio thread was still
    # converting stream data -> EXCEPTION_ACCESS_VIOLATION_READ in
    # SDL_Convert_S16_to_F32_SSE2 (Sentry crash on Windows, WinBolo 2.02).
    GIT_TAG        release-3.4.14
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    sdl3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        release-3.2.0
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
# The docking branch (not master) because the log viewer sets
# ImGuiConfigFlags_DockingEnable, and sdl3imgui.cpp / imgui_lobby.cpp pass
# ImGuiWindowFlags_NoDocking — none of which exist on master. Pin to a release
# tag: tracking the branch head silently pulled a backend change that reset the
# SDL renderer's default sampler to LINEAR and put a dark grid through the map
# editor canvas.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b-docking
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/walterschell/Lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    libmaxminddb
    GIT_REPOSITORY https://github.com/maxmind/libmaxminddb.git
    GIT_TAG        1.12.2
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_Declare(
    speexdsp
    GIT_REPOSITORY https://github.com/xiph/speexdsp.git
    # Pinned to a commit rather than a release: the newest tag (SpeexDSP-1.2.1)
    # ships autotools only, and the CMake build lives on main.  A commit hash
    # rules out GIT_SHALLOW, which needs a branch or tag name.
    GIT_TAG        ba75b509fb1c5940ea07fa2ba5552e44f3a3576b
    SYSTEM
    EXCLUDE_FROM_ALL
)
# curl is only built from source on Windows (uses Schannel for SSL).
# On Linux/macOS, the system libcurl is used via find_package().
if(WIN32)
    FetchContent_Declare(
        curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG        curl-8_12_1
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL
    )
endif()

# Skip SDL test/example targets — we only need the library
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)

# iOS: only build static SDL libraries (shared libs require code signing
# and are not useful in an app bundle)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_LIBC ON CACHE BOOL "" FORCE)

    # CMake's try_compile/check_symbol_exists probes each launch a full
    # xcodebuild invocation on the Xcode generator, making configure
    # extremely slow.  Temporarily switch probes to STATIC_LIBRARY mode
    # which uses a simple compile check instead, then restore after all
    # dependency fetches are done.
    set(_saved_try_compile_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

# Fetch SDL first (needed by everything)
FetchContent_MakeAvailable(sdl3)

# Lua backend for the bot brains.
#   Default: LuaJIT (Lua 5.1) on every platform that can build it — LuaJIT source
#   is fetched (pinned by commit below) and built with its own native build
#   system (make / msvcbuild.bat) via ExternalProject at build time. The brains
#   are written in the Lua 5.1 subset (bit ops via bitcompat), so they run
#   unchanged on LuaJIT and PUC-Lua alike — no transpile step.
#   Fallback: PUC-Lua 5.4 (static lib from FetchContent) where LuaJIT cannot run
#   — Emscripten/WASM (no JIT backend) and iOS (OS forbids JIT) — and whenever
#   -DWINBOLO_LUAJIT=OFF is passed.
# CAVEAT: LuaJIT's number model and table-iteration order differ from PUC-Lua
# 5.4, so bot behaviour is NOT guaranteed bit-identical across the two VMs, and
# cross-VM replay / winbolo.net determinism is not yet validated. Pass
# -DWINBOLO_LUAJIT=OFF for a PUC-Lua 5.4 build.
# lua_static / lua_internal are the stable target names the rest of the tree
# links against; under LuaJIT they wrap the built LuaJIT static lib and force-
# include the C-API compat shim (src/gui/sdl3/luajit_compat.h) into every
# Lua-touching TU.
set(WINBOLO_LUAJIT_LUAJIT_TAG   "faaf663340347a78b22ed94c63c24fe090bd9784"
    CACHE STRING "Pinned LuaJIT (v2.1 branch) commit to build")

# Default ON everywhere LuaJIT can build; OFF where it cannot.
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_wbn_luajit_default OFF)
else()
    set(_wbn_luajit_default ON)
endif()
option(WINBOLO_LUAJIT "Run bot brains on LuaJIT (Lua 5.1); OFF uses PUC-Lua 5.4" ${_wbn_luajit_default})

# Hard guard: LuaJIT is unusable on WASM (no backend) and iOS (JIT banned), so
# fall back to PUC-Lua there even if the option was explicitly forced on.
if(WINBOLO_LUAJIT AND (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    message(WARNING "WINBOLO_LUAJIT is not supported on this platform (WASM/iOS); using PUC-Lua 5.4 instead.")
    set(WINBOLO_LUAJIT OFF)
endif()

# Measurement builds only: LUAJIT_SECURITY_PRNG=0 fixes LuaJIT's string-hash
# seed instead of drawing it from OS entropy at VM creation. With stock
# seeding, pairs() order over hash tables differs every run, which reorders
# the brains' /info broadcasts and eval scans and makes identical -seed games
# diverge (found 20260831: two same-seed runs split within 6 game-seconds,
# first observable diff being /info key order). Turn this ON for an A/B run
# and leave it off otherwise.
#
# OFF by default, and it should stay that way, for two reasons.
#
# It is not a free knob. The fixed seed is what a hash-flooding attack needs
# to be reproducible, and brains do hash attacker-controlled strings: human
# chat reaches a bot's inbox and GoalHunter parses it into table keys
# (ally_state.lua's slot.info[key] = value). The per-tick budget hook caps
# the damage at a killed think rather than a stalled server, so the exposure
# is small -- but it is not zero, and it is on a shipped client and a public
# dedicated server. LuaJIT's own lj_arch.h is explicit: "Important note to
# distro maintainers: DO NOT change the defaults for a regular distro build
# -- neither upwards, nor downwards!"
#
# And it is the wrong tool for the job. lj_prng.c says so at the branch this
# define selects: "BTW: This is NOT the way to get predictable table
# iteration, predictable trace generation, predictable bytecode generation,
# etc." A fixed seed makes pairs() order repeatable BETWEEN RUNS OF THE SAME
# BINARY and nothing more -- it is not stable across a LuaJIT upgrade, a
# rebuild with different flags, or a 32/64-bit change, so a reference
# baseline recorded under it is only comparable to runs from that same
# build. The durable fix is for the brain not to depend on pairs() order
# where that order is OBSERVABLE: iterate an array, or sort the keys, in
# those paths. Doing that would let this option be deleted.
#
# Costs nothing at runtime either way: lj_prng_seed_secure runs once per
# lua_newstate (lj_state.c) -- one entropy read per brain, not per think --
# and the hashing on the hot path is identical under both settings.
option(WINBOLO_LUAJIT_DETERMINISTIC
       "Fixed LuaJIT string-hash/PRNG seed for A/B measurement runs (insecure; see comment)" OFF)

if(WINBOLO_LUAJIT)
    include(ExternalProject)
    message(STATUS "WINBOLO_LUAJIT: building LuaJIT ${WINBOLO_LUAJIT_LUAJIT_TAG} from source (native build system)")
    if(WINBOLO_LUAJIT_DETERMINISTIC)
        # A warning, not a status line: this is now opt-in and it weakens the
        # VM. A release build should never print it.
        message(WARNING "WINBOLO_LUAJIT_DETERMINISTIC=ON: LUAJIT_SECURITY_PRNG=0 "
                        "(fixed string-hash seed). For A/B measurement runs only "
                        "-- do not ship a build with this set.")
    endif()

    # 1. LuaJIT source (no CMakeLists of its own — MakeAvailable only populates).
    FetchContent_Declare(luajit_src
        GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git
        GIT_TAG        ${WINBOLO_LUAJIT_LUAJIT_TAG}
        # Deliberately NOT GIT_SHALLOW. Unlike every other dependency here,
        # LuaJIT is pinned to a raw commit SHA rather than a release tag.
        # GIT_SHALLOW makes FetchContent run `git clone --depth 1
        # --no-single-branch`, which fetches only each branch's TIP — so
        # checking out a pinned SHA works only while that SHA happens to be
        # the tip of v2.1. Upstream moving v2.1 forward then breaks every
        # clean build with "unable to read tree", while incremental builds
        # keep working off the cached _deps checkout and hide it. A full
        # clone is ~11MB and always contains the pin.
        # Fixes a void-function `return 0;` that breaks macOS hardened-runtime
        # (LUAJIT_ENABLE_OSX_HRT) builds — see the patch header for details.
        PATCH_COMMAND  ${CMAKE_COMMAND} -P
                       "${CMAKE_SOURCE_DIR}/cmake/patches/luajit_fix_osx_hrt_return.cmake"
        # Fixes a per-thread W^X leak in the same HRT path that SIGBUSes
        # bot worker threads (2.02 Sentry 24dae975) — see the patch header.
        COMMAND        ${CMAKE_COMMAND} -P
                       "${CMAKE_SOURCE_DIR}/cmake/patches/luajit_fix_osx_hrt_thread_leak.cmake")
    FetchContent_MakeAvailable(luajit_src)
    set(_lj_src "${luajit_src_SOURCE_DIR}")

    # 2. Build LuaJIT with its OWN build system (make / msvcbuild.bat). The CMake
    #    wrappers reimplement the DynASM buildvm host-tool bootstrap and mangle
    #    the arch-detection flags on some toolchains; LuaJIT's native build is the
    #    supported path. XCFLAGS=-fPIC so the static lib links into winbolo_gym.
    if(MSVC)
        set(_lj_lib "${_lj_src}/src/lua51.lib")
        set(_lj_msvc_env "")
        if(WINBOLO_LUAJIT_DETERMINISTIC)
            set(_lj_msvc_env "CL=/DLUAJIT_SECURITY_PRNG=0")
        endif()
        ExternalProject_Add(luajit_ext
            SOURCE_DIR        "${_lj_src}"
            BUILD_IN_SOURCE   TRUE
            CONFIGURE_COMMAND ""
            # cmd only searches the current directory for a bare command name
            # when NoDefaultCurrentDirectoryInExePath is unset. Some CI images
            # and hardened dev environments set it, and then `msvcbuild.bat`
            # fails with "is not recognized" even though chdir put us in the
            # directory containing it. Prefixing ".\" only fixes the batch file
            # itself -- msvcbuild.bat then invokes the host tools it just built
            # (minilua, buildvm) as bare names too, and those fail the same way
            # -- so unset the variable for the whole build step instead.
            # cl.exe appends the CL environment variable to every compile, so
            # the determinism define reaches all of msvcbuild.bat's compiles
            # (the bat never sets CL itself) without patching LuaJIT source.
            BUILD_COMMAND     ${CMAKE_COMMAND} -E env
                              --unset=NoDefaultCurrentDirectoryInExePath
                              ${_lj_msvc_env}
                              ${CMAKE_COMMAND} -E chdir "${_lj_src}/src"
                              cmd /c .\\msvcbuild.bat static
            INSTALL_COMMAND   ""
            BUILD_BYPRODUCTS  "${_lj_lib}")
    else()
        set(_lj_xcflags "-fPIC")
        if(WINBOLO_LUAJIT_DETERMINISTIC)
            string(APPEND _lj_xcflags " -DLUAJIT_SECURITY_PRNG=0")
        endif()
        if(APPLE)
            # Hardened runtime kills any process executing JIT-compiled code
            # from plain anonymous pages (SIGKILL "Code Signature Invalid").
            # LUAJIT_ENABLE_OSX_HRT makes LuaJIT allocate mcode with MAP_JIT +
            # pthread_jit_write_protect_np, which the runtime permits when the
            # app is signed with the com.apple.security.cs.allow-jit
            # entitlement (see src/gui/sdl3/platform/winbolo.entitlements).
            string(APPEND _lj_xcflags " -DLUAJIT_ENABLE_OSX_HRT")
        endif()
        set(_lj_make make -C "${_lj_src}" -j BUILDMODE=static "XCFLAGS=${_lj_xcflags}")
        if(APPLE)
            # LuaJIT's Makefile aborts on macOS unless MACOSX_DEPLOYMENT_TARGET
            # is exported (even for `make clean`), and each invocation emits a
            # single-arch library — so build one slice per entry in
            # CMAKE_OSX_ARCHITECTURES and lipo them into the fat archive the
            # universal link expects.
            set(_lj_env ${CMAKE_COMMAND} -E env
                "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
            set(_lj_lib "${_lj_src}/src/libluajit-fat.a")
            set(_lj_slices "")
            set(_lj_steps "")
            foreach(_lj_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
                list(APPEND _lj_steps
                    COMMAND ${_lj_env} make -C "${_lj_src}" clean
                    COMMAND ${_lj_env} ${_lj_make} "TARGET_FLAGS=-arch ${_lj_arch}"
                    COMMAND ${CMAKE_COMMAND} -E copy
                            "${_lj_src}/src/libluajit.a"
                            "${_lj_src}/src/libluajit-${_lj_arch}.a")
                list(APPEND _lj_slices "${_lj_src}/src/libluajit-${_lj_arch}.a")
            endforeach()
            list(POP_FRONT _lj_steps)   # drop leading COMMAND keyword; the rest follows BUILD_COMMAND
            ExternalProject_Add(luajit_ext
                SOURCE_DIR        "${_lj_src}"
                BUILD_IN_SOURCE   TRUE
                CONFIGURE_COMMAND ""
                BUILD_COMMAND     ${_lj_steps}
                COMMAND           lipo -create ${_lj_slices} -output "${_lj_lib}"
                INSTALL_COMMAND   ""
                BUILD_BYPRODUCTS  "${_lj_lib}")
        else()
            set(_lj_lib "${_lj_src}/src/libluajit.a")
            ExternalProject_Add(luajit_ext
                SOURCE_DIR        "${_lj_src}"
                BUILD_IN_SOURCE   TRUE
                CONFIGURE_COMMAND ""
                BUILD_COMMAND     ${_lj_make}
                INSTALL_COMMAND   ""
                BUILD_BYPRODUCTS  "${_lj_lib}")
        endif()
    endif()

    add_library(luajit_imported STATIC IMPORTED GLOBAL)
    set_target_properties(luajit_imported PROPERTIES IMPORTED_LOCATION "${_lj_lib}")
    add_dependencies(luajit_imported luajit_ext)

    # 3. Force-include the 5.4->5.1 C-API shim into every Lua-touching TU. The
    #    flag spelling is compiler-specific: MSVC uses /FI, GCC/Clang use -include.
    set(_wbn_ljshim "${CMAKE_SOURCE_DIR}/src/gui/sdl3/luajit_compat.h")
    if(MSVC)
        set(_wbn_ljforce "/FI${_wbn_ljshim}")
    else()
        set(_wbn_ljforce "-include" "${_wbn_ljshim}")
    endif()

    # 4. Present LuaJIT under the stable lua_static / lua_internal names so every
    #    existing `target_link_libraries(... lua_static ...)` inherits it. LuaJIT
    #    needs libdl/libm on Unix — pulled via ${CMAKE_DL_LIBS} + the m below.
    add_library(lua_static INTERFACE)
    target_link_libraries(lua_static INTERFACE luajit_imported ${CMAKE_DL_LIBS})
    if(NOT MSVC)
        target_link_libraries(lua_static INTERFACE m)
    endif()
    target_include_directories(lua_static INTERFACE "${_lj_src}/src")
    target_compile_definitions(lua_static INTERFACE WINBOLO_LUAJIT)
    target_compile_options(lua_static INTERFACE ${_wbn_ljforce})

    add_library(lua_internal INTERFACE)
    target_include_directories(lua_internal INTERFACE "${_lj_src}/src")
    target_compile_definitions(lua_internal INTERFACE WINBOLO_LUAJIT)
    target_compile_options(lua_internal INTERFACE ${_wbn_ljforce})
else()
    # Lua — only build the static library, skip interpreter/compiler/tests
    set(LUA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
    set(LUA_BUILD_COMPILER OFF CACHE BOOL "" FORCE)
    set(LUA_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(lua)
    # lua_static must be built with -fPIC on Linux so it can link into shared libs (winbolo_gym)
    set_target_properties(lua_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_compile_definitions(lua_internal INTERFACE LUA_USE_IOS)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iOS: vendored freetype, no harfbuzz (avoids CoreText linking issues)
    # Static only — shared libs require code signing in Xcode
    # Point SDL3_ttf's find_package(SDL3) at the in-tree build so it can
    # locate the static-only SDL3 we just built.
    set(SDL3_DIR "${sdl3_BINARY_DIR}" CACHE PATH "" FORCE)
    set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
    set(SDLTTF_SHARED OFF CACHE BOOL "" FORCE)
    set(SDLTTF_STATIC ON CACHE BOOL "" FORCE)
    # SDL3_ttf reads BUILD_SHARED_LIBS (not SDLTTF_SHARED) to decide whether
    # to look for SDL3::SDL3-shared.  Force it OFF so it uses the static SDL3.
    set(_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl3_ttf imgui zlib)
    set(BUILD_SHARED_LIBS "${_saved_build_shared_libs}" CACHE BOOL "" FORCE)
    # Restore try_compile mode now that all probing dependencies are fetched
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_saved_try_compile_type}")
else()
    # MinGW (MSYS2): vendor freetype so we don't depend on vcpkg or system packages.
    # macOS: vendor freetype so a universal2 (arm64;x86_64) build doesn't link
    # against an arm64-only Homebrew libfreetype.dylib.
    if(MINGW OR APPLE)
        set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
        set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
    endif()
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl3_ttf imgui zlib)
endif()

# MinGW debug builds: HarfBuzz's single-file amalgamation overflows COFF debug
# section limits. Use -Wa,-mbig-obj so the assembler emits PE-COFF bigobj format.
if(MINGW AND TARGET harfbuzz)
    target_compile_options(harfbuzz PRIVATE -Wa,-mbig-obj)
endif()

# On Unix (non-iOS), shared libraries have a versioned filename (e.g. libSDL3_ttf.0.2.0.dylib)
# and a soname symlink (libSDL3_ttf.0.dylib).  Setting VERSION = SOVERSION makes the real
# filename match the soname so copies just work.
if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    foreach(_lib SDL3_ttf-shared SDL3-shared)
        if(TARGET ${_lib})
            get_target_property(_sover ${_lib} SOVERSION)
            if(NOT _sover STREQUAL "_sover-NOTFOUND")
                set_target_properties(${_lib} PROPERTIES VERSION "${_sover}")
            endif()
        endif()
    endforeach()
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Build libmaxminddb as static to avoid needing a separate DLL
    set(BUILD_SHARED_LIBS_SAVE "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(MAXMINDDB_BUILD_BINARIES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libmaxminddb)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE}" CACHE BOOL "" FORCE)

    # libopus — voice codec, forced static
    set(BUILD_SHARED_LIBS_SAVE3 "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(opus)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE3}" CACHE BOOL "" FORCE)

    # SpeexDSP — the acoustic echo canceller in the voice capture path, forced
    # static.  Its CMake build declares no test or program targets, so there is
    # nothing of that kind to switch off; the one option it does carry picks
    # fixed- over floating-point arithmetic, and every platform we build for
    # has an FPU.
    set(BUILD_SHARED_LIBS_SAVE4 "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(USE_FIXED_POINT OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(speexdsp)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE4}" CACHE BOOL "" FORCE)
endif()

# cJSON — force static.  Outside the guard above: the iOS target compiles
# src/common/prefs_doc.c and src/bolo/lobby_bot_pools_json.c and links cjson.
set(BUILD_SHARED_LIBS_SAVE2 "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cjson)
# cJSON enables -Werror which breaks on MinGW (isnan/isinf float-conversion warnings)
if(TARGET cjson AND MINGW)
    target_compile_options(cjson PRIVATE -Wno-error)
endif()
set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE2}" CACHE BOOL "" FORCE)

# minizip library from zlib's contrib/minizip.  Outside the guard above: zlib
# is fetched on iOS too, and the iOS client reads skin archives through it.
add_library(minizip STATIC
    ${zlib_SOURCE_DIR}/contrib/minizip/zip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/unzip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/ioapi.c
)
target_include_directories(minizip PUBLIC ${zlib_SOURCE_DIR}/contrib/minizip)
target_link_libraries(minizip PUBLIC zlibstatic)

# nanosvg — header-only SVG parser/rasterizer (vendored)
add_library(nanosvg STATIC src/third_party/nanosvg/nanosvg_impl.c)
target_include_directories(nanosvg SYSTEM PUBLIC src/third_party/nanosvg)
if(MSVC)
    target_compile_options(nanosvg PRIVATE /wd4201 /wd4244 /wd4267)
else()
    target_compile_options(nanosvg PRIVATE -Wno-unused-function)
endif()

# mdns — header-only mDNS/DNS-SD library (vendored, public domain). All
# functions are static inline, so no implementation TU: an INTERFACE lib
# carries only the include directory. The one consumer (mdns_advertise.c)
# applies -Wno-unused-function / /wd4505 locally for the static-inline noise.
add_library(mdns INTERFACE)
target_include_directories(mdns SYSTEM INTERFACE src/third_party/mdns)

# stb_image — header-only image loader (vendored)
add_library(stb_image STATIC src/third_party/stb/stb_image_impl.c)
target_include_directories(stb_image SYSTEM PUBLIC src/third_party/stb)
if(NOT MSVC)
    target_compile_options(stb_image PRIVATE -Wno-unused-function)
endif()

# utf8proc — Unicode normalization + property tables (vendored, MIT)
# Compiled as a static lib to avoid duplicating its ~2MB Unicode table
# across every target that links the bolo sources.
add_library(utf8proc STATIC src/third_party/utf8proc/utf8proc.c)
target_include_directories(utf8proc SYSTEM PUBLIC src/third_party/utf8proc)
target_compile_definitions(utf8proc PUBLIC UTF8PROC_STATIC=1)
set_target_properties(utf8proc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(utf8proc PRIVATE /wd4244 /wd4267)
endif()

# ImGui backends library for SDL3 + SDL Renderer
add_library(imgui_backends STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui_backends PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_backends PUBLIC SDL3::SDL3)

# Enable C++ for ImGui
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------

if(MSVC)
    # COMPILE_LANGUAGE guard prevents these C/C++ flags from being passed
    # to MASM (ml64.exe) for crashpad's .asm files.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:C,CXX>:/WX> $<$<COMPILE_LANGUAGE:C,CXX>:/sdl> $<$<COMPILE_LANGUAGE:C,CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:C,CXX>:/W4> $<$<COMPILE_LANGUAGE:C,CXX>:/external:W3>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4005> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4013>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4018> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4057>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4100> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4101>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4127> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4131>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4132> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4146>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4152> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4189>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4244> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4245>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4295> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4310>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4456> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4457>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4459> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4505>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4554> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4701>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4702> $<$<COMPILE_LANGUAGE:C,CXX>:/wd4996>
        $<$<COMPILE_LANGUAGE:C,CXX>:/guard:cf> $<$<COMPILE_LANGUAGE:C,CXX>:/GL>
        # /RTC1 (Debug runtime checks) is incompatible with /fsanitize=address;
        # gate it off when ENABLE_ASAN is on so the two don't fight.
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Debug>,$<NOT:$<BOOL:${ENABLE_ASAN}>>>:/RTC1>
    )
    add_link_options(/GUARD:CF /INCREMENTAL:NO /LTCG)
else()
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-sign-compare
        -Wno-implicit-fallthrough
    )
    # -Werror=undef makes an undefined macro in #if an error instead of a
    # silent 0, so a lobby source that tests one of the BOLO_* feature
    # macros without including lobby_internal.h fails to build. Set on
    # those sources rather than added above, because add_compile_options
    # is inherited by every target created later in this directory --
    # including the FetchContent dependencies, and libplum's log.c tests
    # an undefined RELEASE. Not set for MSVC: C4668 is forced to level 1
    # and fires throughout the Windows SDK headers, where /external:W3
    # cannot suppress it and the /WX above would make it fatal.
    #
    # The second option makes a function with external linkage and no
    # previous declaration an error, so every symbol the lobby's sources
    # share has to be declared in lobby_internal.h, and a definition cannot
    # lose its static without gaining a declaration. The spelling is not
    # interchangeable between the two compilers: Clang wants
    # -Wmissing-prototypes and uses -Wmissing-declarations for an unrelated
    # diagnostic (a declaration that declares nothing), while GCC wants
    # -Wmissing-declarations and rejects -Wmissing-prototypes outright in
    # C++.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(LOBBY_MISSING_DECL_OPTION -Werror=missing-prototypes)
    else()
        set(LOBBY_MISSING_DECL_OPTION -Werror=missing-declarations)
    endif()
    set_source_files_properties(
        src/gui/sdl3/dialogs/imgui_lobby.cpp
        src/gui/sdl3/dialogs/lobby/lobby_assets.cpp
        src/gui/sdl3/dialogs/lobby/lobby_chat.cpp
        src/gui/sdl3/dialogs/lobby/lobby_settings.cpp
        src/gui/sdl3/dialogs/lobby/lobby_status.cpp
        src/gui/sdl3/dialogs/lobby/lobby_players.cpp
        src/gui/sdl3/dialogs/lobby/lobby_map_preview.cpp
        src/gui/sdl3/dialogs/lobby/lobby_commands.cpp
        src/gui/sdl3/dialogs/lobby/lobby_recap.cpp
        src/gui/sdl3/dialogs/lobby/lobby_rating.cpp
        src/gui/sdl3/dialogs/lobby/lobby_reel.cpp
        src/gui/sdl3/dialogs/lobby/lobby_clip_gif.cpp
        src/gui/sdl3/dialogs/lobby/lobby_chooser.cpp
        src/gui/sdl3/dialogs/lobby/lobby_scenario_chooser.cpp
        src/gui/sdl3/dialogs/lobby/lobby_wbn_maps.cpp
        src/gui/sdl3/wbn_map_source.cpp
        PROPERTIES COMPILE_OPTIONS "-Werror=undef;${LOBBY_MISSING_DECL_OPTION}"
    )
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer (opt-in via -DENABLE_ASAN=ON)
#
# Instruments allocations to catch heap overflows, use-after-free, and
# stack/global overflows at the moment of the bad access. Slows the
# binary ~2-3x. On MinGW the runtime DLL (libasan*.dll, libssp*.dll)
# must be on PATH or next to the .exe. With MSVC, /RTC1 is incompatible;
# this block strips it from Debug when ASan is enabled.
# ---------------------------------------------------------------------------

option(ENABLE_ASAN "Build with AddressSanitizer instrumentation" OFF)
if(ENABLE_ASAN)
    if(MSVC)
        # /RTC1 is gated off above when ENABLE_ASAN is on so the two
        # don't conflict; just turn ASan on here.
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "AddressSanitizer ENABLED")
endif()

# ---------------------------------------------------------------------------
# Flat output directories (no /Debug, /Release subdirs under MSVC)
# ---------------------------------------------------------------------------

function(set_flat_output TARGET OUTDIR)
    set_target_properties(${TARGET} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${OUTDIR}"
        LIBRARY_OUTPUT_DIRECTORY "${OUTDIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${OUTDIR}"
    )
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
            string(TOUPPER "${cfg}" CFGU)
            set_target_properties(${TARGET} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${CFGU} "${OUTDIR}"
                LIBRARY_OUTPUT_DIRECTORY_${CFGU} "${OUTDIR}"
                ARCHIVE_OUTPUT_DIRECTORY_${CFGU} "${OUTDIR}"
            )
        endforeach()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Sentry crash reporting — optional.
# Set -DENABLE_SENTRY=OFF to skip entirely.
# Pass -DSENTRY_DSN="https://key@o0.ingest.sentry.io/0" to enable reporting.
#
# Declared here, above the platform split, because the iOS target reads
# HAVE_SENTRY_IOS / SENTRY_COCOA_XCFW inside that split and CMake is
# sequential.  The desktop sentry-native fetch stays below with the other
# desktop dependencies: it manipulates BUILD_SHARED_LIBS and SENTRY_BACKEND
# and must keep its position relative to them.  The iOS side is a plain
# download of a prebuilt xcframework, so it moves without side effects.
# ---------------------------------------------------------------------------
option(ENABLE_SENTRY "Enable Sentry crash reporting" ON)
set(SENTRY_DSN "" CACHE STRING "Sentry DSN for crash reporting")
set(HAVE_SENTRY FALSE)
set(HAVE_SENTRY_IOS FALSE)

# Sentry crash reporting — iOS (sentry-cocoa static xcframework).
# Requires CMake 3.28+ for xcframework support with the Xcode generator.
#
# SOURCE_DIR is pinned instead of taking the FetchContent default.
# Sentry.xcframework.zip holds a single top-level directory, which URL
# extraction strips, so the bundle's contents land directly in the
# destination directory.  target_link_libraries recognises an xcframework
# by the .xcframework suffix on the directory name, so the destination has
# to supply that name itself — with the default name the link step reports
# a missing Info.plist.
if(ENABLE_SENTRY AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    FetchContent_Declare(
        sentry_cocoa
        URL https://github.com/getsentry/sentry-cocoa/releases/download/9.8.0/Sentry.xcframework.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/Sentry.xcframework"
    )
    FetchContent_MakeAvailable(sentry_cocoa)
    set(SENTRY_COCOA_XCFW "${sentry_cocoa_SOURCE_DIR}")
    set(HAVE_SENTRY_IOS TRUE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(HAVE_SENTRY_IOS)
        message(STATUS "Sentry crash reporting enabled (sentry-cocoa)")
    else()
        message(STATUS "Sentry crash reporting disabled")
    endif()
endif()

# ===========================================================================
# Platform targets: iOS vs Desktop
# ===========================================================================

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")

# ---------------------------------------------------------------------------
# WinBolo iOS Client
# ---------------------------------------------------------------------------

enable_language(OBJC)

set(IOS_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/gui/ios/Info.plist")

# App icon asset catalog
set(IOS_ASSET_CATALOG "${CMAKE_SOURCE_DIR}/src/gui/ios/Assets.xcassets")

# Data files to bundle
# Data files that go into the data/ subdirectory of the bundle
set(IOS_DATA_BMP
    ${CMAKE_SOURCE_DIR}/data/background.bmp
    ${CMAKE_SOURCE_DIR}/data/skin.bmp
    ${CMAKE_SOURCE_DIR}/data/tile.bmp
    ${CMAKE_SOURCE_DIR}/data/tanks.bmp
    ${CMAKE_SOURCE_DIR}/data/boats.bmp
    ${CMAKE_SOURCE_DIR}/data/items.bmp
    ${CMAKE_SOURCE_DIR}/data/splash.bmp
    ${CMAKE_SOURCE_DIR}/data/logo.bmp
    ${CMAKE_SOURCE_DIR}/data/base.bmp
    ${CMAKE_SOURCE_DIR}/data/pillbox.bmp
)
# Data files that go into the root of the bundle (next to the executable)
set(IOS_DATA_ROOT
    ${CMAKE_SOURCE_DIR}/data/smalllogo.bmp
    ${CMAKE_SOURCE_DIR}/data/smalllogo-transparent.png
    ${CMAKE_SOURCE_DIR}/data/button_practice.bmp
    ${CMAKE_SOURCE_DIR}/data/button_tcp.bmp
    ${CMAKE_SOURCE_DIR}/data/button_lan.bmp
    ${CMAKE_SOURCE_DIR}/data/button_internet.bmp
)
# Maps that go into the root (Everard Island for single-player)
set(IOS_DATA_ROOT_MAPS
    "${CMAKE_SOURCE_DIR}/data/maps/Everard Island.map"
    "${CMAKE_SOURCE_DIR}/data/maps/Inbuilt Tutorial.map"
)
file(GLOB IOS_DATA_FONT
    "${CMAKE_SOURCE_DIR}/data/fonts/*.ttf"
    "${CMAKE_SOURCE_DIR}/data/fonts/*.otf"
)
file(GLOB IOS_DATA_MAPS "${CMAKE_SOURCE_DIR}/data/maps/*.map")
file(GLOB IOS_DATA_SOUNDS "${CMAKE_SOURCE_DIR}/data/sounds/*.wav")
file(GLOB IOS_DATA_SVG "${CMAKE_SOURCE_DIR}/data/svg/*.png" "${CMAKE_SOURCE_DIR}/data/svg/*.svg")
file(GLOB IOS_DATA_UI "${CMAKE_SOURCE_DIR}/data/ui/*.svg")
# The smart-ping glyphs sit in their own folder, so the flat data/ui glob
# above does not reach them.
file(GLOB IOS_DATA_UI_PING "${CMAKE_SOURCE_DIR}/data/ui/ping/*.svg")
file(GLOB IOS_LANG_FILES "${CMAKE_SOURCE_DIR}/data/lang/*.txt")
# About-box markdown docs that live at the repo root but ship in data/
set(IOS_ABOUT_DOCS
    "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${CMAKE_SOURCE_DIR}/AUTHORS.md"
)

# Bolo engine sources that stay per-target (HAVE_STEAM-divergent).
# Everything else now lives in bolo_static (added via add_subdirectory
# below). iOS doesn't define HAVE_STEAM, so PLAYER_FLAG_STEAM_BUILD is
# never OR'd into selfFlags / clientHints by this TU.
set(IOS_BOLO_SOURCES
    src/bolo/transport_udp_client.c
)

# Build bolo_static for the iOS branch. iOS doesn't set HAVE_ONNXRUNTIME
# or ENABLE_PILLMASSAGE_BUG, so bolo_static here has the matching
# minimal feature set.
add_subdirectory(src/bolo)
add_subdirectory(src/server)
add_subdirectory(src/scenario)
add_subdirectory(brains)

# Server sources needed for single-player mode
# server_sim.c + servermessages.c now come from server_sim_static;
# threads.c comes from threads_static.
set(IOS_SERVER_SOURCES
    src/server/server_stubs.c
    src/mapeditor/mapgen_imgui.cpp
)



# GUI/support sources (reused from sdl3 — matches desktop CLIENT_SOURCES minus winbolo.c)
set(IOS_GUI_SOURCES
    src/gui/sdl3/clientmutex.c
    src/gui/sdl3/sdl3imgui.cpp
    src/gui/sdl3/imgui_steam_nav.cpp
    src/gui/sdl3/sdl3imgui_tablet.cpp
    src/gui/sdl3/sdl3draw.c
    src/gui/sdl3/sdl3draw_status.c
    src/gui/sdl3/tank_label.c
    src/gui/sdl3/mapview.c
    src/gui/sdl3/mapview_overlay.c
    src/gui/sdl3/sprite_positions.c
    src/gui/sdl3/tileloader.c
    src/gui/sdl3/skin_source.c
    src/gui/sdl3/gfx_settings.c
    src/gui/sdl3/sdl_bmp.c
    src/gui/sdl3/sound.c
    src/gui/sdl3/sound_variants.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/bg_game.c
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/ping_overlay.cpp
    src/gui/sdl3/ping_icons.c
    src/gui/sdl3/ping_marker.c
    src/gui/sdl3/ring_band.c
    src/gui/sdl3/flags.c
    src/gui/sdl3/dns_lookups.c
    src/gui/sdl3/input.c
    src/gui/sdl3/input_touch.c
    src/gui/sdl3/input_joystick.c
    src/gui/sdl3/input_gamepad.c
    src/gui/sdl3/glyphs.c
    src/gui/sdl3/build_cursor.c
    src/gui/sdl3/input_source.c
    src/gui/sdl3/tutorial_text.c
    src/gui/ui_mode.c
    src/gui/visibility_presets.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/gui/sdl3/dialogs/imgui_welcome.cpp
    src/gui/sdl3/dialogs/imgui_mapchooser.cpp
    src/gui/sdl3/dialogs/imgui_udpsetup.cpp
    src/gui/sdl3/dialogs/imgui_gamebrowser.cpp
    src/gui/sdl3/dialogs/imgui_setname.cpp
    src/gui/sdl3/dialogs/imgui_trackersetup.cpp
    src/gui/sdl3/dialogs/imgui_winbolonet.cpp
    src/gui/sdl3/dialogs/imgui_messagebox.cpp
    src/gui/sdl3/dialogs/imgui_keycap.cpp
    src/gui/sdl3/dialogs/imgui_lobby.cpp
    src/gui/sdl3/dialogs/lobby/lobby_assets.cpp
    src/gui/sdl3/dialogs/lobby/lobby_chat.cpp
    src/gui/sdl3/dialogs/lobby/lobby_settings.cpp
    src/gui/sdl3/dialogs/lobby/lobby_status.cpp
    src/gui/sdl3/dialogs/lobby/lobby_players.cpp
    src/gui/sdl3/dialogs/lobby/lobby_map_preview.cpp
    src/gui/sdl3/dialogs/lobby/lobby_commands.cpp
    src/gui/sdl3/dialogs/lobby/lobby_recap.cpp
    src/gui/sdl3/dialogs/lobby/lobby_reel.cpp
    src/gui/sdl3/dialogs/lobby/lobby_chooser.cpp
    src/gui/sdl3/dialogs/lobby/lobby_scenario_chooser.cpp
    src/gui/sdl3/dialogs/lobby/lobby_wbn_maps.cpp
    src/gui/sdl3/dialogs/imgui_settings.cpp
    src/gui/sdl3/dialogs/imgui_keysetup.cpp
    src/gui/sdl3/dialogs/imgui_deck_pause.cpp
    src/gui/sdl3/dialogs/imgui_tutorial_overlay.cpp
    src/gui/sdl3/dialogs/imgui_keyboard.cpp
    src/gui/sdl3/dialogs/imgui_quickchat.cpp
    src/gui/sdl3/dialogs/imgui_controller_prompt.cpp
    src/gui/sdl3/dialogs/imgui_controller_disconnect.cpp
    src/gui/sdl3/dialogs/imgui_onboarding.cpp
    src/gui/sdl3/dialogs/imgui_about.cpp
    src/gui/sdl3/dialogs/dialog_footer.cpp
    src/gui/sdl3/dialogs/imgui_nav_outline.cpp
    src/gui/sdl3/map_preview_popup.cpp
    src/gui/sdl3/lobby_start_list.cpp
    src/gui/sdl3/map_preview_view.cpp
    src/gui/sdl3/map_preview_fetch.cpp
    src/gui/sdl3/overview_camera.cpp
    src/gui/sdl3/overview_fog.cpp
    src/gui/sdl3/overview_hud_layout.cpp
    src/gui/sdl3/overview_view.cpp
    src/gui/sdl3/map_stars.cpp
    src/gui/sdl3/wbn_map_source.cpp
    src/gui/sdl3/wb_theme.cpp
)

# Shared mobile UI (touch input, legacy players panel for non-tablet mode)
set(IOS_MOBILE_SOURCES
    src/gui/mobile/touch_input.c
    src/gui/mobile/players_panel.cpp
)

# Lua brain scripts to bundle
file(GLOB IOS_BRAIN_SCRIPTS "${CMAKE_SOURCE_DIR}/brains/GoalHunter_1.7/*.lua")


add_executable(WinBoloIOS MACOSX_BUNDLE
    src/gui/ios/winbolo_ios.m
    src/gui/ios/ios_stubs.c
    src/gui/ios/imgui_ios.cpp
    src/gui/sdl3/gamefront.c
    src/client_frontend/client_frontend_connect.c
    src/client_frontend/client_frontend_tick.c
    src/bolo/lobby_bot_pools_json.c
    src/gui/sdl3/dialog_backend.c
    # PNG writer for the map chooser's thumbnail cache (imgui_mapchooser.cpp).
    # The stb_image target compiles only the reader; the writer is a separate
    # impl TU each client adds for itself.
    src/third_party/stb/stb_image_write_impl.c
    ${IOS_BOLO_SOURCES}
    ${IOS_SERVER_SOURCES}
    ${IOS_GUI_SOURCES}
    ${IOS_MOBILE_SOURCES}
    ${IOS_INFO_PLIST}
    ${IOS_DATA_BMP}
    ${IOS_DATA_ROOT}
    ${IOS_DATA_ROOT_MAPS}
    ${IOS_DATA_MAPS}
    ${IOS_DATA_FONT}
    ${IOS_BRAIN_SCRIPTS}
    ${IOS_DATA_SOUNDS}
    ${IOS_DATA_SVG}
    ${IOS_DATA_UI}
    ${IOS_DATA_UI_PING}
    ${IOS_LANG_FILES}
    ${IOS_ABOUT_DOCS}
    ${IOS_ASSET_CATALOG}
)

# Info.plist is used by CMake for bundle metadata, not compiled — mark as header
set_source_files_properties(${IOS_INFO_PLIST} PROPERTIES
    HEADER_FILE_ONLY TRUE
)

# Place data files in the app bundle
set_source_files_properties(${IOS_DATA_BMP} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data"
)
set_source_files_properties(${IOS_DATA_FONT} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/fonts"
)
set_source_files_properties(${IOS_DATA_MAPS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/maps"
)
set_source_files_properties(${IOS_BRAIN_SCRIPTS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Brains/GoalHunter_1.7"
)
set_source_files_properties(${IOS_DATA_SOUNDS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/sounds"
)
set_source_files_properties(${IOS_DATA_SVG} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/svg"
)
set_source_files_properties(${IOS_DATA_UI} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/ui"
)
set_source_files_properties(${IOS_DATA_UI_PING} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/ui/ping"
)
set_source_files_properties(${IOS_LANG_FILES} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data/lang"
)
set_source_files_properties(${IOS_ABOUT_DOCS} PROPERTIES
    MACOSX_PACKAGE_LOCATION "data"
)
set_source_files_properties(${IOS_DATA_ROOT} PROPERTIES
    MACOSX_PACKAGE_LOCATION ""
)
set_source_files_properties(${IOS_DATA_ROOT_MAPS} PROPERTIES
    MACOSX_PACKAGE_LOCATION ""
)
set_source_files_properties(${IOS_ASSET_CATALOG} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
)

# Organize in Xcode sidebar
source_group("Resources" FILES ${IOS_INFO_PLIST} ${IOS_DATA_BMP} ${IOS_DATA_FONT} ${IOS_DATA_MAPS} ${IOS_DATA_SOUNDS} ${IOS_DATA_SVG} ${IOS_DATA_UI} ${IOS_DATA_UI_PING})
source_group("Brains" FILES ${IOS_BRAIN_SCRIPTS})
source_group("Bolo Engine" FILES ${IOS_BOLO_SOURCES})
source_group("Server" FILES ${IOS_SERVER_SOURCES})
source_group("GUI" FILES ${IOS_GUI_SOURCES})
source_group("Mobile" FILES ${IOS_MOBILE_SOURCES})
source_group("iOS" FILES
    src/gui/ios/winbolo_ios.m
    src/gui/ios/ios_stubs.c
    src/gui/ios/imgui_ios.cpp
    src/gui/ios/gamefront_ios.c
)

set_target_properties(WinBoloIOS PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${IOS_INFO_PLIST}"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.winbolo.ios"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_INCLUDE_ALL_APPICON_ASSETS "YES"
)

target_include_directories(WinBoloIOS PRIVATE
    src/gui/sdl3
    src/gui/sdl3/dialogs
    src/gui
    src/gui/ios
    src/gui/mobile
    src/server
    src/mapeditor
    src/client_frontend
    src/third_party/imgui_markdown
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(WinBoloIOS gui)
# bg_game.c: same grant as the desktop target — the dev-mode lobby
# background assigns bot teams through server_sim_lifecycle.h.
bolo_grant_internal_source_access(WinBoloIOS
    src/bolo/transport_udp_client.c
    src/gui/sdl3/bg_game.c)

target_link_libraries(WinBoloIOS PRIVATE bolo_static scenario_static server_sim_static bot_brains_static threads_static)
target_link_libraries(WinBoloIOS PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends lua_static
    nanosvg stb_image utf8proc zlibstatic minizip cjson
    "-framework CoreText" "-framework CoreFoundation" "-framework CoreGraphics"
    "-framework AVFoundation"
    "-framework GameController"
)

target_compile_options(WinBoloIOS PRIVATE -fcommon)
target_compile_definitions(WinBoloIOS PRIVATE
    HAVE_SCREEN_C=1
    __IPHONEOS__=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

if(HAVE_SENTRY_IOS)
    target_compile_definitions(WinBoloIOS PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBoloIOS PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBoloIOS PRIVATE "${SENTRY_COCOA_XCFW}")
    # sentry-cocoa is a Swift framework and this target has no Swift of its
    # own, so Xcode never runs its Swift link step and the static
    # compatibility shim the framework's objects reference —
    # __swift_FORCE_LOAD_$_swiftCompatibility56 — is left unresolved.  It is
    # the only swiftCompatibility* the framework asks for; the ~29 other
    # Swift force-load symbols in it are the OS runtime, which resolves from
    # the SDK and needs no embedding (Swift 5 has shipped inside iOS since
    # 12.2, well below this target's 15.0 floor).
    #
    # The shim lives in the toolchain, one directory per SDK, so the search
    # path has to be expanded by Xcode rather than by CMake: $(PLATFORM_NAME)
    # resolves to iphoneos or iphonesimulator from the destination being
    # built, so a single configure serves a device and a simulator build.
    # Passed as link flags rather than through
    # XCODE_ATTRIBUTE_LIBRARY_SEARCH_PATHS because CMake already writes that
    # setting for this target — it is where the SDK's usr/lib/swift comes
    # from — and setting the attribute replaces that value instead of adding
    # to it, which would strand the OS runtime symbols instead.
    target_link_libraries(WinBoloIOS PRIVATE
        "-L$(TOOLCHAIN_DIR)/usr/lib/swift/$(PLATFORM_NAME)"
        "-lswiftCompatibility56")
    # winbolo_ios.m reaches the SDK through `@import Sentry`, which needs
    # Objective-C modules.  Set per-source rather than target-wide: the rest
    # of WinBoloIOS is C and C++ that has no reason to change how it
    # compiles.  A different source property from the internal-access grant
    # above, so the two do not overwrite each other.
    set_source_files_properties(src/gui/ios/winbolo_ios.m
        PROPERTIES COMPILE_OPTIONS "-fmodules")
endif()

target_sources(WinBoloIOS PRIVATE src/steam/steam_wrapper_stub.c)
target_sources(WinBoloIOS PRIVATE src/common/wb_log.c)
target_sources(WinBoloIOS PRIVATE src/common/md5.c)
target_sources(WinBoloIOS PRIVATE src/common/mp_diag_log.c)
target_sources(WinBoloIOS PRIVATE src/common/prefs.c)
target_sources(WinBoloIOS PRIVATE src/common/prefs_doc.c)
set_flat_output(WinBoloIOS "${CMAKE_BINARY_DIR}")

# Hide dependency targets from the Xcode project — only WinBoloIOS should
# appear as a buildable scheme.
foreach(_t
    SDL3-static SDL3_Headers SDL3_test
    SDL3_ttf-static
    freetype
    lua_static lua_internal
    imgui_backends
    nanosvg stb_image
)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endforeach()

else() # NOT iOS — Desktop targets (Windows, Linux, macOS)

# libcurl for WinBolo.net HTTP/HTTPS
if(WIN32)
    # Windows: build curl from source using Schannel (no OpenSSL needed)
    set(BUILD_SHARED_LIBS_SAVE_CURL "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(curl)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE_CURL}" CACHE BOOL "" FORCE)
    # Provide the CURL::libcurl alias that the rest of the build expects
    if(NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl ALIAS libcurl_shared)
    endif()
else()
    # Linux/macOS: use system-installed libcurl
    find_package(CURL REQUIRED)
endif()

find_package(OpenMP)

# Classic "pillmassage" bug — pillboxes miss tanks at very close range due to
# the original Bolo forward-prediction formula breaking down at short distances.
# Enable for authentic behaviour matching the original game.
option(ENABLE_PILLMASSAGE_BUG "Enable classic Bolo pillmassage close-range miss bug" OFF)
if(ENABLE_PILLMASSAGE_BUG)
    add_compile_definitions(ENABLE_PILLMASSAGE_BUG)
endif()

# Net-debug instrumentation: per-player sim-executed counters and the
# netdebug.* repro tests. OFF in every shipping build.
option(WB_NETDEBUG "Build net-debug instrumentation and netdebug tests" OFF)
if(WB_NETDEBUG)
    # Global, not per-target: the gated counters live inside struct
    # ServerSim, so every TU must agree on the struct layout — a partial
    # define would diverge struct sizes across libraries.
    add_compile_definitions(WB_NETDEBUG=1)
endif()

# Voice capture-chain recorder: WAV taps at each stage of the capture chain,
# the two CSVs, and the WAV injection path (src/client_frontend/voice_debug.c).
# OFF in every shipping build. The source is on the WinBolo target only, which
# is the only target that compiles voice_client.c.
option(WB_VOICEDEBUG "Build the voice capture-chain recorder" OFF)
if(WB_VOICEDEBUG)
    add_compile_definitions(WB_VOICEDEBUG=1)
endif()

# Fuzzing: libFuzzer targets over the wire parsers and the server packet
# dispatcher (hardening plan §1.2). Linux + Clang only. This is a DEDICATED
# build config, not mixed with a normal build: when ON, the whole build is
# instrumented with AddressSanitizer + libFuzzer coverage. Normal MSVC/GCC
# builds never set WB_FUZZ, so they are untouched. The WB_FUZZ define is
# global (not per-target) so server_static's transport_udp_server.c picks up
# the WB_FUZZ-gated dispatcher seam — same rationale as WB_NETDEBUG above.
# AFL++ is the fallback if requiring Clang is unwanted (see tests/fuzz/README.md).
option(WB_FUZZ "Build libFuzzer fuzz targets (Linux + Clang only)" OFF)
if(WB_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "WB_FUZZ requires Clang for -fsanitize=fuzzer,address. Configure a "
            "separate build dir with -DCMAKE_C_COMPILER=clang "
            "-DCMAKE_CXX_COMPILER=clang++, or use the AFL++ fallback documented "
            "in tests/fuzz/README.md.")
    endif()
    add_compile_definitions(WB_FUZZ=1)
    # Strip wb_log compile-time so the only thing on the fuzzer's stderr is
    # libFuzzer's own stats (exec count, coverage) — a multi-hour run's log
    # stays small and still reports the total runs. Raw [UDP SERVER] fprintfs
    # in the hot path are separately gated under WB_FUZZ in the source.
    set(WINBOLO_LOG_LEVEL "OFF" CACHE STRING "fuzz build strips wb_log" FORCE)
    # Instrument every TU for coverage + ASan; only the fuzz executables below
    # additionally link libFuzzer's main via -fsanitize=fuzzer.
    add_compile_options(-fsanitize=fuzzer-no-link,address -g -O1
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# libplum — automatic UPnP / NAT-PMP / PCP port mapping for hosted servers.
# When OFF, src/bolo/nat_portmap.c compiles to no-op stubs.
option(BOLO_PORTMAP "Enable UPnP/NAT-PMP automatic port mapping" ON)

# wb_log compile-time level. Calls below this level expand to ((void)0)
# and are stripped by the compiler. Empty = wb_log.h decides (DEBUG in
# Debug builds, INFO in Release).
set(WINBOLO_LOG_LEVEL "" CACHE STRING
    "Compile-time log level for wb_log macros: TRACE/DEBUG/INFO/WARN/ERROR/OFF (empty = auto)")
set_property(CACHE WINBOLO_LOG_LEVEL PROPERTY STRINGS
    "" "TRACE" "DEBUG" "INFO" "WARN" "ERROR" "OFF")
if(WINBOLO_LOG_LEVEL)
    if(WINBOLO_LOG_LEVEL STREQUAL "TRACE")
        set(_WB_LOG_LEVEL_NUM 1)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "DEBUG")
        set(_WB_LOG_LEVEL_NUM 2)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "INFO")
        set(_WB_LOG_LEVEL_NUM 3)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "WARN")
        set(_WB_LOG_LEVEL_NUM 4)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "ERROR")
        set(_WB_LOG_LEVEL_NUM 5)
    elseif(WINBOLO_LOG_LEVEL STREQUAL "OFF")
        set(_WB_LOG_LEVEL_NUM 6)
    else()
        message(FATAL_ERROR
            "WINBOLO_LOG_LEVEL must be one of TRACE/DEBUG/INFO/WARN/ERROR/OFF (got '${WINBOLO_LOG_LEVEL}')")
    endif()
    add_compile_definitions(WB_LOG_LEVEL=${_WB_LOG_LEVEL_NUM})
    message(STATUS "wb_log: compile-time level = ${WINBOLO_LOG_LEVEL} (${_WB_LOG_LEVEL_NUM})")
else()
    message(STATUS "wb_log: compile-time level = auto (DEBUG in Debug builds, INFO in Release)")
endif()

# ONNX Runtime — optional dependency for ML brain inference
# Downloads prebuilt binaries via FetchContent when not already installed.
# Set -DENABLE_ONNXRUNTIME=OFF to skip entirely.
option(ENABLE_ONNXRUNTIME "Enable ONNX Runtime ML brain support" ON)
set(HAVE_ONNXRUNTIME FALSE)

if(ENABLE_ONNXRUNTIME AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # Try system/vcpkg install first
    find_package(onnxruntime QUIET)
    if(onnxruntime_FOUND)
        set(HAVE_ONNXRUNTIME TRUE)
    else()
        find_library(ONNXRUNTIME_LIB onnxruntime)
        find_path(ONNXRUNTIME_INCLUDE onnxruntime_c_api.h PATH_SUFFIXES onnxruntime)
        if(ONNXRUNTIME_LIB AND ONNXRUNTIME_INCLUDE)
            set(HAVE_ONNXRUNTIME TRUE)
            add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
            set_target_properties(onnxruntime::onnxruntime PROPERTIES
                IMPORTED_LOCATION "${ONNXRUNTIME_LIB}"
                INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE}"
            )
        endif()
    endif()

    # Fall back to downloading prebuilt release
    if(NOT HAVE_ONNXRUNTIME)
        set(_ORT_VERSION "1.22.0")
        if(WIN32)
            set(_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ORT_VERSION}/onnxruntime-win-x64-${_ORT_VERSION}.zip")
        elseif(APPLE)
            set(_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ORT_VERSION}/onnxruntime-osx-universal2-${_ORT_VERSION}.tgz")
        else()
            set(_ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${_ORT_VERSION}/onnxruntime-linux-x64-${_ORT_VERSION}.tgz")
        endif()

        FetchContent_Declare(
            onnxruntime_prebuilt
            URL "${_ORT_URL}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(onnxruntime_prebuilt)

        # Find the shared library in the extracted directory
        if(WIN32)
            file(GLOB _ORT_DLL "${onnxruntime_prebuilt_SOURCE_DIR}/lib/onnxruntime.dll")
            file(GLOB _ORT_IMPLIB "${onnxruntime_prebuilt_SOURCE_DIR}/lib/onnxruntime.lib")
            if(_ORT_IMPLIB AND _ORT_DLL)
                add_library(onnxruntime::onnxruntime SHARED IMPORTED)
                set_target_properties(onnxruntime::onnxruntime PROPERTIES
                    IMPORTED_IMPLIB "${_ORT_IMPLIB}"
                    IMPORTED_LOCATION "${_ORT_DLL}"
                    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_prebuilt_SOURCE_DIR}/include"
                )
                set(HAVE_ONNXRUNTIME TRUE)
                set(ONNXRUNTIME_DLL "${_ORT_DLL}" CACHE INTERNAL "")
            endif()
        else()
            file(GLOB _ORT_SO "${onnxruntime_prebuilt_SOURCE_DIR}/lib/libonnxruntime${CMAKE_SHARED_LIBRARY_SUFFIX}*")
            list(GET _ORT_SO 0 _ORT_SO_FIRST)
            if(_ORT_SO_FIRST)
                add_library(onnxruntime::onnxruntime SHARED IMPORTED)
                set_target_properties(onnxruntime::onnxruntime PROPERTIES
                    IMPORTED_LOCATION "${_ORT_SO_FIRST}"
                    IMPORTED_NO_SONAME TRUE
                    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_prebuilt_SOURCE_DIR}/include"
                )
                set(HAVE_ONNXRUNTIME TRUE)
                set(ONNXRUNTIME_LIBDIR "${onnxruntime_prebuilt_SOURCE_DIR}/lib" CACHE INTERNAL "")
            endif()
        endif()
    endif()
endif()

if(HAVE_ONNXRUNTIME)
    message(STATUS "ONNX Runtime found — ML brain support enabled")
else()
    message(STATUS "ONNX Runtime not found — ML brain support disabled")
endif()

# Sentry crash reporting — sentry-native for desktop.  The ENABLE_SENTRY /
# SENTRY_DSN / HAVE_SENTRY* declarations, and the iOS sentry-cocoa download,
# live above the iOS/desktop split because the iOS target consumes them there.
if(ENABLE_SENTRY AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT EMSCRIPTEN)
    FetchContent_Declare(
        sentry
        GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
        GIT_TAG        0.14.2
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Use the inproc backend on toolchains where crashpad's vendored
    # mini_chromium / libc++ headers don't compile cleanly:
    #   - MinGW / GCC 15: poor crashpad support upstream.
    #   - Apple / Xcode 26: stricter constexpr rules + <math.h> macro
    #     collisions inside libc++ headers (isnan / isfinite) hit
    #     crashpad TUs that include <format> / <cmath>. Upstream
    #     crashpad has not been updated for these yet.
    if(MINGW OR APPLE)
        set(SENTRY_BACKEND "inproc" CACHE STRING "" FORCE)
    else()
        set(SENTRY_BACKEND "crashpad" CACHE STRING "" FORCE)
    endif()
    FetchContent_MakeAvailable(sentry)
    if(MSVC AND TARGET sentry)
        # The WinHTTP transport assigns in conditions; LTCG diagnoses C4706.
        target_compile_options(sentry PRIVATE /wd4706)
    endif()

    set(HAVE_SENTRY TRUE)
endif()

if(HAVE_SENTRY)
    message(STATUS "Sentry crash reporting enabled")
else()
    message(STATUS "Sentry crash reporting disabled")
endif()

# libplum — UPnP / NAT-PMP / PCP port mapping for hosted servers.
# Skipped on iOS (mobile) and Emscripten (wasm); the wrapper compiles to
# no-op stubs there.  Build static so we don't ship an extra shared library.
set(HAVE_LIBPLUM FALSE)
if(BOLO_PORTMAP AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT EMSCRIPTEN)
    FetchContent_Declare(
        libplum
        GIT_REPOSITORY https://github.com/paullouisageneau/libplum.git
        GIT_TAG        v0.5.3
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL
    )
    set(PLUM_NO_EXAMPLE         ON  CACHE INTERNAL "")
    set(PLUM_WARNINGS_AS_ERRORS OFF CACHE INTERNAL "")
    set(BUILD_SHARED_LIBS_SAVE_PLUM "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libplum)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVE_PLUM}" CACHE BOOL "" FORCE)
    # PIC so the static lib can also link into shared targets (winbolo_gym).
    if(TARGET plum)
        set_target_properties(plum PROPERTIES POSITION_INDEPENDENT_CODE ON)
        # libplum uses a zero-sized array (C4200) and assignments in conditions
        # (C4706, also diagnosed during LTCG). Allow these in the dependency.
        # Older Windows SDK headers also trigger C5105. Keep this suppression
        # private to plum for all MSVC generators, including Ninja where the
        # Visual Studio generator's selected-SDK variable is unavailable.
        if(MSVC)
            target_compile_options(plum PRIVATE /wd4200 /wd4706 /wd5105)
        endif()
    endif()
    set(HAVE_LIBPLUM TRUE)
    message(STATUS "libplum enabled — automatic NAT port mapping")
else()
    message(STATUS "libplum disabled — nat_portmap wrapper compiles to no-ops")
endif()

# ---------------------------------------------------------------------------
# bolo_static / server_sim_static / server_static — the bulk of the sim
# and the shared server-peer files. Added here so every desktop target
# below can link them. server_sim_static holds server_sim.c + servermessages.c;
# server_static (the dedicated-server runtime) PUBLIC-links it.
# Per-target conditional sources (transport_udp_client, nat_portmap,
# servermain, server_frontend_stubs, server_stubs) stay
# in their respective *_SOURCES lists.
# ---------------------------------------------------------------------------

add_subdirectory(src/bolo)
add_subdirectory(src/server)
add_subdirectory(src/scenario)
add_subdirectory(brains)

# server_static and server_sim_static have a circular dependency that
# GNU ld resolves with --start-group/--end-group ($<LINK_GROUP:RESCAN>).
# MSVC's link.exe does multi-pass static-lib resolution natively, and
# Apple's ld doesn't support library groups at all -- both also reject
# $<LINK_GROUP> for the CXX link language, so emit plain library names
# there. Use this variable in target_link_libraries wherever both libs
# are linked together.
if(MSVC OR APPLE)
    set(SERVER_LINK_GROUP server_static server_sim_static)
else()
    set(SERVER_LINK_GROUP "$<LINK_GROUP:RESCAN,server_static,server_sim_static>")
endif()

# ---------------------------------------------------------------------------
# winbolonet_core / winbolonet_server / winbolonet_client — three static
# libs replacing the inline winbolonet/* source lists. Binaries pick the
# subset they need via target_link_libraries.
# ---------------------------------------------------------------------------
add_library(winbolonet_core STATIC
    src/winbolonet/http.c
    src/winbolonet/winbolonetthread.c
    src/winbolonet/winbolonetevents.c
    src/winbolonet/winbolonet_core.c
    src/winbolonet/wbn_bearer.c
    src/winbolonet/wbn_country_cache.c
    src/winbolonet/wbn_prefs_sync.c
    src/winbolonet/wbn_serverlist.c
    src/winbolonet/wbn_map.c
)
target_include_directories(winbolonet_core PRIVATE
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
target_link_libraries(winbolonet_core PRIVATE SDL3::SDL3 CURL::libcurl)
bolo_apply_include_rules(winbolonet_core runtime_only)

add_library(winbolonet_server STATIC
    src/winbolonet/winbolonet_server.c
)
target_include_directories(winbolonet_server PRIVATE
    ${cjson_SOURCE_DIR}
)
target_link_libraries(winbolonet_server PRIVATE SDL3::SDL3)
target_compile_definitions(winbolonet_server PRIVATE
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)
bolo_apply_include_rules(winbolonet_server runtime_only)

add_library(winbolonet_client STATIC
    src/winbolonet/winbolonet_client.c
    src/winbolonet/wbn_comments.cpp
    src/winbolonet/wbn_news.cpp
    src/winbolonet/wbn_news_parser.cpp
)
target_include_directories(winbolonet_client PRIVATE
    ${cjson_SOURCE_DIR}
)
target_link_libraries(winbolonet_client PRIVATE SDL3::SDL3)
bolo_apply_include_rules(winbolonet_client runtime_only)

# ---------------------------------------------------------------------------
# WinBolo Client (cross-platform: Windows, Linux, macOS)
# ---------------------------------------------------------------------------

set(CLIENT_SOURCES
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/gui/ui_mode.c
    src/gui/visibility_presets.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/sdl3imgui.cpp src/gui/sdl3/imgui_steam_nav.cpp src/gui/sdl3/sdl3imgui_tablet.cpp src/gui/sdl3/luabrainshandler.c src/gui/sdl3/wb_theme.cpp
    src/gui/sdl3/cursor.c src/gui/sdl3/flags.c src/gui/sdl3/dns_lookups.c
    src/gui/sdl3/winbolo.c src/gui/sdl3/gamefront.c
    src/client_frontend/client_frontend_connect.c
    src/client_frontend/client_frontend_tick.c
    src/client_frontend/client_frontend_common.c
    src/gui/sdl3/dialog_backend.c src/gui/sdl3/dialogs/imgui_welcome.cpp src/gui/sdl3/dialogs/imgui_mapchooser.cpp src/gui/sdl3/dialogs/imgui_udpsetup.cpp src/gui/sdl3/dialogs/imgui_gamebrowser.cpp src/gui/sdl3/dialogs/imgui_setname.cpp src/gui/sdl3/dialogs/imgui_trackersetup.cpp src/gui/sdl3/dialogs/imgui_winbolonet.cpp src/gui/sdl3/dialogs/imgui_messagebox.cpp src/gui/sdl3/dialogs/imgui_keycap.cpp src/gui/sdl3/dialogs/imgui_lobby.cpp src/gui/sdl3/dialogs/lobby/lobby_assets.cpp src/gui/sdl3/dialogs/lobby/lobby_chat.cpp src/gui/sdl3/dialogs/lobby/lobby_settings.cpp src/gui/sdl3/dialogs/lobby/lobby_status.cpp src/gui/sdl3/dialogs/lobby/lobby_players.cpp src/gui/sdl3/dialogs/lobby/lobby_map_preview.cpp src/gui/sdl3/dialogs/lobby/lobby_commands.cpp src/gui/sdl3/dialogs/lobby/lobby_recap.cpp src/gui/sdl3/dialogs/lobby/lobby_rating.cpp src/gui/sdl3/dialogs/lobby/lobby_reel.cpp src/gui/sdl3/dialogs/lobby/lobby_clip_gif.cpp src/gui/sdl3/dialogs/lobby/lobby_chooser.cpp src/gui/sdl3/dialogs/lobby/lobby_scenario_chooser.cpp src/gui/sdl3/dialogs/lobby/lobby_wbn_maps.cpp src/gui/sdl3/dialogs/imgui_settings.cpp src/gui/sdl3/dialogs/imgui_keysetup.cpp src/gui/sdl3/dialogs/imgui_deck_pause.cpp src/gui/sdl3/dialogs/imgui_tutorial_overlay.cpp src/gui/sdl3/dialogs/imgui_keyboard.cpp src/gui/sdl3/dialogs/imgui_quickchat.cpp src/gui/sdl3/dialogs/imgui_controller_prompt.cpp src/gui/sdl3/dialogs/imgui_controller_disconnect.cpp src/gui/sdl3/dialogs/imgui_onboarding.cpp src/gui/sdl3/dialogs/imgui_nav_outline.cpp src/gui/sdl3/dialogs/imgui_wbn_browser.cpp src/gui/sdl3/dialogs/imgui_news.cpp src/gui/sdl3/dialogs/imgui_about.cpp src/gui/sdl3/dialogs/dialog_footer.cpp
    src/gui/sdl3/dialogs/imgui_star_rating.cpp
    src/gui/sdl3/map_preview_popup.cpp
    src/gui/sdl3/lobby_start_list.cpp
    src/gui/sdl3/map_preview_view.cpp
    src/gui/sdl3/map_preview_fetch.cpp
    src/gui/sdl3/overview_camera.cpp
    src/gui/sdl3/overview_fog.cpp
    src/gui/sdl3/overview_hud_layout.cpp
    src/gui/sdl3/overview_view.cpp
    src/gui/sdl3/map_stars.cpp
    src/gui/sdl3/wbn_map_source.cpp
    src/gui/sdl3/news_image_cache.cpp
    # Smart-ping pie menu and the markers for pings that have arrived.
    src/gui/sdl3/ping_overlay.cpp
    src/gui/sdl3/ping_icons.c
    src/gui/sdl3/ping_marker.c
    src/gui/sdl3/ring_band.c
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/bg_game.c src/gui/sdl3/mapview.c src/gui/sdl3/mapview_overlay.c src/gui/sdl3/sprite_positions.c src/gui/sdl3/tileloader.c src/gui/sdl3/skin_source.c src/gui/sdl3/skin_preview.c src/gui/sdl3/gfx_settings.c src/gui/sdl3/sdl3draw.c src/gui/sdl3/sdl3draw_status.c src/gui/sdl3/tank_label.c src/gui/sdl3/sdl_bmp.c src/gui/sdl3/input.c src/gui/sdl3/input_touch.c src/gui/sdl3/input_joystick.c src/gui/sdl3/input_gamepad.c src/gui/sdl3/glyphs.c src/gui/sdl3/build_cursor.c src/gui/sdl3/input_source.c
    # Real bodies for the spectator_input.h seam: the embedded LogViewer's
    # spectatorRun reads the controller through it for overview pan/zoom. Reads
    # the singleton gamepad input_gamepad.c owns. The standalone LogViewer links
    # spectator_input_stub.c instead.
    src/gui/sdl3/spectator_input.c
    src/gui/sdl3/tutorial_text.c
    src/gui/sdl3/lang.c src/gui/sdl3/sound.c src/gui/sdl3/sound_variants.c
    src/client_frontend/voice_core.c
    src/client_frontend/voice_client.c
    src/gui/sdl3/voice.c
    src/mapeditor/mapeditor.c src/mapeditor/mapeditor_imgui.cpp src/mapeditor/mapgen_imgui.cpp src/mapeditor/mapeditor_undo.c src/mapeditor/mapeditor_validate.c src/mapeditor/mapeditor_symmetry.c src/mapeditor/mapeditor_text.c src/mapeditor/mapeditor_fonts.c src/mapeditor/mapeditor_stats.c src/mapeditor/mapeditor_image.c src/mapeditor/mapeditor_stamp.c src/mapeditor/mapeditor_export.c
    # Embedded editor's "Open from WinBolo.net" dialog — client only, it
    # sits on the chooser + wbn_map_source stack the standalone MapEditor
    # doesn't link (see MAPEDITOR_WBN_OPEN below).
    src/mapeditor/mapeditor_wbn_open.cpp
    src/third_party/stb/stb_image_write_impl.c
    # GIF encoder for the recap's clip export. Client only — no other target
    # exports clips, and the LogViewer already omits stb_image_write_impl.c.
    src/third_party/msf_gif/msf_gif_impl.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/third_party/tweetnacl/tweetnacl.c
)


# BSD headers (compiled as .c but really just declarations)
file(GLOB BSD_SOURCES src/bsd/*.c)
list(APPEND CLIENT_SOURCES ${BSD_SOURCES})

# common/ utilities
file(GLOB COMMON_SOURCES src/common/*.c)
list(APPEND CLIENT_SOURCES ${COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER CLIENT_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

# Logviewer library sources (embedded in WinBolo, excludes standalone main.c)
set(LOGVIEWER_LIB_SOURCES
    src/logviewer/logviewer.c
    src/logviewer/lv_embed.c
    src/logviewer/screen.c
    src/logviewer/draw.c
    src/logviewer/sound.c
    src/logviewer/players.c
    # Logviewer-world spectator seed orchestration (lv_specSeedLoad /
    # lv_specRecordPump): translates captured ring keyframe/forward records
    # into the decoder. Includes spectator_replay.h, now a dep-free public
    # seam, so it builds into the client like any other decode TU.
    src/logviewer/spec_seed_load.c
    src/logviewer/blocks.c
    src/logviewer/lv_attribution.c
    src/logviewer/lv_stats.c
    src/logviewer/shells.c
    src/logviewer/snapshot.c
    src/logviewer/allience.c
    src/logviewer/pillbox.c
    src/logviewer/bases.c
    src/logviewer/bolo_map.c
    src/logviewer/starts.c
    src/logviewer/screencalc.c
    src/logviewer/screentank.c
    src/logviewer/screenlgm.c
    src/logviewer/screenbullet.c
    src/logviewer/sounddist.c
    src/logviewer/clientmutex.c
    src/logviewer/dns.c
    src/logviewer/util.c
    src/logviewer/global.c
    src/logviewer/labels.c
    src/logviewer/messages.c
    src/logviewer/game_settings_blob.c
    src/logviewer/imgui/imgui_context.cpp
    src/logviewer/imgui/imgui_main_menu.cpp
    src/logviewer/imgui/imgui_controls.cpp
    src/logviewer/imgui/imgui_game_info.cpp
    src/logviewer/imgui/imgui_events.cpp
    src/logviewer/imgui/imgui_item_info.cpp
    src/logviewer/imgui/imgui_dialogs.cpp
    src/logviewer/imgui/imgui_game_viewport.cpp
    src/logviewer/imgui/imgui_game_view.cpp
    src/logviewer/imgui/imgui_comments.cpp
    src/logviewer/imgui/imgui_logviewer_menu.cpp
    src/logviewer/platform/platform_config.c
    src/logviewer/platform/platform_dialogs.c
    # The client links the real game-view renderer (game_view.c) plus an
    # lv-accessor-bound copy of mapview.c through the winbolo_render_island
    # object library (defined after the WinBolo target), so the empty
    # game_view_stubs.c is no longer linked here.
)
list(APPEND CLIENT_SOURCES ${LOGVIEWER_LIB_SOURCES})

add_executable(WinBolo ${CLIENT_SOURCES})
# Bot-name pool JSON loader (cJSON). The core pool table + wire codec live
# in lobby_bot_pools.c inside bolo_static (no cJSON); only this file-loading
# helper, linked into the binaries that read a pool file, pulls in cJSON.
target_sources(WinBolo PRIVATE src/bolo/lobby_bot_pools_json.c)
target_compile_definitions(WinBolo PRIVATE
    HAVE_SCREEN_C=1
    # Gates the embedded map editor's File > Open from WinBolo.net entry in
    # the mapeditor TUs shared with the standalone MapEditor target.
    MAPEDITOR_WBN_OPEN=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

# macOS trackpad pinch-to-zoom support
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(WinBolo PRIVATE src/gui/sdl3/macos_pinch.m)
    target_sources(WinBolo PRIVATE src/gui/sdl3/platform/mac_menubar.mm)
    # Embedded Log Viewer reuses WinBolo's window — the LogViewer NSMenu
    # shim is linked in so gamefront.c can swap menu bars on entry/exit.
    target_sources(WinBolo PRIVATE src/logviewer/platform/mac_menubar.mm)
    # Same story for the embedded Map Editor entry from the welcome
    # screen — the editor swaps WinBolo's menu out and restores it on
    # exit via its own NSMenu shim.
    target_sources(WinBolo PRIVATE src/mapeditor/platform/mac_menubar.mm)
    target_link_libraries(WinBolo PRIVATE "-framework AppKit")
endif()

# macOS app bundle with icon
if(APPLE)
    set(WINBOLO_ICON "${CMAKE_SOURCE_DIR}/data/icons/winbolo.icns")
    if(EXISTS "${WINBOLO_ICON}")
        target_sources(WinBolo PRIVATE "${WINBOLO_ICON}")
        set_source_files_properties("${WINBOLO_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(WinBolo PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "winbolo.icns"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/gui/sdl3/platform/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.winbolo.macos"
            MACOSX_BUNDLE_BUNDLE_NAME "WinBolo")
    endif()
endif()

# Address Sanitizer — enable to diagnose heap corruption
if(MSVC AND ENABLE_ASAN)
    target_compile_options(WinBolo PRIVATE /fsanitize=address)
    target_link_options(WinBolo PRIVATE /fsanitize=address)
endif()

target_include_directories(WinBolo PRIVATE
    src/gui/sdl3
    src/gui/sdl3/dialogs
    src/gui
    src/server
    src/mapeditor
    src
    src/client_frontend
    src/logviewer
    src/logviewer/imgui
    src/logviewer/platform
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
    src/third_party/imgui_markdown
)
bolo_apply_include_rules(WinBolo gui)
# bg_game.c: dev-mode lobby-background animation that assigns bot
# teams directly because the background loop has no ClientSim per
# bot. The write goes through a privatised setter; the clean
# endpoint is moving bot team-assignment into serverSimCreateBot,
# at which point the grant deletes.
bolo_grant_internal_source_access(WinBolo
    src/bolo/transport_udp_client.c
    src/gui/sdl3/bg_game.c
    src/mapeditor/mapeditor.c
    src/mapeditor/mapeditor_export.c
    src/mapeditor/mapeditor_validate.c)

if(WIN32)
    target_sources(WinBolo PRIVATE
        src/res/winbolo.rc
    )
    set_target_properties(WinBolo PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(WinBolo PRIVATE ws2_32 winmm user32 comctl32 dbghelp)
    target_compile_options(WinBolo PRIVATE -fcommon)
    if(MSVC)
        target_link_options(WinBolo PRIVATE "/SUBSYSTEM:WINDOWS,10.0" "/MANIFEST:NO")
        # Suppress warnings in third-party sources that MSVC treats as errors
        set_source_files_properties(src/third_party/tweetnacl/tweetnacl.c
            PROPERTIES COMPILE_FLAGS "/wd4132 /wd4146 /wd4018 /wd4295")
        # Suppress C4505 (unreferenced static function) in mapgen_imgui.cpp
        set_source_files_properties(src/mapeditor/mapgen_imgui.cpp
            PROPERTIES COMPILE_FLAGS "/wd4505")
    endif()
else()
    target_link_libraries(WinBolo PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBolo PRIVATE -fcommon)
endif()


# IP-to-country geolocation (optional)
if(TARGET maxminddb)
    target_link_libraries(WinBolo PRIVATE maxminddb)
    target_compile_definitions(WinBolo PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(WinBolo PRIVATE
    SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends minizip lua_static CURL::libcurl
    nanosvg stb_image utf8proc cjson)

# Voice capture/playback (src/gui/sdl3/voice.c + src/client_frontend/voice_core.c).
# WINBOLO_VOICE gates the settings-dialog controls, which also compile into
# targets that do not build the voice sources.
target_link_libraries(WinBolo PRIVATE Opus::opus)
target_compile_definitions(WinBolo PRIVATE WINBOLO_VOICE=1)

# Acoustic echo cancellation of remote voice out of the microphone
# (src/client_frontend/voice_aec.c).  This target only: the browser build uses
# getUserMedia's own canceller and never sees SpeexDSP, and no other target
# compiles the voice sources at all.
target_sources(WinBolo PRIVATE src/client_frontend/voice_aec.c)
target_link_libraries(WinBolo PRIVATE speexdsp)
target_compile_definitions(WinBolo PRIVATE WINBOLO_VOICE_AEC=1)

# The capture-chain recorder (WB_VOICEDEBUG above). This target only: it is
# called from voice_client.c and winbolo.c, which nothing else compiles.
if(WB_VOICEDEBUG)
    target_sources(WinBolo PRIVATE src/client_frontend/voice_debug.c)
endif()

# libplum NAT port mapping (optional)
if(HAVE_LIBPLUM)
    target_compile_definitions(WinBolo PRIVATE BOLO_PORTMAP)
    target_link_libraries(WinBolo PRIVATE LibPlum::LibPlum)
endif()

# bolo_static contains ml_brain.c unconditionally; when HAVE_ONNXRUNTIME
# is set the lib was compiled with the real path and PUBLIC-links
# onnxruntime, so consumers inherit the link without repeating it.
target_link_libraries(WinBolo PRIVATE
    bolo_static
    # Ahead of the server group: scenario_static calls the funnel and the
    # round-start registration, which the group provides.
    scenario_static
    ${SERVER_LINK_GROUP}
    bot_brains_static
    winbolonet_core
    winbolonet_server
    winbolonet_client)

# Sentry crash reporting (always compile — stubs when HAVE_SENTRY is off)
target_sources(WinBolo PRIVATE src/common/sentry_integration.c)
if(HAVE_SENTRY)
    target_compile_definitions(WinBolo PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBolo PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBolo PRIVATE sentry::sentry)
endif()

# Steamworks SDK integration (optional)
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/steamworks")
if(EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
    if(APPLE)
        set(STEAM_RUNTIME_LIB "${STEAMWORKS_SDK_DIR}/redistributable_bin/osx/libsteam_api.dylib")
    elseif(WIN32)
        set(STEAM_RUNTIME_LIB "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll")
    else()
        set(STEAM_RUNTIME_LIB "${STEAMWORKS_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so")
    endif()
    add_library(steam_api SHARED IMPORTED)
    set_target_properties(steam_api PROPERTIES
        IMPORTED_LOCATION "${STEAM_RUNTIME_LIB}"
        IMPORTED_IMPLIB "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${STEAMWORKS_SDK_DIR}/public"
    )
    target_sources(WinBolo PRIVATE src/steam/steam_wrapper.cpp)
    target_compile_definitions(WinBolo PRIVATE HAVE_STEAM=1)
    target_link_libraries(WinBolo PRIVATE steam_api)
    message(STATUS "Steamworks SDK found — Steam integration enabled")
else()
    target_sources(WinBolo PRIVATE src/steam/steam_wrapper_stub.c)
    message(STATUS "Steamworks SDK not found — Steam integration disabled (stubs)")
endif()

# ---------------------------------------------------------------------------
# Render island — link the real log-viewer game-view renderer into the client.
#
# game_view.c draws the log viewer's reconstructed world, whose
# screen/screenMines/screenTanks buffers use a pointer layout that differs
# from the client's inline-array bolo layout. mapViewDraw* read those buffers
# ONLY through the screen* accessors, so a second copy of mapview.c whose
# accessors are rebound (at build time) to the lv_* bodies renders lv-layout
# data correctly. The remap is target-scoped, so the client's own bolo-bound
# mapview.o and the standalone LogViewer build are untouched.
#
#   * mapViewDraw{Tiles,Shells,Tanks,LGMs} and the file-scope
#     mapViewCalcSquare/mapViewRenderCentered are renamed lvg_* so the island
#     copies don't clash with the client's live-game mapview.o; game_view.c's
#     calls are renamed to match.
#   * The eleven screen* accessors mapView* references rebind to the existing
#     lv_* bodies (already linked via LOGVIEWER_LIB_SOURCES — no new symbols).
#     screenTanksGetItem has a different parameter list on the two sides, so it
#     maps to lvg_screenTanksGetItem (render_island_screentank_adapter.c)
#     rather than a plain macro. The three GetSubPixel accessors take the same
#     parameters on both sides, so they map straight to their lv_ bodies.
#     Every accessor the Draw* path calls must appear below: an unmapped one
#     still links, against bolo's body, and reads an lv-layout struct through
#     the wrong offsets.
#
# game-view stays gated off by ownsWindow (draw.c), so this renderer is
# linked-but-dormant.
# ---------------------------------------------------------------------------
add_library(winbolo_render_island OBJECT
    src/gui/sdl3/mapview.c
    src/logviewer/game_view.c
    src/logviewer/render_island_screentank_adapter.c
)
# PIC so the objects link into the client regardless of its PIE setting
# (no effect on MSVC).
set_target_properties(winbolo_render_island PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Compile these sources against the same headers and definitions the client
# uses for them (mirrors WinBolo's include dirs + compile defs at generate
# time, including the bolo public-header access granted by gui rules).
target_include_directories(winbolo_render_island PRIVATE
    $<TARGET_PROPERTY:WinBolo,INCLUDE_DIRECTORIES>)
target_compile_definitions(winbolo_render_island PRIVATE
    $<TARGET_PROPERTY:WinBolo,COMPILE_DEFINITIONS>)
bolo_apply_include_rules(winbolo_render_island gui)
# SDL3 / SDL3_ttf headers reached by game_view.c.
target_link_libraries(winbolo_render_island PRIVATE
    SDL3::SDL3 SDL3_ttf::SDL3_ttf)
# Build-time symbol remap that makes this the lv-bound island (see comment).
target_compile_definitions(winbolo_render_island PRIVATE
    mapViewDrawTiles=lvg_mapViewDrawTiles
    mapViewDrawShells=lvg_mapViewDrawShells
    mapViewDrawTanks=lvg_mapViewDrawTanks
    mapViewDrawLGMs=lvg_mapViewDrawLGMs
    mapViewCalcSquare=lvg_mapViewCalcSquare
    mapViewRenderCentered=lvg_mapViewRenderCentered
    screenGetPos=lv_screenGetPos
    screenIsMine=lv_screenIsMine
    screenBulletsGetNumEntries=lv_screenBulletsGetNumEntries
    screenBulletsGetItem=lv_screenBulletsGetItem
    screenBulletsGetSubPixel=lv_screenBulletsGetSubPixel
    screenTanksGetNumEntries=lv_screenTanksGetNumEntries
    screenLgmGetNumEntries=lv_screenLgmGetNumEntries
    screenLgmGetItem=lv_screenLgmGetItem
    screenLgmGetSubPixel=lv_screenLgmGetSubPixel
    screenTanksGetItem=lvg_screenTanksGetItem
    screenTanksGetSubPixel=lv_screenTanksGetSubPixel
)
target_link_libraries(WinBolo PRIVATE winbolo_render_island)

# Copy data assets and brain scripts to the correct location.
# On macOS bundles, SDL_GetBasePath() returns Contents/Resources/, so assets go there.
# On Windows/Linux, they go next to the executable.
if(APPLE)
    set(WINBOLO_DATA_DIR "$<TARGET_FILE_DIR:WinBolo>/../Resources")
else()
    set(WINBOLO_DATA_DIR "$<TARGET_FILE_DIR:WinBolo>")
endif()

add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data" "${WINBOLO_DATA_DIR}/data"
    COMMENT "Copying WinBolo data files to output directory"
)

# Ship the top-level docs (Third Party Notices, Authors) inside data/
# so the About box can find them via SDL_GetBasePath()/data/.
add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        "${WINBOLO_DATA_DIR}/data/THIRD_PARTY_NOTICES.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/AUTHORS.md"
        "${WINBOLO_DATA_DIR}/data/AUTHORS.md"
    COMMENT "Copying About-box markdown docs to data/"
)

add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${WINBOLO_DATA_DIR}/Brains/GoalHunter_1.7"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}/brains/GoalHunter_1.7
        -DDEST_DIR=${WINBOLO_DATA_DIR}/Brains/GoalHunter_1.7
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_brains.cmake
    COMMENT "Copying brain scripts (GoalHunter_1.7) to output Brains/ directory (excludes *.md)"
)

# Steam Input action manifest + per-controller binding files — Steam reads
# them from <install dir>/controller_config/ at runtime. Copy the whole
# directory so the per-controller .vdf files ship too (matches the dist
# target). Production also uploads to Steamworks; this local copy is the
# dev path.
add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/controller_config"
        "${WINBOLO_DATA_DIR}/controller_config"
    COMMENT "Copying Steam Input controller_config/ to output directory"
)

# Tutorial glyph atlases (Xelu CC0 packs).  Loaded on demand by
# glyphs.c relative to data/controller/<set>/.  Without this copy the
# tutorial dialogs would render every action token as a procedural
# keycap fallback.
add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/controller"
        "${WINBOLO_DATA_DIR}/data/controller"
    COMMENT "Copying controller glyph atlases to data/controller/"
)

# Copy shared libraries next to WinBolo executable (all platforms).
# FetchContent-built libs (SDL3, SDL3_ttf) are not picked up by TARGET_RUNTIME_DLLS.
add_custom_command(TARGET WinBolo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>"         "$<TARGET_FILE_DIR:WinBolo>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>" "$<TARGET_FILE_DIR:WinBolo>"
    COMMENT "Copying SDL3 shared libraries to WinBolo output directory"
)
# Copy ONNX Runtime shared library if using FetchContent prebuilt
if(HAVE_ONNXRUNTIME)
    if(WIN32 AND DEFINED ONNXRUNTIME_DLL)
        add_custom_command(TARGET WinBolo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:WinBolo>"
            COMMENT "Copying ONNX Runtime DLL to WinBolo output directory"
        )
    elseif(DEFINED ONNXRUNTIME_LIBDIR)
        file(GLOB _ORT_ALL_SO "${ONNXRUNTIME_LIBDIR}/libonnxruntime*")
        foreach(_f ${_ORT_ALL_SO})
            # Skip .dSYM bundles — they are directories, not files, so
            # copy_if_different fails on them. They are debug symbols that
            # don't need to ship next to the binary.
            if(_f MATCHES "\\.dSYM$")
                continue()
            endif()
            add_custom_command(TARGET WinBolo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:WinBolo>"
            )
        endforeach()
    endif()
endif()
# Copy crashpad_handler next to WinBolo executable
if(HAVE_SENTRY AND TARGET crashpad_handler)
    add_custom_command(TARGET WinBolo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>" "$<TARGET_FILE_DIR:WinBolo>"
        COMMENT "Copying crashpad_handler to WinBolo output directory"
    )
endif()
# Copy Steam runtime library and steam_appid.txt next to executable
if(EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
    if(WIN32)
        add_custom_command(TARGET WinBolo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll"
                "$<TARGET_FILE_DIR:WinBolo>"
            COMMENT "Copying steam_api64.dll to WinBolo output directory"
        )
    elseif(APPLE)
        add_custom_command(TARGET WinBolo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMWORKS_SDK_DIR}/redistributable_bin/osx/libsteam_api.dylib"
                "$<TARGET_FILE_DIR:WinBolo>"
            COMMENT "Copying libsteam_api.dylib to WinBolo bundle"
        )
    else()
        add_custom_command(TARGET WinBolo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${STEAMWORKS_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so"
                "$<TARGET_FILE_DIR:WinBolo>"
            COMMENT "Copying libsteam_api.so to WinBolo output directory"
        )
    endif()
    # On macOS, $<TARGET_FILE_DIR:WinBolo> is WinBolo.app/Contents/MacOS, so
    # copying steam_appid.txt there embeds it inside the signed bundle — it then
    # has to be stripped before signing/notarizing. Skip it on Apple; Steam
    # supplies the App ID via env when it launches the game. For local Mac dev
    # testing, drop a steam_appid.txt into the cwd by hand.
    if(NOT APPLE)
        add_custom_command(TARGET WinBolo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/steam_appid.txt"
                "$<TARGET_FILE_DIR:WinBolo>"
            COMMENT "Copying steam_appid.txt to WinBolo output directory"
        )
    endif()
endif()
# On Windows, also copy vcpkg / imported DLLs (curl, zlib, etc.)
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET WinBolo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:WinBolo> "$<TARGET_FILE_DIR:WinBolo>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs to WinBolo output directory"
    )
endif()
# MinGW: copy runtime DLLs (libwinpthread, libgcc, libstdc++) next to executables
if(MINGW)
    get_filename_component(_MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    foreach(_dll libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll)
        if(EXISTS "${_MINGW_BIN_DIR}/${_dll}")
            add_custom_command(TARGET WinBolo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_MINGW_BIN_DIR}/${_dll}" "$<TARGET_FILE_DIR:WinBolo>"
                COMMENT "Copying ${_dll} to WinBolo output directory"
            )
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# WinBoloDS Server (cross-platform: Windows, Linux, macOS)
# ---------------------------------------------------------------------------

set(SERVER_SOURCES
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/server/servermain.c
    src/server/server_frontend_stubs.c
    src/gui/sdl3/luabrainshandler.c
    src/headless/cmd_stdin.c
    src/third_party/tweetnacl/tweetnacl.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
)


add_executable(WinBoloDS ${SERVER_SOURCES})
target_sources(WinBoloDS PRIVATE src/steam/steam_wrapper_stub.c
    src/bolo/lobby_bot_pools_json.c)
target_compile_definitions(WinBoloDS PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(WinBoloDS PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
bolo_apply_include_rules(WinBoloDS runtime_only)
bolo_grant_internal_source_access(WinBoloDS
    src/bolo/transport_udp_client.c
    src/server/servermain.c
    src/server/server_frontend_stubs.c)

if(WIN32)
    target_sources(WinBoloDS PRIVATE
        src/gui/sdl3/lang.c
        src/res/winbolods.rc
    )
    target_link_libraries(WinBoloDS PRIVATE ws2_32 winmm)
    target_compile_options(WinBoloDS PRIVATE -fcommon)
    if(MSVC)
        target_link_options(WinBoloDS PRIVATE "/SUBSYSTEM:CONSOLE,10.0")
    endif()
else()
    target_sources(WinBoloDS PRIVATE
        src/gui/sdl3/lang.c
    )
    target_link_libraries(WinBoloDS PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBoloDS PRIVATE -fcommon)
endif()

if(TARGET maxminddb)
    target_link_libraries(WinBoloDS PRIVATE maxminddb)
    target_compile_definitions(WinBoloDS PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(WinBoloDS PRIVATE
    bolo_static
    # Ahead of the server group: scenario_static calls the funnel and the
    # round-start registration, which the group provides.
    scenario_static
    ${SERVER_LINK_GROUP}
    bot_brains_static
    winbolonet_core
    winbolonet_server
    winbolonet_client)
target_link_libraries(WinBoloDS PRIVATE minizip SDL3::SDL3 CURL::libcurl lua_static cjson utf8proc)

# libplum NAT port mapping (optional). nat_portmap.c stays per-target
# so its #ifdef BOLO_PORTMAP block compiles against this binary's view.
if(HAVE_LIBPLUM)
    target_compile_definitions(WinBoloDS PRIVATE BOLO_PORTMAP)
    target_link_libraries(WinBoloDS PRIVATE LibPlum::LibPlum)
endif()

# Sentry crash reporting (always compile — stubs when HAVE_SENTRY is off)
target_sources(WinBoloDS PRIVATE src/common/sentry_integration.c)
target_sources(WinBoloDS PRIVATE src/common/wb_log.c)
target_sources(WinBoloDS PRIVATE src/common/md5.c)
target_sources(WinBoloDS PRIVATE src/common/mp_diag_log.c)
target_sources(WinBoloDS PRIVATE src/common/prefs_doc.c)
target_sources(WinBoloDS PRIVATE src/common/prefs.c)
if(HAVE_SENTRY)
    target_compile_definitions(WinBoloDS PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBoloDS PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBoloDS PRIVATE sentry::sentry)
endif()

# Copy geo database to server output directory
if(EXISTS "${CMAKE_SOURCE_DIR}/data/dbip-country-lite.mmdb")
    add_custom_command(TARGET WinBoloDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WinBoloDS>/data"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/data/dbip-country-lite.mmdb"
            "$<TARGET_FILE_DIR:WinBoloDS>/data/dbip-country-lite.mmdb"
        COMMENT "Copying geo database to server output directory"
    )
endif()

# Copy shared libraries next to WinBoloDS (all platforms)
add_custom_command(TARGET WinBoloDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:WinBoloDS>"
    COMMENT "Copying SDL3 shared library to WinBoloDS output directory"
)

# bolo_static PUBLIC-links onnxruntime, so WinBoloDS inherits the dylib
# dependency. Mirror the WinBolo/MapEditor POST_BUILD copy step so the
# dedicated server finds the dylib next to its executable — without this,
# launching WinBoloDS fails at dyld load time with
# "Library not loaded: @rpath/libonnxruntime.*.dylib".
if(HAVE_ONNXRUNTIME)
    if(WIN32 AND DEFINED ONNXRUNTIME_DLL)
        add_custom_command(TARGET WinBoloDS POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:WinBoloDS>"
            COMMENT "Copying ONNX Runtime DLL to WinBoloDS output directory"
        )
    elseif(DEFINED ONNXRUNTIME_LIBDIR)
        file(GLOB _ORT_ALL_SO "${ONNXRUNTIME_LIBDIR}/libonnxruntime*")
        foreach(_f ${_ORT_ALL_SO})
            # Skip .dSYM bundles — they are directories, not files, so
            # copy_if_different fails on them. They are debug symbols that
            # don't need to ship next to the binary.
            if(_f MATCHES "\\.dSYM$")
                continue()
            endif()
            add_custom_command(TARGET WinBoloDS POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:WinBoloDS>"
            )
        endforeach()
    endif()
endif()

add_custom_command(TARGET WinBoloDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/lang" "$<TARGET_FILE_DIR:WinBoloDS>/data/lang"
    COMMENT "Copying WinBoloDS lang files to output directory"
)
# Default bot naming pools so a -bots server names its bots from the
# shipped themes without needing -botnames. (-botnames overrides this.)
add_custom_command(TARGET WinBoloDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/data/bot_names.json"
        "$<TARGET_FILE_DIR:WinBoloDS>/data/bot_names.json"
    COMMENT "Copying WinBoloDS default bot_names.json to output directory"
)
if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET WinBoloDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:WinBoloDS> "$<TARGET_FILE_DIR:WinBoloDS>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs to WinBoloDS output directory"
    )
endif()

# Copy crashpad_handler next to WinBoloDS executable
if(HAVE_SENTRY AND TARGET crashpad_handler)
    add_custom_command(TARGET WinBoloDS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>" "$<TARGET_FILE_DIR:WinBoloDS>"
        COMMENT "Copying crashpad_handler to WinBoloDS output directory"
    )
endif()
# Copy Lua brain scripts to output Brains/ directory (server)
add_custom_command(TARGET WinBoloDS POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WinBoloDS>/Brains"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/brains/GoalHunter_1.7"
        "$<TARGET_FILE_DIR:WinBoloDS>/Brains/GoalHunter_1.7"
    COMMENT "Copying brain scripts to WinBoloDS output Brains/ directory"
)

# ---------------------------------------------------------------------------
# LogViewer (cross-platform: Windows, Linux, macOS)
# ---------------------------------------------------------------------------

file(GLOB LOGVIEWER_SOURCES src/logviewer/*.c src/common/*.c)
if(NOT WIN32)
    list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()
# bolo_shim.c and game_view.c are added explicitly via LOGVIEWER_IMGUI_SOURCES
# (Phases B and D). They include bolo headers directly and would conflict
# with logviewer/backend.h if pulled into a TU that also sees logviewer types.
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/bolo_shim\\.c$")
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/game_view\\.c$")
# game_view_stubs.c is the embedded-only stub set; standalone uses the
# real game_view.c via LOGVIEWER_IMGUI_SOURCES — drop the stubs from the
# glob so the standalone link doesn't see duplicate symbols.
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/game_view_stubs\\.c$")
# render_island_screentank_adapter.c is a client-only shim: it bridges the
# game-view render island's screenTanksGetItem to lv_screenTanksGetItem.
# The standalone uses bolo_shim.c for that, so drop the adapter from the glob.
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/render_island_screentank_adapter\\.c$")
# spectator_drain_stub.c is added explicitly to the LogViewer target below;
# drop it from the glob so it is listed once. It stubs the spectator_drain.h
# seam for the standalone (which carries spectatorRun but links no bolo sim).
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/spectator_drain_stub\\.c$")
# spectator_input_stub.c is added explicitly to the LogViewer target below;
# drop it from the glob so it is listed once. It stubs the spectator_input.h
# seam for the standalone (which carries spectatorRun but links no gui-tier
# gamepad module); the WinBolo client links the real src/gui/sdl3/spectator_input.c.
list(FILTER LOGVIEWER_SOURCES EXCLUDE REGEX ".*logviewer/spectator_input_stub\\.c$")

set(LOGVIEWER_IMGUI_SOURCES
    src/logviewer/imgui/imgui_context.cpp
    src/logviewer/imgui/imgui_main_menu.cpp
    src/logviewer/imgui/imgui_controls.cpp
    src/logviewer/imgui/imgui_game_info.cpp
    src/logviewer/imgui/imgui_events.cpp
    src/logviewer/imgui/imgui_item_info.cpp
    src/logviewer/imgui/imgui_dialogs.cpp
    src/logviewer/imgui/imgui_game_viewport.cpp
    src/logviewer/imgui/imgui_game_view.cpp
    src/logviewer/imgui/imgui_comments.cpp
    src/logviewer/imgui/imgui_logviewer_menu.cpp
    src/gui/sdl3/dialogs/imgui_nav_outline.cpp
    src/gui/sdl3/mapview.c
    src/gui/sdl3/sdl3draw_status.c
    src/gui/sdl3/tank_label.c
    src/logviewer/bolo_shim.c
    src/logviewer/game_view.c
)

# Reuse the SDL3 main-game WBN browser dialog. LogViewer provides stubs for
# the few main-game-only symbols it pulls in (bg_game, gamefront token,
# imgui_winbolonet sign-in section). The dialog is *not* added to
# LOGVIEWER_LIB_SOURCES because the WinBolo client already links its own copy.
set(LOGVIEWER_WBN_BROWSER_SOURCES
    src/gui/sdl3/dialogs/imgui_wbn_browser.cpp
    src/gui/sdl3/dialogs/imgui_star_rating.cpp
    src/gui/sdl3/dialogs/dialog_footer.cpp
    src/gui/sdl3/dialogs/imgui_nav_outline.cpp
    src/gui/sdl3/dialogs/imgui_keyboard.cpp
    src/logviewer/imgui/imgui_lv_browser_stubs.cpp
)

set(LOGVIEWER_PLATFORM_SOURCES
    src/logviewer/platform/platform_config.c
    src/logviewer/platform/platform_dialogs.c
)

# WinBolo.net HTTP client + Ed25519 signing — shared with the SDL3 client and
# servers. LogViewer uses it to fetch/post comments on logs that have a WBN key.
set(LOGVIEWER_WBN_SOURCES
    src/third_party/tweetnacl/tweetnacl.c
)

add_executable(LogViewer
    ${LOGVIEWER_SOURCES}
    ${LOGVIEWER_IMGUI_SOURCES}
    ${LOGVIEWER_PLATFORM_SOURCES}
    ${LOGVIEWER_WBN_SOURCES}
    ${LOGVIEWER_WBN_BROWSER_SOURCES}
    # Spectator replay translator: spec_seed_load.c calls specReplay* but the
    # standalone links no bolo sim, so compile the translator in directly. It
    # pulls only public bolo headers (spectator_replay.h / log.h / platform_net.h)
    # and no backend.h-conflicting type, so it builds under the runtime_only rules.
    src/bolo/spectator_replay.c
    # Shared round-stats derivation: roundStatsApplyRecord / computeAwards
    # rebuild a round's stats, awards, and notable timeline from the parsed
    # attribution track. Pure C over the public record/stats types (only
    # round_stats.h / attribution_track.h), so it compiles in directly like
    # spectator_replay.c — the standalone links no bolo sim.
    src/bolo/round_stats_derive.c
    # No-op stubs for the spectator_drain.h seam: logviewer.c carries
    # spectatorRun, but the standalone links no bolo sim (the real wrappers
    # live in client_sim.c / bolo_static for the WinBolo client) and never
    # spectates, so these resolve the seam symbols here.
    src/logviewer/spectator_drain_stub.c
    # No-op stubs for the spectator_input.h seam: the standalone links no
    # gui-tier gamepad module (the real bodies live in
    # src/gui/sdl3/spectator_input.c for the WinBolo client) and never
    # spectates, so these resolve the seam symbols here.
    src/logviewer/spectator_input_stub.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/gamefront_stubs.c
    # The replay draws the same smart-ping glyphs the game does, out of the
    # same loose files.
    src/gui/sdl3/ping_icons.c
    src/gui/sdl3/ping_marker.c
    src/gui/sdl3/ring_band.c
    src/gui/sdl3/sdl_bmp.c
    src/gui/sdl3/sprite_positions.c
    src/gui/sdl3/tileloader.c
    src/gui/sdl3/skin_source.c
    src/gui/sdl3/gfx_settings.c
    src/gui/ui_mode.c
    src/steam/steam_wrapper_stub.c
)
target_compile_definitions(LogViewer PRIVATE
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
    PREFERENCE_FILE="WinBolo.ini"
)

if(WIN32)
    target_sources(LogViewer PRIVATE src/logviewer/script1.rc)
endif()

set_target_properties(LogViewer PROPERTIES OUTPUT_NAME "Log Viewer")

if(WIN32)
    set_target_properties(LogViewer PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(LogViewer PRIVATE "/SUBSYSTEM:WINDOWS,10.0" "/MANIFEST:NO")
    endif()
endif()

if(APPLE)
    set_target_properties(LogViewer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/logviewer/platform/Info.plist"
    )
    set(APP_ICON_ICNS "${CMAKE_SOURCE_DIR}/data/icons/logviewer.icns")
    if(EXISTS "${APP_ICON_ICNS}")
        target_sources(LogViewer PRIVATE "${APP_ICON_ICNS}")
        set_source_files_properties("${APP_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(LogViewer PROPERTIES
            MACOSX_BUNDLE_ICON_FILE "logviewer.icns")
    endif()
endif()

target_include_directories(LogViewer PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/logviewer
    ${CMAKE_SOURCE_DIR}/src/logviewer/imgui
    ${CMAKE_SOURCE_DIR}/src/logviewer/platform
    ${CMAKE_SOURCE_DIR}/src/winbolonet
    ${CMAKE_SOURCE_DIR}/src/third_party/tweetnacl
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(LogViewer runtime_only)

target_link_libraries(LogViewer PRIVATE
    winbolonet_core
    winbolonet_client
    minizip SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends
    nanosvg stb_image CURL::libcurl cjson
)

if(WIN32)
    target_link_libraries(LogViewer PRIVATE ws2_32)
endif()
if(MATH_LIB)
    target_link_libraries(LogViewer PRIVATE ${MATH_LIB})
endif()

# macOS trackpad pinch-to-zoom support, plus native NSMenu shim for the
# standalone Log Viewer.app menu bar.
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(LogViewer PRIVATE src/gui/sdl3/macos_pinch.m)
    target_sources(LogViewer PRIVATE src/logviewer/platform/mac_menubar.mm)
    target_link_libraries(LogViewer PRIVATE "-framework AppKit")
endif()

# Copy shared libraries next to LogViewer (all platforms)
add_custom_command(TARGET LogViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:LogViewer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>" "$<TARGET_FILE_DIR:LogViewer>"
    COMMENT "Copying SDL3 shared libraries to LogViewer output directory"
)

if(APPLE)
    add_custom_command(TARGET LogViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_BUNDLE_CONTENT_DIR:LogViewer>/Resources/data"
        COMMENT "Copying data files into app bundle Resources"
    )
else()
    add_custom_command(TARGET LogViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:LogViewer>/data"
        COMMENT "Copying LogViewer data files to output directory"
    )
endif()

# ---------------------------------------------------------------------------
# WinBoloHeadless (headless client for automated testing)
# ---------------------------------------------------------------------------

set(HEADLESS_SOURCES
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/dns_lookups.c
    src/headless/headless_main.c src/headless/headless_frontend.c
    src/headless/cmd_stdin.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/third_party/tweetnacl/tweetnacl.c
)


# BSD headers
file(GLOB HEADLESS_BSD_SOURCES src/bsd/*.c)
list(APPEND HEADLESS_SOURCES ${HEADLESS_BSD_SOURCES})

# common/ utilities
file(GLOB HEADLESS_COMMON_SOURCES src/common/*.c)
list(APPEND HEADLESS_SOURCES ${HEADLESS_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER HEADLESS_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_executable(WinBoloHeadless ${HEADLESS_SOURCES})
target_sources(WinBoloHeadless PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(WinBoloHeadless PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(WinBoloHeadless PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/headless
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
bolo_apply_include_rules(WinBoloHeadless runtime_only)
bolo_grant_internal_source_access(WinBoloHeadless
    src/bolo/transport_udp_client.c)

if(WIN32)
    target_link_libraries(WinBoloHeadless PRIVATE ws2_32 winmm user32)
    target_compile_options(WinBoloHeadless PRIVATE -fcommon)
else()
    target_link_libraries(WinBoloHeadless PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBoloHeadless PRIVATE -fcommon)
endif()

if(TARGET maxminddb)
    target_link_libraries(WinBoloHeadless PRIVATE maxminddb)
    target_compile_definitions(WinBoloHeadless PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(WinBoloHeadless PRIVATE
    bolo_static
    scenario_static
    ${SERVER_LINK_GROUP}
    bot_brains_static
    winbolonet_core
    winbolonet_server
    winbolonet_client)
target_link_libraries(WinBoloHeadless PRIVATE
    SDL3::SDL3 minizip lua_static CURL::libcurl cjson utf8proc)

# Sentry crash reporting (always compile — stubs when HAVE_SENTRY is off)
target_sources(WinBoloHeadless PRIVATE src/common/sentry_integration.c)
if(HAVE_SENTRY)
    target_compile_definitions(WinBoloHeadless PRIVATE HAVE_SENTRY=1)
    if(SENTRY_DSN)
        target_compile_definitions(WinBoloHeadless PRIVATE SENTRY_DSN="${SENTRY_DSN}")
    endif()
    target_link_libraries(WinBoloHeadless PRIVATE sentry::sentry)
endif()

# Copy shared libraries and brain scripts next to headless executable
add_custom_command(TARGET WinBoloHeadless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:WinBoloHeadless>"
    COMMENT "Copying SDL3 shared library to WinBoloHeadless output directory"
)
add_custom_command(TARGET WinBoloHeadless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WinBoloHeadless>/Brains"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/brains/GoalHunter_1.7"
        "$<TARGET_FILE_DIR:WinBoloHeadless>/Brains/GoalHunter_1.7"
    COMMENT "Copying brain scripts to WinBoloHeadless output directory"
)
add_custom_command(TARGET WinBoloHeadless POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/lang" "$<TARGET_FILE_DIR:WinBoloHeadless>/data/lang"
    COMMENT "Copying WinBoloHeadless lang files to output directory"
)
# Copy crashpad_handler next to WinBoloHeadless executable
if(HAVE_SENTRY AND TARGET crashpad_handler)
    add_custom_command(TARGET WinBoloHeadless POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>" "$<TARGET_FILE_DIR:WinBoloHeadless>"
        COMMENT "Copying crashpad_handler to WinBoloHeadless output directory"
    )
endif()

# ---------------------------------------------------------------------------
# WinBoloGym shared library (C API for ML training)
# ---------------------------------------------------------------------------

set(GYM_SOURCES
    src/bolo/transport_udp_client.c
    src/bolo/nat_portmap.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/dns_lookups.c
    src/headless/headless_frontend.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/gym/winbolonet_stub.c
    src/gym/winbolo_gym.c
)


# BSD headers
file(GLOB GYM_BSD_SOURCES src/bsd/*.c)
list(APPEND GYM_SOURCES ${GYM_BSD_SOURCES})

# common/ utilities
file(GLOB GYM_COMMON_SOURCES src/common/*.c)
list(APPEND GYM_SOURCES ${GYM_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER GYM_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_library(winbolo_gym SHARED ${GYM_SOURCES})
target_sources(winbolo_gym PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(winbolo_gym PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_GYM_EXPORTS=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(winbolo_gym PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/headless
    src/mapeditor
    ${cjson_SOURCE_DIR}
    src/third_party/tweetnacl
)
bolo_apply_include_rules(winbolo_gym gym)

if(WIN32)
    target_link_libraries(winbolo_gym PRIVATE ws2_32 winmm user32)
    target_compile_options(winbolo_gym PRIVATE -fcommon)
else()
    target_link_libraries(winbolo_gym PRIVATE ${MATH_LIB} pthread)
    target_compile_options(winbolo_gym PRIVATE -fcommon)
endif()

if(TARGET maxminddb)
    target_link_libraries(winbolo_gym PRIVATE maxminddb)
    target_compile_definitions(winbolo_gym PRIVATE HAVE_MAXMINDDB=1)
endif()

target_link_libraries(winbolo_gym PRIVATE
    bolo_static
    ${SERVER_LINK_GROUP}
    bot_brains_static)
target_link_libraries(winbolo_gym PRIVATE
    SDL3::SDL3 minizip lua_static utf8proc cjson)

if(OpenMP_C_FOUND)
    target_link_libraries(winbolo_gym PRIVATE OpenMP::OpenMP_C)
endif()

set_target_properties(winbolo_gym PROPERTIES
    PUBLIC_HEADER src/gym/winbolo_gym.h
)

# Copy SDL3 next to the gym library
add_custom_command(TARGET winbolo_gym POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:winbolo_gym>"
    COMMENT "Copying SDL3 shared library to winbolo_gym output directory"
)
add_custom_command(TARGET winbolo_gym POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/lang" "$<TARGET_FILE_DIR:winbolo_gym>/data/lang"
    COMMENT "Copying winbolo_gym lang files to output directory"
)

# ---------------------------------------------------------------------------
# gym_batch_test (Phase 0 determinism baseline)
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# lua_strip — generic tool: strips named call-statement patterns from Lua
# source files and syntax-checks the result via luaL_loadfile.
# Brains use this to produce a debug-free opt/ copy; see each brain's
# strip.bat / strip.sh for the per-bot invocation.
# ---------------------------------------------------------------------------
add_executable(lua_strip src/tools/lua_strip.c)
target_link_libraries(lua_strip PRIVATE lua_static)
if(WIN32)
    target_link_libraries(lua_strip PRIVATE winmm)
endif()

# ---------------------------------------------------------------------------
# brainrec_split — cuts one -braindebug session into smaller sessions that
# BrainTest can load one at a time. A long full-pool recording is gigabytes
# on disk, and btLoadSession materialises every frame, so loading a whole
# game can cost more RAM than the machine has; halving the frames halves it.
# Links brain_record_walk.c (the shared brainrec.btr frame walker) rather
# than bolo_static, so the tool doesn't drag in the sim, Lua or SDL's
# runtime — SDL3 is here for its filesystem calls and for the headers the
# format structs live behind.
# ---------------------------------------------------------------------------
add_executable(brainrec_split
    src/tools/brainrec_split.c
    src/bolo/brain_record_walk.c)
bolo_apply_include_rules(brainrec_split runtime_only)
target_link_libraries(brainrec_split PRIVATE SDL3::SDL3 zlibstatic)
if(WIN32)
    target_link_libraries(brainrec_split PRIVATE psapi)
endif()

# ---------------------------------------------------------------------------
# BrainTest (standalone brain debug viewer with overlays)
# ---------------------------------------------------------------------------

set(BRAINTEST_SOURCES
    src/bolo/transport_udp_client.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/mapview.c src/gui/sdl3/sprite_positions.c src/gui/sdl3/tileloader.c src/gui/sdl3/skin_source.c src/gui/sdl3/gfx_settings.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/server_stubs.c
    src/braintest/braintest_lifecycle_stub.c
    src/braintest/braintest_main.c src/braintest/braintest_frontend.c
    src/braintest/braintest_viz_registry.c
    src/braintest/braintest_vizwindow.cpp
    src/braintest/braintest_loadbrowser.cpp
    src/braintest/braintest_loadbrowser_segment.c
    src/braintest/braintest_loadbrowser_rename.c
    src/braintest/braintest_panel_registry.c
    src/braintest/braintest_panel_types.c
    src/braintest/braintest_panelwindow.cpp
    src/braintest/braintest_botwindow.cpp
    src/braintest/braintest_mainimgui.cpp
    src/braintest/braintest_shotsimpanel.cpp
    src/braintest/braintest_shotsim_poi_registry.c
    src/braintest/braintest_vizdetail_registry.c
    src/braintest/braintest_vizdetailwindow.cpp
    src/braintest/braintest_pillcontrib_registry.c
)

# Per-bot BrainTest panel modules. Each bot may ship its own set of
# C++ panel renderers under brains/<bot>/braintest_panels/*.cpp; they
# self-register via static initializers using panelTypeRegister().
# Glob is build-time — adding a new bot's panels needs a CMake re-run.
file(GLOB BRAINTEST_BOT_PANEL_SOURCES
    "${CMAKE_SOURCE_DIR}/brains/*/braintest_panels/*.cpp")
list(APPEND BRAINTEST_SOURCES ${BRAINTEST_BOT_PANEL_SOURCES})


# BSD headers
file(GLOB BRAINTEST_BSD_SOURCES src/bsd/*.c)
list(APPEND BRAINTEST_SOURCES ${BRAINTEST_BSD_SOURCES})

# common/ utilities
file(GLOB BRAINTEST_COMMON_SOURCES src/common/*.c)
list(APPEND BRAINTEST_SOURCES ${BRAINTEST_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER BRAINTEST_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_executable(BrainTest ${BRAINTEST_SOURCES})
target_sources(BrainTest PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(BrainTest PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(BrainTest PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/braintest
    src/mapeditor
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(BrainTest braintest)

# braintest_vizwindow.cpp uses Dear ImGui (SDL3 + SDL_Renderer
# backends). imgui_backends carries the headers + the impl libs.
target_link_libraries(BrainTest PRIVATE imgui_backends)

if(WIN32)
    target_link_libraries(BrainTest PRIVATE ws2_32 winmm user32 dbghelp)
    target_compile_options(BrainTest PRIVATE -fcommon)
else()
    target_link_libraries(BrainTest PRIVATE ${MATH_LIB} pthread)
    target_compile_options(BrainTest PRIVATE -fcommon)
endif()

target_link_libraries(BrainTest PRIVATE bolo_static server_sim_static bot_brains_static threads_static)
target_link_libraries(BrainTest PRIVATE
    SDL3::SDL3 minizip lua_static
    nanosvg stb_image utf8proc cjson
)

# Copy shared libraries and assets next to BrainTest executable
add_custom_command(TARGET BrainTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:BrainTest>"
    COMMENT "Copying SDL3 shared library to BrainTest output directory"
)
add_custom_command(TARGET BrainTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BrainTest>/Brains"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/brains/GoalHunter_1.7"
        "$<TARGET_FILE_DIR:BrainTest>/Brains/GoalHunter_1.7"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:BrainTest>/data"
    COMMENT "Copying BrainTest data files to output directory"
)

# ---------------------------------------------------------------------------
# Bundle the dev-tool CLIs inside WinBolo.app (macOS)
# ---------------------------------------------------------------------------
# Drop the dedicated server, brain debugger, and headless client into
# Contents/MacOS/ next to the WinBolo binary so developers can run them
# straight from the signed bundle:
#     WinBolo.app/Contents/MacOS/WinBoloDS
#     WinBolo.app/Contents/MacOS/BrainTest
#     WinBolo.app/Contents/MacOS/WinBoloHeadless
# Because they live in Contents/MacOS/, SDL_GetBasePath() resolves to the
# bundle's Contents/Resources/ for them too, so they share WinBolo's bundled
# data/ and Brains/ — no separate asset copies needed. They load the
# SDL3/SDL3_ttf/onnxruntime dylibs already sitting next to WinBolo via the
# @executable_path rpath. `prepare.sh` signs them automatically: codesign
# --deep on the bundle recursively signs every nested Mach-O with the hardened
# runtime, which is all these terminal-launched tools need to notarize and run.
# This makes a WinBolo-only build also build the tools; turn it off with
# -DWINBOLO_BUNDLE_DEV_TOOLS=OFF for faster WinBolo iteration.
option(WINBOLO_BUNDLE_DEV_TOOLS
    "Bundle WinBoloDS, BrainTest, and WinBoloHeadless inside WinBolo.app (macOS)" ON)
if(APPLE AND WINBOLO_BUNDLE_DEV_TOOLS)
    # Guarantee all tools are built before WinBolo's POST_BUILD copy runs.
    add_dependencies(WinBolo WinBoloDS BrainTest WinBoloHeadless)
    add_custom_command(TARGET WinBolo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WinBoloDS>" "$<TARGET_FILE_DIR:WinBolo>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:BrainTest>" "$<TARGET_FILE_DIR:WinBolo>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:WinBoloHeadless>" "$<TARGET_FILE_DIR:WinBolo>"
        COMMENT "Bundling WinBoloDS, BrainTest, and WinBoloHeadless into WinBolo.app/Contents/MacOS"
    )
endif()


# ---------------------------------------------------------------------------
# MapEditor (standalone map editor)
# ---------------------------------------------------------------------------

set(MAPEDITOR_SOURCES
    src/bolo/transport_udp_client.c
    src/gui/sdl3/clientmutex.c src/gui/sdl3/luabrainshandler.c
    src/gui/sdl3/minimap_render.c
    src/gui/sdl3/lang.c
    src/gui/sdl3/gamefront_stubs.c
    src/gui/sdl3/mapview.c src/gui/sdl3/sprite_positions.c src/gui/sdl3/tileloader.c src/gui/sdl3/skin_source.c src/gui/sdl3/gfx_settings.c
    src/lzw/ecodlzw.c src/lzw/dcodlzw.c
    src/server/server_stubs.c
    src/mapeditor/mapeditor.c src/mapeditor/mapeditor_imgui.cpp src/mapeditor/mapgen_imgui.cpp src/mapeditor/mapeditor_undo.c src/mapeditor/mapeditor_validate.c src/mapeditor/mapeditor_symmetry.c src/mapeditor/mapeditor_text.c src/mapeditor/mapeditor_fonts.c src/mapeditor/mapeditor_stats.c src/mapeditor/mapeditor_image.c src/mapeditor/mapeditor_stamp.c src/mapeditor/mapeditor_export.c
    src/gui/sdl3/dialogs/dialog_footer.cpp
    src/gui/sdl3/dialogs/imgui_nav_outline.cpp
    src/third_party/stb/stb_image_write_impl.c
    src/mapeditor/mapeditor_main.c
    src/braintest/braintest_frontend.c
    # File > Open from WinBolo.net: the map chooser wired to the WBN
    # catalogue (MAPEDITOR_WBN_OPEN). The chooser drags in the preview
    # popup, its claim-a-start list and the star cache; ui_mode.c's one
    # external call (inputSourceCurrent) is already in gamefront_stubs.c.
    # The WBN HTTP layer comes from winbolonet_core; tweetnacl.c is
    # compiled per consumer like the WinBolo client and LogViewer do.
    src/gui/sdl3/dialogs/imgui_mapchooser.cpp
    src/gui/sdl3/map_preview_view.cpp
    src/gui/sdl3/map_preview_popup.cpp
    src/gui/sdl3/lobby_start_list.cpp
    src/gui/ui_mode.c
    src/gui/sdl3/map_stars.cpp
    src/gui/sdl3/wbn_map_source.cpp
    src/mapeditor/mapeditor_wbn_open.cpp
    src/third_party/tweetnacl/tweetnacl.c
)

file(GLOB MAPEDITOR_BSD_SOURCES src/bsd/*.c)
list(APPEND MAPEDITOR_SOURCES ${MAPEDITOR_BSD_SOURCES})

file(GLOB MAPEDITOR_COMMON_SOURCES src/common/*.c)
list(APPEND MAPEDITOR_SOURCES ${MAPEDITOR_COMMON_SOURCES})
if(NOT WIN32)
    list(FILTER MAPEDITOR_SOURCES EXCLUDE REGEX ".*common/util\\.c$")
endif()

add_executable(MapEditor ${MAPEDITOR_SOURCES})
target_sources(MapEditor PRIVATE src/steam/steam_wrapper_stub.c)
target_compile_definitions(MapEditor PRIVATE
    HAVE_SCREEN_C=1
    MAPEDITOR_WBN_OPEN=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
)

target_include_directories(MapEditor PRIVATE
    src/gui/sdl3
    src/gui
    src/server
    src/mapeditor
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(MapEditor mapeditor)

if(WIN32)
    target_sources(MapEditor PRIVATE src/res/mapeditor.rc)
    target_link_libraries(MapEditor PRIVATE ws2_32 winmm user32)
    target_compile_options(MapEditor PRIVATE -fcommon)
else()
    target_link_libraries(MapEditor PRIVATE ${MATH_LIB} pthread)
    target_compile_options(MapEditor PRIVATE -fcommon)
endif()

target_link_libraries(MapEditor PRIVATE bolo_static server_sim_static bot_brains_static threads_static)
target_link_libraries(MapEditor PRIVATE
    winbolonet_core winbolonet_client
    SDL3::SDL3 SDL3_ttf::SDL3_ttf imgui_backends minizip lua_static
    nanosvg stb_image utf8proc cjson CURL::libcurl
)

# ---------------------------------------------------------------------------
# MakeTileTestMap — writes a small .map that puts the terrain-shape lookups in
# screencalc.c side by side (river ends around a bridge, the deep-sea
# shoreline, boats), so each can be eyeballed in the game or the editor.
# EXCLUDE_FROM_ALL: built on demand, never part of a normal build.
#   cmake --build build --target MakeTileTestMap && ./build/MakeTileTestMap
#
# The source lives under src/mapeditor/ rather than tools/ because it needs
# bolo's T2 map-data headers (bolo_map.h, starts.h, bases.h, pillbox.h) and
# the editor's map-render math — exactly the scope of the mapeditor
# privileged exception in docs/ARCHITECTURE.md. tools/ is a public-only
# directory (T1+T3+T4); putting it there would have been a new, undocumented
# T2 exception. MAPEDITOR_SOURCES is an explicit list, not a glob, so this
# file's main() cannot leak into the MapEditor binary.
# ---------------------------------------------------------------------------
# test_stubs.c supplies the frontend / winbolonet / natPortMap link-time
# defaults, the same way it does for WinBoloUnitTests; transport_udp_client.c
# is per-target everywhere because of its HAVE_STEAM compile defs.
add_executable(MakeTileTestMap EXCLUDE_FROM_ALL
    src/mapeditor/tools/make_tile_test_map.c
    tests/unit/test_stubs.c
    src/bolo/transport_udp_client.c
    src/steam/steam_wrapper_stub.c
    src/lzw/ecodlzw.c
    src/lzw/dcodlzw.c
    src/common/md5.c
    src/common/mp_diag_log.c)
bolo_apply_include_rules(MakeTileTestMap mapeditor)
target_compile_definitions(MakeTileTestMap PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION})
# No bolo_grant_internal_source_access here: the mapeditor profile above
# already puts public/ + internal/ + the flat src/bolo/ on every TU in this
# target, so a per-file grant for transport_udp_client.c would grant nothing.
# MapEditor compiles the same file under the same profile without one.
target_include_directories(MakeTileTestMap PRIVATE
    src src/server src/gui src/gui/sdl3 src/client_frontend src/winbolonet
    ${cjson_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/unit)
# Same link set WinBoloUnitTests uses — the map writer sits in bolo_static,
# whose TUs reach back into the server sim and the WinBolo.net client.
target_link_libraries(MakeTileTestMap PRIVATE
    bolo_static ${SERVER_LINK_GROUP} bot_brains_static threads_static
    winbolonet_core winbolonet_client winbolonet_server
    SDL3::SDL3 minizip lua_static nanosvg stb_image utf8proc cjson CURL::libcurl
    mdns)
target_compile_options(MakeTileTestMap PRIVATE -fcommon)
if(WIN32)
    target_link_libraries(MakeTileTestMap PRIVATE ws2_32 winmm user32)
else()
    target_link_libraries(MakeTileTestMap PRIVATE ${MATH_LIB} pthread)
endif()
set_flat_output(MakeTileTestMap "${CMAKE_BINARY_DIR}")

# macOS trackpad pinch-to-zoom support, plus native NSMenu shim for the
# Map Editor's menu bar (used by both the standalone MapEditor.app and
# the embedded entry from WinBolo's welcome screen).
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(MapEditor PRIVATE src/gui/sdl3/macos_pinch.m)
    target_sources(MapEditor PRIVATE src/mapeditor/platform/mac_menubar.mm)
    target_link_libraries(MapEditor PRIVATE "-framework AppKit")
endif()

# macOS app bundle with icon
if(APPLE)
    set(MAPEDITOR_ICON "${CMAKE_SOURCE_DIR}/data/icons/mapeditor.icns")
    if(EXISTS "${MAPEDITOR_ICON}")
        target_sources(MapEditor PRIVATE "${MAPEDITOR_ICON}")
        set_source_files_properties("${MAPEDITOR_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        set_target_properties(MapEditor PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "mapeditor.icns"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/mapeditor/platform/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.winbolo.mapeditor"
            MACOSX_BUNDLE_BUNDLE_NAME "WinBolo Map Editor")
    endif()
endif()

# Copy shared libraries and assets next to MapEditor executable
add_custom_command(TARGET MapEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:MapEditor>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:SDL3_ttf::SDL3_ttf>" "$<TARGET_FILE_DIR:MapEditor>"
    COMMENT "Copying SDL3 and SDL3_ttf shared libraries to MapEditor output directory"
)

# bot_brains_static pulls in onnxruntime, so MapEditor inherits the
# dylib dependency. Mirror the WinBolo POST_BUILD copy step so the
# .app bundle ships with the dylib next to the executable — without
# this, launching the standalone MapEditor.app fails at dyld load
# time with "Library not loaded: @rpath/libonnxruntime.*.dylib".
if(HAVE_ONNXRUNTIME)
    if(WIN32 AND DEFINED ONNXRUNTIME_DLL)
        add_custom_command(TARGET MapEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:MapEditor>"
            COMMENT "Copying ONNX Runtime DLL to MapEditor output directory"
        )
    elseif(DEFINED ONNXRUNTIME_LIBDIR)
        file(GLOB _ORT_ALL_SO "${ONNXRUNTIME_LIBDIR}/libonnxruntime*")
        foreach(_f ${_ORT_ALL_SO})
            # Skip .dSYM bundles — they are directories, not files, so
            # copy_if_different fails on them. They are debug symbols that
            # don't need to ship next to the binary.
            if(_f MATCHES "\\.dSYM$")
                continue()
            endif()
            add_custom_command(TARGET MapEditor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" "$<TARGET_FILE_DIR:MapEditor>"
            )
        endforeach()
    endif()
endif()
if(APPLE)
    add_custom_command(TARGET MapEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data"
            "$<TARGET_BUNDLE_CONTENT_DIR:MapEditor>/Resources/data"
        COMMENT "Copying data files into MapEditor app bundle Resources"
    )
else()
    add_custom_command(TARGET MapEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/data" "$<TARGET_FILE_DIR:MapEditor>/data"
        COMMENT "Copying MapEditor data files to output directory"
    )
endif()

# MinGW: statically link the GCC/C++ runtime so the executables don't need
# libgcc_s_seh-1.dll, libstdc++-6.dll, or libwinpthread-1.dll at runtime.
if(MINGW)
    foreach(_tgt WinBolo WinBoloDS LogViewer WinBoloHeadless BrainTest MapEditor)
        if(TARGET ${_tgt})
            target_link_options(${_tgt} PRIVATE -static)
        endif()
    endforeach()
endif()

set_flat_output(LogViewer        "${CMAKE_BINARY_DIR}")
set_flat_output(WinBolo          "${CMAKE_BINARY_DIR}")
set_flat_output(WinBoloDS        "${CMAKE_BINARY_DIR}")
set_flat_output(WinBoloHeadless  "${CMAKE_BINARY_DIR}")
set_flat_output(BrainTest        "${CMAKE_BINARY_DIR}")
set_flat_output(MapEditor        "${CMAKE_BINARY_DIR}")
set_flat_output(winbolo_gym      "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Distribution zip  (cmake --build <dir> --target dist)
# ---------------------------------------------------------------------------

set(DIST_DIR "${CMAKE_BINARY_DIR}/dist/winbolo")
set(DIST_ZIP "${CMAKE_BINARY_DIR}/winbolo-dist.zip")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/dist")

# The WinBolo POST_BUILD steps already copy ALL needed shared libraries
# (SDL3, SDL3_ttf, curl, zlib, etc.) into the flat build output
# directory (CMAKE_BINARY_DIR).  The dist target depends on WinBolo, so by
# the time it runs, every DLL / .so / .dylib is already sitting there.
# We use a helper cmake -P script to glob and copy them all into dist/.

# Crashpad handler must be inside the dist target's command sequence (before the
# zip step) so it ends up in the archive.  We build the COMMAND as a variable
# because if() cannot appear inside add_custom_target().
set(DIST_CRASHPAD_HANDLER_CMD "")
if(HAVE_SENTRY AND TARGET crashpad_handler)
    set(DIST_CRASHPAD_HANDLER_CMD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:crashpad_handler>" "${DIST_DIR}/")
endif()

# Strip debug info from the staged Linux binaries before zipping.  Only Linux
# carries embedded DWARF in the ELF binaries; Windows keeps it in separate .pdb
# files and macOS in .dSYM bundles, so neither needs stripping here.  We strip
# the staged copies in DIST_DIR, never the build-tree originals, so the symbols
# uploaded to Sentry stay intact regardless of build/upload order.
set(DIST_STRIP_CMD "")
if(UNIX AND NOT APPLE AND CMAKE_STRIP)
    set(DIST_STRIP_CMD
        COMMAND "${CMAKE_STRIP}" "${DIST_DIR}/$<TARGET_FILE_NAME:WinBolo>"
        COMMAND "${CMAKE_STRIP}" "${DIST_DIR}/$<TARGET_FILE_NAME:WinBoloDS>"
        COMMAND "${CMAKE_STRIP}" "${DIST_DIR}/$<TARGET_FILE_NAME:LogViewer>"
        COMMAND "${CMAKE_STRIP}" "${DIST_DIR}/$<TARGET_FILE_NAME:MapEditor>"
        COMMAND "${CMAKE_STRIP}" "${DIST_DIR}/$<TARGET_FILE_NAME:BrainTest>")
     if(HAVE_SENTRY AND TARGET crashpad_handler)
        list(APPEND DIST_STRIP_CMD
            COMMAND "${CMAKE_STRIP}" "${DIST_DIR}/$<TARGET_FILE_NAME:crashpad_handler>")
    endif()
endif()

add_custom_target(dist
    DEPENDS WinBolo WinBoloDS LogViewer MapEditor BrainTest
    COMMENT "Creating distribution zip ${DIST_ZIP}"

    COMMAND ${CMAKE_COMMAND} -E rm -rf "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/Brains"

    # Binaries
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinBolo>"   "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinBoloDS>" "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:LogViewer>" "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:MapEditor>" "${DIST_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:BrainTest>" "${DIST_DIR}/"
    ${DIST_CRASHPAD_HANDLER_CMD}

    # Shared libraries — glob all DLLs / .so / .dylib from the build output
    # directory.  This picks up FetchContent libs AND vcpkg libs in one step.
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_BINARY_DIR}
        -DDEST_DIR=${DIST_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_shared_libs.cmake

    # Data assets (includes data/lang)
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data" "${DIST_DIR}/data"

    # Steam Input action manifest (Steam reads from controller_config/)
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/controller_config" "${DIST_DIR}/controller_config"

    # About-box docs (Authors, Third Party Notices) ship inside data/ so the
    # About box can find them via SDL_GetBasePath()/data/.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${DIST_DIR}/data/THIRD_PARTY_NOTICES.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/AUTHORS.md" "${DIST_DIR}/data/AUTHORS.md"

    # Brain scripts (excludes *.md plan/roadmap notes via copy_brains.cmake)
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/Brains/GoalHunter_1.7"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}/brains/GoalHunter_1.7
        -DDEST_DIR=${DIST_DIR}/Brains/GoalHunter_1.7
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_brains.cmake

    # Strip debug info from the staged Linux binaries (no-op on Win/macOS)
    ${DIST_STRIP_CMD}

    # Create zip (files at the archive root — no winbolo/ prefix).
    # chdir into DIST_DIR for the tar step only; the target's WORKING_DIRECTORY
    # has to stay at the parent because DIST_DIR is rm -rf'd above.
    COMMAND ${CMAKE_COMMAND} -E rm -f "${DIST_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${DIST_DIR}"
            ${CMAKE_COMMAND} -E tar "cf" "${DIST_ZIP}" --format=zip "."

    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    BYPRODUCTS "${DIST_ZIP}"
    VERBATIM
)

# ---------------------------------------------------------------------------
# macOS code-signing + notarization + stapling
# (cmake --build <dir> --target sign_macos)
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_custom_target(sign_macos
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/sign_macos.sh" "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Sign + notarize + staple all .app bundles (requires Developer ID cert)"
        VERBATIM
        USES_TERMINAL
    )

    add_custom_target(package_macos
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/package_macos.sh" "${CMAKE_BINARY_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS sign_macos
        COMMENT "Bundle signed apps + WinBoloDS into a notarized DMG (requires create-dmg)"
        VERBATIM
        USES_TERMINAL
    )
endif()

enable_testing()

# ---------------------------------------------------------------------------
# warmup.* — pay each test binary's first-execution cost before anything timed
#
# macOS assesses a newly written executable the first time it runs, and the
# assessment is not cheap: a freshly linked WinBoloDS takes ~1.05s to reach
# -help with only ~0.04s of that on CPU, against ~0.50s once the result is
# cached. Every dylib it pulls in (SDL3, onnxruntime, sentry) is assessed on
# first load too.
#
# That lands squarely on the UDP baseline scenarios. tests/baseline/run.sh
# waits a fixed 0.5s for the DS to come up and then schedules client commands
# against server tick counts, so on the first run after a build the server is
# still being assessed when the clients connect and the commands land at the
# wrong ticks. The captures diverge, the run reports a broad set of failures,
# and rerunning the identical tree passes — which makes the failures look
# flaky rather than mechanical.
#
# Running each binary once with a flag that loads it and exits pays that cost
# outside any timed window. FIXTURES_SETUP makes CTest schedule these first
# and only once, including when a subset is selected with -R.
#
# WinBoloUnitTests takes --list, not --help: test_main.c only recognises
# --list and --test, and anything else falls through to running all 600-odd
# tests in one process — including the *_capture tests that are kept out of
# CTest precisely because they rewrite committed fixtures. Getting this flag
# wrong does not just make the fixture slow, it dirties the working tree.
#
# Not guarded on APPLE: the cost elsewhere is three sub-second process starts,
# it warms the page cache on every platform, and a binary too broken to print
# its own usage is worth catching before 600-odd tests try to use it.
#
# -nocrashreporting on the two that have it: both call sentryInit before they
# parse anything, so even `--help` would otherwise open the one per-user crash
# database every other process in the suite is also opening. Same reason
# tests/baseline/run.sh passes it to every DS and client it starts.
add_test(NAME warmup.headless  COMMAND $<TARGET_FILE:WinBoloHeadless> -nocrashreporting --help)
add_test(NAME warmup.ds        COMMAND $<TARGET_FILE:WinBoloDS> -nocrashreporting -help)
add_test(NAME warmup.unittests COMMAND $<TARGET_FILE:WinBoloUnitTests> --list)
set_tests_properties(warmup.headless warmup.ds warmup.unittests
                     PROPERTIES FIXTURES_SETUP warm_binaries TIMEOUT 60)

# Per-scenario CTest entries. Scenario names must stay in sync with the
# dispatch_scenario case in tests/baseline/run.sh. --fast scenarios have
# no shared state and parallelize freely. UDP scenarios take whatever
# port the OS hands their DS — run.sh launches it with -port 0 and reads the
# port back off its stderr — so they hold no RESOURCE_LOCK and all of them
# run concurrently.
#
# They used to pin a port per helper and lock the scenarios sharing it. That
# lock only held those scenarios apart from each other: it said nothing about
# the unit tests, whose loopback harness binds 127.0.0.1:0 dozens of times
# over a -j run and could be handed the very port a scenario was about to
# bind, nor about a second copy of the suite, which contended for all nine.
# An OS-assigned port has none of those problems and needs no lock, which
# also lets the two ~21 s lobby scenarios run side by side rather than in
# sequence.
set(_baseline_fast
    everard_island_1bot_idle
    everard_island_1bot_sit
    everard_island_1bot_drive
    everard_island_1bot_shoot
    everard_island_1bot_watch
    forest_rig_1bot_idle
    forest_rig_1bot_sit
    forest_rig_1bot_drive
    forest_rig_1bot_shoot
    forest_rig_1bot_watch
    slugfest_iv_1bot_idle
    slugfest_iv_1bot_sit
    slugfest_iv_1bot_drive
    slugfest_iv_1bot_shoot
    slugfest_iv_1bot_watch
    centralize_events_fast
    centralize_events_teams_fast
    centralize_events_alliance_fast
    centralize_events_name_change_fast
    centralize_events_map_skip_fast
    road_spit_shell_open
    road_spit_shell_strict
    road_spit_shell_tournament
    road_spit_mine_open
    road_spit_drown_open
    boat_bank_road_slow
    boat_bank_road_fast
    boat_bank_grass_slow
    boat_bank_grass_fast
    builder_yard_farm
    builder_yard_road
    builder_yard_building
    builder_yard_mine
    builder_yard_pill_place
    builder_yard_pill_repair
    base_yard_capture
    base_yard_refuel
    base_yard_steal
    pill_yard_capture
    pill_yard_anger
    grass_flat_growth
    wave_defense_fast
)
set(_baseline_udp
    ds_4bot_melee
    ds_2v2_team
    centralize_events_udp
    centralize_events_name_change_udp
    centralize_events_map_skip_udp
    centralize_events_alliance_leave_udp
    centralize_events_shutdown_udp
    centralize_events_alliance_2client_udp
    centralize_events_game_over_udp
    centralize_events_lobby_smoke_udp
    centralize_events_lobby_name_change_udp
    centralize_events_chat_unicast_3client_udp
    centralize_events_chat_alliance_3client_udp
    watch_road_capture_2client_udp
    wave_swap_udp
)

foreach(_name IN LISTS _baseline_fast)
    add_test(NAME baseline.${_name}
             COMMAND stdbuf -oL bash ${CMAKE_SOURCE_DIR}/tests/baseline/run.sh
                          --scenario ${_name}
                          $<TARGET_FILE:WinBoloHeadless>
                          $<TARGET_FILE:WinBoloDS>)
    set_tests_properties(baseline.${_name} PROPERTIES
                         TIMEOUT 60
                         FIXTURES_REQUIRED warm_binaries)
endforeach()

foreach(_name IN LISTS _baseline_udp)
    add_test(NAME baseline.${_name}
             COMMAND stdbuf -oL bash ${CMAKE_SOURCE_DIR}/tests/baseline/run.sh
                          --scenario ${_name}
                          $<TARGET_FILE:WinBoloHeadless>
                          $<TARGET_FILE:WinBoloDS>)
    set_tests_properties(baseline.${_name} PROPERTIES
                         TIMEOUT 60
                         FIXTURES_REQUIRED warm_binaries)
endforeach()

# ---------------------------------------------------------------------------
# WinBoloUnitTests — in-process tests of bolo_static / server_sim_static
# / server_static without the SDL3 GUI. Each test is a registered name
# dispatched from tests/unit/test_main.c; CTest entries below run them
# one at a time.
#
# Uses the `unittests` privileged include profile defined in
# cmake/bolo_lib.cmake; scope and expiry documented in
# docs/ARCHITECTURE.md.
# ---------------------------------------------------------------------------
set(WINBOLO_UNITTESTS_SOURCES
    tests/unit/test_main.c
    tests/unit/test_stubs.c
    tests/unit/scratch_dir.c
    tests/unit/test_transport_local_passive_threads.c
    tests/unit/test_sp_subscriber_delivery.c
    tests/unit/test_active_sim_lifetime.c
    tests/unit/test_active_local_input_to_shot.c
    tests/unit/test_sp_shoot_through_timer.c
    tests/unit/test_prefs_roundtrip.c
    tests/unit/test_prefs_doc.c
    tests/unit/test_prefs_api.c
    src/common/prefs_doc.c
    src/common/prefs.c
    tests/unit/test_join_running_phase.c
    tests/unit/test_lobby_control_variants.c
    tests/unit/test_command_codec_roundtrip.c
    tests/unit/test_lobby_claim_start_dispatch.c
    tests/unit/test_error_smoothing.c
    tests/unit/test_command_rejected_apply.c
    tests/unit/test_live_player_stats.c
    tests/unit/test_shell_death_event.c
    tests/unit/test_alliance_reset.c
    tests/unit/test_lobby_add_bot_dispatch.c
    tests/unit/test_lobby_bot_config_memory.c
    tests/unit/test_command_queue_drain.c
    tests/unit/test_game_vote_control.c
    tests/unit/test_vote_surrender.c
    # All-bases win: one alliance holding every non-dead base ends the round on
    # the spot, announced and without starting a vote; a single dead base
    # (armour <= MIN_ARMOUR_CAPTURE, the X on the status pane) holds the round
    # open, which is the only comeback window there is.
    tests/unit/test_base_win_immediate.c
    # Game-over resolution: serverSimResolveGameOver maps returnToLobbyReason
    # to the returning lobby's message and the WinBolo.net win crediting — a
    # swept round names its winner, a passed vote credits nobody, a surrender
    # credits the opposing team, an abandoned round reports nothing.
    tests/unit/test_base_win_resolve.c
    # Surrender-vote privacy: the team-scoped CTRL_GAME_VOTE_STATE is withheld
    # from non-members on the wire, its newswire lines are suppressed on the
    # in-process bus, and a team-scoped (base-monopoly) back-to-lobby vote
    # stays public through both.
    tests/unit/test_vote_surrender_privacy.c
    tests/unit/test_round_stats.c
    tests/unit/test_mine_kill_event.c
    tests/unit/test_lv_stats_time.c
    tests/unit/test_attribution_track.c
    tests/unit/test_vote_toggle_modes.c
    tests/unit/test_vote_nolobby_disabled.c
    tests/unit/test_lobby_set_team_clamps.c
    # The scenario write door: the op funnel's prelude, the policy and
    # tick registrations beside it, and the flag that keeps the all-ready
    # detector out of a start already under way.
    tests/unit/test_scenario_funnel.c
    tests/unit/test_scenario_host.c
    # The validator: a script read in a stub VM and checked against the map,
    # the lobby template and the rule catalogue, without a round being run.
    tests/unit/test_scenario_validate.c
    # The WBSC container: a container written and read back through minizip's
    # memory I/O, the refusals a malformed buffer gets, the entry and brain
    # lists, and two containers open at the same time.
    tests/unit/test_scenario_package.c
    # manifest.json: the version 1 schema decoded into the same struct the
    # Lua reader fills, written back out with the keys this build does not
    # read still in it, and the comparison that holds a package's manifest
    # against the table its script declared.
    tests/unit/test_scenario_manifest_json.c
    # Where a map file's map data ends: the measure over synthetic BMAPs, the
    # container found after one, the scripted tag a packed map gets, and the
    # lobby preview that stops at the end of the map.
    tests/unit/test_scenario_map_body.c
    # A scenario carried inside the map file: the container's script run in
    # place of a loose one, the loose script that overrides it, the manifest
    # handed to a script that declares no table of its own, and the refusal
    # one that restates it and disagrees gets.
    tests/unit/test_scenario_packed_map.c
    # A map's scenario written into the map: what the container holds, the
    # manifest derived from the script's own table, the second pack that
    # replaces the first, and the map with nothing to pack.
    tests/unit/test_scenario_pack.c
    # The brain a scenario names: the name a team writes reaching the seat as
    # that brain's init.lua, the name this server has not got falling back to
    # the server's own brain, the path refused where a name belongs, and the
    # two roster ops that resolve a name of their own.
    tests/unit/test_scenario_brain_name.c
    # The scenarios directory: a .scenario package listed from its manifest
    # with no Lua run, a loose .lua listed through the validator's stub VM,
    # what is skipped, and the list encoded into the SCENARIO_LIST_RSP shape
    # against committed golden bytes and decoded back.
    tests/unit/test_scenario_dir.c
    # Which scenario plays when a map and a mod both have a claim, and the
    # points the template is applied at.
    tests/unit/test_scenario_precedence.c
    # The binding table: the game table a script reads the sim through, the
    # three index rules and what an absent entity reads as.
    tests/unit/test_scenario_lua.c
    # The state a scenario runs in: the names the whitelist takes and the
    # ones it keeps, the precompiled chunk the loader refuses, where a
    # script's print ends up, the memory cap it is refused at and the
    # instruction budget one call is cut off at.
    tests/unit/test_scenario_sandbox.c
    # The bus events the host queues and drains: the subscriber, the
    # fixed queue and the bounded drain at the end of each tick.
    tests/unit/test_scenario_events.c
    # What a drained event becomes: the hook written for it, the payload it
    # is handed, the mark saying whether the scenario caused the fact, and
    # the per-tick and round-end calls the drain makes beside them.
    tests/unit/test_scenario_hooks.c
    # What the host derives rather than hears: the timers a script sets, the
    # regions it names, and the enter and leave hooks the per-tick scan of
    # tank positions produces.
    tests/unit/test_scenario_derived.c
    # The scenario read surface: the roster, pill, base, start, tank and
    # builder accessors, the whole-map terrain copy and the fielded count.
    tests/unit/test_scenario_reads.c
    # The tank ops: the six arms, the refusals each one answers with, and the
    # records they leave through the functions a player's own action reaches.
    tests/unit/test_scenario_tank_arms.c
    # The builder ops: the five arms, the refusals each one answers with, the
    # wood a dispatched order costs, and the loss a scripted death records.
    tests/unit/test_scenario_builder_arms.c
    # The pill and base ops: the six arms, the refusals each one answers with,
    # an empty slot as an owner, the drain a hand-over makes and the flag that
    # keeps the stock, and the records the five changes leave in a recording.
    tests/unit/test_scenario_pill_base_arms.c
    tests/unit/test_scenario_entity_arms.c
    # The map ops: the four arms, the refusals each one answers with, deep sea
    # as a terrain a script may write, one publish per square a fill changes,
    # the budget that carries an oversized fill over two ticks, the answer a
    # fill with no budget left gets, and the round start and lobby map swap
    # that drop a remainder rather than painting the map that replaced it.
    tests/unit/test_scenario_map_arms.c
    # The roster ops: the six arms, the refusals each one answers with, the
    # queue that paces a spawn and a removal one to a tick, the init table a
    # spawned bot's brain is created with, and the one quiet accept a spawn
    # onto a team publishes.
    tests/unit/test_scenario_roster_arms.c
    # The unfielded seat: what a seat held without a bot in it counts for, the
    # start sequence walking past it, the spawn that fields it and the removal
    # that gives it back, what that removal costs — one event, no resync — and
    # the byte that carries the seat to clients.
    # The lobby template: the engine seating a scenario's teams at a map
    # commit, reconciling them when a round comes back, re-seating them when
    # an emptied lobby resets, and leaving a preview and a plain map alone.
    tests/unit/test_lobby_template.c
    # The scripted game type: the fourth value resolving through the base game
    # a scenario declared, at the loadout and at the start, and that value
    # going when the template does.
    tests/unit/test_scripted_game_type.c
    # What the lobby says about its scenario: the settings body with and
    # without one, the scripted game type a commit sets and gives back, and
    # the ranked and AI-policy settings a scenario does not go with.
    tests/unit/test_lobby_scenario_settings.c
    tests/unit/test_unfielded_seat.c
    # What a wave costs: the brain, the ClientSim and the control subscription
    # behind a held seat, destroyed when the seat comes off the field and built
    # again when it goes back on, counted at the calls that do it.
    tests/unit/test_scenario_wave_cost.c
    # The comms ops: the five arms, the refusals each one answers with, who
    # sees a line held to a team or to one player, the sound that reaches a
    # listener wherever they stand, and the records the three lines leave in a
    # recording. Plus the two cases that keep the destination honest: the body
    # decoder addresses a rebuilt line to every recipient, and the in-process
    # filter reads 0xFF as everyone at a client that is not slot 0.
    tests/unit/test_scenario_comms_arms.c
    # Three shots = go there: the shell-expiry detector that turns three
    # full-range shells on one open square into an order line in every
    # allied bot's inbox — which squares count, the window, and the count
    # that starts again after one.
    tests/unit/test_three_shot_order.c
    # The flow ops: a scripted end to the round — its state, its reason, the
    # line it leaves the returning lobby and the WinBolo.net crediting it does
    # not do — and the game-time change, the four lengths it refuses, and the
    # new length on the sim, at a subscriber and in a recording.
    tests/unit/test_scenario_flow_arms.c
    # The rules op: the index list against the table it indexes, a rule
    # written and read back, a rate the op's double carries and an int32
    # could not, the value outside a row and the value that breaks a pair
    # with the table each of them leaves untouched, and the record a write
    # puts in a recording — its bytes on disk and both of the viewer's
    # readers over them.
    tests/unit/test_scenario_rule_arms.c
    # fill_to_caps: a mod that raises a cap and asks for the map's pills and
    # bases to start at it rather than at the numbers the map file holds — a
    # base stock, a pill's armour, the same mod with the key left out, and
    # what the fill moved read back out of a recording.
    tests/unit/test_scenario_fill_caps.c
    # The six lifecycle and lobby policy pointers: the start a tank is given,
    # the base sweep, the team write in both lobby arms that make one, the
    # player cap, the loadout at all three places one is handed out, and the
    # death wait — each at its own call site under a policy answering the
    # opposite of classic, and once more with nothing registered.
    tests/unit/test_scenario_policy_lifecycle.c
    # The four combat policy pointers: the damage helper all three damage
    # paths go through, the build request, the pill and base captures, and the
    # five death sites — each under a policy answering the opposite of
    # classic, with the two kill ops shown to reach the death bodies without
    # asking, and once more with nothing registered.
    tests/unit/test_scenario_policy_combat.c
    # The same rows answered from a script instead: the host's lookup, the
    # words and indices a script is handed at each site, nil as the classic
    # answer, the op a policy may not issue while it is answering, and the
    # raise that counts toward the error limit.
    tests/unit/test_scenario_policy_lua.c
    # The in-process game-event channel: the second subscriber callback, and
    # the captures and builder death that reach it from GameSim callbacks
    # rather than from an event built in shared code, byte for byte as before.
    tests/unit/test_game_event_channel.c
    # The eight facts that had no game event: the spawn, the landing, the
    # pill placed, picked up and killed, the finished build, the mine laid
    # and the mine exploded — and the one of them that never leaves the host.
    tests/unit/test_game_events_new.c
    # The announce policy: the quiet byte on every fact that carries one,
    # the client line sites that read it, the vote line the server holds
    # back, and the same over a real loopback client.
    tests/unit/test_scenario_announce.c
    # The init table a bot is created with: one table per brain VM, an
    # empty one when the creator passed none, and the -bot-init [arg]
    # text that fills one.
    tests/unit/test_bot_init_table.c
    # The per-tank modifier set: the op that writes it, the snapshot group
    # under the ninth presence bit, and when the values are cleared.
    tests/unit/test_tank_modifiers.c
    # The sites that read a modifier: the movement rates, the turn rate, the
    # reload helper, the damage helper, the boat-exit speed and the brain's
    # stop predictor.
    tests/unit/test_tank_modifier_sites.c
    tests/unit/test_md5.c
    tests/unit/test_rand_save_restore.c
    tests/unit/test_net_opt_wire.c
    src/common/md5.c
    src/common/mp_diag_log.c
    tests/unit/test_ranked_lobby.c
    tests/unit/test_lobby_locks.c
    tests/unit/test_view_policy_settings.c
    tests/unit/test_classic_mode_settings.c
    tests/unit/test_allies_in_trees_settings.c
    tests/unit/test_fog_settings.c
    # The named visibility sets the lobby offers, and the match that reads
    # a live set of settings back as one of them.
    tests/unit/test_visibility_presets.c
    src/gui/visibility_presets.c
    tests/unit/test_info_packet_view_policies.c
    tests/unit/test_upload_cap.c
    tests/unit/test_map_field_clamps.c
    tests/unit/test_map_reload_rollback.c
    tests/unit/test_map_read_memory.c
    tests/unit/test_upload_busy.c
    tests/unit/test_upload_filename_safe.c
    tests/unit/test_lobby_time_minutes_valid.c
    tests/unit/test_lobby_bot_name.c
    tests/unit/test_lobby_map_list_chunked.c
    # The lobby's reload request: who may ask, when, and what a lobby with
    # no scenario answers.
    tests/unit/test_lobby_reload_scenario.c
    # The lobby's scenario pick: who may pick, when, and which names the
    # scenarios directory makes good.
    tests/unit/test_lobby_set_scenario.c
    tests/unit/test_wbn_bearer_state.c
    tests/unit/test_wbn_rekey_codec.c
    tests/unit/test_wbn_news_parse.cpp
    tests/unit/test_wbn_news_fetch.cpp
    tests/unit/test_wbn_country_cache.c
    tests/unit/test_wbn_prefs_sync.c
    tests/unit/test_wbn_serverlist.c
    tests/unit/test_wbn_map.c
    tests/unit/test_brain_crash_log.c
    tests/unit/test_brain_inbox.c
    tests/unit/test_brain_list_scan.c
    tests/unit/test_brain_list_texts.c
    tests/unit/test_unit_test_registry.c
    tests/unit/test_lobby_brain_docs_gate.c
    tests/unit/test_brain_modes.c
    tests/unit/test_brain_view_data.c
    tests/unit/test_bolo_rand.c
    tests/unit/test_pf_corner_cut.c
    tests/unit/test_pf_influence_tail.c
    # BrainTest Load Session next/prev-segment lookup. The finder is plain C
    # and ImGui-free precisely so it can be linked here; the rest of the
    # browser (braintest_loadbrowser.cpp) is not part of this target.
    tests/unit/test_loadbrowser_segment.c
    src/braintest/braintest_loadbrowser_segment.c
    # ... and the session-name grammar + rename planner that walk is built on
    # (braintest_loadbrowser_rename.c). Its apply step is the only part that
    # touches disk, so test_loadbrowser_rename.c exercises that against a
    # throwaway dir tree under the working directory.
    tests/unit/test_loadbrowser_rename.c
    src/braintest/braintest_loadbrowser_rename.c
    # Relay 0x01 metadata-frame parser (platform-neutral; the consume site
    # is emscripten-only but the parse compiles everywhere).
    tests/unit/test_proxy_meta_parse.c
    # Shared client-frontend cores: the tick cadence (keys-half pump gated
    # on clientSimTransportTicksServer) and the connect/landing wait. The
    # core sources compile here against the stubs in the test files;
    # client_frontend_common.c stays out (it drags the sdl3 draw layer).
    src/client_frontend/client_frontend_tick.c
    src/client_frontend/client_frontend_connect.c
    tests/unit/test_client_frontend_tick.c
    tests/unit/test_await_join.c
    tests/unit/test_net_impair.c
    tests/unit/test_lobby_runtime_fixes.c
    tests/unit/test_bot_chat_routing.c
    tests/unit/test_bot_ping.c
    tests/unit/test_brain_internal_msg_routing.c
    tests/unit/test_subscriber_arm_orphans.c
    tests/unit/test_identity_survives_reset.c
    tests/unit/test_wbn_session_rotation.c
    tests/unit/test_maprotate_rotation.c
    tests/unit/test_wbn_deferred_join.c
    tests/unit/test_join_collision.c
    tests/unit/test_base_refuel_timers.c
    tests/unit/test_lobby_reset_cleanup.c
    tests/unit/test_autolock_on_game_start.c
    tests/unit/test_lobby_transfer_host.c
    tests/unit/test_treegrow_no_sea.c
    tests/unit/test_pill_repair_load.c
    tests/unit/test_entity_lifecycle.c
    tests/unit/test_entity_event.c
    tests/unit/test_entity_sync.c
    tests/unit/test_entity_record.c
    tests/unit/test_starts_pick_incremental.c
    tests/unit/test_starts_assign_batch.c
    tests/unit/test_starts_open_ideal.c
    tests/unit/test_start_sides.c
    tests/unit/test_lobby_side_axis.c
    tests/unit/test_lobby_start_shared.c
    tests/unit/test_starts_scatter_separation.c
    tests/unit/test_starts_team_side.c
    tests/unit/test_lobby_team_side_dispatch.c
    tests/unit/test_starts_side_integration.c
    tests/unit/test_input_redundancy.c
    tests/unit/test_edge_send.c
    tests/unit/test_stall_advance.c
    tests/unit/test_base_stock_visibility.c
    tests/unit/test_base_half_tick.c
    tests/unit/test_base_death_prediction.c
    tests/unit/test_tank_alliance_unfielded.c
    tests/unit/test_tank_death_state.c
    tests/unit/test_tank_status_wire.c
    tests/unit/test_voice_flags.c
    tests/unit/test_voice_talking_codec.c
    tests/unit/test_voice_talking_set.c
    # The rules event's body codec: every carried rule round-trips field by
    # field, rates a fixed-point scale could not carry survive, and the
    # body's bytes are pinned against a layout written out by hand.
    tests/unit/test_sim_rules_codec.c
    tests/unit/test_fx_viewport_cull.c
    tests/unit/test_sound_delivery.c
    tests/unit/test_view_policy_rects.c
    tests/unit/test_tree_hide.c
    tests/unit/test_lgm_visibility.c
    tests/unit/test_view_state.c
    tests/unit/test_view_target_codec.c
    tests/unit/test_view_target_apply.c
    tests/unit/test_view_cycling.c
    tests/unit/test_pill_pos_current.c
    tests/unit/test_viewport_calc.c
    tests/unit/test_overview_map.c
    tests/unit/test_overview_view_policy.c
    tests/unit/test_overview_camera.cpp
    tests/unit/test_overview_fog.cpp
    tests/unit/test_overview_hud_layout.cpp
    tests/unit/test_sight.c
    tests/unit/test_sight_shadow.c
    tests/unit/test_sight_shadow_overflow.c
    tests/unit/test_overview_snapshot.c
    tests/unit/test_mapview_sprite_scale.c
    tests/unit/test_mapview_overlay.c
    tests/unit/test_input_catchup.c
    tests/unit/test_build_harvest_stale.c
    tests/unit/test_lgm_request_valid.c
    tests/unit/test_lgm_quit_pill.c
    tests/unit/test_jitter_buffer_grow.c
    tests/unit/test_shell_projection.c
    tests/unit/test_sim_rules.c
    tests/unit/test_screen_bullets_build.c
    tests/unit/test_ping_smoother.c
    tests/unit/test_client_timing.c
    tests/unit/test_interp_render.c
    tests/unit/test_interp_respawn_no_death_flash.c
    tests/unit/test_snapshot_compaction.c
    tests/unit/test_log_roundtrip.c
    tests/unit/test_log_dir_path.c
    src/server/server_dedicated_log_path.c
    # Record-and-decode replay fixture and the case that uses it. The
    # fixture is two translation units because the sim headers and the
    # log-viewer headers cannot share one: replay_harness.c records a round
    # and diffs two captures, replay_harness_decode.c reads the .wbv back
    # through the production viewer. replay_harness.h carries the seam.
    tests/unit/replay_harness.c
    tests/unit/replay_harness_decode.c
    tests/unit/test_replay_roundtrip.c
    # Recordings older than the reader that opens them.
    tests/unit/test_replay_version_compat.c
    # The brain recorder's own version check.
    tests/unit/test_brainrec_version.c
    # Loopback transport tests: real UDP client + server over localhost,
    # sharing the loopback_harness fixture.
    tests/unit/loopback_harness.c
    tests/unit/test_loopback_join.c
    tests/unit/test_loopback_join_loss.c
    tests/unit/test_loopback_lobby_running_loss.c
    tests/unit/test_loopback_quiet_lobby_control_loss.c
    tests/unit/test_loopback_command_liveness.c
    tests/unit/test_loopback_channel.c
    tests/unit/test_player_mute.c
    tests/unit/test_lock_channel.c
    tests/unit/test_loopback_preview.c
    tests/unit/test_loopback_download.c
    tests/unit/test_loopback_map_change.c
    tests/unit/test_loopback_entity_change.c
    # A scenario's changeover from a connected client: the pillbox a human
    # holds keeps its owner while a held seat is fielded and taken off again,
    # with no full sync in the middle to cover a migration.
    tests/unit/test_loopback_scenario_seat.c
    # The rules table over the wire: a mid-round change reaching a connected
    # client, a joiner arriving on the changed table, a server-only rule
    # publishing nothing, and a new table clamping what the client holds.
    tests/unit/test_loopback_sim_rules.c
    # The client's own check of the rules table before it keeps one.
    tests/unit/test_sim_rules_client_check.c
    # The sixteen pillbox pictures against a pill armour cap above 15.
    tests/unit/test_pill_armour_scale.c
    tests/unit/test_loopback_map_cull.c
    tests/unit/test_sound_delivery_wire.c
    tests/unit/test_view_ally_loopback.c
    tests/unit/test_loopback_upload.c
    tests/unit/test_gate1_integration.c
    tests/unit/test_interp_jitter_e2e.c
    tests/unit/test_join_version_gate.c
    tests/unit/test_join_rate_limit.c
    tests/unit/test_cookie_handshake.c
    tests/unit/test_map_amp_gate.c
    tests/unit/test_map_reask_throttle.c
    tests/unit/test_map_resync.c
    tests/unit/test_map_compress_roundtrip.c
    tests/unit/test_map_shadow.c
    tests/unit/test_resync_finalize.c
    tests/unit/test_conn_migration.c
    tests/unit/test_viewtick_rewind.c
    # mDNS discovery: unicast-loopback round-trip through the real advertiser
    # builder + browser parse path (no multicast socket).
    tests/unit/test_mdns_discovery.c
    tests/unit/test_mdns_view_txt.c
    # Wire-codec corpus: the tap sink, the fast differential test (reads
    # committed fixtures), and the on-demand capture generator (uses the
    # loopback harness above).
    tests/unit/wire_corpus.c
    tests/unit/test_wire_corpus.c
    tests/unit/test_wire_corpus_capture.c
    # ChannelMux reliability primitive: the off-socket loss/reorder/dup/
    # window/boundary matrix (two muxes + an in-test frame shuttle).
    tests/unit/test_channel_mux.c
    # Spectator delayed-stream ring buffer: segmentation across a game-tick
    # reset, keyframe-at-segment-start, mid-interval seek + forward replay,
    # segment isolation, previous-generation read, cold start and the
    # retention window boundary, all driven with synthetic byte payloads.
    tests/unit/test_spectator_ring.c
    # Spectator ring records without a .wbv log: a registered ring driven by the
    # normal serverSimTick (logWriteTick) populates and a delay-0 seek returns a
    # seed, with no logStart and no .wbv file — the real (non-recording) server
    # path that the logStart-based ring tests hid.
    tests/unit/test_spectator_ring_nolog.c
    # Spectator replay translator: ring keyframe / event-tick records ->
    # the LOG_*-marked plaintext byte stream + synthesized v2 header, proven
    # byte-for-byte and by decoding a real translated keyframe through
    # lv_screenLoadFromStream.
    tests/unit/test_spectator_replay.c
    # Spectator config plumbing: -maxspectators / -specdelay flow through
    # ServerInstanceConfig into the sim and back via the getters.
    tests/unit/test_spectator_config.c
    # Spectator JOIN handshake: a JOIN_FLAG_SPECTATOR join registers a tankless
    # viewer (slot 0xFF, no tank slot) under the maxSpectators cap.
    tests/unit/test_spectator_join.c
    # Spectator roster publish: the server broadcasts CTRL_SPECTATOR_SLOT to
    # player clients on spectator join/leave and catch-up-sends the roster to a
    # newly-joined player; verified through the real client apply path.
    tests/unit/test_spectator_roster_publish.c
    # CTRL_SPECTATOR_SLOT body-codec round-trip: encode/decode the per-spectator
    # roster event through the body tables, including the out-of-range rejection.
    tests/unit/test_spectator_slot_codec.c
    # CTRL_SPECTATOR_CHAT body-codec round-trip: encode/decode the spectator
    # lobby-chat event through the body tables, including the empty/max-length
    # messages and the out-of-range specIdx / msgLen-overrun rejections.
    tests/unit/test_spectator_chat_codec.c
    # CTRL_STATS_SEED body-codec round-trip: encode/decode the mid-round live
    # scoreboard seed through the body tables, including the full roster, the
    # zero-row minimum, and the truncated / bad-slot rejections and the clamp.
    tests/unit/test_stats_seed_codec.c
    # Spectator-chat routing: a published CTRL_SPECTATOR_CHAT reaches a player
    # bus subscriber and a live spectator (allowlist); broadcast CTRL_CHAT
    # reaches the spectator; team/unicast CTRL_CHAT does not.
    tests/unit/test_spectator_chat_routing.c
    # Spectator-chat replay emission: serverSimReceiveSpectatorChat writes a
    # log_SpectatorChat (specIdx + message) into the recording .wbv stream.
    tests/unit/test_spectator_chat_log.c
    # Lobby-chat catch-up buffer: broadcast + spectator chat captured in
    # lobby/countdown, cleared at game start, capped, and re-delivered by
    # serverSimReplayLobbyChat at the spectator drain-flip (fresh joins none).
    tests/unit/test_spectator_chat_catchup.c
    # Spectator connect — client side: the real client transport connecting with
    # the spectator flag lands in CLIENT_CONNECT_SPECTATING (tankless, awaiting
    # seed), consuming no tank slot and starting no map download.
    tests/unit/test_spectator_connect.c
    # Spectator live-lobby control intake — client side: a tankless spectator
    # (myPlayerNum 0, aliasing real slot 0) fed the allowlisted lobby control
    # bus populates its mirror without mis-attributing slot 0 to the viewer.
    tests/unit/test_spectator_client_lobby_intake.c
    # Spectator command isolation: a tankless viewer's broadcast chat is accepted
    # and round-trips into its lobby log, while its non-chat lobby/gameplay
    # commands have no server-state effect (client guards refuse to originate
    # them; the dispatcher guard rejects an out-of-range sender slot).
    tests/unit/test_spectator_command_reject.c
    # Spectator lobby read-only invariant: a tankless viewer fires every guarded
    # clientSimNetSend* mutation path and the seated lobby is wholly untouched —
    # the client-side send-suppression backstop (dialog rendering is human-tested).
    tests/unit/test_spectator_lobby_readonly.c
    # Spectator feed capture — client side: a real spectator client over loopback
    # captures the CHANNEL_BULK seed + ordered forward records into the ClientSim
    # feed (record header stripped, order preserved), exercising the ack-emitter
    # gate fix end-to-end.
    tests/unit/test_spectator_capture.c
    # Spectator replay-log events: serialize round-trip of the Joined/Left/Chat
    # kinds over the v2 framed .wbv stream, plus loopback emission of Joined on
    # a spectator join and Left on its timeout.
    tests/unit/test_spectator_log.c
    # Spectator seed transfer: a connected spectator on a recording-ring server
    # gets the delayed keyframe at head - delay armed as a BULK_KIND_SPEC_SEED
    # transfer whose blob is byte-equal to the ring keyframe at that delay.
    tests/unit/test_spectator_seed.c
    # Live-lobby spectator control bus: lobby subscribe + sync replay,
    # serverSpectatorDeliverControl allowlist, subscriber capacity, and the
    # spectator-roster enumerator + fan to live spectators.
    tests/unit/test_spectator_live_bus.c
    # Live↔delayed cutover for spectators: the anti-cheat boundary at game
    # start (a live spectator is unsubscribed before any running-state publish,
    # so it receives zero running control) and the drain-then-flip back to the
    # live lobby once the delayed game is fully watched.
    tests/unit/test_spectator_cutover.c
    # Dual-mode spectator session: the client live-lobby bit is seeded from the
    # accept mode byte, flips to delayed when the feed begins at game start, and
    # back to live when lobby control resumes after the delayed game drains.
    tests/unit/test_loopback_spectator_lobby.c
    # Bulk-transfer framing on CHANNEL_BULK: stream-header round-trip,
    # byte-identical blob reassembly under loss, header robustness,
    # pipelining and the send-side serializer guard.
    tests/unit/test_bulk_transfer.c
    # Round-log transfer: the BULK_KIND_ROUND_LOG stream header at the extremes
    # the transfer uses, the realignment a rejected stream must leave behind,
    # and the loopback refusal a server with no RoundLogSource answers with.
    tests/unit/test_round_log.c
    # Send-side overflow guards on channelStreamSend / bulkSenderBegin: a
    # wrap-prone length is rejected with no state change.
    tests/unit/test_overflow_guards.c
    # CTRL_CHANNEL_RESET: body-codec round-trip and the per-client game-start
    # baselines transportUdpServerOnGameStart sends across two fabricated slots.
    tests/unit/test_channel_reset.c
    tests/unit/test_packet_type_names.c
    # Smart-ping pure headers — the chord packing and its exact-modifier
    # match rule, the pie's slice selection (dead zone and the wrap at the
    # top), and the off-screen edge marker on all four borders and at a
    # corner. No SDL window, no game.
    tests/unit/test_ping.c
    # The CMD_PING arm in server_command_dispatch.c: its state and range
    # gates, the per-player rate limit, the GameEvent it builds, and the
    # team-only delivery predicate both snapshot filters share.
    tests/unit/test_ping_dispatch.c
    # The two pure curves the world marker is drawn from: the three-flash
    # blink in ping_kinds.h and the arrival ring's close/pulse in
    # ring_band.c. Both take a ping's age and nothing else.
    tests/unit/test_ping_marker_anim.c
    # ringAnimAt, for the test above. A leaf under the "Linked GUI sources"
    # rule in docs/ARCHITECTURE.md: plain SDL, no ImGui, and it opens no
    # window, renderer or device of its own.
    src/gui/sdl3/ring_band.c
    # The log_GameSettings blob: the dedicated log's writer and the log
    # viewer's decode over the same bytes, old payload lengths included.
    tests/unit/test_game_settings_blob.c
    # Smart ping over the wire: the full client -> server -> client path on the
    # real loopback UDP transport, driven through serverInstanceTick so the
    # per-frame event-buffer clear that drops a networked ping is exercised.
    tests/unit/test_ping_network.c
    # Generated lang-name table: K_LANG_NAME_TABLE_SIZE vs the real array
    # length, strict bsearch sort order, and name round-trips — pins the
    # off-by-one that crashed resolveName's bsearch on startup.
    tests/unit/test_lang_name_table.c
    # Bot difficulty plumbing: the "difficulty=<word>" BRAIN_INIT_ARG token
    # assembly (whole-token-or-nothing when the buffer is tight) and the
    # difficulty <-> word round trip -difficulty and the preference share.
    tests/unit/test_bot_init_arg.c
    # River tile lookup: ROAD must read as "the river continues" in all four
    # RIVER_END arms (a bridge on a river cross drew corner pieces), plus the
    # game vs log-viewer copies of screenCalcRiver never disagreeing.
    tests/unit/test_screencalc_river.c
    tests/unit/test_client_type.c
    # A downloaded map's bases/pillboxes/starts are memcpy'd from the wire, so
    # the file-load setters' per-field clamps never run on that path.
    tests/unit/test_map_load_validate.c
    # The client's tank and lgm live at its slot index; a second JOIN_ACCEPT
    # with a different slot has to carry them across rather than read slot 0.
    tests/unit/test_client_slot_reassign.c
    tests/unit/test_wire_player_bounds.c
    tests/unit/test_control_overflow_disconnect.c
    # Mid-sim-frame player removal vs the arrays simRunHalfStep snapshots for
    # its world update — the high-ping kick crash (shellsUpdate NULL lgm slot).
    tests/unit/test_ping_kick_teardown.c
    # tkExplosionUpdate's lgm/tank index pairing over the compacted per-player
    # arrays, plus the small-explosion sweep's off-by-one.
    tests/unit/test_tkexp_lgm_pairing.c
    tests/unit/test_input_gate.c
    tests/unit/test_key_claims.c
    tests/unit/test_server_address_parse.c
    # Skin asset reads: a directory, a flat zip and a zip wrapped in one
    # top-level folder must resolve the same relative names to the same
    # bytes.
    tests/unit/test_skin_source.c
    src/gui/sdl3/skin_source.c
    src/gui/sdl3/gfx_settings.c
    # Which densities a skin serves and what each Tile Detail mode picks.
    # Needs the tile loader itself, which no other unit test pulls in.
    tests/unit/test_skin_density.c
    src/gui/sdl3/tileloader.c
    # The colour left under the built sheet's transparency, and the bleed
    # that takes it out. Runs on the real data/svg art beside the binary.
    tests/unit/test_sheet_bleed.c
    # The texel ring around a sprite slot: clean on the padded atlas the
    # sprite drawers sample, and what the packed sheet has there instead.
    tests/unit/test_sprite_atlas.c
    src/gui/sdl3/sdl_bmp.c
    # Which of <name>.wav and <name>_0.wav .. <name>_9.wav a source holds,
    # and the compaction that leaves the members which decoded contiguous.
    # Name arithmetic only: no audio device, and it opens no file.
    tests/unit/test_sound_variants.c
    src/gui/sdl3/sound_variants.c
    # The Bolo pascal-string reader in both places it is implemented —
    # src/bolo/util.c and the log viewer's own copy.
    tests/unit/test_pascal_string.c
    # Operator console command parsing, driven through ServerConsoleOps
    # with recording callbacks instead of a live server (server_console.c
    # itself comes in with server_static).
    tests/unit/test_server_console.c
    # Compiled standalone (libcurl-free) so the bearer-state, news-parser
    # and country-cache tests can link without dragging the rest of
    # winbolonet_core into the test binary. The test_stubs.c stubs cover
    # the other winbolonet_* functions; these TUs only add the bearer,
    # country-cache and parser surfaces.
    src/winbolonet/wbn_bearer.c
    src/winbolonet/wbn_country_cache.c
    src/winbolonet/wbn_news_parser.cpp
    # The overview's camera maths, its fog mask, the in-window HUD geometry
    # and the sprite placement arithmetic behind mapview.c's drawers, with
    # test_overview_camera.cpp / test_overview_fog.cpp /
    # test_overview_hud_layout.cpp / test_mapview_sprite_scale.c exercising
    # them directly. Like the other src/gui/sdl3 files in this target they
    # are leaves a test can call with no display attached: no ImGui, and no
    # window, renderer or device of their own. The drawing halves
    # (overview_view.cpp, mapview.c) stay out. They keep the gui profile's
    # public-only access rather than borrowing this target's T2;
    # ARCHITECTURE.md's "Linked GUI sources" carries the rule a file has to
    # meet to be added here, and when the exception lapses.
    #
    # sprite_positions.c qualifies on the same footing as the other three:
    # its includes are the tile atlas coordinates, tilenum.h, global.h and the
    # GfxAnimSmoothness enum, and its functions are float arithmetic over
    # their arguments. It is the file the log viewer already links on its
    # own for the atlas table.
    src/gui/sdl3/overview_camera.cpp
    src/gui/sdl3/overview_fog.cpp
    src/gui/sdl3/overview_hud_layout.cpp
    src/gui/sdl3/sprite_positions.c
    # The threaded news fetcher itself (not just the pure parser): the
    # free-during-fetch lifetime test drives its real worker thread. Its
    # network GET is reached through a test seam, so no libcurl is pulled in
    # (test_stubs.c owns the wbn_api_get link-time default).
    src/winbolonet/wbn_news.cpp
    src/winbolonet/wbn_prefs_sync.c
    src/winbolonet/wbn_serverlist.c
    src/winbolonet/wbn_map.c
    # transport_udp_client.c stays per-target because of HAVE_STEAM
    # gating (it's excluded from bolo_static for that reason). Linked
    # here without HAVE_STEAM so client_net.c's transportUdpClient*
    # references resolve — the tests never run the UDP code path.
    src/bolo/transport_udp_client.c
    # transport_udp_common.c is also compiled per-target so this target's
    # WB_WIRE_CORPUS_CAPTURE reaches its WIRE_CORPUS_TAP calls; bolo_static
    # ships it without the flag. This object provides every transport_udp_common
    # symbol, so bolo_static's copy is not pulled from the archive (no
    # duplicate symbols). The file references no bolo_static-only define, so the
    # per-target build is behaviourally identical bar the tap.
    src/bolo/transport_udp_common.c
    # server_stubs.c (transportUdpServer* / serverLifecycle* stubs)
    # is intentionally NOT linked here: server_static (linked below)
    # provides the real transport_udp_server.c and server_lifecycle.c
    # bodies, and stubbing them too would collide at link time. The
    # client-side hooks server_lifecycle.c still references but
    # server_static doesn't own (winbolonet* in src/winbolonet/,
    # natPortMap* in src/bolo/nat_portmap.c) stay stubbed in
    # test_stubs.c — acceptRemoteClients=false keeps them off the
    # tick-time call paths.
    src/lzw/dcodlzw.c
    src/lzw/ecodlzw.c
    # .wbv reader gate: load the committed v1/v2 fixtures through the
    # production log-viewer reader (test_wbv_reader.c), and the on-demand
    # writer-drive that (re)generates the v2 fixture (test_wbv_v2_capture.c).
    tests/unit/test_wbv_reader.c
    tests/unit/test_wbv_v2_capture.c
    # Recorded headless runs: each committed <name>.wbv is decoded through
    # the viewer and its summary compared with <name>.summary; the same
    # file holds the on-demand capture that rewrites those summaries.
    tests/unit/test_wbv_fixture_summaries.c
    tests/unit/test_attribution_reader.c
    # Calibration anchor walk: synthetic v2 logs prove the highlight
    # clip-time fit resolves base cells from the log's own snapshots
    # (test_lv_calibration.c).
    tests/unit/test_lv_calibration.c
    # On-demand generator for the committed real ring-seed fixture
    # (spectator_seed.bin): captures a ServerSim ring keyframe with no trailing
    # data for test_spec_seed_load_real (test_spectator_seed_capture.c).
    tests/unit/test_spectator_seed_capture.c
    # Blocks-level append-fed stream source: drives blocks.c's no-zip path.
    tests/unit/test_blocks_stream.c
    # Viewer-side decode of per-tank stocks: hand-built snapshot player blocks
    # (with and without the stock bytes) and a log_TankSetStock record, through
    # lv_specSeedLoad / lv_specRecordPump.
    tests/unit/test_lv_tank_stocks.c
    # The viewer reading a recording it cannot trust: item counts past the
    # arrays, an entity removal past the array, a server line to a slot past
    # the roster, a modifiers blob of the wrong length.
    tests/unit/test_lv_hostile_records.c
    # Stream-load decode path: feeds the v2 fixture's plaintext log.dat through
    # lv_screenLoadFromStream and asserts it decodes the same content the zip
    # reader does (reuses extractLogDat from test_log_roundtrip.c).
    tests/unit/test_stream_load.c
    # Feeds the same fixture through lv_screenStreamPump a record at a time
    # (with a caught-up pump) and matches a one-shot load to end-of-log.
    tests/unit/test_stream_pump.c
    # Spectator seed load: fabricates a ring-shaped seed from the v2 fixture's
    # snapshot body and runs it through lv_specSeedLoad (synthesize header ->
    # translate keyframe -> load), asserting the decode + the stashed control
    # slice.
    tests/unit/test_spec_seed_load.c
    # Real spectator seed load: loads the committed spectator_seed.bin (a real
    # ring seed, no trailing data) through lv_specSeedLoad and asserts TRUE —
    # reproduces the lv_processSnapshot over-read (test_spec_seed_load_real.c).
    tests/unit/test_spec_seed_load_real.c
    # Spectator forward-record pump: seeds the decoder from the v2 fixture's
    # snapshot, derives ring-shaped forward records from its event stream and
    # pumps them through lv_specRecordPump, asserting the decoder advances (same
    # end state), an empty tick is clean, and a keyframe re-syncs + refreshes the
    # control slice.
    tests/unit/test_spec_record_pump.c
    # Headless spectator seed integration: a running ServerSim with a registered
    # spectator ring (no logStart) delivers a seed over the loopback transport;
    # the client captures it and the production decode path loads it
    # (test_spectator_seed_integration.c).
    tests/unit/test_spectator_seed_integration.c
    # specSeedDecodeInfo decode: captures a real ring seed and asserts the
    # control-snapshot lobby-settings event decodes to the sim's map / settings,
    # plus the no-event negative path (test_spec_seed_decode_info.c).
    tests/unit/test_spec_seed_decode_info.c
    # Link-only no-op stubs for the host/render/window/message entry points
    # the decode subset below references but whose real bodies live in the
    # GUI TUs (logviewer.c, draw.c, messages.c) we do not link.
    tests/unit/test_logviewer_stubs.c
    # Log-viewer decode subset reachable from lv_screenLoadMapFromMemory:
    # screen.c + blocks.c plus the snapshot / players / world-object / map /
    # render-data-structure TUs they call. The GUI/render TUs (logviewer.c,
    # draw.c, sound.c, messages.c, game_view.c, bolo_shim.c, imgui/*) are
    # NOT linked — their referenced entry points are stubbed above.
    src/logviewer/screen.c
    src/logviewer/blocks.c
    src/logviewer/lv_attribution.c
    src/logviewer/lv_stats.c
    src/logviewer/snapshot.c
    src/logviewer/players.c
    # Logviewer-world spectator seed orchestration: raw ring keyframe seed ->
    # synthesized v2 header + translated snapshot -> lv_screenLoadFromStream.
    # Listed explicitly because this target uses its own decode-subset list
    # (not LOGVIEWER_LIB_SOURCES); spectator_replay.h is now a dep-free public
    # seam, so the runtime LOGVIEWER libs also link it on their own.
    src/logviewer/spec_seed_load.c
    src/logviewer/pillbox.c
    src/logviewer/bases.c
    src/logviewer/starts.c
    src/logviewer/shells.c
    src/logviewer/bolo_map.c
    src/logviewer/screencalc.c
    src/logviewer/screentank.c
    src/logviewer/screenlgm.c
    src/logviewer/screenbullet.c
    src/logviewer/sounddist.c
    src/logviewer/allience.c
    src/logviewer/labels.c
    src/logviewer/util.c
    src/logviewer/global.c
    src/logviewer/clientmutex.c
    src/logviewer/dns.c
    # log_GameSettings blob decode. Plain C with no bolo or ImGui headers,
    # split out of imgui_game_info.cpp so test_game_settings_blob.c drives
    # the code the Game Information panel runs rather than a copy of it.
    src/logviewer/game_settings_blob.c
)

if(WB_NETDEBUG)
    list(APPEND WINBOLO_UNITTESTS_SOURCES tests/unit/test_netdebug_rig.c)
endif()

add_executable(WinBoloUnitTests ${WINBOLO_UNITTESTS_SOURCES})
target_sources(WinBoloUnitTests PRIVATE src/steam/steam_wrapper_stub.c
    src/bolo/lobby_bot_pools_json.c
    tests/unit/test_bot_name_pools.c
    src/client_frontend/voice_core.c
    tests/unit/test_voice_core.c
    tests/unit/test_voice_device.c
    tests/unit/test_voice_peak.c
    tests/unit/test_voice_jitter.c
    tests/unit/test_voice_jitter_loss.c
    tests/unit/test_voice_flood_cap.c
    src/bolo/voice_segment.c
    tests/unit/test_voice_segment.c
    src/bolo/voice_talker_select.c
    tests/unit/test_voice_talker_select.c)
target_compile_definitions(WinBoloUnitTests PRIVATE
    HAVE_SCREEN_C=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
    # Activate the network-impairment hooks for this target's compilation of
    # transport_udp_client.c (compiled per-target above), so the loopback
    # tests can drive a seeded lossy path via WB_NETIMPAIR. server_static
    # owns transport_udp_server.c and is NOT recompiled here, so every
    # shipping target keeps WB_ENABLE_NETIMPAIR=0; client-endpoint impairment
    # already covers both directions (impairIn + impairOut).
    WB_ENABLE_NETIMPAIR=1
    # Compile the WIRE_CORPUS_TAP calls into this target's per-target
    # transport_udp_common.c (shipping builds leave them out), and point the
    # differential test at the committed fixture directory.
    WB_WIRE_CORPUS_CAPTURE=1
    WB_WIRE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/wire"
    # Root for every scratch file a test writes, under the BUILD dir rather
    # than the source tree or /tmp: two worktrees then cannot pick the same
    # path. scratch_dir.c adds a pid and the test name beneath it and sweeps
    # its own subtree at exit.
    WB_TEST_SCRATCH_DIR="${CMAKE_BINARY_DIR}/test-scratch"
    # Committed v1 .wbv fixture directory: the reader gate loads from here and
    # the on-demand capture (re)writes spectator_v1.wbv into it.
    WB_WBV_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/wbv"
    # Committed skin fixture directory: the parity test opens
    # <dir>/basic and zips it into the working directory at run time.
    WB_SKINS_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/skins"
    # The maps the baseline runs play, and the scenario scripts that sit
    # beside them: the validator case reads one from here.
    WB_BASELINE_MAPS_DIR="${CMAKE_SOURCE_DIR}/tests/baseline/maps"
    # The repository root. unit_test_names_match_cmake reads THIS file and
    # tests/unit/test_main.c out of it and compares the two test rosters, so a
    # case added to one and not the other cannot go unnoticed.
    WB_REPO_ROOT_DIR="${CMAKE_SOURCE_DIR}"
)

target_include_directories(WinBoloUnitTests PRIVATE
    src/gui/sdl3
    src/gui/sdl3/dialogs
    # gamefront.h / brainsHandler.h / clientmutex.h for the shared
    # client-frontend cores compiled into this target.
    src/gui
    src/client_frontend
    src/server
    src
    src/winbolonet
    # Headers for the log-viewer decode subset (backend.h, logviewer.h,
    # blocks.h, snapshot.h, lv_*.h) used by test_wbv_reader.c and the
    # linked src/logviewer/*.c decode TUs.
    src/logviewer
    # braintest_loadbrowser.h for the Load Session segment-walk test (the
    # finder TU compiled above); no other braintest source is linked here.
    src/braintest
    ${CMAKE_SOURCE_DIR}/tests/unit
    ${cjson_SOURCE_DIR}
)
bolo_apply_include_rules(WinBoloUnitTests unittests)

target_link_libraries(WinBoloUnitTests PRIVATE
    bolo_static scenario_static ${SERVER_LINK_GROUP} bot_brains_static
    SDL3::SDL3 minizip lua_static cjson utf8proc
    # tileloader.c reads SVG through nanosvg and PNG through stb_image.
    nanosvg stb_image
    # mdns is header-only; linked for its include dir so test_mdns_discovery.c
    # sees mdns.h / mdns_records.h (bolo_static / server_static link it PRIVATE,
    # so the include dir doesn't propagate transitively).
    mdns
    # voice_core.c is compiled into this target for the codec round-trip test.
    Opus::opus)

if(WIN32)
    target_link_libraries(WinBoloUnitTests PRIVATE ws2_32 winmm user32)
    target_compile_options(WinBoloUnitTests PRIVATE -fcommon)
    # 8 MB stack, matching the Linux default.
    #
    # test_net_impair.c declares NetImpair as a stack LOCAL in five test
    # functions, and NetImpair is ~1.06 MB (queue[512] of 2048-byte packets,
    # net_impair.h). That overflows Windows' 1 MB default, so the runner died
    # at STATUS_STACK_OVERFLOW partway through `net_impair` -- and because the
    # test list in test_main.c is a static table walked in order, EVERY test
    # after that entry (100+ of them, including all the spectator and loopback
    # suites) silently never ran on Windows. It passed on Linux only because
    # the default stack there is already 8 MB.
    #
    # Raising the stack rather than making those locals `static` keeps each
    # test case's fixture genuinely fresh, which is the point of a stack local.
    if(MSVC)
        target_link_options(WinBoloUnitTests PRIVATE /STACK:8388608)
    else()
        target_link_options(WinBoloUnitTests PRIVATE -Wl,--stack,8388608)
    endif()
else()
    target_link_libraries(WinBoloUnitTests PRIVATE ${MATH_LIB} pthread)
    target_compile_options(WinBoloUnitTests PRIVATE -fcommon)
endif()

set_flat_output(WinBoloUnitTests "${CMAKE_BINARY_DIR}")

set(_unit_test_names
    md5_to_hex
    rand_save_restore
    pill_armour_intank_roundtrip
    base_event_classification
    transport_local_passive_threads
    sp_subscriber_delivery
    active_local_input_to_shot
    sp_shoot_through_timer
    prefs_document_roundtrip
    prefs_keys_roundtrip
    prefs_doc_roundtrip
    prefs_doc_defaults
    prefs_doc_special_chars
    prefs_doc_unknown_preserved
    prefs_doc_save_atomic
    prefs_api_roundtrip
    prefs_api_defaults
    prefs_api_corrupt_backup
    prefs_api_debounce
    prefs_api_shutdown_flush
    prefs_api_upload_excludes_local
    prefs_api_sync_dirty
    prefs_api_device_identity
    prefs_api_adopt_server
    prefs_api_mark_synced
    join_running_phase_not_lobby
    lobby_settings_codec_and_apply
    lobby_settings_scenario_apply
    lobby_team_meta_codec_and_apply
    lobby_bot_config_codec_and_apply
    lobby_bot_brain_codec_and_apply
    lobby_brain_list_codec_and_apply
    lobby_brain_docs_chunk_codec_roundtrip
    lobby_brain_docs_chunk_len_is_exact
    lobby_brain_list_clears_stale_texts
    lobby_chat_line_format_is_what_is_appended
    lobby_brain_docs_stay_out_of_the_control_snapshot
    lobby_brain_docs_reach_a_joining_subscriber
    lobby_sync_complete_codec_roundtrip
    lobby_rating_posted_codec_roundtrip
    command_codec_roundtrip_variants
    command_codec_lobby_claim_start
    command_codec_rating_posted
    command_codec_view_state
    command_codec_view_cycle
    command_codec_ping
    lobby_claim_start_host_swaps_occupied
    lobby_claim_start_host_assign_onto_own
    lobby_claim_start_host_swap_into_none
    lobby_claim_start_non_host_occupied_rejected
    lobby_claim_start_non_host_other_slot_rejected
    lobby_claim_start_self_free_and_release
    lobby_claim_start_validation
    command_codec_cmdseq_slot
    command_codec_bounds_checks
    command_rejected_parks_name_codes
    command_rejected_ignores_other_slot
    command_rejected_clear_resets_both_fields
    live_stats_kill_credits_killer_and_victim
    live_stats_suicide_counts_death_only
    live_stats_lgm_loss_splits_victim_and_killer
    live_stats_captures_credit_new_owner
    live_stats_out_of_range_slot_ignored
    live_stats_cleared_on_running_phase
    shell_death_codec_roundtrip
    shell_death_culls_matching_predicted_shell
    shell_death_rejected_culls_without_impact
    # A new bot's mode/difficulty rule (test_lobby_bot_config_memory.c): the
    # base, then the host's last MANUAL pick, remembered by key so it survives
    # a brain declaring a different list. Reads the shipped GoalHunter_1.7
    # modes.txt beside the binary and writes nothing, so these are safe under
    # `ctest -j`.
    lobby_bot_config_memory_applies
    lobby_bot_config_memory_empty_is_noop
    lobby_bot_config_memory_unknown_key
    lobby_bot_config_memory_not_honoured
    lobby_bot_config_memory_manual_only
    lobby_bot_config_memory_new_lobby
    lobby_add_bot_rejects_empty_brain_path
    lobby_add_bot_rejects_ai_none
    lobby_add_bot_rejects_not_in_lobby
    lobby_add_bot_rejects_non_host_sender
    command_queue_first_submit_drains
    command_queue_second_submit_does_not_drain
    command_queue_ack_drains_pending_tail
    command_queue_ack_clearing_queue_skips_drain
    vote_surrender_rejects_unassigned_caller
    vote_surrender_starts_for_real_team_caller
    vote_surrender_rejects_three_active_teams
    vote_surrender_state_withheld_from_other_team
    vote_surrender_newswire_hidden_from_other_team
    vote_surrender_newswire_shown_to_own_team
    vote_back_to_lobby_newswire_stays_public
    vote_surrender_may_answer_rule
    base_win_ends_round_immediately
    base_win_dead_base_blocks_the_win
    base_win_fires_when_dead_base_regenerates
    base_win_quitonwin_with_lobby_returns_to_lobby
    base_win_nolobby_quitonwin_is_instant
    base_win_reports_the_winner
    manual_vote_countdown_survives_lost_base
    surrender_credits_the_opposing_team
    win_during_manual_countdown_resolves_as_vote
    abandoned_round_reports_nothing
    round_stats_zeroed_on_fresh_sim
    round_stats_kill_basic
    round_stats_drown_not_suicide
    round_stats_suicide_shell
    round_stats_mine_death
    round_stats_trees_wasted_and_pills_dropped
    round_stats_capture_steal
    round_stats_base_capture_steal
    round_stats_lgm
    round_stats_lifecycle_reset
    round_stats_reset_clears_notables
    round_stats_notables_ordered
    round_stats_leaver_dropped
    round_stats_attribution_callbacks
    round_stats_direct_damage
    round_stats_mine_owner_api
    round_stats_mine_damage
    round_stats_leaver_clears_mines
    mine_kill_publishes_event
    mine_kill_on_boat_publishes_event
    mine_kill_own_mine_names_self
    mine_blast_range
    awards_basic_winners
    awards_tiebreak
    awards_omission
    awards_floors
    awards_include_bots
    awards_subset_basic
    awards_subset_deterministic
    round_stats_codec_roundtrip
    round_stats_codec_worstcase
    round_stats_codec_rejects_bad_key
    round_stats_build_summary
    round_stats_summary_highlights
    round_stats_client_ingest
    round_stats_track_records
    round_stats_track_cap
    round_stats_derive_equivalence
    highlights_cluster_wipe
    highlights_wipe_team_bonus
    highlights_objective_steal
    highlights_award_anchor
    highlights_selection
    highlights_lead_in
    highlights_empty
    territory_shift_basic
    territory_shift_bounded
    highlights_turning_point
    territory_recent_damage
    highlights_front_collapse
    highlights_award_anchor_density
    highlights_time_spread
    highlights_multi_lgm
    highlights_fumble
    highlights_rare_death
    highlights_award_dedup
    highlights_pickup_spree
    highlights_action_density
    lv_stats_clip_time_format
    attribution_track_schema
    vote_toggle_standalone_no_does_nothing
    vote_toggle_invalid_mode_dropped
    vote_toggle_yes_opens_vote
    vote_toggle_no_during_running_records_answered
    lobby_set_team_clamps_max_tanks
    lobby_set_team_accepts_max_legal
    lobby_set_team_accepts_unassigned
    lobby_set_team_clamps_above_max
    ranked_flag_persists_with_one_player
    ranked_shape_gate
    lobby_lock_bit_lookup
    lobby_lock_rejects_settings
    lobby_lock_mask_roundtrip
    view_policy_defaults
    view_policy_apply_validates
    view_policy_lock_bits
    view_policy_lobby_reset
    classic_mode_defaults
    classic_mode_forces_views
    classic_mode_lock_bit
    classic_mode_lock_implied
    classic_mode_lock_blocks_dispatch
    classic_mode_lobby_reset
    allies_in_trees_defaults
    allies_in_trees_classic_mode
    fog_settings_defaults
    fog_settings_classic_mode_preset
    fog_settings_lock_implied
    fog_settings_lobby_edits
    fog_settings_lobby_reset
    info_packet_view_policy_layout
    info_packet_view_policy_length_tier
    visibility_preset_round_trip
    visibility_preset_custom
    visibility_preset_ignores_decay
    visibility_preset_pref_words
    info_packet_preset_round_trip
    info_packet_absent_views_read_classic
    upload_cap_enforced
    map_field_clamps_evil
    map_field_clamps_passthrough
    map_field_clamps_angry_start
    map_field_clamps_no_sim
    map_reload_rollback
    map_read_memory_matches_file
    map_read_memory_handbuilt
    map_convert_file_to_compressed
    map_read_memory_rejects_garbage
    map_read_clears_mine_under_pill
    upload_busy_predicate
    upload_lost_ack_retry
    upload_timeout_releases_other_player
    upload_lost_done_retry
    upload_partial_timeout_retry
    upload_filename_safe
    lobby_bot_name_rejects_reserved_prefix
    lobby_bot_name_rejects_disallowed_char
    lobby_bot_name_rejects_collision
    lobby_bot_name_skip_does_not_bypass_validator_prefix
    lobby_bot_name_skip_does_not_bypass_validator_control
    lobby_bot_name_skip_excludes_self_from_uniqueness
    lobby_bot_name_accepts_clean_unique
    lobby_map_list_chunked
    lobby_map_list_scripted_golden
    lobby_map_list_scripted_mixed
    lobby_reload_scenario_needs_host
    lobby_reload_scenario_needs_lobby
    lobby_reload_scenario_no_scenario
    lobby_reload_scenario_calls_back
    lobby_reload_scenario_cooldown
    lobby_set_scenario_selects
    lobby_set_scenario_none
    lobby_set_scenario_refuses_unknown
    lobby_set_scenario_refuses_shape
    lobby_set_scenario_refuses_bound
    lobby_set_scenario_cooldown
    lobby_set_scenario_unreadies
    lobby_map_search_chunked
    wbn_bearer_state
    wbn_rekey_codec
    wbn_news_parse
    wbn_news_free_during_fetch
    wbn_country_cache
    wbn_prefs_parse_get
    wbn_prefs_parse_updatedat
    wbn_prefs_decide
    wbn_prefs_build_put_body
    wbn_serverlist_parse
    wbn_serverlist_players
    wbn_serverlist_motd
    wbn_serverlist_malformed
    wbn_map_parse
    brain_crash_log_writes_file
    brain_crash_log_falls_back_to_luaptr
    brain_inbox_push_peek_fifo
    brain_inbox_overflow_drops_oldest
    brain_inbox_legacy_drain_fifo
    brain_inbox_clear_resets
    brain_list_scan_path_resolves
    brain_list_texts_read
    unit_test_names_match_cmake
    brain_modes_manifest_parses
    brain_modes_missing_falls_back
    brain_modes_malformed_lines_skipped
    brain_modes_counts_clamped
    brain_view_data_edge_rect
    bot_init_arg_difficulty_token
    bot_init_arg_mode_tokens
    bot_difficulty_names
    bolo_rand_golden_sequence
    pf_dijkstra_no_solid_corner_cut
    pf_astar_no_solid_corner_cut
    pf_costto_no_solid_corner_cut
    pf_tail_reaches_radius_and_stops
    pf_tail_blocked_by_wall_and_sea
    pf_tail_neutral_zone_slows
    pf_tail_meeting_cancels_tie_to_hostile
    pf_tail_never_overwrites_a_stamp
    pf_tail_deep_margin_keeps_off_the_shore
    pf_tail_contact_makes_a_front_line
    loadbrowser_segment_walks_parts
    loadbrowser_segment_crosses_blocks
    loadbrowser_segment_no_neighbour
    loadbrowser_segment_path_forms
    loadbrowser_segment_labels
    loadbrowser_rename_splits_names
    loadbrowser_rename_family_plan
    loadbrowser_rename_rejections
    loadbrowser_rename_applies
    countdown_abort_publishes_phase
    lobby_auto_unready_clears_humans_keeps_bots
    mapdir_root_fallback
    bot_chat_send_to_human_lands_in_human_inbox
    bot_chat_send_to_other_bot_lands_in_recipient_inbox
    bot_chat_receive_from_human_lands_in_bot_inbox
    bot_chat_receive_from_other_bot_via_broadcast
    bot_chat_enemy_broadcast_is_not_an_order
    bot_chat_enemy_unicast_still_lands
    bot_chat_mid_round_bot_reads_server_alliances
    bot_ping_queue_becomes_cmd_ping_from_bot_slot
    bot_ping_rate_limit_drops_extras
    bot_ping_ignores_bad_slot
    bot_ping_ignores_a_slot_that_is_not_a_bot
    internal_msg_reaches_allied_bot
    internal_msg_skips_sender_self
    internal_msg_skips_non_allied_bot
    internal_msg_skips_inactive_bot_slot
    internal_msg_handles_oversized_body
    internal_msg_null_inputs_are_noop
    alliance_request_flags_addressed
    alliance_request_ignores_other_slot
    phase_lobby_sets_inlobby
    phase_gameover_resets_in_game
    player_join_appends_lobby_chat
    player_join_self_does_not_announce
    player_leave_appends_lobby_chat
    lobby_settings_clears_balance_proposal
    reset_round_state_preserves_identity
    start_game_preserves_client_flags
    return_to_lobby_drops_wbn_keeps_identity
    wbn_lobby_update_deferred_during_rotation
    wbn_lobby_update_sends_when_not_rotating
    wbn_join_keyed_on_reauth
    wbn_join_anonymous_on_grace
    wbn_join_idempotent_reauth_no_double
    wbn_join_disconnect_drops
    wbn_join_rearm_per_session
    wbn_join_rekey_target_gate
    remove_player_clears_slot
    return_to_lobby_clears_phantom_slot
    last_human_leave_returns_to_lobby
    humanless_round_does_not_autoend
    host_departs_promotes_lowest_human
    nonhost_departs_keeps_host
    host_reassign_skips_bots
    lobby_reset_clears_host_slot
    transfer_host_promotes_target
    transfer_host_rejects_non_host_sender
    transfer_host_openhost_does_not_grant
    transfer_host_rejects_self
    transfer_host_rejects_unconnected
    transfer_host_rejects_bot_target
    firstjoinhost_promotes_over_bot
    firstjoinhost_off_leaves_host_alone
    firstjoinhost_second_joiner_does_not_displace
    log_roundtrip_basic
    log_roundtrip_snapshot_keeps_chain_synced
    log_roundtrip_lobby_snapshot_is_empty_world
    log_roundtrip_lobby_mode_drops_world_events
    log_roundtrip_ping
    log_roundtrip_snapshot_tank_stocks
    log_roundtrip_tank_stock_record
    replay_roundtrip_world
    replay_roundtrip_base_migrate
    replay_roundtrip_base_damage
    replay_roundtrip_base_capture_stock
    replay_roundtrip_pill_health
    replay_v2_pill_health_nibble
    replay_v1_pill_health_nibble
    brainrec_version_rejects_old
    lv_tank_stocks_from_snapshot
    lv_tank_stocks_snapshot_without_tail
    lv_tank_stocks_from_record
    lv_hostile_item_counts
    lv_hostile_entity_remove_index
    lv_hostile_server_text_slot
    lv_hostile_modifiers_length
    wbv_reader_v1
    wbv_reader_v2
    attribution_reader_roundtrip
    attribution_reader_rejects_bad
    attribution_reader_old_wbv
    attribution_reader_clamps_slotcount
    lv_walk_base_anchor_lobby_log
    lv_walk_base_anchor_opening_snapshot
    lv_walk_base_anchor_ordinal
    lv_hide_lobby_window_mapping
    lv_hide_lobby_no_lobby_fallback
    lv_hide_lobby_no_marker_anchor
    lv_hide_lobby_periodic_snapshot
    lv_logged_name_from_join_event
    lv_game_settings_from_walk
    lv_game_settings_absent
    blocks_stream
    stream_load
    stream_pump
    spec_seed_load
    spec_seed_load_real
    spec_record_pump
    spec_seed_decode_info
    log_path_empty_uses_autobase
    log_path_explicit_file_verbatim
    log_path_explicit_file_keeps_single_wbv
    log_path_directory_autonames_inside
    log_path_directory_trailing_slash_no_double
    log_path_directory_arg_appends_wbv
    alliance_reset_codec_roundtrip
    alliance_reset_decoder_rejects_short
    alliance_reset_reapply_publishes_one_event
    alliance_reset_apply_rebuilds_alliances
    alliance_reset_bots_synced_after_inplace_start
    alliance_reset_bots_synced_after_countdown_start
    alliance_reset_apply_keeps_owners
    alliance_reset_changed_matrix_keeps_owners
    treegrow_never_plants_on_deep_sea
    treegrow_reset_clears_stale_target
    pill_repair_tops_up_from_arrival_armour
    pill_repair_short_load_spends_what_it_has
    pill_repair_full_load_covers_a_dead_pill
    entity_removed_pill_is_gone_from_gameplay
    entity_removed_base_is_gone_from_gameplay
    entity_removed_start_is_never_chosen
    entity_tournament_removed_start_is_never_chosen
    entity_tournament_neutral_share_counts_live_bases
    entity_add_reuses_lowest_removed_slot
    entity_add_refused_when_full
    entity_remove_refuses_already_removed
    entity_remove_keeps_indices_above
    entity_blob_load_marks_every_item_live
    entity_event_codec_roundtrip
    entity_event_client_adds_at_fresh_index
    entity_event_client_add_reuses_removed_slot
    entity_event_client_remove_keeps_the_slot
    entity_event_client_add_lands_on_the_server_index
    entity_event_removed_index_sends_no_delta
    entity_event_wire_corpus_fixture
    entity_event_client_refuses_out_of_range_index
    entity_sync_codec_roundtrip
    entity_sync_client_clears_the_holes
    entity_sync_client_restores_the_live
    entity_sync_mask_above_the_count_ignored
    entity_sync_loopback_join
    entity_sync_wire_corpus_fixture
    entity_record_round_trip_per_kind
    entity_record_viewer_across_a_snapshot
    entity_record_removed_and_restored_pill_replays
    entity_record_brain_rect_skips_a_removed_pill
    entity_record_lobby_add_republishes_counts
    entity_record_lobby_removal_replays
    entity_record_seek_lands_on_the_right_liveness
    starts_pick_cluster_nearest_teammate
    starts_pick_farthest_when_solo
    starts_pick_none_when_all_taken
    starts_batch_reserved_lands_exact
    starts_batch_unreserved_avoids_reserved
    starts_batch_stale_reservation_falls_through
    starts_batch_duplicate_honors_first
    starts_batch_null_reservations_place_normally
    starts_batch_solo_random_seed
    starts_batch_teams_cluster_and_separate
    starts_batch_team_anchor_jitter_varies
    starts_open_ideal_friendly_pill_eligible
    starts_open_ideal_removed_pill_ignored
    starts_open_removed_start_never_chosen
    start_side_mask_sectors
    start_side_mask_degenerate_bbox
    start_side_eligible_closed_mask
    lobby_side_axis_two_team_pair
    lobby_side_axis_pair_classification
    lobby_side_axis_click_targets
    lobby_start_shared_prefix_len
    lobby_start_shared_prefix_label
    lobby_start_shared_name_label
    lobby_start_shared_owner_fold
    starts_scatter_avoids_existing_tanks
    starts_scatter_falls_back_when_crowded
    starts_side_team_stays_on_its_side
    starts_side_overflow_rides_own_side
    starts_side_never_crosses_when_opposite_free
    starts_side_any_matches_legacy
    starts_side_reservation_beats_side
    starts_side_empty_side_falls_back
    starts_side_quota_capped_by_eligible
    starts_side_any_team_kept_off_chosen_side
    lobby_team_side_clamps_and_rejects_non_host
    lobby_side_change_repicks_everyone
    lobby_unassign_and_team_side_claims
    lobby_map_change_releases_off_side
    lobby_non_host_off_side_claim_rejected
    lobby_any_team_claim_kept_off_chosen_side
    starts_side_end_to_end_four_v_twelve
    input_redundancy
    bases_closest_for_player
    base_stock_visibility
    base_half_tick_per_sim_sequence
    base_half_tick_other_sim_does_not_disturb
    base_armour_fog_of_war
    base_armour_reveal_in_range
    base_death_prediction_replay_tick
    base_death_prediction_authority
    tank_alliance_unfielded_shows_no_tank
    tank_alliance_unfielded_status_tile
    tank_damage_exact_armour_survives
    tank_damage_overkill_destroys
    tank_damage_partial_survives
    tank_destroyed_snapshot_round_trip
    tank_status_wire_bits
    tank_status_wire_start_find_round_trip
    two_clients_full_sync_independent
    voice_flags_snapshot_masking
    voice_talking_codec
    sim_rules_codec_roundtrip
    sim_rules_codec_golden
    voice_talking_stops_in_countdown
    voice_talking_clears_on_leave
    fx_viewport_cull
    viewport_floor
    sound_event_codec
    sound_delivery_builder
    sound_payload_shape
    view_rects_default_baseline
    view_rects_always_base_ally
    view_rects_off
    view_rects_decay
    view_rects_dead_player
    view_pill_pos_current
    view_pill_pos_reveal
    view_pill_update_event_fogged
    view_pill_pos_bot_advantage
    tree_hide_enemy
    tree_hide_ally_option
    lgm_visibility_tank_in_trees
    lgm_visibility_tank_visible
    view_state_key_grants_rect
    view_state_bad_claims_degrade
    view_state_invalidation
    view_state_lifecycle
    view_cycle_pick_order
    view_cycle_pick_policy
    view_cycle_pick_direction
    view_target_codec
    view_target_apply
    view_cycle_bases
    view_cycle_base_direction
    view_cycle_allies
    view_cycle_ally_direction
    view_cycle_exits
    view_cycle_decay_skip
    view_cycle_eligible_mask
    view_cycle_ally_stub
    view_cycle_stale_pill
    pill_pos_current_lookups
    pill_pos_current_num_split
    pill_pos_current_duplicate_square
    pill_pos_current_snapshot_apply
    pill_pos_current_server_unchanged
    pill_pos_current_moved_state
    pill_pos_current_carry_cycle
    viewport_calc_square_pure
    viewport_calc_pill_square_moved
    overview_regions
    overview_reveal
    overview_freeze_no_leak
    overview_pill_capture
    overview_reset
    overview_decay_mirror
    overview_decay_view_exit
    overview_policy_baseline
    overview_policy_pills_and_allies
    overview_policy_categories
    overview_policy_decay
    overview_loopback
    overview_camera
    overview_scroll
    overview_fog
    overview_hud_layout
    sight
    sight_shadow
    sight_shadow_cost
    sight_shadow_overflow
    overview_dead_tank
    overview_entities
    overview_removed_item_has_no_region
    overview_gunsight
    tank_pos_dead
    overview_snapshot_mirror
    overview_snapshot_isolation
    overview_snapshot_filter
    overview_snapshot_generation
    overview_snapshot_removed_item_has_no_label
    mapview_sprite_classic
    mapview_sprite_ladder
    mapview_sprite_shell_tip
    mapview_sprite_lgm_snap
    mapview_overlay_gunsight
    mapview_overlay_tank_label
    mapview_overlay_cursor
    mapview_overlay_item_labels
    stall_advances_processed_tick
    stall_mine_late_lays_once
    stall_mine_duplicate_not_relaid
    stall_fire_not_harvested
    stall_never_fires
    stall_brief_trough_no_advance
    stall_long_dry_advances
    input_catchup
    catchup_ignores_redundant_duplicates
    build_harvest_stale
    build_harvest_valid
    build_harvest_busy_queues
    lgm_request_valid
    lgm_request_quiet
    lgm_quit_drops_carried_pill
    lgm_unfield_drops_carried_pill
    jitter_buffer_grow
    shell_projection
    sim_rules_classic_defaults
    sim_rules_validate_ranges
    sim_rules_copies_follow
    sim_rules_terrain_caps_follow
    sim_rules_river_cap_moves_drowning
    sim_rules_base_regen_seed_follows
    sim_rules_terrain_life_follows
    sim_rules_base_empties_without_wrapping
    sim_rules_pill_empties_without_wrapping
    sim_rules_pairs
    sim_rules_capture_threshold_moves
    sim_rules_builder_cost_follows
    sim_rules_are_classic
    sim_rules_obs_refuses_non_classic
    sim_rules_shell_flight_follows
    sim_rules_brain_shot_follows
    sim_rules_worldsim_pill_follows
    sim_rules_boat_speed_follows
    sim_rules_obs_reload_follows
    screen_bullets_build
    ping_smoother
    client_timing
    interp_render
    interp_respawn_no_death_flash
    snapshot_compaction
    edge_send_predicate
    error_smoothing
    shells_survive_cleared_lgm_slot
    ping_kick_defers_teardown
    ping_kick_clears_strikes_on_disconnect
    tkexp_lgm_pairing
    loopback_join
    loopback_join_loss
    loopback_lobby_running_loss
    loopback_quiet_lobby_control_loss
    loopback_command_liveness
    loopback_channel
    player_mute_mask_roundtrip
    lock_channel
    loopback_map_preview
    loopback_map_upload
    loopback_download_join
    loopback_download_midgame
    loopback_resync
    loopback_map_change_clean
    loopback_map_change_loss
    loopback_join_accept_loss
    loopback_join_accept_loss_midgame
    gate1_viewtick_loopback
    gate1_host_noop
    interp_jitter_e2e
    join_version_gate
    join_rate_limit
    cookie_handshake
    map_amp_gate
    map_reask_throttle
    map_resync_cut_and_deliver_once
    map_resync_duplicate_request_no_recut
    map_resync_send_gate_holds
    map_resync_stale_gen_rejected
    map_compress_roundtrip_stock
    map_compress_capacity_refuses
    map_compress_incompressible
    map_compress_roundtrip_mutated
    map_compress_rejects_null_handles
    map_checksum_ignores_mines
    map_resync_base_crater_converges
    map_pill_mine_cleared_on_load
    map_carried_pill_keeps_terrain
    map_shadow_tracks_real
    map_shadow_crc_matches
    map_shadow_blob_identical
    map_shadow_seed_lifecycle
    map_shadow_cull_withholds
    map_shadow_sweep_converges
    map_shadow_sweep_bounds
    map_shadow_round_start_capture
    map_shadow_join_seeds_round_start
    map_shadow_round_start_fallback
    pill_shadow_tracks_real
    pill_shadow_crc_matches
    pill_shadow_blob_identical
    pill_shadow_fullsync_move_matches
    pill_shadow_withheld_crc_matches
    loopback_map_cull
    loopback_map_cull_resync
    sound_delivery_wire_cull
    sound_tier_playback
    view_ally_loopback
    resync_finalize_corrupt_keeps_gen
    resync_finalize_valid_advances_gen
    install_compressed_map_rejects_garbage
    resync_debounce_threshold
    conn_migration_rehome
    conn_migration_e2e
    viewtick_rewind
    viewtick_displayed_tick
    wire_corpus
    channel_mux
    spectator_ring
    spectator_ring_nolog
    spectator_replay
    spectator_config
    spectator_join
    spectator_roster_publish
    spectator_slot_codec
    spectator_chat_codec
    stats_seed_codec
    spectator_chat_routing
    spectator_chat_log
    spectator_chat_catchup
    spectator_connect
    spectator_client_lobby_intake
    spectator_command_reject
    spectator_lobby_readonly
    spectator_seed_integration
    spectator_capture
    spectator_log
    spectator_seed
    spectator_lobby_subscribe
    spectator_control_filter
    spectator_subscriber_capacity
    spectator_roster_to_spectators
    spectator_lobby_cutover
    loopback_spectator_lobby
    bulk_transfer
    round_log_header_roundtrip
    round_log_sink_rejection_realigns
    round_log_refused_when_unavailable
    overflow_guards
    channel_reset_codec_roundtrip
    channel_reset_two_slot_baselines
    packet_type_names
    ping_binding_encode_decode
    ping_binding_match
    ping_binding_direct
    ping_binding_format
    ping_pie_slices
    ping_edge_sides
    ping_edge_corner
    ping_edge_size_from_distance
    ping_edge_name_anchor
    ping_rect_inset
    ping_event_wire
    ping_sound_fallback
    ping_name_truncate
    ping_dispatch_accepts_and_builds_event
    ping_dispatch_rejects_lobby
    ping_dispatch_rejects_empty_slot_and_out_of_range
    ping_dispatch_map_range_bound
    ping_dispatch_rejects_bad_kind
    ping_dispatch_rate_limit
    ping_dispatch_spam_30s_window
    ping_dispatch_new_round_clears_rate_limit
    ping_dispatch_smart_pings_off
    ping_reaches_team_only
    ping_mute_relay_skip
    ping_mute_client_mirror_cleared_on_leave
    ping_sender_name_empty_for_unused_slot
    ping_world_marker_alpha
    ring_anim_at
    game_settings_blob
    ping_network
    lang_name_table
    screencalc_river_road_counts_as_water
    screencalc_river_arms_of_road_centred_cross
    screencalc_river_copies_agree
    mdns_discovery
    mdns_view_txt_roundtrip
    client_type_matches_platform
    client_type_name_round_trips
    map_load_clamps_base_fields
    map_load_clamps_pill_fields
    map_load_clamps_start_dir
    client_slot_reassign_carries_tank_and_lgm
    client_slot_reassign_same_slot_is_stable
    client_slot_reassign_back_to_zero
    players_oob_index_safe
    control_oob_player_dropped
    input_gate_taxonomy
    key_claims
    addrparse_host_only
    addrparse_host_port
    addrparse_scheme
    addrparse_trailing_path
    addrparse_whitespace
    addrparse_port_bounds
    addrparse_bad_port
    addrparse_empty
    bot_pool_builtin_defaults
    bot_pool_install_and_clamp
    bot_pool_install_dedup_and_drop
    bot_pool_reset_restores_builtin
    bot_pool_pick_unique_and_overflow
    bot_pool_json_load_roundtrip
    bot_pool_json_missing_file_keeps_active
    bot_pool_wire_roundtrip
    bot_pool_wire_builtin_roundtrip
    bot_pool_wire_rejects_garbage
    proxy_meta_parse_roundtrip
    proxy_meta_parse_truncation_safe
    proxy_meta_parse_oversized_name
    proxy_meta_parse_prefs_clamps
    tick_core_active_local_no_double_step
    tick_core_passive_local_pumps_keys_half
    tick_core_lobby_preserves_parity
    transport_ticks_server_lifecycle
    await_join_connected_immediate
    await_join_lobby_latch
    await_join_timeout_and_error
    md5_rfc1321_vectors
    md5_block_boundaries
    md5_streaming_matches_oneshot
    net_impair
    lobby_time_minutes_valid
    control_overflow_defers_disconnect
    join_collision_verified_rejects_will_auth
    join_collision_verified_flag_irrelevant
    join_collision_unverified_will_auth_admits
    join_collision_unverified_no_auth_rejects
    claim_resolve_free_promotes
    claim_resolve_free_ignores_holder_flag
    claim_resolve_unverified_preempts
    claim_resolve_verified_keeps_temp
    base_timer_cleared_on_leave
    base_timer_not_inherited_next_round
    autolock_locks_on_start_inplace
    autolock_locks_on_start_wire
    autolock_join_blocked_after_start
    autolock_lobby_stays_open
    autolock_released_on_return_to_lobby
    no_autolock_stays_open_on_start
    maprotate_restarts_round_and_rearms
    maprotate_boot_does_not_rotate_while_empty
    maprotate_gameover_is_not_terminal
    maprotate_vote_return_is_not_terminal
    maprotate_defers_wbn_update_until_key_rotated
    game_vote_state_codec_surrender_passed
    game_vote_state_codec_back_to_lobby
    game_vote_state_decoder_rejects_short
    server_text_codec_via_chat_wire
    vote_surrender_blocked_without_lobby
    vote_back_to_lobby_blocked_without_lobby
    lobby_add_bot_rejects_when_at_maxbots
    lobby_bot_count_counts_only_connected_bots
    lobby_maxbots_cap_boundary
    empty_return_to_lobby_removes_bots_and_unlocks
    bot_pool_wire_chunk_transport
    voice_core_roundtrip
    voice_device_resolve
    voice_peak
    voice_segment_roundtrip
    voice_segment_rejects_malformed
    voice_jitter_ordering_and_plc
    voice_jitter_undecodable_run
    voice_jitter_under_loss
    voice_flood_cap_enforced
    voice_talker_select_ranks_recent
    voice_talker_select_bounds
    active_sim_armed_on_lobby_tick
    active_sim_cleared_on_cross_thread_destroy
    skin_source_dir_and_zip
    skin_active_vs_requested
    skin_workshop_id_roundtrip
    skin_density_scan
    sheet_bleed_edges
    sheet_no_key_under_alpha
    bmp_sheet_no_key_under_alpha
    sprite_atlas_isolated
    sprite_atlas_lookup
    sprite_atlas_packed_sheet_unsafe
    sound_variant_pool_names
    sound_variant_load_compaction
    pascal_string_lengths
    pascal_string_viewer_copy_agrees
    pascal_string_roundtrip
    console_lock_unlock
    console_info_and_status
    console_savemap_path
    console_unknown_command_is_inert
    console_say_keeps_case
    console_kick_and_host
    console_kick_host_without_newline
    console_read_reports_eof
    console_read_timeout_then_eof
    wbv_fixture_summaries
    tank_mod_speed_caps_on_road
    tank_mod_speed_river_still_moves
    tank_mod_accel_doubles_ticks_to_cap
    tank_mod_accel_doubles_ticks_to_brake
    tank_mod_accel_halves_autoslow
    tank_mod_turn_halves_circle_ticks
    tank_mod_reload_fires_twice_as_often
    tank_mod_dealt_kills_in_half_the_hits
    tank_mod_taken_takes_more_hits
    tank_mod_mine_damage_scales_with_layer
    mine_damage_fatal_reduction
    tank_mod_neutral_owner_deals_classic
    tank_mod_boat_exit_at_half_speed
    tank_mod_pill_leads_half_speed_boat
    tank_mod_predicted_stop_matches_engine
    tank_modifiers_op_writes_set
    tank_modifiers_op_refusals
    tank_modifiers_wire_roundtrip
    tank_modifiers_survive_death
    tank_modifiers_cleared_at_create
    scenario_op_unknown_type_unsupported
    scenario_op_every_type_unsupported
    scenario_policy_register_replace_clear
    scenario_op_refused_in_policy
    scenario_op_refused_in_nested_policy
    scenario_op_refused_during_start
    scenario_tick_called_both_branches
    scenario_start_flag_set_during_start
    scenario_start_guard_blocks_reentry
    scenario_start_flag_cleared_after_start
    scenario_round_start_called_both_starts
    scenario_setup_window_admits_ops
    scenario_setup_window_roster_refused
    scenario_setup_window_shut_refuses_all
    scenario_setup_window_no_callback
    scenario_setup_window_holds_publish
    scenario_round_boot_before_tanks
    scenario_round_boot_publishes_rules_once
    scenario_setup_events_off_the_wire
    scenario_setup_terrain_raises_map_events
    scenario_setup_terrain_reaches_shadow
    scenario_round_start_clears_seat_holders
    scenario_read_roster_slot
    scenario_read_pill_info
    scenario_read_base_info
    scenario_read_start_info
    scenario_read_tank_info
    scenario_read_builder_info
    scenario_read_terrain_buffer
    scenario_read_num_fielded
    scenario_tank_set_stocks
    scenario_tank_kill
    scenario_tank_teleport
    scenario_tank_set_boat
    scenario_tank_give_pill
    scenario_tank_drop_pill
    scenario_tank_arm_records
    scenario_tank_death_ticks_respawn
    scenario_lgm_dispatch
    scenario_lgm_recall
    scenario_lgm_kill
    scenario_lgm_parachute
    scenario_lgm_set_carried
    scenario_lgm_kill_record
    scenario_pill_set_owner
    scenario_pill_set_armour
    scenario_pill_set_speed
    scenario_pill_move
    scenario_base_set_owner
    scenario_base_owner_keep_stock
    scenario_base_set_stock
    scenario_pill_base_arm_records
    scenario_entity_add_pill
    scenario_entity_remove_pill
    scenario_removed_item_is_no_item
    scenario_entity_add_base
    scenario_entity_remove_base
    scenario_entity_add_start
    scenario_entity_remove_start
    scenario_entity_publish
    scenario_entity_add_out_null
    loopback_entity_change
    loopback_scenario_seat_keeps_pills
    pill_armour_scale_classic_cap
    pill_armour_scale_raised_cap
    pill_armour_scale_client_caps
    sim_rules_client_check_every_carried_field
    sim_rules_client_check_nan_and_inf
    sim_rules_client_check_attack_pair
    sim_rules_client_check_server_only_pair
    sim_rules_client_check_valid_applies
    loopback_sim_rules_change
    loopback_sim_rules_join
    loopback_sim_rules_reclamp
    loopback_sim_rules_agree
    scenario_map_set_tile
    scenario_map_fill_rect
    scenario_map_fill_paced
    scenario_map_fill_no_budget_refused
    scenario_map_fill_dropped_at_round_start
    scenario_map_fill_dropped_at_map_swap
    scenario_map_fill_respects_event_buffer
    scenario_map_place_mine
    scenario_map_remove_mine
    scenario_map_arm_records
    scenario_roster_spawn_refusals
    scenario_roster_spawn_lands
    scenario_roster_spawn_paced
    scenario_roster_team_during_add
    scenario_roster_spawn_allies_team
    scenario_roster_spawn_ally_is_quiet
    scenario_roster_spawn_no_team_allies_nothing
    scenario_roster_remove_bot
    scenario_roster_set_team
    scenario_lobby_add_bot
    scenario_lobby_remove_bot
    scenario_lobby_set_team
    scenario_roster_bots_seat_past_the_human_cap
    scenario_roster_spawn_named_start
    scenario_roster_spawn_loadout_named
    scenario_roster_spawn_loadout_without_policy
    scenario_roster_spawn_loadout_not_next_life
    lobby_template_map_commit_seats
    lobby_template_return_reconciles
    lobby_template_return_unfields
    lobby_template_reset_reseats
    lobby_template_preview_is_inert
    lobby_template_plain_map_clears
    lobby_template_caps_humans_only
    lobby_template_cap_seats_human_above_bots
    lobby_template_cap_refuses_extra_human
    lobby_template_cap_never_binds_bots
    lobby_template_cancel_keeps_trim
    lobby_template_cancel_keeps_empty_team
    lobby_template_cancel_chain_rolls_back
    lobby_template_commit_keeps_new_lobby
    lobby_template_cancel_restores_path_inmem
    lobby_template_cancel_restores_path_random
    lobby_template_seat_carries_init
    scripted_game_type_loadout_follows_base
    scripted_game_type_no_base_is_strict
    scripted_game_type_strict_ignores_base
    scripted_game_type_start_follows_base
    scripted_game_type_plain_map_clears_base
    scripted_game_type_client_follows_settings
    lobby_scenario_settings_plain_bytes
    lobby_scenario_settings_scripted_bytes
    lobby_scenario_settings_roundtrip
    lobby_scenario_commit_sets_type
    lobby_scenario_reset_keeps_rules
    lobby_scenario_refuses_ranked
    lobby_scenario_refuses_ai_none
    lobby_scenario_refuses_game_type
    lobby_scenario_boot_sets_type
    lobby_scenario_identity_strips_controls
    lobby_scenario_nolobby_boot_seats_template
    unfielded_seat_counts
    unfielded_seat_is_empty
    unfielded_seat_all_ready
    unfielded_seat_start_skips_it
    unfielded_seat_spawn_fields_it
    unfielded_seat_survives_remove
    unfielded_seat_unfield_is_quiet
    unfielded_seat_unfield_keeps_sync
    unfielded_seat_spawn_keeps_alliances
    unfielded_seat_on_the_wire
    scenario_wave_cost_refield_resumes
    scenario_wave_cost_seats_swap_counts
    scenario_wave_cost_other_brain_rebuilds
    scenario_wave_cost_brain_case_rebuilds
    scenario_wave_cost_other_init_rebuilds
    scenario_wave_cost_round_end_releases
    scenario_wave_cost_all_parked_releases
    scenario_wave_cost_rotation_releases
    scenario_wave_cost_seat_leaving_releases
    scenario_wave_cost_destroy_releases
    scenario_wave_cost_countdown_warms_seats
    scenario_wave_cost_warm_is_one_a_frame
    scenario_wave_cost_warmed_field_is_free
    scenario_wave_cost_warm_skips_bad_brain
    scenario_wave_cost_failed_build_leaves_nothing
    scenario_wave_cost_abort_countdown_releases
    scenario_wave_cost_warmed_init_rebuilds
    scenario_wave_cost_template_init_warms
    scenario_wave_cost_all_ready_clears_skips
    scenario_comms_msg_all
    scenario_comms_msg_team
    scenario_comms_msg_player
    scenario_comms_sound
    scenario_comms_log
    scenario_comms_say
    three_shot_order_orders_allied_bots
    three_shot_order_ignores_closed_ground
    three_shot_order_needs_one_square
    three_shot_order_window_and_reset
    three_shot_order_needs_quiet_before
    three_shot_order_quiet_counts_hits
    three_shot_order_needs_quiet_after
    three_shot_order_quiet_after_sees_a_shell_still_flying
    three_shot_order_quiet_before_sees_a_shell_still_flying
    three_shot_order_uses_the_server_tick
    three_shot_order_server_tick_quiet_after
    three_shot_order_ignores_a_forged_fire_tick
    three_shot_order_rejects_off_map_squares
    three_shot_order_scenario_refuses_off_map
    scenario_comms_arm_records
    scenario_comms_decoder_dest_player
    scenario_comms_apply_non_zero_slot
    scenario_flow_end_round
    scenario_flow_end_round_resolve
    scenario_flow_end_round_refusals
    scenario_flow_set_game_time
    scenario_flow_set_game_time_refusals
    scenario_flow_arm_records
    scenario_rule_index_matches_table
    scenario_rule_set
    scenario_rule_set_float
    scenario_rule_refusals
    scenario_rule_arm_records
    scenario_rule_clamps_world
    scenario_rule_clamp_records
    scenario_fill_caps_raises_bases
    scenario_fill_caps_raises_pills
    scenario_fill_caps_off_changes_nothing
    scenario_fill_caps_records
    scenario_policy_choose_start
    scenario_policy_choose_start_out_of_range
    scenario_policy_allow_base_win
    scenario_policy_allow_extra_teams
    scenario_policy_team_set_extra_teams
    scenario_policy_max_players
    scenario_policy_spawn_loadout
    scenario_policy_can_respawn
    scenario_policy_null_is_classic
    scenario_policy_hostile_answers
    scenario_policy_named_start_outranks_choose_start
    scenario_policy_damage_scale
    scenario_policy_invulnerable_tank
    scenario_policy_protected_builder
    scenario_policy_protected_pill
    scenario_policy_can_build
    scenario_policy_can_capture
    scenario_policy_kill_ops_ignore_can_die
    scenario_policy_combat_null_is_classic
    scenario_policy_lua_allow_base_win
    scenario_policy_lua_can_respawn
    scenario_policy_lua_can_build
    scenario_policy_lua_can_capture
    scenario_policy_lua_announce
    scenario_policy_lua_can_die
    scenario_policy_lua_nil_is_classic
    scenario_policy_lua_op_in_policy
    scenario_policy_lua_error_counts
    scenario_policy_lua_choose_start
    scenario_policy_lua_spawn_loadout
    scenario_policy_lua_damage_scale
    scenario_policy_lua_value_classic
    scenario_policy_lua_value_error_counts
    scenario_policy_lua_value_in_policy
    game_event_channel_base_captured
    game_event_channel_pill_captured
    game_event_channel_lgm_lost
    game_event_channel_control_only_subscriber
    game_event_channel_client_emits_nothing
    game_event_channel_neutralised
    game_event_channel_capture_index_base
    game_event_channel_capture_wire_bytes
    game_event_channel_neutralised_no_client_line
    game_event_channel_capture_attribution
    game_event_channel_subscriber_count
    game_events_tank_spawned
    game_events_lgm_landed
    game_events_pill_placed
    game_events_pill_picked_up
    game_events_pill_killed
    game_events_built
    game_events_mine_laid
    game_events_mine_exploded
    game_events_mine_placed_is_local
    scenario_announce_null_policy_is_classic
    scenario_announce_policy_asked_per_kind
    scenario_announce_quiet_stamped_on_every_fact
    scenario_announce_client_lines_read_the_byte
    scenario_announce_wire_bytes
    scenario_announce_pill_line_written_once
    scenario_announce_vote_line_held
    scenario_announce_loopback_quiet_draws_no_line
    scenario_host_metadata
    scenario_host_rules_change_round
    scenario_host_syntax_error_line
    scenario_host_unknown_rule_key
    scenario_host_api_too_new
    scenario_host_no_script
    scenario_host_manifest_roundtrip
    scenario_host_team_init_read
    scenario_host_seed_reproducible
    scenario_host_edit_after_attach
    scenario_host_reload_picks_up_edit
    scenario_host_reload_bad_syntax
    scenario_host_reload_bad_api
    scenario_host_reload_applies_nothing
    scenario_host_fresh_globals_per_round
    scenario_host_setup_in_window
    scenario_host_start_on_first_running_tick
    scenario_host_error_limit_boundary
    scenario_host_disabled_stops_hooks
    scenario_host_vm_lock_same_thread
    scenario_host_vm_lock_second_thread
    scenario_host_audit_human_lost
    scenario_host_failed_start_drops_manifest
    scenario_host_round_answers_its_own_start
    scenario_host_opening_tank_under_rules
    scenario_host_boot_failure_still_starts
    scenario_host_round_after_scenario_is_classic
    scenario_host_tag_follows_switch
    scenario_host_chunk_events_reach_hooks
    scenario_host_metatable_raises
    scenario_host_metatable_not_read
    scenario_host_hook_via_global_metatable
    scenario_host_disabled_refuses_script
    scenario_host_disabled_plain_map
    scenario_host_enabled_again
    scenario_validate_clean
    scenario_validate_api_too_new
    scenario_validate_lobby_shape
    scenario_validate_team_init_reported
    scenario_validate_unknown_rule
    scenario_validate_rule_out_of_range
    scenario_validate_rule_pair
    scenario_validate_tag_past_map
    scenario_validate_fifth_tag
    scenario_validate_region_off_map
    scenario_validate_bound_false_with_tags
    scenario_validate_syntax_error_line
    scenario_validate_lines_point_at_the_key
    scenario_validate_wave_defense
    scenario_validate_unknown_game
    scenario_lua_every_row_answers
    scenario_lua_read_index_passes_through
    scenario_lua_op_index_subtracts_one
    scenario_lua_script_index_adds_one
    scenario_lua_absent_reads_are_nil
    scenario_lua_terrain_is_the_whole_map
    scenario_lua_shape_error_counts
    scenario_lua_rule_reads_the_table
    scenario_lua_tags_and_regions
    scenario_lua_refusals_in_round
    scenario_lua_refusals_in_lobby
    scenario_lua_op_index_reaches_the_payload
    scenario_lua_add_answers_its_index
    scenario_lua_queued_answers_queued
    scenario_lua_shape_raises_refusal_does_not
    scenario_lua_detail_carries_the_number
    scenario_lua_teleport_start_refuses_bad_index
    scenario_lua_spawn_bot_refuses_bad_start
    scenario_lua_game_type_resolves_scripted
    scenario_sandbox_removed_names_are_nil
    scenario_sandbox_bytecode_chunk_refused
    scenario_sandbox_collectgarbage_stop_refused
    scenario_sandbox_print_reaches_the_console
    scenario_sandbox_memory_cap_refuses
    scenario_sandbox_state_survives_a_refusal
    scenario_sandbox_instruction_budget_cuts_a_loop
    scenario_sandbox_budget_is_per_call
    scenario_sandbox_state_survives_the_budget
    scenario_sandbox_budget_survives_a_nested_call
    scenario_sandbox_budget_survives_a_pcall
    scenario_sandbox_budget_survives_a_coroutine
    scenario_sandbox_os_date_refuses_a_bad_format
    scenario_sandbox_print_bounded_in_one_call
    scenario_sandbox_print_allowance_returns
    scenario_events_queued_then_drained
    scenario_events_drain_reads_the_length_once
    scenario_events_queued_during_a_drain_waits
    scenario_events_overflow_boundary
    scenario_events_overflow_counts_errors
    scenario_events_no_scenario_delivers_nothing
    scenario_events_lobby_tick_drains
    scenario_events_control_reaches_the_queue
    scenario_events_both_channels_in_publish_order
    scenario_events_channel_says_which
    scenario_events_overflow_covers_both
    scenario_hooks_every_hook_from_its_event
    scenario_hooks_fire_one_tick_later
    scenario_hooks_scripted_both_ways
    scenario_hooks_spawn_drain_is_scripted
    scenario_hooks_team_changed_on_difference
    scenario_hooks_tick_and_end
    scenario_hooks_error_counts_and_disables
    scenario_derived_timer_fires_on_its_tick
    scenario_derived_timer_cancelled_and_stale
    scenario_derived_timer_limit_boundary
    scenario_derived_timers_die_with_the_round
    scenario_derived_region_enter_and_leave
    scenario_derived_define_region_adds_replaces_and_expires
    scenario_derived_region_loop_terminates
    scenario_derived_fixture_wins_without_on_tick
    scenario_package_round_trip
    scenario_package_bad_framing
    scenario_package_entry_names
    scenario_package_two_open
    scenario_package_entry_cap
    scenario_manifest_json_round_trip
    scenario_manifest_json_refusals
    scenario_manifest_agrees
    scenario_manifest_from_values
    scenario_manifest_json_team_init
    scenario_manifest_json_number_range
    scenario_map_body_length
    scenario_map_find_container
    scenario_map_has_script_chunk
    scenario_map_preview_truncates
    scenario_map_preview_passes_plain_bytes
    scenario_map_body_use_local_compare
    scenario_map_has_script_cached
    scenario_packed_map_script_runs
    scenario_packed_map_loose_overrides
    scenario_packed_map_script_omits_table
    scenario_packed_map_table_disagrees
    scenario_packed_map_round_start_keeps_it
    scenario_packed_map_upload_switch
    scenario_packed_map_team_init
    scenario_pack_writes_container
    scenario_pack_manifest_agrees
    scenario_pack_replaces_trailer
    scenario_pack_refuses_unscripted
    scenario_brain_name_resolves
    scenario_brain_name_missing
    scenario_brain_name_rejects_path
    scenario_brain_op_resolves
    scenario_brain_name_op_missing_refused
    scenario_dir_lists_package
    scenario_dir_lists_loose_script
    scenario_dir_skips_junk
    scenario_dir_skips_subdirectory
    scenario_dir_list_cached
    scenario_dir_entry_roundtrip
    scenario_dir_chunk_not_in_flight
    scenario_precedence_mod_over_map
    scenario_precedence_none_restores_map
    scenario_precedence_plain_map_keeps_mod
    scenario_precedence_template_seats
    scenario_precedence_reset_reapplies
    scenario_precedence_reload_reseats
    scenario_precedence_no_game_plays_strict
    scenario_precedence_open_game_plays_open
    scenario_precedence_reload_refuses_bound
    bot_init_table_two_bots_keep_own
    bot_init_table_empty_when_none
    bot_init_arg_text_to_table
)
# wire_corpus_capture is intentionally omitted: it runs a loopback session to
# regenerate fixtures and is invoked on demand via
# `WinBoloUnitTests --test wire_corpus_capture`, not under CTest.
# spectator_seed_capture is omitted for the same reason: it regenerates the
# committed spectator_seed.bin fixture on demand.
# wbv_v2_capture is omitted for the same reason: it (re)writes the committed
# spectator_v2.wbv fixture and is invoked on demand via
# `WinBoloUnitTests --test wbv_v2_capture`; CTest only runs wbv_reader_v1/v2.
# wbv_summary_capture is omitted for the same reason: it (re)writes the
# committed tests/fixtures/wbv/*.summary files that wbv_fixture_summaries
# compares against, via `WinBoloUnitTests --test wbv_summary_capture`.
foreach(_name IN LISTS _unit_test_names)
    add_test(NAME unit.${_name}
             COMMAND $<TARGET_FILE:WinBoloUnitTests> --test ${_name})
    set_tests_properties(unit.${_name} PROPERTIES
                         TIMEOUT 60
                         FIXTURES_REQUIRED warm_binaries)
endforeach()

# ---- Include-rule check: the scenario surface is invisible to a frontend --
# src/bolo/scenario_api/ is on the include path of scenario_host, sim_owner
# and unittests alone, so a gui translation unit that includes the funnel
# header must not compile. The target below is exactly that translation
# unit, built under the gui profile and kept out of the ordinary build; the
# test builds it and expects the build to fail. If the include rule is ever
# loosened, the build succeeds and the test goes red.
add_library(scenario_api_hidden_from_gui STATIC EXCLUDE_FROM_ALL
    tests/include_rules/scenario_api_under_gui.c)
bolo_apply_include_rules(scenario_api_hidden_from_gui gui)

add_test(NAME include_rules.scenario_api_hidden_from_gui
         COMMAND ${CMAKE_COMMAND} --build . --target scenario_api_hidden_from_gui
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(include_rules.scenario_api_hidden_from_gui PROPERTIES
                     TIMEOUT 120
                     WILL_FAIL TRUE)

if(WB_NETDEBUG)
    foreach(_name netdebug_commanded_vs_executed netdebug_overshoot_under_loss netdebug_mine_once_under_loss netdebug_error_offset_clamped)
        add_test(NAME netdebug.${_name}
                 COMMAND $<TARGET_FILE:WinBoloUnitTests> --test ${_name})
        set_tests_properties(netdebug.${_name} PROPERTIES
                             TIMEOUT 60
                             FIXTURES_REQUIRED warm_binaries)
    endforeach()
endif()

# ---- Fuzz targets (WB_FUZZ, Linux + Clang) -------------------------------
# Two libFuzzer executables and, for each, a corpus-replay ctest: the binary
# is run over its committed corpus + crash set with -runs=0 (load and execute
# each input once, no mutation), so it is bounded, deterministic, and fails
# the build on any regression. Open-ended discovery is a separate, non-ctest
# job — see tests/fuzz/run_discovery.sh.
if(WB_FUZZ)
    set(WB_FUZZ_CORPUS_DIR "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus")
    set(WB_FUZZ_CRASH_DIR  "${CMAKE_SOURCE_DIR}/tests/fuzz/crashes")

    # Tier 1 — pure bounded wire codecs + command codec. bolo_static only.
    add_executable(fuzz_wire_codec tests/fuzz/fuzz_wire_codec.c)
    bolo_apply_include_rules(fuzz_wire_codec unittests)
    target_link_libraries(fuzz_wire_codec PRIVATE bolo_static SDL3::SDL3 ${MATH_LIB} pthread)
    # -fcommon: the bolo/lzw sources rely on tentative-definition merging that
    # Clang's default -fno-common would reject. Mirrors WinBoloUnitTests.
    target_compile_options(fuzz_wire_codec PRIVATE -fcommon)
    target_link_options(fuzz_wire_codec PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_wire_codec "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.wire_codec
             COMMAND $<TARGET_FILE:fuzz_wire_codec> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/wire_codec ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.wire_codec PROPERTIES TIMEOUT 120)

    # The compressed-map loader. Every map that arrives over the network lands
    # in mapLoadCompressedMap: three fixed-size struct memcpys for the bases,
    # pillboxes and starts, then an LZW decode of the terrain. The struct
    # region is copied wholesale, so the file-load setters' per-field clamps
    # never run on this route and basesValidate/pillsValidate/startsValidate
    # stand in for them. Links like join_accept: bolo_static alone leaves the
    # front-end, lang and lzw symbols the map path reaches undefined.
    add_executable(fuzz_map_load
        tests/fuzz/fuzz_map_load.c
        tests/unit/test_stubs.c
        src/steam/steam_wrapper_stub.c
        src/common/prefs_doc.c
        src/common/prefs.c
        src/common/md5.c
        src/common/mp_diag_log.c
        src/server/server_dedicated_log_path.c
        src/winbolonet/wbn_bearer.c
        src/winbolonet/wbn_country_cache.c
        src/winbolonet/wbn_news_parser.cpp
        src/winbolonet/wbn_prefs_sync.c
        src/bolo/transport_udp_client.c
        src/lzw/dcodlzw.c
        src/lzw/ecodlzw.c)
    bolo_apply_include_rules(fuzz_map_load unittests)
    target_include_directories(fuzz_map_load PRIVATE
        src/gui/sdl3 src/server src src/winbolonet
        ${CMAKE_SOURCE_DIR}/tests/unit ${cjson_SOURCE_DIR})
    target_compile_definitions(fuzz_map_load PRIVATE
        HAVE_SCREEN_C=1
        WINBOLO_VERSION="${WINBOLO_VERSION}"
        WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
        WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
        WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
        WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
        BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
        BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
        BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION})
    target_link_libraries(fuzz_map_load PRIVATE
        bolo_static ${SERVER_LINK_GROUP} bot_brains_static
        SDL3::SDL3 minizip lua_static cjson utf8proc mdns ${MATH_LIB} pthread)
    target_compile_options(fuzz_map_load PRIVATE -fcommon)
    target_link_options(fuzz_map_load PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_map_load "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.map_load
             COMMAND $<TARGET_FILE:fuzz_map_load> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/map_load ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.map_load PROPERTIES TIMEOUT 120)

    # The same loader, past the LZW gate. fuzz_map_load cannot reach a
    # successful load — a mutated compressed stream almost never decodes to
    # exactly 65536 bytes — so this one compresses fuzzer-chosen terrain with
    # the writer's own lzwencoding and hands the result to the real
    # mapLoadCompressedMap. Every input therefore loads, which is what
    # exercises the field clamps and the terrain the file format cannot
    # express.
    add_executable(fuzz_map_fields
        tests/fuzz/fuzz_map_fields.c
        tests/unit/test_stubs.c
        src/steam/steam_wrapper_stub.c
        src/common/prefs_doc.c
        src/common/prefs.c
        src/common/md5.c
        src/common/mp_diag_log.c
        src/server/server_dedicated_log_path.c
        src/winbolonet/wbn_bearer.c
        src/winbolonet/wbn_country_cache.c
        src/winbolonet/wbn_news_parser.cpp
        src/winbolonet/wbn_prefs_sync.c
        src/bolo/transport_udp_client.c
        src/lzw/dcodlzw.c
        src/lzw/ecodlzw.c)
    bolo_apply_include_rules(fuzz_map_fields unittests)
    target_include_directories(fuzz_map_fields PRIVATE
        src/gui/sdl3 src/server src src/winbolonet
        ${CMAKE_SOURCE_DIR}/tests/unit ${cjson_SOURCE_DIR})
    target_compile_definitions(fuzz_map_fields PRIVATE
        HAVE_SCREEN_C=1
        WINBOLO_VERSION="${WINBOLO_VERSION}"
        WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
        WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
        WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
        WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
        BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
        BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
        BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION})
    target_link_libraries(fuzz_map_fields PRIVATE
        bolo_static ${SERVER_LINK_GROUP} bot_brains_static
        SDL3::SDL3 minizip lua_static cjson utf8proc mdns ${MATH_LIB} pthread)
    target_compile_options(fuzz_map_fields PRIVATE -fcommon)
    target_link_options(fuzz_map_fields PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_map_fields "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.map_fields
             COMMAND $<TARGET_FILE:fuzz_map_fields> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/map_fields ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.map_fields PROPERTIES TIMEOUT 120)

    # Tier 1 — the channel-mux frame parser (channelRecvFrame), the single
    # bounds-checked decode site the reliability rework collapsed every reliable
    # queue / carrier / chunker onto. bolo_static only, like the wire codecs.
    add_executable(fuzz_channel_frame tests/fuzz/fuzz_channel_frame.c)
    bolo_apply_include_rules(fuzz_channel_frame unittests)
    target_link_libraries(fuzz_channel_frame PRIVATE bolo_static SDL3::SDL3 ${MATH_LIB} pthread)
    target_compile_options(fuzz_channel_frame PRIVATE -fcommon)
    target_link_options(fuzz_channel_frame PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_channel_frame "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.channel_frame
             COMMAND $<TARGET_FILE:fuzz_channel_frame> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/channel_frame ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.channel_frame PROPERTIES TIMEOUT 120)

    # Tier 1 — the voice segment parsers (voiceSegmentUnpackUp / UnpackDown),
    # the parse sites for everything carried on the best-effort CHANNEL_VOICE:
    # nothing upstream validates a voice segment, and the opus slice each parse
    # returns points into the caller's buffer. bolo_static only, like the wire
    # codecs.
    add_executable(fuzz_voice_segment tests/fuzz/fuzz_voice_segment.c)
    bolo_apply_include_rules(fuzz_voice_segment unittests)
    target_link_libraries(fuzz_voice_segment PRIVATE bolo_static SDL3::SDL3 ${MATH_LIB} pthread)
    target_compile_options(fuzz_voice_segment PRIVATE -fcommon)
    target_link_options(fuzz_voice_segment PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_voice_segment "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.voice_segment
             COMMAND $<TARGET_FILE:fuzz_voice_segment> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/voice_segment ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.voice_segment PROPERTIES TIMEOUT 120)

    # Tier 1 — the bulk-transfer receive state machine (bulkParseStreamHeader +
    # bulkReceiverFeed), reassembling map download / resync / upload / preview
    # blobs off CHANNEL_BULK from wire-supplied, untrusted headers and bodies.
    # bolo_static only, like the wire codecs.
    add_executable(fuzz_bulk_transfer tests/fuzz/fuzz_bulk_transfer.c)
    bolo_apply_include_rules(fuzz_bulk_transfer unittests)
    target_link_libraries(fuzz_bulk_transfer PRIVATE bolo_static SDL3::SDL3 ${MATH_LIB} pthread)
    target_compile_options(fuzz_bulk_transfer PRIVATE -fcommon)
    target_link_options(fuzz_bulk_transfer PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_bulk_transfer "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.bulk_transfer
             COMMAND $<TARGET_FILE:fuzz_bulk_transfer> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/bulk_transfer ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.bulk_transfer PROPERTIES TIMEOUT 120)

    # Tier 2 — full server dispatcher via the WB_FUZZ-gated seam. Links
    # server_static (which owns udp/udp_server_test_hooks.c, the file that
    # carries the seam).
    # test_stubs.c + the steam stub resolve the winbolonet / natPortMap /
    # frontend callbacks the server graph references but server_static doesn't
    # own — the same stub surface WinBoloUnitTests links.
    # The loose (non-library) sources WinBoloUnitTests compiles directly to
    # close the link: mpDiagLog, md5*, lzw*, and the client transport
    # (transport_udp_client.c is NOT in bolo_static — client_sim/client_net in
    # the listen-server path reference it). Matching the unit-test set verbatim
    # keeps the link closure identical to a known-good target.
    add_executable(fuzz_server_dispatch
        tests/fuzz/fuzz_server_dispatch.c
        tests/unit/test_stubs.c
        src/steam/steam_wrapper_stub.c
        src/common/prefs_doc.c
        src/common/prefs.c
        src/common/md5.c
        src/common/mp_diag_log.c
        src/server/server_dedicated_log_path.c
        src/winbolonet/wbn_bearer.c
        src/winbolonet/wbn_country_cache.c
        src/winbolonet/wbn_news_parser.cpp
        src/winbolonet/wbn_prefs_sync.c
        src/bolo/transport_udp_client.c
        src/lzw/dcodlzw.c
        src/lzw/ecodlzw.c)
    bolo_apply_include_rules(fuzz_server_dispatch unittests)
    # Same include set WinBoloUnitTests gives test_stubs.c (luabrainshandler.h
    # lives in src/gui/sdl3, etc.).
    target_include_directories(fuzz_server_dispatch PRIVATE
        src/gui/sdl3 src/server src src/winbolonet
        ${CMAKE_SOURCE_DIR}/tests/unit ${cjson_SOURCE_DIR})
    # Version/build defines the loose sources need (transport_udp_client.c
    # bakes BOLO_VERSION_* into its JOIN packet). Mirrors WinBoloUnitTests,
    # minus the test-only NETIMPAIR / wire-capture seams.
    target_compile_definitions(fuzz_server_dispatch PRIVATE
        HAVE_SCREEN_C=1
        WINBOLO_VERSION="${WINBOLO_VERSION}"
        WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
        WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
        WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
        WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
        BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
        BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
        BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION})
    target_link_libraries(fuzz_server_dispatch PRIVATE
        bolo_static ${SERVER_LINK_GROUP} bot_brains_static
        SDL3::SDL3 minizip lua_static cjson utf8proc mdns ${MATH_LIB} pthread)
    target_compile_options(fuzz_server_dispatch PRIVATE -fcommon)
    target_link_options(fuzz_server_dispatch PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_server_dispatch "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.server_dispatch
             COMMAND $<TARGET_FILE:fuzz_server_dispatch> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/server_dispatch ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.server_dispatch PROPERTIES TIMEOUT 120)

    # Client snapshot target. Same link closure as the dispatcher (it drives
    # the client transport seam, which lives in transport_udp_client.c and
    # pulls the same client_sim / stub graph).
    add_executable(fuzz_client_snapshot
        tests/fuzz/fuzz_client_snapshot.c
        tests/unit/test_stubs.c
        src/steam/steam_wrapper_stub.c
        src/common/prefs_doc.c
        src/common/prefs.c
        src/common/md5.c
        src/common/mp_diag_log.c
        src/server/server_dedicated_log_path.c
        src/winbolonet/wbn_bearer.c
        src/winbolonet/wbn_country_cache.c
        src/winbolonet/wbn_news_parser.cpp
        src/winbolonet/wbn_prefs_sync.c
        src/bolo/transport_udp_client.c
        src/lzw/dcodlzw.c
        src/lzw/ecodlzw.c)
    bolo_apply_include_rules(fuzz_client_snapshot unittests)
    target_include_directories(fuzz_client_snapshot PRIVATE
        src/gui/sdl3 src/server src src/winbolonet
        ${CMAKE_SOURCE_DIR}/tests/unit ${cjson_SOURCE_DIR})
    target_compile_definitions(fuzz_client_snapshot PRIVATE
        HAVE_SCREEN_C=1
        WINBOLO_VERSION="${WINBOLO_VERSION}"
        WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
        WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
        WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
        WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
        BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
        BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
        BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION})
    target_link_libraries(fuzz_client_snapshot PRIVATE
        bolo_static ${SERVER_LINK_GROUP} bot_brains_static
        SDL3::SDL3 minizip lua_static cjson utf8proc mdns ${MATH_LIB} pthread)
    target_compile_options(fuzz_client_snapshot PRIVATE -fcommon)
    target_link_options(fuzz_client_snapshot PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_client_snapshot "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.client_snapshot
             COMMAND $<TARGET_FILE:fuzz_client_snapshot> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/client_snapshot ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.client_snapshot PROPERTIES TIMEOUT 120)

    # JOIN_ACCEPT target. Same link closure as the client snapshot target —
    # only the entry point differs (drives the join-handshake seam).
    add_executable(fuzz_join_accept
        tests/fuzz/fuzz_join_accept.c
        tests/unit/test_stubs.c
        src/steam/steam_wrapper_stub.c
        src/common/prefs_doc.c
        src/common/prefs.c
        src/common/md5.c
        src/common/mp_diag_log.c
        src/server/server_dedicated_log_path.c
        src/winbolonet/wbn_bearer.c
        src/winbolonet/wbn_country_cache.c
        src/winbolonet/wbn_news_parser.cpp
        src/winbolonet/wbn_prefs_sync.c
        src/bolo/transport_udp_client.c
        src/lzw/dcodlzw.c
        src/lzw/ecodlzw.c)
    bolo_apply_include_rules(fuzz_join_accept unittests)
    target_include_directories(fuzz_join_accept PRIVATE
        src/gui/sdl3 src/server src src/winbolonet
        ${CMAKE_SOURCE_DIR}/tests/unit ${cjson_SOURCE_DIR})
    target_compile_definitions(fuzz_join_accept PRIVATE
        HAVE_SCREEN_C=1
        WINBOLO_VERSION="${WINBOLO_VERSION}"
        WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
        WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
        WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
        WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
        BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
        BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
        BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION})
    target_link_libraries(fuzz_join_accept PRIVATE
        bolo_static ${SERVER_LINK_GROUP} bot_brains_static
        SDL3::SDL3 minizip lua_static cjson utf8proc mdns ${MATH_LIB} pthread)
    target_compile_options(fuzz_join_accept PRIVATE -fcommon)
    target_link_options(fuzz_join_accept PRIVATE -fsanitize=fuzzer)
    set_flat_output(fuzz_join_accept "${CMAKE_BINARY_DIR}")
    add_test(NAME fuzz.join_accept
             COMMAND $<TARGET_FILE:fuzz_join_accept> -runs=0
                     ${WB_FUZZ_CORPUS_DIR}/join_accept ${WB_FUZZ_CRASH_DIR})
    set_tests_properties(fuzz.join_accept PROPERTIES TIMEOUT 120)

    # Stage the FetchContent-built shared libs next to the fuzz binaries so the
    # $ORIGIN RPATH resolves them at run time (these copies normally ride the
    # game/server targets, which a fuzz-only build doesn't produce). Mirrors
    # the WinBolo POST_BUILD copy. Tier 1 needs only SDL3; tier 2 also pulls in
    # onnxruntime via bot_brains_static.
    add_custom_command(TARGET fuzz_wire_codec POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:fuzz_wire_codec>")
    add_custom_command(TARGET fuzz_channel_frame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:fuzz_channel_frame>")
    add_custom_command(TARGET fuzz_bulk_transfer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:fuzz_bulk_transfer>")
    add_custom_command(TARGET fuzz_server_dispatch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:fuzz_server_dispatch>")
    add_custom_command(TARGET fuzz_client_snapshot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:fuzz_client_snapshot>")
    add_custom_command(TARGET fuzz_join_accept POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:fuzz_join_accept>")
    if(HAVE_ONNXRUNTIME AND DEFINED ONNXRUNTIME_LIBDIR)
        file(GLOB _FUZZ_ORT_SO "${ONNXRUNTIME_LIBDIR}/libonnxruntime*")
        foreach(_f ${_FUZZ_ORT_SO})
            if(_f MATCHES "\\.dSYM$")
                continue()
            endif()
            add_custom_command(TARGET fuzz_server_dispatch POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_f}" "$<TARGET_FILE_DIR:fuzz_server_dispatch>")
            add_custom_command(TARGET fuzz_client_snapshot POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_f}" "$<TARGET_FILE_DIR:fuzz_client_snapshot>")
            add_custom_command(TARGET fuzz_join_accept POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_f}" "$<TARGET_FILE_DIR:fuzz_join_accept>")
        endforeach()
    endif()
endif()

endif() # iOS vs Desktop
