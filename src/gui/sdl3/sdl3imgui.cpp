/*
 * Copyright (c) 1998-2008 John Morrison.
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
#include "../../bolo/global.h"    /* BYTE, bool, FALSE/TRUE */
#include "../../bolo/screen.h"   /* labelLen, lblNone/lblShort/lblLong */
#include "../../bolo/client_sim.h"
#include "../../bolo/netpacks.h" /* PACKET_MAX_CHAT_MESSAGE */
#include "../../steam/steam_wrapper.h"
#include "../../steam/steam_input_actions.h"
#include "../gamefront.h"
#include "../lang.h"
}

/* Our own header */
#include "sdl3imgui.h"
#include "sdl3draw.h"
#include "luabrainshandler.h"
#include "flags.h"
#include "glyphs.h"
#include "dialogs/imgui_keycap.h"

/* Include players.h with C linkage — no #pragma pack inside, safe here */
extern "C" {
#include "../../bolo/players.h"
#include "../../bolo/transport.h"
#include "../../bolo/transport_udp.h"
#include "../../bolo/bot_manager.h"
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
}

#include "sdl3imgui_tablet.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "dialogs/imgui_dialog_utils.h"
#include "dialogs/imgui_deck_pause.h"
#include "dialogs/imgui_quickchat.h"
#include "dialogs/imgui_controller_prompt.h"
#include "imgui_steam_nav.h"

extern "C" void windowSetQuitting(void);

/* Network type enum values come from bolo_packets.h via client_sim.h */

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
/* gameFrontGetTransport: now provided by gamefront.h */
extern "C" uint16_t transportUdpClientGetPing(Transport *t);
/* Dialog helpers — declared without pulling in pragma-pack headers */
extern "C" void screenGetPlayerNameCS(struct ClientSim *cs, char *dest);
extern "C" bool screenSetPlayerNameCS(struct ClientSim *cs, char *name);
/* gameFrontSetGameOptions: now provided by gamefront.h */
extern "C" void utilStripName(char *name);

/* Key setup helpers */
extern "C" void windowGetKeys(keyItems *value);
extern "C" void windowSetKeys(keyItems *value);
extern "C" void windowKeyPressed(struct ClientSim *cs, int keyCode);
extern "C" bool screenGetTankAutoSlowdownCS(struct ClientSim *csPtr);
extern "C" void screenSetTankAutoSlowdownCS(struct ClientSim *csPtr, bool useSlowdown);
extern "C" bool screenGetTankAutoHideGunsightCS(struct ClientSim *csPtr);
extern "C" void screenSetTankAutoHideGunsightCS(struct ClientSim *csPtr, bool useAutohide);
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
#define MENU_BAR_HEIGHT 22
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
extern "C" void windowMenuAllowNewPlayers_toggle(struct ClientSim *cs);
extern "C" void windowMenuNewswire_toggle(struct ClientSim *cs);
extern "C" void windowMenuAssistant_toggle(struct ClientSim *cs);
extern "C" void windowMenuAI_toggle(struct ClientSim *cs);
extern "C" void windowMenuNetwork_toggle(struct ClientSim *cs);
extern "C" void windowMenuNetworkDebug_toggle(struct ClientSim *cs);
extern "C" void windowHideMainView_toggle(void);
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
extern "C" void windowSaveMap(struct ClientSim *cs);
extern "C" void windowSuspendBackground(void);
extern "C" void windowResumeForeground(struct ClientSim *cs);

/* Backend functions called directly from menu */
extern "C" void screenRequestAllianceCS(struct ClientSim *csPtr);
extern "C" void screenLeaveAllianceCS(struct ClientSim *csPtr);
extern "C" void screenCheckAllNonePlayersCS(struct ClientSim *csPtr, bool isChecked);
extern "C" void screenCheckAlliedPlayersCS(struct ClientSim *csPtr);
extern "C" void screenCheckNearbyPlayersCS(struct ClientSim *csPtr);
extern "C" void screenTogglePlayerCheckStateCS(struct ClientSim *csPtr, BYTE playerNum);

extern "C" bool showGunsight;
extern "C" bool autoScrollingEnabled;
extern "C" bool smoothScrollingEnabled;
extern "C" bool showPillLabels;
extern "C" bool showBaseLabels;
extern "C" bool hideMainView;
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
extern "C" bool showNewswireMessages;
extern "C" bool showAssistantMessages;
extern "C" bool showAIMessages;
extern "C" bool showNetworkStatusMessages;
extern "C" bool showNetworkDebugMessages;

/* Device presets are defined in imgui_dialog_utils.h (shared with dialogs) */
#include "dialogs/imgui_dialog_utils.h"

/* -------------------------------------------------------
 * Module state
 * ------------------------------------------------------- */
static SDL_Window   *s_window   = nullptr;
static SDL_Renderer *s_renderer = nullptr;

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

/* Deferred zoom change — windowZoomChange destroys the ImGui context, so we
   must not call it mid-frame.  Store the requested value and apply it after
   the frame ends. 255 = no pending change. */
static BYTE s_pendingZoom = 255;
static bool s_pendingZoomFromResize = false;  /* True if zoom change came from resize snap */

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

/* Player slot state — updated by frontEndSetPlayer / frontEndClearPlayer */
#define MAX_PLAYERS 16
static char     s_playerName[MAX_PLAYERS][33] = {};        /* PLAYER_NAME_LEN = 33 */
static char     s_playerCountry[MAX_PLAYERS][3] = {};      /* 2-char ISO country code + NUL */
static bool     s_playerEnabled[MAX_PLAYERS]  = {};
static bool     s_playerChecked[MAX_PLAYERS]  = {};
static uint16_t s_playerPing[MAX_PLAYERS] = {};
static uint8_t  s_playerClientType[MAX_PLAYERS] = {};
static uint8_t  s_playerFlags[MAX_PLAYERS] = {};

/* WBN/Steam icon textures */
static SDL_Texture *s_iconGlobe = nullptr;
static SDL_Texture *s_iconSteam = nullptr;
static bool s_wbnIconsLoaded = false;
#define WBN_ICON_SIZE 14

static void ensureWbnIconsLoaded(void) {
    if (s_wbnIconsLoaded) return;
    s_wbnIconsLoaded = true;
    SDL_Renderer *r = s_renderer ? s_renderer : sdl3DrawGetRenderer();
    s_iconGlobe = imguiLoadSvgIcon(r, "data/ui/globe.svg", WBN_ICON_SIZE);
    s_iconSteam = imguiLoadSvgIcon(r, "data/ui/steam.svg", WBN_ICON_SIZE);
    WB_LOG_DEBUG(WB_LOG_CAT_GUI, "[WBN ICONS] globe=%p steam=%p s_renderer=%p drawRenderer=%p",
            (void *)s_iconGlobe, (void *)s_iconSteam,
            (void *)s_renderer, (void *)sdl3DrawGetRenderer());
}

/* Platform icon textures, indexed by ClientType. UNKNOWN slot stays NULL. */
static SDL_Texture *s_iconPlatform[CLIENT_TYPE_COUNT] = {};
static bool s_platformIconsLoaded = false;

static void ensurePlatformIconsLoaded(void) {
    if (s_platformIconsLoaded) return;
    s_platformIconsLoaded = true;
    SDL_Renderer *r = s_renderer ? s_renderer : sdl3DrawGetRenderer();
    /* Force white so platform icons read against the dark ImGui background
     * regardless of each SVG's authored fill (mac.svg=#888, windows.svg=#000…). */
    s_iconPlatform[CLIENT_TYPE_UNKNOWN]   = nullptr;
    s_iconPlatform[CLIENT_TYPE_WINDOWS]   = imguiLoadSvgIconWhite(r, "data/ui/windows.svg",    WBN_ICON_SIZE);
    s_iconPlatform[CLIENT_TYPE_LINUX]     = imguiLoadSvgIconWhite(r, "data/ui/linux.svg",      WBN_ICON_SIZE);
    s_iconPlatform[CLIENT_TYPE_MACOS]     = imguiLoadSvgIconWhite(r, "data/ui/mac.svg",        WBN_ICON_SIZE);
    s_iconPlatform[CLIENT_TYPE_IOS]       = imguiLoadSvgIconWhite(r, "data/ui/ios.svg",        WBN_ICON_SIZE);
    s_iconPlatform[CLIENT_TYPE_ANDROID]   = imguiLoadSvgIconWhite(r, "data/ui/android.svg",    WBN_ICON_SIZE);
    s_iconPlatform[CLIENT_TYPE_STEAMDECK] = imguiLoadSvgIconWhite(r, "data/ui/steam-deck.svg", WBN_ICON_SIZE);
    s_iconPlatform[CLIENT_TYPE_WEB]       = imguiLoadSvgIconWhite(r, "data/ui/globe.svg",      WBN_ICON_SIZE);
}

/* Settings panel state */
static bool s_showSettings       = false;
static char s_settingsNameBuf[33] = "";  /* PLAYER_NAME_LEN = 33 */
static bool s_wbnInitialised     = false;

/* Modal dialog state */
static bool s_showAbout          = false;
static bool s_closeAllPopups     = false;

static bool s_showChangeName     = false;
static char s_changeNameBuf[33]  = "";  /* PLAYER_NAME_LEN = 33 */

static bool s_showAllianceOpen   = false;
static char s_alliancePlayerName[33] = "";
static BYTE s_alliancePlayerNum  = 0;

static bool s_showPasswordOpen   = false;
static char s_passwordBuf[36]    = "";  /* MAP_STR_SIZE = 36 */

/* "Join Game?" confirmation when a winbolo:// URL is received mid-game */
static bool s_showJoinConfirm       = false;
static char s_joinConfirmUrl[512]   = "";
static char s_joinConfirmAddr[256]  = "";
static int  s_joinConfirmPort       = 0;

/* Key Setup modal state */
/* Which binding is currently being captured; -1 = none */
enum KeySetupField {
    ksNone = -1,
    ksForward, ksBackward, ksTurnLeft, ksTurnRight,
    ksShoot, ksLayMine, ksGunIncrease, ksGunDecrease,
    ksTankView, ksPillView,
    ksScrollUp, ksScrollDown, ksScrollLeft, ksScrollRight,
    ksQuickTree, ksQuickRoad, ksQuickWall, ksQuickPillbox, ksQuickMine,
};

