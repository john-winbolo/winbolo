/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
*Name:          SDL3 ImGui Menu Bar
*Filename:      sdl3imgui.cpp
*Purpose:
*  Implements the ImGui menu bar rendered into the SDL3
*  mirror window.  Uses imgui_impl_sdl3 +
*  imgui_impl_sdlrenderer3 backends.
*
*  Menu items call functions in winbolo.c directly
*  instead of posting Win32 WM_COMMAND messages.
*
*  Include order: SDL3 → ImGui → bolo headers
*  (SDL3 must precede bolo headers to avoid #pragma pack
*  assertion failures inside SDL3 internal headers).
*********************************************************/

/* MSVC: include crtdbg before SDL to avoid _malloca redefinition warning */
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

/* WinSock2 must precede SDL3 (which pulls in windows.h) on Win32 */
#ifdef _WIN32
#include <WinSock2.h>
#endif

/* Apple: need TARGET_OS_IOS for viewport guards */
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

/* SDL3 before bolo headers — see note above */
#include <SDL3/SDL.h>

#include <cctype>   /* toupper — country-code normalization */
#include <cstdlib>  /* bsearch — country-name lookup */
#include <cstring>  /* strcmp — bsearch comparator */

#include "../../common/wb_log.h"

/* ImGui */
#include "imgui.h"
#include "../imgui_theme.h"
#include "../imgui_fonts.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

/* Bolo types (included after SDL3 to avoid #pragma pack conflicts) */
extern "C" {
#include "global.h"    /* BYTE, bool, FALSE/TRUE */
#include "client_sim.h" /* labelLen, lblNone/lblShort/lblLong via client_enums.h */
#include "wire_limits.h" /* PACKET_MAX_CHAT_MESSAGE */
#include "../gamefront.h"
#include "../lang.h"
#include "../sound.h"  /* soundPlayEffect — lobby game-start jingle (wasm seam) */
#if defined(WINBOLO_VOICE)
#include "../voice.h"  /* voiceGetTalkingMap / voiceIsPlayerMuted — mic icons */
#endif
}

/* Maps an uppercased alpha-2 code to its localized STR_COUNTRY_* name id
 * (generated; sorted by code for bsearch). Needs langid + STR_COUNTRY_*
 * from lang.h above. */
#include "countries.inc"

/* Our own header */
#include "sdl3imgui.h"
#include "input_gate.h"
#include "sdl3draw.h"
#include "overview_view.h"
#include "tileloader.h"
#include "luabrainshandler.h"
#include "flags.h"
#include "glyphs.h"
#include "dialogs/imgui_keycap.h"

extern "C" {
#include "client_net.h"
#include "../clientmutex.h" /* the overview's render reads sim state */
#include "../../server/server_lifecycle.h"
#include "../../server/threads.h"
}

/* Include input.h for keyItems — SDL3 already included, safe here */
extern "C" {
#include "input.h"
#include "input_touch.h"
#include "input_gamepad.h"
#include "input_source.h"
#include "../ui_mode.h"
#include "../../steam/steam_input_actions.h"
}

#include "sdl3imgui_tablet.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "dialogs/imgui_dialog_utils.h"
#include "dialogs/imgui_deck_pause.h"
#include "dialogs/imgui_tutorial_overlay.h"
#include "dialogs/imgui_keyboard.h"
#include "dialogs/imgui_quickchat.h"
#include "dialogs/imgui_controller_prompt.h"
#include "dialogs/imgui_controller_disconnect.h"
#include "imgui_steam_nav.h"
#include "dialogs/imgui_server_address.h"
#include "dialogs/dialog_footer.h"
#include "dialogs/imgui_keysetup.h"
#include "dialogs/imgui_settings.h"
#include "dialogs/imgui_about.h"
#include "dialogs/imgui_nav_outline.h"
#include "dialogs/imgui_lobby.h"
#include "platform/mac_menubar.h"

extern "C" void windowSetQuitting(void);
#ifdef __EMSCRIPTEN__
extern "C" void windowLeaveGame(void);
#endif

/* Network type enum values come from client_enums.h via client_sim.h */

/* -------------------------------------------------------
 * External C linkage: data-source functions for info windows.
 * Declared here to avoid pulling in headers with #pragma pack
 * after SDL3 is already compiled.
 * ------------------------------------------------------- */
extern "C" int  drawGetFrameRate(void);
extern "C" int  windowGetDrawTime(void);
extern "C" int  windowGetSimTime(void);
extern "C" int  windowGetNetTime(void);
extern "C" int  windowGetAiTime(void);
/* Dialog helpers — declared without pulling in pragma-pack headers */
/* gameFrontSetGameOptions: now provided by gamefront.h */
extern "C" void utilStripName(char *name);

/* Key setup helpers */
extern "C" void windowGetKeys(keyItems *value);
extern "C" void windowSetKeys(keyItems *value);
extern "C" bool useAutoslow;
extern "C" bool useAutohide;
extern "C" void windowKeyPressed(struct ClientSim *cs, int keyCode);
extern "C" void inputTouchSetAbsoluteSteering(bool enabled);
extern "C" bool inputTouchGetAbsoluteSteering(void);

/* -------------------------------------------------------
 * Frame-rate / zoom constants (mirrors winbolo.h values).
 * Re-defined here to avoid pulling in the full Win32
 * winbolo.h header.
 * ------------------------------------------------------- */
#ifndef FRAME_RATE_10
#define FRAME_RATE_10  19
#define FRAME_RATE_12  23
#define FRAME_RATE_15  28
#define FRAME_RATE_20  37
#define FRAME_RATE_30  55
#define FRAME_RATE_50  82
#define FRAME_RATE_60  111
#endif
#ifndef ZOOM_FACTOR_NORMAL
#define ZOOM_FACTOR_NORMAL 1
#define ZOOM_FACTOR_DOUBLE 2
#define ZOOM_FACTOR_TRIPLE 3
#define ZOOM_FACTOR_QUAD   4
#define ZOOM_FACTOR_CUSTOM 0
#endif

#ifndef MENU_BAR_HEIGHT
  #ifdef __APPLE__
    #define MENU_BAR_HEIGHT 0
  #else
    #define MENU_BAR_HEIGHT 22
  #endif
#endif

/* -------------------------------------------------------
 * External C linkage: function + globals defined in
 * winbolo.c / gamefront.c that we need for the menu bar.
 * ------------------------------------------------------- */

/* Direct menu command handlers in winbolo.c */
extern "C" void windowShowGunsight_toggle(struct ClientSim *cs);
extern "C" void windowAutomaticScrolling_toggle(struct ClientSim *cs);
extern "C" void windowSmoothScrolling_toggle(void);
extern "C" void windowShowPillLabels_toggle(struct ClientSim *cs);
extern "C" void windowShowBaseLabels_toggle(struct ClientSim *cs);
extern "C" void windowSoundEffects_toggle(void);
extern "C" void windowBackgroundSoundChange_toggle(void);
extern "C" void windowSoundKeepalive(void);
extern "C" void windowSetSoundVolume(int pct);
extern "C" void windowMenuAllowNewPlayers_toggle(struct ClientSim *cs);
extern "C" void windowMenuNewswire_toggle(struct ClientSim *cs);
extern "C" void windowMenuAssistant_toggle(struct ClientSim *cs);
extern "C" void windowMenuAI_toggle(struct ClientSim *cs);
extern "C" void windowMenuNetwork_toggle(struct ClientSim *cs);
extern "C" void windowMenuNetworkDebug_toggle(struct ClientSim *cs);
extern "C" void windowLabelOwnTank_toggle(struct ClientSim *cs);
extern "C" void imguiWinbolonetDrawSection(bool inGame);
extern "C" void imguiWinbolonetReset(void);
extern "C" void windowSetMessageLabelLen(struct ClientSim *cs, labelLen newLen);
extern "C" void windowSetTankLabelLen(struct ClientSim *cs, labelLen newLen);
extern "C" void windowSetFrameRate(int newFrameRate, bool setTimer);
extern "C" void windowZoomChange(BYTE amount, bool fromDragResize = false);
extern "C" void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                                int *outW, int *outH, int *outX, int *outY);
extern "C" void windowNewGame(void);
extern "C" void windowQuit(void);
extern "C" void windowFullScreenChoose(bool on);
extern "C" void windowSaveMap(struct ClientSim *cs);
extern "C" void windowSuspendBackground(struct ClientSim *cs);
extern "C" void windowResumeForeground(struct ClientSim *cs);
extern "C" void windowControllerLostPause(struct ClientSim *cs, bool active);
extern "C" void windowDeckPause(struct ClientSim *cs, bool active);
extern "C" void windowTutorialPause(struct ClientSim *cs, bool active);

extern "C" bool showGunsight;
extern "C" bool autoScrollingEnabled;
extern "C" bool smoothScrollingEnabled;
extern "C" bool letterboxBarsGray;
extern "C" bool showPillLabels;
extern "C" bool showBaseLabels;
extern "C" int  frameRate;
extern "C" labelLen labelMsg;
extern "C" labelLen labelTank;
extern "C" bool labelSelf;
extern "C" BYTE zoomFactor;
extern "C" bool allowNewPlayers;
extern "C" bool soundEffects;
extern "C" void soundSetMuted(bool mute);
extern "C" bool backgroundSound;
extern "C" bool useSoundKeepalive;
extern "C" int  soundVolume;
extern "C" bool showNewswireMessages;
extern "C" bool showAssistantMessages;
extern "C" bool showAIMessages;
extern "C" bool showNetworkStatusMessages;
extern "C" bool showNetworkDebugMessages;

/* Device presets are defined in imgui_dialog_utils.h (shared with dialogs) */
#include "dialogs/imgui_dialog_utils.h"
#include "dialogs/imgui_nav_outline.h"

/* -------------------------------------------------------
 * Module state
 * ------------------------------------------------------- */
static SDL_Window   *s_window   = nullptr;
static SDL_Renderer *s_renderer = nullptr;

/* A texture may only be drawn through the renderer that created it, and the
 * players pop-out window has its own renderer, so the icon textures and the
 * flag cache are kept per renderer. Two are enough: the game window, and the
 * one pop-out that draws textures. The other pop-outs draw only text, so they
 * never load a slot of their own.
 *
 * s_popOutRenderer is the renderer of the pop-out currently being drawn,
 * set by popOutBeginFrame and cleared by popOutEndFrame; NULL means the game
 * window is drawing. */
#define ICON_SLOT_MAIN   0
#define ICON_SLOT_POPOUT 1
#define ICON_SLOT_COUNT  2
static SDL_Renderer *s_popOutRenderer = nullptr;

/* The renderer the current draw goes to. */
static SDL_Renderer *activeRenderer(void) {
    if (s_popOutRenderer) return s_popOutRenderer;
    return s_renderer ? s_renderer : sdl3DrawGetRenderer();
}

/* Texture slot the current draw reads from. */
static int activeIconSlot(void) {
    return s_popOutRenderer ? ICON_SLOT_POPOUT : ICON_SLOT_MAIN;
}

/* UI scale applied to the main in-game ImGui context (font + style), set in
   sdl3ImguiSetup.  Dialog seed/min sizes and the window minimum multiply by
   this so they track the scaled font.  1.0 until setup runs. */
static float s_uiScale = 1.0f;

/* Brain settings window state */
static bool              s_brainSettingsOpen    = false;
static LuaBrainSetting  *s_brainSettings        = nullptr;
static int               s_brainSettingsCount   = 0;

/* Info panel visibility — rendered as ImGui windows within the main context */
static bool s_showSysInfo  = false;
static bool s_showNetInfo  = false;
static bool s_showGameInfo = false;
static bool s_showSendMsg  = false;
static bool s_showPlayersPanel = false;

/* Deferred zoom change — the reconfigure mutates the live renderer, so it must
   not run mid-frame; store the requested value and apply it after the frame
   ends. 255 = no pending change. */
static BYTE s_pendingZoom = 255;
static bool s_pendingZoomFromResize = false;  /* True if zoom change came from resize snap */

/* Deferred full screen change, for the same reason: -1 = no pending change,
   0 = leave full screen, 1 = enter it. */
static signed char s_pendingFullScreen = -1;

/* Suppress auto-switch to Custom on the next resize event.  Set before
   programmatic SDL_SetWindowSize so the resulting event doesn't trigger
   an unwanted mode change. */
static bool s_suppressAutoCustom = false;

/* True while user is dragging the window border (between WM_ENTERSIZEMOVE
   and WM_EXITSIZEMOVE).  Suppresses auto-switch to Custom during drag. */
static bool s_inModalResize = false;

/* Optional extra render callback (used by Android for players panel) */
static sdl3ImguiExtraRenderFn s_extraRenderFn = nullptr;

/* Set true when a mouse click lands outside all ImGui windows (game area).
   Cleared each frame after use to reset ImGui nav focus so that menu open/close
   does not restore focus to the Send Message window. */
static bool s_clearNavFocus = false;

/* Set when a panel/chat opens; the render loop retracts any open menu-bar
   dropdown on the next authority pass so the two don't render active at once. */
static bool s_closeMenuPopups = false;

/* Player slot state — updated by frontEndSetPlayer / frontEndClearPlayer */
#define MAX_PLAYERS 16
/* These rows index ClientSim state sized by MAX_TANKS (the ping band, for
 * one). A local define drifting past it would silently render every extra
 * slot grey rather than fail. */
static_assert(MAX_PLAYERS <= MAX_TANKS, "player rows exceed ClientSim slots");
static char     s_playerName[MAX_PLAYERS][PLAYER_NAME_LEN] = {};  /* display copy */
static char     s_playerCountry[MAX_PLAYERS][3] = {};      /* 2-char ISO country code + NUL */
static bool     s_playerEnabled[MAX_PLAYERS]  = {};
static bool     s_playerChecked[MAX_PLAYERS]  = {};
static uint16_t s_playerPing[MAX_PLAYERS] = {};
static uint8_t  s_playerClientType[MAX_PLAYERS] = {};
static uint8_t  s_playerFlags[MAX_PLAYERS] = {};

/* WBN/Steam icon textures. The WBN-verified shield is drawn procedurally
 * (imguiShieldBadge / imguiDrawSpinningShield) rather than from a texture, so
 * there is no s_iconWbnVerified — the Mac menubar loads its own copy of
 * shield.svg for native Cocoa drawing. */
static SDL_Texture *s_iconSteam[ICON_SLOT_COUNT] = {};
static SDL_Texture *s_iconBrain[ICON_SLOT_COUNT] = {};
/* Large brain rasterization used for tank-label overlays, kept as a surface
 * because the label caches texture it per renderer (main window, pop-out
 * overview). The small s_iconBrain is rasterized at WBN_ICON_SIZE for the
 * player-popup / renderPlayerName paths; sized up to a tank-label height
 * (~16-48 px depending on zoom) the small one looks soft because the SVG's
 * vector edges were already baked into a 14-px bitmap.
 * WBN_ICON_TANK_LABEL_SIZE rasterizes the same SVG at a height that covers
 * the realistic zoom range so the label-side blit is a (sharp) downscale
 * rather than an upscale. */
static SDL_Surface *s_iconBrainSurf = nullptr;
/* Skull for the players panel's death counter columns. Its own copy of
 * data/ui/skull.svg rather than the lobby's — that one lives in the lobby's
 * icon cache behind lobbyIcons(), which is lobby-internal. */
static SDL_Texture *s_iconSkull[ICON_SLOT_COUNT] = {};
#if defined(WINBOLO_VOICE)
/* Voice state icons for the players panel. Which shape is drawn says which
 * end the state belongs to: a speaker for the states about playback here —
 * a remote player idle, talking, or muted by this client — and a microphone
 * for the states about capture at the other end, no microphone or muted
 * their own. Talking and idle share the speaker under different tints,
 * because the difference between them is momentary and a shape change would
 * read as flicker. */
static SDL_Texture *s_iconMic[ICON_SLOT_COUNT]          = {};
static SDL_Texture *s_iconMicMuted[ICON_SLOT_COUNT]     = {};
static SDL_Texture *s_iconMicOff[ICON_SLOT_COUNT]       = {};
static SDL_Texture *s_iconSpeaker[ICON_SLOT_COUNT]      = {};
static SDL_Texture *s_iconSpeakerMuted[ICON_SLOT_COUNT] = {};
/* Tank-label rasterization of the talking speaker, a surface for the same
 * reason s_iconBrainSurf is one. */
static SDL_Surface *s_iconSpeakerSurf = nullptr;
/* Voice icon tints. Declared here rather than beside NO_TINT/SUPPORTER_TINT
 * further down the file because the players panel is rendered above them.
 * Talking is the only one that has to catch the eye mid-game; the rest sit
 * back so a panel full of idle rows is not a wall of colour. */
static const ImVec4 MIC_TINT_NORMAL  = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
static const ImVec4 MIC_TINT_TALKING = ImVec4(0.30f, 1.00f, 0.40f, 1.00f);
static const ImVec4 MIC_TINT_MUTED   = ImVec4(1.00f, 0.35f, 0.35f, 1.00f);
static const ImVec4 MIC_TINT_DIM     = ImVec4(1.00f, 1.00f, 1.00f, 0.40f);
/* What sdl3ImguiCreateMicIconSurface will rasterize at. Below the floor the
 * glyph is unreadable whatever we do; the ceiling is well past the largest
 * size the game view asks for and stops a degenerate layout turning into a
 * huge allocation. */
#define MIC_ICON_MIN_PX 8
#define MIC_ICON_MAX_PX 256
#endif
static bool s_wbnIconsLoaded[ICON_SLOT_COUNT] = {};
#define WBN_ICON_SIZE 14
#define WBN_ICON_TANK_LABEL_SIZE 48

static void ensureWbnIconsLoaded(void) {
    int slot = activeIconSlot();
    if (s_wbnIconsLoaded[slot]) return;
    s_wbnIconsLoaded[slot] = true;
    SDL_Renderer *r = activeRenderer();
    s_iconSteam[slot]   = imguiLoadSvgIconWhite(r, "data/ui/steam.svg", WBN_ICON_SIZE);
    s_iconBrain[slot]   = imguiLoadSvgIconWhite(r, "data/ui/brain.svg", WBN_ICON_SIZE);
    /* Outside the voice test below: the counter columns that draw this are
     * not a voice feature and ship in -DWINBOLO_VOICE=OFF builds too. */
    s_iconSkull[slot]   = imguiLoadSvgIconWhite(r, "data/ui/skull.svg", WBN_ICON_SIZE);
#if defined(WINBOLO_VOICE)
    s_iconMic[slot]          = imguiLoadSvgIconWhite(r, "data/ui/mic.svg",           WBN_ICON_SIZE);
    s_iconMicMuted[slot]     = imguiLoadSvgIconWhite(r, "data/ui/mic-muted.svg",     WBN_ICON_SIZE);
    s_iconMicOff[slot]       = imguiLoadSvgIconWhite(r, "data/ui/mic-off.svg",       WBN_ICON_SIZE);
    s_iconSpeaker[slot]      = imguiLoadSvgIconWhite(r, "data/ui/speaker.svg",       WBN_ICON_SIZE);
    s_iconSpeakerMuted[slot] = imguiLoadSvgIconWhite(r, "data/ui/speaker-muted.svg", WBN_ICON_SIZE);
#endif
    /* Renderer-free, so they are loaded once for every slot rather than
     * rasterized again per renderer. */
    if (!s_iconBrainSurf) {
        s_iconBrainSurf = imguiLoadSvgIconWhiteSurface("data/ui/brain.svg",
                                                       WBN_ICON_TANK_LABEL_SIZE);
    }
#if defined(WINBOLO_VOICE)
    if (!s_iconSpeakerSurf) {
        s_iconSpeakerSurf = imguiLoadSvgIconWhiteSurface("data/ui/speaker.svg",
                                                         WBN_ICON_TANK_LABEL_SIZE);
    }
#endif
    WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[WBN ICONS] slot=%d steam=%p brain=%p brainSurf=%p renderer=%p s_renderer=%p drawRenderer=%p",
            slot, (void *)s_iconSteam[slot],
            (void *)s_iconBrain[slot], (void *)s_iconBrainSurf,
            (void *)r, (void *)s_renderer, (void *)sdl3DrawGetRenderer());
}

/* Skull for the players panel's death counter columns, drawn square at
 * text height and tinted to the text colour so it sits with the other header
 * art rather than shouting. Returns false when the asset is missing, which is
 * the caller's cue to fall back to the column's written label. */
static bool playersPanelDrawSkull(void) {
    SDL_Texture *skull = s_iconSkull[activeIconSlot()];
    if (!skull) return false;
    float sz = ImGui::GetTextLineHeight();
    ImGui::ImageWithBg((ImTextureID)skull, ImVec2(sz, sz),
                       ImVec2(0, 0), ImVec2(1, 1),
                       ImVec4(0, 0, 0, 0),
                       ImGui::GetStyleColorVec4(ImGuiCol_Text));
    return true;
}

/* Platform icon textures, indexed by ClientType. UNKNOWN slot stays NULL. */
static SDL_Texture *s_iconPlatform[ICON_SLOT_COUNT][CLIENT_TYPE_COUNT] = {};
static bool s_platformIconsLoaded[ICON_SLOT_COUNT] = {};

static void ensurePlatformIconsLoaded(void) {
    int slot = activeIconSlot();
    if (s_platformIconsLoaded[slot]) return;
    s_platformIconsLoaded[slot] = true;
    SDL_Renderer *r = activeRenderer();
    /* Force white so platform icons read against the dark ImGui background
     * regardless of each SVG's authored fill (mac.svg=#888, windows.svg=#000…). */
    s_iconPlatform[slot][CLIENT_TYPE_UNKNOWN]   = nullptr;
    s_iconPlatform[slot][CLIENT_TYPE_WINDOWS]   = imguiLoadSvgIconWhite(r, "data/ui/windows.svg",    WBN_ICON_SIZE);
    s_iconPlatform[slot][CLIENT_TYPE_LINUX]     = imguiLoadSvgIconWhite(r, "data/ui/linux.svg",      WBN_ICON_SIZE);
    s_iconPlatform[slot][CLIENT_TYPE_MACOS]     = imguiLoadSvgIconWhite(r, "data/ui/mac.svg",        WBN_ICON_SIZE);
    s_iconPlatform[slot][CLIENT_TYPE_IOS]       = imguiLoadSvgIconWhite(r, "data/ui/ios.svg",        WBN_ICON_SIZE);
    s_iconPlatform[slot][CLIENT_TYPE_ANDROID]   = imguiLoadSvgIconWhite(r, "data/ui/android.svg",    WBN_ICON_SIZE);
    s_iconPlatform[slot][CLIENT_TYPE_STEAMDECK] = imguiLoadSvgIconWhite(r, "data/ui/steam-deck.svg", WBN_ICON_SIZE);
    s_iconPlatform[slot][CLIENT_TYPE_WEB]       = imguiLoadSvgIconWhite(r, "data/ui/globe.svg",      WBN_ICON_SIZE);
}

/* Free one renderer's copies of every icon and let them be loaded again.
 * Must run while that renderer is still alive. */
static void destroyIconSlot(int slot) {
    if (s_iconSteam[slot]) { SDL_DestroyTexture(s_iconSteam[slot]); s_iconSteam[slot] = nullptr; }
    if (s_iconBrain[slot]) { SDL_DestroyTexture(s_iconBrain[slot]); s_iconBrain[slot] = nullptr; }
    if (s_iconSkull[slot]) { SDL_DestroyTexture(s_iconSkull[slot]); s_iconSkull[slot] = nullptr; }
#if defined(WINBOLO_VOICE)
    if (s_iconMic[slot]) { SDL_DestroyTexture(s_iconMic[slot]); s_iconMic[slot] = nullptr; }
    if (s_iconMicMuted[slot]) { SDL_DestroyTexture(s_iconMicMuted[slot]); s_iconMicMuted[slot] = nullptr; }
    if (s_iconMicOff[slot]) { SDL_DestroyTexture(s_iconMicOff[slot]); s_iconMicOff[slot] = nullptr; }
    if (s_iconSpeaker[slot]) { SDL_DestroyTexture(s_iconSpeaker[slot]); s_iconSpeaker[slot] = nullptr; }
    if (s_iconSpeakerMuted[slot]) { SDL_DestroyTexture(s_iconSpeakerMuted[slot]); s_iconSpeakerMuted[slot] = nullptr; }
#endif
    s_wbnIconsLoaded[slot] = false;
    for (int i = 0; i < CLIENT_TYPE_COUNT; i++) {
        /* Entry may alias another (e.g. WEB → globe.svg), but each load returns a
         * distinct SDL_Texture so destroying every one is safe. */
        if (s_iconPlatform[slot][i]) {
            SDL_DestroyTexture(s_iconPlatform[slot][i]);
            s_iconPlatform[slot][i] = nullptr;
        }
    }
    s_platformIconsLoaded[slot] = false;
}

/* Settings panel state */
static bool s_showSettings       = false;
/* Set when the in-game UI-scale combo changes; the main render loop
   rebuilds the font atlas + style at a safe point (between Present and the
   next NewFrame) rather than mid-frame. */
static bool s_pendingUiScaleRebuild = false;
/* Set when the Settings Skin combo changes; the main render loop reloads the
   tile sheet and sound set at the same safe point, since gameFrontReloadSkins
   destroys and rebuilds the tile texture. */
static bool s_pendingSkinReload = false;
static bool s_wbnInitialised     = false;

/* Modal dialog state */
static bool s_closeAllPopups     = false;

static bool s_showChangeName     = false;
static char s_changeNameBuf[PLAYER_NAME_LEN] = "";

static bool s_showAllianceOpen   = false;
static char s_alliancePlayerName[PLAYER_NAME_LEN] = "";
static BYTE s_alliancePlayerNum  = 0;
static bool s_allianceVisible     = false;

static bool alliancePendingGet(const char **nameOut, BYTE *numOut) {
    if (!s_allianceVisible) return false;
    if (nameOut) *nameOut = s_alliancePlayerName;
    if (numOut)  *numOut  = s_alliancePlayerNum;
    return true;
}
static void allianceClearPending(void) { s_allianceVisible = false; }

static bool s_showPasswordOpen   = false;
static char s_passwordBuf[36]    = "";  /* MAP_STR_SIZE = 36 */

/* "Join Game?" confirmation when a winbolo:// URL is received mid-game */
static bool s_showJoinConfirm       = false;
static char s_joinConfirmUrl[512]   = "";
static char s_joinConfirmAddr[256]  = "";
static int  s_joinConfirmPort       = 0;

/* Key Setup modal state is owned by imgui_keysetup.cpp now —
 * trigger via imguiKeySetupOpenInGame, render each frame via
 * imguiKeySetupRenderInGamePopup, route key-capture scancodes
 * through imguiKeySetupHandleInGameScancode. This file used to
 * carry a parallel copy of the form + state; the two diverged
 * (Auto Slowdown not persisting from the in-game popup was the
 * symptom) so it was consolidated into the standalone dialog's
 * module. */

/* Gamepad rebind working state.  Mirrors s_keySetupKeys / s_keySetupWaiting:
   the dialog populates s_keySetupGamepadBindings on open, mutates it as
   the player rebinds, commits on OK, and discards on Cancel.  Waiting
   action is GP_ACT_COUNT when no capture is pending; otherwise the
   slot field selects which slot (primary or secondary) the next
   captured input writes to.  s_keySetupTrigArmed[] tracks whether
   each trigger needs to release-then-press to count (true when the
   trigger was already pulled at Change-click time, so the first
   cross-up-from-below is a real player action and not a spurious
   match against held state). */
static GamepadBindings s_keySetupGamepadBindings;
static int             s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
static GamepadSlot     s_keySetupGamepadWaitingSlot   = GP_SLOT_PRIMARY;
static bool            s_keySetupTrigArmed[2]         = { true, true };

/* Send Message panel state */
enum SendMsgRecipient { kSendAll = 0, kSendAllies, kSendNearby, kSendSelected };
static int    s_sendMsgRecipient  = kSendAll;
/* Sized to match the wire payload cap (PACKET_MAX_CHAT_MESSAGE bytes) used by
 * the CMD_CHAT body inside a PACKET_COMMAND_TICK frame (submitted via
 * clientSimSubmitCommand, applied by the CMD_CHAT dispatcher arm), + 1
 * for NUL. ImGui's InputText caps insertions at sizeof(buf) and rejects
 * a whole UTF-8 codepoint that would overflow rather than splitting it,
 * so this is the limit users see in the dialog too. */
static char   s_sendMsgBuf[PACKET_MAX_CHAT_MESSAGE + 1] = "";
static Uint64 s_sendMsgCooldownEnd = 0;   /* SDL_GetTicks() value; 0 = not in cooldown */
static bool   s_sendMsgFocusInput = false; /* Set true to focus the text input next frame */
static int    s_sendMsgFocusFrames = 0;
static bool   s_sendMsgHideNav = false;
static bool   s_showCtrlSendMsg    = false; /* controller-mode simplified send dialog open */
static bool   s_pendingCtrlSendMsg = false; /* deferred OpenPopup (must run during render) */
#define SEND_MSG_WAIT_MS 2000

/* Alliance request cooldown */
static Uint64 s_allianceReqCooldownEnd = 0; /* SDL_GetTicks() value; 0 = not in cooldown */
#define ALLIANCE_REQ_WAIT_MS 5000

bool sdl3ImguiAllianceReqInCooldown(void) {
    return (s_allianceReqCooldownEnd != 0 && SDL_GetTicks() < s_allianceReqCooldownEnd);
}

void sdl3ImguiNoteAllianceRequested(void) {
    s_allianceReqCooldownEnd = SDL_GetTicks() + ALLIANCE_REQ_WAIT_MS;
}

/* -------------------------------------------------------
 * Pop-out window support (desktop only)
 * Each pop-out gets its own SDL_Window + SDL_Renderer +
 * ImGui context so it can be moved independently.
 * ------------------------------------------------------- */
struct PopOutWindow {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    ImGuiContext  *imguiCtx;
    bool          open;
    int           width;
    int           height;
};

static PopOutWindow s_popSysInfo     = {};
static PopOutWindow s_popNetInfo     = {};
static PopOutWindow s_popGameInfo    = {};
static PopOutWindow s_popSendMsg     = {};
static PopOutWindow s_popPlayers     = {};
static PopOutWindow s_popMapOverview = {};

/* Every site that treats the pop-outs as a set — event routing, the
 * focus/mute check, cleanup — walks this table, so adding a pop-out means
 * adding it here and nowhere else. The per-frame pump stays unrolled
 * because each pop-out draws different content. */
static PopOutWindow *const s_popOuts[] = {
    &s_popSysInfo, &s_popNetInfo, &s_popGameInfo, &s_popSendMsg,
    &s_popPlayers, &s_popMapOverview
};
#define POPOUT_COUNT ((int)(sizeof(s_popOuts) / sizeof(s_popOuts[0])))

static ImGuiContext *s_mainImguiCtx = nullptr;

/* Map Overview drawing state. An SDL texture only works on the renderer that
   created it, so the shared game atlas (which belongs to the main window's
   renderer) cannot be blitted into the pop-out — the overview gets its own
   sheet on the pop-out's renderer, rebuilt whenever the shared atlas changes
   scale. Both this and the view are torn down in sdl3ImguiCleanup, ahead of
   the renderer they were made on. */
static OverviewView *s_overviewView          = nullptr;
/* What the render reads from the sim, filled under the client mutex and
   drawn from after it is released. Made and torn down with the view. */
static OverviewSnapshot *s_overviewSnapshot  = nullptr;
static SDL_Texture  *s_overviewTiles         = nullptr;
static SDL_Renderer *s_overviewTilesRenderer = nullptr;
static int           s_overviewTilesScale    = 0;
/* The tile-atlas build this sheet came from. A skin change rebuilds the
   atlas without changing the renderer or the scale, so those two alone
   would keep the pop-out on the old art. */
static unsigned int  s_overviewTilesGen      = 0;
/* The gunsight sprite, on the pop-out's renderer for the same reason. */
static SDL_Texture  *s_overviewCrosshair         = nullptr;
static SDL_Renderer *s_overviewCrosshairRenderer = nullptr;
/* Last frame's running state, so the start of a game can be told from the
   middle of one — see the auto-hide/reopen in sdl3ImguiPumpAndRender. */
static bool          s_overviewWasRunning    = false;

/* Whether the windows the player drives from (main window + Map Overview)
   held keyboard focus as of the end of the last event poll, and whether any
   focus event arrived during the current one.  Held keys are dropped only
   when that set as a whole gains or loses focus, so handing focus between
   the two windows keeps a key the player is still holding.  Seeded true
   because the main window is created and raised before events start
   flowing, so it owns focus by the time the first one is judged. */
static bool          s_gameInputHadFocus     = true;
static bool          s_focusEventThisPoll    = false;

static SDL_Texture *overviewEnsureTiles(SDL_Renderer *r) {
    if (!r) return nullptr;

    int want = sdl3DrawGetSheetScale();
    if (want < 1) want = 1;
    unsigned int gen = sdl3DrawGetTilesGeneration();
    /* Records the attempt, not just the result: a build that failed must not
       be retried — and the SVGs re-rasterized — on every frame after. */
    if (s_overviewTilesRenderer == r && s_overviewTilesScale == want &&
        s_overviewTilesGen == gen) {
        return s_overviewTiles;
    }

    if (s_overviewTiles) {
        SDL_DestroyTexture(s_overviewTiles);
        s_overviewTiles = nullptr;
    }
    s_overviewTilesRenderer = r;
    s_overviewTilesScale    = want;
    s_overviewTilesGen      = gen;

    SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X * want);
    if (!sheet) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                     "[Overview] tileLoaderBuildSheet failed");
        return nullptr;
    }
    s_overviewTiles = SDL_CreateTextureFromSurface(r, sheet);
    SDL_DestroySurface(sheet);
    if (!s_overviewTiles) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                     "[Overview] SDL_CreateTextureFromSurface failed: %s",
                     SDL_GetError());
        return nullptr;
    }

    /* Blend mode sticks; the sampler does not. The ImGui SDL3 backend sets the
       scale mode per draw, so anything set here is gone by the time the sheet
       is drawn — overviewViewRenderOffscreen re-asserts the player's Texture
       Filter setting on it every frame instead. */
    SDL_SetTextureBlendMode(s_overviewTiles, SDL_BLENDMODE_BLEND);
    return s_overviewTiles;
}

/* The pop-out's own crosshair. Keyed on the renderer alone — the PNG has one
   size, so there is nothing here matching the tile sheet's scale. Like the
   sheet, the attempt is recorded before it is made so a failed load is not
   retried every frame. */
static SDL_Texture *overviewEnsureCrosshair(SDL_Renderer *r) {
    if (!r) return nullptr;

    if (s_overviewCrosshairRenderer == r) {
        return s_overviewCrosshair;
    }

    if (s_overviewCrosshair) {
        SDL_DestroyTexture(s_overviewCrosshair);
        s_overviewCrosshair = nullptr;
    }
    s_overviewCrosshairRenderer = r;

    s_overviewCrosshair = sdl3DrawCreateCrosshairTexture(r);
    if (!s_overviewCrosshair) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET,
                     "[Overview] sdl3DrawCreateCrosshairTexture failed");
    }
    return s_overviewCrosshair;
}

