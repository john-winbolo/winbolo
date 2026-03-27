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
}

/* Include input.h for keyItems — SDL3 already included, safe here */
extern "C" {
#include "input.h"
#include "input_touch.h"
#include "../ui_mode.h"
}

#include "sdl3imgui_tablet.h"

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
#define ZOOM_FACTOR_QUAD   4
#endif

/* -------------------------------------------------------
 * External C linkage: function + globals defined in
 * winbolo.c / gamefront.c that we need for the menu bar.
 * ------------------------------------------------------- */

/* Direct menu command handlers in winbolo.c */
extern "C" void windowShowGunsight_toggle(struct ClientSim *cs);
extern "C" void windowAutomaticScrolling_toggle(void);
extern "C" void windowShowPillLabels_toggle(struct ClientSim *cs);
extern "C" void windowShowBaseLabels_toggle(struct ClientSim *cs);
extern "C" void windowSoundEffects_toggle(void);
extern "C" void windowBackgroundSoundChange_toggle(void);
extern "C" void windowSoundKeepalive(void);
extern "C" void windowMenuAllowNewPlayers_toggle(struct ClientSim *cs);
extern "C" void windowMenuNewswire_toggle(void);
extern "C" void windowMenuAssistant_toggle(void);
extern "C" void windowMenuAI_toggle(void);
extern "C" void windowMenuNetwork_toggle(void);
extern "C" void windowMenuNetworkDebug_toggle(void);
extern "C" void windowHideMainView_toggle(void);
extern "C" void windowLabelOwnTank_toggle(void);
extern "C" void imguiWinbolonetShow(void);
extern "C" void windowSetMessageLabelLen(labelLen newLen);
extern "C" void windowSetTankLabelLen(labelLen newLen);
extern "C" void windowSetFrameRate(int newFrameRate, bool setTimer);
extern "C" void windowZoomChange(BYTE amount);
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
   the frame ends. 0 = no pending change. */
static BYTE s_pendingZoom = 0;

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

/* Settings panel state */
static bool s_showSettings       = false;
static char s_settingsNameBuf[33] = "";  /* PLAYER_NAME_LEN = 33 */
static bool s_pendingWbnDialog   = false;

/* Modal dialog state */
static bool s_showAbout          = false;

static bool s_showChangeName     = false;
static char s_changeNameBuf[33]  = "";  /* PLAYER_NAME_LEN = 33 */

static bool s_showAllianceOpen   = false;
static char s_alliancePlayerName[33] = "";
static BYTE s_alliancePlayerNum  = 0;

static bool s_showPasswordOpen   = false;
static char s_passwordBuf[36]    = "";  /* MAP_STR_SIZE = 36 */