static bool         s_showKeySetup        = false;
static keyItems     s_keySetupKeys;          /* working copy */
static bool         s_keySetupAutoSlowdown  = false;
static bool         s_keySetupAutoGunsight  = false;
static KeySetupField s_keySetupWaiting      = ksNone;

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
 * transportUdpClientSendChat / transport_udp_server PACKET_CHAT_MESSAGE,
 * + 1 for NUL. ImGui's InputText caps insertions at sizeof(buf) and
 * rejects a whole UTF-8 codepoint that would overflow rather than
 * splitting it, so this is the limit users see in the dialog too. */
static char   s_sendMsgBuf[PACKET_MAX_CHAT_MESSAGE + 1] = "";
static Uint64 s_sendMsgCooldownEnd = 0;   /* SDL_GetTicks() value; 0 = not in cooldown */
static bool   s_sendMsgFocusInput = false; /* Set true to focus the text input next frame */
#define SEND_MSG_WAIT_MS 2000

/* Alliance request cooldown */
static Uint64 s_allianceReqCooldownEnd = 0; /* SDL_GetTicks() value; 0 = not in cooldown */
#define ALLIANCE_REQ_WAIT_MS 5000

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

static PopOutWindow s_popSysInfo  = {};
static PopOutWindow s_popNetInfo  = {};
static PopOutWindow s_popGameInfo = {};
static PopOutWindow s_popSendMsg  = {};
static ImGuiContext *s_mainImguiCtx = nullptr;

static bool popOutCreate(PopOutWindow *pw, const char *title, int w, int h) {
    pw->window = SDL_CreateWindow(title, w, h, 0);
    if (!pw->window) return false;

#ifdef _WIN32
    /* Remove minimize/maximize buttons — leave only the close box */
    {
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

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
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

static bool popOutBeginFrame(PopOutWindow *pw) {
    if (!pw->open || !pw->window) return false;

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
    ImGui::EndFrame();
    ImGui::Render();
    SDL_SetRenderDrawColor(pw->renderer, 30, 30, 30, 255);
    SDL_RenderClear(pw->renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), pw->renderer);
    SDL_RenderPresent(pw->renderer);
}

static void togglePopOut(PopOutWindow *pw, const char *title, int w, int h) {
    if (pw->open) {
        popOutDestroy(pw);
    } else {
        popOutCreate(pw, title, w, h);
    }
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
        hasBots = botManagerHasAnyBot();
        botManagerGetPoolStats(&ps);
        for (int i = 0; i < MAX_TANKS; i++) {
            botInfoValid[i] = botManagerGetBotInfo((BYTE)i, &botInfos[i]);
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

    ImGui::SetNextWindowSize(ImVec2(440, 600), ImGuiCond_FirstUseEver);
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

    netGetServerAddressStr(cs, str);
    ImGui::Text("%s %s", langGetText(STR_DLGNETINFO_SERVER), str);

    /* Client in a networked game: prepend player location to port */
    if (cs->networkGameType != netSingle) {
        char addr[256];
        players *plrs = &cs->sim.plyrs;
        playersGetPlayerLocation(plrs, cs->myPlayerNum, addr);
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
    {
        Transport *tp = gameFrontGetTransport();
        if (tp) {
            uint16_t udpPing = transportUdpClientGetPing(tp);
            if (udpPing > 0) ping = (int)udpPing;
            int udpErrors = 0;
            transportUdpClientGetNetStats(tp, &ppsIn, &ppsOut, &bpsIn, &bpsOut, &udpErrors);
            numErrors = udpErrors;
        }
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
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGNETINFO_ERRORS, &args));
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

    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
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
    screenGetMapNameCS(cs, mapName);
    ImGui::Text("%s%s", langGetText(STR_DLGGAMEINFO_MAPNAME), mapName);
    if (strncmp(mapName, "rand_", 5) == 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(langGetText(STR_DLGGAMEINFO_COPYSEED))) {
            SDL_SetClipboardText(mapName + 5);
        }
    }
    {
        MessageArgs args = {};
        args.number = (int)screenGetNumPlayersCS(cs);
        ImGui::TextUnformatted(langGetTextFmt(STR_DLGGAMEINFO_NUMPLAYERS, &args));
    }

    gameType *gt = &cs->sim.game;
    langid gtStr = STR_DLGGAMEINFO_STRICT;
    if      (*gt == gameOpen)       gtStr = STR_DLGGAMEINFO_OPEN;
    else if (*gt == gameTournament) gtStr = STR_DLGGAMEINFO_TOURN;
    ImGui::Text("%s%s", langGetText(STR_DLGGAMEINFO_GAMETYPE), langGetText(gtStr));

    ImGui::Text("%s%s", langGetText(STR_DLGGAMEINFO_HIDDENMINES),
                screenGetAllowHiddenMinesCS(cs) ? langGetText(STR_YES) : langGetText(STR_NO));

    aiType ai = screenGetAiTypeCS(cs);
    langid aiStr = STR_NO;
    if      (ai == aiYes)          aiStr = STR_YES;
    else if (ai == aiYesAdvantage) aiStr = STR_DLGGAMEINFO_AIADV;
    else if (ai == aiFull)         aiStr = STR_DLGGAMEINFO_FULLADV;
    ImGui::Text("%s %s", langGetText(STR_DLGGAMEINFO_AILABEL), langGetText(aiStr));

    long timeLeft = screenGetGameTimeLeftCS(cs);
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
}

static void renderGameInfoPanel(ClientSim *cs) {
    if (!s_showGameInfo || s_popGameInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(320, 200), ImGuiCond_FirstUseEver);
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
        case kSendAll:      numSend = (int)screenGetNumPlayersCS(cs);     break;
        case kSendAllies:   numSend = screenNumAlliesCS(cs);              break;
        case kSendNearby:   numSend = screenNumNearbyTanksCS(cs);         break;
        case kSendSelected: numSend = screenNumCheckedPlayersCS(cs);      break;
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
    if (ImGui::IsWindowAppearing() || s_sendMsgFocusInput) {
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere(0);
        wantSelectAll = true;
        s_sendMsgFocusInput = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    bool pressedEnter = ImGui::InputText("##msg", s_sendMsgBuf, sizeof(s_sendMsgBuf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (wantSelectAll) {
        if (ImGuiInputTextState *state = ImGui::GetInputTextState(ImGui::GetItemID()))
            state->SelectAll();
    }

    /* Send button with cooldown */
    bool inCooldown = (s_sendMsgCooldownEnd != 0 &&
                       SDL_GetTicks() < s_sendMsgCooldownEnd);
    if (inCooldown) ImGui::BeginDisabled();
    bool doSend = ImGui::Button(langGetText(STR_DLGMSG_BUTTON)) || (!inCooldown && pressedEnter);
    if (inCooldown) ImGui::EndDisabled();

    if (doSend && s_sendMsgBuf[0] != '\0') {
        switch (s_sendMsgRecipient) {
            case kSendAll:      screenSendMessageAllPlayersCS(cs, s_sendMsgBuf);  break;
            case kSendAllies:   screenSendMessageAllAlliesCS(cs, s_sendMsgBuf);   break;
            case kSendNearby:   screenSendMessageAllNearbyCS(cs, s_sendMsgBuf);   break;
            case kSendSelected: screenSendMessageAllSelectedCS(cs, s_sendMsgBuf); break;
        }
        s_sendMsgCooldownEnd = SDL_GetTicks() + SEND_MSG_WAIT_MS;
#if BOLO_MOBILE
        /* On mobile, close the dialog after sending via Enter */
        if (pressedEnter) {
            s_showSendMsg = false;
            dialogDismissKeyboard(s_window);
        }
#else
        /* Re-focus the input and select all so the user can type to
         * overwrite the previous message immediately after cooldown. */
        s_sendMsgFocusInput = true;
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
        ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
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

/* -------------------------------------------------------
 * Players panel (standalone window for tablet mode)
 * ------------------------------------------------------- */

static void renderPlayersPanel(ClientSim *cs) {
    if (!s_showPlayersPanel) return;

    if (uiModeIsTablet()) {
        ImGuiIO &io = ImGui::GetIO();
        float w = io.DisplaySize.x * 0.8f;
        float h = io.DisplaySize.y * 0.8f;
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    } else {
        ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
    }
    bool *pOpen = uiModeIsTablet() ? nullptr : &s_showPlayersPanel;
    ImGuiWindowFlags flags = uiModeIsTablet() ? (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse) : 0;
    char title[128];
    snprintf(title, sizeof(title), "%s###playerspanel", langGetText(STR_DLGPLAYERS_TITLE));
    if (!ImGui::Begin(title, pOpen, flags)) {
        ImGui::End();
        return;
    }

    /* Selection helpers */
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALL)))    screenCheckAllNonePlayersCS(cs, true);
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NONE)))   screenCheckAllNonePlayersCS(cs, false);
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_ALLIES))) screenCheckAlliedPlayersCS(cs);
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGPLAYERS_NEARBY))) screenCheckNearbyPlayersCS(cs);

    ImGui::Separator();

    /* Pre-compute alliance state */
    players *plrs = &cs->sim.plyrs;
    BYTE self = cs->myPlayerNum;
    bool hasAllies  = false;
    bool canRequest = false;
    bool isAlly[MAX_PLAYERS] = {};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (s_playerEnabled[i] && i != self) {
            isAlly[i] = playersIsAllie(plrs, self, (BYTE)i);
            if (isAlly[i]) hasAllies = true;
            else if (s_playerChecked[i]) canRequest = true;
        }
    }

    /* Collect enabled player indices */
    int enabledPlayers[MAX_PLAYERS];
    int enabledCount = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (s_playerEnabled[i]) {
            enabledPlayers[enabledCount++] = i;
        }
    }

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

        /* Flag icon */
        if (s_playerCountry[i][0] != '\0') {
            SDL_Texture *flagTex = flagsGetTexture(s_playerCountry[i]);
            if (flagTex) {
                ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                ImGui::SameLine();
            }
        }

        /* Platform / WBN / Steam icons */
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

        /* Checkbox + selectable name */
        if (i != self) {
            char checkLabel[64];
            snprintf(checkLabel, sizeof(checkLabel), "##chk%d", i);
            bool checked = s_playerChecked[i];
            if (ImGui::Checkbox(checkLabel, &checked)) {
                screenTogglePlayerCheckStateCS(cs, (BYTE)i);
            }
            ImGui::SameLine();
        }

        char selectLabel[64];
        snprintf(selectLabel, sizeof(selectLabel), "%s##psel%d", label, i);
        if (ImGui::Selectable(selectLabel, s_playerChecked[i],
                              ImGuiSelectableFlags_DontClosePopups,
                              ImVec2(fullWidth - pingWidth - spacing -
                                     (i != self ? ImGui::GetFrameHeight() + spacing : 0), 0))) {
            if (i != self) screenTogglePlayerCheckStateCS(cs, (BYTE)i);
        }

        /* Right-aligned ping */
        ImGui::SameLine(fullWidth - pingWidth);
        ImVec4 pingColor;
        if (s_playerPing[i] == 0)        pingColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
        else if (s_playerPing[i] < 50)   pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
        else if (s_playerPing[i] < 150)  pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
        else                              pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, pingColor);
        ImGui::TextUnformatted(pingStr);
        ImGui::PopStyleColor();
    };

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
        for (int idx = 0; idx < enabledCount; idx++)
            renderPlayerRow(enabledPlayers[idx]);
    }

    /* Alliance actions */
    ImGui::Separator();
    {
        bool inCooldown = (s_allianceReqCooldownEnd != 0 &&
                           SDL_GetTicks() < s_allianceReqCooldownEnd);
        if (hasAllies) {
            if (ImGui::Button(langGetText(STR_LEAVE_ALLIANCE), ImVec2(-1, 0)))
                screenLeaveAllianceCS(cs);
        } else {
            if (!canRequest || inCooldown) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_REQUEST_ALLIANCE), ImVec2(-1, 0))) {
                screenRequestAllianceCS(cs);
                s_allianceReqCooldownEnd = SDL_GetTicks() + ALLIANCE_REQ_WAIT_MS;
            }
            if (!canRequest || inCooldown) ImGui::EndDisabled();
        }
    }

    /* Allow new players toggle */
    {
        bool anp = (bool)allowNewPlayers;
        if (ImGui::Checkbox(langGetText(STR_ALLOW_NEW_PLAYERS), &anp))
            windowMenuAllowNewPlayers_toggle(cs);
    }

    ImGui::End();
}