static bool popOutCreate(PopOutWindow *pw, const char *title, int w, int h, Uint32 flags) {
    /* Re-show an existing pop-out rather than recreating it. We deliberately
     * keep the SDL_Window + Metal SDL_Renderer alive across closes: destroying
     * a Metal renderer mid-run releases Metal objects that the Steam overlay
     * (gameoverlayrenderer.dylib) has cached, and the overlay then messages the
     * freed object on the next present of the main window -> SIGSEGV. The
     * renderers are only torn down for real at shutdown (sdl3ImguiCleanup). */
    if (pw->window) {
        pw->open = true;
        SDL_ShowWindow(pw->window);
        SDL_RaiseWindow(pw->window);
        return true;
    }

    pw->window = SDL_CreateWindow(title, w, h, flags);
    if (!pw->window) return false;

#ifdef _WIN32
    /* Remove minimize/maximize buttons — leave only the close box. A
     * resizable pop-out keeps both: the maximize box is the normal way to
     * fill the screen with it, and stripping it would leave edge-dragging as
     * the only way to make the window bigger. */
    if ((flags & SDL_WINDOW_RESIZABLE) == 0) {
        HWND hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties(pw->window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            LONG style = GetWindowLong(hwnd, GWL_STYLE);
            style &= ~(WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
            SetWindowLong(hwnd, GWL_STYLE, style);
        }
    }
#endif

    pw->renderer = SDL_CreateRenderer(pw->window, NULL);
    if (!pw->renderer) {
        SDL_DestroyWindow(pw->window);
        pw->window = NULL;
        return false;
    }

    pw->imguiCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(pw->imguiCtx);
    imguiRegisterPlatformOpenUrl();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    imguiLoadBoloFont(18.0f * dialogDeckFontMul());
    ImGui_ImplSDL3_InitForSDLRenderer(pw->window, pw->renderer);
    ImGui_ImplSDLRenderer3_Init(pw->renderer);

    pw->open   = true;
    pw->width  = w;
    pw->height = h;

    ImGui::SetCurrentContext(s_mainImguiCtx);
    return true;
}

static void popOutDestroy(PopOutWindow *pw) {
    if (!pw->window) return;

    ImGuiContext *savedCtx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(pw->imguiCtx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(pw->imguiCtx);
    ImGui::SetCurrentContext(savedCtx);

    SDL_DestroyRenderer(pw->renderer);
    SDL_DestroyWindow(pw->window);
    *pw = {};
}

/* Runtime "close": hide the window and stop rendering it, but keep the
 * SDL_Renderer + ImGui context alive. See popOutCreate for why we must not
 * call SDL_DestroyRenderer while the app (and the Steam overlay) keeps
 * presenting the main window. */
static void popOutHide(PopOutWindow *pw) {
    if (!pw->window || !pw->open) return;
    pw->open = false;
    /* A pop-out the player took full screen owns a macOS Space of its own,
     * and hiding it there leaves that Space behind with nothing in it — the
     * raise below then carries focus off to wherever the main window is,
     * across an empty screen. Drop back to windowed first, and wait for it:
     * the raise would otherwise race the transition. */
    if (SDL_GetWindowFlags(pw->window) & SDL_WINDOW_FULLSCREEN) {
        SDL_SetWindowFullscreen(pw->window, false);
        SDL_SyncWindow(pw->window);
    }
    SDL_HideWindow(pw->window);
    /* Hiding the pop-out leaves keyboard focus orphaned (notably on macOS,
     * where the OS does not auto-return key status to the main window), so
     * explicitly raise the main game window back to the front/focus. */
    if (s_window) SDL_RaiseWindow(s_window);
}

/* True while the window manager owns the pop-out's size and position — full
 * screen or zoomed. Neither is geometry the player chose, so neither is
 * remembered. */
static bool popOutGeometryIsOsManaged(const PopOutWindow *pw) {
    if (!pw->window) return false;
    return (SDL_GetWindowFlags(pw->window) &
            (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) != 0;
}

/* Whether a remembered overview rect can still be handed back. A rect saved
 * from before the full screen guard in the resize handler is the display's
 * own — as big as the display, at its origin, which on macOS is up under the
 * menu bar — and handing that back strands the window with no title bar to
 * grab and no close box to click. Both halves of that shape are what is
 * tested, against the usable area rather than the full display: the part
 * clear of the menu bar and the dock. A window the player has dragged
 * part-way off the right or bottom edge is their own doing and is still
 * remembered. A prefs file already carrying a bad rect recovers here. */
static bool overviewSavedGeometryUsable(int x, int y, int w, int h) {
    SDL_Point pt = { x, y };
    SDL_DisplayID disp = SDL_GetDisplayForPoint(&pt);
    if (!disp) return false;
    SDL_Rect usable;
    if (!SDL_GetDisplayUsableBounds(disp, &usable)) return false;
    if (w > usable.w || h > usable.h) return false;
    return x >= usable.x && y >= usable.y;
}

static bool popOutBeginFrame(PopOutWindow *pw) {
    if (!pw->open || !pw->window) return false;

    /* Everything drawn from here to popOutEndFrame goes to this renderer, so
     * the icon loaders and the flag cache use its slot rather than the game
     * window's. */
    s_popOutRenderer = pw->renderer;

    ImGui::SetCurrentContext(pw->imguiCtx);
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    return true;
}

static void popOutBeginContent(void) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##content", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoDocking);
}

static void popOutEndContent(PopOutWindow *pw) {
    ImGui::End();
}

static void popOutEndFrame(PopOutWindow *pw) {
    dialogDrawNavOutline();
    ImGui::EndFrame();
    dialogDrawNavOutline();
    ImGui::Render();
    SDL_SetRenderDrawColor(pw->renderer, 30, 30, 30, 255);
    SDL_RenderClear(pw->renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), pw->renderer);
    SDL_RenderPresent(pw->renderer);
    s_popOutRenderer = nullptr;
}

static void togglePopOut(PopOutWindow *pw, const char *title, int w, int h, Uint32 flags) {
    if (pw->open) {
        popOutHide(pw);
    } else {
        popOutCreate(pw, title, w, h, flags);
    }
}

/* Classic mode as last seen from the connected server, refreshed once a
 * frame in sdl3ImguiPumpAndRender. A file static rather than a ClientSim
 * read at each site because the two suppression points below are reached
 * from callers that have no ClientSim in hand. Spectators are exempt: they
 * are on the delayed god-view stream and are not competing. */
static bool s_classicMode = false;

static bool classicModeActive(void) { return s_classicMode; }

/* The overview is the one pop-out whose geometry is remembered, so every
 * place that opens it comes through here rather than calling popOutCreate
 * with a fixed size. A saved size below 200 px is treated as junk and
 * replaced by the default — the window would be too small to read a map in.
 *
 * The window is created hidden so a restored position can be applied before
 * it is ever shown; without that it would appear at the OS default and jump.
 * That only applies to the first create: popOutCreate's re-show path keeps
 * the window the player last dragged, position included.
 *
 * The saved position is honoured only while SDL still finds a display under
 * it, so a monitor that has been unplugged since the last run cannot strand
 * the window off-screen. Same test the main window does in winbolo.c. */
static void mapOverviewOpen(void) {
    /* Full screen mode owns the whole window and draws the same map itself,
       so the pop-out never opens while it is on — in a game, in the lobby or
       in the menus. The test is the app flag rather than the in-window view
       because the view only comes up once a game is running, and the
       game-start reopen runs ahead of it. gameFrontShowMapOverview is left
       alone: a player who had the pop-out flagged to reopen gets it back the
       moment they are back in classic mode. */
    if (gameFrontFullScreen) return;
    /* The server is holding the player in the classic framed view, so the
       pop-out does not open while that lasts. gameFrontShowMapOverview is
       left alone for the same reason as above: it is the player's own
       preference and it hands the pop-out back on the next server. */
    if (classicModeActive()) return;
    bool firstCreate = (s_popMapOverview.window == nullptr);
    int w = gameFrontOverviewW;
    int h = gameFrontOverviewH;
    if (w < 200) w = 640;
    if (h < 200) h = 640;
    /* Only a remembered rect is checked: with no position saved there is
       nothing to check it against, and the size alone cannot strand the
       window. */
    bool havePos = (gameFrontOverviewX >= 0 && gameFrontOverviewY >= 0);
    if (havePos && !overviewSavedGeometryUsable(gameFrontOverviewX,
                                                gameFrontOverviewY, w, h)) {
        w = 640;
        h = 640;
        havePos = false;
    }
    Uint32 flags = SDL_WINDOW_RESIZABLE |
                   (firstCreate ? SDL_WINDOW_HIDDEN : 0);
    if (!popOutCreate(&s_popMapOverview,
                      langGetText(STR_MENU_MAP_OVERVIEW), w, h, flags))
        return;
    if (firstCreate) {
        if (havePos) {
            SDL_SetWindowPosition(s_popMapOverview.window,
                                  gameFrontOverviewX, gameFrontOverviewY);
        }
        SDL_ShowWindow(s_popMapOverview.window);
        SDL_RaiseWindow(s_popMapOverview.window);
    }
    gameFrontShowMapOverview = true;
}

/* Every path that takes the overview off screen comes through here, so the
 * pointer the view may have switched to the game crosshair is always handed
 * back. overviewViewHandleInput only restores it when the pointer leaves the
 * map, which never happens when the window goes away underneath it. */
static void mapOverviewHide(void) {
    if (s_popMapOverview.open) popOutHide(&s_popMapOverview);
    overviewViewReleaseCursor(s_overviewView);
}

/* An explicit close: the window goes away and is not brought back with the
 * next game. The auto-hide at the end of a game deliberately does not come
 * through here — see the comment there. */
static void mapOverviewClose(void) {
    mapOverviewHide();
    gameFrontShowMapOverview = false;
}

/* Turn the in-window overview on or off. The mode takes the main window
 * fullscreen so the map gets the whole screen; leaving drops the window back
 * to windowed only when the app full screen flag is off, because with it on
 * the window stays full screen for the lobby and the menus. The window call
 * sits outside the mode test on purpose: a flag change with no mode change
 * still has to be able to move the window. */
static void overviewInWindowSet(bool on) {
    SDL_Window *win = sdl3DrawGetWindow();
    if (on && !win) return;
    if (on != sdl3DrawIsOverviewInWindow()) {
        sdl3DrawSetOverviewInWindow(on);  /* hands the OS pointer back on the way out */
    }
    if (win) {
        SDL_SetWindowFullscreen(win, on || gameFrontFullScreen);
        /* SDL_SetWindowFullscreen is asynchronous on Wayland and X11 and the
           frame that follows reads the window geometry, so wait for the
           transition here — the same reason windowFullScreenChoose syncs. */
        SDL_SyncWindow(win);
    }
}

/* The player asking for the mode, on or off, which is what the next game
 * brings back. The auto-exit at the end of a game and the teardown in
 * sdl3ImguiCleanup call overviewInWindowSet directly instead: an automatic
 * exit must not forget that the player wanted the mode. The flag is assigned
 * first because overviewInWindowSet reads it — turning the mode off in game
 * has to clear it before the call or the window never leaves full screen. */
static void overviewInWindowChoose(bool on) {
    /* Classic mode refuses entry only. Leaving still has to work, or a
       player already in the map view when they joined would be stuck in it.
       Ahead of the assignment below on purpose: gameFrontFullScreen is the
       player's own preference and must survive a classic-mode server. */
    if (on && classicModeActive()) return;
    gameFrontFullScreen = on;
    overviewInWindowSet(on);
    /* The two views of the map never share the screen, so the pop-out swaps
       with the mode. Going full screen puts it away without forgetting it;
       coming back to classic mode hands it straight back at the size and
       place it was left — the window it had, or a fresh one at the saved
       geometry when this run never opened it. Only in a game: outside one
       the pop-out is already down and gameFrontShowMapOverview is what the
       next game reads. */
    if (on) mapOverviewHide();
    else if (gameFrontShowMapOverview && s_overviewWasRunning) mapOverviewOpen();
    /* The player's own choice, so it survives the run — the same save
       windowFullScreenChoose makes for the same flag on the screens outside
       a game. The auto-exit and the cleanup path call overviewInWindowSet
       directly and deliberately never reach this. */
    gameFrontSaveCurrentPrefs();
}

/* True while the info panels and Send Message are drawn in the main window
 * *as stand-ins for their pop-outs*: the overview owns the game window, so a
 * separate OS window would land behind the map the player is looking at and
 * each panel opens in-window for the duration.
 *
 * Deliberately narrower than "the panel is in-window". The Settings > Session
 * Info entries open the same in-window panels on every platform and mode, and
 * on the Deck — no menu bar under a controller — that is the only way to
 * reach them; those are the panel's real form, not a stand-in, and the rules
 * keyed on this must leave them exactly as they were. False on the tablet /
 * mobile / web builds for the same reason: they have no pop-outs at all. */
static bool panelsStandInForPopOuts(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    return !uiModeIsTablet() && sdl3DrawIsOverviewInWindow();
#else
    return false;
#endif
}

/* -------------------------------------------------------
 * System Info panel
 * ------------------------------------------------------- */

/* Tick / brain timing graph state — only populated while
 * the System Info dialog is open. Sampled once per second
 * to match the ping graph's cadence. */
#define SYS_GRAPH_SIZE 120   /* ~2 minutes at 1 sample/sec */
static float    s_tickHistory[SYS_GRAPH_SIZE];
static float    s_brainHistory[SYS_GRAPH_SIZE];
static int      s_sysHistoryOffset = 0;
static int      s_sysHistoryCount  = 0;
static uint32_t s_sysLastSampleTick = 0;

static void sysInfoGraphReset(void) {
    memset(s_tickHistory, 0, sizeof(s_tickHistory));
    memset(s_brainHistory, 0, sizeof(s_brainHistory));
    s_sysHistoryOffset = 0;
    s_sysHistoryCount  = 0;
    s_sysLastSampleTick = 0;
}

/* Sample once per second. tickMs and brainPhaseMs are the
 * "last" values from server_lifecycle / bot_manager — already
 * updated each tick by the server timer thread. */
static void sysInfoGraphSample(double tickMs, double brainPhaseMs) {
    uint32_t now = SDL_GetTicks();
    if (now - s_sysLastSampleTick < 1000 && s_sysHistoryCount > 0) return;
    s_sysLastSampleTick = now;
    s_tickHistory[s_sysHistoryOffset]  = (float)tickMs;
    s_brainHistory[s_sysHistoryOffset] = (float)brainPhaseMs;
    s_sysHistoryOffset = (s_sysHistoryOffset + 1) % SYS_GRAPH_SIZE;
    if (s_sysHistoryCount < SYS_GRAPH_SIZE) s_sysHistoryCount++;
}

static void renderSysInfoContent(void) {
    float drawPct  = (windowGetDrawTime() / 1000.0f) * 100.0f;
    float simPct   = (windowGetSimTime()  / 1000.0f) * 100.0f;
    float netPct   = (windowGetNetTime()  / 1000.0f) * 100.0f;
    float aiPct    = (windowGetAiTime()   / 1000.0f) * 100.0f;
    float totalPct = drawPct + simPct + netPct + aiPct;

    ImGui::Text("%s %d fps", langGetText(STR_DLGSYSINFO_FRAMERATE), drawGetFrameRate());
    ImGui::Separator();
    ImGui::TextUnformatted(langGetText(STR_DLGSYSINFO_CPUUSAGE));
    ImGui::Text("  %s %.2f %%", langGetText(STR_DLGSYSINFO_GRAPHICS), drawPct);
    ImGui::Text("  %s %.2f %%", langGetText(STR_DLGSYSINFO_SIMMODELING), simPct);
    ImGui::Text("  %s %.2f %%", langGetText(STR_DLGSYSINFO_COMPROCESSING), netPct);
    ImGui::Text("  %s %.2f %%", langGetText(STR_DLGSYSINFO_AITANKS), aiPct);
    ImGui::Separator();
    ImGui::Text("  %s %.2f %%", langGetText(STR_DLGSYSINFO_TOTAL), totalPct);

    /* Server-side bot/sim telemetry. Only present when a local
     * server sim is ticking (single-player or local listen-server). */
    ServerSim *spSim = gameFrontGetServerSim();
    if (spSim != NULL) {
        BotPoolStats ps = {};
        BotInfo  botInfos[MAX_TANKS] = {};
        bool     botInfoValid[MAX_TANKS] = {};
        double tickLast = 0.0, tickEwma = 0.0;
        double simLast  = 0.0, simEwma  = 0.0;
        bool   hasBots  = false;

        /* The server timer thread writes these file-statics every
         * tick. Take the same mutex serverInstanceTick uses so the
         * snapshot is consistent. Cheap — these are quick reads. */
        threadsWaitForMutex();
        hasBots = serverSimHasAnyBot(spSim);
        serverSimGetBotPoolStats(spSim, &ps);
        for (int i = 0; i < MAX_TANKS; i++) {
            botInfoValid[i] = serverSimGetBotInfo(spSim, (BYTE)i, &botInfos[i]);
        }
        serverLifecycleGetTickStats(&tickLast, &tickEwma);
        serverLifecycleGetSimStats(&simLast, &simEwma);
        threadsReleaseMutex();

        sysInfoGraphSample(tickLast, ps.lastBrainPhaseMs);

        ImGui::Separator();
        ImGui::TextUnformatted(langGetText(STR_DLGSYSINFO_SERVER));

        if (hasBots) {
            if (ps.workerCount == 0) {
                ImGui::Text("%s: single-thread (%d active bots), target=%.1fms/bot",
                            langGetText(STR_DLGSYSINFO_BOTPOOL),
                            ps.activeBots, ps.currentTargetMs);
            } else {
                ImGui::Text("%s: %d workers (%d active bots), target=%.1fms/bot",
                            langGetText(STR_DLGSYSINFO_BOTPOOL),
                            ps.workerCount, ps.activeBots, ps.currentTargetMs);
            }
        }

        /* Tick wall-clock chart. Show min/avg/max over the
         * sample window above the plot, like the ping graph
         * does. Y-axis capped so the 20ms budget is visible
         * within frame even when ticks are well under it. */
        if (s_sysHistoryCount > 1) {
            float minTick = s_tickHistory[0];
            float maxTick = s_tickHistory[0];
            float sumTick = 0;
            for (int i = 0; i < s_sysHistoryCount; i++) {
                float v = s_tickHistory[i];
                if (v < minTick) minTick = v;
                if (v > maxTick) maxTick = v;
                sumTick += v;
            }
            float avgTick = sumTick / (float)s_sysHistoryCount;
            float plotMax = (maxTick > 25.0f) ? maxTick * 1.2f : 25.0f;
            ImGui::Text("%s: min=%.1fms avg=%.1fms max=%.1fms (budget=20ms)",
                        langGetText(STR_DLGSYSINFO_TICK),
                        minTick, avgTick, maxTick);
            ImGui::PlotLines("##tick", s_tickHistory, s_sysHistoryCount,
                             s_sysHistoryOffset, nullptr,
                             0.0f, plotMax,
                             ImVec2(ImGui::GetContentRegionAvail().x, 60));
        }

        /* Brain phase chart — only when there are bots. */
        if (hasBots && s_sysHistoryCount > 1) {
            float maxBrain = 0;
            for (int i = 0; i < s_sysHistoryCount; i++) {
                if (s_brainHistory[i] > maxBrain) maxBrain = s_brainHistory[i];
            }
            if (maxBrain < 1.0f) maxBrain = 1.0f;
            ImGui::Text("%s: EWMA=%.1fms",
                        langGetText(STR_DLGSYSINFO_BRAIN),
                        ps.ewmaBrainPhaseMs);
            ImGui::PlotLines("##brain", s_brainHistory, s_sysHistoryCount,
                             s_sysHistoryOffset, nullptr,
                             0.0f, maxBrain * 1.2f,
                             ImVec2(ImGui::GetContentRegionAvail().x, 40));
        }

        if (hasBots) {
            ImGui::Text("%s: %u",
                        langGetText(STR_DLGSYSINFO_BRAIN_OVERRUNS),
                        ps.totalOverruns);
            if (ps.totalOverruns > 0) {
                for (int i = 0; i < MAX_TANKS; i++) {
                    if (botInfoValid[i] && botInfos[i].overrunCount > 0) {
                        ImGui::Text("  bot[%d]: %u", i, botInfos[i].overrunCount);
                    }
                }
            }
        }

        /* Text-only stats for the remaining metrics. simLast > 0
         * means at least one running-state tick has happened —
         * suppress the line otherwise. */
        if (simLast > 0.0) {
            ImGui::Text("%s: last=%.1fms EWMA=%.1fms",
                        langGetText(STR_DLGSYSINFO_SIMULATION),
                        simLast, simEwma);
        }
        if (hasBots) {
            ImGui::Text("%s: last=%.1fms EWMA=%.1fms",
                        langGetText(STR_DLGSYSINFO_BOTPREP),
                        ps.lastSerialMs, ps.ewmaSerialMs);
        }
    }
}

static void renderSysInfoPanel(void) {
    if (!s_showSysInfo || s_popSysInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(440 * s_uiScale, 600 * s_uiScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280 * s_uiScale, 200 * s_uiScale),
                                        ImVec2(FLT_MAX, FLT_MAX));
    char title[128];
    snprintf(title, sizeof(title), "%s###sysinfo", langGetText(STR_DLGSYSINFO_TITLE));
    if (!ImGui::Begin(title, &s_showSysInfo)) {
        ImGui::End();
        return;
    }
    renderSysInfoContent();
    ImGui::End();
}

/* -------------------------------------------------------
 * Network Info panel
 * ------------------------------------------------------- */

/* Ping graph state — only populated while the dialog is open */
#define PING_GRAPH_SIZE 120  /* ~2 minutes at ~1 sample/sec */
static float s_pingHistory[PING_GRAPH_SIZE];
static int   s_pingHistoryOffset = 0;  /* Next write position (circular) */
static int   s_pingHistoryCount  = 0;  /* Total samples written */
static uint32_t s_pingLastSampleTick = 0;

/* KB/s graph state */
static float s_kbInHistory[PING_GRAPH_SIZE];
static float s_kbOutHistory[PING_GRAPH_SIZE];
static int   s_kbHistoryOffset = 0;
static int   s_kbHistoryCount  = 0;

static void pingGraphReset(void) {
    memset(s_pingHistory, 0, sizeof(s_pingHistory));
    s_pingHistoryOffset = 0;
    s_pingHistoryCount  = 0;
    s_pingLastSampleTick = 0;
    memset(s_kbInHistory, 0, sizeof(s_kbInHistory));
    memset(s_kbOutHistory, 0, sizeof(s_kbOutHistory));
    s_kbHistoryOffset = 0;
    s_kbHistoryCount  = 0;
}

static void pingGraphSample(int pingMs, int bpsIn, int bpsOut) {
    uint32_t now = SDL_GetTicks();
    /* Sample roughly once per second */
    if (now - s_pingLastSampleTick < 1000 && s_pingHistoryCount > 0) return;
    s_pingLastSampleTick = now;
    s_pingHistory[s_pingHistoryOffset] = (float)pingMs;
    s_pingHistoryOffset = (s_pingHistoryOffset + 1) % PING_GRAPH_SIZE;
    if (s_pingHistoryCount < PING_GRAPH_SIZE) s_pingHistoryCount++;

    s_kbInHistory[s_kbHistoryOffset] = (float)bpsIn / 1024.0f;
    s_kbOutHistory[s_kbHistoryOffset] = (float)bpsOut / 1024.0f;
    s_kbHistoryOffset = (s_kbHistoryOffset + 1) % PING_GRAPH_SIZE;
    if (s_kbHistoryCount < PING_GRAPH_SIZE) s_kbHistoryCount++;
}

static void renderNetInfoContent(ClientSim *cs) {
    char str[256];
    int  ping = 0, ppsec = 0, numErrors = 0;
    int  ppsIn = 0, ppsOut = 0;
    int  bpsIn = 0, bpsOut = 0;
    int  snapshotsRecv = 0, snapshotsLost = 0, snapshotsLostTotal = 0;

    /* Server address: show the real IP/host as a clickable join-link when in a
     * networked game (reverse-DNS resolved asynchronously, visual only). Falls
     * back to the legacy label ("Single Player Game") otherwise. */
    {
        char dispIp[64];
        unsigned dispPort = 0;
        if (guiServerDisplayAddress(cs, dispIp, sizeof(dispIp), &dispPort)) {
            ImGui::TextUnformatted(langGetText(STR_DLGNETINFO_SERVER));
            ImGui::SameLine();
            guiServerAddressLink(cs, dispIp, dispPort);
        } else {
            netGetServerAddressStr(cs, str);
            ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_SERVER), str);
        }
    }

    /* Client in a networked game: prepend player location to port */
    if (clientSimGetNetType(cs) != netSingle) {
        char addr[256];
        clientSimGetPlayerLocation(cs, clientSimGetMyPlayerNum(cs), addr,
                                   sizeof(addr));
        netGetOurAddressStr(cs, str);
        const char *portPart = strchr(str, ':');
        if (portPart) {
            size_t addrLen = strlen(addr);
            strncat(addr, portPart, sizeof(addr) - addrLen - 1);
        }
        ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_THISGAME), addr);
    } else {
        netGetOurAddressStr(cs, str);
        ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_THISGAME), str);
    }

    netGetStats(cs, str, &ping, &ppsec, &numErrors);
    /* Prefer stats from new UDP transport when active */
    if (clientSimHasTransport(cs)) {
        uint16_t udpPing = clientSimGetNetPing(cs);
        if (udpPing > 0) ping = (int)udpPing;
        int udpErrors = 0;
        clientSimGetUdpNetStats(cs, &ppsIn, &ppsOut, &bpsIn, &bpsOut, &udpErrors,
                                &snapshotsRecv, &snapshotsLost, &snapshotsLostTotal);
        numErrors = udpErrors;
    }
    ImGui::Separator();
    ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_STATUS), str);
    {
        MessageArgs args = {};
        args.number = ping;
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_SERVERPING, &args));
    }
    {
        MessageArgs args = {};
        args.number = ppsIn;
        args.number2 = ppsOut;
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_PACKETS_RATE, &args));
    }
    {
        MessageArgs args = {};
        SDL_snprintf(args.string1, sizeof(args.string1), "%.1f", (float)bpsIn / 1024.0f);
        SDL_snprintf(args.string2, sizeof(args.string2), "%.1f", (float)bpsOut / 1024.0f);
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_KB_RATE, &args));
    }
    {
        MessageArgs args = {};
        args.number = numErrors;
        args.number2 = clientSimHasTransport(cs) ? clientSimGetMapResyncCount(cs) : 0;
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_ERRORS, &args));
    }
    /* Inbound snapshot loss.  Computed from serverTick gaps — counts
     * snapshots the wire dropped before reaching us, distinct from the
     * cumulative "errors" line above.  Shows current 1-second window
     * (percent + raw counts) plus a game-long lost count. */
    {
        int total = snapshotsRecv + snapshotsLost;
        MessageArgs args = {};
        if (total > 0) {
            args.number = (snapshotsLost * 100 + total / 2) / total;
        } else {
            args.number = 0;
        }
        args.number2 = snapshotsLost;
        args.number3 = total;
        args.number4 = snapshotsLostTotal;
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_LOSS, &args));
    }

    /* Client prediction reconciliations for the last 1-second window —
     * count plus average/peak position error. */
    if (clientSimHasTransport(cs)) {
        int count = 0;
        float avgErrPx = 0.0f, maxErrPx = 0.0f, renderOffsetPx = 0.0f;
        clientSimGetReconcileStats(cs, &count, &avgErrPx, &maxErrPx, &renderOffsetPx);
        MessageArgs args = {};
        args.number = count;
        SDL_snprintf(args.string1, sizeof(args.string1), "%.1f", avgErrPx);
        SDL_snprintf(args.string2, sizeof(args.string2), "%.1f", maxErrPx);
        SDL_snprintf(args.string3, sizeof(args.string3), "%.1f", renderOffsetPx);
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_RECONCILE, &args));
    }

    /* Measurement-only timing estimator (client_timing). Dev-internal
     * readout to validate the estimates on real / -netimpair links before
     * anything consumes them — plain literals, not localized. */
    if (clientSimHasTransport(cs)) {
        int clockOffsetTicks = 0, jitterMs = 0, timingRttMs = 0, depthTicks = 0;
        clientSimGetTimingStats(cs, &clockOffsetTicks, &jitterMs, &timingRttMs,
                                &depthTicks);
        ImGui::Text("Timing: rtt %dms  jitter %dms", timingRttMs, jitterMs);
        ImGui::Text("  clock off %dt  pipe depth %dt", clockOffsetTicks,
                    depthTicks);
    }

    /* Ping graph */
    pingGraphSample(ping, bpsIn, bpsOut);
    if (s_pingHistoryCount > 1) {
        float minPing = s_pingHistory[0], maxPing = s_pingHistory[0], sumPing = 0;
        for (int i = 0; i < s_pingHistoryCount; i++) {
            float v = s_pingHistory[i];
            if (v < minPing) minPing = v;
            if (v > maxPing) maxPing = v;
            sumPing += v;
        }
        float avgPing = sumPing / (float)s_pingHistoryCount;
        if (maxPing < 10) maxPing = 10;

        ImGui::Separator();
        {
            MessageArgs args = {};
            args.number = (int)minPing;
            args.number2 = (int)avgPing;
            args.number3 = (int)maxPing;
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_PINGGRAPH, &args));
        }
        ImGui::PlotLines("##ping", s_pingHistory, s_pingHistoryCount,
                         s_pingHistoryOffset, nullptr,
                         0.0f, maxPing * 1.2f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 60));
    }

    /* KB/s graph */
    if (s_kbHistoryCount > 1) {
        float maxKb = 0;
        for (int i = 0; i < s_kbHistoryCount; i++) {
            if (s_kbInHistory[i] > maxKb) maxKb = s_kbInHistory[i];
            if (s_kbOutHistory[i] > maxKb) maxKb = s_kbOutHistory[i];
        }
        if (maxKb < 1.0f) maxKb = 1.0f;

        ImGui::Separator();
        ImGui::TextUnformatted(langGetText(STR_DLGNETINFO_KBIN));
        ImGui::PlotLines("##kbin", s_kbInHistory, s_kbHistoryCount,
                         s_kbHistoryOffset, nullptr,
                         0.0f, maxKb * 1.2f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 40));
        ImGui::TextUnformatted(langGetText(STR_DLGNETINFO_KBOUT));
        ImGui::PlotLines("##kbout", s_kbOutHistory, s_kbHistoryCount,
                         s_kbHistoryOffset, nullptr,
                         0.0f, maxKb * 1.2f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 40));
    }
}

static void renderNetInfoPanel(ClientSim *cs) {
    if (!s_showNetInfo || s_popNetInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(360 * s_uiScale, 420 * s_uiScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280 * s_uiScale, 200 * s_uiScale),
                                        ImVec2(FLT_MAX, FLT_MAX));
    char title[128];
    snprintf(title, sizeof(title), "%s###netinfo", langGetText(STR_DLGNETINFO_TITLE));
    if (!ImGui::Begin(title, &s_showNetInfo)) {
        ImGui::End();
        return;
    }
    renderNetInfoContent(cs);
    ImGui::End();
}

/* -------------------------------------------------------
 * Game Info panel
 * ------------------------------------------------------- */
static void renderGameInfoContent(ClientSim *cs) {
    char mapName[256];
    mapName[0] = '\0';
    strcpy(mapName, clientSimGetMapName(cs));
    ImGui::Text("%s%s", langGetText(STR_DLGGAMEINFO_MAPNAME), mapName);
    if (strncmp(mapName, "rand_", 5) == 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(langGetText(STR_DLGGAMEINFO_COPYSEED))) {
            SDL_SetClipboardText(mapName + 5);
        }
        imguiHandOnHover();
    }
    {
        MessageArgs args = {};
        args.number = (int)clientSimGetNumPlayers(cs);
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGGAMEINFO_NUMPLAYERS, &args));
    }

    gameType gt = clientSimGetGameType(cs);
    langid gtStr = STR_DLGGAMEINFO_STRICT;
    if      (gt == gameOpen)       gtStr = STR_DLGGAMEINFO_OPEN;
    else if (gt == gameTournament) gtStr = STR_DLGGAMEINFO_TOURN;
    ImGui::Text("%s%s", langGetText(STR_DLGGAMEINFO_GAMETYPE), langGetText(gtStr));

    ImGui::Text("%s%s", langGetText(STR_DLGGAMEINFO_HIDDENMINES),
                clientSimGetAllowHiddenMines(cs) ? langGetText(STR_YES) : langGetText(STR_NO));

    aiType ai = clientSimGetAiType(cs);
    langid aiStr = STR_NO;
    if      (ai == aiYes)          aiStr = STR_YES;
    else if (ai == aiYesAdvantage) aiStr = STR_DLGGAMEINFO_AIADV;
    else if (ai == aiFull)         aiStr = STR_DLGGAMEINFO_FULLADV;
    ImGui::Text("%s %s", langGetText(STR_DLGGAMEINFO_AILABEL), langGetText(aiStr));

    long timeLeft = clientSimGetGmeLength(cs);
    if (timeLeft == UNLIMITED_GAME_TIME) {
        ImGui::Text("%s %s", langGetText(STR_DLGGAMEINFO_TIMELIMIT),
                    langGetText(STR_DLGGAMEINFO_UNLIMITED));
    } else {
        long mins = timeLeft;
        mins /= 50;   /* GAME_NUMGAMETICKS_SEC = (1000/20) */
        mins /= 60;   /* NUM_SECONDS_MINUTE */
        mins++;       /* round up */
        MessageArgs args = {};
        args.number = (int)mins;
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGGAMEINFO_TIMEREMAINING, &args));
    }

    /* Server visibility rules, one row per category. Read-only mirror of
     * the lobby's pill / base / ally rows, shown so a player can check
     * them without opening the lobby. The row labels come from the lobby
     * form and carry no punctuation, so the colon is supplied here. */
    {
        static const struct {
            langid       label;
            ViewCategory cat;
        } viewRows[] = {
            { STR_DLGLOBBY_VIEW_PILL, viewCategoryPill },
            { STR_DLGLOBBY_VIEW_BASE, viewCategoryBase },
            { STR_DLGLOBBY_VIEW_ALLY, viewCategoryAlly },
        };
        const char *modes[] = {
            langGetText(STR_DLGLOBBY_VIEW_ALWAYS),
            langGetText(STR_DLGLOBBY_VIEW_KEY),
            langGetText(STR_DLGLOBBY_VIEW_DECAY),
            langGetText(STR_DLGLOBBY_VIEW_OFF),
        };
        for (int r = 0; r < 3; r++) {
            /* The lobby-settings decoder mirrors the policy byte as it
             * arrives, so a value outside the enum can reach here. Fall
             * back to the wire default rather than index past modes[]. */
            int policy = (int)clientSimGetViewPolicy(cs, viewRows[r].cat);
            if (policy < (int)viewPolicyAlways || policy > (int)viewPolicyOff) {
                policy = (int)viewPolicyAlways;
            }
            const char *label = langGetText(viewRows[r].label);
            if (policy == (int)viewPolicyDecay) {
                int secs = (int)clientSimGetViewDecaySecs(cs, viewRows[r].cat);
                ImGui::Text("%s: %s %d %s", label, modes[policy], secs,
                            langGetText(STR_DLGLOBBY_VIEW_DECAY_SECS));
            } else {
                ImGui::Text("%s: %s", label, modes[policy]);
            }
        }
    }

    /* Classic mode and the allies-in-trees rule, read-only mirrors of the
     * lobby's two checkboxes. */
    ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_CLASSIC_MODE_CB),
                clientSimGetClassicMode(cs) ? langGetText(STR_YES) : langGetText(STR_NO));
    ImGui::Text("%s: %s", langGetText(STR_DLGLOBBY_ALLIES_TREES_CB),
                clientSimGetAlliesInTrees(cs) ? langGetText(STR_YES) : langGetText(STR_NO));
}

static void renderGameInfoPanel(ClientSim *cs) {
    if (!s_showGameInfo || s_popGameInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(320 * s_uiScale, 200 * s_uiScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280 * s_uiScale, 200 * s_uiScale),
                                        ImVec2(FLT_MAX, FLT_MAX));
    char title[128];
    snprintf(title, sizeof(title), "%s###gameinfo", langGetText(STR_DLGGAMEINFO_TITLE));
    if (!ImGui::Begin(title, &s_showGameInfo)) {
        ImGui::End();
        return;
    }
    renderGameInfoContent(cs);
    ImGui::End();
}

/* -------------------------------------------------------
 * Brain settings window
 * Renders a modal window listing all settings returned by
 * brain.settings().  Called each frame when open.
 * ------------------------------------------------------- */
