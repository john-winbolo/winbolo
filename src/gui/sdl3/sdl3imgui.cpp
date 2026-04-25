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
#include "../gamefront.h"
}

/* Our own header */
#include "sdl3imgui.h"
#include "sdl3draw.h"
#include "luabrainshandler.h"
#include "flags.h"

/* Include players.h with C linkage — no #pragma pack inside, safe here */
extern "C" {
#include "../../bolo/players.h"
#include "../../bolo/transport.h"
#include "../../bolo/transport_udp.h"
}

/* Include input.h for keyItems — SDL3 already included, safe here */
extern "C" {
#include "input.h"
#include "input_touch.h"
#include "override_mode.h"
#include "../ui_mode.h"
}

#include "sdl3imgui_tablet.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

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

/* Backend functions called directly from menu */
extern "C" void screenRequestAllianceCS(struct ClientSim *csPtr);
extern "C" void screenLeaveAllianceCS(struct ClientSim *csPtr);
extern "C" void screenCheckAllNonePlayersCS(struct ClientSim *csPtr, bool isChecked);
extern "C" void screenCheckAlliedPlayersCS(struct ClientSim *csPtr);
extern "C" void screenCheckNearbyPlayersCS(struct ClientSim *csPtr);
extern "C" void screenTogglePlayerCheckStateCS(struct ClientSim *csPtr, BYTE playerNum);

extern "C" bool showGunsight;
extern "C" bool autoScrollingEnabled;
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
#include "dialogs/imgui_settings.h"

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

/* Saved custom window size — restored when switching back to Custom mode */
static int s_customWindowW = 0;
static int s_customWindowH = 0;

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
static bool     s_playerWbn[MAX_PLAYERS]  = {};
static bool     s_playerSteam[MAX_PLAYERS] = {};

/* WBN/Steam icon textures */
static SDL_Texture *s_iconGlobe = nullptr;
static SDL_Texture *s_iconSteam = nullptr;
static bool s_wbnIconsLoaded = false;
#define WBN_ICON_SIZE 14

static SDL_Texture *loadSvgIconSmall(const char *path, int size) {
    NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
    if (!image) return nullptr;
    if (image->width < 1.0f || image->height < 1.0f) { nsvgDelete(image); return nullptr; }
    float scale = (float)size / image->height;
    if (image->width * scale > (float)size) scale = (float)size / image->width;
    int w = size, h = size;
    unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
    if (!pixels) { nsvgDelete(image); return nullptr; }
    memset(pixels, 0, (size_t)(w * h * 4));
    float offX = ((float)w - image->width * scale) * 0.5f;
    float offY = ((float)h - image->height * scale) * 0.5f;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, image, offX, offY, scale, pixels, w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) { SDL_free(pixels); return nullptr; }
    SDL_Renderer *r = s_renderer ? s_renderer : sdl3DrawGetRenderer();
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surface);
    SDL_DestroySurface(surface);
    SDL_free(pixels);
    return tex;
}

static void ensureWbnIconsLoaded(void) {
    if (s_wbnIconsLoaded) return;
    s_wbnIconsLoaded = true;
    s_iconGlobe = loadSvgIconSmall("data/ui/globe.svg", WBN_ICON_SIZE);
    s_iconSteam = loadSvgIconSmall("data/ui/steam.svg", WBN_ICON_SIZE);
    SDL_Log("[WBN ICONS] globe=%p steam=%p s_renderer=%p drawRenderer=%p",
            (void *)s_iconGlobe, (void *)s_iconSteam,
            (void *)s_renderer, (void *)sdl3DrawGetRenderer());
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

/* Send Message panel state */
enum SendMsgRecipient { kSendAll = 0, kSendAllies, kSendNearby, kSendSelected };
static int    s_sendMsgRecipient  = kSendAll;
static char   s_sendMsgBuf[101]   = "";
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
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
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
static void renderSysInfoContent(void) {
    float drawPct  = (windowGetDrawTime() / 1000.0f) * 100.0f;
    float simPct   = (windowGetSimTime()  / 1000.0f) * 100.0f;
    float netPct   = (windowGetNetTime()  / 1000.0f) * 100.0f;
    float aiPct    = (windowGetAiTime()   / 1000.0f) * 100.0f;
    float totalPct = drawPct + simPct + netPct + aiPct;

    ImGui::Text("Frame Rate:        %d fps", drawGetFrameRate());
    ImGui::Separator();
    ImGui::Text("CPU Usage:");
    ImGui::Text("  Graphics:        %.2f %%", drawPct);
    ImGui::Text("  Sim Modeling:    %.2f %%", simPct);
    ImGui::Text("  Com Processing:  %.2f %%", netPct);
    ImGui::Text("  AI Tanks:        %.2f %%", aiPct);
    ImGui::Separator();
    ImGui::Text("  Total:           %.2f %%", totalPct);
}

static void renderSysInfoPanel(void) {
    if (!s_showSysInfo || s_popSysInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(300, 210), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("System Info", &s_showSysInfo)) {
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
    ImGui::Text("Server:      %s", str);

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
        ImGui::Text("This game:   %s", addr);
    } else {
        netGetOurAddressStr(cs, str);
        ImGui::Text("This game:   %s", str);
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
    ImGui::Text("Status:      %s", str);
    ImGui::Text("Server ping: %d ms", ping);
    ImGui::Text("Packets/sec: %d in / %d out", ppsIn, ppsOut);
    ImGui::Text("KB/sec:      %.1f in / %.1f out", (float)bpsIn / 1024.0f, (float)bpsOut / 1024.0f);
    ImGui::Text("Net errors:  %d", numErrors);

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
        ImGui::Text("Ping: min %d / avg %d / max %d ms",
                    (int)minPing, (int)avgPing, (int)maxPing);
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
        ImGui::Text("KB/s In:");
        ImGui::PlotLines("##kbin", s_kbInHistory, s_kbHistoryCount,
                         s_kbHistoryOffset, nullptr,
                         0.0f, maxKb * 1.2f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 40));
        ImGui::Text("KB/s Out:");
        ImGui::PlotLines("##kbout", s_kbOutHistory, s_kbHistoryCount,
                         s_kbHistoryOffset, nullptr,
                         0.0f, maxKb * 1.2f,
                         ImVec2(ImGui::GetContentRegionAvail().x, 40));
    }
}