/* Key Setup modal state */
/* Which binding is currently being captured; -1 = none */
enum KeySetupField {
    ksNone = -1,
    ksForward, ksBackward, ksTurnLeft, ksTurnRight,
    ksShoot, ksLayMine, ksGunIncrease, ksGunDecrease,
    ksTankView, ksPillView, ksAllyView, ksLGMView, ksBaseView,
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
static void renderNetInfoContent(ClientSim *cs) {
    char str[256];
    int  ping = 0, ppsec = 0, numErrors = 0;

    netGetServerAddressStr(cs, str);
    ImGui::Text("Server:      %s", str);

    /* Client in a networked game: prepend player location to port */
    if (cs->networkGameType != netSingle) {
        char addr[256];
        players *plrs = &cs->sim.plyrs;
        playersGetPlayerLocation(plrs, playersGetSelf(plrs), addr);
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
    /* Prefer ping from new UDP transport when active */
    {
        Transport *tp = gameFrontGetTransport();
        if (tp) {
            uint16_t udpPing = transportUdpClientGetPing(tp);
            if (udpPing > 0) ping = (int)udpPing;
        }
    }
    ImGui::Separator();
    ImGui::Text("Status:      %s", str);
    ImGui::Text("Server ping: %d ms", ping);
    ImGui::Text("Packets/sec: %d", ppsec);
    ImGui::Text("Net errors:  %d", numErrors);
}

static void renderNetInfoPanel(ClientSim *cs) {
    if (!s_showNetInfo || s_popNetInfo.open) return;

    ImGui::SetNextWindowSize(ImVec2(360, 185), ImGuiCond_FirstUseEver);
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
        /* Select all text so the user can overwrite immediately after cooldown */
        ImGui::SetKeyboardFocusHere(-1);
    }
}

static void renderSendMsgPanel(ClientSim *cs) {
    if (!s_showSendMsg || s_popSendMsg.open) return;

    ImGui::SetNextWindowSize(ImVec2(350, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Send Message", &s_showSendMsg,
                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
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

    ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Players", &s_showPlayersPanel)) {
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
    BYTE self = playersGetSelf(plrs);
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

    /* Player list */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!s_playerEnabled[i]) continue;

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
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
            ImGui::TextUnformatted("W");
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        /* Steam participant icon */
        if (s_playerSteam[i]) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.4f, 1.0f));
            ImGui::TextUnformatted("S");
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }

        /* Refresh ping/WBN/Steam */
        s_playerPing[i] = playersGetPing(&cs->sim.plyrs, (BYTE)i);
        s_playerWbn[i]  = playersGetWbnParticipant(&cs->sim.plyrs, (BYTE)i);
        s_playerSteam[i] = playersGetSteamParticipant(&cs->sim.plyrs, (BYTE)i);

        const char *label = s_playerName[i][0] ? s_playerName[i] : nullptr;
        char defLabel[8];
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
        case ksAllyView:   return &ki->kiAllyView;
        case ksLGMView:    return &ki->kiLGMView;
        case ksBaseView:   return &ki->kiBaseView;
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
    keySetupRow("Ally View",   ksAllyView);
    keySetupRow("LGM View",    ksLGMView);
    keySetupRow("Base View",   ksBaseView);
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

    ImGui::SetNextWindowSize(ImVec2(460, 580), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &s_showSettings)) {
        ImGui::End();
        return;
    }

    /* File actions — tablet/mobile only (desktop has menu bar) */
    if (uiModeIsTablet()) {
        if (ImGui::Button("New Game", ImVec2(-1, 0))) {
            windowNewGame();
            s_showSettings = false;
        }
        if (ImGui::Button("Save Map", ImVec2(-1, 0))) {
            windowSaveMap(cs);
            s_showSettings = false;
        }
#ifndef __EMSCRIPTEN__
        if (ImGui::Button("Exit", ImVec2(-1, 0))) {
            windowQuit();
        }
#endif
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

        ImGui::Spacing();
        if (ImGui::Button("Sign in to WBN...")) {
            s_pendingWbnDialog = true;
        }

#ifndef __ANDROID__
        ImGui::Spacing();
        if (ImGui::Button("Set Keys...")) {
            sdl3ImguiShowKeySetup();
        }
#endif
    }

    /* ---- Display ---- */
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        /* Frame Rate */
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