static void renderBrainSettingsWindow(void) {
    if (!s_brainSettingsOpen) return;

    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Always);
    char title[128];
    snprintf(title, sizeof(title), "%s###brainsettings", langGetText(STR_BRAINSETTINGS_TITLE));
    if (!ImGui::Begin(title, &s_brainSettingsOpen,
                      ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }

    if (s_brainSettings == nullptr || s_brainSettingsCount == 0) {
        ImGui::TextDisabled("%s", langGetText(STR_BRAINSETTINGS_NONE));
        ImGui::End();
        return;
    }

    for (int i = 0; i < s_brainSettingsCount; i++) {
        LuaBrainSetting *s = &s_brainSettings[i];
        bool changed = false;

        ImGui::PushID(i);
        switch (s->type) {
          case LUA_BRAIN_SETTING_BOOL: {
            bool v = s->value.b;
            if (ImGui::Checkbox(s->label, &v)) {
                s->value.b = v;
                changed = true;
            }
            break;
          }
          case LUA_BRAIN_SETTING_INT: {
            int v = s->value.i;
            bool hasRange = (s->range_min != 0.0f || s->range_max != 0.0f);
            if (hasRange) {
                if (ImGui::SliderInt(s->label, &v,
                                     (int)s->range_min, (int)s->range_max)) {
                    s->value.i = v;
                    changed = true;
                }
            } else {
                if (ImGui::InputInt(s->label, &v)) {
                    s->value.i = v;
                    changed = true;
                }
            }
            break;
          }
          case LUA_BRAIN_SETTING_FLOAT: {
            float v = s->value.f;
            bool hasRange = (s->range_min != 0.0f || s->range_max != 0.0f);
            if (hasRange) {
                if (ImGui::SliderFloat(s->label, &v, s->range_min, s->range_max)) {
                    s->value.f = v;
                    changed = true;
                }
            } else {
                if (ImGui::InputFloat(s->label, &v, 0.0f, 0.0f, "%.3f")) {
                    s->value.f = v;
                    changed = true;
                }
            }
            break;
          }
          case LUA_BRAIN_SETTING_STRING: {
            if (ImGui::InputText(s->label, s->value.s, sizeof(s->value.s),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                changed = true;
            }
            break;
          }
        }
        ImGui::PopID();

        if (changed) {
            luaBrainSetSetting(s);
        }
    }

    ImGui::End();
}

/* Bring the desktop Send Message pop-out to the front and put the caret in
 * its input box, with any draft text already there selected so typing
 * replaces it.
 *
 * Deliberately not a toggle. Players open the pop-out, click back into the
 * game window to keep playing, then press Ctrl+M again expecting the message
 * box — but the key press lands on the main window, so a toggle hides the
 * pop-out instead of raising it. Every desktop entry point (Ctrl+M, the
 * Players menu item, windowShowSendMessages(wsrOpen), the mac menu bar)
 * reaches this — directly, or via sdl3ImguiShowSendMsg where the caller must
 * also honour controller mode — so the window comes forward however it was
 * asked for.
 * Closing is the pop-out's own close box or Escape.
 *
 * Deliberately does not touch s_sendMsgCooldownEnd. Opening is not a reason
 * to hand back an early Send button: clearing it here would let a player
 * send, close the pop-out and re-press Ctrl+M to skip the remainder of
 * SEND_MSG_WAIT_MS. The cooldown is short and expires on its own. */
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
static void sendMsgPopOutShow(void) {
    /* popOutCreate re-shows and raises a window it created earlier, so this
     * one call covers both the first open and a raise from behind the game. */
    if (!popOutCreate(&s_popSendMsg, langGetText(STR_MENU_SEND_MESSAGE), 400, 200, 0))
        return;
    s_sendMsgFocusInput = true;
    s_closeMenuPopups   = true;
}
#endif

/* -------------------------------------------------------
 * Send Message panel
 * Mirrors dialogMessages.c: radio buttons for recipient,
 * text input, a "Sending to N players" counter, and a
 * Send button with a 2-second cooldown after each send.
 * ------------------------------------------------------- */
static void renderSendMsgContent(ClientSim *cs) {
    /* Recipient radio buttons */
    ImGui::RadioButton(langGetText(STR_DLGMSG_ALLPLAYERS),  &s_sendMsgRecipient, kSendAll);
    ImGui::SameLine();
    ImGui::RadioButton(langGetText(STR_DLGMSG_ALLALLIES),   &s_sendMsgRecipient, kSendAllies);
    ImGui::SameLine();
    ImGui::RadioButton(langGetText(STR_DLGMSG_NEARBY),      &s_sendMsgRecipient, kSendNearby);
    ImGui::SameLine();
    ImGui::RadioButton(langGetText(STR_DLGMSG_SELECTION),   &s_sendMsgRecipient, kSendSelected);

    /* "Sending to N player(s)" label */
    int numSend = 0;
    switch (s_sendMsgRecipient) {
        case kSendAll:      numSend = (int)clientSimGetNumPlayers(cs);     break;
        case kSendAllies:   numSend = clientSimGetNumAllies(cs);              break;
        case kSendNearby:   numSend = clientSimGetNumNearbyTanks(cs);         break;
        case kSendSelected: numSend = clientSimGetNumCheckedPlayers(cs);      break;
    }
    {
        MessageArgs args = {};
        args.number = numSend;
        ImGui::TextDisabled("%s", langGetTextFmt(
            numSend == 1 ? STR_DLGMSG_SENDPLAYER : STR_DLGMSG_SENDPLAYERS, &args));
    }

    /* Text input — capped by the buffer size to 128 bytes of content
     * (matches the chat wire payload). For CJK that's ~42 visible chars
     * (3 bytes each); for ASCII it's 128. */
    /* Auto-focus on window appear or when Ctrl+M re-pressed */
    bool wantSelectAll = false;
    if (s_sendMsgFocusInput) {
        s_sendMsgFocusFrames = 2;
        s_sendMsgFocusInput = false;
    }
    if (ImGui::IsWindowAppearing() || s_sendMsgFocusFrames > 0) {
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere(0);
        wantSelectAll = true;
        if (s_sendMsgFocusFrames > 0) s_sendMsgFocusFrames--;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (s_sendMsgHideNav)
        ImGui::GetCurrentWindow()->DC.NavHideHighlightOneFrame = true;
    bool pressedEnter = ImGui::InputText("##msg", s_sendMsgBuf, sizeof(s_sendMsgBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (s_sendMsgHideNav) {
        ImGui::GetCurrentContext()->NavCursorVisible = false;
        if (ImGui::IsItemActive()) s_sendMsgHideNav = false;
    }
    if (wantSelectAll) {
        if (ImGuiInputTextState *state = ImGui::GetInputTextState(ImGui::GetItemID()))
            state->SelectAll();
    }

    /* Send button with cooldown */
    bool inCooldown = (s_sendMsgCooldownEnd != 0 &&
                       SDL_GetTicks() < s_sendMsgCooldownEnd);
    if (inCooldown) ImGui::BeginDisabled();
    bool doSend = ImGui::Button(langGetText(STR_DLGMSG_BUTTON)) || (!inCooldown && pressedEnter);
    imguiHandOnHover();
    if (inCooldown) ImGui::EndDisabled();

    if (doSend && s_sendMsgBuf[0] != '\0') {
        switch (s_sendMsgRecipient) {
            case kSendAll:      clientSimSendMessageAllPlayers(cs, s_sendMsgBuf);  break;
            case kSendAllies:   clientSimSendMessageAllAllies(cs, s_sendMsgBuf);   break;
            case kSendNearby:   clientSimSendMessageAllNearby(cs, s_sendMsgBuf);   break;
            case kSendSelected: clientSimSendMessageAllSelected(cs, s_sendMsgBuf); break;
        }
        s_sendMsgCooldownEnd = SDL_GetTicks() + SEND_MSG_WAIT_MS;
#if BOLO_MOBILE
        /* On mobile, close the dialog after sending via Enter */
        if (pressedEnter) {
            s_showSendMsg = false;
            dialogDismissKeyboard(s_window);
        }
#else
        s_sendMsgFocusInput = true;
        s_sendMsgHideNav = true;
#endif
    }
}

static void renderSendMsgPanel(ClientSim *cs) {
    if (!s_showSendMsg || s_popSendMsg.open) return;

    if (uiModeIsTablet()) {
        ImGuiIO &io = ImGui::GetIO();
        float w = io.DisplaySize.x * 0.8f;
        ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
#if BOLO_MOBILE
        /* Position near the top so the soft keyboard doesn't cover it */
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 8.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
#else
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
#endif
    } else {
        ImGui::SetNextWindowSize(ImVec2(350 * s_uiScale, 0), ImGuiCond_FirstUseEver);
    }
    bool *pOpen = uiModeIsTablet() ? nullptr : &s_showSendMsg;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize;
    if (uiModeIsTablet()) flags |= ImGuiWindowFlags_NoCollapse;
    char title[128];
    snprintf(title, sizeof(title), "%s###sendmsg", langGetText(STR_DLGMSG_TITLE));
    if (!ImGui::Begin(title, pOpen, flags)) {
        ImGui::End();
        return;
    }
    renderSendMsgContent(cs);
    ImGui::End();
}

/* Controller-mode message entry: a simplified modal that sends to all
 * players (no recipient picker) with just a text field + Send and a
 * B/Escape cancel.  Replaces the pop-out / in-window dialog when
 * uiShouldUseControllerMode() — see sdl3ImguiShowSendMsg.  Lives in the
 * main ImGui context so it picks up gamepad nav and the Steam OSK. */
static void renderCtrlSendMsg(ClientSim *cs) {
    char title[128];
    snprintf(title, sizeof(title), "%s###ctrlsendmsg", langGetText(STR_DLGMSG_TITLE));

    if (s_pendingCtrlSendMsg) {
        ImGui::OpenPopup(title);
        s_pendingCtrlSendMsg = false;
        s_showCtrlSendMsg    = true;
        s_sendMsgFocusInput  = true;
    }
    if (!s_showCtrlSendMsg) return;

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::BeginPopupModal(title, &s_showCtrlSendMsg, flags)) {
        /* ImGui's NavCancel does not auto-close modals, so detect B/Escape
           and close manually — checked before InputText() so the press
           closes rather than just reverting the edit.  On the Steam Input
           path B arrives as Escape. */
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ImGui::CloseCurrentPopup();
            s_showCtrlSendMsg = false;
            ImGui::EndPopup();
            return;
        }

        bool wantSelectAll = false;
        if (s_sendMsgFocusInput) { s_sendMsgFocusInput = false; wantSelectAll = true; }
        if (ImGui::IsWindowAppearing() || wantSelectAll) {
            ImGui::SetKeyboardFocusHere();
            wantSelectAll = true;
        }
        ImGui::SetNextItemWidth(340.0f);
        bool pressedEnter = ImGui::InputText("##ctrlmsg", s_sendMsgBuf, sizeof(s_sendMsgBuf),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (wantSelectAll) {
            if (ImGuiInputTextState *st = ImGui::GetInputTextState(ImGui::GetItemID()))
                st->SelectAll();
        }

        bool inCooldown = (s_sendMsgCooldownEnd != 0 &&
                           SDL_GetTicks() < s_sendMsgCooldownEnd);
        if (inCooldown) ImGui::BeginDisabled();
        bool doSend = ImGui::Button(langGetText(STR_DLGMSG_BUTTON)) ||
                      (!inCooldown && pressedEnter);
        if (inCooldown) ImGui::EndDisabled();

        if (doSend && s_sendMsgBuf[0] != '\0') {
            clientSimSendMessageAllPlayers(cs, s_sendMsgBuf);
            s_sendMsgBuf[0] = '\0';
            ImGui::CloseCurrentPopup();
            s_showCtrlSendMsg = false;
        }

        ImGui::EndPopup();
    } else {
        s_showCtrlSendMsg = false;
    }
}

/* -------------------------------------------------------
 * Map Overview pop-out
 * ------------------------------------------------------- */
static void renderMapOverviewContent(ClientSim *cs) {
    SDL_Texture *tex  = overviewViewGetTexture(s_overviewView);
    int          texW = 0;
    int          texH = 0;
    overviewViewGetSize(s_overviewView, &texW, &texH);
    if (!tex || texW <= 0 || texH <= 0) return;

    /* Drawn at the offscreen's own size, which is the size it was rendered
       at, so the blit is 1:1 and point sampling has no fractional scale to
       fight. */
    ImVec2 imgMin = ImGui::GetCursorScreenPos();
    imguiPushNearestSampling();
    ImGui::Image((ImTextureID)tex, ImVec2((float)texW, (float)texH));
    imguiPopNearestSampling();

    /* An InvisibleButton over the image rect takes the right-drag as an
       active item, so dragging with the right button pans the map instead of
       moving the window, and gives the input handler its hover test. The left
       button is left unclaimed on purpose — it builds at the square under the
       pointer, and an active item would swallow the click. */
    ImGui::SetCursorScreenPos(imgMin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##OverviewPan", ImVec2((float)texW, (float)texH),
                           ImGuiButtonFlags_MouseButtonRight);
    /* The live bindings, so a key the player has bound to an in-game action
       drives the tank and does nothing to the overview. Fetched each frame —
       Key Setup can change them while the pop-out is open. */
    keyItems keys;
    windowGetKeys(&keys);
    overviewViewHandleInput(s_overviewView, ImGui::IsItemHovered(),
                            texW, texH, cs, &keys, false);

    /* Persist zoom and follow the moment the player changes either. The
       comparison is exact on purpose: the stored zoom came out of the same
       ladder table it is being compared against, so equal values are
       bit-identical and there is no drift for an epsilon to absorb. */
    OverviewCamera *cam = overviewViewCamera(s_overviewView);
    if (cam) {
        float zoom = overviewCameraZoomScale(cam);
        if (zoom != gameFrontOverviewZoom ||
            cam->follow != gameFrontOverviewFollow) {
            gameFrontOverviewZoom   = zoom;
            gameFrontOverviewFollow = cam->follow;
            gameFrontSaveWindowSettings();
        }

        /* Zoom and follow state along the bottom-left of the map. Drawn onto
           the image with the window draw list rather than as a widget: the
           image is exactly DisplaySize, so anything that added to the
           content would give the pop-out a scrollbar. %g keeps the ladder
           readable (0.5, 1, 1.5, 2) with no trailing zeros, and the text is
           ASCII because this file is compiled without /utf-8. */
        char status[64];
        SDL_snprintf(status, sizeof(status), "%gx - %s", (double)zoom,
                     langGetText(cam->follow ? STR_OVERVIEW_FOLLOWING
                                             : STR_OVERVIEW_FREE));
        const float pad = 4.0f;
        ImVec2 textSize = ImGui::CalcTextSize(status);
        ImVec2 boxMin(imgMin.x,
                      imgMin.y + (float)texH - (textSize.y + pad * 2.0f));
        ImVec2 boxMax(imgMin.x + textSize.x + pad * 2.0f,
                      imgMin.y + (float)texH);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 160));
        dl->AddText(ImVec2(boxMin.x + pad, boxMin.y + pad),
                    IM_COL32(230, 230, 230, 255), status);
    }
}

/* -------------------------------------------------------
 * In-window Map Overview
 * ------------------------------------------------------- */
/* Is the pointer inside a HUD rectangle? The layout's coordinates are relative
 * to the map rect, so the caller passes the rect's origin and this shifts them
 * into the window coordinates ImGui reports the mouse in — the same shift the
 * blit applies. */
static bool overviewHudRectHit(ImVec2 mouse, float originX, float originY,
                               float x, float y, float w, float h) {
    float left = originX + x;
    float top  = originY + y;
    return mouse.x >= left && mouse.x < left + w &&
           mouse.y >= top  && mouse.y < top + h;
}

/* The input half of the in-window mode: sdl3draw.c has already rendered the
 * map and blitted it to the window this frame, so this only has to put the pan
 * item and the status strip over exactly the rect it blitted to. Submitted at
 * the start of the frame so it sits at the back of the z-order and never takes
 * a click from a panel or dialog on top of it. */
static void renderOverviewInWindow(ClientSim *cs) {
    if (!sdl3DrawIsOverviewInWindow()) return;
    OverviewView *view = sdl3DrawOverviewInWindowView();
    if (!view) return;
    float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
    if (!sdl3DrawGetOverviewInWindowRect(&rx, &ry, &rw, &rh)) return;

    /* Cleared here rather than only set below, so a frame that never reaches
       the hit-test cannot leave a panel stuck faded up under a pointer that
       has gone. */
    sdl3DrawSetHudPanelHover(false, false, false);

    ImGui::SetNextWindowPos(ImVec2(rx, ry));
    ImGui::SetNextWindowSize(ImVec2(rw, rh));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("##OverviewInWindow", nullptr,
                             ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavInputs |
                             ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();

    /* Submitting this first is not what puts it at the back. ImGui files a
       window carrying NoBringToFrontOnFocus at the back of the display order
       once, when it is created, and never re-sorts by submission order after
       that — so a window carrying the same flag that is created later ends up
       behind this one. The vote widgets and the alliance request are exactly
       that, and behind a window covering the whole screen they still draw but
       every click on them hit-tests to the map. Pushing this back each frame
       is what actually makes it the bottom window. */
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());

    if (open) {
        /* Same split as the pop-out: the item claims the right button so a
           right-drag pans, and leaves the left one unclaimed so a click still
           reaches the overview's build path. */
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##OverviewInWindowPan", ImVec2(rw, rh),
                               ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();

        /* The HUD is blitted over the map by sdl3draw.c rather than submitted
           as ImGui items, so the pan item still spans it. Hit-test the three
           backing rectangles here and hold the pointer back from the view over
           them: a click on a panel must not build at the map square
           underneath, and hovered = false also hands the OS pointer back, so
           an arrow shows over the chrome instead of the game crosshair. A
           right-drag that began on the map keeps panning either way — that
           path keys off the item being active, not hovered. */
        OverviewHudLayout hud;
        bool haveHud    = sdl3DrawGetOverviewHudLayout(&hud);
        bool overBuild  = false;
        bool overStatus = false;
        bool overNews   = false;
        ImVec2 mouse    = ImGui::GetMousePos();
        if (haveHud) {
            overBuild = overviewHudRectHit(mouse, rx, ry, hud.buildX, hud.buildY,
                                           hud.buildW, hud.buildH);
            overStatus = overviewHudRectHit(mouse, rx, ry,
                                            hud.columnX, hud.columnY,
                                            hud.columnW, hud.columnH);
            overNews = overviewHudRectHit(mouse, rx, ry,
                                          hud.newswireX, hud.newswireY,
                                          hud.newswireW, hud.newswireH);
        }
        bool overHud = overBuild || overStatus || overNews;

        /* The same three hits drive the fade in sdl3draw.c, so a panel the
           pointer is resting on comes up solid to be read. Qualified by
           `hovered`, which is false when a dialog or a vote widget is over the
           map there: the pointer is on that, not on the HUD under it. */
        sdl3DrawSetHudPanelHover(hovered && overStatus, hovered && overBuild,
                                 hovered && overNews);

        /* The build items are the only interactive part of the HUD; a click
           anywhere else on it is simply swallowed. Same trio the classic
           hit-test in sdl3DrawHandleEvent runs, so the indent drawn into the
           HUD slice follows the new selection. */
        if (overBuild && cs && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            for (int i = 0; i <= (int)BsMine; i++) {
                float ix = 0.0f, iy = 0.0f, iw = 0.0f, ih = 0.0f;
                if (!overviewHudBuildItemRect(&hud, i, &ix, &iy, &iw, &ih)) break;
                if (!overviewHudRectHit(mouse, rx, ry, ix, iy, iw, ih)) continue;
                buildSelect picked = (buildSelect)i;
                if (picked != clientSimGetCurrentBuildSelect(cs)) {
                    sdl3DrawSelectIndentsOff(clientSimGetCurrentBuildSelect(cs), 0, 0);
                    sdl3DrawSelectIndentsOn(picked, 0, 0);
                    clientMutexWaitFor();
                    clientSimSetCurrentBuildSelect(cs, picked);
                    clientMutexRelease();
                }
                break;
            }
        }

        /* The live bindings, fetched each frame — Key Setup can change them
           while the mode is up, and a key bound to an in-game action has to
           drive the tank rather than the map. */
        keyItems keys;
        windowGetKeys(&keys);
        overviewViewHandleInput(view, hovered && !overHud,
                                (int)rw, (int)rh, cs, &keys, true);

        /* Zoom and follow state along the top of the map — the pop-out puts the
           same readout bottom-left, but here the bottom of the window is where
           the newswire goes. Drawn with the window draw list so it adds nothing
           to the window's content. %g keeps the ladder readable (0.5, 1, 1.5,
           2) with no trailing zeros, and the text is ASCII because this file is
           compiled without /utf-8. */
        OverviewCamera *cam = overviewViewCamera(view);
        if (cam) {
            char status[64];
            SDL_snprintf(status, sizeof(status), "%gx - %s",
                         (double)overviewCameraZoomScale(cam),
                         langGetText(cam->follow ? STR_OVERVIEW_FOLLOWING
                                                 : STR_OVERVIEW_FREE));
            const float pad = 4.0f;
            ImVec2 textSize = ImGui::CalcTextSize(status);
            /* The corner belongs to the build strip, so the readout starts
               just past it, level with its top. The gap matches the margin the
               strip itself keeps from the map's edge. With no HUD drawn there
               is nothing to clear and it sits in the corner. */
            float textX = rx;
            float textY = ry;
            if (haveHud) {
                const float hudGap = 8.0f;
                textX = rx + hud.buildX + hud.buildW + hudGap;
                textY = ry + hud.buildY;
            }
            ImVec2 boxMin(textX, textY);
            ImVec2 boxMax(textX + textSize.x + pad * 2.0f,
                          textY + textSize.y + pad * 2.0f);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 160));
            dl->AddText(ImVec2(boxMin.x + pad, boxMin.y + pad),
                        IM_COL32(230, 230, 230, 255), status);
        }
    }
    ImGui::End();
}

/* Whether the local player may answer a given vote. Surrender votes are
   answerable only by members of the surrendering team (teamId); other vote
   kinds are open to all connected players. Thin wrapper over the shared
   rule in client_sim.c, which the newswire gate uses too — a non-member no
   longer even receives the state event, so this is now belt-and-braces for
   anything already mirrored. */
static bool localCanAnswerGameVote(ClientSim *cs,
                                   const ClientGameVoteSnapshot *snap) {
    return clientSimMayAnswerGameVote(cs, snap->kind, snap->teamId);
}

/* -------------------------------------------------------
 * Players panel (standalone window for tablet mode)
 * ------------------------------------------------------- */

static void renderPlayersContent(ClientSim *cs) {
    /* Selection helpers */
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALL)))    clientSimCheckAllNonePlayers(cs, true);
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NONE)))   clientSimCheckAllNonePlayers(cs, false);
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALLIES))) clientSimCheckAlliedPlayers(cs);
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NEARBY))) clientSimCheckNearbyPlayers(cs);
    imguiHandOnHover();

    ImGui::Separator();

    /* Pre-compute alliance state */
    BYTE self = clientSimGetMyPlayerNum(cs);
    bool hasAllies  = false;
    bool canRequest = false;
    bool isAlly[MAX_PLAYERS] = {};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (s_playerEnabled[i] && i != self) {
            isAlly[i] = clientSimIsPlayerAlly(cs, self, (BYTE)i);
            if (isAlly[i]) hasAllies = true;
            else if (s_playerChecked[i]) canRequest = true;
        }
    }

    /* Outside the voice guard below: the counter columns' skull comes from
     * here too, and it is drawn in every build. Idempotent. */
    ensureWbnIconsLoaded();

#if defined(WINBOLO_VOICE)
    /* Who is producing voice right now. Derived locally from frames
     * arriving, so it only ever names players this client can actually
     * hear; read once for the whole panel rather than per row. */
    PlayerBitMap talkingMap = voiceGetTalkingMap();
#endif

    /* Collect enabled player indices */
    int enabledPlayers[MAX_PLAYERS];
    int enabledCount = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (s_playerEnabled[i]) {
            enabledPlayers[enabledCount++] = i;
        }
    }

    /* ── Live counter columns ────────────────────────────────────────
     * The end-of-round recap's counters, counted live and drawn on each
     * player's own row between the name and the ping: same columns, same
     * header art, minus damage dealt and builds, which no client-side
     * event carries. Single column only — two half-width columns cannot
     * hold a name and six numbers, so the tablet branch below keeps
     * drawing bare rows and the touch layout is a later slice's problem. */
    const bool showStats = !(uiModeIsTablet() && enabledCount > 1);

    /* One read of the counters per slot. A slot the sim has no stats for
     * reads as zeroes, so an enabled row still prints a full set of
     * numbers instead of dropping out of columns the row above has. */
    ClientPlayerStats slotStats[MAX_PLAYERS] = {};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const ClientPlayerStats *ps =
            cs ? clientSimGetPlayerStats(cs, (BYTE)i) : NULL;
        if (ps) slotStats[i] = *ps;
    }

    /* Column order is the recap's: kills, deaths, base captures, pill
     * captures, LGM kills, LGM deaths. */
    auto slotStatValue = [&](int slot, int c) -> unsigned {
        const ClientPlayerStats *p = &slotStats[slot];
        switch (c) {
            case 0:  return p->kills;
            case 1:  return p->deaths;
            case 2:  return p->baseCaptures;
            case 3:  return p->pillCaptures;
            case 4:  return p->lgmKills;
            default: return p->lgmDeaths;
        }
    };
    const langid statColStr[6] = {
        STR_DLGLOBBY_LASTROUND_COL_KILLS,
        STR_DLGLOBBY_LASTROUND_COL_DEATHS,
        STR_DLGLOBBY_LASTROUND_COL_BASE,
        STR_DLGLOBBY_LASTROUND_COL_PILL,
        STR_DLGLOBBY_LASTROUND_COL_LGMK,
        STR_DLGLOBBY_LASTROUND_COL_LGMD,
    };

    /* Rows the single-column list draws: every enabled slot, plus any slot
     * that is no longer enabled but still carries counters from this game.
     * A player who leaves keeps their line, blank, so nothing below them
     * moves up mid-round. The sim goes on counting a slot after its player
     * goes and zeroes every slot at the start of a game, so the counters
     * are the whole test — no extra state to keep and none to clear.
     *
     * Two consequences, neither of them fixed here:
     *  - a player who leaves having scored nothing leaves no gap, because
     *    nothing distinguishes their slot from one nobody ever used;
     *  - a new player taking that slot over inherits the previous
     *    occupant's numbers until the mid-game re-seed lands. */
    int panelRows[MAX_PLAYERS];
    bool panelRowBlank[MAX_PLAYERS];
    int panelRowCount = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        bool blank = false;
        if (!s_playerEnabled[i]) {
            bool scored = false;
            for (int c = 0; c < 6 && !scored; c++)
                if (slotStatValue(i, c) != 0) scored = true;
            if (!scored) continue;
            blank = true;
        }
        panelRowBlank[panelRowCount] = blank;
        panelRows[panelRowCount++] = i;
    }

    /* Column widths, computed once for the whole panel rather than per row,
     * which is what makes the numbers line up down it: each column is as
     * wide as the widest number it will actually print this round or its
     * header sprite, whichever is more. Sprites are text-height tall and
     * keep their source aspect, so this follows the UI scale without a
     * hard-coded pixel anywhere. */
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float iconH = ImGui::GetTextLineHeight();
    const float lgmW  = iconH * (float)LGM_WIDTH / (float)LGM_HEIGHT;
    const float statIconW[6] = {
        iconH, iconH, iconH, iconH, lgmW,
        /* LGM deaths heads with the man and the skull side by side. */
        lgmW + sty.ItemInnerSpacing.x + iconH,
    };
    float statW[6], statOffX[6];
    float statTotal = 0.0f;
    for (int c = 0; c < 6; c++) {
        unsigned widest = 0;
        for (int r = 0; r < panelRowCount; r++) {
            if (panelRowBlank[r]) continue;
            unsigned v = slotStatValue(panelRows[r], c);
            if (v > widest) widest = v;
        }
        char buf[16];
        SDL_snprintf(buf, sizeof(buf), "%u", widest);
        float w = ImGui::CalcTextSize(buf).x;
        if (statIconW[c] > w) w = statIconW[c];
        /* One pixel of slop on top of the padding: a sprite sized to
         * exactly fill the column would otherwise be at the mercy of
         * rounding at the edge. */
        statW[c] = w + sty.CellPadding.x * 2.0f + 1.0f;
        statOffX[c] = statTotal;
        statTotal += statW[c];
    }

    /* Widest ping the panel will print. The ping itself stays right-aligned
     * on its own width, but the columns to its left have to start at the
     * same x on every row, so they are laid out against this instead. */
    float pingColW = 0.0f;
    for (int r = 0; r < panelRowCount; r++) {
        if (panelRowBlank[r]) continue;
        int slot = panelRows[r];
        char buf[16];
        if (s_playerPing[slot] > 0)
            SDL_snprintf(buf, sizeof(buf), "%dms", (int)s_playerPing[slot]);
        else
            SDL_snprintf(buf, sizeof(buf), "---");
        float w = ImGui::CalcTextSize(buf).x;
        if (w > pingColW) pingColW = w;
    }

    /* SameLine() offsets are measured from the window's left edge, while
     * GetContentRegionAvail() inside the row is measured from the cursor —
     * which by then has moved past the alliance mark and the flag, by a
     * different amount on every row. Read one window-relative right edge
     * here, at the start of a line, so the columns and the ping land on the
     * same x in every row. */
    const float rowRightX =
        ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float statRightX = rowRightX - pingColW - sty.ItemSpacing.x;

    /* x at which content of width w sits right-aligned in column c. */
    auto statContentX = [&](int c, float w) -> float {
        return statRightX - statTotal + statOffX[c] + statW[c] -
               sty.CellPadding.x - w;
    };

    /* Render a single player row */
    auto renderPlayerRow = [&](int i) {
        /* Alliance indicator */
        if (i != self) {
            if (isAlly[i]) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.9f, 0.0f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.0f, 0.0f, 1.0f));
            }
            ImGui::TextUnformatted("*");
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        /* Flag icon — skipped for bots (no real country; renderPlayerName
         * below shows a brain icon in the platform-icon slot instead). */
        if (!(s_playerFlags[i] & PLAYER_FLAG_BOT) && s_playerCountry[i][0] != '\0') {
            if (drawCountryFlagWithTip(s_playerCountry[i])) {
                ImGui::SameLine();
            }
        }

        /* Platform / WBN / Steam icons (brain icon for bots) */
        renderPlayerName(NULL, s_playerFlags[i], s_playerClientType[i], "", false);

        const char *label = s_playerName[i][0] ? s_playerName[i] : nullptr;
        char defLabel[16];
        if (!label) {
            snprintf(defLabel, sizeof(defLabel), "%d", i + 1);
            label = defLabel;
        }

        /* Build ping string */
        char pingStr[16];
        if (s_playerPing[i] > 0)
            snprintf(pingStr, sizeof(pingStr), "%dms", (int)s_playerPing[i]);
        else
            snprintf(pingStr, sizeof(pingStr), "---");

        float fullWidth = ImGui::GetContentRegionAvail().x;
        float pingWidth = ImGui::CalcTextSize(pingStr).x;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
#if defined(WINBOLO_VOICE)
        /* Width the mic icon takes out of the row, icon plus its trailing
         * spacing, so the name Selectable gives it room the same way it
         * already does for the checkbox. */
        float micWidth = (float)WBN_ICON_SIZE;
        float micColumn = micWidth + spacing;
        /* Room the per-player volume slider takes, slider plus its trailing
         * spacing. Reserved separately from the mic rather than folded into
         * it: they are two cells with two widths, and the subtraction below
         * reads as the list of things in front of the name. Reserved on the
         * local player's row too, which draws a blank there, or the name
         * would start at a different x on that one row. */
        float volWidth  = ImGui::GetFrameHeight() * 3.0f;
        float volColumn = volWidth + spacing;
#else
        const float micColumn = 0.0f;
        const float volColumn = 0.0f;
#endif
        /* Room the counter columns take out of the row, block plus the gap
         * that separates it from the name — reserved the same way the ping
         * and the mic are. The ping is reserved at the width of the widest
         * one in the panel, not this row's, so the block starts at a fixed
         * x while the ping stays hard right on its own width. */
        float statBlock   = showStats ? statTotal + spacing : 0.0f;
        float pingReserve = showStats ? pingColW : pingWidth;

        /* Checkbox + selectable name */
        if (i != self) {
            char checkLabel[64];
            snprintf(checkLabel, sizeof(checkLabel), "##chk%d", i);
            bool checked = s_playerChecked[i];
            if (ImGui::Checkbox(checkLabel, &checked)) {
                clientSimTogglePlayerCheckState(cs, (BYTE)i);
            }
            ImGui::SameLine();
        }

#if defined(WINBOLO_VOICE)
        /* Voice state, between the checkbox and the name. */
        renderPlayerMicCell(cs, i, s_playerFlags[i], talkingMap,
                            i == self, micWidth, false);
        ImGui::SameLine();

        /* How loud that player is played here, beside their speaker. Local
         * playback only: nothing is sent, and it lasts until they leave. */
        if (i == self) {
            /* Nothing to set for yourself, but the width is held all the
             * same so every name starts at the same x. */
            ImGui::Dummy(ImVec2(volWidth, ImGui::GetTextLineHeight()));
        } else {
            char volLabel[64];
            snprintf(volLabel, sizeof(volLabel), "##vol%d", i);
            float gain = voiceGetPlayerVolume(i);
            ImGui::SetNextItemWidth(volWidth);
            /* Zero FramePadding for the reason the mic button has it: the
             * default padding would make this taller than the Selectable
             * beside it and leave a dead strip down the row. No value
             * printed in it either — at this width it would be unreadable,
             * and the grab says where the volume is. */
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
            if (ImGui::SliderFloat(volLabel, &gain, 0.0f,
                                   VOICE_PLAYER_VOLUME_MAX, "")) {
                voiceSetPlayerVolume(i, gain);
            }
            ImGui::PopStyleVar();
            imguiHelpTooltip(langGetText(STR_PLAYER_TIP_VOICE_VOLUME));
        }
        ImGui::SameLine();
#endif

        char selectLabel[64];
        snprintf(selectLabel, sizeof(selectLabel), "%s##psel%d", label, i);
        if (ImGui::Selectable(selectLabel, s_playerChecked[i],
                              ImGuiSelectableFlags_DontClosePopups,
                              ImVec2(fullWidth - pingReserve - spacing - statBlock - micColumn -
                                     volColumn -
                                     (i != self ? ImGui::GetFrameHeight() + spacing : 0), 0))) {
            if (i != self) clientSimTogglePlayerCheckState(cs, (BYTE)i);
        }
        imguiHandOnHover();

        /* Live counters, right-aligned in their columns so the digits line
         * up as counts reach two figures. */
        if (showStats) {
            for (int c = 0; c < 6; c++) {
                char numBuf[16];
                SDL_snprintf(numBuf, sizeof(numBuf), "%u", slotStatValue(i, c));
                ImGui::SameLine(statContentX(c, ImGui::CalcTextSize(numBuf).x));
                ImGui::TextUnformatted(numBuf);
            }
        }

        /* Right-aligned ping. The single-column path measures from the
         * window-relative right edge so the ping does not shift row to row
         * with the width of the icons in front of the name — the counter
         * columns beside it would shift with it. */
        ImGui::SameLine((showStats ? rowRightX : fullWidth) - pingWidth);
        ImVec4 pingColor = imguiPingBandColor(
            cs ? clientSimGetPlayerPingBand(cs, (BYTE)i)
               : pingBandClassify(s_playerPing[i]));
        ImGui::PushStyleColor(ImGuiCol_Text, pingColor);
        ImGui::TextUnformatted(pingStr);
        ImGui::PopStyleColor();
    };

    /* Header line for the counter columns: the map's own art for what each
     * one counts, drawn at the same x offsets the rows use so every sprite
     * sits over its column, with the written column name on the tooltip.
     * The name region is left empty. */
    if (showStats && panelRowCount > 0) {
        /* The tile sheet is built for the game window's renderer only, so a
         * pop-out cannot draw the map art at all — and unlike a missing SVG
         * the texture pointer is valid, just not this renderer's, so the
         * "no sprite" fallback below never fires. Off the main renderer the
         * whole line prints its written names instead, all six of them, so it
         * does not mix sprites and words. */
        const bool headerAsText = (activeRenderer() != s_renderer);
        ImGui::Dummy(ImVec2(1.0f, iconH));
        for (int c = 0; c < 6; c++) {
            const char *label = langGetText(statColStr[c]);
            ImGui::SameLine(statContentX(c, statIconW[c]));
            if (headerAsText) {
                ImGui::TextUnformatted(label);
                imguiHelpTooltip(label);
                continue;
            }
            bool drewIcon = true;
            switch (c) {
                case 0:
                    imguiDrawTileIcon(TANK_SELF_0_X, TANK_SELF_0_Y);
                    break;
                case 1:
                    drewIcon = playersPanelDrawSkull();
                    break;
                case 2:
                    imguiDrawTileIcon(BASE_GOOD_X, BASE_GOOD_Y);
                    break;
                case 3:
                    imguiDrawTileIcon(PILL_EVIL15_X, PILL_EVIL15_Y);
                    break;
                case 4:
                    imguiDrawAtlasIcon(LGM0_X, LGM0_Y, LGM_WIDTH, LGM_HEIGHT);
                    break;
                default:
                    /* Man then skull — the pair reads as "little men lost",
                     * against the previous column's bare man for the ones
                     * you killed. Without the skull the pair is ambiguous,
                     * so that case falls back to the written label. */
                    imguiDrawAtlasIcon(LGM0_X, LGM0_Y, LGM_WIDTH, LGM_HEIGHT);
                    imguiHelpTooltip(label);
                    ImGui::SameLine(0.0f, sty.ItemInnerSpacing.x);
                    drewIcon = playersPanelDrawSkull();
                    break;
            }
            /* No sprite means no column marker at all, so the written name
             * stands in for it even though it is wider than the column. */
            if (drewIcon) imguiHelpTooltip(label);
            else          ImGui::TextUnformatted(label);
        }
    }

    /* Player list — 2 columns on tablet, single column on desktop */
    if (uiModeIsTablet() && enabledCount > 1) {
        int half = (enabledCount + 1) / 2;
        if (ImGui::BeginTable("##playerCols", 2, ImGuiTableFlags_None)) {
            ImGui::TableNextColumn();
            for (int idx = 0; idx < half; idx++)
                renderPlayerRow(enabledPlayers[idx]);
            ImGui::TableNextColumn();
            for (int idx = half; idx < enabledCount; idx++)
                renderPlayerRow(enabledPlayers[idx]);
            ImGui::EndTable();
        }
    } else {
        for (int r = 0; r < panelRowCount; r++) {
            if (panelRowBlank[r]) {
                /* Slot whose player left mid-round: an empty line the height
                 * of a row, so the lines below it stay where they were. */
                ImGui::Dummy(ImVec2(1.0f, ImGui::GetFrameHeight()));
            } else {
                renderPlayerRow(panelRows[r]);
            }
        }
    }

    /* Alliance actions */
    ImGui::Separator();
    {
        bool rankedGame   = clientSimGetLobbyRanked(cs);
        bool inCooldown = (s_allianceReqCooldownEnd != 0 &&
                           SDL_GetTicks() < s_allianceReqCooldownEnd);
        if (hasAllies) {
            if (ImGui::Button(langGetText(STR_LEAVE_ALLIANCE), ImVec2(-1, 0)))
                clientSimLeaveAllianceSelf(cs);
                imguiHandOnHover();
        } else {
            bool disabled = !canRequest || inCooldown || rankedGame;
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_REQUEST_ALLIANCE), ImVec2(-1, 0))) {
                clientSimRequestAllianceSelected(cs);
                s_allianceReqCooldownEnd = SDL_GetTicks() + ALLIANCE_REQ_WAIT_MS;
            }
            imguiHandOnHover();
            if (disabled) ImGui::EndDisabled();
            if (rankedGame && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", langGetText(STR_ALLIANCE_RANKED_DISABLED));
            }
            /* Controller users can't hover for the tooltip — show the reason
               as a greyed caption under the disabled button. */
            if (rankedGame && uiShouldUseControllerMode()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", langGetText(STR_ALLIANCE_RANKED_DISABLED));
                ImGui::PopStyleColor();
            }
        }
    }

    /* Inbound alliance request — accept/decline here so it's reachable
       with a controller (the overlay is NoNavInputs). */
    {
        const char *reqName = NULL;
        BYTE reqNum = 0;
        if (alliancePendingGet(&reqName, &reqNum)) {
            ImGui::Separator();
            MessageArgs args = {};
            strncpy(args.playerName, reqName, sizeof(args.playerName) - 1);
            args.playerFlags = clientSimGetPlayerAccountFlags(cs, reqNum);
            clientSimGetPlayerCountryCode(cs, reqNum, args.playerCountry);
            ImGui::TextWrapped("%s", langGetTextFmt(STR_DLGALLIANCE_BLURB, &args));
            if (ImGui::Button(langGetText(STR_DLGALLIANCE_ACCEPT))) {
                clientSimAllianceAccept(cs, reqNum);
                allianceClearPending();
            }
            imguiHandOnHover();
            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_DLGALLIANCE_DECLINE))) {
                allianceClearPending();
            }
            imguiHandOnHover();
        }
    }

    /* In-game vote actions — siblings of Request Alliance, only during
     * the running game phase and only on lobby-enabled servers (votes
     * return to the lobby; the server rejects them when there is none). */
    if (clientSimGetNetStatus(cs) == netRunning && clientSimIsLobbyAvailable(cs)) {
        /* Count active teams (distinct teamNumber across connected
         * humans) for the surrender precondition. */
        bool teamSeen[17] = {0};
        int activeTeams = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
            if (!ls || !ls->connected || ls->isBot) continue;
            uint8_t t = ls->teamNumber;
            if (t == 0 || t > 16) continue;
            if (!teamSeen[t]) { teamSeen[t] = true; activeTeams++; }
        }

        if (ImGui::Button(langGetText(STR_VOTE_BACK_TO_LOBBY), ImVec2(-1, 0))) {
            clientSimNetSendGameVoteToggle(cs, GAME_VOTE_KIND_BACK_TO_LOBBY,
                                           GAME_VOTE_TOGGLE_OPEN_ONLY);
            clientSimSetGameVoteWidgetVisible(cs, GAME_VOTE_KIND_BACK_TO_LOBBY, true);
        }

        const ClientLobbySlot *meSlot =
            clientSimGetLobbySlot(cs, clientSimGetMyPlayerNum(cs));
        bool meUnassigned = (meSlot && meSlot->teamNumber == 0);
        bool surrDisabled = (activeTeams != 2) || meUnassigned;
        if (surrDisabled) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_VOTE_SURRENDER), ImVec2(-1, 0))) {
            clientSimNetSendGameVoteToggle(cs, GAME_VOTE_KIND_SURRENDER,
                                           GAME_VOTE_TOGGLE_OPEN_ONLY);
            clientSimSetGameVoteWidgetVisible(cs, GAME_VOTE_KIND_SURRENDER, true);
        }
        if (surrDisabled) ImGui::EndDisabled();
        if (surrDisabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (meUnassigned) {
                ImGui::SetTooltip("%s", langGetText(STR_VOTE_SURRENDER_PICK_TEAM_TIP));
            } else {
                ImGui::SetTooltip("%s", langGetText(STR_VOTE_SURRENDER_TWO_TEAMS_TIP));
            }
        }
        /* Controller users can't hover for the tooltip — show the reason
           as a greyed caption under the disabled button. */
        if (surrDisabled && uiShouldUseControllerMode()) {
            const char *reason = meUnassigned
                ? langGetText(STR_VOTE_SURRENDER_PICK_TEAM_TIP)
                : langGetText(STR_VOTE_SURRENDER_TWO_TEAMS_TIP);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", reason);
            ImGui::PopStyleColor();
        }

        /* Answer rows for any in-flight vote — reachable with a
           controller (the overlay is NoNavInputs). */
        static const uint8_t voteAnswerKinds[] = {
            GAME_VOTE_KIND_BACK_TO_LOBBY, GAME_VOTE_KIND_SURRENDER
        };
        for (size_t vk = 0; vk < sizeof(voteAnswerKinds)/sizeof(voteAnswerKinds[0]); vk++) {
            uint8_t vkind = voteAnswerKinds[vk];
            ClientGameVoteSnapshot vs = {};
            if (!clientSimGetGameVote(cs, vkind, &vs)) continue;
            if (vs.active != GAME_VOTE_ACTIVE_RUNNING) continue;
            /* Surrender votes are private to the surrendering team — non-members
             * don't see the row at all (back-to-lobby stays visible to all). */
            if (!localCanAnswerGameVote(cs, &vs)) continue;
            ImGui::Separator();
            const char *vnm = (vkind == GAME_VOTE_KIND_BACK_TO_LOBBY)
                              ? langGetText(STR_VOTE_BACK_TO_LOBBY)
                              : langGetText(STR_VOTE_SURRENDER);
            ImGui::Text("%s: %u / %u", vnm,
                        (unsigned)vs.yesCount, (unsigned)vs.threshold);
            BYTE vme = clientSimGetMyPlayerNum(cs);
            bool vMyYes = (vme < 16) && ((vs.votes >> vme) & 1u);
            char yLbl[40]; snprintf(yLbl, sizeof(yLbl), "%s##vy%u", langGetText(STR_YES), (unsigned)vkind);
            char nLbl[40]; snprintf(nLbl, sizeof(nLbl), "%s##vn%u", langGetText(STR_NO),  (unsigned)vkind);
            if (vMyYes) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
            if (ImGui::Button(yLbl, ImVec2(80, 0)))
                clientSimNetSendGameVoteToggle(cs, vkind, GAME_VOTE_TOGGLE_YES);
            if (vMyYes) ImGui::PopStyleColor();
            imguiHandOnHover();
            ImGui::SameLine();
            if (ImGui::Button(nLbl, ImVec2(80, 0)))
                clientSimNetSendGameVoteToggle(cs, vkind, GAME_VOTE_TOGGLE_NO);
            imguiHandOnHover();
        }
    }

    /* Allow new players toggle */
    {
        bool anp = (bool)allowNewPlayers;
        if (ImGui::Checkbox(langGetText(STR_ALLOW_NEW_PLAYERS), &anp))
            windowMenuAllowNewPlayers_toggle(cs);
    }
}