/* -------------------------------------------------------
 * About modal
 * ------------------------------------------------------- */
static void renderAboutModal(void) {
    char title[128];
    snprintf(title, sizeof(title), "%s###about", langGetText(STR_DLGABOUT_TITLE));
    if (s_showAbout) {
        ImGui::OpenPopup(title);
        s_showAbout = false;
    }
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted(langGetText(STR_DLGABOUT_VERSION));
        ImGui::TextUnformatted(langGetText(STR_DLGABOUT_COPYRIGHT));
        ImGui::Separator();
        ImGui::TextDisabled("%s", langGetText(STR_DLGABOUT_BOLOCOPYRIGHT));
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_OK), ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

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
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted(langGetText(STR_DLGJOIN_BLURB));
        ImGui::Spacing();
        if (s_joinConfirmPort > 0) {
            ImGui::Text("%s:%d", s_joinConfirmAddr, s_joinConfirmPort);
        } else {
            ImGui::TextUnformatted(s_joinConfirmAddr);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_DLGJOIN_BUTTON), ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
            /* Leave current game and return to menu with the URL queued */
            gameFrontHandleUrlOpen(s_joinConfirmUrl);
            windowNewGame();
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(80, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
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
        screenGetPlayerNameCS(cs, s_changeNameBuf);
    }
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted(langGetText(STR_DLGSETNAME_BLURB));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        ImGui::SetNextItemWidth(300);
        bool enter = ImGui::InputText("##name", s_changeNameBuf,
                                      sizeof(s_changeNameBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        bool doOK     = ImGui::Button(langGetText(STR_OK),     ImVec2(120, 0)) || enter;
        ImGui::SameLine();
        bool doCancel = ImGui::Button(langGetText(STR_CANCEL), ImVec2(120, 0));

        if (doOK) {
            s_changeNameBuf[32] = '\0'; /* PLAYER_NAME_LAST - 1 */
            utilStripName(s_changeNameBuf);
            if (s_changeNameBuf[0] == '\0') {
                /* blank — stay open */
            } else if (s_changeNameBuf[0] == '*') {
                /* invalid — stay open */
            } else {
                if (screenSetPlayerNameCS(cs, s_changeNameBuf))
                    ImGui::CloseCurrentPopup();
                /* else: name in use — stay open */
            }
        }
        if (doCancel) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* -------------------------------------------------------
 * Alliance Request modal
 * ------------------------------------------------------- */
static bool s_allianceVisible = false;

static void renderAllianceRequest(ClientSim *cs) {
    if (s_showAllianceOpen) {
        s_allianceVisible = true;
        s_showAllianceOpen = false;
    }
    if (!s_allianceVisible) return;

    /* Pin to the top of the status panel (right of the main game view).
     * MAIN_OFFSET_X=81, MAIN_SCREEN_SIZE_X=15, TILE_SIZE_X=16  =>  321 px at zoom 1 */
    float menuH      = ImGui::GetFrameHeight();
    float statusLeft = (float)(zoomFactor * 321);
    ImGui::SetNextWindowPos(ImVec2(statusLeft, menuH), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_Always);
    char title[128];
    snprintf(title, sizeof(title), "%s###alliancereq", langGetText(STR_DLGALLIANCE_TITLE));
    if (ImGui::Begin(title, &s_allianceVisible,
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse)) {
        {
            MessageArgs args = {};
            strncpy(args.playerName, s_alliancePlayerName, sizeof(args.playerName) - 1);
            args.playerFlags = playersGetAccountFlags(&cs->sim.plyrs, s_alliancePlayerNum);
            playersGetCountryCode(&cs->sim.plyrs, s_alliancePlayerNum, args.playerCountry);
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGALLIANCE_BLURB, &args));
        }
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_DLGALLIANCE_ACCEPT), ImVec2(120, 0))) {
            clientSimAllianceAccept(cs, s_alliancePlayerNum);
            s_allianceVisible = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_DLGALLIANCE_DECLINE), ImVec2(120, 0)))
            s_allianceVisible = false;
    }
    ImGui::End();
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
    if (ImGui::BeginPopupModal(title, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted(langGetText(STR_DLGPASSWORD_BLURB));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        ImGui::SetNextItemWidth(270);
        bool enter = ImGui::InputText("##pass", s_passwordBuf,
                                      sizeof(s_passwordBuf),
                                      ImGuiInputTextFlags_Password |
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_OK), ImVec2(120, 0)) || enter) {
            /* gameOpen=1, aiNone=0, justPass=TRUE */
            gameFrontSetGameOptions(s_passwordBuf, (gameType)1, false, (aiType)0, 0, 0, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* -------------------------------------------------------
 * Key Setup modal
 * Mirrors dialogKeySetup.c — allows the user to rebind
 * all game keys.  Key capture is driven by the SDL event
 * loop in sdl3ImguiProcessEvents().
 * ------------------------------------------------------- */

/* Return a human-readable name for a scancode. */
static const char *keySetupScancodeLabel(int scancode) {
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    if (name && name[0] != '\0') return name;
    return langGetText(STR_DLGKEYSETUP_NONE_VAL);
}

/* Map a KeySetupField to the corresponding keyItems member and label. */
static int *keySetupFieldPtr(KeySetupField f, keyItems *ki) {
    switch (f) {
        case ksForward:    return &ki->kiForward;
        case ksBackward:   return &ki->kiBackward;
        case ksTurnLeft:   return &ki->kiLeft;
        case ksTurnRight:  return &ki->kiRight;
        case ksShoot:      return &ki->kiShoot;
        case ksLayMine:    return &ki->kiLayMine;
        case ksGunIncrease:return &ki->kiGunIncrease;
        case ksGunDecrease:return &ki->kiGunDecrease;
        case ksTankView:   return &ki->kiTankView;
        case ksPillView:   return &ki->kiPillView;
        case ksScrollUp:   return &ki->kiScrollUp;
        case ksScrollDown: return &ki->kiScrollDown;
        case ksScrollLeft: return &ki->kiScrollLeft;
        case ksScrollRight:return &ki->kiScrollRight;
        case ksQuickTree:  return &ki->kiQuickTree;
        case ksQuickRoad:  return &ki->kiQuickRoad;
        case ksQuickWall:  return &ki->kiQuickWall;
        case ksQuickPillbox:return &ki->kiQuickPillbox;
        case ksQuickMine:  return &ki->kiQuickMine;
        default:           return nullptr;
    }
}

/* Render a single key-binding row: "Label   [Key Name]  [Change]" */
static void keySetupRow(const char *label, KeySetupField field) {
    int *ptr = keySetupFieldPtr(field, &s_keySetupKeys);
    if (!ptr) return;

    bool waiting = (s_keySetupWaiting == field);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);

    ImGui::TableSetColumnIndex(1);
    if (waiting) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_DLGKEYSETUP_PRESSAKEY));
    } else {
        const char  *name      = keySetupScancodeLabel(*ptr);
        float        textLineH = ImGui::GetTextLineHeight();
        float        glyphSize = textLineH * 1.5f;
        SDL_Texture *glyph     = glyphForKeyboardScancode((SDL_Scancode)*ptr);
        /* Lift the glyph by half its overshoot so its vertical centre
           aligns with the row text baseline; otherwise the cap sits
           below the line. */
        float        glyphYOff = (glyphSize - textLineH) * 0.5f;
        float        cursorY   = ImGui::GetCursorPosY();
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

    ImGui::TableSetColumnIndex(2);
    ImGui::PushID((int)field);
    if (waiting) {
        if (ImGui::SmallButton(langGetText(STR_CANCEL))) {
            s_keySetupWaiting = ksNone;
        }
    } else {
        if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
            s_keySetupWaiting = field;
        }
    }
    ImGui::PopID();
}

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