#ifndef __ANDROID__
        /* Window Size — desktop only */
        const char *zoomLabels[] = { "Normal", "Double", "Quad" };
        BYTE zoomValues[] = { ZOOM_FACTOR_NORMAL, ZOOM_FACTOR_DOUBLE, ZOOM_FACTOR_QUAD };
        int curZoomIdx = 0;
        for (int i = 0; i < 3; i++) {
            if (zoomFactor == zoomValues[i]) { curZoomIdx = i; break; }
        }
        ImGui::Text("Window Size:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::BeginCombo("##windowsize", zoomLabels[curZoomIdx])) {
            for (int i = 0; i < 3; i++) {
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
#endif

        {
            bool as = (bool)autoScrollingEnabled;
            if (ImGui::Checkbox("Automatic Scrolling", &as)) {
                windowAutomaticScrolling_toggle();
            }
        }
        {
            bool gs = (bool)showGunsight;
            if (ImGui::Checkbox("Show Gunsight", &gs)) {
                windowShowGunsight_toggle(cs);
            }
        }

#ifndef __ANDROID__
        {
            bool tabletMode = uiModeIsTablet();
            if (ImGui::Checkbox("Tablet UI Mode", &tabletMode)) {
                uiModeSet(tabletMode ? UI_MODE_TABLET : UI_MODE_DESKTOP);
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
            if (ImGui::RadioButton("Short##msg", isShort)) windowSetMessageLabelLen(lblShort);
            ImGui::SameLine();
            if (ImGui::RadioButton("Long##msg", !isShort)) windowSetMessageLabelLen(lblLong);
        }

        /* Tank Labels */
        ImGui::Text("Tank Labels:");
        ImGui::SameLine();
        {
            if (ImGui::RadioButton("None##tank", labelTank == lblNone))  windowSetTankLabelLen(lblNone);
            ImGui::SameLine();
            if (ImGui::RadioButton("Short##tank", labelTank == lblShort)) windowSetTankLabelLen(lblShort);
            ImGui::SameLine();
            if (ImGui::RadioButton("Long##tank", labelTank == lblLong))  windowSetTankLabelLen(lblLong);
        }
        {
            bool noSelf = !(bool)labelSelf;
            if (ImGui::Checkbox("Don't label own tank", &noSelf)) {
                windowLabelOwnTank_toggle();
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
        {
            bool bg = (bool)backgroundSound;
            if (ImGui::Checkbox("Background Sound", &bg)) {
                windowBackgroundSoundChange_toggle();
            }
        }
#ifndef __ANDROID__
        {
            bool sk = (bool)useSoundKeepalive;
            if (ImGui::Checkbox("Sound Keepalive", &sk)) {
                windowSoundKeepalive();
            }
        }
#endif
    }

    /* ---- Messages ---- */
    if (ImGui::CollapsingHeader("Messages", ImGuiTreeNodeFlags_DefaultOpen)) {
        {
            bool nw = (bool)showNewswireMessages;
            if (ImGui::Checkbox("Newswire Messages", &nw)) {
                windowMenuNewswire_toggle();
            }
        }
        {
            bool am = (bool)showAssistantMessages;
            if (ImGui::Checkbox("Assistant Messages", &am)) {
                windowMenuAssistant_toggle();
            }
        }
        {
            bool ai = (bool)showAIMessages;
            if (ImGui::Checkbox("AI Brain Messages", &ai)) {
                windowMenuAI_toggle();
            }
        }
        {
            bool ns = (bool)showNetworkStatusMessages;
            if (ImGui::Checkbox("Network Status Messages", &ns)) {
                windowMenuNetwork_toggle();
            }
        }
        {
            bool nd = (bool)showNetworkDebugMessages;
            if (ImGui::Checkbox("Network Debug Messages", &nd)) {
                windowMenuNetworkDebug_toggle();
            }
        }
    }

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
            if (ImGui::MenuItem("Network Info", nullptr, s_popNetInfo.open))   togglePopOut(&s_popNetInfo, "Network Info", 360, 185);
        } else {
#endif
            if (ImGui::MenuItem("Game Info",    nullptr, s_showGameInfo))  s_showGameInfo  = !s_showGameInfo;
            if (ImGui::MenuItem("System Info",  nullptr, s_showSysInfo))   s_showSysInfo   = !s_showSysInfo;
            if (ImGui::MenuItem("Network Info", nullptr, s_showNetInfo))   s_showNetInfo   = !s_showNetInfo;
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IOS)
        }
#endif
        ImGui::Separator();
        if (ImGui::MenuItem("Exit"))                        windowQuit();
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
            if (ImGui::MenuItem("Normal", nullptr, zoomFactor == ZOOM_FACTOR_NORMAL)) s_pendingZoom = ZOOM_FACTOR_NORMAL;
            if (ImGui::MenuItem("Double", nullptr, zoomFactor == ZOOM_FACTOR_DOUBLE)) s_pendingZoom = ZOOM_FACTOR_DOUBLE;
            if (ImGui::MenuItem("Quad",   nullptr, zoomFactor == ZOOM_FACTOR_QUAD))   s_pendingZoom = ZOOM_FACTOR_QUAD;
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Automatic Scrolling", "Ctrl+A", (bool)autoScrollingEnabled)) windowAutomaticScrolling_toggle();
        if (ImGui::MenuItem("Show Gunsight",        "Ctrl+G", (bool)showGunsight))        windowShowGunsight_toggle(cs);

        if (ImGui::BeginMenu("Message Sender Names")) {
            if (ImGui::MenuItem("Short", nullptr, labelMsg == lblShort)) windowSetMessageLabelLen(lblShort);
            if (ImGui::MenuItem("Long",  nullptr, labelMsg == lblLong))  windowSetMessageLabelLen(lblLong);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tank Labels")) {
            if (ImGui::MenuItem("None",                 "Ctrl+1", labelTank == lblNone))  windowSetTankLabelLen(lblNone);
            if (ImGui::MenuItem("Short",                "Ctrl+2", labelTank == lblShort)) windowSetTankLabelLen(lblShort);
            if (ImGui::MenuItem("Long",                 "Ctrl+3", labelTank == lblLong))  windowSetTankLabelLen(lblLong);
            if (ImGui::MenuItem("Don't label own tank", nullptr, !(bool)labelSelf))       windowLabelOwnTank_toggle();
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
        if (ImGui::MenuItem("Newswire Messages",       nullptr, (bool)showNewswireMessages))      windowMenuNewswire_toggle();
        if (ImGui::MenuItem("Assistant Messages",      nullptr, (bool)showAssistantMessages))     windowMenuAssistant_toggle();
        if (ImGui::MenuItem("AI Brain Messages",       nullptr, (bool)showAIMessages))            windowMenuAI_toggle();
        if (ImGui::MenuItem("Network Status Messages", nullptr, (bool)showNetworkStatusMessages)) windowMenuNetwork_toggle();
        if (ImGui::MenuItem("Network Debug Messages",  nullptr, (bool)showNetworkDebugMessages))  windowMenuNetworkDebug_toggle();
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
        BYTE self = playersGetSelf(plrs);
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
            char defLabel[8];
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
                /* Measure right-side width: WBN + ping + checkmark (rightmost) */
                ImGuiContext &g = *GImGui;
                float checkSz = g.FontSize * 0.866f;
                float wbnWidth = ImGui::CalcTextSize(s_playerWbn[i] ? "W" : "-").x;
                float pingWidth = ImGui::CalcTextSize(pingStr).x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;
                float rightWidth = wbnWidth + spacing + pingWidth + spacing + checkSz;

                /* Selectable player name (no highlight) */
                char selectLabel[64];
                snprintf(selectLabel, sizeof(selectLabel), "%s##sel%d", label, i);
                if (ImGui::Selectable(selectLabel, false, ImGuiSelectableFlags_DontClosePopups, ImVec2(fullWidth - rightWidth - spacing, 0))) {
                    screenTogglePlayerCheckStateCS(cs, (BYTE)i);
                }

                /* Right-aligned WBN tick */
                ImGui::SameLine(fullWidth - rightWidth);
                if (s_playerWbn[i]) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.9f, 0.0f, 1.0f));
                    ImGui::TextUnformatted("W");
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::TextUnformatted("-");
                    ImGui::PopStyleColor();
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    s_mainImguiCtx = ImGui::GetCurrentContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr; /* no imgui.ini — avoid filesystem clutter */

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();

    /* Tablet mode: scale up ImGui for touch targets */
    if (uiModeIsTablet()) {
        io.FontGlobalScale = 1.8f;
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
        ImGuiStyle &style = ImGui::GetStyle();
        style.FramePadding      = ImVec2(12, 8);
        style.ItemSpacing       = ImVec2(12, 8);
        style.TouchExtraPadding = ImVec2(8, 8);
        style.ScrollbarSize     = 24.0f;
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
        ImGui_ImplSDL3_ProcessEvent(&ev);

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

        /* Route finger events to touch input system in tablet mode */
        if (uiModeIsTablet() &&
            (ev.type == SDL_EVENT_FINGER_DOWN ||
             ev.type == SDL_EVENT_FINGER_UP ||
             ev.type == SDL_EVENT_FINGER_MOTION)) {
            int ww, wh;
            SDL_GetWindowSize(s_window, &ww, &wh);
            inputTouchProcessEvent(&ev, ww, wh);
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
                windowAutomaticScrolling_toggle();
                continue;
            case SDL_SCANCODE_1:
                windowSetTankLabelLen(lblNone);
                continue;
            case SDL_SCANCODE_2:
                windowSetTankLabelLen(lblShort);
                continue;
            case SDL_SCANCODE_3:
                windowSetTankLabelLen(lblLong);
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

        if (ev.type == SDL_EVENT_QUIT) {
            windowSetQuitting();
        }
        /* Main window close button — SDL3 won't send SDL_EVENT_QUIT
           while pop-out windows are still open. */
        if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            ev.window.windowID == SDL_GetWindowID(s_window)) {
            windowSetQuitting();
        }

        /* Dispatch tap-style key actions (pill view, tank view) that are
         * not handled by the polling-based inputGetKeys(). */
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat &&
            ev.key.windowID == SDL_GetWindowID(s_window)) {
            windowKeyPressed(cs, (int)ev.key.scancode);
        }

        sdl3DrawHandleEvent(cs, &ev);
    }
}

