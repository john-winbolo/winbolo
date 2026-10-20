cmake_minimum_required(VERSION 3.18)

# Allow vendored dependencies with older cmake_minimum_required calls
set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE INTERNAL "")

project(winbolo_android C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

# Android 15+ requires 16 KB page size aligned ELF segments
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

# Project root is two levels up from android/app/
set(PROJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include(${PROJ_ROOT}/cmake/bolo_lib.cmake)

# ---------------------------------------------------------------------------
# Dependencies (FetchContent) — same versions as main CMakeLists.txt
# ---------------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(
    sdl3
    URL      https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.14.tar.gz
    SYSTEM
)
FetchContent_Declare(
    sdl3_ttf
    URL      https://github.com/libsdl-org/SDL_ttf/archive/refs/tags/release-3.2.0.tar.gz
    SYSTEM
)
# The same release the desktop, iOS and web builds pin. The docking branch
# head moves, and a fetch of a newer one drops names the shared ImGui code
# still uses.
FetchContent_Declare(
    imgui
    URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    SYSTEM
)
FetchContent_Declare(
    lua
    URL      https://github.com/walterschell/Lua/archive/refs/tags/v5.4.7.tar.gz
    SYSTEM
)
FetchContent_Declare(
    zlib
    URL      https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
    SYSTEM
)
FetchContent_Declare(
    libmaxminddb
    URL      https://github.com/maxmind/libmaxminddb/archive/refs/tags/1.12.2.tar.gz
    SYSTEM
)
# cJSON, for the scenario manifest. scenario_static links it PRIVATE, which
# passes the link along to whatever links scenario_static but does not create
# the target — so the build that links main has to declare it, exactly as the
# root CMakeLists.txt does for the five desktop binaries. Same repository and
# tag as that one.
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

# libmaxminddb: static, no tests/binaries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(MAXMINDDB_BUILD_BINARIES OFF CACHE BOOL "" FORCE)

# SDL3 on Android MUST be built as a shared library for JNI loading
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)

# SDL3_ttf: disable vendored and harfbuzz, we provide freetype via FetchContent
set(SDLTTF_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)

# Fetch freetype and make it available before SDL3_ttf configures
FetchContent_Declare(
    freetype
    URL      https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
    SYSTEM
)
FetchContent_MakeAvailable(freetype)

# Set Freetype variables so SDL3_ttf's find_package(Freetype) succeeds
set(FREETYPE_FOUND TRUE)
set(FREETYPE_LIBRARY freetype)
set(FREETYPE_LIBRARIES freetype)
set(FREETYPE_INCLUDE_DIRS "${freetype_SOURCE_DIR}/include")
set(Freetype_FOUND TRUE)