static void renderNetInfoPanel(ClientSim *cs) {
    if (!s_showNetInfo || s_popNetInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Network Info", &s_showNetInfo)) {
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
    ImGui::Text("Map:           %s", mapName);
    if (strncmp(mapName, "rand_", 5) == 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Seed")) {
            SDL_SetClipboardText(mapName + 5);
        }
    }
    ImGui::Text("Players:       %d", (int)screenGetNumPlayersCS(cs));

    gameType *gt = &cs->sim.game;
    const char *gtStr = "Strict Tournament";
    if      (*gt == gameOpen)       gtStr = "Open";
    else if (*gt == gameTournament) gtStr = "Tournament";
    ImGui::Text("Game Type:     %s", gtStr);

    ImGui::Text("Hidden Mines:  %s", screenGetAllowHiddenMinesCS(cs) ? "Yes" : "No");

    aiType ai = screenGetAiTypeCS(cs);
    const char *aiStr = "No";
    if      (ai == aiYes)          aiStr = "Yes";
    else if (ai == aiYesAdvantage) aiStr = "Yes (Advantage)";
    else if (ai == aiFull)         aiStr = "Full Advantage";
    ImGui::Text("AI Tanks:      %s", aiStr);

    long timeLeft = screenGetGameTimeLeftCS(cs);
    if (timeLeft == UNLIMITED_GAME_TIME) {
        ImGui::Text("Time Limit:    Unlimited");
    } else {
        long mins = timeLeft;
        mins /= 50;   /* GAME_NUMGAMETICKS_SEC = (1000/20) */
        mins /= 60;   /* NUM_SECONDS_MINUTE */
        mins++;       /* round up */
        ImGui::Text("Time Remaining: %ld min", mins);
    }
}