void sdl3ImguiPumpAndRender(ClientSim *cs) {
    if (!s_window || !s_renderer) return;

    /* Build the ImGui frame */
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();

    /* In tablet mode, override ImGui's DisplaySize to match the SDL
       render logical presentation space so ImGui coordinates align
       with SDL rendering coordinates (game tiles, etc.). */
    if (uiModeIsTablet()) {
        int logW = 0, logH = 0;
        SDL_RendererLogicalPresentation logMode;
        SDL_GetRenderLogicalPresentation(s_renderer, &logW, &logH, &logMode);
        if (logW > 0 && logH > 0) {
            ImGuiIO &io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)logW, (float)logH);
        }
    }

    ImGui::NewFrame();

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

    /* Extra render callback (e.g. Android players panel) */
    if (s_extraRenderFn) {
        s_extraRenderFn(cs);
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
    if (s_pendingZoom != 0) {
        BYTE zoom = s_pendingZoom;
        s_pendingZoom = 0;
        windowZoomChange(zoom);
    }

    /* Deferred WBN dialog — runs its own ImGui context and event loop,
       so it must be called outside our frame. */
    if (s_pendingWbnDialog) {
        s_pendingWbnDialog = false;
        imguiWinbolonetShow();
        sdl3ImguiResetFrameState();
    }
}

void sdl3ImguiSetExtraRenderCallback(sdl3ImguiExtraRenderFn fn) {
    s_extraRenderFn = fn;
}

void sdl3ImguiShowSysInfo(bool open) {
    s_showSysInfo = open;
}
void sdl3ImguiShowNetInfo(bool open) {
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
    }
}
void sdl3ImguiShowSettings(void) {
    s_showSettings = !s_showSettings;
    if (s_showSettings) {
        s_settingsNameBuf[0] = '\0';
        gameFrontGetPlayerName(s_settingsNameBuf);
    }
}
void sdl3ImguiShowPlayersPanel(bool open) {
    s_showPlayersPanel = open;
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
    luaBrainFreeSettings(s_brainSettings);
    s_brainSettings      = nullptr;
    s_brainSettingsCount = 0;
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