static void renderPlayersPanel(ClientSim *cs) {
    if (!s_showPlayersPanel || s_popPlayers.open) return;

    if (uiModeIsTablet()) {
        ImGuiIO &io = ImGui::GetIO();
        float w = io.DisplaySize.x * 0.8f;
        float h = io.DisplaySize.y * 0.8f;
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    } else {
        /* Cap the panel to the viewport work area so a large font (or a
         * small game window) can't push it taller than the screen and clip
         * the bottom off-screen; ImGui then shows a scrollbar for overflow.
         * The default size is also clamped so it never opens oversized. */
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        float maxW = vp->WorkSize.x, maxH = vp->WorkSize.y;
        /* Wide enough for a name plus the six counter columns and the ping;
         * the old 340/280 pair was sized for a name and a ping alone. */
        ImGui::SetNextWindowSize(ImVec2(SDL_min(520 * s_uiScale, maxW),
                                        SDL_min(420 * s_uiScale, maxH)),
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(420 * s_uiScale, 200 * s_uiScale),
                                            ImVec2(maxW, maxH));
    }
    bool *pOpen = uiModeIsTablet() ? nullptr : &s_showPlayersPanel;
    ImGuiWindowFlags flags = uiModeIsTablet() ? (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse) : 0;
    char title[128];
    snprintf(title, sizeof(title), "%s###playerspanel", langGetText(STR_DLGPLAYERS_TITLE));
    if (!ImGui::Begin(title, pOpen, flags)) {
        ImGui::End();
        return;
    }
    renderPlayersContent(cs);
    ImGui::End();
}

/* About modal + linked markdown popups live in dialogs/imgui_about.cpp so
 * the welcome screen (its own ImGui context) can show the same dialog. */

/* -------------------------------------------------------
 * "Join Game?" confirmation modal — shown when a winbolo://
 * URL is received while already in a game.
 * ------------------------------------------------------- */
static void renderJoinConfirmModal(void) {
    char title[128];
    snprintf(title, sizeof(title), "%s###urlconfirm", langGetText(STR_DLGJOIN_TITLE));
    if (s_showJoinConfirm) {
        ImGui::OpenPopup(title);
        s_showJoinConfirm = false;
    }
    static float s_fadeJoinConfirm = 0.0f;
    bool joinOpen = true;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, &joinOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            imguiPopupFadeAlpha(&s_fadeJoinConfirm));
        if (s_closeAllPopups) { ImGui::PopStyleVar(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted(langGetText(STR_DLGJOIN_BLURB));
        ImGui::Spacing();
        if (s_joinConfirmPort > 0) {
            ImGui::Text("%s:%d", s_joinConfirmAddr, s_joinConfirmPort);
        } else {
            ImGui::TextUnformatted(s_joinConfirmAddr);
        }
        int f = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                   langGetText(STR_DLGJOIN_BUTTON));
        if (f == WBUI::FOOTER_CONFIRM) {
            ImGui::CloseCurrentPopup();
            /* Leave current game and return to menu with the URL queued */
            gameFrontHandleUrlOpen(s_joinConfirmUrl);
            windowNewGame();
        } else if (f == WBUI::FOOTER_CANCEL) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

/* -------------------------------------------------------
 * Change Player Name modal (in-game)
 * ------------------------------------------------------- */
static void renderChangeNameModal(ClientSim *cs) {
    char title[128];
    snprintf(title, sizeof(title), "%s###changename", langGetText(STR_DLGCHANGENAME_TITLE));
    if (s_showChangeName) {
        ImGui::OpenPopup(title);
        s_showChangeName    = false;
        s_changeNameBuf[0] = '\0';
        clientSimGetPlayerName(cs, s_changeNameBuf, sizeof(s_changeNameBuf));
    }
    static float s_fadeChangeName = 0.0f;
    bool changeNameOpen = true;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, &changeNameOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            imguiPopupFadeAlpha(&s_fadeChangeName));
        if (s_closeAllPopups) { ImGui::PopStyleVar(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted(langGetText(STR_DLGSETNAME_BLURB));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        ImGui::SetNextItemWidth(300);
        bool enter = ImGui::InputText("##name", s_changeNameBuf,
                                      sizeof(s_changeNameBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        int f = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                   langGetText(STR_OK),
                                   /*enterConfirms*/ true);
        bool doOK     = (f == WBUI::FOOTER_CONFIRM) || enter;
        bool doCancel = (f == WBUI::FOOTER_CANCEL);

        if (doOK) {
            s_changeNameBuf[PLAYER_NAME_LAST] = '\0'; /* final byte stays NUL */
            utilStripName(s_changeNameBuf);
            if (s_changeNameBuf[0] == '\0') {
                /* blank — stay open */
            } else if (s_changeNameBuf[0] == '*') {
                /* invalid — stay open */
            } else {
                if (clientSimSetPlayerName(cs, s_changeNameBuf))
                    ImGui::CloseCurrentPopup();
                /* else: name in use — stay open */
            }
        }
        if (doCancel) ImGui::CloseCurrentPopup();
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

/* -------------------------------------------------------
 * In-game vote widget — one floating window per active vote.
 *
 * Each widget is its own top-level ImGui window (no parent
 * constraint) so the OS window decoration / multi-viewport
 * platform can drag it outside the main game window. We give
 * it a half-transparent background so it doesn't fully obscure
 * the battlefield underneath.
 *
 * Pressing the title-bar X just hides locally; the vote keeps
 * running. Re-press the menu item to bring it back.
 * ------------------------------------------------------- */
/* Per-kind layout state for the vote widget's auto-positioning.
 *
 * Lifecycle:
 *   - active=false initially. When the widget first becomes visible
 *     we set the window to (321*zoom, 0) — flush against the right
 *     edge of the game viewport. After ImGui::Begin we capture the
 *     real size into capturedW/H but mark sizeKnown for next frame.
 *   - On the second frame, with size in hand, we re-position the
 *     window centred in the status-panel column [321, 437], or
 *     below an already-shown sibling vote widget if that centre
 *     would overlap it. Then positioned=true and we stop forcing
 *     SetNextWindowPos so the user can drag the window freely.
 *   - When the widget hides (X-close, auto-dismiss, vote concludes
 *     >5s ago) we clear active=false so the next appearance
 *     re-runs the positioning. */
/* Shared auto-positioning + sizing state for floating panels that
 * pin themselves against the status-panel column. Used by the vote
 * widgets AND the alliance-request modal — anything that wants to
 * sit in the right-of-game column with the same anti-overlap rules. */
struct AutoPanelLayout {
    bool  active;
    bool  sizeKnown;
    bool  positioned;
    float capturedW;
    float capturedH;
    float lastX;
    float lastY;
};
static AutoPanelLayout s_voteLayout[2];
static AutoPanelLayout s_allianceLayout;

/* Source-pixel anchor band for the right-edge column. Status panel
 * starts at x=321 (MAIN_OFFSET_X + MAIN_SCREEN_SIZE_X * TILE_SIZE_X)
 * and ends at x=437. Used as the centering bounds before zoom +
 * gameScale scaling. */
static constexpr float VOTE_ANCHOR_X_LEFT  = 321.0f;
static constexpr float VOTE_ANCHOR_X_RIGHT = 437.0f;
static constexpr float VOTE_ANCHOR_Y_TOP   = 0.0f;

static int voteLayoutIndex(uint8_t kind) {
    if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) return 0;
    if (kind == GAME_VOTE_KIND_SURRENDER)     return 1;
    return -1;
}

static bool rectsIntersect(float ax, float ay, float aw, float ah,
                            float bx, float by, float bw, float bh) {
    return !(ax + aw <= bx || bx + bw <= ax ||
             ay + ah <= by || by + bh <= ay);
}

/* Compute the on-screen anchor band in actual pixels, accounting for
 * integer-zoom render-target plus the non-integer blit scale used
 * when the window is resized between integer zoom levels.
 *
 *   on-screen X = destX + sourceX * zoomFactor * gameScale
 *
 * sourceX comes from VOTE_ANCHOR_X_* (game-source coords). */
static void autoPanelComputeAnchors(float *anchorXL, float *anchorXR,
                                    float *anchorY) {
    int rawZoom = sdl3DrawGetZoomFactor();
    if (rawZoom < 1) rawZoom = 1;
    float destX = 0.0f, destY = 0.0f, gameScale = 1.0f;
    sdl3DrawGetGameRect(&destX, &destY, NULL, NULL, &gameScale);
    if (gameScale <= 0.0f) gameScale = 1.0f;
    float effZoom = (float)rawZoom * gameScale;
    if (anchorXL) *anchorXL = destX + VOTE_ANCHOR_X_LEFT  * effZoom;
    if (anchorXR) *anchorXR = destX + VOTE_ANCHOR_X_RIGHT * effZoom;
    if (anchorY)  *anchorY  = destY + VOTE_ANCHOR_Y_TOP   * effZoom;
}

/* Pre-Begin step: set window position, size cap, and background
 * alpha for an auto-positioned panel. siblings (optional) are other
 * already-positioned panels we should stack underneath instead of
 * overlapping. */
static void autoPanelApply(AutoPanelLayout &lay,
                            float anchorXL, float anchorXR, float anchorY,
                            float guessW, float guessH,
                            AutoPanelLayout *const *siblings,
                            int numSiblings,
                            float bgAlpha) {
    float anchorCenter = 0.5f * (anchorXL + anchorXR);

    if (!lay.active) {
        /* Stage 1: first frame visible. Pre-guess the size to centre
         * roughly; stage 2 re-centres precisely once Begin gives us
         * the real content size. */
        lay.active = true;
        lay.sizeKnown = false;
        lay.positioned = false;
        float guessX = anchorCenter - guessW * 0.5f;
        for (int j = 0; j < numSiblings; j++) {
            const AutoPanelLayout *other = siblings[j];
            if (!other || !other->active || !other->positioned) continue;
            if (rectsIntersect(guessX, anchorY, guessW, guessH,
                               other->lastX, other->lastY,
                               other->capturedW, other->capturedH)) {
                guessX = other->lastX;
            }
        }
        if (guessX < anchorXL) guessX = anchorXL;
        ImGui::SetNextWindowPos(ImVec2(guessX, anchorY), ImGuiCond_Always);
        lay.lastX = guessX;
        lay.lastY = anchorY;
    } else if (lay.sizeKnown && !lay.positioned && lay.capturedW > 60.0f) {
        /* Stage 2: we now have the real auto-sized width; recentre. */
        float targetX = anchorCenter - lay.capturedW * 0.5f;
        float targetY = anchorY;
        for (int j = 0; j < numSiblings; j++) {
            const AutoPanelLayout *other = siblings[j];
            if (!other || !other->active || !other->positioned) continue;
            if (rectsIntersect(targetX, targetY, lay.capturedW, lay.capturedH,
                               other->lastX, other->lastY,
                               other->capturedW, other->capturedH)) {
                targetX = other->lastX;
                targetY = other->lastY + other->capturedH;
            }
        }
        if (targetX < anchorXL) targetX = anchorXL;
        ImGui::SetNextWindowPos(ImVec2(targetX, targetY), ImGuiCond_Always);
        lay.lastX = targetX;
        lay.lastY = targetY;
        lay.positioned = true;
    }
    /* Stage 3 (positioned): no SetNextWindowPos — user can drag. */

    /* Width cap: never extend past the WinBolo window's right edge.
     * AutoResize still fits to content; when capped, the height grows
     * downward instead. */
    {
        float windowRight = ImGui::GetIO().DisplaySize.x;
        float maxW = windowRight - lay.lastX;
        if (maxW < 60.0f) maxW = 60.0f;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(40.0f, 40.0f),
            ImVec2(maxW,  FLT_MAX));
    }

    ImGui::SetNextWindowBgAlpha(bgAlpha);
}

/* Post-Begin step: refresh captured size + on-screen position so
 * stage 2 can centre using the real size and stage 3 can honor user
 * drags. */
static void autoPanelCapture(AutoPanelLayout &lay) {
    ImVec2 wPos  = ImGui::GetWindowPos();
    ImVec2 wSize = ImGui::GetWindowSize();
    lay.capturedW = wSize.x;
    lay.capturedH = wSize.y;
    lay.sizeKnown = true;
    if (lay.positioned) {
        lay.lastX = wPos.x;
        lay.lastY = wPos.y;
    }
}

static void autoPanelReset(AutoPanelLayout &lay) {
    lay.active = false;
    lay.sizeKnown = false;
    lay.positioned = false;
}

/* Controller-mode hint: bound glyph for `action` followed by `text`,
   pointing the player at the Players panel where the action lives. */
static void renderControllerActionHint(const char *action, const char *text) {
    SDL_Texture *glyph = action ? glyphForActionAuto(action) : NULL;
    if (glyph) {
        const float h = ImGui::GetFrameHeight();
        ImGui::Image((ImTextureID)glyph, ImVec2(h, h));
        ImGui::SameLine();
    }
    ImGui::TextWrapped("%s", text);
}

static void renderOneGameVoteWidget(ClientSim *cs, uint8_t kind,
                                    const ClientGameVoteSnapshot *snap) {
    int li = voteLayoutIndex(kind);
    if (li < 0) return;
    AutoPanelLayout &lay = s_voteLayout[li];

    /* Only render while a vote is in-flight or recently concluded
     * and the user hasn't dismissed. */
    if (snap->active == GAME_VOTE_ACTIVE_NONE) { autoPanelReset(lay); return; }
    if (!snap->widgetVisible)                  { autoPanelReset(lay); return; }
    /* Surrender votes are private to the surrendering team — don't render the
     * floating widget for anyone outside that team. */
    if (kind == GAME_VOTE_KIND_SURRENDER && !localCanAnswerGameVote(cs, snap)) {
        autoPanelReset(lay); return;
    }

    /* Auto-dismiss 5 seconds after the vote concludes (pass / fail /
     * cancel). Back-to-lobby with the server's return-to-lobby
     * countdown still active is exempt — keep the widget through the
     * full N → 1 countdown. */
    if (snap->active != GAME_VOTE_ACTIVE_RUNNING && snap->concludedAtMs != 0) {
        bool lobbyCountdownActive =
            (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) &&
            (clientSimGetReturnToLobbySecs(cs) > 0);
        uint32_t age = SDL_GetTicks() - snap->concludedAtMs;
        if (age >= 5000u && !lobbyCountdownActive) {
            clientSimSetGameVoteWidgetVisible(cs, kind, false);
            autoPanelReset(lay);
            return;
        }
    }

    /* Apply the shared auto-panel layout (anti-overlap clamp + width
     * cap + transparency). Siblings: the other vote widget (so they
     * stack instead of overlapping each other). */
    float anchorXL, anchorXR, anchorY;
    autoPanelComputeAnchors(&anchorXL, &anchorXR, &anchorY);
    AutoPanelLayout *siblings[1] = { &s_voteLayout[1 - li] };
    autoPanelApply(lay, anchorXL, anchorXR, anchorY,
                    200.0f, 140.0f, siblings, 1, 0.55f);

    /* Build the title — includes (Draw) tag for ranked manual
     * back-to-lobby votes. The ###id suffix is ImGui's internal
     * window identifier and stays out of the localized portion. */
    char title[128];
    const char *kindName = (kind == GAME_VOTE_KIND_BACK_TO_LOBBY)
                           ? langGetText(STR_VOTE_BACK_TO_LOBBY)
                           : langGetText(STR_VOTE_SURRENDER);
    bool drawTag = (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) &&
                   (snap->triggerSrc == GAME_VOTE_TRIGGER_MANUAL) &&
                   clientSimGetLobbyRanked(cs);
    snprintf(title, sizeof(title), "%s%s###gamevote_%u",
             kindName,
             drawTag ? langGetText(STR_VOTE_DRAW_TAG) : "",
             (unsigned)kind);

    bool open = true;
    if (!ImGui::Begin(title, &open,
                      ImGuiWindowFlags_AlwaysAutoResize |
                      ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoSavedSettings |
                      ImGuiWindowFlags_NoFocusOnAppearing |
                      ImGuiWindowFlags_NoBringToFrontOnFocus |
                      ImGuiWindowFlags_NoNavInputs)) {
        ImGui::End();
        if (!open) { clientSimSetGameVoteWidgetVisible(cs, kind, false); autoPanelReset(lay); }
        return;
    }

    autoPanelCapture(lay);

    /* Tally + circular progress.
     *
     * Ring is split into one slice per eligible voter:
     *   - green  : voted yes
     *   - red    : voted no
     *   - gray   : not yet voted (background)
     *
     * When eligibleCount is unknown (legacy server / not running) we
     * fall back to a single yes-vs-threshold green arc on a gray
     * background, matching the pre-noCount behavior. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 base = ImGui::GetCursorScreenPos();
    float radius = 22.0f;
    ImVec2 centre(base.x + radius + 2.0f, base.y + radius + 2.0f);

    /* Background ring (drawn always; the colored arcs paint over it). */
    dl->AddCircle(centre, radius, IM_COL32(120, 120, 120, 200), 36, 3.0f);

    const ImU32 kYes  = IM_COL32(80, 200, 80, 255);
    const ImU32 kNo   = IM_COL32(220, 70, 70, 255);
    const float kStart = -IM_PI * 0.5f;   /* 12 o'clock */

    if (snap->eligibleCount > 0) {
        float slice = (2.0f * IM_PI) / (float)snap->eligibleCount;
        if (snap->yesCount > 0) {
            float a0 = kStart;
            float a1 = a0 + slice * (float)snap->yesCount;
            dl->PathArcTo(centre, radius, a0, a1, 36);
            dl->PathStroke(kYes, 0, 3.5f);
        }
        if (snap->noCount > 0) {
            float a0 = kStart + slice * (float)snap->yesCount;
            float a1 = a0 + slice * (float)snap->noCount;
            dl->PathArcTo(centre, radius, a0, a1, 36);
            dl->PathStroke(kNo, 0, 3.5f);
        }
    } else if (snap->threshold > 0 && snap->yesCount > 0) {
        float progress = (float)snap->yesCount / (float)snap->threshold;
        if (progress > 1.0f) progress = 1.0f;
        float a0 = kStart;
        float a1 = a0 + progress * IM_PI * 2.0f;
        dl->PathArcTo(centre, radius, a0, a1, 36);
        dl->PathStroke(kYes, 0, 3.5f);
    }

    /* Reserve the space for the ring + put the tally text next to it. */
    ImGui::Dummy(ImVec2(radius * 2 + 8, radius * 2 + 4));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%u / %u",
                (unsigned)snap->yesCount, (unsigned)snap->threshold);
    if (snap->active == GAME_VOTE_ACTIVE_RUNNING) {
        /* Solo (single eligible voter) "are you sure?" grace: server
         * gives one-human votes 5 s after the yes before firing, so
         * a misclick is reversible. Multi-human votes fire instantly
         * on unanimity, so this branch only ever shows for solo. */
        bool soloPendingPass = (snap->threshold == 1 &&
                                snap->yesCount >= snap->threshold);
        if (soloPendingPass) {
            ImGui::TextColored(ImVec4(0.0f, 0.85f, 0.0f, 1.0f),
                               "Passing in %us...",
                               (unsigned)snap->secondsRemaining);
        } else if (snap->eligibleCount > 1) {
            /* Only show the countdown deadline when there's more than one
             * voter — for a solo vote it's meaningless since the
             * single voter decides instantly on yes. */
            ImGui::TextDisabled("%us left", (unsigned)snap->secondsRemaining);
        }
    } else if (snap->active == GAME_VOTE_ACTIVE_PASSED) {
        /* For back-to-lobby: show "Return to lobby in N" while the
         * server's snapshot-driven countdown is still running, then
         * fall back to plain "Passed" once it's expired (or for
         * surrender, which doesn't drive the countdown itself). */
        uint8_t rtlSecs = clientSimGetReturnToLobbySecs(cs);
        if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY && rtlSecs > 0) {
            /* Wrap so the line fits when the widget is width-capped
             * against a narrow status column (e.g. at 1x zoom). */
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.0f, 0.9f, 0.0f, 1.0f));
            ImGui::TextWrapped("Return to lobby in %u", (unsigned)rtlSecs);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(ImVec4(0.0f, 0.9f, 0.0f, 1.0f), "Passed");
        }
    } else if (snap->active == GAME_VOTE_ACTIVE_FAILED) {
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.2f, 1.0f), "Failed");
    } else if (snap->active == GAME_VOTE_ACTIVE_CANCELLED) {
        ImGui::TextDisabled("Cancelled");
    }
    ImGui::EndGroup();

    /* Yes / No buttons — only meaningful while the vote is running.
     * Highlight the user's current choice so they can see their stance. */
    if (snap->active == GAME_VOTE_ACTIVE_RUNNING &&
        localCanAnswerGameVote(cs, snap)) {
        ImGui::Spacing();
        if (uiShouldUseControllerMode()) {
            renderControllerActionHint(SI_ACTION_VIEW_PLAYERS,
                                       langGetText(STR_VOTE_RESPOND_IN_PLAYERS));
        } else {
            BYTE me = clientSimGetMyPlayerNum(cs);
            bool myYes = (me < 16) && ((snap->votes >> me) & 1u);

            if (myYes) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
            if (ImGui::Button(langGetText(STR_YES), ImVec2(80, 0))) {
                clientSimNetSendGameVoteToggle(cs, kind, GAME_VOTE_TOGGLE_YES);
            }
            if (myYes) ImGui::PopStyleColor();

            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_NO), ImVec2(80, 0))) {
                clientSimNetSendGameVoteToggle(cs, kind, GAME_VOTE_TOGGLE_NO);
            }
        }
    }

    ImGui::End();
    if (!open) {
        clientSimSetGameVoteWidgetVisible(cs, kind, false);
        autoPanelReset(lay);
    }
}

static void renderGameVoteWidgets(ClientSim *cs) {
    if (!cs) return;
    if (clientSimGetNetStatus(cs) != netRunning) return;
    static const uint8_t kinds[] = {
        GAME_VOTE_KIND_BACK_TO_LOBBY, GAME_VOTE_KIND_SURRENDER
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        ClientGameVoteSnapshot snap = {};
        if (!clientSimGetGameVote(cs, kinds[i], &snap)) continue;
        renderOneGameVoteWidget(cs, kinds[i], &snap);
    }
}

/* -------------------------------------------------------
 * Alliance Request modal
 * ------------------------------------------------------- */
static void renderAllianceRequest(ClientSim *cs) {
    if (s_showAllianceOpen) {
        s_allianceVisible = true;
        s_showAllianceOpen = false;
    }
    if (!s_allianceVisible) { autoPanelReset(s_allianceLayout); return; }

    /* Use the same auto-positioning + width cap + transparency as
     * the vote widgets. Siblings: both vote widgets so the alliance
     * request stacks below any active vote. */
    float anchorXL, anchorXR, anchorY;
    autoPanelComputeAnchors(&anchorXL, &anchorXR, &anchorY);
    AutoPanelLayout *siblings[2] = { &s_voteLayout[0], &s_voteLayout[1] };
    autoPanelApply(s_allianceLayout, anchorXL, anchorXR, anchorY,
                    220.0f, 100.0f, siblings, 2, 0.55f);

    char title[128];
    snprintf(title, sizeof(title), "%s###alliancereq", langGetText(STR_DLGALLIANCE_TITLE));
    if (ImGui::Begin(title, &s_allianceVisible,
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNavInputs)) {
        autoPanelCapture(s_allianceLayout);
        {
            MessageArgs args = {};
            strncpy(args.playerName, s_alliancePlayerName, sizeof(args.playerName) - 1);
            args.playerFlags = clientSimGetPlayerAccountFlags(cs, s_alliancePlayerNum);
            clientSimGetPlayerCountryCode(cs, s_alliancePlayerNum, args.playerCountry);
            ImGui::TextWrapped("%s", langGetTextFmt(STR_DLGALLIANCE_BLURB, &args));
        }
        ImGui::Spacing();
        if (uiShouldUseControllerMode()) {
            renderControllerActionHint(SI_ACTION_VIEW_PLAYERS,
                                       langGetText(STR_ALLIANCE_RESPOND_IN_PLAYERS));
        } else {
            if (ImGui::Button(langGetText(STR_DLGALLIANCE_ACCEPT), ImVec2(80, 0))) {
                clientSimAllianceAccept(cs, s_alliancePlayerNum);
                s_allianceVisible = false;
            }
            imguiHandOnHover();
            ImGui::SameLine();
            if (ImGui::Button(langGetText(STR_DLGALLIANCE_DECLINE), ImVec2(80, 0))) {
                s_allianceVisible = false;
            }
            imguiHandOnHover();
        }
    }
    ImGui::End();
    if (!s_allianceVisible) autoPanelReset(s_allianceLayout);
}

/* -------------------------------------------------------
 * Password modal — shown when joining a protected game
 * ------------------------------------------------------- */
static void renderPasswordModal(void) {
    char title[128];
    snprintf(title, sizeof(title), "%s###passwordreq", langGetText(STR_DLGPASSWORD_TITLE));
    if (s_showPasswordOpen) {
        ImGui::OpenPopup(title);
        s_showPasswordOpen  = false;
        s_passwordBuf[0]   = '\0';
    }
    static float s_fadePassword = 0.0f;
    bool passOpen = true;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(title, &passOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            imguiPopupFadeAlpha(&s_fadePassword));
        if (s_closeAllPopups) { ImGui::PopStyleVar(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted(langGetText(STR_DLGPASSWORD_BLURB));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        ImGui::SetNextItemWidth(270);
        bool enter = ImGui::InputText("##pass", s_passwordBuf,
                                      sizeof(s_passwordBuf),
                                      ImGuiInputTextFlags_Password |
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        int f = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                   langGetText(STR_OK),
                                   /*enterConfirms*/ true);
        if (f == WBUI::FOOTER_CONFIRM || enter) {
            /* gameOpen=1, aiNone=0, justPass=TRUE */
            gameFrontSetGameOptions(s_passwordBuf, (gameType)1, false, (aiType)0, 0, 0, true);
            ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_CANCEL) {
            /* Abort the join attempt. */
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    }
}

/* -------------------------------------------------------
 * Gamepad binding helpers — orphaned from their original host
 * popup (renderKeySetupModal, removed when main moved the in-game
 * Key Setup popup to imgui_keysetup.cpp:imguiKeySetupRenderInGamePopup).
 * Kept here so a follow-up commit can hook them into the new popup
 * to restore controller rebinding UI.  The keyboard-side scaffolding
 * (keySetupRow, keySetupFieldPtr, keySetupScancodeLabel) is gone with
 * the popup; its replacement lives in imgui_keysetup.cpp.
 * ------------------------------------------------------- */

/* Returns a stable short label for the binding's button/axis using
 * SDL_GetGamepadStringForButton/Axis.  Used for both the procedural
 * keycap fallback text and the default row label when no PNG art
 * exists in the active glyph set. */
static const char *gamepadBindingLabel(const GamepadBinding *b) {
    if (!b) return "-";
    if (b->kind == GP_BIND_NONE) return "-";
    if (b->kind == GP_BIND_BUTTON) {
        const char *n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b->code);
        return (n && n[0]) ? n : "?";
    }
    if (b->kind == GP_BIND_TRIGGER) {
        const char *n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)b->code);
        return (n && n[0]) ? n : "?";
    }
    return "?";
}

/* Snapshot trigger state — a trigger that's already pulled at the
   moment of a Change-click must release before a re-press counts as
   the player's choice.  Shared by both slot-Change buttons. */
static void armTriggersForCapture(void) {
    SDL_Gamepad *gp = inputGamepadGetActiveHandle();
    for (int t = 0; t < 2; ++t) {
        SDL_GamepadAxis ax = (t == 0) ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                      : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        float v = gp ? (float)SDL_GetGamepadAxis(gp, ax) * (1.0f / 32767.0f) : 0.0f;
        s_keySetupTrigArmed[t] = (v <= 0.5f);
    }
}

/* Renders one slot's glyph + label, or a faded "—" placeholder when
   the binding is NONE.  Used by gamepadSetupRow for both pri and sec. */
static void gamepadSetupRenderSlot(const GamepadBinding *b) {
    float textLineH = ImGui::GetTextLineHeight();
    float glyphSize = textLineH * 1.5f;
    float glyphYOff = (glyphSize - textLineH) * 0.5f;
    float cursorY   = ImGui::GetCursorPosY();

    if (!b || b->kind == GP_BIND_NONE) {
        /* Faded em-dash placeholder; reserves the same vertical room
           as a real glyph so rows keep aligning. */
        ImGui::SetCursorPosY(cursorY - glyphYOff);
        ImGui::Dummy(ImVec2(glyphSize, glyphSize));
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetCursorPosY(cursorY);
        ImGui::TextDisabled("%s", "-");
        return;
    }

    const char  *name  = gamepadBindingLabel(b);
    SDL_Texture *glyph = NULL;
    if (b->kind == GP_BIND_BUTTON) {
        glyph = glyphForGamepadButton((SDL_GamepadButton)b->code);
    } else if (b->kind == GP_BIND_TRIGGER) {
        glyph = glyphForGamepadAxis((SDL_GamepadAxis)b->code);
    }
    ImGui::SetCursorPosY(cursorY - glyphYOff);
    if (glyph) {
        ImGui::Image((ImTextureID)glyph, ImVec2(glyphSize, glyphSize));
    } else {
        drawProceduralKeycapAt(ImGui::GetCursorScreenPos(), glyphSize, name);
        ImGui::Dummy(ImVec2(glyphSize, glyphSize));
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetCursorPosY(cursorY);
    ImGui::TextUnformatted(name);
}

/* Render a single gamepad-binding row with both slots:
   "Label  [pri glyph][pri text]  [sec glyph][sec text]  [Change Pri][Change Sec]" */
static void gamepadSetupRow(const char *label, GamepadAction action) {
    const GamepadBinding *bp = &s_keySetupGamepadBindings.b[(int)action].pri;
    const GamepadBinding *bs = &s_keySetupGamepadBindings.b[(int)action].sec;
    bool waitingPri = (s_keySetupGamepadWaitingAction == (int)action &&
                       s_keySetupGamepadWaitingSlot   == GP_SLOT_PRIMARY);
    bool waitingSec = (s_keySetupGamepadWaitingAction == (int)action &&
                       s_keySetupGamepadWaitingSlot   == GP_SLOT_SECONDARY);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);

    /* Column 1: both slot glyphs side by side, separated by a tab-wide
       gap.  When a slot is waiting for capture, its half shows the
       prompt instead. */
    ImGui::TableSetColumnIndex(1);
    if (waitingPri) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_GP_REBIND_PROMPT));
    } else {
        gamepadSetupRenderSlot(bp);
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2.0f);
    if (waitingSec) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_GP_REBIND_PROMPT));
    } else {
        gamepadSetupRenderSlot(bs);
    }

    /* Column 2: two stacked SmallButtons (Change Pri / Change Sec).
       While one slot is being captured, its button reads "Cancel"
       and the other slot's button stays enabled but inert via PushID
       isolation. */
    ImGui::TableSetColumnIndex(2);
    ImGui::PushID(0x1000 + (int)action);
    if (waitingPri) {
        if (ImGui::SmallButton(langGetText(STR_CANCEL))) {
            s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
        }
    } else {
        if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
            s_keySetupGamepadWaitingAction = (int)action;
            s_keySetupGamepadWaitingSlot   = GP_SLOT_PRIMARY;
            armTriggersForCapture();
        }
    }
    ImGui::PushID(1);
    if (waitingSec) {
        if (ImGui::SmallButton(langGetText(STR_CANCEL))) {
            s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
        }
    } else {
        if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
            s_keySetupGamepadWaitingAction = (int)action;
            s_keySetupGamepadWaitingSlot   = GP_SLOT_SECONDARY;
            armTriggersForCapture();
        }
    }
    ImGui::PopID();
    ImGui::PopID();
}

/* -------------------------------------------------------
 * Settings panel — collects Edit + WinBolo menu options
 * into a single window for in-game use and mobile.
 * ------------------------------------------------------- */