static void renderGameInfoPanel(ClientSim *cs) {
    if (!s_showGameInfo || s_popGameInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(320, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Game Info", &s_showGameInfo)) {
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
    if (!ImGui::Begin("Brain Settings", &s_brainSettingsOpen,
                      ImGuiWindowFlags_NoResize)) {
        ImGui::End();
        return;
    }

    if (s_brainSettings == nullptr || s_brainSettingsCount == 0) {
        ImGui::TextDisabled("(no settings)");
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
    ImGui::RadioButton("All Players",        &s_sendMsgRecipient, kSendAll);
    ImGui::SameLine();
    ImGui::RadioButton("All Allies",         &s_sendMsgRecipient, kSendAllies);
    ImGui::SameLine();
    ImGui::RadioButton("All Nearby",         &s_sendMsgRecipient, kSendNearby);
    ImGui::SameLine();
    ImGui::RadioButton("Selected Players",   &s_sendMsgRecipient, kSendSelected);

    /* "Sending to N player(s)" label */
    int numSend = 0;
    switch (s_sendMsgRecipient) {
        case kSendAll:      numSend = (int)screenGetNumPlayersCS(cs);     break;
        case kSendAllies:   numSend = screenNumAlliesCS(cs);              break;
        case kSendNearby:   numSend = screenNumNearbyTanksCS(cs);         break;
        case kSendSelected: numSend = screenNumCheckedPlayersCS(cs);      break;
    }
    if (numSend == 1)
        ImGui::TextDisabled("Sending to 1 player");
    else
        ImGui::TextDisabled("Sending to %d players", numSend);

    /* Text input — max 100 chars, matching Win32 EM_LIMITTEXT */
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
    bool doSend = ImGui::Button("Send") || (!inCooldown && pressedEnter);
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
    if (!ImGui::Begin("Send Message", pOpen, flags)) {
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
    if (!ImGui::Begin("Players", pOpen, flags)) {
        ImGui::End();
        return;
    }

    /* Selection helpers */
    if (ImGui::Button("All"))    screenCheckAllNonePlayersCS(cs, true);
    ImGui::SameLine();
    if (ImGui::Button("None"))   screenCheckAllNonePlayersCS(cs, false);
    ImGui::SameLine();
    if (ImGui::Button("Allies")) screenCheckAlliedPlayersCS(cs);
    ImGui::SameLine();
    if (ImGui::Button("Nearby")) screenCheckNearbyPlayersCS(cs);

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
            /* Refresh ping/WBN/Steam */
            s_playerPing[i] = playersGetPing(&cs->sim.plyrs, (BYTE)i);
            s_playerWbn[i]  = playersGetWbnParticipant(&cs->sim.plyrs, (BYTE)i);
            s_playerSteam[i] = playersGetSteamParticipant(&cs->sim.plyrs, (BYTE)i);
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

        /* WBN participant icon */
        if (s_playerWbn[i]) {
            ensureWbnIconsLoaded();
            if (s_iconGlobe) {
                ImGui::Image((ImTextureID)s_iconGlobe, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                ImGui::SameLine();
            }
        }

        /* Steam participant icon */
        if (s_playerSteam[i]) {
            ensureWbnIconsLoaded();
            if (s_iconSteam) {
                ImGui::Image((ImTextureID)s_iconSteam, ImVec2(WBN_ICON_SIZE, WBN_ICON_SIZE));
                ImGui::SameLine();
            }
        }

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
            if (ImGui::Button("Leave Alliance", ImVec2(-1, 0)))
                screenLeaveAllianceCS(cs);
        } else {
            if (!canRequest || inCooldown) ImGui::BeginDisabled();
            if (ImGui::Button("Request Alliance", ImVec2(-1, 0))) {
                screenRequestAllianceCS(cs);
                s_allianceReqCooldownEnd = SDL_GetTicks() + ALLIANCE_REQ_WAIT_MS;
            }
            if (!canRequest || inCooldown) ImGui::EndDisabled();
        }
    }

    /* Allow new players toggle */
    {
        bool anp = (bool)allowNewPlayers;
        if (ImGui::Checkbox("Allow New Players", &anp))
            windowMenuAllowNewPlayers_toggle(cs);
    }

    ImGui::End();
}

/* -------------------------------------------------------
 * About modal
 * ------------------------------------------------------- */
static void renderAboutModal(void) {
    if (s_showAbout) {
        ImGui::OpenPopup("About WinBolo");
        s_showAbout = false;
    }
    if (ImGui::BeginPopupModal("About WinBolo", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted("WinBolo v1.0.1.7");
        ImGui::TextUnformatted("Copyright 1998-2008 John Morrison");
        ImGui::Separator();
        ImGui::TextDisabled("Bolo Copyright 1987-1995 Stuart Cheshire");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* -------------------------------------------------------
 * "Join Game?" confirmation modal — shown when a winbolo://
 * URL is received while already in a game.
 * ------------------------------------------------------- */
static void renderJoinConfirmModal(void) {
    if (s_showJoinConfirm) {
        ImGui::OpenPopup("Join Game?##urlconfirm");
        s_showJoinConfirm = false;
    }
    if (ImGui::BeginPopupModal("Join Game?##urlconfirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::Text("Leave current game and join server?");
        ImGui::Spacing();
        if (s_joinConfirmPort > 0) {
            ImGui::Text("%s:%d", s_joinConfirmAddr, s_joinConfirmPort);
        } else {
            ImGui::TextUnformatted(s_joinConfirmAddr);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Join", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
            /* Leave current game and return to menu with the URL queued */
            gameFrontHandleUrlOpen(s_joinConfirmUrl);
            windowNewGame();
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button("Cancel", ImVec2(80, 0)) ||
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
    if (s_showChangeName) {
        ImGui::OpenPopup("Change Player Name");
        s_showChangeName    = false;
        s_changeNameBuf[0] = '\0';
        screenGetPlayerNameCS(cs, s_changeNameBuf);
    }
    if (ImGui::BeginPopupModal("Change Player Name", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted("Enter the new player name for your tank:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        ImGui::SetNextItemWidth(300);
        bool enter = ImGui::InputText("##name", s_changeNameBuf,
                                      sizeof(s_changeNameBuf),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        bool doOK     = ImGui::Button("OK",     ImVec2(120, 0)) || enter;
        ImGui::SameLine();
        bool doCancel = ImGui::Button("Cancel", ImVec2(120, 0));

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
    if (ImGui::Begin("Alliance Request", &s_allianceVisible,
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("%s requests alliance. Accept?", s_alliancePlayerName);
        ImGui::Spacing();
        if (ImGui::Button("Accept", ImVec2(120, 0))) {
            clientSimAllianceAccept(cs, s_alliancePlayerNum);
            s_allianceVisible = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reject", ImVec2(120, 0)))
            s_allianceVisible = false;
    }
    ImGui::End();
}

/* -------------------------------------------------------
 * Password modal — shown when joining a protected game
 * ------------------------------------------------------- */
static void renderPasswordModal(void) {
    if (s_showPasswordOpen) {
        ImGui::OpenPopup("Password Required");
        s_showPasswordOpen  = false;
        s_passwordBuf[0]   = '\0';
    }
    if (ImGui::BeginPopupModal("Password Required", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (s_closeAllPopups) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
        ImGui::TextUnformatted("This game requires a password:");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(0);
        ImGui::SetNextItemWidth(270);
        bool enter = ImGui::InputText("##pass", s_passwordBuf,
                                      sizeof(s_passwordBuf),
                                      ImGuiInputTextFlags_Password |
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0)) || enter) {
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
    return "(none)";
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
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Press a key...");
    } else {
        ImGui::TextUnformatted(keySetupScancodeLabel(*ptr));
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::PushID((int)field);
    if (waiting) {
        if (ImGui::SmallButton("Cancel")) {
            s_keySetupWaiting = ksNone;
        }
    } else {
        if (ImGui::SmallButton("Change")) {
            s_keySetupWaiting = field;
        }
    }
    ImGui::PopID();
}

static void renderKeySetupModal(ClientSim *cs) {
    if (s_showKeySetup) {
        ImGui::OpenPopup("Key Setup");
        s_showKeySetup = false;
        windowGetKeys(&s_keySetupKeys);
        s_keySetupAutoSlowdown = screenGetTankAutoSlowdownCS(cs);
        s_keySetupAutoGunsight = screenGetTankAutoHideGunsightCS(cs);
        s_keySetupWaiting      = ksNone;
    }

    /* Keep the popup centered on first use */
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_Always);

    /* ImGuiWindowFlags_NoMove so the user cannot accidentally drag it off-screen */
    bool open = true;
    if (!ImGui::BeginPopupModal("Key Setup", &open,
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
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
                           "Press a key to assign, or click Cancel.");
        ImGui::Separator();
    }

    /* Scrollable region containing all binding rows */
    float footerH = ImGui::GetFrameHeightWithSpacing() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::BeginChild("##bindings", ImVec2(0.0f, -footerH), false);

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_RowBg;

    auto section = [&](const char *title) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", title);
        ImGui::BeginTable(title, 3, tflags, ImVec2(-1, 0));
        ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Key",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed,  68.0f);
    };
    auto endSection = [&]() { ImGui::EndTable(); };

    section("Drive Tank");
    keySetupRow("Faster",      ksForward);
    keySetupRow("Slower",      ksBackward);
    keySetupRow("Turn Left",   ksTurnLeft);
    keySetupRow("Turn Right",  ksTurnRight);
    endSection();

    section("Weapons");
    keySetupRow("Shoot",       ksShoot);
    keySetupRow("Lay Mine",    ksLayMine);
    endSection();

    section("Gun Range");
    keySetupRow("Increase",    ksGunIncrease);
    keySetupRow("Decrease",    ksGunDecrease);
    endSection();

    section("Views");
    keySetupRow("Tank View",   ksTankView);
    keySetupRow("Pill View",   ksPillView);
    endSection();

    section("Scroll");
    keySetupRow("Up",          ksScrollUp);
    keySetupRow("Down",        ksScrollDown);
    keySetupRow("Left",        ksScrollLeft);
    keySetupRow("Right",       ksScrollRight);
    endSection();

    section("Quick Keys");
    keySetupRow("Tree",        ksQuickTree);
    keySetupRow("Road",        ksQuickRoad);
    keySetupRow("Wall",        ksQuickWall);
    keySetupRow("Pillbox",     ksQuickPillbox);
    keySetupRow("Mine",        ksQuickMine);
    endSection();

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Checkbox("Auto Slowdown",          &s_keySetupAutoSlowdown);
    ImGui::SameLine();
    ImGui::Checkbox("Auto Hide Gunsight",     &s_keySetupAutoGunsight);
    ImGui::Spacing();

    /* OK / Cancel — disabled while a key-capture is pending so the user
     * must press a key or click Cancel on the row first. */
    bool busy = (s_keySetupWaiting != ksNone);
    if (busy) ImGui::BeginDisabled();

    if (ImGui::Button("OK", ImVec2(120, 0))) {
        windowSetKeys(&s_keySetupKeys);
        screenSetTankAutoSlowdownCS(cs, s_keySetupAutoSlowdown);
        screenSetTankAutoHideGunsightCS(cs, s_keySetupAutoGunsight);
        s_keySetupWaiting = ksNone;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        s_keySetupWaiting = ksNone;
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
    if (!ImGui::Begin("Settings", pOpen, flags)) {
        ImGui::End();
        return;
    }

    /* File actions — tablet/mobile only (desktop has menu bar) */
    if (uiModeIsTablet()) {
        if (ImGui::Button("Save Map", ImVec2(-1, 0))) {
            windowSaveMap(cs);
            s_showSettings = false;
        }
        if (ImGui::Button("Leave Game", ImVec2(-1, 0))) {
            windowNewGame();
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    /* ---- Player ---- */
    if (ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Player Name:");
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
        if (ImGui::Button("Apply##name")) {
            s_settingsNameBuf[32] = '\0';
            utilStripName(s_settingsNameBuf);
            if (s_settingsNameBuf[0] != '\0' && s_settingsNameBuf[0] != '*') {
                screenSetPlayerNameCS(cs, s_settingsNameBuf);
            }
        }

        if (!uiModeIsTablet()) {
            ImGui::Spacing();
            imguiWinbolonetDrawSection(true);
        }

#ifndef __ANDROID__
        if (!uiModeIsTablet()) {
            ImGui::Spacing();
            if (ImGui::Button("Set Keys...")) {
                sdl3ImguiShowKeySetup();
            }
        }
#endif
    }

    /* ---- Display ---- */
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        /* Frame Rate — not shown in tablet mode */
        if (!uiModeIsTablet()) {
            const char *frLabels[] = { "60", "50", "30", "20", "15", "12", "10" };
            int frValues[] = { FRAME_RATE_60, FRAME_RATE_50, FRAME_RATE_30,
                               FRAME_RATE_20, FRAME_RATE_15, FRAME_RATE_12, FRAME_RATE_10 };
            int curFrIdx = 2; /* default to 30 */
            for (int i = 0; i < 7; i++) {
                if (frameRate == frValues[i]) { curFrIdx = i; break; }
            }
            ImGui::Text("Frame Rate:");
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
            const char *zoomLabels[] = { "Normal", "Double", "Quad", "Custom" };
            BYTE zoomValues[] = { ZOOM_FACTOR_NORMAL, ZOOM_FACTOR_DOUBLE, ZOOM_FACTOR_QUAD, ZOOM_FACTOR_CUSTOM };
            int curZoomIdx = 0;
            for (int i = 0; i < 4; i++) {
                if (zoomFactor == zoomValues[i]) { curZoomIdx = i; break; }
            }
            ImGui::Text("Window Size:");
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
                if (ImGui::Checkbox("Hide Main View", &hmv)) {
                    windowHideMainView_toggle();
                }
            }
        }
#endif

        {
            bool as = (bool)autoScrollingEnabled;
            if (ImGui::Checkbox("Automatic Scrolling", &as)) {
                windowAutomaticScrolling_toggle(cs);
            }
        }
        {
            bool gs = (bool)showGunsight;
            if (ImGui::Checkbox("Show Gunsight", &gs)) {
                windowShowGunsight_toggle(cs);
            }
        }

        if (uiModeIsTablet()) {
            bool relSteering = !inputTouchGetAbsoluteSteering();
            if (ImGui::Checkbox("Relative Steering", &relSteering)) {
                inputTouchSetAbsoluteSteering(!relSteering);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When off, joystick points the tank directly.\nWhen on, joystick turns left/right relative to tank.");
            }
        }

#ifndef __ANDROID__
        if (!uiModeIsTablet()) {
            bool tabletMode = false;
            if (ImGui::Checkbox("Tablet UI Mode", &tabletMode)) {
                uiModeSet(UI_MODE_TABLET);
            }
        }
#endif
    }

    /* ---- Labels ---- */
    if (ImGui::CollapsingHeader("Labels", ImGuiTreeNodeFlags_DefaultOpen)) {
        /* Message Sender Names */
        ImGui::Text("Message Names:");
        ImGui::SameLine();
        {
            bool isShort = (labelMsg == lblShort);
            if (ImGui::RadioButton("Short##msg", isShort)) windowSetMessageLabelLen(cs, lblShort);
            ImGui::SameLine();
            if (ImGui::RadioButton("Long##msg", !isShort)) windowSetMessageLabelLen(cs, lblLong);
        }

        /* Tank Labels */
        ImGui::Text("Tank Labels:");
        ImGui::SameLine();
        {
            if (ImGui::RadioButton("None##tank", labelTank == lblNone))  windowSetTankLabelLen(cs, lblNone);
            ImGui::SameLine();
            if (ImGui::RadioButton("Short##tank", labelTank == lblShort)) windowSetTankLabelLen(cs, lblShort);
            ImGui::SameLine();
            if (ImGui::RadioButton("Long##tank", labelTank == lblLong))  windowSetTankLabelLen(cs, lblLong);
        }
        {
            bool noSelf = !(bool)labelSelf;
            if (ImGui::Checkbox("Don't label own tank", &noSelf)) {
                windowLabelOwnTank_toggle(cs);
            }
        }

        {
            bool pl = (bool)showPillLabels;
            if (ImGui::Checkbox("Pillbox Labels", &pl)) {
                windowShowPillLabels_toggle(cs);
            }
        }
        {
            bool bl = (bool)showBaseLabels;
            if (ImGui::Checkbox("Refuelling Base Labels", &bl)) {
                windowShowBaseLabels_toggle(cs);
            }
        }
    }

    /* ---- Sound ---- */
    if (ImGui::CollapsingHeader("Sound", ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            bool se = (bool)soundEffects;
            if (ImGui::Checkbox("Sound Effects", &se)) {
                windowSoundEffects_toggle();
            }
        }
        if (!uiModeIsTablet()) {
            bool bg = (bool)backgroundSound;
            if (ImGui::Checkbox("Background Sound", &bg)) {
                windowBackgroundSoundChange_toggle();
            }
        }
        if (!uiModeIsTablet()) {
            bool sk = (bool)useSoundKeepalive;
            if (ImGui::Checkbox("Sound Keepalive", &sk)) {
                windowSoundKeepalive();
            }
        }
    }

    /* ---- Messages ---- */
    if (ImGui::CollapsingHeader("Messages", ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            bool nw = (bool)showNewswireMessages;
            if (ImGui::Checkbox("Newswire Messages", &nw)) {
                windowMenuNewswire_toggle(cs);
            }
        }
        {
            bool am = (bool)showAssistantMessages;
            if (ImGui::Checkbox("Assistant Messages", &am)) {
                windowMenuAssistant_toggle(cs);
            }
        }
        {
            bool ai = (bool)showAIMessages;
            if (ImGui::Checkbox("AI Brain Messages", &ai)) {
                windowMenuAI_toggle(cs);
            }
        }
        {
            bool ns = (bool)showNetworkStatusMessages;
            if (ImGui::Checkbox("Network Status Messages", &ns)) {
                windowMenuNetwork_toggle(cs);
            }
        }
        {
            bool nd = (bool)showNetworkDebugMessages;
            if (ImGui::Checkbox("Network Debug Messages", &nd)) {
                windowMenuNetworkDebug_toggle(cs);
            }
        }
    }

    /* ---- Graphics ---- */
    imguiSettingsDrawGraphicsSection(s_renderer);

    /* ---- Game ---- */
    if (ImGui::CollapsingHeader("Game")) {
        {
            bool anp = (bool)allowNewPlayers;
            if (ImGui::Checkbox("Allow New Players", &anp)) {
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
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New"))                         windowNewGame();
        if (ImGui::MenuItem("Save Map", "Ctrl+S"))          windowSaveMap(cs);
        ImGui::Separator();
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        if (!uiModeIsTablet()) {
            if (ImGui::MenuItem("Game Info",    nullptr, s_popGameInfo.open))  togglePopOut(&s_popGameInfo, "Game Info", 320, 200);
            if (ImGui::MenuItem("System Info",  nullptr, s_popSysInfo.open))   togglePopOut(&s_popSysInfo, "System Info", 300, 210);
            if (ImGui::MenuItem("Network Info", nullptr, s_popNetInfo.open))   togglePopOut(&s_popNetInfo, "Network Info", 360, 420);
        } else {
#endif
            if (ImGui::MenuItem("Game Info",    nullptr, s_showGameInfo))  s_showGameInfo  = !s_showGameInfo;
            if (ImGui::MenuItem("System Info",  nullptr, s_showSysInfo))   s_showSysInfo   = !s_showSysInfo;
            if (ImGui::MenuItem("Network Info", nullptr, s_showNetInfo))   { if (!s_showNetInfo) pingGraphReset(); s_showNetInfo = !s_showNetInfo; }
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))                        windowSetQuitting();
        ImGui::EndMenu();
    }

    /* ---- Edit ---------------------------------------- */
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::BeginMenu("Frame Rate")) {
            if (ImGui::MenuItem("60", nullptr, frameRate == FRAME_RATE_60)) windowSetFrameRate(FRAME_RATE_60, true);
            if (ImGui::MenuItem("50", nullptr, frameRate == FRAME_RATE_50)) windowSetFrameRate(FRAME_RATE_50, true);
            if (ImGui::MenuItem("30", nullptr, frameRate == FRAME_RATE_30)) windowSetFrameRate(FRAME_RATE_30, true);
            if (ImGui::MenuItem("20", nullptr, frameRate == FRAME_RATE_20)) windowSetFrameRate(FRAME_RATE_20, true);
            if (ImGui::MenuItem("15", nullptr, frameRate == FRAME_RATE_15)) windowSetFrameRate(FRAME_RATE_15, true);
            if (ImGui::MenuItem("12", nullptr, frameRate == FRAME_RATE_12)) windowSetFrameRate(FRAME_RATE_12, true);
            if (ImGui::MenuItem("10", nullptr, frameRate == FRAME_RATE_10)) windowSetFrameRate(FRAME_RATE_10, true);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window Size")) {
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

            ImGui::BeginDisabled(!fit1x);
            if (ImGui::MenuItem("Normal", nullptr, zoomFactor == ZOOM_FACTOR_NORMAL)) s_pendingZoom = ZOOM_FACTOR_NORMAL;
            if (!fit1x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Requires %dx%d - exceeds display", 1 * SDL3_SCREEN_W, 1 * SDL3_SCREEN_H + MENU_BAR_HEIGHT);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!fit2x);
            if (ImGui::MenuItem("Double", nullptr, zoomFactor == ZOOM_FACTOR_DOUBLE)) s_pendingZoom = ZOOM_FACTOR_DOUBLE;
            if (!fit2x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Requires %dx%d - exceeds display", 2 * SDL3_SCREEN_W, 2 * SDL3_SCREEN_H + MENU_BAR_HEIGHT);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!fit3x);
            if (ImGui::MenuItem("Triple", nullptr, zoomFactor == ZOOM_FACTOR_TRIPLE)) s_pendingZoom = ZOOM_FACTOR_TRIPLE;
            if (!fit3x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Requires %dx%d - exceeds display", 3 * SDL3_SCREEN_W, 3 * SDL3_SCREEN_H + MENU_BAR_HEIGHT);
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!fit4x);
            if (ImGui::MenuItem("Quad",   nullptr, zoomFactor == ZOOM_FACTOR_QUAD))   s_pendingZoom = ZOOM_FACTOR_QUAD;
            if (!fit4x && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Requires %dx%d - exceeds display", 4 * SDL3_SCREEN_W, 4 * SDL3_SCREEN_H + MENU_BAR_HEIGHT);
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Custom (Resizable)", nullptr, zoomFactor == ZOOM_FACTOR_CUSTOM)) s_pendingZoom = ZOOM_FACTOR_CUSTOM;
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Automatic Scrolling", "Ctrl+A", (bool)autoScrollingEnabled)) windowAutomaticScrolling_toggle(cs);
        if (ImGui::MenuItem("Show Gunsight",        "Ctrl+G", (bool)showGunsight))        windowShowGunsight_toggle(cs);

        if (ImGui::BeginMenu("Message Sender Names")) {
            if (ImGui::MenuItem("Short", nullptr, labelMsg == lblShort)) windowSetMessageLabelLen(cs, lblShort);
            if (ImGui::MenuItem("Long",  nullptr, labelMsg == lblLong))  windowSetMessageLabelLen(cs, lblLong);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tank Labels")) {
            if (ImGui::MenuItem("None",                 "Ctrl+1", labelTank == lblNone))  windowSetTankLabelLen(cs, lblNone);
            if (ImGui::MenuItem("Short",                "Ctrl+2", labelTank == lblShort)) windowSetTankLabelLen(cs, lblShort);
            if (ImGui::MenuItem("Long",                 "Ctrl+3", labelTank == lblLong))  windowSetTankLabelLen(cs, lblLong);
            if (ImGui::MenuItem("Don't label own tank", nullptr, !(bool)labelSelf))       windowLabelOwnTank_toggle(cs);
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Pillbox Labels",         "Ctrl+P", (bool)showPillLabels)) windowShowPillLabels_toggle(cs);
        if (ImGui::MenuItem("Refuelling Base Labels", "Ctrl+B", (bool)showBaseLabels)) windowShowBaseLabels_toggle(cs);
        ImGui::Separator();
        if (ImGui::MenuItem("Hide Main View",         "Ctrl+H", (bool)hideMainView))   windowHideMainView_toggle();
        ImGui::Separator();
        {
            const char *presetLabel = (g_currentDevicePreset >= 0 && g_currentDevicePreset < s_numDevicePresets)
                ? s_devicePresets[g_currentDevicePreset].name : "Desktop";
            char deviceMenuItem[64];
            SDL_snprintf(deviceMenuItem, sizeof(deviceMenuItem), "Device: %s", presetLabel);
            if (ImGui::MenuItem(deviceMenuItem, "Ctrl+T")) {
                dialogCycleDevicePreset(sdl3DrawGetWindow());
            }
        }

        ImGui::EndMenu();
    }

    /* ---- WinBolo ------------------------------------- */
    if (ImGui::BeginMenu("WinBolo")) {
        if (ImGui::MenuItem("Allow New Players",       nullptr, (bool)allowNewPlayers))           windowMenuAllowNewPlayers_toggle(cs);
        if (ImGui::MenuItem("Set Keys",                "Ctrl+K"))                                 sdl3ImguiShowKeySetup();
        if (ImGui::MenuItem("Change Player Name"))                                                 s_showChangeName = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Sound Effects",           nullptr, (bool)soundEffects))              windowSoundEffects_toggle();
        if (ImGui::MenuItem("Background Sound",        nullptr, (bool)backgroundSound))           windowBackgroundSoundChange_toggle();
        if (ImGui::MenuItem("Sound keepalive",         nullptr, (bool)useSoundKeepalive))         windowSoundKeepalive();
        ImGui::Separator();
        if (ImGui::MenuItem("Newswire Messages",       nullptr, (bool)showNewswireMessages))      windowMenuNewswire_toggle(cs);
        if (ImGui::MenuItem("Assistant Messages",      nullptr, (bool)showAssistantMessages))     windowMenuAssistant_toggle(cs);
        if (ImGui::MenuItem("AI Brain Messages",       nullptr, (bool)showAIMessages))            windowMenuAI_toggle(cs);
        if (ImGui::MenuItem("Network Status Messages", nullptr, (bool)showNetworkStatusMessages)) windowMenuNetwork_toggle(cs);
        if (ImGui::MenuItem("Network Debug Messages",  nullptr, (bool)showNetworkDebugMessages))  windowMenuNetworkDebug_toggle(cs);
        ImGui::Separator();
        if (ImGui::MenuItem("Request Alliance",        "Ctrl+R"))                                 screenRequestAllianceCS(cs);
        if (ImGui::MenuItem("Leave Alliance"))                                                     screenLeaveAllianceCS(cs);
        ImGui::Separator();
        if (ImGui::MenuItem("Settings..."))                                                        sdl3ImguiShowSettings();
        ImGui::EndMenu();
    }

    /* ---- Players ------------------------------------- */
    if (ImGui::BeginMenu("Players")) {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        if (!uiModeIsTablet()) {
            if (ImGui::MenuItem("Send Message", "Ctrl+M", s_popSendMsg.open))
                togglePopOut(&s_popSendMsg, "Send Message", 400, 200);
        } else {
#endif
            if (ImGui::MenuItem("Send Message", "Ctrl+M")) {
                s_showSendMsg = !s_showSendMsg;
                if (s_showSendMsg) s_sendMsgFocusInput = true;
            }
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        ImGui::Separator();
        if (ImGui::Selectable("Select All", false, ImGuiSelectableFlags_DontClosePopups))              screenCheckAllNonePlayersCS(cs, true);
        if (ImGui::Selectable("Select None", false, ImGuiSelectableFlags_DontClosePopups))             screenCheckAllNonePlayersCS(cs, false);
        if (ImGui::Selectable("Select Allies", false, ImGuiSelectableFlags_DontClosePopups))           screenCheckAlliedPlayersCS(cs);
        if (ImGui::Selectable("Select Nearby Tanks", false, ImGuiSelectableFlags_DontClosePopups))     screenCheckNearbyPlayersCS(cs);
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
                /* Refresh ping/WBN from player struct each frame */
                s_playerPing[i] = playersGetPing(&cs->sim.plyrs, (BYTE)i);
                s_playerWbn[i]  = playersGetWbnParticipant(&cs->sim.plyrs, (BYTE)i);

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
                ImGuiContext &g = *GImGui;
                float checkSz = g.FontSize * 0.866f;
                float iconW = (float)WBN_ICON_SIZE;
                float pingWidth = ImGui::CalcTextSize(pingStr).x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float iconsWidth = 0.0f;
                if (s_playerWbn[i] && s_iconGlobe)  iconsWidth += iconW + spacing;
                if (s_playerSteam[i] && s_iconSteam) iconsWidth += iconW + spacing;
                float rightWidth = iconsWidth + pingWidth + spacing + checkSz;

                /* Selectable player name (no highlight) */
                char selectLabel[64];
                snprintf(selectLabel, sizeof(selectLabel), "%s##sel%d", label, i);
                if (ImGui::Selectable(selectLabel, false, ImGuiSelectableFlags_DontClosePopups, ImVec2(fullWidth - rightWidth - spacing, 0))) {
                    screenTogglePlayerCheckStateCS(cs, (BYTE)i);
                }

                /* Right-aligned WBN/Steam icons */
                ImGui::SameLine(fullWidth - rightWidth);
                if (s_playerWbn[i] && s_iconGlobe) {
                    ImGui::Image((ImTextureID)s_iconGlobe, ImVec2(iconW, iconW));
                    ImGui::SameLine();
                }
                if (s_playerSteam[i] && s_iconSteam) {
                    ImGui::Image((ImTextureID)s_iconSteam, ImVec2(iconW, iconW));
                    ImGui::SameLine();
                }

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
                if (ImGui::MenuItem("Leave Alliance"))
                    screenLeaveAllianceCS(cs);
            } else {
                /* Not in an alliance — show Request */
                if (!canRequest || inCooldown) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Request Alliance")) {
                    screenRequestAllianceCS(cs);
                    s_allianceReqCooldownEnd = SDL_GetTicks() + ALLIANCE_REQ_WAIT_MS;
                }
                if (!canRequest || inCooldown) ImGui::EndDisabled();
            }
        }
        ImGui::EndMenu();
    }

    /* ---- Brains -------------------------------------- */
    if (ImGui::BeginMenu("Brains", screenGetAiTypeCS(cs) != aiNone)) {
        bool running = luaBrainIsRunning() != 0;
        int  runIdx  = luaBrainGetRunningIndex();

        /* Manual (stop brain) entry — checked when no brain is active */
        if (ImGui::MenuItem("Manual", nullptr, !running)) {
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
            if (ImGui::MenuItem("Settings...")) {
                /* Re-fetch on every open so values are current */
                luaBrainFreeSettings(s_brainSettings);
                s_brainSettings      = luaBrainGetSettings(&s_brainSettingsCount);
                s_brainSettingsOpen  = true;
            }
        }

        ImGui::EndMenu();
    }

    /* ---- Help ---------------------------------------- */
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Help"))  { /* TODO: open help file */ }
        if (ImGui::MenuItem("About")) s_showAbout = true;
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
            const char *labels[] = {"", "Normal", "Double", "Triple", "Quad"};
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
    io.IniFilename  = nullptr; /* no imgui.ini — avoid filesystem clutter */

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    imguiLoadBoloFont(18.0f);

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
    }

    if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) return false;
    if (!ImGui_ImplSDLRenderer3_Init(renderer))               return false;

    flagsCreate(renderer);
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

        /* DEBUG: log touch/mouse events in tablet mode — remove after debugging */
        if (uiModeIsTablet()) {
            if (ev.type == SDL_EVENT_FINGER_DOWN || ev.type == SDL_EVENT_FINGER_UP) {
                SDL_Log("TAP-DBG: FINGER %s x=%.2f y=%.2f",
                        ev.type == SDL_EVENT_FINGER_DOWN ? "DOWN" : "UP",
                        ev.tfinger.x, ev.tfinger.y);
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                SDL_Log("TAP-DBG: MOUSE %s btn=%d x=%.1f y=%.1f which=%u winID=%u",
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

        /* Ctrl+key shortcuts — only for events on the main window */
        if (ev.type == SDL_EVENT_KEY_DOWN &&
            ev.key.windowID == SDL_GetWindowID(s_window) &&
            (ev.key.mod & SDL_KMOD_CTRL) != 0) {
            switch (ev.key.scancode) {
            case SDL_SCANCODE_M:
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
                if (!uiModeIsTablet()) {
                    togglePopOut(&s_popSendMsg, "Send Message", 400, 200);
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

        /* Forward key events to input system for event-driven mine key tracking.
         * Must happen before the ImGui swallow so key-up events are never lost. */
        if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
            keyItems ki;
            windowGetKeys(&ki);
            inputButtonInput(&ki, ev.key.scancode, (ev.type == SDL_EVENT_KEY_DOWN));
        }

        /* Ctrl-O toggles hitbox debug overlay (override mode).
         * Ctrl-[ slows game speed (cap +200ms extra delay/frame);
         * Ctrl-] speeds up (capped at normal). */
        if (ev.type == SDL_EVENT_KEY_DOWN
            && (ev.key.mod & SDL_KMOD_CTRL)) {
            if (ev.key.key == SDLK_O) {
                overrideModeToggle();
                continue;
            }
            if (ev.key.key == SDLK_LEFTBRACKET) {
                overrideModeSlower();
                continue;
            }
            if (ev.key.key == SDLK_RIGHTBRACKET) {
                overrideModeFaster();
                continue;
            }
        }

        /* Override-mode scroll-wheel zoom (1× .. 6×).  Anchor the
         * next zoom step on the cursor so the world point under the
         * cursor stays under the cursor. */
        if (overrideModeIsOn() && ev.type == SDL_EVENT_MOUSE_WHEEL) {
            overrideModeSetZoomAnchor((int)ev.wheel.mouse_x,
                                      (int)ev.wheel.mouse_y);
            if (ev.wheel.y > 0) overrideModeZoomIn();
            else if (ev.wheel.y < 0) overrideModeZoomOut();
            continue;
        }

        /* Override-mode mouse-drag pan (middle or right button). */
        if (overrideModeIsOn()) {
            static bool s_overrideDragging = false;
            static float s_dragLastX = 0.0f;
            static float s_dragLastY = 0.0f;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                && (ev.button.button == SDL_BUTTON_MIDDLE
                 || ev.button.button == SDL_BUTTON_RIGHT)) {
                s_overrideDragging = true;
                s_dragLastX = ev.button.x;
                s_dragLastY = ev.button.y;
                continue;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP
                && (ev.button.button == SDL_BUTTON_MIDDLE
                 || ev.button.button == SDL_BUTTON_RIGHT)) {
                s_overrideDragging = false;
                continue;
            }
            if (s_overrideDragging && ev.type == SDL_EVENT_MOUSE_MOTION) {
                float dx = ev.motion.x - s_dragLastX;
                float dy = ev.motion.y - s_dragLastY;
                s_dragLastX = ev.motion.x;
                s_dragLastY = ev.motion.y;
                /* Drag right → world slides right under cursor → edgeX
                 * decreases (subtracts).  overrideModePan adds to edgeX,
                 * so negate the delta. */
                overrideModePan(-(int)dx, -(int)dy);
                continue;
            }
        }

        /* While override mode is on, arrow keys pan the world view.
         * 32 screen pixels per press; key-repeat counts so holding scrolls. */
        if (overrideModeIsOn()
            && ev.type == SDL_EVENT_KEY_DOWN
            && !(ev.key.mod & SDL_KMOD_CTRL)) {
            const int kPanStep = 32;
            if (ev.key.key == SDLK_LEFT) {
                overrideModePan(-kPanStep, 0);
                continue;
            }
            if (ev.key.key == SDLK_RIGHT) {
                overrideModePan(kPanStep, 0);
                continue;
            }
            if (ev.key.key == SDLK_UP) {
                overrideModePan(0, -kPanStep);
                continue;
            }
            if (ev.key.key == SDLK_DOWN) {
                overrideModePan(0, kPanStep);
                continue;
            }
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
                SDL_Log("[URL] Received winbolo:// link while running: %s", url);
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

void sdl3ImguiPumpAndRender(ClientSim *cs) {
    if (!s_window || !s_renderer) return;

    /* Build the ImGui frame */
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    dialogResetTextInputArea(s_window);

    /* Override ImGui's DisplaySize for tablet mode only.
       In tablet mode, SDL logical presentation scales the whole window,
       so ImGui needs to render in that coordinate space.
       In desktop mode, ImGui renders at native window coordinates (no override)
       because the game is blitted to a scaled rect, not the whole window. */
    if (uiModeIsTablet()) {
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

    if (uiModeIsTablet()) {
        sdl3ImguiTabletOverlay(cs);
    } else {
        renderMenuBar(cs);
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
                "System Info", "Network Info", "Game Info",
                "Send Message", "Players", "Settings",
                "Brain Settings", "Alliance Request",
                "About WinBolo", "Change Player Name",
                "Password Required", "Key Setup",
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
        /* Snap Graphics dropdown back to active theme on each open. */
        imguiSettingsResetGraphicsSelection();
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
        SDL_Log("[FLAGS] sdl3ImguiSetPlayer: player=%d name='%s' country='%s' (0x%02X 0x%02X)", playerNum, name, s_playerCountry[playerNum], (unsigned char)countryCode[0], (unsigned char)countryCode[1]);
    } else {
        s_playerCountry[playerNum][0] = '\0';
        SDL_Log("[FLAGS] sdl3ImguiSetPlayer: player=%d name='%s' country=NULL", playerNum, name);
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
    s_playerWbn[playerNum] = false;
    s_playerSteam[playerNum] = false;
}

void sdl3ImguiUpdatePlayerMeta(unsigned char playerNum, uint16_t ping, bool wbn, bool steam) {
    if (playerNum >= MAX_PLAYERS) return;
    s_playerPing[playerNum] = ping;
    s_playerWbn[playerNum]  = wbn;
    s_playerSteam[playerNum] = steam;
}

SDL_Texture *sdl3ImguiGetGlobeIcon(void) {
    ensureWbnIconsLoaded();
    return s_iconGlobe;
}

SDL_Texture *sdl3ImguiGetSteamIcon(void) {
    ensureWbnIconsLoaded();
    return s_iconSteam;
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
    popOutDestroy(&s_popSysInfo);
    popOutDestroy(&s_popNetInfo);
    popOutDestroy(&s_popGameInfo);
    popOutDestroy(&s_popSendMsg);
    flagsDestroy();
    if (s_iconGlobe) { SDL_DestroyTexture(s_iconGlobe); s_iconGlobe = nullptr; }
    if (s_iconSteam) { SDL_DestroyTexture(s_iconSteam); s_iconSteam = nullptr; }
    s_wbnIconsLoaded = false;
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