static void renderKeySetupModal(ClientSim *cs) {
    char title[128];
    snprintf(title, sizeof(title), "%s###keysetup", langGetText(STR_DLGKEYSETUP_TITLE));
    if (s_showKeySetup) {
        ImGui::OpenPopup(title);
        s_showKeySetup = false;
        windowGetKeys(&s_keySetupKeys);
        s_keySetupAutoSlowdown = screenGetTankAutoSlowdownCS(cs);
        s_keySetupAutoGunsight = screenGetTankAutoHideGunsightCS(cs);
        s_keySetupWaiting      = ksNone;
        inputGamepadBindingsGetAll(&s_keySetupGamepadBindings);
        s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
        s_keySetupTrigArmed[0] = true;
        s_keySetupTrigArmed[1] = true;
    }

    /* Keep the popup centered on first use */
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_Always);

    /* ImGuiWindowFlags_NoMove so the user cannot accidentally drag it off-screen */
    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open,
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove)) {
        return;
    }
    if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    /* While this modal is open ALL keyboard/mouse events are consumed by ImGui
     * (BeginPopupModal sets WantCaptureKeyboard + WantCaptureMouse).
     * sdl3ImguiProcessEvents additionally intercepts SDL_EVENT_KEY_DOWN when
     * s_keySetupWaiting != ksNone to route the raw scancode here. */

    if (s_keySetupWaiting != ksNone) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_DLGKEYSETUP_PRESS_OR_CANCEL));
        ImGui::Separator();
    } else if (s_keySetupGamepadWaitingAction != (int)GP_ACT_COUNT) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_GP_REBIND_PROMPT));
        ImGui::Separator();
    }

    /* Scrollable region containing all binding rows */
    float footerH = ImGui::GetFrameHeightWithSpacing() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::BeginChild("##bindings", ImVec2(0.0f, -footerH), false);

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_RowBg;

    auto section = [&](const char *sectionTitle) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", sectionTitle);
        ImGui::BeginTable(sectionTitle, 3, tflags, ImVec2(-1, 0));
        ImGui::TableSetupColumn(langGetText(STR_DLGKEYSETUP_COL_ACTION),
                                ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn(langGetText(STR_DLGKEYSETUP_COL_KEY),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,  68.0f);
    };
    auto endSection = [&]() { ImGui::EndTable(); };

    section(langGetText(STR_DLGKEYSETUP_DRIVETANK));
    keySetupRow(langGetText(STR_DLGKEYSETUP_FASTER),    ksForward);
    keySetupRow(langGetText(STR_DLGKEYSETUP_SLOWER),    ksBackward);
    keySetupRow(langGetText(STR_DLGKEYSETUP_TURNLEFT),  ksTurnLeft);
    keySetupRow(langGetText(STR_DLGKEYSETUP_TURNRIGHT), ksTurnRight);
    endSection();

    section(langGetText(STR_DLGKEYSETUP_WEAPONS));
    keySetupRow(langGetText(STR_DLGKEYSETUP_SHOOT),    ksShoot);
    keySetupRow(langGetText(STR_DLGKEYSETUP_LAYMINE),  ksLayMine);
    endSection();

    section(langGetText(STR_DLGKEYSETUP_GUNRANGE));
    keySetupRow(langGetText(STR_DLGKEYSETUP_INCREASE), ksGunIncrease);
    keySetupRow(langGetText(STR_DLGKEYSETUP_DECREASE), ksGunDecrease);
    endSection();

    section(langGetText(STR_DLGKEYSETUP_VIEW));
    keySetupRow(langGetText(STR_DLGKEYSETUP_TANKVIEW), ksTankView);
    keySetupRow(langGetText(STR_DLGKEYSETUP_PILLVIEW), ksPillView);
    endSection();

    section(langGetText(STR_DLGKEYSETUP_SCROLL));
    keySetupRow(langGetText(STR_DLGKEYSETUP_SCROLLUP),    ksScrollUp);
    keySetupRow(langGetText(STR_DLGKEYSETUP_SCROLLDOWN),  ksScrollDown);
    keySetupRow(langGetText(STR_DLGKEYSETUP_SCROLLLEFT),  ksScrollLeft);
    keySetupRow(langGetText(STR_DLGKEYSETUP_SCROLLRIGHT), ksScrollRight);
    endSection();

    section(langGetText(STR_DLGKEYSETUP_QUICKKEYS));
    keySetupRow(langGetText(STR_DLGKEYSETUP_TREE),         ksQuickTree);
    keySetupRow(langGetText(STR_DLGKEYSETUP_ROAD),         ksQuickRoad);
    keySetupRow(langGetText(STR_DLGKEYSETUP_WALL),         ksQuickWall);
    keySetupRow(langGetText(STR_DLGKEYSETUP_QUICKPILLBOX), ksQuickPillbox);
    keySetupRow(langGetText(STR_DLGKEYSETUP_QUICKMINE),    ksQuickMine);
    endSection();

    /* Controller — only show when a gamepad is currently connected.
       Path A (Steam Input) sees this section but the table is ignored
       at runtime; Steam owns its own binding configurator. */
    if (inputGamepadIsConnected()) {
        section(langGetText(STR_GP_SECTION));
        gamepadSetupRow(langGetText(STR_GP_ACTION_FIRE),                GP_ACT_FIRE);
        gamepadSetupRow(langGetText(STR_GP_ACTION_MINE),                GP_ACT_MINE);
        gamepadSetupRow(langGetText(STR_GP_ACTION_BUILD_CONFIRM),       GP_ACT_BUILD_CONFIRM);
        gamepadSetupRow(langGetText(STR_GP_ACTION_VIEW_CYCLE),          GP_ACT_VIEW_CYCLE);
        gamepadSetupRow(langGetText(STR_GP_ACTION_GUNSIGHT_DEC),        GP_ACT_GUNSIGHT_DEC);
        gamepadSetupRow(langGetText(STR_GP_ACTION_GUNSIGHT_INC),        GP_ACT_GUNSIGHT_INC);
        gamepadSetupRow(langGetText(STR_GP_ACTION_BUILD_PREV),          GP_ACT_BUILD_PREV);
        gamepadSetupRow(langGetText(STR_GP_ACTION_BUILD_NEXT),          GP_ACT_BUILD_NEXT);
        gamepadSetupRow(langGetText(STR_GP_ACTION_BUILD_CURSOR_TOGGLE), GP_ACT_BUILD_CURSOR_TOGGLE);
        gamepadSetupRow(langGetText(STR_GP_ACTION_QUICK_CHAT),          GP_ACT_QUICK_CHAT);
        gamepadSetupRow(langGetText(STR_GP_ACTION_PAUSE),               GP_ACT_PAUSE);
        gamepadSetupRow(langGetText(STR_GP_ACTION_STATUS_TOGGLE),       GP_ACT_STATUS_TOGGLE);
        endSection();
    }

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Checkbox(langGetText(STR_DLGKEYSETUP_AUTOSLOWDOWN), &s_keySetupAutoSlowdown);
    ImGui::SameLine();
    ImGui::Checkbox(langGetText(STR_DLGKEYSETUP_AUTOGUNSIGHT), &s_keySetupAutoGunsight);
    ImGui::Spacing();

    /* OK / Cancel — disabled while a key-capture or gamepad-capture
     * is pending so the user must complete or cancel the row first. */
    bool busy = (s_keySetupWaiting != ksNone) ||
                (s_keySetupGamepadWaitingAction != (int)GP_ACT_COUNT);
    if (busy) ImGui::BeginDisabled();

    if (ImGui::Button(langGetText(STR_OK), ImVec2(120, 0))) {
        windowSetKeys(&s_keySetupKeys);
        inputGamepadBindingsSetAll(&s_keySetupGamepadBindings);
        screenSetTankAutoSlowdownCS(cs, s_keySetupAutoSlowdown);
        screenSetTankAutoHideGunsightCS(cs, s_keySetupAutoGunsight);
        gameFrontSaveTankPrefs(cs);   /* sync globals from tank */
        gameFrontSaveCurrentPrefs();  /* persist to disk now */
        s_keySetupWaiting = ksNone;
        s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(120, 0))) {
        s_keySetupWaiting = ksNone;
        s_keySetupGamepadWaitingAction = (int)GP_ACT_COUNT;
        ImGui::CloseCurrentPopup();
    }

    if (busy) ImGui::EndDisabled();

    ImGui::EndPopup();
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
        ImGui::SetNextWindowSize(ImVec2(460, 580), ImGuiCond_FirstUseEver);
    }
    bool *pOpen = uiModeIsTablet() ? nullptr : &s_showSettings;
    ImGuiWindowFlags flags = uiModeIsTablet() ? (ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse) : 0;
    char title[128];
    snprintf(title, sizeof(title), "%s###settings", langGetText(STR_DLGSETTINGS_TITLE));
    if (!ImGui::Begin(title, pOpen, flags)) {
        ImGui::End();
        return;
    }

    /* File actions — tablet/mobile only (desktop has menu bar) */
    if (uiModeIsTablet()) {
        if (ImGui::Button(langGetText(STR_MENU_SAVE_MAP), ImVec2(-1, 0))) {
            windowSaveMap(cs);
            s_showSettings = false;
        }
        if (ImGui::Button(langGetText(STR_MENU_LEAVE_GAME), ImVec2(-1, 0))) {
            windowNewGame();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    /* ---- Player ---- */
    if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_PLAYER), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_PLAYERNAME));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputText("##playerName", s_settingsNameBuf,
                             sizeof(s_settingsNameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            s_settingsNameBuf[32] = '\0';
            utilStripName(s_settingsNameBuf);
            if (s_settingsNameBuf[0] != '\0' && s_settingsNameBuf[0] != '*') {
                screenSetPlayerNameCS(cs, s_settingsNameBuf);
            }
        }
        ImGui::SameLine();
        {
            char applyBuf[64];
            snprintf(applyBuf, sizeof(applyBuf), "%s##name", langGetText(STR_DLGSETTINGS_APPLY));
            if (ImGui::Button(applyBuf)) {
                s_settingsNameBuf[32] = '\0';
                utilStripName(s_settingsNameBuf);
                if (s_settingsNameBuf[0] != '\0' && s_settingsNameBuf[0] != '*') {
                    screenSetPlayerNameCS(cs, s_settingsNameBuf);
                }
            }
        }

        if (!uiModeIsTablet()) {
            ImGui::Spacing();
            imguiWinbolonetDrawSection(true);
        }

#ifndef __ANDROID__
        if (!uiModeIsTablet()) {
            ImGui::Spacing();
            if (ImGui::Button(langGetText(STR_DLGSETTINGS_SETKEYS))) {
                sdl3ImguiShowKeySetup();
            }
        }
#endif
    }

    /* ---- Display ---- */
    if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_DISPLAY), ImGuiTreeNodeFlags_DefaultOpen)) {
        /* Frame Rate — not shown in tablet mode */
        if (!uiModeIsTablet()) {
            const char *frLabels[] = { "60", "50", "30", "20", "15", "12", "10" };
            int frValues[] = { FRAME_RATE_60, FRAME_RATE_50, FRAME_RATE_30,
                               FRAME_RATE_20, FRAME_RATE_15, FRAME_RATE_12, FRAME_RATE_10 };
            int curFrIdx = 2; /* default to 30 */
            for (int i = 0; i < 7; i++) {
                if (frameRate == frValues[i]) { curFrIdx = i; break; }
            }
            ImGui::TextUnformatted(langGetText(STR_DLGSYSINFO_FRAMERATE));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (ImGui::BeginCombo("##framerate", frLabels[curFrIdx])) {
                for (int i = 0; i < 7; i++) {
                    bool selected = (curFrIdx == i);
                    if (ImGui::Selectable(frLabels[i], selected)) {
                        windowSetFrameRate(frValues[i], true);
                    }
                }
                ImGui::EndCombo();
            }
        }

#ifndef __ANDROID__
        if (!uiModeIsTablet()) {
            /* Window Size — desktop only */
            const char *zoomLabels[] = {
                langGetText(STR_MENU_NORMAL),
                langGetText(STR_MENU_DOUBLE),
                langGetText(STR_MENU_QUAD),
                langGetText(STR_MENU_CUSTOM_RESIZABLE),
            };
            BYTE zoomValues[] = { ZOOM_FACTOR_NORMAL, ZOOM_FACTOR_DOUBLE, ZOOM_FACTOR_QUAD, ZOOM_FACTOR_CUSTOM };
            int curZoomIdx = 0;
            for (int i = 0; i < 4; i++) {
                if (zoomFactor == zoomValues[i]) { curZoomIdx = i; break; }
            }
            ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_WINDOWSIZE));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            if (ImGui::BeginCombo("##windowsize", zoomLabels[curZoomIdx])) {
                for (int i = 0; i < 4; i++) {
                    bool selected = (curZoomIdx == i);
                    if (ImGui::Selectable(zoomLabels[i], selected)) {
                        s_pendingZoom = zoomValues[i];
                    }
                }
                ImGui::EndCombo();
            }

            /* Hide Main View — desktop only */
            {
                bool hmv = (bool)hideMainView;
                if (ImGui::Checkbox(langGetText(STR_MENU_HIDE_MAIN), &hmv)) {
                    windowHideMainView_toggle();
                }
            }

            /* Smooth Scrolling — desktop only */
            {
                bool ss = (bool)smoothScrollingEnabled;
                if (ImGui::Checkbox(langGetText(STR_MENU_SMOOTH_SCROLLING), &ss)) {
                    windowSmoothScrolling_toggle();
                }
            }
        }