static void renderSettingsPanel(ClientSim *cs) {
    if (!s_showSettings) return;

    if (uiModeIsTablet()) {
        ImGuiIO &io = ImGui::GetIO();
        float w = io.DisplaySize.x * 0.8f;
        float h = io.DisplaySize.y * 0.8f;
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    } else {
        /* Scale the panel with the UI scale — the font and style sizes are
           bumped on Deck (1.5x) and high-DPI desktop, so a fixed 520px window
           clips the wider translated labels and combos. */
        ImGui::SetNextWindowSize(ImVec2(680 * s_uiScale, 580 * s_uiScale),
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(280 * s_uiScale, 200 * s_uiScale),
                                            ImVec2(FLT_MAX, FLT_MAX));
    }
    bool *pOpen = uiModeIsTablet() ? nullptr : &s_showSettings;
    ImGuiWindowFlags flags = uiModeIsTablet() ? (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse) : 0;
    char title[128];
    snprintf(title, sizeof(title), "%s###settings", langGetText(STR_DLGSETTINGS_TITLE));
    if (!ImGui::Begin(title, pOpen, flags)) {
        ImGui::End();
        return;
    }

    SettingsRenderCtx ctx = {};
    ctx.cs = cs;
    ctx.inGame = true;
    ctx.pendingZoom = 255;
    ctx.pendingFullScreen = -1;

    /* Controller tab cycling: shoulder buttons (or the Steam menu-tab actions
       where the pad is hidden from SDL) step through the tabs, wrapping at the
       ends.  Every in-game tab is present except Hosting in the web build,
       where a browser tab can't listen for connections. */
    enum { STAB_GENERAL, STAB_DISPLAY, STAB_SOUND, STAB_CONTROLS, STAB_GAMEHUD, STAB_HOSTING, STAB_LAST, STAB_COUNT };
    static int s_igActiveTab = STAB_GENERAL;
    static int s_igForceTab  = -1;
    bool present[STAB_COUNT];
    present[STAB_GENERAL]  = true;
    present[STAB_DISPLAY]  = true;
    present[STAB_SOUND]    = true;
    present[STAB_CONTROLS] = true;
    present[STAB_GAMEHUD]  = true;
#if defined(__EMSCRIPTEN__)
    present[STAB_HOSTING]  = false;
#else
    present[STAB_HOSTING]  = true;
#endif
    present[STAB_LAST]     = true;
    {
        int shift = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0)
                  - (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? 1 : 0);
        if (shift == 0) shift = imguiSteamNavConsumeMenuTabShift();
        if (shift != 0) {
            int i = s_igActiveTab;
            do { i = (i + shift + STAB_COUNT) % STAB_COUNT; } while (!present[i] && i != s_igActiveTab);
            s_igForceTab = i;
        }
    }

    if (ImGui::BeginTabBar("##settingsTabs")) {
        if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT_GENERAL), nullptr,
                s_igForceTab == STAB_GENERAL ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_igActiveTab = STAB_GENERAL;
            ImGui::BeginChild("##generalPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            imguiSettingsRenderGeneralTab(&ctx);
            /* Scan the installed languages once and cache for the process
               lifetime — they don't change at runtime, so the entries are
               intentionally never freed. */
            static LangFileEntry *s_langEntries = nullptr;
            static int            s_langCount   = 0;
            static bool           s_langScanned = false;
            if (!s_langScanned) {
                s_langEntries = langPickerScan(&s_langCount);
                s_langScanned = true;
            }
            imguiSettingsRenderLanguagePicker(s_langEntries, s_langCount, &ctx);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_DISPLAY), nullptr,
                s_igForceTab == STAB_DISPLAY ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_igActiveTab = STAB_DISPLAY;
            ImGui::BeginChild("##displayPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            imguiSettingsRenderDisplayTab(&ctx);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_SOUND), nullptr,
                s_igForceTab == STAB_SOUND ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_igActiveTab = STAB_SOUND;
            ImGui::BeginChild("##soundPanelIG", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            imguiSettingsRenderSoundTab(&ctx);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(langGetText(STR_LV_WIN_CONTROLS), nullptr,
                s_igForceTab == STAB_CONTROLS ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_igActiveTab = STAB_CONTROLS;
            ImGui::BeginChild("##controlsPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            imguiSettingsRenderControlsTab(&ctx);
            if (uiModeIsTablet() || inputGamepadIsConnected()) {
                bool relSteering = !inputTouchGetAbsoluteSteering();
                if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_RELSTEER), &relSteering)) {
                    inputTouchSetAbsoluteSteering(!relSteering);
                }
                imguiHelpTooltip(langGetText(STR_DLGSETTINGS_RELSTEER_TIP));
            }

            if (inputGamepadIsConnected()) {
                ImGui::Separator();
                ImGui::Text("Gamepad: connected");

                float s = g_gamepadScrollSensitivity;
                if (ImGui::SliderFloat("Scroll sensitivity", &s, 0.25f, 4.0f, "%.2fx")) {
                    if (s < 0.25f) s = 0.25f;
                    if (s > 4.0f)  s = 4.0f;
                    g_gamepadScrollSensitivity = s;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_TAB_GAMEHUD), nullptr,
                s_igForceTab == STAB_GAMEHUD ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_igActiveTab = STAB_GAMEHUD;
            ImGui::BeginChild("##gamehudPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            imguiSettingsRenderGameHudTab(&ctx);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
#if !defined(__EMSCRIPTEN__)
        if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_TAB_HOSTING), nullptr,
                s_igForceTab == STAB_HOSTING ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_igActiveTab = STAB_HOSTING;
            ImGui::BeginChild("##hostingPanelIG", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            imguiSettingsRenderHostingTab(&ctx);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
#endif
        if (ImGui::BeginTabItem(langGetText(STR_DLGSETTINGS_TAB_SESSION), nullptr,
                s_igForceTab == STAB_LAST ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_igActiveTab = STAB_LAST;
            ImGui::BeginChild("##sessionPanel", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            /* File actions — Save Map reachable on tablet and under a controller
               (desktop has the menu bar); Leave Game stays tablet-only. */
            if (uiModeIsTablet() || uiShouldUseControllerMode()) {
                if (ImGui::Button(langGetText(STR_MENU_SAVE_MAP), ImVec2(-1, 0))) {
                    windowSaveMap(cs);
                    s_showSettings = false;
                }
                imguiHandOnHover();
                if (uiModeIsTablet()) {
                    if (ImGui::Button(langGetText(STR_MENU_LEAVE_GAME), ImVec2(-1, 0))) {
                        windowNewGame();
                    }
                    imguiHandOnHover();
                }
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }

            /* ---- Game ---- */
            ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_GAME));
            {
                bool anp = (bool)allowNewPlayers;
                if (ImGui::Checkbox(langGetText(STR_ALLOW_NEW_PLAYERS), &anp)) {
                    windowMenuAllowNewPlayers_toggle(cs);
                }
            }

            /* ---- Info ---- */
            ImGui::Separator();
            if (ImGui::Selectable(langGetText(STR_DLGGAMEINFO_TITLE), s_showGameInfo)) s_showGameInfo = !s_showGameInfo;
            if (ImGui::Selectable(langGetText(STR_DLGSYSINFO_TITLE),  s_showSysInfo))  { if (!s_showSysInfo) sysInfoGraphReset(); s_showSysInfo = !s_showSysInfo; }
            if (ImGui::Selectable(langGetText(STR_DLGNETINFO_TITLE),  s_showNetInfo))  { if (!s_showNetInfo) pingGraphReset();  s_showNetInfo = !s_showNetInfo; }

            /* ---- Brains ---- */
            if (clientSimGetAiType(cs) != aiNone) {
                ImGui::SeparatorText(langGetText(STR_MENU_BRAINS));
                bool running = luaBrainIsRunning() != 0;
                int  runIdx  = luaBrainGetRunningIndex();

                /* Manual (stop brain) entry — selected when no brain is active */
                if (ImGui::Selectable(langGetText(STR_MENU_MANUAL), !running)) {
                    if (running) {
                        luaBrainStop();
                        mlBrainStopSingleton();
                    }
                }

                /* One entry per discovered brain */
                int numBrains = luaBrainGetNum();
                if (numBrains > 0) {
                    ImGui::Separator();
                    for (int bi = 0; bi < numBrains; bi++) {
                        const char *name = luaBrainGetName(bi);
                        bool isActive    = running && (bi == runIdx);
                        if (ImGui::Selectable(name ? name : "?", isActive)) {
                            if (!isActive) {
                                const char *path = luaBrainGetPath(bi);
                                if (path) {
                                    if (luaBrainGetType(bi) == BRAIN_TYPE_ONNX) {
                                        mlBrainStartSingleton(path, name ? name : "", cs);
                                    } else {
                                        luaBrainStart(path, name ? name : "", cs);
                                    }
                                    /* Refresh settings descriptor for the new brain */
                                    luaBrainFreeSettings(s_brainSettings);
                                    s_brainSettings      = nullptr;
                                    s_brainSettingsCount = 0;
                                    s_brainSettingsOpen  = false;
                                }
                            }
                        }
                    }
                }

                /* Settings entry — only when a Lua brain is running (ONNX has no settings) */
                if (running && !mlBrainSingletonIsRunning()) {
                    ImGui::Separator();
                    if (ImGui::Button(langGetText(STR_MENU_SETTINGS))) {
                        /* Re-fetch on every open so values are current */
                        luaBrainFreeSettings(s_brainSettings);
                        s_brainSettings      = luaBrainGetSettings(&s_brainSettingsCount);
                        s_brainSettingsOpen  = true;
                    }
                }
            }

            /* ---- About ---- */
            ImGui::Separator();
            if (ImGui::Button(langGetText(STR_MENU_ABOUT))) {
                sdl3ImguiShowAbout();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    s_igForceTab = -1;

    /* Apply the Display & Sound tab's deferred outputs into the file statics
       the existing end-of-frame consumers already act on. */
    if (ctx.pendingZoom != 255)  s_pendingZoom = ctx.pendingZoom;
    if (ctx.wantAtlasRebuild)    s_pendingUiScaleRebuild = true;
    if (ctx.wantSkinReload)      s_pendingSkinReload = true;
    if (ctx.wantKeySetup)        sdl3ImguiShowKeySetup();
    if (ctx.pendingFullScreen >= 0) s_pendingFullScreen = ctx.pendingFullScreen;

    ImGui::End();
}

/* -------------------------------------------------------
 * renderMenuBar — builds the ImGui main-menu-bar frame.
 * Called inside sdl3ImguiPumpAndRender between
 * ImGui::NewFrame() and ImGui::Render().
 * ------------------------------------------------------- */
static void renderMenuBar(ClientSim *cs) {
    if (!ImGui::BeginMainMenuBar()) return;

    /* ---- File ---------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_FILE))) {
#ifndef __EMSCRIPTEN__
        if (ImGui::MenuItem(langGetText(STR_MENU_NEW)))                       windowNewGame();
#endif
        if (ImGui::MenuItem(langGetText(STR_MENU_SAVE_MAP), KMOD_PRIMARY_LABEL "S"))        windowSaveMap(cs);
        ImGui::Separator();
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        if (!uiModeIsTablet()) {
            /* While the overview owns the game window these open as in-window
               panels instead of pop-outs: a separate OS window would land
               behind the map the player is looking at. A pop-out already open
               when the mode is entered keeps winning — each panel returns
               early while its pop-out is up. */
            bool overviewInWindow = sdl3DrawIsOverviewInWindow();
            if (ImGui::MenuItem(langGetText(STR_DLGGAMEINFO_TITLE), nullptr,
                                overviewInWindow ? s_showGameInfo : s_popGameInfo.open)) {
                if (overviewInWindow) s_showGameInfo = !s_showGameInfo;
                else togglePopOut(&s_popGameInfo, langGetText(STR_DLGGAMEINFO_TITLE), 320, 200, 0);
            }
            if (ImGui::MenuItem(langGetText(STR_DLGSYSINFO_TITLE), nullptr,
                                overviewInWindow ? s_showSysInfo : s_popSysInfo.open)) {
                if (overviewInWindow) {
                    if (!s_showSysInfo) sysInfoGraphReset();
                    s_showSysInfo = !s_showSysInfo;
                } else togglePopOut(&s_popSysInfo, langGetText(STR_DLGSYSINFO_TITLE), 440, 600, 0);
            }
            if (ImGui::MenuItem(langGetText(STR_DLGNETINFO_TITLE), nullptr,
                                overviewInWindow ? s_showNetInfo : s_popNetInfo.open)) {
                if (overviewInWindow) {
                    if (!s_showNetInfo) pingGraphReset();
                    s_showNetInfo = !s_showNetInfo;
                } else togglePopOut(&s_popNetInfo, langGetText(STR_DLGNETINFO_TITLE), 360, 420, 0);
            }
            ImGui::Separator();
            /* The overview draws the map the player has seen, so it stays
               greyed out until a game is running — and all the way through
               full screen mode, which fills the window with that same map.
               Classic mode greys both out as well, and that is the one
               reason worth a tooltip: waiting for a game to start explains
               itself, a server rule does not. */
            const bool classicMenu = classicModeActive();
            if (ImGui::MenuItem(langGetText(STR_MENU_MAP_OVERVIEW), KMOD_PRIMARY_LABEL "O", s_popMapOverview.open,
                                cs != nullptr && clientSimIsRunning(cs) && !gameFrontFullScreen && !classicMenu)) {
                if (s_popMapOverview.open) mapOverviewClose(); else mapOverviewOpen();
            }
            if (classicMenu && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", langGetText(STR_MENU_CLASSIC_MODE_TIP));
            if (ImGui::MenuItem(langGetText(STR_MENU_OVERVIEW_IN_WINDOW), "Alt+Enter",
                                sdl3DrawIsOverviewInWindow(),
                                cs != nullptr && clientSimIsRunning(cs) && !classicMenu)) {
                overviewInWindowChoose(!sdl3DrawIsOverviewInWindow());
            }
            if (classicMenu && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", langGetText(STR_MENU_CLASSIC_MODE_TIP));
        } else {
#endif
            if (ImGui::MenuItem(langGetText(STR_DLGGAMEINFO_TITLE),    nullptr, s_showGameInfo))  s_showGameInfo  = !s_showGameInfo;
            if (ImGui::MenuItem(langGetText(STR_DLGSYSINFO_TITLE),     nullptr, s_showSysInfo))   { if (!s_showSysInfo) sysInfoGraphReset(); s_showSysInfo = !s_showSysInfo; }
            if (ImGui::MenuItem(langGetText(STR_DLGNETINFO_TITLE),     nullptr, s_showNetInfo))   { if (!s_showNetInfo) pingGraphReset(); s_showNetInfo = !s_showNetInfo; }
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        ImGui::Separator();
#ifndef __EMSCRIPTEN__
        if (ImGui::MenuItem(langGetText(STR_MENU_EXIT)))                     windowSetQuitting();
#else
        if (ImGui::MenuItem(langGetText(STR_MENU_LEAVE_GAME)))               windowLeaveGame();
#endif
        ImGui::EndMenu();
    }

    /* ---- Edit ---------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_EDIT))) {
        if (ImGui::BeginMenu(langGetText(STR_MENU_FRAME_RATE))) {
            if (ImGui::MenuItem("60", nullptr, frameRate == FRAME_RATE_60)) windowSetFrameRate(FRAME_RATE_60, true);
            if (ImGui::MenuItem("50", nullptr, frameRate == FRAME_RATE_50)) windowSetFrameRate(FRAME_RATE_50, true);
            if (ImGui::MenuItem("30", nullptr, frameRate == FRAME_RATE_30)) windowSetFrameRate(FRAME_RATE_30, true);
            if (ImGui::MenuItem("20", nullptr, frameRate == FRAME_RATE_20)) windowSetFrameRate(FRAME_RATE_20, true);
            if (ImGui::MenuItem("15", nullptr, frameRate == FRAME_RATE_15)) windowSetFrameRate(FRAME_RATE_15, true);
            if (ImGui::MenuItem("12", nullptr, frameRate == FRAME_RATE_12)) windowSetFrameRate(FRAME_RATE_12, true);
            if (ImGui::MenuItem("10", nullptr, frameRate == FRAME_RATE_10)) windowSetFrameRate(FRAME_RATE_10, true);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(langGetText(STR_MENU_WINDOW_SIZE))) {
            /* Get display bounds to disable sizes that don't fit */
            int dispW = 99999, dispH = 99999;
            if (s_window) {
                SDL_DisplayID dispID = SDL_GetDisplayForWindow(s_window);
                SDL_Rect usable;
                if (SDL_GetDisplayUsableBounds(dispID, &usable)) {
                    dispW = usable.w;
                    dispH = usable.h;
                }
            }
            /* Cardinal sizes: width = zoom * 515, height = zoom * 325 + MENU_BAR_HEIGHT */
            bool fit1x = (1 * SDL3_SCREEN_W <= dispW) && (1 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);
            bool fit2x = (2 * SDL3_SCREEN_W <= dispW) && (2 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);
            bool fit3x = (3 * SDL3_SCREEN_W <= dispW) && (3 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);
            bool fit4x = (4 * SDL3_SCREEN_W <= dispW) && (4 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);

            auto requiresTip = [](int zoom) {
                MessageArgs args = {};
                args.number = zoom * SDL3_SCREEN_W;
                args.number2 = zoom * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
                ImGui::SetTooltip("%s", langGetTextFmt(STR_MENU_REQUIRES, &args));
            };
            ImGui::BeginDisabled(!fit1x);
            if (ImGui::MenuItem(langGetText(STR_MENU_NORMAL), nullptr, zoomFactor == ZOOM_FACTOR_NORMAL)) s_pendingZoom = ZOOM_FACTOR_NORMAL;
            if (!fit1x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                requiresTip(1);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!fit2x);
            if (ImGui::MenuItem(langGetText(STR_MENU_DOUBLE), nullptr, zoomFactor == ZOOM_FACTOR_DOUBLE)) s_pendingZoom = ZOOM_FACTOR_DOUBLE;
            if (!fit2x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                requiresTip(2);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!fit3x);
            if (ImGui::MenuItem(langGetText(STR_MENU_TRIPLE), nullptr, zoomFactor == ZOOM_FACTOR_TRIPLE)) s_pendingZoom = ZOOM_FACTOR_TRIPLE;
            if (!fit3x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                requiresTip(3);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!fit4x);
            if (ImGui::MenuItem(langGetText(STR_MENU_QUAD),   nullptr, zoomFactor == ZOOM_FACTOR_QUAD))   s_pendingZoom = ZOOM_FACTOR_QUAD;
            if (!fit4x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                requiresTip(4);
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_MENU_CUSTOM_RESIZABLE), nullptr, zoomFactor == ZOOM_FACTOR_CUSTOM)) s_pendingZoom = ZOOM_FACTOR_CUSTOM;
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(langGetText(STR_MENU_SMOOTH_SCROLLING), nullptr, (bool)smoothScrollingEnabled)) windowSmoothScrolling_toggle();

        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_AUTO_SCROLLING), KMOD_PRIMARY_LABEL "A", (bool)autoScrollingEnabled)) windowAutomaticScrolling_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_SHOW_GUNSIGHT),  KMOD_PRIMARY_LABEL "G", (bool)showGunsight))        windowShowGunsight_toggle(cs);

        if (ImGui::BeginMenu(langGetText(STR_MENU_MSG_NAMES_SUB))) {
            if (ImGui::MenuItem(langGetText(STR_SHORT), nullptr, labelMsg == lblShort)) windowSetMessageLabelLen(cs, lblShort);
            if (ImGui::MenuItem(langGetText(STR_LONG),  nullptr, labelMsg == lblLong))  windowSetMessageLabelLen(cs, lblLong);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(langGetText(STR_MENU_TANK_LABELS_SUB))) {
            if (ImGui::MenuItem(langGetText(STR_NONE),               KMOD_PRIMARY_LABEL "1", labelTank == lblNone))  windowSetTankLabelLen(cs, lblNone);
            if (ImGui::MenuItem(langGetText(STR_SHORT),              KMOD_PRIMARY_LABEL "2", labelTank == lblShort)) windowSetTankLabelLen(cs, lblShort);
            if (ImGui::MenuItem(langGetText(STR_LONG),               KMOD_PRIMARY_LABEL "3", labelTank == lblLong))  windowSetTankLabelLen(cs, lblLong);
            if (ImGui::MenuItem(langGetText(STR_MENU_NO_OWN_LABEL),  nullptr, !(bool)labelSelf))       windowLabelOwnTank_toggle(cs);
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(langGetText(STR_MENU_PILLBOX_LABELS), KMOD_PRIMARY_LABEL "P", (bool)showPillLabels)) windowShowPillLabels_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_BASE_LABELS),    KMOD_PRIMARY_LABEL "B", (bool)showBaseLabels)) windowShowBaseLabels_toggle(cs);

        ImGui::EndMenu();
    }

    /* ---- WinBolo ------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_WINBOLO))) {
        if (ImGui::MenuItem(langGetText(STR_ALLOW_NEW_PLAYERS),    nullptr, (bool)allowNewPlayers))           windowMenuAllowNewPlayers_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_SETKEYS),         KMOD_PRIMARY_LABEL "K"))                                 sdl3ImguiShowKeySetup();
        if (ImGui::MenuItem(langGetText(STR_DLGCHANGENAME_TITLE)))                                            s_showChangeName = true;
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_SOUND_EFFECTS),   nullptr, (bool)soundEffects))              windowSoundEffects_toggle();
        if (ImGui::MenuItem(langGetText(STR_MENU_BACKGROUND_SOUND),nullptr, (bool)backgroundSound))           windowBackgroundSoundChange_toggle();
        if (ImGui::MenuItem(langGetText(STR_MENU_SOUND_KEEPALIVE), nullptr, (bool)useSoundKeepalive))         windowSoundKeepalive();
        {
            int vol = soundVolume;
            const float sliderW = 160.0f;
            ImGui::TextUnformatted(langGetText(STR_MENU_VOLUME));
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - sliderW);
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderInt("##volume", &vol, 0, 100, "%d%%")) {
                windowSetSoundVolume(vol);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_NEWSWIRE_MSGS),   nullptr, (bool)showNewswireMessages))      windowMenuNewswire_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_ASSISTANT_MSGS),  nullptr, (bool)showAssistantMessages))     windowMenuAssistant_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_AI_MSGS),         nullptr, (bool)showAIMessages))            windowMenuAI_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_NETSTATUS_MSGS),  nullptr, (bool)showNetworkStatusMessages)) windowMenuNetwork_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_NETDEBUG_MSGS),   nullptr, (bool)showNetworkDebugMessages))  windowMenuNetworkDebug_toggle(cs);
        ImGui::Separator();
        {
            bool rankedGame = clientSimGetLobbyRanked(cs);
            if (ImGui::MenuItem(langGetText(STR_REQUEST_ALLIANCE),     KMOD_PRIMARY_LABEL "R", false, !rankedGame))
                clientSimRequestAllianceSelected(cs);
            if (rankedGame && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", langGetText(STR_ALLIANCE_RANKED_DISABLED));
            }
            if (ImGui::MenuItem(langGetText(STR_LEAVE_ALLIANCE)))                                             clientSimLeaveAllianceSelf(cs);
        }

        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_SETTINGS)))                                                  sdl3ImguiShowSettings();
        ImGui::EndMenu();
    }

    /* ---- Players ------------------------------------- */
    /* Widen the popup so flag + platform/WBN/Steam icons + name + ping +
     * checkmark can all fit on one row without overlap. */
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f),
                                        ImVec2(FLT_MAX, FLT_MAX));
    /* A full 16-slot roster plus the alliance/vote footer can make this
     * dropdown taller than the window; cap it to the work area so ImGui
     * adds a scrollbar instead of clipping the bottom rows off-screen. */
    {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                            ImVec2(FLT_MAX, vp->WorkSize.y));
    }
    if (ImGui::BeginMenu(langGetText(STR_MENU_PLAYERS))) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        if (!uiModeIsTablet()) {
            /* Checked when the pop-out is open; picking it raises and focuses
               that window rather than closing it, matching Ctrl+M. While the
               overview owns the game window it toggles the in-window panel
               instead, for the same reason the File menu's dialogs do. */
            bool overviewInWindow = sdl3DrawIsOverviewInWindow();
            if (ImGui::MenuItem(langGetText(STR_MENU_SEND_MESSAGE), KMOD_PRIMARY_LABEL "M",
                                overviewInWindow ? s_showSendMsg : s_popSendMsg.open)) {
                if (overviewInWindow) {
                    s_showSendMsg = !s_showSendMsg;
                    if (s_showSendMsg) { s_sendMsgFocusInput = true; s_closeMenuPopups = true; }
                } else sendMsgPopOutShow();
            }
        } else {
#endif
            if (ImGui::MenuItem(langGetText(STR_MENU_SEND_MESSAGE), KMOD_PRIMARY_LABEL "M")) {
                s_showSendMsg = !s_showSendMsg;
                if (s_showSendMsg) { s_sendMsgFocusInput = true; s_closeMenuPopups = true; }
            }
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        /* Show-and-raise, never a toggle, matching the native item in
           mac_menubar.mm; checked while the desktop pop-out is up. */
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        if (!uiModeIsTablet()) {
            if (ImGui::MenuItem(langGetText(STR_MENU_PLAYERS_PANEL), KMOD_PRIMARY_LABEL "Shift+P",
                                s_popPlayers.open))
                sdl3ImguiShowPlayersPanel(true);
        } else {
#endif
            if (ImGui::MenuItem(langGetText(STR_MENU_PLAYERS_PANEL), KMOD_PRIMARY_LABEL "Shift+P"))
                sdl3ImguiShowPlayersPanel(true);
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        ImGui::Separator();
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_ALL),    false, ImGuiSelectableFlags_DontClosePopups))   clientSimCheckAllNonePlayers(cs, true);
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_NONE),   false, ImGuiSelectableFlags_DontClosePopups))   clientSimCheckAllNonePlayers(cs, false);
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_ALLIES), false, ImGuiSelectableFlags_DontClosePopups))   clientSimCheckAlliedPlayers(cs);
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_NEARBY), false, ImGuiSelectableFlags_DontClosePopups))   clientSimCheckNearbyPlayers(cs);
        /* Pre-compute alliance state for each player */
        BYTE self = clientSimGetMyPlayerNum(cs);
        bool hasAllies  = false;
        bool canRequest = false;
        bool isAlly[MAX_PLAYERS] = {};
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (s_playerEnabled[i] && i != self) {
                isAlly[i] = clientSimIsPlayerAlly(cs, self, (BYTE)i);
                if (isAlly[i]) {
                    hasAllies = true;
                } else if (s_playerChecked[i]) {
                    canRequest = true;
                }
            }
        }

        ImGui::Separator();
        for (int i = 0; i < MAX_PLAYERS; i++) {
            const char *label = s_playerEnabled[i] ? s_playerName[i] : nullptr;
            char defLabel[16];
            if (!label || label[0] == '\0') {
                snprintf(defLabel, sizeof(defLabel), "%d", i + 1);
                label = defLabel;
            }
            if (s_playerEnabled[i] && i != self) {
                /* Colored alliance indicator: green = ally, red = enemy */
                if (isAlly[i]) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.9f, 0.0f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.0f, 0.0f, 1.0f));
                }
                ImGui::TextUnformatted("*"); /* alliance indicator */
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            /* Render flag icon inline before player name. Skipped for
             * bots — the brain icon emitted by renderPlayerName below
             * takes the platform-icon slot and stands in for both. */
            if (s_playerEnabled[i] && !(s_playerFlags[i] & PLAYER_FLAG_BOT)
                && s_playerCountry[i][0] != '\0') {
                if (drawCountryFlagWithTip(s_playerCountry[i])) {
                    ImGui::SameLine();
                }
            }
            if (s_playerEnabled[i]) {
                /* Row layout: flag, icons, name, ping, check. Icons go inline
                 * right after the flag (before the name); the checkmark sits
                 * at the right edge of the popup, to the right of the ping. */
                ensureWbnIconsLoaded();
                ensurePlatformIconsLoaded();
                uint8_t pflags = s_playerFlags[i];
                uint8_t pct    = s_playerClientType[i];
                renderPlayerName(NULL, pflags, pct, "", false);

                ImGuiContext &g = *GImGui;
                float checkSz = g.FontSize * 0.866f;
                float spacing = ImGui::GetStyle().ItemSpacing.x;

                /* Build ping string */
                char pingStr[16];
                if (s_playerPing[i] > 0) {
                    snprintf(pingStr, sizeof(pingStr), "%dms", (int)s_playerPing[i]);
                } else {
                    snprintf(pingStr, sizeof(pingStr), "---");
                }
                float pingWidth = ImGui::CalcTextSize(pingStr).x;

                /* Anchor everything to the row's right edge (window-local). */
                float rowRightX   = ImGui::GetContentRegionMax().x;
                float checkLocalX = rowRightX - checkSz;
                float pingLocalX  = checkLocalX - spacing - pingWidth;
                float nameWidth   = pingLocalX - ImGui::GetCursorPosX() - spacing;
                if (nameWidth < 1.0f) nameWidth = 1.0f;

                /* Selectable player name (fills the slot between icons and ping). */
                char selectLabel[64];
                snprintf(selectLabel, sizeof(selectLabel), "%s##sel%d", label, i);
                if (ImGui::Selectable(selectLabel, false, ImGuiSelectableFlags_DontClosePopups,
                                      ImVec2(nameWidth, 0))) {
                    clientSimTogglePlayerCheckState(cs, (BYTE)i);
                }
                imguiHandOnHover();

                /* Ping with color coding — anchored just left of the checkmark slot. */
                ImGui::SameLine(pingLocalX);
                ImVec4 pingColor = imguiPingBandColor(
                    cs ? clientSimGetPlayerPingBand(cs, (BYTE)i)
                       : pingBandClassify(s_playerPing[i]));
                ImGui::PushStyleColor(ImGuiCol_Text, pingColor);
                ImGui::TextUnformatted(pingStr);
                ImGui::PopStyleColor();

                /* Checkmark at the far right of the popup, right of the ping. */
                if (s_playerChecked[i]) {
                    float checkScreenX = ImGui::GetWindowPos().x + checkLocalX;
                    ImVec2 pos = ImVec2(checkScreenX,
                                        ImGui::GetItemRectMin().y + g.FontSize * 0.134f * 0.5f);
                    ImGui::RenderCheckMark(ImGui::GetWindowDrawList(), pos,
                                           ImGui::GetColorU32(ImGuiCol_Text), checkSz);
                }
            } else {
                ImGui::MenuItem(label, nullptr, false, false);
            }
        }
        ImGui::Separator();
        {
            bool inCooldown = (s_allianceReqCooldownEnd != 0 &&
                               SDL_GetTicks() < s_allianceReqCooldownEnd);
            if (hasAllies) {
                /* Already in an alliance — show Leave */
                if (ImGui::MenuItem(langGetText(STR_LEAVE_ALLIANCE)))
                    clientSimLeaveAllianceSelf(cs);
            } else {
                /* Not in an alliance — show Request */
                if (!canRequest || inCooldown) ImGui::BeginDisabled();
                if (ImGui::MenuItem(langGetText(STR_REQUEST_ALLIANCE))) {
                    clientSimRequestAllianceSelected(cs);
                    s_allianceReqCooldownEnd = SDL_GetTicks() + ALLIANCE_REQ_WAIT_MS;
                }
                if (!canRequest || inCooldown) ImGui::EndDisabled();
            }
        }
        /* In-game vote menu — only on lobby-enabled servers (the server
         * rejects votes without a lobby to return to). */
        if (clientSimIsLobbyAvailable(cs)) {
            ImGui::Separator();
            bool running = clientSimGetNetStatus(cs) == netRunning;
            int activeTeams = 0;
            bool teamSeen[17] = {0};
            if (running) {
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
                    if (!ls || !ls->connected || ls->isBot) continue;
                    uint8_t t = ls->teamNumber;
                    if (t == 0 || t > 16) continue;
                    if (!teamSeen[t]) { teamSeen[t] = true; activeTeams++; }
                }
            }

            if (ImGui::MenuItem(langGetText(STR_VOTE_BACK_TO_LOBBY), nullptr, false, running)) {
                clientSimNetSendGameVoteToggle(cs, GAME_VOTE_KIND_BACK_TO_LOBBY,
                                               GAME_VOTE_TOGGLE_OPEN_ONLY);
                clientSimSetGameVoteWidgetVisible(cs, GAME_VOTE_KIND_BACK_TO_LOBBY, true);
            }
            if (!running && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", langGetText(STR_VOTE_NEEDS_RUNNING_TIP));
            }

            const ClientLobbySlot *meSlot =
                clientSimGetLobbySlot(cs, clientSimGetMyPlayerNum(cs));
            bool meUnassigned = (meSlot && meSlot->teamNumber == 0);
            bool surrEnabled = running && (activeTeams == 2) && !meUnassigned;
            if (ImGui::MenuItem(langGetText(STR_VOTE_SURRENDER), nullptr, false, surrEnabled)) {
                clientSimNetSendGameVoteToggle(cs, GAME_VOTE_KIND_SURRENDER,
                                               GAME_VOTE_TOGGLE_OPEN_ONLY);
                clientSimSetGameVoteWidgetVisible(cs, GAME_VOTE_KIND_SURRENDER, true);
            }
            if (!surrEnabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (!running) {
                    ImGui::SetTooltip("%s", langGetText(STR_VOTE_NEEDS_RUNNING_TIP));
                } else if (meUnassigned) {
                    ImGui::SetTooltip("%s", langGetText(STR_VOTE_SURRENDER_PICK_TEAM_TIP));
                } else {
                    ImGui::SetTooltip("%s", langGetText(STR_VOTE_SURRENDER_TWO_TEAMS_TIP));
                }
            }

            /* Re-open the floating widget for an in-flight vote whose popup was
             * closed (X'd). One entry per such vote, only when the local player
             * may see it (surrender stays private to the surrendering team). */
            static const uint8_t showKinds[] = {
                GAME_VOTE_KIND_BACK_TO_LOBBY, GAME_VOTE_KIND_SURRENDER
            };
            for (size_t k = 0; k < sizeof(showKinds) / sizeof(showKinds[0]); k++) {
                uint8_t vk = showKinds[k];
                ClientGameVoteSnapshot vs = {};
                if (!clientSimGetGameVote(cs, vk, &vs)) continue;
                if (vs.active != GAME_VOTE_ACTIVE_RUNNING) continue;
                if (vs.widgetVisible) continue;
                if (!localCanAnswerGameVote(cs, &vs)) continue;
                const char *vnm = (vk == GAME_VOTE_KIND_BACK_TO_LOBBY)
                                  ? langGetText(STR_VOTE_BACK_TO_LOBBY)
                                  : langGetText(STR_VOTE_SURRENDER);
                MessageArgs vargs = {};
                SDL_snprintf(vargs.string1, sizeof(vargs.string1), "%s", vnm);
                char lbl[128];
                snprintf(lbl, sizeof(lbl), "%s",
                         langGetTextFmt(STR_VOTE_SHOW, &vargs));
                if (ImGui::MenuItem(lbl))
                    clientSimSetGameVoteWidgetVisible(cs, vk, true);
            }
        }
        ImGui::EndMenu();
    }

    /* ---- Brains -------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_BRAINS), clientSimGetAiType(cs) != aiNone)) {
        bool running = luaBrainIsRunning() != 0;
        int  runIdx  = luaBrainGetRunningIndex();

        /* Manual (stop brain) entry — checked when no brain is active */
        if (ImGui::MenuItem(langGetText(STR_MENU_MANUAL), nullptr, !running)) {
            if (running) {
                luaBrainStop();
                mlBrainStopSingleton();
            }
        }

        /* One entry per discovered brain */
        int numBrains = luaBrainGetNum();
        if (numBrains > 0) {
            ImGui::Separator();
            for (int bi = 0; bi < numBrains; bi++) {
                const char *name = luaBrainGetName(bi);
                bool isActive    = running && (bi == runIdx);
                if (ImGui::MenuItem(name ? name : "?", nullptr, isActive)) {
                    if (!isActive) {
                        const char *path = luaBrainGetPath(bi);
                        if (path) {
                            if (luaBrainGetType(bi) == BRAIN_TYPE_ONNX) {
                                mlBrainStartSingleton(path, name ? name : "", cs);
                            } else {
                                luaBrainStart(path, name ? name : "", cs);
                            }
                            /* Refresh settings descriptor for the new brain */
                            luaBrainFreeSettings(s_brainSettings);
                            s_brainSettings      = nullptr;
                            s_brainSettingsCount = 0;
                            s_brainSettingsOpen  = false;
                        }
                    }
                }
            }
        }

        /* Settings entry — only when a Lua brain is running (ONNX has no settings) */
        if (running && !mlBrainSingletonIsRunning()) {
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_MENU_SETTINGS))) {
                /* Re-fetch on every open so values are current */
                luaBrainFreeSettings(s_brainSettings);
                s_brainSettings      = luaBrainGetSettings(&s_brainSettingsCount);
                s_brainSettingsOpen  = true;
            }
        }

        ImGui::EndMenu();
    }

    /* ---- Help ---------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_HELP))) {
#ifndef __EMSCRIPTEN__
        if (ImGui::MenuItem(langGetText(STR_MENU_HELP)))  { /* TODO: open help file */ }
#endif
        if (ImGui::MenuItem(langGetText(STR_MENU_ABOUT))) aboutPopupOpen();
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

/* -------------------------------------------------------
 * Windows aspect ratio enforcement via window subclassing
 * ------------------------------------------------------- */
#ifdef _WIN32
#include <commctrl.h>  /* SetWindowSubclass */
#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#endif

#define ASPECT_SUBCLASS_ID 1

/* Snap indicator: 0 = not snapped, 1-4 = snapped to that zoom level */
static int s_snapIndicator = 0;
static HWND s_snapPopup = NULL;

static HFONT s_snapFont = NULL;

static void showSnapPopup(HWND parent, const char *text) {
    if (!s_snapFont) {
        s_snapFont = CreateFontA(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, "Segoe UI");
    }
    if (!s_snapPopup) {
        s_snapPopup = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            "STATIC", text,
            WS_POPUP | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 120, 50,
            NULL, NULL, GetModuleHandle(NULL), NULL);  /* No parent - standalone top-level */
    } else {
        SetWindowTextA(s_snapPopup, text);
    }
    SendMessageA(s_snapPopup, WM_SETFONT, (WPARAM)s_snapFont, TRUE);
    /* Position at bottom-right of parent */
    RECT rc;
    GetWindowRect(parent, &rc);
    int popX = rc.right - 130;
    int popY = rc.bottom - 60;
    SetWindowPos(s_snapPopup, HWND_TOPMOST, popX, popY, 120, 50, SWP_SHOWWINDOW);

    /* Force immediate repaint by drawing directly */
    HDC hdc = GetDC(s_snapPopup);
    if (hdc) {
        RECT clientRc = {0, 0, 120, 50};
        HBRUSH bgBrush = CreateSolidBrush(RGB(50, 50, 50));
        FillRect(hdc, &clientRc, bgBrush);
        DeleteObject(bgBrush);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        SelectObject(hdc, s_snapFont);
        DrawTextA(hdc, text, -1, &clientRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ReleaseDC(s_snapPopup, hdc);
    }
}

static void hideSnapPopup(void) {
    if (s_snapPopup) {
        ShowWindow(s_snapPopup, SW_HIDE);
    }
}

/* Subclass procedure to enforce aspect ratio during live resize.
   WM_SIZING provides the drag rect which we modify in place.
   WM_GETMINMAXINFO enforces 1x minimum size.
   Snaps to cardinal sizes (1x-4x) within 2 pixels. */
static LRESULT CALLBACK aspectSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    (void)uIdSubclass; (void)dwRefData;
    if (msg == WM_ENTERSIZEMOVE) {
        s_inModalResize = true;
    }
    if (msg == WM_GETMINMAXINFO) {
        /* Enforce the 1x minimum client size.  The minimum is fixed at 1x (the
           UI scale instead demotes to fit the window), so the window can always
           reach the size where fonts drop to 1.0x. */
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        RECT clientRect = {0, 0,
                           (LONG)SDL3_SCREEN_W,
                           (LONG)(SDL3_SCREEN_H + MENU_BAR_HEIGHT)};
        DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
        DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        AdjustWindowRectEx(&clientRect, style, FALSE, exStyle);
        mmi->ptMinTrackSize.x = clientRect.right - clientRect.left;
        mmi->ptMinTrackSize.y = clientRect.bottom - clientRect.top;
        return 0;
    }
    if (msg == WM_SIZING) {
        RECT *rect = (RECT *)lParam;
        int winW = rect->right - rect->left;
        int winH = rect->bottom - rect->top;

        /* Calculate frame size (title bar + borders) */
        RECT clientRect = {0, 0, 100, 100};
        RECT windowRect = clientRect;
        DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
        DWORD exStyle = (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
        int frameW = (windowRect.right - windowRect.left) - 100;
        int frameH = (windowRect.bottom - windowRect.top) - 100;

        /* Client size from window size */
        int clientW = winW - frameW;
        int clientH = winH - frameH;

        /* Content aspect ratio 515:325, plus MENU_BAR_HEIGHT menu bar inside client area.
           Always keep top-left fixed, expand right and down only.
           Use rounding (not truncation) to avoid 1px black borders. */
        int correctClientW, correctClientH;
        switch (wParam) {
            case WMSZ_TOP:
            case WMSZ_BOTTOM: {
                /* Vertical edge: keep height, adjust width to the right */
                int contentH = clientH - MENU_BAR_HEIGHT;
                correctClientW = (contentH * SDL3_SCREEN_W + SDL3_SCREEN_H / 2) / SDL3_SCREEN_H;
                correctClientH = clientH;
                break;
            }
            default: {
                /* Horizontal edges and corners: keep width, adjust height downward */
                correctClientW = clientW;
                correctClientH = (clientW * SDL3_SCREEN_H + SDL3_SCREEN_W / 2) / SDL3_SCREEN_W + MENU_BAR_HEIGHT;
                break;
            }
        }

        /* Check for snap to cardinal sizes (1x, 2x, 3x, 4x) within 1% */
        s_snapIndicator = 0;
        for (int zoom = 1; zoom <= 4; zoom++) {
            int cardinalW = zoom * SDL3_SCREEN_W;
            int cardinalH = zoom * SDL3_SCREEN_H + MENU_BAR_HEIGHT;
            int threshW = cardinalW / 100;  /* 1% threshold */
            int threshH = cardinalH / 100;
            if (threshW < 2) threshW = 2;   /* minimum 2 pixels */
            if (threshH < 2) threshH = 2;
            int diffW = correctClientW - cardinalW;
            int diffH = correctClientH - cardinalH;
            if (diffW < 0) diffW = -diffW;
            if (diffH < 0) diffH = -diffH;
            if (diffW <= threshW && diffH <= threshH) {
                correctClientW = cardinalW;
                correctClientH = cardinalH;
                s_snapIndicator = zoom;
                break;
            }
        }

        rect->right = rect->left + correctClientW + frameW;
        rect->bottom = rect->top + correctClientH + frameH;

        /* Show snap indicator popup during resize (only when snapped) */
        if (s_snapIndicator > 0) {
            const char *labels[] = {"", langGetText(STR_MENU_NORMAL), langGetText(STR_MENU_DOUBLE), langGetText(STR_MENU_TRIPLE), langGetText(STR_MENU_QUAD)};
            showSnapPopup(hwnd, labels[s_snapIndicator]);
        } else {
            hideSnapPopup();
        }
        return TRUE;
    }
    if (msg == WM_EXITSIZEMOVE) {
        s_inModalResize = false;
        /* If snapped to a cardinal size, switch to that zoom mode */
        if (s_snapIndicator > 0) {
            s_pendingZoom = (BYTE)s_snapIndicator;
            s_pendingZoomFromResize = true;  /* Don't recenter window */
        } else if (zoomFactor != ZOOM_FACTOR_CUSTOM) {
            /* Not snapped — switch to Custom mode now that resize is done */
            s_pendingZoom = ZOOM_FACTOR_CUSTOM;
            s_pendingZoomFromResize = true;
        }
        /* Hide snap indicator and clear state when resize ends */
        hideSnapPopup();
        s_snapIndicator = 0;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, aspectSubclassProc, ASPECT_SUBCLASS_ID);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}