# Also create a FindFreetype.cmake override so find_package picks it up
file(WRITE "${CMAKE_BINARY_DIR}/cmake-modules/FindFreetype.cmake" "
set(FREETYPE_FOUND TRUE)
set(FREETYPE_LIBRARY freetype)
set(FREETYPE_LIBRARIES freetype)
set(FREETYPE_INCLUDE_DIRS \"${freetype_SOURCE_DIR}/include\")
set(Freetype_FOUND TRUE)
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
")
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_BINARY_DIR}/cmake-modules")

# cJSON: static, and none of its own tests or install rules. BUILD_SHARED_LIBS
# is already OFF above; these are the switches cJSON reads for itself, and the
# same ones the root CMakeLists.txt sets around its own MakeAvailable.
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(sdl3 sdl3_ttf imgui lua zlib libmaxminddb cjson)

# libplum: UPnP, NAT-PMP and PCP port mapping for an Internet game hosted on
# the phone. The same repository, tag and switches as the root's
# bolo_fetch_libplum macro (copied, since the macro reads the root's own
# variables). BUILD_SHARED_LIBS is already OFF above, so plum is static and
# no extra shared library ships. UPnP finds the router by SSDP multicast,
# which the Wi-Fi multicast lock WinBoloActivity holds lets through, so
# every method stays on.
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
FetchContent_MakeAvailable(libplum)
set_target_properties(plum PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Voice chat's codec and echo canceller: the same repositories, tags and
# switches as the root CMakeLists.txt (copied, since its lines read its own
# variables). Both static; BUILD_SHARED_LIBS is already OFF above. Opus
# builds none of its tests, programs or install rules here. SpeexDSP's CMake
# build is on its main branch only (the newest tag ships autotools), so it
# is pinned to a commit, which rules out a shallow clone; its one option
# picks fixed- over floating-point arithmetic and every ABI here has an FPU.
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
    GIT_TAG        ba75b509fb1c5940ea07fa2ba5552e44f3a3576b
    SYSTEM
    EXCLUDE_FROM_ALL
)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)
set(USE_FIXED_POINT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(speexdsp)

# ---------------------------------------------------------------------------
# minizip (from zlib contrib)
# ---------------------------------------------------------------------------

add_library(minizip STATIC
    ${zlib_SOURCE_DIR}/contrib/minizip/zip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/unzip.c
    ${zlib_SOURCE_DIR}/contrib/minizip/ioapi.c
)
target_include_directories(minizip PUBLIC ${zlib_SOURCE_DIR}/contrib/minizip)
target_link_libraries(minizip PUBLIC zlibstatic)

# ---------------------------------------------------------------------------
# nanosvg — header-only SVG parser/rasterizer (vendored)
# ---------------------------------------------------------------------------

add_library(nanosvg STATIC ${PROJ_ROOT}/src/third_party/nanosvg/nanosvg_impl.c)
target_include_directories(nanosvg PUBLIC ${PROJ_ROOT}/src/third_party/nanosvg)
target_compile_options(nanosvg PRIVATE -Wno-unused-function)

# ---------------------------------------------------------------------------
# mdns — header-only mDNS/DNS-SD library (vendored, public domain). Every
# function is static inline, so an INTERFACE target carries the include
# directory and nothing else. bolo_static (discovery_mdns.c) and
# server_static (mdns_advertise.c) link it PRIVATE by this name, the same
# target the root CMakeLists.txt declares.
# ---------------------------------------------------------------------------

add_library(mdns INTERFACE)
target_include_directories(mdns SYSTEM INTERFACE ${PROJ_ROOT}/src/third_party/mdns)

# ---------------------------------------------------------------------------
# stb_image — header-only image loader (vendored)
# ---------------------------------------------------------------------------

add_library(stb_image STATIC ${PROJ_ROOT}/src/third_party/stb/stb_image_impl.c)
target_include_directories(stb_image PUBLIC ${PROJ_ROOT}/src/third_party/stb)
target_compile_options(stb_image PRIVATE -Wno-unused-function)

# ---------------------------------------------------------------------------
# utf8proc — Unicode normalization + property tables (vendored, MIT)
# ---------------------------------------------------------------------------

add_library(utf8proc STATIC ${PROJ_ROOT}/src/third_party/utf8proc/utf8proc.c)
target_include_directories(utf8proc PUBLIC ${PROJ_ROOT}/src/third_party/utf8proc)
target_compile_definitions(utf8proc PUBLIC UTF8PROC_STATIC=1)
set_target_properties(utf8proc PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# ImGui backends (SDL3 + SDL Renderer)
# ---------------------------------------------------------------------------

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

# ---------------------------------------------------------------------------
# Main shared library (SDL3 on Android expects libmain.so)
# ---------------------------------------------------------------------------

# bolo_static covers the bulk of the bolo source set; only the
# HAVE_STEAM-divergent transport_udp_client.c stays per-target here.
# Version string and network protocol bytes come from the shared module so
# Android stays in lock-step with the desktop/server builds — one source of
# truth (cmake/bolo_version.cmake) drives WINBOLO_VERSION, the display suffix,
# and BOLO_VERSION_MAJOR/MINOR/REVISION for every platform.
include(${PROJ_ROOT}/cmake/bolo_version.cmake)
add_subdirectory(${PROJ_ROOT}/src/bolo ${CMAKE_BINARY_DIR}/bolo_static)
add_subdirectory(${PROJ_ROOT}/src/server ${CMAKE_BINARY_DIR}/server_static)
add_subdirectory(${PROJ_ROOT}/src/scenario_io ${CMAKE_BINARY_DIR}/scenario_io_static)
add_subdirectory(${PROJ_ROOT}/src/scenario ${CMAKE_BINARY_DIR}/scenario_static)
add_subdirectory(${PROJ_ROOT}/brains ${CMAKE_BINARY_DIR}/bot_brains_static)

set(ANDROID_SOURCES
    # Android entry point and frontend
    ${PROJ_ROOT}/src/android/main_android.c
    ${PROJ_ROOT}/src/android/android_frontend.c
    # The winbolo:// link or .wbv recording another app hands over while the
    # app runs, kept for the welcome screen (WinBoloActivity.onNewIntent).
    ${PROJ_ROOT}/src/android/android_drops.c
    # The kind of network the phone's traffic goes out on (WinBoloNet.java
    # over ConnectivityManager), for the mobile-data warning before an
    # Internet game is hosted.
    ${PROJ_ROOT}/src/android/android_netpath.c
    # The phone or tablet decision (WinBoloActivity.smallestWidthDp over JNI)
    # ui_mode.c reads, and the on-screen keyboard's height for the dialogs.
    ${PROJ_ROOT}/src/android/android_device.c
    # The RECORD_AUDIO permission voice.c asks for before it opens the
    # microphone (SDL_RequestAndroidPermission, answered on another thread).
    ${PROJ_ROOT}/src/android/android_voice.c
    # The share sheet (WinBoloActivity.shareFile over JNI), which is how a
    # recording or a clip GIF leaves the app's private storage.
    ${PROJ_ROOT}/src/android/android_share.c
    # The Android half of the recordings screen: the directories it lists
    # and the share sheet as its export.
    ${PROJ_ROOT}/src/android/android_recordings.c
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_welcome.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_mapchooser.cpp
    # Shared mobile UI (touch input, legacy players panel), as iOS compiles.
    ${PROJ_ROOT}/src/gui/mobile/touch_input.c
    ${PROJ_ROOT}/src/gui/mobile/players_panel.cpp
    # The mobile replay screens, as iOS compiles them: a downloaded game
    # played in the reel (in place of the modal viewer), the live spectator
    # feed in the reel (in place of the modal viewer's spectatorRun), and
    # the recordings on the phone listed and played in the reel (in place of
    # the modal viewer's logViewerRun; its platform half is
    # android_recordings.c above).
    ${PROJ_ROOT}/src/gui/mobile/mobile_log_player.cpp
    ${PROJ_ROOT}/src/gui/mobile/mobile_spectator.cpp
    ${PROJ_ROOT}/src/gui/mobile/mobile_recordings.cpp
    ${PROJ_ROOT}/src/android/android_dialog_stubs.c

    # WinBolo.net: the same winbolonet_core, winbolonet_client and
    # winbolonet_server sources the iOS target compiles, with the HTTP
    # transport over Java's HttpURLConnection (WinBoloHttp.java, called
    # through JNI) in place of http.c, since Android has no libcurl.
    ${PROJ_ROOT}/src/winbolonet/http_jni.c
    ${PROJ_ROOT}/src/winbolonet/winbolonetthread.c
    ${PROJ_ROOT}/src/winbolonet/winbolonetevents.c
    ${PROJ_ROOT}/src/winbolonet/winbolonet_core.c
    ${PROJ_ROOT}/src/winbolonet/wbn_bearer.c
    ${PROJ_ROOT}/src/winbolonet/wbn_country_cache.c
    ${PROJ_ROOT}/src/winbolonet/wbn_prefs_sync.c
    ${PROJ_ROOT}/src/winbolonet/wbn_serverlist.c
    ${PROJ_ROOT}/src/winbolonet/wbn_map.c
    ${PROJ_ROOT}/src/winbolonet/winbolonet_client.c
    ${PROJ_ROOT}/src/winbolonet/winbolonet_server.c
    ${PROJ_ROOT}/src/winbolonet/wbn_comments.cpp
    ${PROJ_ROOT}/src/winbolonet/wbn_news.cpp
    ${PROJ_ROOT}/src/winbolonet/wbn_news_parser.cpp
    # The Ed25519 signer the transport signs every API request with.
    ${PROJ_ROOT}/src/third_party/tweetnacl/tweetnacl.c
    # The WinBolo.net screens: the news popup with its image cache, the
    # recap's stars and comments, the log browser, and the map chooser's
    # WinBolo.net catalogue with the stored-map preview fetch.
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_news.cpp
    ${PROJ_ROOT}/src/gui/sdl3/news_image_cache.cpp
    ${PROJ_ROOT}/src/gui/sdl3/news_image_budget.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_star_rating.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_rating.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_wbn_browser.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/wbn_browser_sort.c
    ${PROJ_ROOT}/src/gui/sdl3/map_preview_fetch.cpp
    ${PROJ_ROOT}/src/gui/sdl3/wbn_map_source.cpp
    ${PROJ_ROOT}/src/steam/steam_wrapper_stub.c

    # Per-target bolo source (HAVE_STEAM-divergent).
    ${PROJ_ROOT}/src/bolo/transport_udp_client.c
    # The port mapping wrapper server_lifecycle.c calls, over libplum
    # (BOLO_PORTMAP below), as the desktop and iOS targets compile it.
    ${PROJ_ROOT}/src/bolo/nat_portmap.c
    # The lobby's default bot naming pools, read from data/bot_names.json.
    ${PROJ_ROOT}/src/bolo/lobby_bot_pools_json.c

    ${PROJ_ROOT}/src/common/md5.c
    # The preferences document (WinBolo.json) the game front reads and
    # writes, and the multiplayer diagnostic log the UDP client writes.
    ${PROJ_ROOT}/src/common/prefs.c
    ${PROJ_ROOT}/src/common/prefs_doc.c
    ${PROJ_ROOT}/src/common/mp_diag_log.c
    # PNG writer for the map chooser's thumbnail cache. The stb_image
    # target compiles only the reader; the writer is a separate impl TU.
    ${PROJ_ROOT}/src/third_party/stb/stb_image_write_impl.c

    # SDL3 GUI (rendering, sound, input, etc.)
    ${PROJ_ROOT}/src/gui/ui_mode.c
    ${PROJ_ROOT}/src/gui/visibility_presets.c
    ${PROJ_ROOT}/src/gui/scn_panel_prefs.c
    ${PROJ_ROOT}/src/gui/sdl3/clientmutex.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl3imgui.cpp
    ${PROJ_ROOT}/src/gui/sdl3/scenario_panel_draw.cpp
    ${PROJ_ROOT}/src/gui/sdl3/scenario_marker.c
    ${PROJ_ROOT}/src/gui/sdl3/imgui_steam_nav.cpp
    ${PROJ_ROOT}/src/gui/sdl3/sdl3imgui_tablet.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialog_backend.c
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_udpsetup.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_gamebrowser.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_setname.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_trackersetup.cpp
    # Touch handling every ImGui context needs on a phone: the keyboard
    # without auto-capitalisation or autocorrect, and drag to scroll. Run
    # from dialogDrawNavOutline, so it reaches every dialog and the in-game
    # frame.
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_touch.cpp
    # The "Connecting to <host>..." screen gamefront.c shows on mobile while
    # a join waits on the network.
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_connecting.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_messagebox.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_keycap.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_lobby.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_assets.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_chat.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_settings.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_status.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_players.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_map_preview.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_commands.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_recap.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_reel.cpp
    # The recap's clip GIF export and the encoder's one implementation TU.
    # The clip is written into the app's files directory (lobby_clip_gif.cpp),
    # as there is no save dialog, and offered through the share sheet.
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_clip_gif.cpp
    ${PROJ_ROOT}/src/third_party/msf_gif/msf_gif_impl.c
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_chooser.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_scenario_chooser.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_wbn_maps.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_settings.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_keysetup.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_deck_pause.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_keyboard.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_quickchat.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_onboarding.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_winbolonet.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_about.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/dialog_footer.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_nav_outline.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_tutorial_overlay.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_controller_prompt.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/imgui_controller_disconnect.cpp
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/dialog_quit.cpp
    ${PROJ_ROOT}/src/gui/sdl3/map_preview_popup.cpp
    ${PROJ_ROOT}/src/gui/sdl3/lobby_start_list.cpp
    ${PROJ_ROOT}/src/gui/sdl3/map_preview_view.cpp
    ${PROJ_ROOT}/src/gui/sdl3/overview_camera.cpp
    ${PROJ_ROOT}/src/gui/sdl3/overview_fog.cpp
    ${PROJ_ROOT}/src/gui/sdl3/overview_hud_layout.cpp
    ${PROJ_ROOT}/src/gui/sdl3/overview_view.cpp
    ${PROJ_ROOT}/src/gui/sdl3/map_stars.cpp
    ${PROJ_ROOT}/src/gui/sdl3/wb_theme.cpp
    ${PROJ_ROOT}/src/gui/sdl3/map_colours.c
    ${PROJ_ROOT}/src/gui/sdl3/map_markers.c
    ${PROJ_ROOT}/src/gui/sdl3/minimap_render.c
    ${PROJ_ROOT}/src/gui/sdl3/luabrainshandler.c
    ${PROJ_ROOT}/src/gui/sdl3/cursor.c
    ${PROJ_ROOT}/src/gui/sdl3/flags.c
    ${PROJ_ROOT}/src/gui/sdl3/dns_lookups.c
    ${PROJ_ROOT}/src/gui/sdl3/gamefront.c
    # The shared sim-driving cores every client platform calls: the
    # keys/game tick step, the join wait, the guarded frontEnd* bodies with
    # the tutorial sequencer, and the single-player lobby seeding.
    ${PROJ_ROOT}/src/client_frontend/client_frontend_connect.c
    ${PROJ_ROOT}/src/client_frontend/client_frontend_tick.c
    ${PROJ_ROOT}/src/client_frontend/client_frontend_common.c
    ${PROJ_ROOT}/src/client_frontend/client_frontend_sp.c
    ${PROJ_ROOT}/src/gui/sdl3/bg_game.c
    ${PROJ_ROOT}/src/gui/sdl3/mapview.c
    ${PROJ_ROOT}/src/gui/sdl3/mapview_overlay.c
    ${PROJ_ROOT}/src/gui/sdl3/sprite_positions.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl3draw.c
    ${PROJ_ROOT}/src/gui/sdl3/newswire_slide.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl3draw_status.c
    ${PROJ_ROOT}/src/gui/sdl3/tank_label.c
    ${PROJ_ROOT}/src/gui/sdl3/tileloader.c
    ${PROJ_ROOT}/src/gui/sdl3/skin_source.c
    ${PROJ_ROOT}/src/gui/sdl3/gfx_settings.c
    ${PROJ_ROOT}/src/gui/sdl3/sdl_bmp.c
    ${PROJ_ROOT}/src/gui/sdl3/input.c
    ${PROJ_ROOT}/src/gui/sdl3/input_touch.c
    ${PROJ_ROOT}/src/gui/sdl3/input_joystick.c
    ${PROJ_ROOT}/src/gui/sdl3/input_gamepad.c
    ${PROJ_ROOT}/src/gui/sdl3/input_source.c
    ${PROJ_ROOT}/src/gui/sdl3/glyphs.c
    # Smart-ping pie menu and markers, and the icon loader they share with
    # the log viewer's replay draw. sdl3draw.c calls into both.
    ${PROJ_ROOT}/src/gui/sdl3/ping_overlay.cpp
    ${PROJ_ROOT}/src/gui/sdl3/ping_icons.c
    ${PROJ_ROOT}/src/gui/sdl3/ping_marker.c
    ${PROJ_ROOT}/src/gui/sdl3/ring_band.c
    ${PROJ_ROOT}/src/gui/sdl3/build_cursor.c
    ${PROJ_ROOT}/src/gui/sdl3/tutorial_text.c
    ${PROJ_ROOT}/src/gui/sdl3/lang.c
    ${PROJ_ROOT}/src/gui/sdl3/sim_rules_phrase.c
    ${PROJ_ROOT}/src/gui/sdl3/sound.c
    ${PROJ_ROOT}/src/gui/sdl3/sound_variants.c
    ${PROJ_ROOT}/src/gui/sdl3/sound_mix.c

    # Voice chat: the shared runtime in src/client_frontend and the SDL3
    # device half in src/gui/sdl3/voice.c, which captures and plays through
    # SDL audio streams, so it runs here as it does on iOS. The same sources
    # and definitions as the WinBoloIOS target (WINBOLO_VOICE and
    # WINBOLO_VOICE_AEC below). SDL's AAudio driver declares the game chat
    # role, so voiceBackendOsCancelsEcho answers true at run time and Speex
    # stays idle; it is compiled all the same, as on every platform.
    ${PROJ_ROOT}/src/client_frontend/voice_core.c
    ${PROJ_ROOT}/src/client_frontend/voice_client.c
    ${PROJ_ROOT}/src/client_frontend/voice_aec.c
    ${PROJ_ROOT}/src/gui/sdl3/voice.c
    ${PROJ_ROOT}/src/gui/sdl3/voice_settings.c

    # Shared map-gen controls used by the map chooser dialog, listed apart
    # from the editor below as the iOS target lists them (the lobby needs
    # them with or without the editor).
    ${PROJ_ROOT}/src/mapeditor/mapgen_imgui.cpp

    # The map editor, the same sources the root CMakeLists.txt's
    # IOS_MAPEDITOR_SOURCES list compiles into WinBoloIOS: the desktop's
    # editor less mapgen_imgui.cpp (above) and the macOS menu bar shim, as
    # the editor draws its ImGui menu bar here. Its touch input (a tap
    # paints, a drag draws, two fingers pan and pinch) is the iOS one. The
    # welcome screen does not offer it yet (imgui_welcome.cpp).
    ${PROJ_ROOT}/src/mapeditor/mapeditor.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_imgui.cpp
    ${PROJ_ROOT}/src/mapeditor/mapeditor_undo.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_validate.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_symmetry.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_text.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_fonts.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_stats.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_image.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_stamp.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_export.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_scenario.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_scenario_form.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_scenario_check.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_scenario_pack.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_scenario_imgui.cpp
    ${PROJ_ROOT}/src/mapeditor/mapeditor_scenario_fndesc.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_scenario_fnscan.c
    # File > Open from WinBolo.net (MAPEDITOR_WBN_OPEN below).
    ${PROJ_ROOT}/src/mapeditor/mapeditor_wbn_open.cpp
    # SDL has no file dialogs on Android; the editor's open and save, and
    # the map chooser's Load from device, go through the picker iOS shares.
    # Its Android half lists the Maps folder under the preferences path,
    # where the in-game Save Map writes, and the bundled maps under the
    # working directory.
    ${PROJ_ROOT}/src/mapeditor/mapeditor_mobile_files.cpp
    ${PROJ_ROOT}/src/android/android_mapeditor_files.c
    # The scenario script pane's text editor widget.
    ${PROJ_ROOT}/src/third_party/ImGuiColorTextEdit/TextEditor.cpp
    ${PROJ_ROOT}/src/third_party/ImGuiColorTextEdit/LanguageDefinitions.cpp

    # The lobby's replay reel and the rounds it plays. The log viewer's
    # decoder, renderer and embedded reel host (lv_embed.c) are the set the
    # iOS target lists (IOS_LOGVIEWER_SOURCES in the root CMakeLists.txt, and
    # the "Never add here" list of src/wasm/CMakeLists.txt applies here too),
    # with the desktop's platform drivers in place of the wasm ones, except
    # the dialogs, which have an Android driver. The modal viewer,
    # logviewer.c, is not built; the WinBolo.net log browser opens a game in
    # the reel instead. Rounds are recorded by server_round_log_static.
    ${PROJ_ROOT}/src/logviewer/lv_embed.c
    ${PROJ_ROOT}/src/logviewer/lv_embed_stubs.c
    ${PROJ_ROOT}/src/logviewer/blocks.c
    ${PROJ_ROOT}/src/logviewer/lv_attribution.c
    ${PROJ_ROOT}/src/logviewer/snapshot.c
    ${PROJ_ROOT}/src/logviewer/screen.c
    ${PROJ_ROOT}/src/logviewer/screencalc.c
    ${PROJ_ROOT}/src/logviewer/screentank.c
    ${PROJ_ROOT}/src/logviewer/screenlgm.c
    ${PROJ_ROOT}/src/logviewer/screenbullet.c
    ${PROJ_ROOT}/src/logviewer/spec_seed_load.c
    ${PROJ_ROOT}/src/logviewer/lv_spec_feed.c
    ${PROJ_ROOT}/src/logviewer/players.c
    ${PROJ_ROOT}/src/logviewer/shells.c
    ${PROJ_ROOT}/src/logviewer/allience.c
    ${PROJ_ROOT}/src/logviewer/pillbox.c
    ${PROJ_ROOT}/src/logviewer/bases.c
    ${PROJ_ROOT}/src/logviewer/bolo_map.c
    ${PROJ_ROOT}/src/logviewer/starts.c
    ${PROJ_ROOT}/src/logviewer/labels.c
    ${PROJ_ROOT}/src/logviewer/messages.c
    ${PROJ_ROOT}/src/logviewer/sounddist.c
    ${PROJ_ROOT}/src/logviewer/global.c
    ${PROJ_ROOT}/src/logviewer/util.c
    ${PROJ_ROOT}/src/logviewer/draw.c
    ${PROJ_ROOT}/src/logviewer/game_view_stubs.c
    ${PROJ_ROOT}/src/logviewer/clientmutex.c
    ${PROJ_ROOT}/src/logviewer/dns.c
    ${PROJ_ROOT}/src/logviewer/platform/platform_config.c
    # platform/platform_dialogs.c runs its file pickers through system() and
    # popen(), which Android does not have; the message boxes go through the
    # client's ImGui message box.
    ${PROJ_ROOT}/src/logviewer/android/platform_dialogs_android.c
)

add_library(main SHARED ${ANDROID_SOURCES})

# Third-party source: the same warnings the root CMakeLists.txt turns off
# for it on the Apple targets, its 64-to-32 bit narrowing and its 16-byte
# constant written as a 16-character string.
set_source_files_properties(${PROJ_ROOT}/src/third_party/tweetnacl/tweetnacl.c
    PROPERTIES COMPILE_OPTIONS
    "-Wno-shorten-64-to-32;-Wno-unterminated-string-initialization;-Wno-unknown-warning-option")
# msf_gif.h tests the MSVC-only _M_IX86_FP in an #if, which this target's
# -Werror=undef (below) refuses on the ABIs where __SSE2__ does not settle
# the test first; the two files that include it are let off that one.
set_source_files_properties(
    ${PROJ_ROOT}/src/third_party/msf_gif/msf_gif_impl.c
    ${PROJ_ROOT}/src/gui/sdl3/dialogs/lobby/lobby_clip_gif.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-undef")
# ImGuiColorTextEdit: the same two warnings the root CMakeLists.txt turns
# off for it, so the vendored widget stays as upstream wrote it.
set_source_files_properties(
    ${PROJ_ROOT}/src/third_party/ImGuiColorTextEdit/TextEditor.cpp
    ${PROJ_ROOT}/src/third_party/ImGuiColorTextEdit/LanguageDefinitions.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable;-Wno-logical-op-parentheses")

target_include_directories(main PRIVATE
    ${PROJ_ROOT}/src/gui/sdl3
    ${PROJ_ROOT}/src/gui/sdl3/dialogs
    ${PROJ_ROOT}/src/gui
    ${PROJ_ROOT}/src/gui/mobile
    ${PROJ_ROOT}/src/server
    ${PROJ_ROOT}/src/client_frontend
    # imgui_markdown.h for the WinBolo.net dialog's news bodies.
    ${PROJ_ROOT}/src/third_party/imgui_markdown
    # cJSON.h for prefs_doc.c and lobby_bot_pools_json.c; the cjson target
    # does not export its directory.
    ${cjson_SOURCE_DIR}
    # tweetnacl.h for the WinBolo.net transport's request signing.
    ${PROJ_ROOT}/src/third_party/tweetnacl
    # The map editor's headers, and the text editor widget its scenario
    # pane draws with, as the iOS target adds them.
    ${PROJ_ROOT}/src/mapeditor
    ${PROJ_ROOT}/src/third_party/ImGuiColorTextEdit
)
# After src/gui, so a GUI source's "tiles.h" / "positions.h" still resolves
# to its own; the log viewer's sources reach their copies through their own
# directory. The same three the iOS target adds.
target_include_directories(main PRIVATE
    ${PROJ_ROOT}/src/logviewer
    ${PROJ_ROOT}/src/logviewer/imgui
    ${PROJ_ROOT}/src/logviewer/platform
)
bolo_apply_include_rules(main gui)
# bg_game.c takes the same per-file grant the desktop and iOS targets give
# it: the menu's background game assigns bot teams through setTeam in
# server_sim_lifecycle.h.
bolo_grant_internal_source_access(main
    ${PROJ_ROOT}/src/bolo/transport_udp_client.c
    ${PROJ_ROOT}/src/gui/sdl3/bg_game.c)
# The embedded map editor's three sim-co-owner files take the same per-file
# grant the desktop and iOS targets give them (docs/ARCHITECTURE.md,
# "Privileged exceptions"); the rest of the target keeps the gui profile.
bolo_grant_internal_source_access(main
    ${PROJ_ROOT}/src/mapeditor/mapeditor.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_export.c
    ${PROJ_ROOT}/src/mapeditor/mapeditor_validate.c)

# server_static is the server runtime the desktop hosts with: the UDP server,
# server_lifecycle.c and geolookup.c. Single player starts and ticks through
# the same lifecycle. The same link line and order as WinBoloIOS in the root
# CMakeLists.txt; lld resolves the server_static / server_sim_static cycle
# without a link group.
target_link_libraries(main PRIVATE bolo_static scenario_static
    server_static server_sim_static server_round_log_static
    bot_brains_static threads_static)
target_link_libraries(main PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    imgui_backends
    minizip
    nanosvg
    stb_image
    utf8proc
    lua_static
    zlibstatic
    maxminddb
    # scenario_static reads manifest.json through cJSON and links it PRIVATE,
    # so the symbols land here the way minizip's and Lua's do.
    cjson
    # Port mapping for a hosted Internet game (nat_portmap.c).
    LibPlum::LibPlum
    # Voice chat's codec (voice_core.c) and echo canceller (voice_aec.c).
    Opus::opus
    speexdsp
    m
    log
    android
)

target_compile_definitions(main PRIVATE
    HAVE_SCREEN_C=1
    HAVE_MAXMINDDB=1
    WINBOLO_VERSION="${WINBOLO_VERSION}"
    WINBOLO_DISPLAY_VERSION="${WINBOLO_DISPLAY_VERSION}"
    BOLO_VERSION_MAJOR=${BOLO_VERSION_MAJOR}
    BOLO_VERSION_MINOR=${BOLO_VERSION_MINOR}
    BOLO_VERSION_REVISION=${BOLO_VERSION_REVISION}
    # The About box's build line, from the same bolo_version.cmake.
    WINBOLO_GIT_HASH="${WINBOLO_GIT_HASH}"
    WINBOLO_BUILD_DATE="${WINBOLO_BUILD_DATE}"
    WINBOLO_BUILD_YEAR="${WINBOLO_BUILD_YEAR}"
    # The network-impairment hooks stay out of this build, as they do in
    # every shipping target. Said outright because transport_udp.h tests the
    # macro in an #if before net_impair.h supplies its default, and this
    # target compiles with -Werror=undef.
    WB_ENABLE_NETIMPAIR=0
    # Tells http.h, and the code that links the transport, that it is the
    # JNI one: http.h then declares httpGetUrlToMemory, which the news image
    # cache uses in place of libcurl, as WBN_HTTP_NSURL does on iOS.
    WBN_HTTP_JNI=1
    # nat_portmap.c compiles its libplum body rather than its no-op stubs.
    BOLO_PORTMAP
    # Voice chat and its echo canceller, as the desktop and iOS targets
    # define them: the settings, lobby and status code test these.
    WINBOLO_VOICE=1
    WINBOLO_VOICE_AEC=1
    # The map editor's File > Open from WinBolo.net, as in the desktop's
    # and iOS's embedded editor.
    MAPEDITOR_WBN_OPEN=1
)

target_compile_options(main PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-implicit-fallthrough
    -Wno-implicit-function-declaration
    -Wno-nonportable-include-path
    -Werror=undef
    -fcommon
)