#endif

        {
            bool as = (bool)autoScrollingEnabled;
            if (ImGui::Checkbox(langGetText(STR_MENU_AUTO_SCROLLING), &as)) {
                windowAutomaticScrolling_toggle(cs);
            }
        }
        {
            bool gs = (bool)showGunsight;
            if (ImGui::Checkbox(langGetText(STR_MENU_SHOW_GUNSIGHT), &gs)) {
                windowShowGunsight_toggle(cs);
            }
        }

        if (uiModeIsTablet() || inputGamepadIsConnected()) {
            bool relSteering = !inputTouchGetAbsoluteSteering();
            if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_RELSTEER), &relSteering)) {
                inputTouchSetAbsoluteSteering(!relSteering);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGSETTINGS_RELSTEER_TIP));
            }
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

#ifndef __ANDROID__
        /* Controller Mode (Phase 8.1) — desktop-only.  On Steam Deck the
           UI is always in controller mode regardless of pref, so don't
           offer the radio there. */
        if (!uiModeIsTablet() && !uiModeIsSteamDeck()) {
            ImGui::Separator();
            ImGui::TextUnformatted("Controller Mode");
            ControllerModePref cm = uiControllerModeGet();
            int cur = (int)cm;
            bool changed = false;
            if (ImGui::RadioButton("Off##cmode",  cur == CONTROLLER_MODE_OFF))  { cur = CONTROLLER_MODE_OFF;  changed = true; }
            ImGui::SameLine();
            if (ImGui::RadioButton("On##cmode",   cur == CONTROLLER_MODE_ON))   { cur = CONTROLLER_MODE_ON;   changed = true; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Auto##cmode", cur == CONTROLLER_MODE_AUTO)) { cur = CONTROLLER_MODE_AUTO; changed = true; }
            if (changed) {
                uiControllerModeSet((ControllerModePref)cur);
                gameFrontSaveCurrentPrefs();
            }
            bool ask = uiControllerPromptAskOnConnectGet();
            if (ImGui::Checkbox("Ask when controller connected", &ask)) {
                uiControllerPromptAskOnConnectSet(ask);
                gameFrontSaveCurrentPrefs();
            }
        }

        if (!uiModeIsTablet()) {
            bool tabletMode = false;
            if (ImGui::Checkbox(langGetText(STR_DLGSETTINGS_TABLETMODE), &tabletMode)) {
                uiModeSet(UI_MODE_TABLET);
            }
        }
#endif
    }

    /* ---- Labels ---- */
    if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_LABELS), ImGuiTreeNodeFlags_DefaultOpen)) {
        /* Message Sender Names */
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_MSGNAMES));
        ImGui::SameLine();
        {
            bool isShort = (labelMsg == lblShort);
            char shortBuf[64], longBuf[64];
            snprintf(shortBuf, sizeof(shortBuf), "%s##msg", langGetText(STR_SHORT));
            snprintf(longBuf,  sizeof(longBuf),  "%s##msg", langGetText(STR_LONG));
            if (ImGui::RadioButton(shortBuf, isShort)) windowSetMessageLabelLen(cs, lblShort);
            ImGui::SameLine();
            if (ImGui::RadioButton(longBuf, !isShort)) windowSetMessageLabelLen(cs, lblLong);
        }

        /* Tank Labels */
        ImGui::TextUnformatted(langGetText(STR_DLGSETTINGS_TANKLABELS));
        ImGui::SameLine();
        {
            char noneBuf[64], shortBuf[64], longBuf[64];
            snprintf(noneBuf,  sizeof(noneBuf),  "%s##tank", langGetText(STR_NONE));
            snprintf(shortBuf, sizeof(shortBuf), "%s##tank", langGetText(STR_SHORT));
            snprintf(longBuf,  sizeof(longBuf),  "%s##tank", langGetText(STR_LONG));
            if (ImGui::RadioButton(noneBuf,  labelTank == lblNone))  windowSetTankLabelLen(cs, lblNone);
            ImGui::SameLine();
            if (ImGui::RadioButton(shortBuf, labelTank == lblShort)) windowSetTankLabelLen(cs, lblShort);
            ImGui::SameLine();
            if (ImGui::RadioButton(longBuf,  labelTank == lblLong))  windowSetTankLabelLen(cs, lblLong);
        }
        {
            bool noSelf = !(bool)labelSelf;
            if (ImGui::Checkbox(langGetText(STR_MENU_NO_OWN_LABEL), &noSelf)) {
                windowLabelOwnTank_toggle(cs);
            }
        }

        {
            bool pl = (bool)showPillLabels;
            if (ImGui::Checkbox(langGetText(STR_MENU_PILLBOX_LABELS), &pl)) {
                windowShowPillLabels_toggle(cs);
            }
        }
        {
            bool bl = (bool)showBaseLabels;
            if (ImGui::Checkbox(langGetText(STR_MENU_BASE_LABELS), &bl)) {
                windowShowBaseLabels_toggle(cs);
            }
        }
    }

    /* ---- Sound ---- */
    if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_SOUND), ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            bool se = (bool)soundEffects;
            if (ImGui::Checkbox(langGetText(STR_MENU_SOUND_EFFECTS), &se)) {
                windowSoundEffects_toggle();
            }
        }
        if (!uiModeIsTablet()) {
            bool bg = (bool)backgroundSound;
            if (ImGui::Checkbox(langGetText(STR_MENU_BACKGROUND_SOUND), &bg)) {
                windowBackgroundSoundChange_toggle();
            }
        }
        if (!uiModeIsTablet()) {
            bool sk = (bool)useSoundKeepalive;
            if (ImGui::Checkbox(langGetText(STR_MENU_SOUND_KEEPALIVE), &sk)) {
                windowSoundKeepalive();
            }
        }
    }

    /* ---- Messages ---- */
    if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_MESSAGES), ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            bool nw = (bool)showNewswireMessages;
            if (ImGui::Checkbox(langGetText(STR_MENU_NEWSWIRE_MSGS), &nw)) {
                windowMenuNewswire_toggle(cs);
            }
        }
        {
            bool am = (bool)showAssistantMessages;
            if (ImGui::Checkbox(langGetText(STR_MENU_ASSISTANT_MSGS), &am)) {
                windowMenuAssistant_toggle(cs);
            }
        }
        {
            bool ai = (bool)showAIMessages;
            if (ImGui::Checkbox(langGetText(STR_MENU_AI_MSGS), &ai)) {
                windowMenuAI_toggle(cs);
            }
        }
        {
            bool ns = (bool)showNetworkStatusMessages;
            if (ImGui::Checkbox(langGetText(STR_MENU_NETSTATUS_MSGS), &ns)) {
                windowMenuNetwork_toggle(cs);
            }
        }
        {
            bool nd = (bool)showNetworkDebugMessages;
            if (ImGui::Checkbox(langGetText(STR_MENU_NETDEBUG_MSGS), &nd)) {
                windowMenuNetworkDebug_toggle(cs);
            }
        }
    }

    /* ---- Game ---- */
    if (ImGui::CollapsingHeader(langGetText(STR_DLGSETTINGS_GAME))) {
        {
            bool anp = (bool)allowNewPlayers;
            if (ImGui::Checkbox(langGetText(STR_ALLOW_NEW_PLAYERS), &anp)) {
                windowMenuAllowNewPlayers_toggle(cs);
            }
        }
    }

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
        if (ImGui::MenuItem(langGetText(STR_MENU_NEW)))                       windowNewGame();
        if (ImGui::MenuItem(langGetText(STR_MENU_SAVE_MAP), "Ctrl+S"))        windowSaveMap(cs);
        ImGui::Separator();
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        if (!uiModeIsTablet()) {
            if (ImGui::MenuItem(langGetText(STR_DLGGAMEINFO_TITLE),    nullptr, s_popGameInfo.open))  togglePopOut(&s_popGameInfo, langGetText(STR_DLGGAMEINFO_TITLE), 320, 200);
            if (ImGui::MenuItem(langGetText(STR_DLGSYSINFO_TITLE),     nullptr, s_popSysInfo.open))   togglePopOut(&s_popSysInfo,  langGetText(STR_DLGSYSINFO_TITLE),  440, 600);
            if (ImGui::MenuItem(langGetText(STR_DLGNETINFO_TITLE),     nullptr, s_popNetInfo.open))   togglePopOut(&s_popNetInfo,  langGetText(STR_DLGNETINFO_TITLE),  360, 420);
        } else {
#endif
            if (ImGui::MenuItem(langGetText(STR_DLGGAMEINFO_TITLE),    nullptr, s_showGameInfo))  s_showGameInfo  = !s_showGameInfo;
            if (ImGui::MenuItem(langGetText(STR_DLGSYSINFO_TITLE),     nullptr, s_showSysInfo))   { if (!s_showSysInfo) sysInfoGraphReset(); s_showSysInfo = !s_showSysInfo; }
            if (ImGui::MenuItem(langGetText(STR_DLGNETINFO_TITLE),     nullptr, s_showNetInfo))   { if (!s_showNetInfo) pingGraphReset(); s_showNetInfo = !s_showNetInfo; }
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_EXIT)))                     windowSetQuitting();
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
        if (ImGui::MenuItem(langGetText(STR_MENU_AUTO_SCROLLING), "Ctrl+A", (bool)autoScrollingEnabled)) windowAutomaticScrolling_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_SHOW_GUNSIGHT),  "Ctrl+G", (bool)showGunsight))        windowShowGunsight_toggle(cs);

        if (ImGui::BeginMenu(langGetText(STR_MENU_MSG_NAMES_SUB))) {
            if (ImGui::MenuItem(langGetText(STR_SHORT), nullptr, labelMsg == lblShort)) windowSetMessageLabelLen(cs, lblShort);
            if (ImGui::MenuItem(langGetText(STR_LONG),  nullptr, labelMsg == lblLong))  windowSetMessageLabelLen(cs, lblLong);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(langGetText(STR_MENU_TANK_LABELS_SUB))) {
            if (ImGui::MenuItem(langGetText(STR_NONE),               "Ctrl+1", labelTank == lblNone))  windowSetTankLabelLen(cs, lblNone);
            if (ImGui::MenuItem(langGetText(STR_SHORT),              "Ctrl+2", labelTank == lblShort)) windowSetTankLabelLen(cs, lblShort);
            if (ImGui::MenuItem(langGetText(STR_LONG),               "Ctrl+3", labelTank == lblLong))  windowSetTankLabelLen(cs, lblLong);
            if (ImGui::MenuItem(langGetText(STR_MENU_NO_OWN_LABEL),  nullptr, !(bool)labelSelf))       windowLabelOwnTank_toggle(cs);
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem(langGetText(STR_MENU_PILLBOX_LABELS), "Ctrl+P", (bool)showPillLabels)) windowShowPillLabels_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_BASE_LABELS),    "Ctrl+B", (bool)showBaseLabels)) windowShowBaseLabels_toggle(cs);
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_HIDE_MAIN),      "Ctrl+H", (bool)hideMainView))   windowHideMainView_toggle();
        ImGui::Separator();
        {
            const char *presetLabel = (g_currentDevicePreset >= 0 && g_currentDevicePreset < s_numDevicePresets)
                ? s_devicePresets[g_currentDevicePreset].name : langGetText(STR_MENU_DESKTOP);
            char deviceMenuItem[96];
            SDL_snprintf(deviceMenuItem, sizeof(deviceMenuItem), "%s %s",
                         langGetText(STR_MENU_DEVICE), presetLabel);
            if (ImGui::MenuItem(deviceMenuItem, "Ctrl+T")) {
                dialogCycleDevicePreset(sdl3DrawGetWindow());
            }
        }

        ImGui::EndMenu();
    }

    /* ---- WinBolo ------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_WINBOLO))) {
        if (ImGui::MenuItem(langGetText(STR_ALLOW_NEW_PLAYERS),    nullptr, (bool)allowNewPlayers))           windowMenuAllowNewPlayers_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_SETKEYS),         "Ctrl+K"))                                 sdl3ImguiShowKeySetup();
        if (ImGui::MenuItem(langGetText(STR_DLGCHANGENAME_TITLE)))                                            s_showChangeName = true;
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_SOUND_EFFECTS),   nullptr, (bool)soundEffects))              windowSoundEffects_toggle();
        if (ImGui::MenuItem(langGetText(STR_MENU_BACKGROUND_SOUND),nullptr, (bool)backgroundSound))           windowBackgroundSoundChange_toggle();
        if (ImGui::MenuItem(langGetText(STR_MENU_SOUND_KEEPALIVE), nullptr, (bool)useSoundKeepalive))         windowSoundKeepalive();
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_NEWSWIRE_MSGS),   nullptr, (bool)showNewswireMessages))      windowMenuNewswire_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_ASSISTANT_MSGS),  nullptr, (bool)showAssistantMessages))     windowMenuAssistant_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_AI_MSGS),         nullptr, (bool)showAIMessages))            windowMenuAI_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_NETSTATUS_MSGS),  nullptr, (bool)showNetworkStatusMessages)) windowMenuNetwork_toggle(cs);
        if (ImGui::MenuItem(langGetText(STR_MENU_NETDEBUG_MSGS),   nullptr, (bool)showNetworkDebugMessages))  windowMenuNetworkDebug_toggle(cs);
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_REQUEST_ALLIANCE),     "Ctrl+R"))                                 screenRequestAllianceCS(cs);
        if (ImGui::MenuItem(langGetText(STR_LEAVE_ALLIANCE)))                                                 screenLeaveAllianceCS(cs);
        ImGui::Separator();
        if (ImGui::MenuItem(langGetText(STR_MENU_SETTINGS)))                                                  sdl3ImguiShowSettings();
        ImGui::EndMenu();
    }

    /* ---- Players ------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_PLAYERS))) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        if (!uiModeIsTablet()) {
            if (ImGui::MenuItem(langGetText(STR_MENU_SEND_MESSAGE), "Ctrl+M", s_popSendMsg.open))
                togglePopOut(&s_popSendMsg, langGetText(STR_MENU_SEND_MESSAGE), 400, 200);
        } else {
#endif
            if (ImGui::MenuItem(langGetText(STR_MENU_SEND_MESSAGE), "Ctrl+M")) {
                s_showSendMsg = !s_showSendMsg;
                if (s_showSendMsg) s_sendMsgFocusInput = true;
            }
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        ImGui::Separator();
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_ALL),    false, ImGuiSelectableFlags_DontClosePopups))   screenCheckAllNonePlayersCS(cs, true);
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_NONE),   false, ImGuiSelectableFlags_DontClosePopups))   screenCheckAllNonePlayersCS(cs, false);
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_ALLIES), false, ImGuiSelectableFlags_DontClosePopups))   screenCheckAlliedPlayersCS(cs);
        if (ImGui::Selectable(langGetText(STR_MENU_SELECT_NEARBY), false, ImGuiSelectableFlags_DontClosePopups))   screenCheckNearbyPlayersCS(cs);
        /* Pre-compute alliance state for each player */
        players *plrs = &cs->sim.plyrs;
        BYTE self = cs->myPlayerNum;
        bool hasAllies  = false;
        bool canRequest = false;
        bool isAlly[MAX_PLAYERS] = {};
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (s_playerEnabled[i] && i != self) {
                isAlly[i] = playersIsAllie(plrs, self, (BYTE)i);
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
            /* Render flag icon inline before player name */
            if (s_playerEnabled[i] && s_playerCountry[i][0] != '\0') {
                SDL_Texture *flagTex = flagsGetTexture(s_playerCountry[i]);
                if (flagTex) {
                    ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
                    ImGui::SameLine();
                }
            }
            if (s_playerEnabled[i]) {
                /* Custom row: selectable name on left, colored WBN+ping on right */
                float fullWidth = ImGui::GetContentRegionAvail().x;

                /* Build ping string */
                char pingStr[16];
                if (s_playerPing[i] > 0) {
                    snprintf(pingStr, sizeof(pingStr), "%dms", (int)s_playerPing[i]);
                } else {
                    snprintf(pingStr, sizeof(pingStr), "---");
                }
                /* Measure right-side width: icons + ping + checkmark (rightmost) */
                ensureWbnIconsLoaded();
                ensurePlatformIconsLoaded();
                ImGuiContext &g = *GImGui;
                float checkSz = g.FontSize * 0.866f;
                float iconW = (float)WBN_ICON_SIZE;
                float pingWidth = ImGui::CalcTextSize(pingStr).x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                uint8_t pflags = s_playerFlags[i];
                uint8_t pct    = s_playerClientType[i];
                float iconsWidth = 0.0f;
                if (sdl3ImguiGetPlatformIcon(pct))                         iconsWidth += iconW + spacing;
                if ((pflags & PLAYER_FLAG_WBN_VERIFIED) && s_iconGlobe)    iconsWidth += iconW + spacing;
                if ((pflags & (PLAYER_FLAG_WBN_STEAM_LINKED | PLAYER_FLAG_STEAM_BUILD)) && s_iconSteam)
                    iconsWidth += iconW + spacing;
                float rightWidth = iconsWidth + pingWidth + spacing + checkSz;

                /* Selectable player name (no highlight) */
                char selectLabel[64];
                snprintf(selectLabel, sizeof(selectLabel), "%s##sel%d", label, i);
                if (ImGui::Selectable(selectLabel, false, ImGuiSelectableFlags_DontClosePopups, ImVec2(fullWidth - rightWidth - spacing, 0))) {
                    screenTogglePlayerCheckStateCS(cs, (BYTE)i);
                }

                /* Right-aligned platform/WBN/Steam icons */
                ImGui::SameLine(fullWidth - rightWidth);
                renderPlayerName(NULL, pflags, pct, "", false);

                /* Ping with color coding */
                ImGui::SameLine();
                ImVec4 pingColor;
                if (s_playerPing[i] == 0)        pingColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                else if (s_playerPing[i] < 50)   pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                else if (s_playerPing[i] < 150)  pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                else                              pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, pingColor);
                ImGui::TextUnformatted(pingStr);
                ImGui::PopStyleColor();

                /* Render checkmark to the right of ping (same as MenuItem tick) */
                if (s_playerChecked[i]) {
                    float checkX = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x + fullWidth - checkSz;
                    ImVec2 pos = ImVec2(checkX, ImGui::GetItemRectMin().y + g.FontSize * 0.134f * 0.5f);
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
                    screenLeaveAllianceCS(cs);
            } else {
                /* Not in an alliance — show Request */
                if (!canRequest || inCooldown) ImGui::BeginDisabled();
                if (ImGui::MenuItem(langGetText(STR_REQUEST_ALLIANCE))) {
                    screenRequestAllianceCS(cs);
                    s_allianceReqCooldownEnd = SDL_GetTicks() + ALLIANCE_REQ_WAIT_MS;
                }
                if (!canRequest || inCooldown) ImGui::EndDisabled();
            }
        }
        ImGui::EndMenu();
    }

    /* ---- Brains -------------------------------------- */
    if (ImGui::BeginMenu(langGetText(STR_MENU_BRAINS), screenGetAiTypeCS(cs) != aiNone)) {
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
        if (ImGui::MenuItem(langGetText(STR_MENU_HELP)))  { /* TODO: open help file */ }
        if (ImGui::MenuItem(langGetText(STR_MENU_ABOUT))) s_showAbout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

/* -------------------------------------------------------
 * Windows aspect ratio enforcement via window subclassing
 * ------------------------------------------------------- */
#ifdef _WIN32
#include <commctrl.h>  /* SetWindowSubclass */
#pragma comment(lib, "comctl32.lib")

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
        /* Enforce 1x minimum window size */
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        RECT clientRect = {0, 0, SDL3_SCREEN_W, SDL3_SCREEN_H + MENU_BAR_HEIGHT};
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

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename  = nullptr; /* no imgui.ini — avoid filesystem clutter */

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    imguiLoadBoloFont(18.0f * dialogDeckFontMul());

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
    } else if (uiModeIsSteamDeck()) {
        /* Match the bumped font size with proportionally bumped layout
           metrics (FramePadding, ItemSpacing, ScrollbarSize, etc.) so
           in-game dialogs (Settings / Players / Send Message / pause
           overlay) don't clip text or overlap.  No touch padding —
           Deck uses desktop hover/click feel. */
        const float deckMul = dialogDeckFontMul();
        if (deckMul > 1.0f) {
            ImGui::GetStyle().ScaleAllSizes(deckMul);
        }
    }

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

        /* Route events to pop-out windows — if the event belongs to a
           pop-out, forward it there and skip the rest of the main loop
           so it doesn't reach the game input. */
        {
            PopOutWindow *popOuts[] = { &s_popSysInfo, &s_popNetInfo, &s_popGameInfo, &s_popSendMsg };
            bool consumedByPopOut = false;
            for (int i = 0; i < 4; i++) {
                PopOutWindow *pw = popOuts[i];
                if (!pw->open || !pw->window) continue;

                SDL_WindowID pwID = SDL_GetWindowID(pw->window);
                bool isForThisWindow = false;
                switch (ev.type) {
                    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    case SDL_EVENT_WINDOW_FOCUS_GAINED:
                    case SDL_EVENT_WINDOW_FOCUS_LOST:
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
                }

                if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == pwID) {
                    popOutDestroy(pw);
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
            if (soundEffects && !backgroundSound) {
                SDL_Window *focused = SDL_GetKeyboardFocus();
                bool focusedIsOurs = (focused == s_window);
                if (!focusedIsOurs) {
                    PopOutWindow *pws[] = { &s_popSysInfo, &s_popNetInfo, &s_popGameInfo, &s_popSendMsg };
                    for (int i = 0; i < 4; i++) {
                        if (pws[i]->window && pws[i]->window == focused) { focusedIsOurs = true; break; }
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
            windowSuspendBackground();
            continue;
        }
        if (ev.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            windowResumeForeground(cs);
            continue;
        }

        /* Ctrl+key shortcuts — only for events on the main window */
        if (ev.type == SDL_EVENT_KEY_DOWN &&
            ev.key.windowID == SDL_GetWindowID(s_window) &&
            (ev.key.mod & SDL_KMOD_CTRL) != 0) {
            switch (ev.key.scancode) {
            case SDL_SCANCODE_M:
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
                if (!uiModeIsTablet()) {
                    togglePopOut(&s_popSendMsg, langGetText(STR_MENU_SEND_MESSAGE), 400, 200);
                } else {
#endif
                    if (s_showSendMsg) {
                        s_sendMsgFocusInput = true;
                    } else {
                        s_showSendMsg = true;
                        s_sendMsgFocusInput = true;
                    }
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
                windowShowPillLabels_toggle(cs);
                continue;
            case SDL_SCANCODE_B:
                windowShowBaseLabels_toggle(cs);
                continue;
            case SDL_SCANCODE_H:
                windowHideMainView_toggle();
                continue;
            case SDL_SCANCODE_R:
                screenRequestAllianceCS(cs);
                continue;
            case SDL_SCANCODE_T:
                /* Cycle through device resolution presets */
                dialogCycleDevicePreset(sdl3DrawGetWindow());
                continue;
            default:
                break;
            }
        }

        /* Key capture for the Key Setup modal — intercept before the game sees it. */
        if (s_keySetupWaiting != ksNone && ev.type == SDL_EVENT_KEY_DOWN &&
            ev.key.windowID == SDL_GetWindowID(s_window)) {
            SDL_Scancode sc = ev.key.scancode;
            if (sc == SDL_SCANCODE_ESCAPE) {
                /* Escape cancels the current capture but leaves the modal open. */
                s_keySetupWaiting = ksNone;
            } else {
                int *ptr = keySetupFieldPtr(s_keySetupWaiting, &s_keySetupKeys);
                if (ptr) *ptr = (int)sc;
                s_keySetupWaiting = ksNone;
            }
            /* Do NOT forward to the game — key was consumed by the dialog. */
            continue;
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

        /* While the Key Setup modal is open, swallow all mouse + keyboard events
         * so they never reach the game. */
        ImGuiIO &io = ImGui::GetIO();
        /* A click outside all ImGui windows (game area) should clear nav focus
           so that menu open/close does not restore focus to Send Message. */
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !io.WantCaptureMouse) {
            s_clearNavFocus = true;
        }
        if (io.WantCaptureKeyboard || io.WantCaptureMouse) {
            bool isGameInput = (ev.type == SDL_EVENT_MOUSE_MOTION       ||
                                ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN  ||
                                ev.type == SDL_EVENT_MOUSE_BUTTON_UP    ||
                                ev.type == SDL_EVENT_MOUSE_WHEEL        ||
                                ev.type == SDL_EVENT_KEY_DOWN           ||
                                ev.type == SDL_EVENT_KEY_UP);
            if (isGameInput) continue;
        }

        /* Handle winbolo:// URL opened while app is already running.
           macOS delivers URL scheme activations as SDL_EVENT_DROP_FILE. */
        if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
            const char *url = ev.drop.data;
            if (strncmp(url, "winbolo://", 10) == 0) {
                WB_LOG_INFO(WB_LOG_CAT_GUI, "[URL] Received winbolo:// link while running: %s", url);
                if (cs && cs->netStat == netRunning) {
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
        /* Window resized — enforce content aspect ratio (515:325) accounting for menu bar */
        if (ev.type == SDL_EVENT_WINDOW_RESIZED &&
            ev.window.windowID == SDL_GetWindowID(s_window)) {
            if (s_suppressAutoCustom) {
                /* Programmatic resize from windowZoomChange — don't auto-switch or adjust.
                   Don't clear the flag here - it gets cleared at end of frame after zoom is applied. */
            } else {
                /* Enforce aspect ratio: adjust height to match width */
                int w = ev.window.data1;
                int h = ev.window.data2;
                int contentH = h - MENU_BAR_HEIGHT;
                int correctContentH = w * SDL3_SCREEN_H / SDL3_SCREEN_W;
                int correctH = correctContentH + MENU_BAR_HEIGHT;
                if (h != correctH) {
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
        /* Window moved — save position */
        if (ev.type == SDL_EVENT_WINDOW_MOVED &&
            ev.window.windowID == SDL_GetWindowID(s_window)) {
            windowSaveCurrentPosition();
            gameFrontSaveWindowSettings();
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
   so Steam Input must run Menu set even though cs->inLobby is
   false.  Add new popups here as they're introduced. */
static bool any_popup_modal_open(void) {
    return deckPauseIsOpen() ||
           quickChatIsOpen() ||
           s_showSendMsg ||
           s_showPlayersPanel ||
           s_showSettings ||
           s_showKeySetup;
}

/* Steam Input action-set follower.  Menu set wins on no-cs / lobby /
   any-popup-open; InGame set otherwise.  Helper module owns the
   idempotent activation. */
static void update_steam_input_action_set(ClientSim *cs) {
    bool wantMenu = !cs || cs->inLobby || any_popup_modal_open();
    if (wantMenu) imguiSteamNavActivateMenuSet();
    else          imguiSteamNavActivateGameSet();
}

void sdl3ImguiPumpAndRender(ClientSim *cs) {
    if (!s_window || !s_renderer) return;

    /* Sync Steam Input action set to current gameplay context.  Must
       run before any consumer of action data (edge triggers below
       and the input wiring downstream). */
    update_steam_input_action_set(cs);

    /* Feed ImGui gamepad nav from Steam Input on Path A — must run
       before NewFrame so the events are visible to ImGui this frame. */
    imguiSteamNavFeedCurrentContext();

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

    /* Phase 8.1 — controller-detected prompt.  Rising edge from no
       gamepad → gamepad connected, when controller mode is currently off
       and the player hasn't dismissed the prompt with "Don't ask again".
       Skip on tablet (mobile has its own touch UX) and on Deck (already
       always controller-mode).  Allowed in lobby — a controller plugged
       in at the menu is exactly when the prompt is most useful.

       First-frame sync: seed from the current connection state without
       firing.  Without this, a controller plugged in before the main
       context started rendering would always look like a "rising edge"
       on the first frame and pop the prompt even if the player just
       launched with the pad already attached. */
    {
        static bool s_initialized   = false;
        static bool s_lastConnected = false;
        bool nowConnected = inputGamepadIsConnected();
        if (!s_initialized) {
            s_lastConnected = nowConnected;
            s_initialized   = true;
        } else if (nowConnected && !s_lastConnected &&
                   !uiModeIsTablet() && !uiModeIsSteamDeck() &&
                   !uiShouldUseControllerMode() &&
                   uiControllerPromptAskOnConnectGet() &&
                   !controllerPromptIsOpen()) {
            controllerPromptOpen();
        }
        s_lastConnected = nowConnected;
    }

    /* Pause-overlay open trigger: Start button in controller mode (Deck
       always, desktop when the Controller Mode pref opts in — Phase 8.1). */
    if (inputGamepadIsPauseEdge() && uiShouldUseControllerMode()) {
        deckPauseOpen();
    }
    /* Active-controller-disconnect open trigger: open pause overlay so the
       player can recover (battery dies, dongle drops).  Skip in lobby
       (keyboard UI) and when overlay is already open. */
    if (inputGamepadConsumeActiveDisconnect() &&
        uiShouldUseControllerMode() && cs && !cs->inLobby &&
        !deckPauseIsOpen()) {
        deckPauseOpen();
    }
    /* Quick-chat open trigger: D-pad UP, in-game only.  Gamepad-universal
       (not Deck-gated) — desktop gamepad players also benefit.  Skipped
       in lobby because the lobby has its own chat UI, and skipped while
       any in-game panel / overlay is open (settings, players, send-msg,
       pause, popups) so D-pad nav inside those windows isn't also
       interpreted as a quick-chat open. */
    if (inputGamepadIsQuickChatEdge() && cs && !cs->inLobby &&
        !sdl3ImguiIsDialogOpen() && !quickChatIsOpen()) {
        quickChatOpen();
    }

    /* B button — close the topmost open in-game panel.  Modal popups
       (pause, quick-chat) close themselves via the p_open passed to
       BeginPopupModal; ImGui's NavCancel only closes non-modal popups.
       Regular ImGui windows (Settings, Players, Send Message, etc.)
       aren't auto-closed by anything, so handle them here.  Skipped
       while a popup is on the stack or a text input is active — those
       want B for popup-close / clear-text first. */
    {
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        bool anyPopup = (ctx && ctx->OpenPopupStack.Size > 0);
        if (inputGamepadIsConnected() && !anyPopup &&
            !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if      (s_showSettings)       s_showSettings = false;
            else if (s_showSendMsg)        s_showSendMsg = false;
            else if (s_showPlayersPanel)   s_showPlayersPanel = false;
            else if (s_brainSettingsOpen)  s_brainSettingsOpen = false;
            else if (s_allianceVisible)    s_allianceVisible = false;
            else if (s_showSysInfo)        s_showSysInfo = false;
            else if (s_showNetInfo)        s_showNetInfo = false;
            else if (s_showGameInfo)       s_showGameInfo = false;
        }
    }

    if (uiModeIsTablet()) {
        sdl3ImguiTabletOverlay(cs);
    } else {
        /* Hide the menu bar in controller mode — controller-only players
           can't reach the 22px menu strip; the pause overlay replaces it. */
        if (!uiShouldUseControllerMode()) {
            renderMenuBar(cs);
        }
        /* Pause overlay + quick-chat overlay (no-ops when closed). */
        deckPauseRender(cs);
        quickChatRender(cs);
        /* Controller-detected prompt (Phase 8.1) — also a no-op when
           closed.  Rendered through the main context so it inherits
           NavEnableGamepad for A/B selection. */
        controllerPromptRender();
    }

    /* Detect when a menu-bar dropdown (child menu popup) just closed.
       When this happens, clear nav focus so the panel that had focus before
       the menu was opened does not regain focus unexpectedly. */
    {
        static bool s_menuPopupWasOpen = false;
        ImGuiContext *g = ImGui::GetCurrentContext();
        bool menuPopupOpen = false;
        for (int i = 0; i < g->OpenPopupStack.Size; i++) {
            ImGuiWindow *w = g->OpenPopupStack[i].Window;
            if (w && (w->Flags & ImGuiWindowFlags_ChildMenu)) {
                menuPopupOpen = true;
                break;
            }
        }
        if (s_menuPopupWasOpen && !menuPopupOpen) {
            ImGui::SetWindowFocus(nullptr);
        }
        s_menuPopupWasOpen = menuPopupOpen;
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
    renderAboutModal();
    renderChangeNameModal(cs);
    renderAllianceRequest(cs);
    renderPasswordModal();
    renderKeySetupModal(cs);
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
                "###about", "###changename",
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
                dialogDismissKeyboard(s_window);
            }
        }
    }


    ImGui::EndFrame();
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

        ImGui::SetCurrentContext(mainCtx);
    }

    /* Apply deferred zoom change after the frame is fully rendered.
       windowZoomChange destroys and recreates the ImGui context, so it
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
    if (open && !s_showSysInfo) sysInfoGraphReset();
    s_showSysInfo = open;
}
void sdl3ImguiShowNetInfo(bool open) {
    if (open && !s_showNetInfo) pingGraphReset();
    s_showNetInfo = open;
}
void sdl3ImguiShowGameInfo(bool open) {
    s_showGameInfo = open;
}
void sdl3ImguiShowSendMsg(bool open) {
    s_showSendMsg = open;
    if (open) {
        /* Reset cooldown so the Send button is always enabled on fresh open */
        s_sendMsgCooldownEnd = 0;
        s_sendMsgFocusInput = true;
#if BOLO_MOBILE
        s_showSettings = false;
        s_showPlayersPanel = false;
#endif
    }
}
void sdl3ImguiShowSettings(void) {
    s_showSettings = !s_showSettings;
    if (s_showSettings) {
        s_settingsNameBuf[0] = '\0';
        gameFrontGetPlayerName(s_settingsNameBuf);
#if BOLO_MOBILE
        s_showSendMsg = false;
        s_showPlayersPanel = false;
#endif
    }
}
void sdl3ImguiShowPlayersPanel(bool open) {
    s_showPlayersPanel = open;
#if BOLO_MOBILE
    if (open) {
        s_showSendMsg = false;
        s_showSettings = false;
    }
#endif
}

void sdl3ImguiTogglePlayersPanel(void) {
    sdl3ImguiShowPlayersPanel(!s_showPlayersPanel);
}

bool sdl3ImguiWantsKeyboard(void) {
    if (!s_window) return false;
    return ImGui::GetIO().WantCaptureKeyboard;
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

void sdl3ImguiClearPlayer(unsigned char playerNum) {
    if (playerNum >= MAX_PLAYERS) return;
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

SDL_Texture *sdl3ImguiGetGlobeIcon(void) {
    ensureWbnIconsLoaded();
    return s_iconGlobe;
}

SDL_Texture *sdl3ImguiGetSteamIcon(void) {
    ensureWbnIconsLoaded();
    return s_iconSteam;
}

SDL_Texture *sdl3ImguiGetPlatformIcon(uint8_t clientType) {
    ensurePlatformIconsLoaded();
    if (clientType >= CLIENT_TYPE_COUNT) return nullptr;
    return s_iconPlatform[clientType];
}

/* Gold tint for supporters; white = no tint (passthrough). */
static const ImVec4 SUPPORTER_TINT = ImVec4(1.00f, 0.84f, 0.20f, 1.00f);
static const ImVec4 NO_TINT        = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);

static const char *platformName(uint8_t ct) {
    static const char *names[CLIENT_TYPE_COUNT] = {
        "", "Windows", "Linux", "macOS", "iOS", "Android", "Steam Deck", "Web"
    };
    return (ct < CLIENT_TYPE_COUNT) ? names[ct] : "";
}

void renderPlayerName(const char *name, uint8_t flags, uint8_t clientType,
                      const char *countryCode, bool showCountry) {
    ensurePlatformIconsLoaded();
    SDL_Texture *platTex = sdl3ImguiGetPlatformIcon(clientType);
    if (platTex) {
        ImVec4 tint = (flags & PLAYER_FLAG_SUPPORTER) ? SUPPORTER_TINT : NO_TINT;
        /* ImGui 1.91.9+ removed tint_col from Image(); ImageWithBg takes
         * (size, uv0, uv1, bg_col, tint_col) - bg transparent. */
        ImGui::ImageWithBg((ImTextureID)platTex,
                           ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE),
                           ImVec2(0, 0), ImVec2(1, 1),
                           ImVec4(0, 0, 0, 0), tint);
        if (ImGui::IsItemHovered()) {
            const char *plat = platformName(clientType);
            if (flags & PLAYER_FLAG_SUPPORTER)
                ImGui::SetTooltip("%s — Supporter", plat);
            else
                ImGui::SetTooltip("%s", plat);
        }
        ImGui::SameLine();
    }

    ensureWbnIconsLoaded();
    if ((flags & PLAYER_FLAG_WBN_VERIFIED) && s_iconGlobe) {
        ImGui::Image((ImTextureID)s_iconGlobe, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
        ImGui::SameLine();
    }
    if ((flags & (PLAYER_FLAG_WBN_STEAM_LINKED | PLAYER_FLAG_STEAM_BUILD)) && s_iconSteam) {
        ImGui::Image((ImTextureID)s_iconSteam, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
        ImGui::SameLine();
    }
    /* Icon-only mode: a NULL/empty name skips the text and trailing
     * country flag so callers can use this helper to render just the
     * WBN/Steam badges (the lobby table and players panel both render
     * the name via their own widgets — Selectable/TextColored — and
     * only need the badge sequence from here). */
    if (name && name[0] != '\0') {
        ImGui::TextUnformatted(name);
        if (showCountry && countryCode && countryCode[0] != '\0' &&
            !(countryCode[0] == 'X' && countryCode[1] == 'X')) {
            SDL_Texture *flagTex = flagsGetTexture(countryCode);
            if (flagTex) {
                ImGui::SameLine();
                ImGui::Image((ImTextureID)flagTex, ImVec2(FLAG_WIDTH, FLAG_HEIGHT));
            }
        }
    }
}

void sdl3ImguiSetPlayerCheckState(unsigned char playerNum, bool isChecked) {
    if (playerNum >= MAX_PLAYERS) return;
    s_playerChecked[playerNum] = isChecked;
}

void sdl3ImguiShowKeySetup(void) {
    s_showKeySetup = true;
}

void sdl3ImguiCleanup(void) {
    if (!s_window) return;
    inputGamepadShutdown();
    popOutDestroy(&s_popSysInfo);
    popOutDestroy(&s_popNetInfo);
    popOutDestroy(&s_popGameInfo);
    popOutDestroy(&s_popSendMsg);
    flagsDestroy();
    if (s_iconGlobe) { SDL_DestroyTexture(s_iconGlobe); s_iconGlobe = nullptr; }
    if (s_iconSteam) { SDL_DestroyTexture(s_iconSteam); s_iconSteam = nullptr; }
    s_wbnIconsLoaded = false;
    for (int i = 0; i < CLIENT_TYPE_COUNT; i++) {
        /* Slot may alias another (e.g. WEB → globe.svg), but each load returns a
         * distinct SDL_Texture so destroying every slot is safe. */
        if (s_iconPlatform[i]) { SDL_DestroyTexture(s_iconPlatform[i]); s_iconPlatform[i] = nullptr; }
    }
    s_platformIconsLoaded = false;
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