#endif

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */

/* Largest UI scale whose 1x dialog content still fits the current window,
   quantised to a 0.25 ladder.  Dialogs lay out in logical points against
   SDL_GetWindowSize (DPI is handled by the renderer, not here), so the fit is
   measured in those same units: window width over the 515pt content width, and
   the menu-bar-less window height over the 325pt content height.  Because the
   window is aspect-locked and the menu bar is a fixed 22pt (0 on macOS), the
   two ratios agree at every zoom step.  Floor (not round) to the ladder so the
   result never exceeds the true fit and re-overflows; a drag-resize therefore
   crosses only a handful of atlas rebuilds.  This is what lets a Large pref
   auto-demote in a small window and snap back when it grows. */
static float desktopWindowFitScale(SDL_Window *window) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0) return 1.0f;
    float fitW = (float)w / (float)SDL3_SCREEN_W;
    float availH = (float)h - (float)MENU_BAR_HEIGHT;
    float fitH = (availH > 0.0f) ? availH / (float)SDL3_SCREEN_H : fitW;
    float fit = SDL_min(fitW, fitH);
    fit = (float)((int)(fit * 4.0f)) / 4.0f;   /* floor to 0.25 steps */
    if (fit < 1.0f) fit = 1.0f;
    if (fit > 2.0f) fit = 2.0f;   /* cap at the Large preset; never exceed it */
    return fit;
}

/* (Re)apply the main ImGui context's font atlas, style, and window minimum
   for the current UI scale.  Recomputes the scale (desktop: preference capped
   by what the window can hold; Auto follows the same window-height scale the
   front-end dialogs use, so the in-game UI matches the menu/lobby rather than
   ballooning to the game-canvas fit), rebuilds the font atlas, resets and
   re-scales the style.  Called at setup, when the
   UI-scale pref changes, and on window resize — the latter two only from the
   deferred safe point between Present and NewFrame, so the atlas swap can't
   race draw data still queued against the old texture.  Resize fires it every
   frame of a drag, so it early-outs when the quantised scale hasn't moved. */
static void applyMainContextUiScale(void) {
    if (!s_window) return;
    ImGuiIO &io = ImGui::GetIO();

    /* One scale value drives both the font size and the style metrics.
       Tablet uses FontGlobalScale below (so uiScale stays 1); Deck keeps
       its 1.5x; desktop caps the preferred scale (Small/Med/Large, or for
       Auto the front-end dialog scale) by what the window can actually hold,
       so a big font in a small window demotes to fit and restores when it
       grows.  Auto mirrors dialogComputeScale (window height vs 1080) instead
       of the game-canvas fit (window width vs the 515x325 viewport): the
       in-game window is sized as a 2x multiple of that viewport, so the fit
       would resolve to 2.0 and double the menu-bar font relative to the
       identically-windowed menu/lobby.  Sharing the dialog scale keeps them in
       step. */
    float uiScale;
    if (uiModeIsTablet())          uiScale = 1.0f;
    else if (uiModeIsSteamDeck())  uiScale = dialogDeckFontMul();  /* 1.5, unchanged */
    else {
        int winW = 0, winH = 0;
        SDL_GetWindowSize(s_window, &winW, &winH);
        float fit  = desktopWindowFitScale(s_window);
        float pref = (uiUiScaleGet() == UI_SCALE_AUTO)
                       ? dialogComputeScale(winW, winH)       /* Auto: match the dialogs */
                       : uiUiScalePresetFactor(uiUiScaleGet()); /* 1.0 / 1.5 / 2.0 */
        uiScale = SDL_min(pref, fit);
        if (uiScale < 1.0f) uiScale = 1.0f;
    }

    /* Resize events land here every frame of a drag; skip the costly atlas
       rebuild when the quantised scale hasn't actually changed.  The atlas is
       empty at first setup, so that pass always proceeds. */
    if (io.Fonts->Fonts.Size > 0 && uiScale == s_uiScale) return;
    s_uiScale = uiScale;

    /* Rebuild the font atlas at the new size.  Clear() first because
       imguiLoadBoloFont only appends; the SDL3 backend's
       ImGuiBackendFlags_RendererHasTextures contract swaps the GPU texture
       on the next RenderDrawData, so no manual texture teardown is needed.
       At setup the atlas is empty, so Clear() is a harmless no-op. */
    io.Fonts->Clear();
    imguiLoadBoloFont(18.0f * uiScale);

    /* Reset the style to a clean base before re-scaling.  ScaleAllSizes
       compounds, so re-running it on an already-scaled style would
       double-count every metric; assigning a default ImGuiStyle clears any
       prior scaling (and the fields imguiApplyBoloTheme doesn't set). */
    ImGui::GetStyle() = ImGuiStyle();
    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();

    /* Tablet mode: scale up ImGui for touch targets.
       Scale proportionally to the logical coordinate space height.
       Reference: zoom 2 → 480px height → 1.0x pixel scale. */
    if (uiModeIsTablet()) {
        float ps = (float)sdl3DrawGetZoomFactor() / 2.0f;
        if (ps < 1.0f) ps = 1.0f;
        io.FontGlobalScale = 1.8f * ps;
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
        ImGuiStyle &style = ImGui::GetStyle();
        style.FramePadding      = ImVec2(12 * ps, 8 * ps);
        style.ItemSpacing       = ImVec2(12 * ps, 8 * ps);
        style.TouchExtraPadding = ImVec2(8 * ps, 8 * ps);
        style.ScrollbarSize     = 24.0f * ps;
    } else if (uiScale > 1.0f) {
        /* Match the bumped font size with proportionally bumped layout
           metrics (FramePadding, ItemSpacing, ScrollbarSize, etc.) so
           in-game dialogs (Settings / Players / Send Message / pause
           overlay) don't clip text or overlap.  Covers Steam Deck (1.5x)
           and high-DPI / overridden desktop.  No touch padding — both use
           desktop hover/click feel. */
        ImGui::GetStyle().ScaleAllSizes(uiScale);
    }

    /* On the resizable desktop window, keep the OS window from shrinking below
       the 1x content size.  The minimum is fixed at 1x (not scaled by the UI
       scale) so the window can always reach the size where fonts demote to
       1.0x; scaling the minimum by s_uiScale would make the two circular.  The
       Deck/tablet fullscreen paths don't resize, so skip them. */
    if (!uiModeIsTablet() && !uiModeIsSteamDeck()) {
        SDL_SetWindowMinimumSize(s_window,
            SDL3_SCREEN_W, SDL3_SCREEN_H + MENU_BAR_HEIGHT);
    }
}

bool sdl3ImguiSetup(SDL_Window *window, SDL_Renderer *renderer) {
    /* Already initialised — just update the window/renderer pointers */
    if (ImGui::GetCurrentContext()) {
        s_window   = window;
        s_renderer = renderer;
        return true;
    }

    s_window   = window;
    s_renderer = renderer;

#ifdef _WIN32
    /* Subclass the window to enforce aspect ratio during live resize */
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                              SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (hwnd) {
        SetWindowSubclass(hwnd, aspectSubclassProc, ASPECT_SUBCLASS_ID, 0);
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    s_mainImguiCtx = ImGui::GetCurrentContext();
    imguiRegisterPlatformOpenUrl();

#ifdef __APPLE__
    mac_menubar_install(s_window, NULL);
#endif

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename  = nullptr; /* no imgui.ini — avoid filesystem clutter */

    /* Font atlas, style, and desktop window minimum for the current UI
       scale.  Factored out so the in-game UI-scale override can rebuild it
       live without a restart. */
    applyMainContextUiScale();

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) return false;
    if (!ImGui_ImplSDLRenderer3_Init(renderer))               return false;

    flagsCreate(renderer);
    inputGamepadInit();
    inputSourceInit();
    return true;
}

void sdl3ImguiResetFrameState(void) {
    if (!s_window || !s_renderer) return;
    /* Run one full dummy frame to put ImGui in a clean state.
       This is needed after modal dialogs may have pumped SDL
       events and left ImGui mid-frame. */
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::EndFrame();
}

void sdl3ImguiProcessEvents(ClientSim *cs) {
    if (!s_window) return;
#ifdef __APPLE__
    mac_menubar_set_clientsim(cs);
#endif
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        SDL_Event rawEv = ev;
        /* Convert mouse/touch coordinates from window space to the
           renderer's logical presentation coordinate space so ImGui
           coordinates match the overridden DisplaySize. */
        if (s_renderer) {
            SDL_ConvertEventToRenderCoordinates(s_renderer, &ev);
        }
        ImGui_ImplSDL3_ProcessEvent(&ev);

        /* Track which input device the player most recently used so
         * tutorial dialogs can pick keyboard vs gamepad glyphs.  Sits
         * here because every poll iteration runs this exactly once,
         * before any subsystem-specific continue/break, regardless of
         * whether the event is later swallowed by ImGui or a popup. */
        inputSourceUpdate(&ev);

        /* DEBUG: log touch/mouse events in tablet mode — remove after debugging */
        if (uiModeIsTablet()) {
            if (ev.type == SDL_EVENT_FINGER_DOWN || ev.type == SDL_EVENT_FINGER_UP) {
                WB_LOG_TRACE(WB_LOG_CAT_GUI, "TAP-DBG: FINGER %s x=%.2f y=%.2f",
                        ev.type == SDL_EVENT_FINGER_DOWN ? "DOWN" : "UP",
                        ev.tfinger.x, ev.tfinger.y);
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                WB_LOG_TRACE(WB_LOG_CAT_GUI, "TAP-DBG: MOUSE %s btn=%d x=%.1f y=%.1f which=%u winID=%u",
                        ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "DOWN" : "UP",
                        ev.button.button, ev.button.x, ev.button.y,
                        ev.button.which, ev.button.windowID);
            }
        }

        /* Note that focus moved, whichever window it moved to or from, and
           decide what it means for the held keys once the queue has drained
           (below the poll loop).  Only the net change over the whole poll
           matters, so the order SDL delivers a hand-off's LOST/GAINED pair
           in — and whether the window flags have settled mid-queue — cannot
           get it wrong.  Sits above the pop-out routing because that block
           consumes a pop-out's own focus events; this is the only place the
           overview gaining or losing focus can be seen.  Consumes nothing:
           every handler further down still runs exactly as before. */
        if (ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
            ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            s_focusEventThisPoll = true;
        }

        /* Meta/Cmd release — re-sync held keys. macOS does not deliver KEY_UP
           for a non-modifier key that is released while Cmd is held (browsers
           on macOS inherit this), so tapping Cmd mid-turn and letting go of a
           movement key under it leaves that key reading as held in
           SDL_GetKeyboardState, with no focus transition to clear it — the
           tank turns forever with nothing pressed. Take the modifier's own
           release as the cue that any key-ups issued under it were swallowed.
           A player still physically holding a key re-presses it; that beats an
           unbounded spin.  Ahead of the pop-out routing because that block
           swallows the Map Overview's key events, and the overview is a window
           the player drives from — a Cmd release there has to reach this. */
        if (ev.type == SDL_EVENT_KEY_UP &&
            (ev.key.scancode == SDL_SCANCODE_LGUI ||
             ev.key.scancode == SDL_SCANCODE_RGUI)) {
            inputResetHeldKeys();
        }

        /* Route events to pop-out windows — if the event belongs to a
           pop-out, forward it there and skip the rest of the main loop
           so it doesn't reach the game input. */
        {
            bool consumedByPopOut = false;
            for (int i = 0; i < POPOUT_COUNT; i++) {
                PopOutWindow *pw = s_popOuts[i];
                if (!pw->open || !pw->window) continue;

                SDL_WindowID pwID = SDL_GetWindowID(pw->window);
                bool isForThisWindow = false;
                switch (ev.type) {
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    case SDL_EVENT_WINDOW_FOCUS_LOST:
                    case SDL_EVENT_WINDOW_RESIZED:
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        isForThisWindow = (ev.window.windowID == pwID);
                        break;
                    case SDL_EVENT_KEY_DOWN:
                    case SDL_EVENT_KEY_UP:
                        isForThisWindow = (ev.key.windowID == pwID);
                        break;
                    case SDL_EVENT_TEXT_INPUT:
                        isForThisWindow = (ev.text.windowID == pwID);
                        break;
                    case SDL_EVENT_TEXT_EDITING:
                        isForThisWindow = (ev.edit.windowID == pwID);
                        break;
                    case SDL_EVENT_MOUSE_MOTION:
                    case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    case SDL_EVENT_MOUSE_BUTTON_UP:
                    case SDL_EVENT_MOUSE_WHEEL:
                        isForThisWindow = (ev.motion.windowID == pwID);
                        break;
                    default:
                        break;
                }

                if (isForThisWindow) {
                    ImGuiContext *savedCtx = ImGui::GetCurrentContext();
                    ImGui::SetCurrentContext(pw->imguiCtx);
                    ImGui_ImplSDL3_ProcessEvent(&ev);
                    ImGui::SetCurrentContext(savedCtx);
                    consumedByPopOut = true;

                    /* Track the current size of a resizable pop-out. The main
                       window's resize handler further down only ever looks at
                       s_window, so this is the only place a pop-out learns it
                       changed size. RESIZED carries the logical size, the same
                       units popOutCreate was given; PIXEL_SIZE_CHANGED carries
                       backing-store pixels and would disagree on a HiDPI
                       display, so only RESIZED is recorded. */
                    if (ev.type == SDL_EVENT_WINDOW_RESIZED &&
                        ev.window.data1 > 0 && ev.window.data2 > 0) {
                        pw->width  = ev.window.data1;
                        pw->height = ev.window.data2;
                        /* The size the window manager gave a full screen or
                           zoomed pop-out is not the size the player chose,
                           and writing it down brings the window back filling
                           the display with its title bar under the menu bar.
                           The main window guards its own geometry the same
                           way in the resize handler further down. The live
                           surface size above still tracks either way: the
                           window really is that big now. */
                        if (pw == &s_popMapOverview &&
                            !popOutGeometryIsOsManaged(pw)) {
                            gameFrontOverviewW = pw->width;
                            gameFrontOverviewH = pw->height;
                            gameFrontSaveWindowSettings();
                        }
                    }
                }

                /* Handled out here rather than in the switch above because
                   the switch decides which events a pop-out consumes, and a
                   move is not one of them: adding it would have all five
                   pop-outs swallow SDL_EVENT_WINDOW_MOVED and skip
                   sdl3DrawHandleEvent, which needs to see the main window
                   move. Only the overview remembers where it was put. */
                if (ev.type == SDL_EVENT_WINDOW_MOVED &&
                    ev.window.windowID == pwID && pw == &s_popMapOverview &&
                    !popOutGeometryIsOsManaged(pw)) {
                    gameFrontOverviewX = ev.window.data1;
                    gameFrontOverviewY = ev.window.data2;
                    gameFrontSaveWindowSettings();
                }

                /* Out here with the move above, and for the same reason: the
                   main window's own full screen tracking further down has to
                   see these too. Coming back out is where the player's rect
                   is re-read from the window. The resize and move that arrive
                   during a full screen transition can land before the window
                   flags admit to it, so a display-sized rect can still slip
                   past the guard above; taking the real windowed geometry
                   here puts it right. */
                if (ev.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN &&
                    ev.window.windowID == pwID && pw == &s_popMapOverview) {
                    int px = 0, py = 0, pww = 0, pwh = 0;
                    SDL_GetWindowSize(pw->window, &pww, &pwh);
                    SDL_GetWindowPosition(pw->window, &px, &py);
                    if (pww > 0 && pwh > 0) {
                        pw->width  = pww;
                        pw->height = pwh;
                        gameFrontOverviewW = pww;
                        gameFrontOverviewH = pwh;
                        gameFrontOverviewX = px;
                        gameFrontOverviewY = py;
                        gameFrontSaveWindowSettings();
                    }
                }

                if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == pwID) {
                    if (pw == &s_popMapOverview) mapOverviewClose();
                    else popOutHide(pw);
                    consumedByPopOut = true;
                }

                /* Escape closes a focused pop-out, matching the Escape ladder
                   the tablet panels use. Without it the close box is the only
                   way out, and on macOS that leaves no keyboard path at all —
                   there is no Cmd+W item in the native menu. The event was
                   already forwarded above, so ImGui has deactivated any live
                   InputText before the window goes away. */
                if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
                    ev.key.windowID == pwID &&
                    ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (pw == &s_popMapOverview) mapOverviewClose();
                    else popOutHide(pw);
                    consumedByPopOut = true;
                }
            }
            if (consumedByPopOut) continue;
        }

        /* Route finger events to touch input system in tablet mode.
           Use the raw (unconverted) event since inputTouchProcessEvent
           expects normalized tfinger coords multiplied by the target size.
           Use logical presentation size so coordinates match the game
           viewport bounds (same approach as winbolo_ios.m).
           When a dialog/panel is open, only pass finger-up events so that
           active joystick/button state gets properly released, but don't
           start new joystick/button interactions behind the overlay. */
        if (uiModeIsTablet() &&
            (rawEv.type == SDL_EVENT_FINGER_DOWN ||
             rawEv.type == SDL_EVENT_FINGER_UP ||
             rawEv.type == SDL_EVENT_FINGER_MOTION)) {
            bool dialogOpen = sdl3ImguiIsDialogOpen();
            if (!dialogOpen || rawEv.type == SDL_EVENT_FINGER_UP) {
                int tw = 0, th = 0;
                SDL_RendererLogicalPresentation logMode;
                SDL_GetRenderLogicalPresentation(s_renderer, &tw, &th, &logMode);
                if (tw <= 0 || th <= 0) SDL_GetWindowSize(s_window, &tw, &th);
                inputTouchProcessEvent(&rawEv, tw, th);
            }
        }

        /* Window focus — mute sound when backgroundSound is off.
           Don't mute if focus moved to one of our own windows (main or pop-out). */
        if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            /* Retract any open menu-bar dropdown so it isn't left hanging open
               when the user tabs away to another window. */
            s_closeMenuPopups = true;
            if (soundEffects && !backgroundSound) {
                SDL_Window *focused = SDL_GetKeyboardFocus();
                bool focusedIsOurs = (focused == s_window);
                if (!focusedIsOurs) {
                    for (int i = 0; i < POPOUT_COUNT; i++) {
                        if (s_popOuts[i]->window && s_popOuts[i]->window == focused) { focusedIsOurs = true; break; }
                    }
                }
                if (!focusedIsOurs) soundSetMuted(true);
            }
            continue;
        }
        if (ev.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
            if (soundEffects) {
                soundSetMuted(false);
            }
            continue;
        }

        /* Suspend / resume — Steam Deck Verified requirement.  Fires on
           sleep, home-button overlay, and other backgrounding.  In a
           network game, resume drops back to menu via the standard
           "you have been disconnected" flow.  In single-player it just
           resets the catchup-loop wallclock baseline. */
        if (ev.type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
            windowSuspendBackground(cs);
            continue;
        }
        if (ev.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            windowResumeForeground(cs);
            continue;
        }

        /* Cmd+key shortcuts (non-macOS — macOS routes these through NSMenu in mac_menubar.mm) */
#ifndef __APPLE__
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
            ev.key.windowID == SDL_GetWindowID(s_window) &&
            (ev.key.mod & KMOD_PRIMARY) != 0) {
            switch (ev.key.scancode) {
            case SDL_SCANCODE_M:
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
                if (!uiModeIsTablet()) {
                    /* Via sdl3ImguiSendMsgShortcut rather than straight to
                       sendMsgPopOutShow: it picks the pop-out, the in-window
                       panel or the controller modal, and decides raise vs
                       toggle for each. The pop-out is a separate OS window
                       that receives no controller input, so on a Deck — where
                       the virtual pad reports as keyboard and can reach this
                       shortcut — opening it directly would leave a pad user
                       with a window they cannot close. */
                    sdl3ImguiSendMsgShortcut();
                } else {
#endif
                    /* Never a toggle — an already-open panel is raised to the
                       front of the ImGui stack and refocused (SetWindowFocus
                       in renderSendMsgContent) instead of being hidden. */
                    s_showSendMsg       = true;
                    s_sendMsgFocusInput = true;
                    s_closeMenuPopups   = true;
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
                }
#endif
                continue;
            case SDL_SCANCODE_K:
                sdl3ImguiShowKeySetup();
                continue;
            case SDL_SCANCODE_S:
                windowSaveMap(cs);
                continue;
            case SDL_SCANCODE_G:
                windowShowGunsight_toggle(cs);
                continue;
            case SDL_SCANCODE_A:
                windowAutomaticScrolling_toggle(cs);
                continue;
            case SDL_SCANCODE_1:
                windowSetTankLabelLen(cs, lblNone);
                continue;
            case SDL_SCANCODE_2:
                windowSetTankLabelLen(cs, lblShort);
                continue;
            case SDL_SCANCODE_3:
                windowSetTankLabelLen(cs, lblLong);
                continue;
            case SDL_SCANCODE_P:
                /* This switch gates on KMOD_PRIMARY only, so the shift-modified
                   form has to be separated here rather than by its own case. */
                if (ev.key.mod & SDL_KMOD_SHIFT) sdl3ImguiShowPlayersPanel(true);
                else                             windowShowPillLabels_toggle(cs);
                continue;
            case SDL_SCANCODE_B:
                windowShowBaseLabels_toggle(cs);
                continue;
            case SDL_SCANCODE_R:
                clientSimRequestAllianceSelected(cs);
                continue;
            case SDL_SCANCODE_O:
                /* Same running-game condition as the File menu item. */
                if (cs != nullptr && clientSimIsRunning(cs)) sdl3ImguiShowMapOverview(true);
                continue;
            default:
                break;
            }
        }
#endif

        /* Key capture for the Key Setup modal — intercept before the
         * game sees it. State + the actual binding write live in
         * imgui_keysetup.cpp now; we just feed it the scancode. */
        if (imguiKeySetupIsCapturingInGameKey() &&
            ev.type == SDL_EVENT_KEY_DOWN &&
            ev.key.windowID == SDL_GetWindowID(s_window)) {
            imguiKeySetupHandleInGameScancode((int)ev.key.scancode);
            /* Do NOT forward to the game — key was consumed by the dialog. */
            continue;
        }

        /* Alt+Enter is the full screen key, the way it is everywhere else.
         * Which of the two toggles that is lives in sdl3ImguiToggleFullScreen,
         * shared with the macOS Window menu item. Read here rather than
         * through the bindings because it is a window command, not a game
         * action: it is not in keyItems, so no binding can shadow it and it
         * works while an ImGui panel has the keyboard. After the Key Setup
         * capture above, so a player binding a key to Alt or Enter still gets
         * the keystroke. */
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
            ev.key.windowID == SDL_GetWindowID(s_window) &&
            (ev.key.mod & SDL_KMOD_ALT) != 0 &&
            (ev.key.scancode == SDL_SCANCODE_RETURN ||
             ev.key.scancode == SDL_SCANCODE_KP_ENTER)) {
            sdl3ImguiToggleFullScreen(cs);
            continue;
        }

        /* Controller-tab binding capture for the in-game Key Setup popup.
         * While a controller row is armed, route a gamepad button-down or a
         * trigger crossing its threshold into the dialog; Escape cancels.
         * State lives in imgui_keysetup.cpp (mirrors the scancode path). */
        if (imguiKeySetupIsCapturingInGamePad()) {
            if (ev.type == SDL_EVENT_KEY_DOWN &&
                ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                imguiKeySetupCancelInGamePad();
                continue;
            }
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                imguiKeySetupHandleInGamePadButton((int)ev.gbutton.button);
                continue;
            }
            if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                 ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
                ev.gaxis.value > 16384 /* ~0.5 of 32767 */) {
                imguiKeySetupHandleInGamePadTrigger((int)ev.gaxis.axis);
                continue;
            }
        }

        /* Esc on the keyboard also cancels gamepad capture mode. */
        if (s_keySetupGamepadWaitingAction != (int)GP_ACT_COUNT &&
            ev.type == SDL_EVENT_KEY_DOWN &&
            ev.key.windowID == SDL_GetWindowID(s_window) &&
            ev.key.scancode == SDL_SCANCODE_ESCAPE) {
            s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
            continue;
        }

        /* Forward key events to input system for event-driven mine key tracking.
         * Must happen before the ImGui swallow so key-up events are never lost. */
        if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
            keyItems ki;
            windowGetKeys(&ki);
            inputButtonInput(&ki, ev.key.scancode, (ev.type == SDL_EVENT_KEY_DOWN));
        }

        /* Gamepad capture for the Key Setup modal — intercept button-down
         * events and trigger axis cross-edges before the game / input
         * module sees them.  Stick axes are silently ignored: capture
         * stays open until a button or trigger arrives.  Esc cancels
         * the capture (handled in the keyboard intercept above). */
        if (s_keySetupGamepadWaitingAction != (int)GP_ACT_COUNT) {
            int           idx     = s_keySetupGamepadWaitingAction;
            GamepadBinding *target = (s_keySetupGamepadWaitingSlot == GP_SLOT_SECONDARY)
                                       ? &s_keySetupGamepadBindings.b[idx].sec
                                       : &s_keySetupGamepadBindings.b[idx].pri;
            if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                target->kind = GP_BIND_BUTTON;
                target->code = (int)ev.gbutton.button;
                s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
                continue;
            }
            if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                SDL_GamepadAxis axis = (SDL_GamepadAxis)ev.gaxis.axis;
                if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                    axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                    int slot = (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) ? 0 : 1;
                    float v = (float)ev.gaxis.value * (1.0f / 32767.0f);
                    if (v <= 0.5f) {
                        /* Trigger released — arm so the next press counts. */
                        s_keySetupTrigArmed[slot] = true;
                    } else if (s_keySetupTrigArmed[slot]) {
                        target->kind = GP_BIND_TRIGGER;
                        target->code = (int)axis;
                        s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
                    }
                    continue;
                }
                /* Stick axis: ignore (do not consume — let game/UI keep its state). */
            }
        }

        /* Route gamepad connect/disconnect, button events, and trigger
         * axis events to the gamepad input module.  Stick axes are
         * still polled via SDL_GetGamepadAxis each frame; only triggers
         * need event delivery for the rebindable edge-action path. */
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED        ||
            ev.type == SDL_EVENT_GAMEPAD_REMOVED      ||
            ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN  ||
            ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP    ||
            ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            inputGamepadProcessEvent(&ev);
        }

        /* Swallow events ImGui is using so they never reach the game, but
         * gate each device on its own capture flag: keyboard events only when
         * ImGui wants the keyboard, mouse events only when it wants the mouse.
         * Cross-gating these (dropping keyboard whenever the mouse was over a
         * panel) ate event-driven game keys — notably the Tank View key that
         * exits pill view — whenever the cursor merely hovered the menu bar or
         * a vote/alliance overlay. */
        ImGuiIO &io = ImGui::GetIO();
        {
            bool isMouseEvent = (ev.type == SDL_EVENT_MOUSE_MOTION       ||
                                 ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN  ||
                                 ev.type == SDL_EVENT_MOUSE_BUTTON_UP    ||
                                 ev.type == SDL_EVENT_MOUSE_WHEEL);
            bool isKeyEvent   = (ev.type == SDL_EVENT_KEY_DOWN           ||
                                 ev.type == SDL_EVENT_KEY_UP);
            if ((isMouseEvent && io.WantCaptureMouse) ||
                (isKeyEvent && io.WantCaptureKeyboard)) {
                continue;
            }
        }

        /* Mouse wheel adjusts gunsight range while in-game. Reaches here
         * only when ImGui isn't capturing the mouse (the swallow block
         * above continues out for wheel events over UI panels). */
        if (ev.type == SDL_EVENT_MOUSE_WHEEL &&
            cs && clientSimGetNetStatus(cs) == netRunning) {
            if (ev.wheel.y > 0.0f) {
                inputBumpGunsight(+1);
            } else if (ev.wheel.y < 0.0f) {
                inputBumpGunsight(-1);
            }
        }

        /* Handle winbolo:// URL opened while app is already running.
           macOS delivers URL scheme activations as SDL_EVENT_DROP_FILE. */
        if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
            const char *url = ev.drop.data;
            if (strncmp(url, "winbolo://", 10) == 0) {
                WB_LOG_INFO(WB_LOG_CAT_GUI, "[URL] Received winbolo:// link while running: %s", url);
                if (cs && clientSimGetNetStatus(cs) == netRunning) {
                    /* In-game: show confirmation popup instead of switching immediately */
                    strncpy(s_joinConfirmUrl, url, sizeof(s_joinConfirmUrl) - 1);
                    s_joinConfirmUrl[sizeof(s_joinConfirmUrl) - 1] = '\0';
                    /* Parse address and port for display */
                    const char *hostStart = url + 10; /* skip "winbolo://" */
                    const char *colon = strchr(hostStart, ':');
                    if (colon) {
                        size_t len = (size_t)(colon - hostStart);
                        if (len >= sizeof(s_joinConfirmAddr)) len = sizeof(s_joinConfirmAddr) - 1;
                        memcpy(s_joinConfirmAddr, hostStart, len);
                        s_joinConfirmAddr[len] = '\0';
                        s_joinConfirmPort = atoi(colon + 1);
                    } else {
                        strncpy(s_joinConfirmAddr, hostStart, sizeof(s_joinConfirmAddr) - 1);
                        s_joinConfirmAddr[sizeof(s_joinConfirmAddr) - 1] = '\0';
                        s_joinConfirmPort = 0;
                    }
                    s_showJoinConfirm = true;
                } else {
                    /* Not in-game: handle directly */
                    char urlCopy[512];
                    strncpy(urlCopy, url, sizeof(urlCopy) - 1);
                    urlCopy[sizeof(urlCopy) - 1] = '\0';
                    gameFrontHandleUrlOpen(urlCopy);
                }
            }
            continue;
        }

        if (ev.type == SDL_EVENT_QUIT) {
            windowSetQuitting();
        }
        /* Main window close button — SDL3 won't send SDL_EVENT_QUIT
           while pop-out windows are still open. */
        if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            ev.window.windowID == SDL_GetWindowID(s_window)) {
            windowSetQuitting();
        }
        /* The main window can enter or leave full screen without the app
           asking — the green button, Mission Control, a swipe. Track the flag
           from what the window actually did, or it goes on claiming windowed
           while the window is not and the next toggle computes the wrong
           target. Leaving while the full screen map is up ends the mode as
           well: the surface it fills has gone. That goes through
           overviewInWindowChoose, not Set — the player reached for the green
           button themselves, so it is their choice exactly as the menu item
           would have been, and it hands the pop-out back and is remembered
           the same way. The mode test keeps this from re-entering when the
           app asked for the transition itself: by then the mode is already
           off. Skipped on the tablet and the Deck, which are born full screen
           — there the flag is not the player's preference to overwrite. */
        if ((ev.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
             ev.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) &&
            ev.window.windowID == SDL_GetWindowID(s_window) &&
            !uiModeIsTablet() && !uiModeIsSteamDeck()) {
            bool nowFull = (ev.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
            if (!nowFull && sdl3DrawIsOverviewInWindow()) {
                overviewInWindowChoose(false);
            } else if (nowFull != (bool)gameFrontFullScreen) {
                gameFrontFullScreen = nowFull;
                gameFrontSaveCurrentPrefs();
            }
        }

        /* Window resized — enforce content aspect ratio (515:325) accounting for menu bar */
        if (ev.type == SDL_EVENT_WINDOW_RESIZED &&
            ev.window.windowID == SDL_GetWindowID(s_window)) {
            /* The window-fit cap on the UI scale may have changed.  Defer the
               actual recompute/atlas rebuild to the safe point before NewFrame;
               applyMainContextUiScale early-outs if the quantised scale held. */
            if (!uiModeIsTablet() && !uiModeIsSteamDeck())
                s_pendingUiScaleRebuild = true;
            /* A fullscreen size is not the player's window size: the
               remembered custom size and position, and the zoom mode
               derived from the width, all have to stay whatever they were
               when the window was last windowed.  Re-deriving the zoom mode
               from a fullscreen surface is worse than a bad saved value —
               it switches to Custom, which calls SDL_SetWindowSize and
               resizes the window out from under the fullscreen map.  The UI
               scale rebuild above still applies: the surface really did
               change size. */
            if (!(SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN)) {
                if (s_suppressAutoCustom) {
                    /* Programmatic resize from windowZoomChange — don't auto-switch or adjust.
                       Don't clear the flag here - it gets cleared at end of frame after zoom is applied. */
                } else {
                    /* Enforce aspect ratio: adjust height to match width — but not
                       while maximized or fullscreen, where the window must keep the
                       size the OS gave it and the draw side letterboxes the game
                       inside.  Forcing a taller-than-screen height there pushes the
                       title bar off-screen and strands the window with no way to
                       move or restore it. */
                    SDL_WindowFlags wflags = SDL_GetWindowFlags(s_window);
                    bool osManaged =
                        (wflags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0;
                    int w = ev.window.data1;
                    int h = ev.window.data2;
                    int correctContentH = w * SDL3_SCREEN_H / SDL3_SCREEN_W;
                    int correctH = correctContentH + MENU_BAR_HEIGHT;
                    if (!osManaged && h != correctH) {
                        s_suppressAutoCustom = true;  /* Prevent recursion */
                        SDL_SetWindowSize(s_window, w, correctH);
                    }
                    /* Auto-switch zoom mode based on width — but NOT during modal
                       resize (WM_SIZING loop), we handle that in WM_EXITSIZEMOVE.
                       If width matches a cardinal size, switch to that cardinal mode.
                       Otherwise switch to custom. */
                    if (s_pendingZoom == 255 && !s_inModalResize) {
                        BYTE targetZoom = ZOOM_FACTOR_CUSTOM;
                        if (w == 1 * SDL3_SCREEN_W) targetZoom = ZOOM_FACTOR_NORMAL;
                        else if (w == 2 * SDL3_SCREEN_W) targetZoom = ZOOM_FACTOR_DOUBLE;
                        else if (w == 3 * SDL3_SCREEN_W) targetZoom = ZOOM_FACTOR_TRIPLE;
                        else if (w == 4 * SDL3_SCREEN_W) targetZoom = ZOOM_FACTOR_QUAD;
                        if (zoomFactor != targetZoom) {
                            s_pendingZoom = targetZoom;
                        }
                    }
                    /* Save custom size on USER-initiated resize (not programmatic menu changes).
                       Only save if it's actually a non-cardinal size.
                       Save the CORRECTED size (proper aspect ratio), not actual window size,
                       so maximize (which allows any ratio with gray bars) doesn't save a bad size.
                       Find the largest aspect-correct size that FITS WITHIN the actual window. */
                    if (s_pendingZoom == ZOOM_FACTOR_CUSTOM ||
                        (zoomFactor == ZOOM_FACTOR_CUSTOM && s_pendingZoom == 255)) {
                        int curW, curH, curX, curY;
                        SDL_GetWindowSize(s_window, &curW, &curH);
                        SDL_GetWindowPosition(s_window, &curX, &curY);
                        /* Don't save cardinal sizes as "custom" */
                        bool isCardinal = (curW == 1 * SDL3_SCREEN_W || curW == 2 * SDL3_SCREEN_W ||
                                           curW == 3 * SDL3_SCREEN_W || curW == 4 * SDL3_SCREEN_W);
                        if (!isCardinal) {
                            int saveW, saveH, saveX, saveY;
                            windowComputeAspectCorrectSize(curW, curH, curX, curY, &saveW, &saveH, &saveX, &saveY);
                            windowSetCustomSize(saveW, saveH);
                            windowSetSavedPosition(saveX, saveY);
                        }
                    }
                }
                /* Save position on resize too (window may have been repositioned) - but only if
                   we didn't already save a corrected position above */
                if (s_pendingZoom != ZOOM_FACTOR_CUSTOM &&
                    !(zoomFactor == ZOOM_FACTOR_CUSTOM && s_pendingZoom == 255)) {
                    windowSaveCurrentPosition();
                }
                gameFrontSaveWindowSettings();
            }
        }
        /* Window moved — save position */
        if (ev.type == SDL_EVENT_WINDOW_MOVED &&
            ev.window.windowID == SDL_GetWindowID(s_window)) {
            /* A fullscreen window's position is the display's, not the
               player's, so the remembered position has to survive going
               fullscreen and coming back. */
            if (!(SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN)) {
                windowSaveCurrentPosition();
                gameFrontSaveWindowSettings();
            }
        }

        /* Dispatch tap-style key actions (pill view, tank view) that are
         * not handled by the polling-based inputGetKeys(). */
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
            ev.key.windowID == SDL_GetWindowID(s_window)) {
            windowKeyPressed(cs, (int)ev.key.scancode);
        }

        /* Pass RAW event to game handler - it does its own coordinate transform
           using SDL_GetRenderLogicalPresentationRect for resizable window support */
        sdl3DrawHandleEvent(cs, &rawEv);
    }

    /* Held-key / latched edge state is dropped when the windows the player
     * drives from gain or lose focus as a set, and kept when focus merely
     * moves between them.  The game polls SDL_GetKeyboardState, so a movement
     * key released while another application had focus (or while typing in
     * the Send Message pop-out, which steals focus without SDL clearing the
     * keyboard) would otherwise read as still held and spin the tank once
     * focus came back; clearing on the way back in as well means a key still
     * physically down from before never drives the tank until re-pressed.
     * Judged here, after the queue has drained, so a hand-off's LOST and
     * GAINED cancel out instead of firing a reset in between. */
    if (s_focusEventThisPoll) {
        s_focusEventThisPoll = false;
        bool hasFocus = sdl3ImguiGameInputWindowHasFocus();
        if (hasFocus != s_gameInputHadFocus) {
            inputResetHeldKeys();
        }
        s_gameInputHadFocus = hasFocus;
    }

    /* Consume the window-settings dirty flag: the throttle in
     * gameFrontSaveWindowSettings drops moves/resizes that arrive inside
     * its 500ms window. Pumping each frame guarantees the trailing
     * event in a drag burst eventually flushes once idle. */
    gameFrontPumpDirty();
}

void sdl3ImguiForwardEvent(const void *event) {
    if (event) {
        ImGui_ImplSDL3_ProcessEvent((const SDL_Event *)event);
    }
}

bool sdl3ImguiWantCaptureMouse(void) {
    return ImGui::GetIO().WantCaptureMouse;
}

bool sdl3ImguiIsDialogOpen(void) {
    ImGuiContext *g = ImGui::GetCurrentContext();
    return s_showSysInfo || s_showNetInfo || s_showGameInfo ||
           s_showSendMsg || s_showPlayersPanel || s_showSettings ||
           s_brainSettingsOpen || s_allianceVisible ||
           (g && g->OpenPopupStack.Size > 0);
}

/* True whenever any in-game popup modal is on screen.  When one of
   these is up the player is navigating UI, not driving the tank —
   so Steam Input must run Menu set even though clientSimIsInLobby(cs) is
   false.  Add new popups here as they're introduced. */
static bool any_popup_modal_open(void) {
    /* Any open ImGui popup modal (e.g. the in-game Key Setup popup, whose
       open-state isn't exposed separately) means the player is navigating UI,
       so run the Menu action set — this is what makes the left-stick menu nav
       work inside those popups, not just the D-pad. */
    ImGuiContext *g = ImGui::GetCurrentContext();
    if (g && g->OpenPopupStack.Size > 0)
        return true;
    return deckPauseIsOpen() ||
           tutorialOverlayIsOpen() ||
           quickChatIsOpen() ||
           s_showSendMsg ||
           s_showPlayersPanel ||
           s_showSettings;
}

/* Steam Input action-set follower.  Menu set wins on no-cs / lobby /
   any-popup-open; InGame set otherwise.  Helper module owns the
   idempotent activation. */
static void update_steam_input_action_set(ClientSim *cs) {
    bool wantMenu = !cs || clientSimIsInLobby(cs) || any_popup_modal_open();
    if (wantMenu) imguiSteamNavActivateMenuSet();
    else          imguiSteamNavActivateGameSet();
}

#ifdef __APPLE__
/* Build a fresh MacMenuState from current globals + display geometry.
 * Fit1x..fit4x mirror the in-window Window Size enable-gating arithmetic
 * at line ~2038 above; the device-label format mirrors the in-window
 * snprintf at line ~2099. */
static void populateMacMenuState(MacMenuState *s, ClientSim *cs) {
    s->frameRate       = frameRate;
    s->zoomFactor      = (int)zoomFactor;
    s->smoothScrolling = smoothScrollingEnabled;
    s->autoScrolling   = autoScrollingEnabled;
    s->showGunsight    = showGunsight;
    s->showPillLabels  = showPillLabels;
    s->showBaseLabels  = showBaseLabels;
    s->noOwnLabel      = !labelSelf;
    s->labelMsg        = (int)labelMsg;
    s->labelTank       = (int)labelTank;

    s->allowNewPlayers       = allowNewPlayers;
    s->soundEffects          = soundEffects;
    s->backgroundSound       = backgroundSound;
    s->useSoundKeepalive     = useSoundKeepalive;
    s->soundVolume           = soundVolume;
    s->newswireMessages      = showNewswireMessages;
    s->assistantMessages     = showAssistantMessages;
    s->aiMessages            = showAIMessages;
    s->networkStatusMessages = showNetworkStatusMessages;
    s->networkDebugMessages  = showNetworkDebugMessages;

    s->sysInfoOpen     = sdl3ImguiIsSysInfoOpen();
    s->netInfoOpen     = sdl3ImguiIsNetInfoOpen();
    s->gameInfoOpen    = sdl3ImguiIsGameInfoOpen();
    s->sendMsgOpen     = sdl3ImguiIsSendMsgOpen();
    s->mapOverviewOpen    = sdl3ImguiIsMapOverviewOpen();
    s->mapOverviewEnabled = (cs != nullptr && clientSimIsRunning(cs) &&
                             !gameFrontFullScreen && !classicModeActive());
    s->overviewInWindow        = sdl3ImguiIsOverviewInWindowOpen();
    s->overviewInWindowEnabled = (cs != nullptr && clientSimIsRunning(cs) &&
                                  !classicModeActive());
    s->fullScreenOn            = gameFrontFullScreen;

    int dispW = 99999, dispH = 99999;
    if (s_window) {
        SDL_DisplayID dispID = SDL_GetDisplayForWindow(s_window);
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(dispID, &usable)) {
            dispW = usable.w;
            dispH = usable.h;
        }
    }
    s->fit1x = (1 * SDL3_SCREEN_W <= dispW) && (1 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);
    s->fit2x = (2 * SDL3_SCREEN_W <= dispW) && (2 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);
    s->fit3x = (3 * SDL3_SCREEN_W <= dispW) && (3 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);
    s->fit4x = (4 * SDL3_SCREEN_W <= dispW) && (4 * SDL3_SCREEN_H + MENU_BAR_HEIGHT <= dispH);

    /* Alliance gating — mirrors the in-window Players menu pre-compute
     * at line ~2157. NULL cs leaves both predicates false, so the native
     * Request/Leave Alliance items render disabled during bring-up. */
    bool hasAllies = false, canRequest = false;
    if (cs) {
        BYTE self = clientSimGetMyPlayerNum(cs);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (s_playerEnabled[i] && i != self) {
                bool ally = clientSimIsPlayerAlly(cs, self, (BYTE)i);
                if (ally) hasAllies = true;
                else if (s_playerChecked[i]) canRequest = true;
            }
        }
    }
    s->hasAllies  = hasAllies;
    s->canRequest = canRequest;
    s->inCooldown = sdl3ImguiAllianceReqInCooldown();

    /* Vote gating — same pre-compute as the in-window Players menu vote
     * block (count active human teams, check our own team assignment).
     * NULL cs leaves both predicates false, matching the alliance block. */
    bool voteRunning = false, voteCanSurrender = false;
    if (cs) {
        /* Votes return to the lobby; on a lobby-less server the server
         * rejects them, so disable the native Vote: items there too. */
        voteRunning = (clientSimGetNetStatus(cs) == netRunning) &&
                      clientSimIsLobbyAvailable(cs);
        if (voteRunning) {
            int activeTeams = 0;
            bool teamSeen[17] = {0};
            for (int i = 0; i < MAX_PLAYERS; i++) {
                const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
                if (!ls || !ls->connected || ls->isBot) continue;
                uint8_t t = ls->teamNumber;
                if (t == 0 || t > 16) continue;
                if (!teamSeen[t]) { teamSeen[t] = true; activeTeams++; }
            }
            const ClientLobbySlot *meSlot =
                clientSimGetLobbySlot(cs, clientSimGetMyPlayerNum(cs));
            bool meUnassigned = (meSlot && meSlot->teamNumber == 0);
            voteCanSurrender = (activeTeams == 2) && !meUnassigned;
        }
    }
    s->voteRunning      = voteRunning;
    s->voteCanSurrender = voteCanSurrender;

    /* Per-slot snapshot — uses the fresh ping accessor (the s_playerPing
     * cache is updated only when the server pushes; the accessor includes
     * unflushed local timing). Stale slot rows in the native menu are
     * cheap (one drawRect per refresh), so we fill all 16 unconditionally
     * and let mac_menubar_refresh() decide between view + numeric title. */
    /* The copy below takes sizeof p->name bytes out of s_playerName[i].
     * mac_menubar.h spells the field length as a literal to stay free of
     * global.h, so a divergence would read past the source array. */
    static_assert(sizeof(((struct MacPlayerSlot *)0)->name) == PLAYER_NAME_LEN,
                  "MacPlayerSlot.name must match PLAYER_NAME_LEN");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        struct MacPlayerSlot *p = &s->players[i];
        p->enabled = s_playerEnabled[i];
        p->checked = s_playerChecked[i];
        if (p->enabled) {
            memcpy(p->name, s_playerName[i], sizeof p->name);
            p->name[sizeof p->name - 1] = '\0';
            memcpy(p->country, s_playerCountry[i], sizeof p->country);
            p->country[sizeof p->country - 1] = '\0';
            p->pflags = (int)s_playerFlags[i];
            p->ptype  = (int)s_playerClientType[i];
            p->ping   = cs ? (int)clientSimGetPlayerPing(cs, (BYTE)i) : 0;
            /* Band travels with the number so the native row doesn't
             * re-derive thresholds and lose the hysteresis. */
            p->pingBand = cs ? (int)clientSimGetPlayerPingBand(cs, (BYTE)i)
                             : PING_BAND_NONE;
        } else {
            p->name[0]    = '\0';
            p->country[0] = '\0';
            p->pflags     = 0;
            p->ptype      = 0;
            p->ping       = 0;
            p->pingBand   = PING_BAND_NONE;
        }
    }

    /* Brains submenu snapshot — parent is enabled-gated on aiActive (so
     * the menu is visible-but-disabled until an AI tank is in play); the
     * brain list is capped at 16 entries (mac_menubar refresh sizes its
     * NSMenuItem cache to match). The Settings entry is only meaningful
     * for Lua brains — ONNX brains have no set_setting hook. */
    s->aiActive           = cs ? (clientSimGetAiType(cs) != aiNone) : false;
    s->brainRunning       = luaBrainIsRunning();
    s->brainRunIdx        = luaBrainGetRunningIndex();
    s->brainSettingsShown = s->brainRunning && !mlBrainSingletonIsRunning();

    int totalBrains = luaBrainGetNum();
    int snapCount   = (totalBrains > 16) ? 16 : totalBrains;
    s->brainCount   = snapCount;
    for (int i = 0; i < snapCount; i++) {
        const char *name = luaBrainGetName(i);
        const char *src  = name ? name : "?";
        strncpy(s->brainNames[i], src, sizeof s->brainNames[i] - 1);
        s->brainNames[i][sizeof s->brainNames[i] - 1] = '\0';
    }
}
#endif

/* Drain any pending NAME_* reject (CTRL_COMMAND_REJECTED with a
 * CMD_REJECT_NAME_* reason) into the in-game message overlay when
 * we're not in the lobby. The lobby toast in lobbyRenderRejectToast
 * handles the in-lobby case; this closes the gap for in-game name
 * changes (WinBolo > Change Name, Settings > Player Name), which
 * non-WBN servers accept at any phase. Clearing the reject after
 * surfacing avoids re-showing the same line in the lobby toast on
 * return to lobby. Non-name reason codes (1-8) fall through and stay
 * pending so the lobby toast still surfaces them later. */
static void drainInGameNameReject(ClientSim *cs) {
    if (!cs) return;
    if (clientSimIsInLobby(cs)) return;
    if (clientSimGetLobbyLastRejectPacket(cs) == 0) return;
    langid msgId;
    switch (clientSimGetLobbyLastRejectReason(cs)) {
        case  9: msgId = STR_NAME_INVALID_EMPTY;           break;  /* CMD_REJECT_NAME_EMPTY */
        case 10: msgId = STR_NAME_INVALID_RESERVED_PREFIX; break;  /* CMD_REJECT_NAME_RESERVED_PREFIX */
        case 11: msgId = STR_NAME_INVALID_RESERVED_SUFFIX; break;  /* CMD_REJECT_NAME_RESERVED_SUFFIX */
        case 12: msgId = STR_NAME_INVALID_MIXED_SCRIPTS;   break;  /* CMD_REJECT_NAME_MIXED_SCRIPTS */
        case 13: msgId = STR_NAME_INVALID_CHARS;           break;  /* CMD_REJECT_NAME_INVALID */
        case 14: msgId = STR_DLGSETNAME_INUSE_ERR;         break;  /* CMD_REJECT_NAME_TAKEN */
        default: return;
    }
    clientSimNetStatusMessage(cs, langGetText(msgId));
    clientSimClearLobbyLastReject(cs);
}

void sdl3ImguiPumpAndRender(ClientSim *cs) {
    if (!s_window || !s_renderer) return;

    /* One read a frame, ahead of the menu bars and the suppression points
       below. A frame of staleness costs nothing: the setting is lobby-only
       and cannot change while a game runs. */
    s_classicMode = (cs != nullptr && !clientSimIsSpectator(cs) &&
                     clientSimGetClassicMode(cs));

    /* Sync Steam Input action set to current gameplay context.  Must
       run before any consumer of action data (edge triggers below
       and the input wiring downstream). */
    update_steam_input_action_set(cs);

    /* Feed ImGui gamepad nav from Steam Input on Path A — must run
       before NewFrame so the events are visible to ImGui this frame. */
    imguiSteamNavFeedCurrentContext();

#ifdef __APPLE__
    if (!uiModeIsTablet()) {
        MacMenuState mms = {};
        populateMacMenuState(&mms, cs);
        mac_menubar_refresh(&mms);
    }
#endif

    /* Apply a pending UI-scale change here — after the previous frame's
       SDL_RenderPresent and before this frame's NewFrame — so rebuilding the
       font atlas can't race draw data still queued against the old texture. */
    if (s_pendingUiScaleRebuild) {
        s_pendingUiScaleRebuild = false;
        applyMainContextUiScale();
    }

    /* Same window for a skin change — the previous frame's draw data, which
       can reference the old tile texture, has already been presented. */
    if (s_pendingSkinReload) {
        s_pendingSkinReload = false;
        gameFrontReloadSkins();
    }

    /* Build the ImGui frame */
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    dialogResetTextInputArea(s_window);

    /* Override ImGui's DisplaySize for tablet and Deck modes.
       In both, SDL logical presentation scales the whole window, so ImGui
       needs to render in that coordinate space — otherwise dialogs draw
       at native pixel coords inside a logical surface and scale wrong.
       In desktop mode, ImGui renders at native window coordinates (no override)
       because the game is blitted to a scaled rect, not the whole window. */
    if (uiModeIsTablet() || uiModeIsSteamDeck()) {
        int logW = 0, logH = 0;
        SDL_RendererLogicalPresentation logMode;
        SDL_GetRenderLogicalPresentation(s_renderer, &logW, &logH, &logMode);
        if (logW > 0 && logH > 0 && logMode != SDL_LOGICAL_PRESENTATION_DISABLED) {
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)logW, (float)logH);
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
    }

    ImGui::NewFrame();
    s_closeAllPopups = false;

    /* Clear nav focus when user clicked in the game area last frame,
       so menu close does not restore focus to an ImGui window. */
    if (s_clearNavFocus) {
        ImGui::SetWindowFocus(nullptr);
        s_clearNavFocus = false;
    }

    /* Keep the keyboard with the game during active play. With
       NavEnableKeyboard on, clicking the menu bar (or just closing a menu)
       leaves an ImGui window nav-focused, which latches io.WantCaptureKeyboard
       true indefinitely — the swallow in sdl3ImguiProcessEvents then eats
       event-driven game keys like Tank View, trapping the player in pill view.
       So while a game is running and no panel, popup, menu, or text field is
       genuinely using ImGui, drop any lingering nav focus each frame.

       Hold off only while an ImGui item is actively held (ActiveId != 0):
       dropping focus there clears the active item mid-press, so an in-game
       overlay button (vote Yes/No, alliance Accept/Decline) gets pressed but
       never fires — the release lands with no active id to complete the
       click. An active item also means the player is driving the UI, not the
       tank, so the keyboard isn't being stolen from the game in that instant;
       the drop resumes the moment the press completes and ActiveId clears. */
    if (cs && !clientSimIsInLobby(cs) && !sdl3ImguiIsDialogOpen() &&
        !ImGui::GetIO().WantTextInput &&
        GImGui->ActiveId == 0) {
        ImGui::SetWindowFocus(nullptr);
    }

    /* Fold this frame's controller activity into the last-used device so the
       menu bar (below) and controller-mode UI track it.  Keyboard/mouse use
       arrives as SDL events (inputSourceUpdate in the event pump); Steam
       Input controller input does not, so poll it here. */
    inputSourceTick();

    /* In-game lobby (non-blocking host, e.g. the WASM client).
     *
     * On desktop the lobby is a separate BLOCKING modal (imguiLobbyShow)
     * with its own ImGui context, and this per-frame pump never runs while
     * it is up — so clientSimIsInLobby(cs) is ALWAYS false here on desktop
     * and every branch below is a no-op for it. On the WASM client this is
     * the only loop, so build the lobby into the shared frame and skip the
     * in-game HUD / menu bar / panels, then close out the frame the same way
     * the normal tail does.
     *
     * Release the per-frame lobby state on the edge out of the lobby (game
     * start, or a confirmed Leave) so its map-preview texture / popup
     * buffers don't leak and a later return to lobby starts clean. */
    {
        static bool s_wasInLobby = false;
        bool nowInLobby = (cs && clientSimIsInLobby(cs));
        if (s_wasInLobby && !nowInLobby) {
            imguiLobbyFrameReset();
            /* Lobby → running edge: play the game-start jingle, mirroring
               the desktop blocking loop's netRunning break. A Leave or a
               dropped connection exits the lobby too, but not into
               netRunning, so those stay silent. (Desktop never takes this
               edge — the blocking lobby owns the frame while inLobby.)
               SP jumps straight to running with no real-time countdown and
               gets no countdown cues, so suppress the start noise too for a
               silent SP entry (matches imguiLobbyShow). MP still plays it. */
            if (cs && clientSimGetNetStatus(cs) == netRunning &&
                !clientSimIsSinglePlayer(cs)) {
                soundPlayEffect(lobbyGameStart);
            }
        }
        s_wasInLobby = nowInLobby;

        if (nowInLobby) {
            if (imguiLobbyRenderFrame(cs) == LOBBY_FRAME_LEFT) {
                /* Confirmed Leave: drop the connection, then go wherever
                   this host goes when a game ends. The disconnect alone
                   strands the player in a frozen lobby: it tears down the
                   transport without touching netStat or inLobby, and
                   inLobby is only
                   cleared by the CTRL_GAME_PHASE_RUNNING control event,
                   which cannot arrive once the transport is gone. */
                clientSimDisconnect(cs);
#ifdef __EMSCRIPTEN__
                /* The browser has no welcome screen to fall back to the way
                   winbolo.c does after imguiLobbyShow returns 0 — the menu is
                   the hosting page, so navigate back to it. Ordered after the
                   disconnect so transportUdpClientDestroy still gets its
                   graceful PACKET_QUIT out over a live socket; the navigation
                   itself only runs once this frame returns to the browser. */
                windowLeaveGame();
#endif
            }
            keyboardUpdate();
            dialogDrawNavOutline();
            ImGui::Render();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s_renderer);
            return;
        }
    }

    /* First window of the frame, so the full-window map reads as the backdrop
       every panel, overlay and dialog that follows sits on. Submitting it here
       is not what holds it there — see the reorder inside. */
    renderOverviewInWindow(cs);

    /* Pause-overlay open trigger: the controller's Menu/☰ button (the bound
       Pause action, default Start). Opens whenever a controller is connected
       — matches the Escape-key trigger — so pad users always have a way in,
       even on desktop where the Controller Mode pref is off. */
    if (inputGamepadIsPauseEdge() && inputGamepadIsConnected()) {
        deckPauseOpen();
    }
    /* Active-controller-disconnect: show the "Controller Disconnected" dialog
       so the player knows their pad dropped (battery dies, dongle drops) and
       can reconnect or carry on with keyboard/mouse.  The edge comes from the
       Steam Input device hot-plug callback (or SDL removal) — it only fires
       for a real controller, so no pref gate is needed.  In a solo game
       (single-player / tutorial) freeze the sim via the shared pause path;
       multiplayer keeps running.  Shown in-game and in the in-game lobby.
       Skipped on tablet (mobile has its own touch UX) because the dialog is
       only rendered in the non-tablet branch below; opening it here would
       freeze a solo game behind a modal that never draws. */
    if (inputGamepadConsumeActiveDisconnect() && !uiModeIsTablet() &&
        !controllerDisconnectIsOpen()) {
        controllerDisconnectOpen();
        windowControllerLostPause(cs, true);
    }
    /* Auto-dismiss when a REAL controller is (re)connected.  Must use real
       presence, not inputGamepadIsConnected() — the latter is pinned true by
       the Steam Input virtual controller, which would dismiss the dialog the
       instant it opened. */
    if (controllerDisconnectIsOpen() && inputGamepadRealControllerConnected()) {
        controllerDisconnectClose();
        windowControllerLostPause(cs, false);
    }
    /* Quick-chat open trigger: D-pad UP, in-game only.  Gamepad-universal
       (not Deck-gated) — desktop gamepad players also benefit.  Skipped
       in lobby because the lobby has its own chat UI, and skipped while
       any in-game panel / overlay is open (settings, players, send-msg,
       pause, popups) so D-pad nav inside those windows isn't also
       interpreted as a quick-chat open. */
    if (inputGamepadIsQuickChatEdge() && cs && !clientSimIsInLobby(cs) &&
        !sdl3ImguiIsDialogOpen() && !quickChatIsOpen()) {
        quickChatOpen();
    }

    /* B button — close the topmost open in-game panel.  Modal popups
       (pause, quick-chat) close themselves via the p_open passed to
       BeginPopupModal; ImGui's NavCancel only closes non-modal popups.
       Regular ImGui windows (Settings, Players, Send Message, etc.)
       aren't auto-closed by anything, so handle them here.  Skipped
       while a popup is on the stack or a text input is active — those
       want B for popup-close / clear-text first.

       Accept both the SDL gamepad B (desktop pad) and Escape: on a
       Steam launch Steam Input grabs the physical pad and hides it from
       SDL, so B never arrives as ImGuiKey_GamepadFaceRight — it's
       injected as Escape by imguiSteamNavFeedCurrentContext (and the
       keyboard Escape lands the same way).  Without the Escape branch
       these panels can't be closed with B on the Deck. */
    {
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        bool anyPopup = (ctx && ctx->OpenPopupStack.Size > 0);
        bool cancelEdge =
            (inputGamepadIsConnected() &&
             ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        bool cancelClosedPanel = false;
        if (cancelEdge && !anyPopup && !ImGui::GetIO().WantTextInput) {
            /* The info windows open on top of Settings, so B must close them
               before Settings — check them first in the ladder. */
            if      (s_showSysInfo)        { s_showSysInfo = false;      cancelClosedPanel = true; }
            else if (s_showNetInfo)        { s_showNetInfo = false;      cancelClosedPanel = true; }
            else if (s_showGameInfo)       { s_showGameInfo = false;     cancelClosedPanel = true; }
            else if (s_showSettings)       { s_showSettings = false;     cancelClosedPanel = true; }
            else if (s_showSendMsg)        { s_showSendMsg = false;      cancelClosedPanel = true; }
            else if (s_showPlayersPanel)   { s_showPlayersPanel = false; cancelClosedPanel = true; }
            else if (s_brainSettingsOpen)  { s_brainSettingsOpen = false;cancelClosedPanel = true; }
            else if (s_allianceVisible)    { s_allianceVisible = false;  cancelClosedPanel = true; }
        }

        /* Escape opens the pause overlay when a controller is connected and
           nothing else is in the way — mirrors the Start-button trigger so
           keyboard + pad users both have a way in (the menu bar is hidden in
           controller mode). Only when Escape didn't just close a panel/popup
           and we're in an active game (not the lobby). */
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
            inputGamepadIsConnected() &&
            !anyPopup && !cancelClosedPanel && !ImGui::GetIO().WantTextInput &&
            cs && !clientSimIsInLobby(cs) &&
            !deckPauseIsOpen() && !sdl3ImguiIsDialogOpen()) {
            deckPauseOpen();
        }
    }

    if (uiModeIsTablet()) {
        sdl3ImguiTabletOverlay(cs);
    } else {
#ifndef __APPLE__
        /* Show the menu bar only when the player's last-used device is the
           keyboard/mouse.  A connected-but-idle controller doesn't hide it,
           and reaching for the mouse/keyboard brings it back; using the
           controller hides it again (the pause overlay is the pad's way in).
           Kept hidden while the controller-disconnected dialog is up so the
           strip doesn't flash behind the modal.  macOS routes the menu
           through native NSMenu so the in-window bar is never drawn there.
           Never draw it in controller-first mode (the Steam Deck) — the
           Deck's virtual pad reports as keyboard input, which would
           otherwise let the bar show; this also matches sdl3draw.c
           reserving zero menu-bar height under uiShouldUseControllerMode(). */
        if (inputSourceCurrent() == INPUT_SOURCE_KEYBOARD &&
            !controllerDisconnectIsOpen() &&
            !uiShouldUseControllerMode()) {
            renderMenuBar(cs);
        }
#endif
        /* DEBUG overlay: left-stick / turn / build-cursor magnitudes (0..1).
           Gated on WB_CONTROLLER_DEBUG (input_gamepad.h). */
        if (WB_CONTROLLER_DEBUG &&
            inputGamepadIsConnected() && cs && !clientSimIsInLobby(cs)) {
            ImGuiViewport *vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8.0f,
                                           vp->WorkPos.y + 8.0f),
                                    ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.55f);
            if (ImGui::Begin("##stickdbg", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
                /* Scroll-method selector (experiment). Picking manual turns
                   autoscroll off; the others turn it on. */
                static const char *kScrollNames[4] = {
                    "winbolo v1 manual", "winbolo v1 autoscroll",
                    "enhanced autoscroll", "enhanced autoscroll, Canuck's"
                };
                int mech = clientSimGetScrollMechanism();
                ImGui::SetNextItemWidth(200);
                if (ImGui::Combo("scroll", &mech, kScrollNames, 4)) {
                    clientSimSetScrollMechanism(mech);
                    clientSimSetAutoScroll(cs, mech != 0);
                }
                ImGui::Separator();
                ImGui::Text("stick mag: %.2f", g_dbgStickMag);
                ImGui::ProgressBar(g_dbgStickMag, ImVec2(160, 0));
                ImGui::Text("turn  mag: %.2f", g_dbgTurnMag);
                ImGui::ProgressBar(g_dbgTurnMag, ImVec2(160, 0));
                ImGui::Separator();
                ImGui::Text("cursor stick: %.2f", g_dbgCursorStickMag);
                ImGui::ProgressBar(g_dbgCursorStickMag, ImVec2(160, 0));
                ImGui::Text("cursor move : %.2f", g_dbgCursorMoveMag);
                ImGui::ProgressBar(g_dbgCursorMoveMag, ImVec2(160, 0));
            }
            ImGui::End();
        }

        /* Pause overlay + quick-chat overlay (no-ops when closed). */
        deckPauseRender(cs);
        /* Freeze a solo game while the pause overlay is up — single-player /
           tutorial only; multiplayer keeps running or the player is booted for
           idling.  Edge-detect the overlay open/close so the shared solo-pause
           path toggles exactly once each way, mirroring the disconnect dialog.
           Checked after deckPauseRender so deckPauseIsOpen() reflects this
           frame's state (the open trigger sets a pending flag the render
           consumes). */
        {
            static bool s_lastDeckPause = false;
            bool nowDeckPause = deckPauseIsOpen();
            if (nowDeckPause != s_lastDeckPause) {
                windowDeckPause(cs, nowDeckPause);
                s_lastDeckPause = nowDeckPause;
            }
        }
        /* Tutorial message overlay — same in-loop pattern as the pause
           menu, so the input gate suspends play and the solo-pause path
           freezes the sim while a message is up.  Edge-detect open/close
           to toggle the freeze exactly once each way. */
        tutorialOverlayRender(cs);
        {
            static bool s_lastTutorialPause = false;
            bool nowTutorialPause = tutorialOverlayIsOpen();
            if (nowTutorialPause != s_lastTutorialPause) {
                windowTutorialPause(cs, nowTutorialPause);
                s_lastTutorialPause = nowTutorialPause;
            }
        }
        quickChatRender(cs);
        renderCtrlSendMsg(cs);

        /* Controller-disconnected dialog.  Returns true the frame the
           "keyboard and mouse" button is pressed — it has already switched
           controller mode off and closed itself, so just unpause any solo
           game (no-op in multiplayer). */
        if (controllerDisconnectRender()) {
            windowControllerLostPause(cs, false);
        }
    }

    /* Retract any open menu-bar dropdown when a panel/chat was just opened, so
       the two don't render active at once. Modals (pause, quick-chat, password,
       change-name, key-setup) are left open. */
    if (s_closeMenuPopups) {
        ImGui::ClosePopupsExceptModals();
        s_closeMenuPopups = false;
    }

    renderBrainSettingsWindow();
    renderSettingsPanel(cs);

    /* Info panels — standard ImGui windows in the main context */
    renderSysInfoPanel();
    renderNetInfoPanel(cs);
    renderGameInfoPanel(cs);
    renderSendMsgPanel(cs);
    renderPlayersPanel(cs);

    /* Modal dialogs */
    aboutPopupRender();
    renderChangeNameModal(cs);
    renderAllianceRequest(cs);
    renderGameVoteWidgets(cs);
    /* Per-frame tick that emits the 3/2/1 newswire lines while a
     * vote-driven back-to-lobby is in-flight. Counts off the local
     * clock; no per-second server broadcast involved. */
    clientSimTickLobbyReturnCountdown(cs);
    /* Surface in-game CMD_REJECT_NAME_* rejects through the message
     * overlay. The lobby toast handles the in-lobby case. */
    drainInGameNameReject(cs);
    renderPasswordModal();
    imguiKeySetupRenderInGamePopup(cs);
    renderJoinConfirmModal();

    /* Extra render callback (e.g. Android players panel) */
    if (s_extraRenderFn) {
        s_extraRenderFn(cs);
    }

    /* Tablet: close all panels/popups when user taps outside dialog windows */
    if (uiModeIsTablet() && ImGui::IsMouseClicked(0)) {
        ImGuiContext *g = ImGui::GetCurrentContext();
        bool anyDialogOpen = s_showSysInfo || s_showNetInfo || s_showGameInfo ||
                             s_showSendMsg || s_showPlayersPanel || s_showSettings ||
                             s_brainSettingsOpen || s_allianceVisible ||
                             g->OpenPopupStack.Size > 0;

        if (anyDialogOpen) {
            /* Check if tap landed inside any dialog/popup window (not the
               full-screen background or tablet overlay windows). */
            static const char *dialogNames[] = {
                "###sysinfo", "###netinfo", "###gameinfo",
                "###sendmsg", "###playerspanel", "###settings",
                "###brainsettings", "###alliancereq",
                "###about", "###thirdparty", "###authors", "###changename",
                "###passwordreq", "###keysetup",
            };
            bool overDialog = false;
            for (int i = 0; i < (int)(sizeof(dialogNames) / sizeof(dialogNames[0])); i++) {
                ImGuiWindow *w = ImGui::FindWindowByName(dialogNames[i]);
                if (w && w->Active && w->WasActive) {
                    if (ImGui::IsMouseHoveringRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), false)) {
                        overDialog = true;
                        break;
                    }
                }
            }
            /* Also check popup stack windows (modals) */
            if (!overDialog) {
                for (int i = 0; i < g->OpenPopupStack.Size; i++) {
                    ImGuiWindow *w = g->OpenPopupStack[i].Window;
                    if (w && w->Active) {
                        if (ImGui::IsMouseHoveringRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), false)) {
                            overDialog = true;
                            break;
                        }
                    }
                }
            }
            if (!overDialog) {
                s_showSysInfo        = false;
                s_showNetInfo        = false;
                s_showGameInfo       = false;
                s_showSendMsg        = false;
                s_showPlayersPanel   = false;
                s_showSettings       = false;
                s_brainSettingsOpen  = false;
                s_allianceVisible    = false;
                s_closeAllPopups = true;
                aboutPopupCloseAll();
                dialogDismissKeyboard(s_window);
            }
        }
    }


    /* Controller text entry: bring up Steam's floating keyboard or ours (or
       neither) for this frame and draw / inject ours if it is up. */
    keyboardUpdate();

    dialogDrawNavOutline();
    ImGui::EndFrame();
    dialogDrawNavOutline();
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s_renderer);

    /* Render pop-out windows */
    {
        ImGuiContext *mainCtx = ImGui::GetCurrentContext();

        if (popOutBeginFrame(&s_popSysInfo)) {
            popOutBeginContent();
            renderSysInfoContent();
            popOutEndContent(&s_popSysInfo);
            popOutEndFrame(&s_popSysInfo);
        }
        if (popOutBeginFrame(&s_popNetInfo)) {
            popOutBeginContent();
            renderNetInfoContent(cs);
            popOutEndContent(&s_popNetInfo);
            popOutEndFrame(&s_popNetInfo);
        }
        if (popOutBeginFrame(&s_popGameInfo)) {
            popOutBeginContent();
            renderGameInfoContent(cs);
            popOutEndContent(&s_popGameInfo);
            popOutEndFrame(&s_popGameInfo);
        }
        if (popOutBeginFrame(&s_popSendMsg)) {
            popOutBeginContent();
            renderSendMsgContent(cs);
            popOutEndContent(&s_popSendMsg);
            popOutEndFrame(&s_popSendMsg);
        }
        if (popOutBeginFrame(&s_popPlayers)) {
            popOutBeginContent();
            renderPlayersContent(cs);
            popOutEndContent(&s_popPlayers);
            popOutEndFrame(&s_popPlayers);
        }
        /* The overview shows what this game has revealed, so it goes away
           with the game rather than sitting over the lobby. Hidden, not
           destroyed — see popOutHide. That hide leaves
           gameFrontShowMapOverview alone, so a player who had the overview up
           gets it back when the next game starts; only an explicit close
           forgets it. Going through sdl3ImguiShowMapOverview rather than
           mapOverviewOpen keeps the reopen behind the same platform and
           tablet tests every other caller uses. */
        bool overviewRunning = (cs != nullptr && clientSimIsRunning(cs));
        if (!overviewRunning) {
            mapOverviewHide();
            /* The in-window map view goes with the game for the same reason.
               What the window does from there is overviewInWindowSet's call:
               windowed for the lobby when app full screen is off, still full
               screen when it is on. */
            overviewInWindowSet(false);
        } else if (!s_overviewWasRunning) {
            /* Only one of the two can come back, because full screen mode
               draws the map itself: the in-window view when the app is full
               screen, the pop-out on its own flag when it is not. */
            if (gameFrontFullScreen) sdl3ImguiShowOverviewInWindow(true);
            else if (gameFrontShowMapOverview) sdl3ImguiShowMapOverview(true);
        }
        s_overviewWasRunning = overviewRunning;
        /* Draw the map into the view's offscreen before the pop-out's ImGui
           frame opens: it swaps the render target and re-points the tile
           sampler, neither of which belongs in the middle of the draw list
           ImGui is about to build. */
        if (s_popMapOverview.open && s_popMapOverview.window) {
            if (!s_overviewView) {
                s_overviewView = overviewViewCreate();
                /* The view is made once per process, so this is the one
                   moment the saved camera state is applied — after it, the
                   camera is whatever the player has since done to it. */
                OverviewCamera *cam = overviewViewCamera(s_overviewView);
                if (cam) {
                    overviewCameraSetZoomScale(cam, gameFrontOverviewZoom);
                    cam->follow = gameFrontOverviewFollow;
                }
            }
            if (!s_overviewSnapshot) {
                s_overviewSnapshot = overviewSnapshotCreate();
            }
            SDL_Texture *ovTiles = overviewEnsureTiles(s_popMapOverview.renderer);
            SDL_Texture *ovCross =
                overviewEnsureCrosshair(s_popMapOverview.renderer);
            /* The fill reads the fog memory, the local tank and the
               per-frame entity lists straight out of the ClientSim, and the
               host server's timer thread writes into those as it dispatches
               a tick to in-process subscribers. The main view's draw takes
               the same lock around the same kind of read in winbolo.c.
               Nothing is held on entry — winbolo.c releases before calling
               the pump, and nothing inside the fill takes a lock of its
               own — so there is no ordering here to invert. The two
               texture-ensure calls above build from assets and touch no sim
               state, so they stay outside.

               Only the fill is under the lock. The render draws from the
               snapshot after the release, so its render-target switches and
               the flushes they force never hold up the server's tick. */
            clientMutexWaitFor();
            clientSimFillOverviewSnapshot(cs, s_overviewSnapshot);
            clientMutexRelease();
            overviewViewRenderOffscreen(s_overviewView,
                                        s_popMapOverview.renderer,
                                        ovTiles, s_overviewTilesScale, ovCross,
                                        s_popMapOverview.width,
                                        s_popMapOverview.height,
                                        s_overviewSnapshot, false);
        }
        if (popOutBeginFrame(&s_popMapOverview)) {
            /* The map fills the window edge to edge: the image is exactly
               DisplaySize, so the usual window padding would push it into a
               scrollbar. */
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            popOutBeginContent();
            ImGui::PopStyleVar();
            renderMapOverviewContent(cs);
            popOutEndContent(&s_popMapOverview);
            popOutEndFrame(&s_popMapOverview);
        }

        ImGui::SetCurrentContext(mainCtx);
    }

    /* Apply deferred zoom change after the frame is fully rendered.
       windowZoomChange reconfigures the live renderer in place, so it
       must not run while we are mid-frame. */
    if (s_pendingZoom != 255) {
        BYTE zoom = s_pendingZoom;
        bool fromResize = s_pendingZoomFromResize;
        s_pendingZoom = 255;
        s_pendingZoomFromResize = false;
        /* Suppress auto-switch to Custom for fixed mode changes */
        if (zoom != ZOOM_FACTOR_CUSTOM) {
            s_suppressAutoCustom = true;
        }
        /* Save window position if this came from a resize snap */
        int savedX = 0, savedY = 0;
        if (fromResize && s_window) {
            SDL_GetWindowPosition(s_window, &savedX, &savedY);
        }
        windowZoomChange(zoom, fromResize);
        /* Restore position after zoom change if from resize snap */
        if (fromResize) {
            SDL_Window *win = sdl3DrawGetWindow();
            if (win) {
                SDL_SetWindowPosition(win, savedX, savedY);
            }
        }
        /* Clear suppress flag now that zoom change is complete */
        s_suppressAutoCustom = false;
    }

    /* Apply the deferred full screen change, after the frame for the same
       reason as the zoom above — the settings tab that asks for it renders
       mid-frame.  In a game this goes through the in-window map view, the one
       path the File menu and the macOS menu bar use as well. */
    if (s_pendingFullScreen >= 0) {
        bool want = (s_pendingFullScreen != 0);
        s_pendingFullScreen = -1;
        sdl3ImguiShowOverviewInWindow(want);
    }

    /* Initialise WBN popup state on first settings open */
    if (s_showSettings && !s_wbnInitialised) {
        s_wbnInitialised = true;
        imguiWinbolonetReset();
    }
    if (!s_showSettings) {
        s_wbnInitialised = false;
    }
}

void sdl3ImguiSetExtraRenderCallback(sdl3ImguiExtraRenderFn fn) {
    s_extraRenderFn = fn;
}

void sdl3ImguiShowSysInfo(bool open) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) {
        /* While the overview owns the game window the panel opens in-window,
           the way the menu item does: a pop-out would land behind the map. */
        if (sdl3DrawIsOverviewInWindow()) {
            if (open && !s_showSysInfo) sysInfoGraphReset();
            s_showSysInfo = open;
            return;
        }
        if (open) {
            if (!s_popSysInfo.open) {
                sysInfoGraphReset();
                popOutCreate(&s_popSysInfo, langGetText(STR_DLGSYSINFO_TITLE), 440, 600, 0);
            }
        } else {
            if (s_popSysInfo.open) popOutHide(&s_popSysInfo);
        }
        return;
    }
#endif
    if (open && !s_showSysInfo) sysInfoGraphReset();
    s_showSysInfo = open;
}
bool sdl3ImguiIsSysInfoOpen(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    /* Reports the in-window panel while the overview owns the window, matching
       where sdl3ImguiShowSysInfo puts it. The macOS menu toggles these items
       with Show(!IsOpen()) and draws their checkmarks from the same answer, so
       reading the pop-out here would leave them open-only. */
    if (!uiModeIsTablet())
        return sdl3DrawIsOverviewInWindow() ? s_showSysInfo : s_popSysInfo.open;
#endif
    return s_showSysInfo;
}

float sdl3ImguiGetUiScale(void) {
    return s_uiScale;
}
void sdl3ImguiShowNetInfo(bool open) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) {
        /* In-window while the overview owns the window — see
           sdl3ImguiShowSysInfo. */
        if (sdl3DrawIsOverviewInWindow()) {
            if (open && !s_showNetInfo) pingGraphReset();
            s_showNetInfo = open;
            return;
        }
        if (open) {
            if (!s_popNetInfo.open) {
                pingGraphReset();
                popOutCreate(&s_popNetInfo, langGetText(STR_DLGNETINFO_TITLE), 360, 420, 0);
            }
        } else {
            if (s_popNetInfo.open) popOutHide(&s_popNetInfo);
        }
        return;
    }
#endif
    if (open && !s_showNetInfo) pingGraphReset();
    s_showNetInfo = open;
}
bool sdl3ImguiIsNetInfoOpen(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    /* In-window while the overview owns the window — see
       sdl3ImguiIsSysInfoOpen. */
    if (!uiModeIsTablet())
        return sdl3DrawIsOverviewInWindow() ? s_showNetInfo : s_popNetInfo.open;
#endif
    return s_showNetInfo;
}
void sdl3ImguiShowGameInfo(bool open) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) {
        /* In-window while the overview owns the window — see
           sdl3ImguiShowSysInfo. */
        if (sdl3DrawIsOverviewInWindow()) {
            s_showGameInfo = open;
            return;
        }
        if (open) {
            if (!s_popGameInfo.open) {
                popOutCreate(&s_popGameInfo, langGetText(STR_DLGGAMEINFO_TITLE), 320, 200, 0);
            }
        } else {
            if (s_popGameInfo.open) popOutHide(&s_popGameInfo);
        }
        return;
    }
#endif
    s_showGameInfo = open;
}
bool sdl3ImguiIsGameInfoOpen(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    /* In-window while the overview owns the window — see
       sdl3ImguiIsSysInfoOpen. */
    if (!uiModeIsTablet())
        return sdl3DrawIsOverviewInWindow() ? s_showGameInfo : s_popGameInfo.open;
#endif
    return s_showGameInfo;
}
void sdl3ImguiShowSendMsg(bool open) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) {
        if (uiShouldUseControllerMode()) {
            /* Controller: the simplified modal (renderCtrlSendMsg).  The
               pop-out is a separate OS window with its own ImGui context that
               receives no controller input — Steam-nav B/Escape feeds only the
               main context and gamepad SDL events aren't routed to pop-outs —
               so a pad user could open it but never close it. */
            if (open) {
                s_pendingCtrlSendMsg = true;
            } else {
                s_showCtrlSendMsg = false;
            }
        } else if (sdl3DrawIsOverviewInWindow()) {
            /* In-window while the overview owns the window — see
               sdl3ImguiShowSysInfo. */
            s_showSendMsg = open;
            if (open) {
                s_sendMsgFocusInput = true;
                s_closeMenuPopups = true;
            }
        } else {
            /* Mouse/keyboard desktop: the draggable pop-out window.  Opening
               an already-open pop-out raises and refocuses it — see
               sendMsgPopOutShow. */
            if (open) {
                sendMsgPopOutShow();
            } else {
                if (s_popSendMsg.open) popOutHide(&s_popSendMsg);
            }
        }
        return;
    }
#endif
    s_showSendMsg = open;
    if (open) {
        /* No cooldown reset here — see sendMsgPopOutShow. s_sendMsgCooldownEnd
           is set only by an actual send and cleared only by time, so reopening
           the panel cannot shorten SEND_MSG_WAIT_MS. */
        s_sendMsgFocusInput = true;
        s_closeMenuPopups = true;
#if BOLO_MOBILE
        s_showSettings = false;
        s_showPlayersPanel = false;
#endif
    }
}

/* What Ctrl/Cmd+M does, for every desktop entry point that carries that
 * shortcut — the key handler here and the macOS menu item.
 *
 * Against a pop-out it opens and never closes: the pop-out is a separate OS
 * window usually sitting behind the game, the key press lands on the main
 * window, and a player pressing it means "bring the message box forward" —
 * see sendMsgPopOutShow. The pop-out's close box or Escape is the way out.
 *
 * The in-window panel the overview mode draws has no window to be behind. The
 * same press with it already on screen can only mean close it, so there it
 * toggles, matching the Players menu item. */
void sdl3ImguiSendMsgShortcut(void) {
    if (panelsStandInForPopOuts() && s_showSendMsg) {
        sdl3ImguiShowSendMsg(false);
        return;
    }
    sdl3ImguiShowSendMsg(true);
}

bool sdl3ImguiIsSendMsgOpen(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    /* Reports the in-window panel while the overview owns the window, matching
       where sdl3ImguiShowSendMsg puts it — see sdl3ImguiIsSysInfoOpen. */
    if (!uiModeIsTablet())
        return sdl3DrawIsOverviewInWindow() ? s_showSendMsg : s_popSendMsg.open;
#endif
    return s_showSendMsg;
}
/* The overview has no in-window twin, so in tablet mode there is nothing to
 * show and nothing to report open. */
void sdl3ImguiShowMapOverview(bool open) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) {
        if (open) mapOverviewOpen(); else mapOverviewClose();
        return;
    }
#endif
    (void)open;
}
bool sdl3ImguiIsMapOverviewOpen(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) return s_popMapOverview.open;
#endif
    return false;
}
/* The in-window mode is a desktop-window mode, so tablet has nothing to show
 * and nothing to report active. */
void sdl3ImguiShowOverviewInWindow(bool active) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) {
        overviewInWindowChoose(active);
        return;
    }
#endif
    (void)active;
}
/* The one full screen command, shared by Alt+Enter and the macOS Window menu
 * so the two routes cannot drift. In a game it is the full screen map, which
 * carries the window full screen with it; outside one there is no map to
 * show, so it is the plain app full screen flag. */
void sdl3ImguiToggleFullScreen(struct ClientSim *cs) {
    if (cs != nullptr && clientSimIsRunning(cs)) {
        sdl3ImguiShowOverviewInWindow(!sdl3ImguiIsOverviewInWindowOpen());
    } else {
        windowFullScreenChoose(!gameFrontFullScreen);
    }
}
bool sdl3ImguiIsOverviewInWindowOpen(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet()) return sdl3DrawIsOverviewInWindow();
#endif
    return false;
}
void sdl3ImguiShowSettings(void) {
    s_showSettings = !s_showSettings;
    if (s_showSettings) {
        s_closeMenuPopups = true;
        imguiSettingsSeedPlayerName();
#if BOLO_MOBILE
        s_showSendMsg = false;
        s_showPlayersPanel = false;
#endif
    }
}
extern "C" void sdl3ImguiShowAbout(void) {
    aboutPopupOpen();
}
extern "C" void sdl3ImguiShowChangeName(void) {
    s_showChangeName = true;
}
extern "C" void sdl3ImguiSetFrameRate(int rate) {
    windowSetFrameRate(rate, true);
}
extern "C" void sdl3ImguiSetZoom(int zoom) {
    s_pendingZoom = (BYTE)zoom;
}
extern "C" void sdl3ImguiSetMessageLabelLen(ClientSim *cs, int len) {
    windowSetMessageLabelLen(cs, (labelLen)len);
}
extern "C" void sdl3ImguiSetTankLabelLen(ClientSim *cs, int len) {
    windowSetTankLabelLen(cs, (labelLen)len);
}

/* Brain-control trampolines for the macOS native Brains menu.
 * Encapsulate the luaBrain/mlBrain split + s_brainSettings ownership so
 * the .mm shim stays data-driven (via MacMenuState) and doesn't need to
 * link against the brain handler. Mirrors the in-window Brains menu at
 * renderMenuBar() above. */
extern "C" void sdl3ImguiStopBrain(void) {
    if (luaBrainIsRunning()) {
        luaBrainStop();
        mlBrainStopSingleton();
    }
}
extern "C" void sdl3ImguiStartBrain(int idx, ClientSim *cs) {
    if (idx < 0 || idx >= luaBrainGetNum()) return;
    const char *path = luaBrainGetPath(idx);
    const char *name = luaBrainGetName(idx);
    if (!path) return;
    if (luaBrainGetType(idx) == BRAIN_TYPE_ONNX) {
        mlBrainStartSingleton(path, name ? name : "", cs);
    } else {
        luaBrainStart(path, name ? name : "", cs);
    }
    luaBrainFreeSettings(s_brainSettings);
    s_brainSettings      = nullptr;
    s_brainSettingsCount = 0;
    s_brainSettingsOpen  = false;
}
extern "C" void sdl3ImguiShowBrainSettings(void) {
    luaBrainFreeSettings(s_brainSettings);
    s_brainSettings      = luaBrainGetSettings(&s_brainSettingsCount);
    s_brainSettingsOpen  = true;
    s_closeMenuPopups    = true;
}

void sdl3ImguiShowPlayersPanel(bool open) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    /* Mouse/keyboard desktop: the draggable pop-out window.  Controller mode
       stays on the in-window panel — the pop-out has its own ImGui context and
       receives neither gamepad events nor Steam-nav B/Escape, so a pad user
       could open it and never close it (see sdl3ImguiShowSendMsg). */
    if (!uiModeIsTablet() && !uiShouldUseControllerMode()) {
        if (open) {
            /* popOutCreate re-shows and raises a window it made earlier, so
               opening an already-open pop-out raises it. */
            if (popOutCreate(&s_popPlayers, langGetText(STR_DLGPLAYERS_TITLE), 520, 420, 0)) {
                /* The rows draw country flags, which have to be rasterized
                   against this window's own renderer. Idempotent, so the
                   re-show path above costs nothing. */
                flagsCreate(s_popPlayers.renderer);
            }
        } else {
            if (s_popPlayers.open) popOutHide(&s_popPlayers);
        }
        return;
    }
#endif
    s_showPlayersPanel = open;
    if (open) s_closeMenuPopups = true;
#if BOLO_MOBILE
    if (open) {
        s_showSendMsg = false;
        s_showSettings = false;
    }
#endif
}

void sdl3ImguiTogglePlayersPanel(void) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
    if (!uiModeIsTablet() && !uiShouldUseControllerMode()) {
        sdl3ImguiShowPlayersPanel(!s_popPlayers.open);
        return;
    }
#endif
    sdl3ImguiShowPlayersPanel(!s_showPlayersPanel);
}

bool sdl3ImguiGameInputWindowHasFocus(void) {
    if (s_window &&
        (SDL_GetWindowFlags(s_window) & SDL_WINDOW_INPUT_FOCUS)) {
        return true;
    }
    if (s_popMapOverview.open && s_popMapOverview.window &&
        (SDL_GetWindowFlags(s_popMapOverview.window) &
         SDL_WINDOW_INPUT_FOCUS)) {
        return true;
    }
    return false;
}

bool sdl3ImguiWantsKeyboard(void) {
    if (!s_window) return false;
    ImGuiIO &io = ImGui::GetIO();
    ImGuiContext *g = ImGui::GetCurrentContext();

    /* Standing in for a pop-out, these four must not suspend the game: a
       pop-out is a separate OS window, so the player kept driving with System
       Info up and the OS-focus gate in input.c muted the keys only once the
       pop-out actually took focus. The in-window stand-in has to match, or
       opening one full screen leaves the tank dead to every key with nothing
       on screen saying why. Send Message still suspends while its box holds
       the caret — that is io.WantTextInput above, and clicking back onto the
       map drops the caret and hands the keys back, the way clicking the game
       window behind the pop-out does.

       Everywhere else they keep blocking, unchanged: the tablet panels, and
       the same panels opened from Settings > Session on a desktop or a Deck,
       which are the panel itself rather than a stand-in for anything. */
    bool infoPanelsBlock = !panelsStandInForPopOuts() &&
                           (s_showSysInfo || s_showNetInfo ||
                            s_showGameInfo || s_showSendMsg);

    InputGateState st;
    st.textInputActive             = io.WantTextInput;
    st.blockingModalOpen           = infoPanelsBlock ||
                                     s_showPlayersPanel || s_showSettings ||
                                     s_brainSettingsOpen;
    /* Every popup currently on the stack is blocking (menu-bar dropdowns and
       the password / change-name / key-setup / pause / quick-chat modals).
       The transient notifications — alliance request and vote widgets — are
       plain Begin() windows, not popups, so they never land here. */
    st.menuOpen                    = (g && g->OpenPopupStack.Size > 0);
    st.allianceNotificationVisible = s_allianceVisible;  /* never suspends */
    st.voteVisible                 = false;              /* votes never suspend */
    st.appHasFocus                 = true;               /* input.c owns the OS-focus gate */
    return gameInputSuspended(&st);
}

void sdl3ImguiClearNavFocus(void) {
    s_clearNavFocus = true;
}

void sdl3ImguiShowAllianceRequest(const char *playerName, unsigned char playerNum) {
    strncpy(s_alliancePlayerName, playerName, sizeof(s_alliancePlayerName) - 1);
    s_alliancePlayerName[sizeof(s_alliancePlayerName) - 1] = '\0';
    s_alliancePlayerNum  = playerNum;
    s_showAllianceOpen   = true;
}

void sdl3ImguiShowPassword(void) {
    s_showPasswordOpen = true;
}

void sdl3ImguiSetPlayer(unsigned char playerNum, const char *name, const char *countryCode) {
    if (playerNum >= MAX_PLAYERS) return;
    strncpy(s_playerName[playerNum], name, sizeof(s_playerName[0]) - 1);
    s_playerName[playerNum][sizeof(s_playerName[0]) - 1] = '\0';
    if (countryCode) {
        s_playerCountry[playerNum][0] = countryCode[0];
        s_playerCountry[playerNum][1] = countryCode[1];
        s_playerCountry[playerNum][2] = '\0';
        WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[FLAGS] sdl3ImguiSetPlayer: player=%d name='%s' country='%s' (0x%02X 0x%02X)", playerNum, name, s_playerCountry[playerNum], (unsigned char)countryCode[0], (unsigned char)countryCode[1]);
    } else {
        s_playerCountry[playerNum][0] = '\0';
        WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[FLAGS] sdl3ImguiSetPlayer: player=%d name='%s' country=NULL", playerNum, name);
    }
    s_playerEnabled[playerNum] = true;
}

/* Two things to know about this getter, neither of them introduced by it.
 *
 * The bound is MAX_PLAYERS, this file's own 16, while the ally-view label
 * passes a tank slot (clientSimGetViewTarget). The two are the same number
 * today, so the check holds by value rather than by name — a MAX_TANKS that
 * ever diverged from it would want MAX_TANKS here instead.
 *
 * The read is unlocked, and s_playerName is written by sdl3ImguiSetPlayer
 * from frontEndSetPlayer, which the sim can reach on the hosted-server timer
 * thread while the render thread is drawing. The other readers of this mirror
 * (the Players menu, the native menu-bar snapshot) already read it the same
 * way; this getter widens that rather than starting it. Locking would be a
 * job for the whole mirror, not for one accessor. */
const char *sdl3ImguiGetPlayerName(unsigned char playerNum) {
    if (playerNum >= MAX_PLAYERS) return "";
    return s_playerName[playerNum];
}

void sdl3ImguiClearPlayer(unsigned char playerNum) {
    if (playerNum >= MAX_PLAYERS) return;
#if defined(WINBOLO_VOICE)
    /* The slot is empty, so nothing of the last occupant's may be carried
     * into it: slots are recycled, and a mute left behind would silence the
     * next joiner here (and their chat, server-side) while the panel showed
     * "muted by you" for a player this client never muted. Hooked here
     * because this is where both frontEndClearPlayer implementations that
     * build voice — desktop and wasm — converge. */
    voiceForgetPlayer((int)playerNum);
#endif
    s_playerName[playerNum][0] = '\0';
    s_playerCountry[playerNum][0] = '\0';
    s_playerEnabled[playerNum] = false;
    s_playerChecked[playerNum] = false;
    s_playerPing[playerNum] = 0;
    s_playerClientType[playerNum] = CLIENT_TYPE_UNKNOWN;
    s_playerFlags[playerNum] = 0;
}

void sdl3ImguiUpdatePlayerMeta(unsigned char playerNum, uint16_t ping,
                               uint8_t clientType, uint8_t clientFlags) {
    if (playerNum >= MAX_PLAYERS) return;
    s_playerPing[playerNum] = ping;
    s_playerClientType[playerNum] = clientType;
    s_playerFlags[playerNum] = clientFlags;
}

void sdl3ImguiUpdatePlayerFlags(unsigned char playerNum, uint8_t clientType,
                                uint8_t clientFlags) {
    if (playerNum >= MAX_PLAYERS) return;
    s_playerClientType[playerNum] = clientType;
    s_playerFlags[playerNum] = clientFlags;
}

void sdl3ImguiUpdatePlayerPing(unsigned char playerNum, uint16_t ping) {
    if (playerNum >= MAX_PLAYERS) return;
    s_playerPing[playerNum] = ping;
}

SDL_Texture *sdl3ImguiGetSteamIcon(void) {
    ensureWbnIconsLoaded();
    return s_iconSteam[activeIconSlot()];
}

SDL_Surface *sdl3ImguiGetBrainIconSurface(void) {
    /* Returns the larger rasterization — the only consumer is the
     * tank-label drawer (tank_label.c), which textures it per renderer
     * and scales it to the TTF label height; the 14-px popup texture
     * would alias badly at that size. renderPlayerName / the in-game
     * player menu read s_iconBrain directly. */
    ensureWbnIconsLoaded();
    return s_iconBrainSurf;
}

#if defined(WINBOLO_VOICE)
SDL_Surface *sdl3ImguiGetSpeakerIconSurface(void) {
    /* Same arrangement as the brain icon: the tank-label drawer is the only
     * consumer, so it gets the label-height rasterization rather than the
     * 14-px players-panel texture, as a surface it textures per renderer.
     * A speaker rather than a microphone because the label marks a player
     * whose voice is coming out of this client's speakers. */
    ensureWbnIconsLoaded();
    return s_iconSpeakerSurf;
}

SDL_Surface *sdl3ImguiCreateMicIconSurface(bool muted, int size) {
    /* Rasterized to order rather than cached: the game view's mute indicator
     * draws at a size that follows the window and the HUD's scale, and an icon
     * scaled at draw time loses the muted glyph's slash, whose gaps are about
     * a unit wide in the SVG's 24-unit viewBox. The caller owns what comes
     * back. */
    if (size < MIC_ICON_MIN_PX) size = MIC_ICON_MIN_PX;
    if (size > MIC_ICON_MAX_PX) size = MIC_ICON_MAX_PX;
    return imguiLoadSvgIconWhiteSurface(muted ? "data/ui/mic-muted.svg"
                                              : "data/ui/mic.svg",
                                        size);
}
#endif

bool sdl3ImguiPlayerIsBot(unsigned char playerNum) {
    if (playerNum >= MAX_PLAYERS) return false;
    return (s_playerFlags[playerNum] & PLAYER_FLAG_BOT) != 0;
}

SDL_Texture *sdl3ImguiGetPlatformIcon(uint8_t clientType) {
    ensurePlatformIconsLoaded();
    if (clientType >= CLIENT_TYPE_COUNT) return nullptr;
    return s_iconPlatform[activeIconSlot()][clientType];
}

/* Gold tint for supporters; white = no tint (passthrough). */
static const ImVec4 SUPPORTER_TINT = ImVec4(1.00f, 0.84f, 0.20f, 1.00f);
static const ImVec4 NO_TINT        = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);

static const char *platformName(uint8_t ct) {
    static const langid ids[CLIENT_TYPE_COUNT] = {
        0, STR_PLATFORM_WINDOWS, STR_PLATFORM_LINUX, STR_PLATFORM_MACOS,
        STR_PLATFORM_IOS, STR_PLATFORM_ANDROID, STR_PLATFORM_STEAMDECK,
        STR_PLATFORM_WEB
    };
    if (ct == 0 || ct >= CLIENT_TYPE_COUNT) return "";   /* CLIENT_TYPE_UNKNOWN -> no name */
    return langGetText(ids[ct]);
}

static int countryNameCmp(const void *key, const void *elem) {
    return strcmp((const char *)key, ((const CountryNameEntry *)elem)->code);
}

bool drawCountryFlagWithTip(const char *countryCode) {
    if (!countryCode || countryCode[0] == '\0' || countryCode[1] == '\0')
        return false;
    char up[3] = { (char)toupper((unsigned char)countryCode[0]),
                   (char)toupper((unsigned char)countryCode[1]), '\0' };
    if (up[0] == 'X' && up[1] == 'X') return false;        /* sentinel */
    SDL_Texture *flagTex = flagsGetTextureFor(activeRenderer(), countryCode);
    if (!flagTex) return false;
    ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
        const CountryNameEntry *e = (const CountryNameEntry *)bsearch(
            up, kCountryNames, K_COUNTRY_NAMES_SIZE,
            sizeof(kCountryNames[0]), countryNameCmp);
        if (e) ImGui::SetTooltip("%s", langGetText(e->id));
        else   ImGui::SetTooltip("%s", up);   /* fall back to uppercase code */
    }
    return true;
}

void renderPlayerName(const char *name, uint8_t flags, uint8_t clientType,
                      const char *countryCode, bool showCountry) {
    ensurePlatformIconsLoaded();
    ensureWbnIconsLoaded();
    const int iconSlot = activeIconSlot();
    if ((flags & PLAYER_FLAG_BOT) && s_iconBrain[iconSlot]) {
        /* Bot slot: brain icon stands in for the platform badge and the
         * WBN/Steam badges are skipped — a bot can never be either. */
        ImGui::Image((ImTextureID)s_iconBrain[iconSlot], ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
        imguiHelpTooltip(langGetText(STR_PLAYER_TIP_AI));
        ImGui::SameLine();
    } else {
        SDL_Texture *platTex = sdl3ImguiGetPlatformIcon(clientType);
        if (platTex) {
            ImVec4 tint = (flags & PLAYER_FLAG_SUPPORTER) ? SUPPORTER_TINT : NO_TINT;
            /* ImGui 1.91.9+ removed tint_col from Image(); ImageWithBg takes
             * (size, uv0, uv1, bg_col, tint_col) - bg transparent. */
            ImGui::ImageWithBg((ImTextureID)platTex,
                               ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE),
                               ImVec2(0, 0), ImVec2(1, 1),
                               ImVec4(0, 0, 0, 0), tint);
            if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
                const char *plat = platformName(clientType);
                if (flags & PLAYER_FLAG_SUPPORTER) {
                    MessageArgs args = {};
                    SDL_snprintf(args.string1, sizeof(args.string1), "%s", plat);
                    ImGui::SetTooltip("%s", langGetTextFmt(STR_PLAYER_TIP_SUPPORTER_FMT, &args));
                } else {
                    ImGui::SetTooltip("%s", plat);
                }
            }
            ImGui::SameLine();
        }

        if (flags & PLAYER_FLAG_WBN_VERIFIED) {
            /* Vector shield (crisp at this size); gold for supporters, white
             * otherwise — same scheme as the platform icon above. */
            ImVec4 tint = (flags & PLAYER_FLAG_SUPPORTER) ? SUPPORTER_TINT : NO_TINT;
            imguiShieldBadge(WBN_ICON_SIZE, ImGui::GetColorU32(tint));
            imguiHelpTooltip(langGetText(STR_PLAYER_TIP_WBN_VERIFIED));
            ImGui::SameLine();
        }
        if ((flags & (PLAYER_FLAG_WBN_STEAM_LINKED | PLAYER_FLAG_STEAM_BUILD)) && s_iconSteam[iconSlot]) {
            ImVec4 tint = (flags & PLAYER_FLAG_SUPPORTER) ? SUPPORTER_TINT : NO_TINT;
            ImGui::ImageWithBg((ImTextureID)s_iconSteam[iconSlot],
                               ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE),
                               ImVec2(0, 0), ImVec2(1, 1),
                               ImVec4(0, 0, 0, 0), tint);
            imguiHelpTooltip(langGetText((flags & PLAYER_FLAG_WBN_STEAM_LINKED)
                                         ? STR_PLAYER_TIP_STEAM_LINKED
                                         : STR_PLAYER_TIP_STEAM_BUILD));
            ImGui::SameLine();
        }
    }
    /* Icon-only mode: a NULL/empty name skips the text and trailing
     * country flag so callers can use this helper to render just the
     * WBN/Steam badges (the lobby table and players panel both render
     * the name via their own widgets — Selectable/TextColored — and
     * only need the badge sequence from here). */
    if (name && name[0] != '\0') {
        ImGui::TextUnformatted(name);
        if (showCountry && countryCode && countryCode[0] != '\0' &&
            !(countryCode[0] == 'X' && countryCode[1] == 'X') &&
            flagsGetTextureFor(activeRenderer(), countryCode)) {
            ImGui::SameLine();
            drawCountryFlagWithTip(countryCode);
        }
    }
}

#if defined(WINBOLO_VOICE)
/* Draws the speaker again over the bottom of itself, up to how loud that
 * player is right now: the dim glyph underneath is the shape, the talking
 * tint over the bottom of it is the level. Source rows and destination rows
 * are cut together, so the fill takes the speaker's own shape rather than
 * sitting over it as a rectangle — the same way the game view's own
 * microphone indicator draws its capture level.
 *
 * Call it straight after the widget that drew the icon: the rect comes from
 * that item, and this only adds to the draw list, so the cell is exactly the
 * size an unfilled icon is and the row cannot move.
 *
 * One fill and no peak band above it — voiceGetPlayerLevel already returns
 * the held peak, and at this icon size a band would be a pixel or two. The
 * game view has both because it draws at 32-43 px.
 *
 * Never runs on the local player's own row: the talking map carries no self
 * bit, so the caller's talking branch is unreachable there. */
static void micDrawLevelFill(SDL_Texture *micTex, int playerNum) {
    float level = voiceGetPlayerLevel(playerNum);
    if (level <= 0.0f) return;
    if (level > 1.0f) level = 1.0f;   /* over 1 would sample off the texture */

    ImVec2 mn  = ImGui::GetItemRectMin();
    ImVec2 mx  = ImGui::GetItemRectMax();
    float  top = mx.y - (mx.y - mn.y) * level;
    ImGui::GetWindowDrawList()->AddImage((ImTextureID)micTex,
                                         ImVec2(mn.x, top), mx,
                                         ImVec2(0.0f, 1.0f - level),
                                         ImVec2(1.0f, 1.0f),
                                         ImGui::GetColorU32(MIC_TINT_TALKING));
}

void renderPlayerMicCell(ClientSim *cs, int playerNum, uint8_t clientFlags,
                         PlayerBitMap talkingMap, bool isSelf, float size,
                         bool inLobby) {
    ensureWbnIconsLoaded();
    const int iconSlot = activeIconSlot();

    /* Resolved in precedence order: muting someone is this client's own
     * doing, so it outranks whatever their microphone is doing — you have to
     * be able to see that you muted them, and to undo it, whatever their
     * state. */
    bool mutedByMe = voiceIsPlayerMuted(playerNum);
    bool hasMic    = (clientFlags & PLAYER_FLAG_HAS_MIC) != 0;
    bool selfMuted = (clientFlags & PLAYER_FLAG_VOICE_MUTED) != 0;
    bool talking   = (talkingMap & ((PlayerBitMap)1u << playerNum)) != 0;

    /* The own row renders from local truth: s_playerFlags[] is only ever
       written from server-published state, so a click would not move the icon
       until it round-tripped, and never at all on a connection that carries no
       voice. */
    if (isSelf) selfMuted = voiceIsSelfMuted();

    if (!inLobby && !isSelf && !hasMic && !mutedByMe) {
        /* In game, a remote player who has no microphone is not worth a
         * glyph — the state never changes and the row is read at a glance.
         * The cell still holds its width, or the name and ping shift
         * between rows. Muted by this client is checked first and still
         * draws, so a player you muted stays visible and clickable. */
        ImGui::Dummy(ImVec2(size, size));
        return;
    }

    /* The shape says which end the state belongs to. A speaker for the
     * states about playback here — idle, talking, and muted by this client,
     * which is what the click changes — and a microphone for the two about
     * capture at the other end. Every state on the own row is about this
     * client's own capture, so that row stays on microphones. */
    SDL_Texture *micTex;
    ImVec4       micTint;
    langid       micTip;
    bool         micLevelFill = false;
    if (mutedByMe) {
        micTex  = s_iconSpeakerMuted[iconSlot];
        micTint = MIC_TINT_MUTED;
        micTip  = STR_PLAYER_TIP_VOICE_MUTEDBYYOU;
    } else if (!hasMic) {
        micTex  = s_iconMicOff[iconSlot];
        micTint = MIC_TINT_DIM;
        micTip  = isSelf ? STR_PLAYER_TIP_VOICE_SELF_NOMIC
                         : STR_PLAYER_TIP_VOICE_NOMIC;
    } else if (talking) {
        /* Dim, because the fill drawn over it carries the talking colour:
         * the glyph is the meter, as it is in the game view's own microphone
         * indicator. */
        micTex       = s_iconSpeaker[iconSlot];
        micTint      = MIC_TINT_DIM;
        micTip       = STR_PLAYER_TIP_VOICE_TALKING;
        micLevelFill = true;
    } else if (selfMuted) {
        /* The barred microphone, not the barred speaker: the far end
         * stopped sending, which is theirs to undo, where the barred
         * speaker means this client stopped listening. */
        micTex  = s_iconMicMuted[iconSlot];
        micTint = MIC_TINT_DIM;
        micTip  = isSelf ? STR_PLAYER_TIP_VOICE_SELF_MUTED
                         : STR_PLAYER_TIP_VOICE_SELFMUTED;
    } else {
        micTex  = isSelf ? s_iconMic[iconSlot] : s_iconSpeaker[iconSlot];
        micTint = isSelf ? MIC_TINT_NORMAL : MIC_TINT_DIM;
        micTip  = isSelf ? STR_PLAYER_TIP_VOICE_SELF
                         : STR_PLAYER_TIP_VOICE_IDLE;
    }

    if (!micTex) {
        /* An SVG that would not load must still hold the column, or
         * the name and ping shift between rows. */
        ImGui::Dummy(ImVec2(size, size));
    } else if (isSelf && !hasMic) {
        /* Not clickable without a microphone: there is nothing to gate.
         * The clickable branch below toggles a local transmit gate, not
         * the mute the server rejects against yourself. */
        ImGui::ImageWithBg((ImTextureID)micTex, ImVec2(size, size),
                           ImVec2(0, 0), ImVec2(1, 1),
                           ImVec4(0, 0, 0, 0), micTint);
        if (micLevelFill) micDrawLevelFill(micTex, playerNum);
        imguiHelpTooltip(langGetText(micTip));
    } else {
        char micLabel[64];
        snprintf(micLabel, sizeof(micLabel), "##mic%d", playerNum);
        /* Zero FramePadding so the button is exactly the icon: the
         * default padding would make this cell taller than the
         * Selectable beside it and leave a dead strip in the row. */
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.15f));
        bool micClicked = ImGui::ImageButton(micLabel, (ImTextureID)micTex,
                                             ImVec2(size, size),
                                             ImVec2(0, 0), ImVec2(1, 1),
                                             ImVec4(0, 0, 0, 0), micTint);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        /* Still the button's rect: the pops above changed no item. */
        if (micLevelFill) micDrawLevelFill(micTex, playerNum);
        imguiHelpTooltip(langGetText(micTip));
        imguiHandOnHover();
        if (micClicked) {
            if (isSelf) {
                /* A local transmit gate — no server round trip, and the
                 * server would reject a mute against yourself anyway.
                 * The state reaches the other players on the next tick,
                 * from the report voice already publishes on change. */
                voiceSetSelfMuted(!voiceIsSelfMuted());
            } else {
                /* Both legs, always: the local one covers the round trip
                 * while the server is being told, and the server is the
                 * authority — it also stops that player's chat. */
                voiceSetPlayerMuted(playerNum, !voiceIsPlayerMuted(playerNum));
                clientSimNetSendPlayerMute(cs, (BYTE)playerNum, voiceIsPlayerMuted(playerNum));
            }
        }
    }
}
#endif

void sdl3ImguiSetPlayerCheckState(unsigned char playerNum, bool isChecked) {
    if (playerNum >= MAX_PLAYERS) return;
    s_playerChecked[playerNum] = isChecked;
}

void sdl3ImguiShowKeySetup(void) {
    /* Hands off to imgui_keysetup.cpp's in-game popup wrapper. */
    imguiKeySetupOpenInGame();
}

void sdl3ImguiCleanup(void) {
    if (!s_window) return;
    /* Runs on return-to-lobby, end-of-game and process exit, so it is the last
       chance to drop the in-window map view: neither the lobby nor the next
       game should inherit it, or a pointer still stuck on the game crosshair.
       Dropping the view is unconditional; the window state that follows is
       overviewInWindowSet's call, and stays full screen while app full screen
       is on. */
    overviewInWindowSet(false);
    /* And the edge that brings it back has to be rearmed with it. A return to
       the lobby keeps the ClientSim — winbolo.c skips the teardown on that
       path — so clientSimIsRunning never goes false and the per-frame test
       above never sees the not-running-then-running edge that reopens the map.
       Left latched, the second game of a session came up windowed-view inside
       a still-full-screen window, and the pop-out did not come back either. */
    s_overviewWasRunning = false;
    inputGamepadShutdown();
    /* Before the loop: all of these were made on the Map Overview pop-out's
       renderer, which popOutDestroy tears down. */
    overviewViewDestroy(s_overviewView);
    s_overviewView = nullptr;
    overviewSnapshotDestroy(s_overviewSnapshot);
    s_overviewSnapshot = nullptr;
    if (s_overviewTiles) {
        SDL_DestroyTexture(s_overviewTiles);
        s_overviewTiles = nullptr;
    }
    s_overviewTilesRenderer = nullptr;
    s_overviewTilesScale    = 0;
    if (s_overviewCrosshair) {
        SDL_DestroyTexture(s_overviewCrosshair);
        s_overviewCrosshair = nullptr;
    }
    s_overviewCrosshairRenderer = nullptr;
    /* Textures die with the renderer that made them, so the pop-out's copies
     * and its flag cache go before popOutDestroy takes its renderer down —
     * SDL_DestroyTexture afterwards would be running against freed state. */
    destroyIconSlot(ICON_SLOT_POPOUT);
    flagsDestroy();
    for (int i = 0; i < POPOUT_COUNT; i++) popOutDestroy(s_popOuts[i]);
    destroyIconSlot(ICON_SLOT_MAIN);
    /* Renderer-free, so they outlive both slots and are freed once here. */
    if (s_iconBrainSurf) { SDL_DestroySurface(s_iconBrainSurf); s_iconBrainSurf = nullptr; }
#if defined(WINBOLO_VOICE)
    if (s_iconSpeakerSurf) { SDL_DestroySurface(s_iconSpeakerSurf); s_iconSpeakerSurf = nullptr; }
#endif
    luaBrainFreeSettings(s_brainSettings);
    s_brainSettings      = nullptr;
    s_brainSettingsCount = 0;
#ifdef _WIN32
    if (s_snapPopup) { DestroyWindow(s_snapPopup); s_snapPopup = NULL; }
    if (s_snapFont) { DeleteObject(s_snapFont); s_snapFont = NULL; }
#endif
    /* Guard against the ImGui context already being destroyed.
     * The welcome dialog (imguiWelcomeShow) creates and tears down its own
     * ImGui context, so by the time we get here the context set up in
     * sdl3ImguiSetup may already be gone. */
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    s_window   = nullptr;
    s_renderer = nullptr;
}
