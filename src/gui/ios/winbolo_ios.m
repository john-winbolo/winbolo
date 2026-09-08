/*
 * winbolo_ios.m — iOS app entry point for WinBolo.
 * Full game loop with engine integration.
 *
 * Closely follows src/android/main_android.c + android_frontend.c.
 * Uses the server-authoritative ClientSim API with transport-based game ticks.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#import <AVFoundation/AVFoundation.h>
#ifdef HAVE_SENTRY
@import Sentry;
#endif
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

#include "frontend.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_render.h"
#include "input_packet.h"
#include "client_frontend_tick.h"
#include "gui_message.h"
#include "../../common/wb_log.h"
#include "server_sim.h"
#include "../../server/threads.h"
#include "../brainsHandler.h"
#include "../clientmutex.h"
#include "../gamefront.h"
#include "../lang.h"
#include "../winbolo.h"
#include "../sound.h"
#include "sdl3draw.h"
#include "../sdl3/build_cursor.h"
#include "../sdl3/sdl3imgui.h"
#include "../sdl3/luabrainshandler.h"
#include "../sdl3/dialog_backend.h"
#include "../sdl3/dialogs/imgui_messagebox.h"
#include "../mobile/touch_input.h"
#include "../ui_mode.h"
#include "../sdl3/input_touch.h"

extern ClientSim *humanSim;

/* -------------------------------------------------------
 * Crash-reporting preference (NSUserDefaults)
 * ------------------------------------------------------- */

bool iosCrashReportingGetEnabled(void) {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if ([defaults objectForKey:@"crash_reporting_enabled"] == nil) {
        return true; /* default to enabled */
    }
    return [defaults boolForKey:@"crash_reporting_enabled"];
}

void iosCrashReportingSetEnabled(bool enabled) {
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    [defaults setBool:enabled forKey:@"crash_reporting_enabled"];
    [defaults synchronize];
}

/* -------------------------------------------------------
 * Globals (matching winbolo.h externs)
 * ------------------------------------------------------- */

bool isTutorial = FALSE;
int frameRate = FRAME_RATE_30;

bool showGunsight = FALSE;
bool soundEffects = TRUE;
bool backgroundSound = FALSE;
bool useSoundKeepalive = FALSE;
int  soundVolume = 50;
bool allowNewPlayers = TRUE;

bool showNewswireMessages = TRUE;
bool showAssistantMessages = TRUE;
bool showAIMessages = FALSE;
bool showNetworkStatusMessages = TRUE;
bool showNetworkDebugMessages = FALSE;

bool autoScrollingEnabled = FALSE;
bool smoothScrollingEnabled = FALSE;  /* Touch platform: arrow-key smooth scroll inactive */
BYTE zoomFactor = ZOOM_FACTOR_NORMAL;
bool letterboxBarsGray = FALSE;       /* gray vs black letterbox bars (sdl3draw) */

bool showPillLabels = FALSE;
bool showBaseLabels = FALSE;
bool labelSelf = TRUE;
labelLen labelMsg = lblShort;
labelLen labelTank = lblShort;

bool isInMenu = FALSE;

keyItems keys;

/* Timing */
DWORD dwSysFrameTotal = 0;
DWORD dwSysFrame = 0;
DWORD dwSysGameTotal = 0;
DWORD dwSysGame = 0;
DWORD dwSysBrainTotal = 0;
DWORD dwSysBrain = 0;

bool drawBusy = FALSE;
static bool paused = FALSE;
bool winboloQuit = FALSE;
bool finishedLoop = FALSE;

DWORD oldTick = 0;
DWORD ttick = 0;
time_t ticks = 0;

/* Frame-based tick accumulator */
static double gameTickAccum = 0.0;
static Uint64 lastFrameTime = 0;

/* True when the last shared tick step run was the game half.  The keys/game
   cadence lives in client_frontend_tick.c and its counter is private, so this
   is how windowRunGameTick knows which half comes next.  Cleared alongside
   clientFrontTickReset so a restarted game starts in phase. */
static bool prevStepWasGameTick = FALSE;

/* -------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------- */
extern void sdl3MessageHandler(const char *message, const char *title);
static void windowRunGameTick(ClientSim *cs);

/* -------------------------------------------------------
 * SDL message handler
 * ------------------------------------------------------- */
void sdl3MessageHandler(const char *message, const char *title) {
    imguiMessageBoxEx(title ? title : "WinBolo",
                      message ? message : "",
                      IMGUI_MSG_INFO, IMGUI_MSG_OK);
}

/* -------------------------------------------------------
 * main — iOS entry point (follows Android main_android.c)
 * ------------------------------------------------------- */
int main(int argc, char *argv[]) {
    DWORD tick;
    (void)argc;
    (void)argv;

    srand((unsigned int)(time(NULL) ^ getpid()));

    /* Initialize Sentry crash reporting if the user hasn't opted out */
#ifdef HAVE_SENTRY
    if (iosCrashReportingGetEnabled()) {
        [SentrySDK startWithConfigureOptions:^(SentryOptions *options) {
#ifdef SENTRY_DSN
            options.dsn = @SENTRY_DSN;
#endif
            options.releaseName = [NSString stringWithFormat:@"winbolo-ios@%s", WINBOLO_VERSION];
        }];
    }
#endif

    /* Configure audio session */
    {
        NSError *err = nil;
        AVAudioSession *session = [AVAudioSession sharedInstance];
        [session setCategory:AVAudioSessionCategoryAmbient error:&err];
        if (err) WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "AVAudioSession setCategory failed: %s", [[err localizedDescription] UTF8String]);
        [session setActive:YES error:&err];
        if (err) WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "AVAudioSession setActive failed: %s", [[err localizedDescription] UTF8String]);
    }

    SDL_Init(0);

    /* Change working directory to the app bundle */
    const char *basePath = SDL_GetBasePath();
    if (basePath) {
        chdir(basePath);
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "Base path: %s", basePath);
    }


    if (clientMutexCreate() == FALSE) {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[iOS] Failed to create client mutex");
        return 1;
    }

    dialogBackendInit();

    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Starting gameFrontStart...");
    if (gameFrontStart("", &keys, FALSE, NULL) == FALSE) {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[iOS] gameFrontStart FAILED");
        clientMutexDestroy();
        SDL_Quit();
        return 1;
    }
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] gameFrontStart OK");

ios_game_start:
    /* Start the shared keys/game cadence from a known state, so the first
       running step is a game step on tick 0 (even = game is the wire contract
       the server routes by).  Placed on the restart label rather than after
       the lobby, so it covers both the first game and every game reached
       through the goto below. */
    clientFrontTickReset();
    prevStepWasGameTick = FALSE;

    /* Set up ImGui and touch input */
    {
        SDL_Window *win = sdl3DrawGetWindow();
        SDL_Renderer *ren = sdl3DrawGetRenderer();
        if (win && ren) {
            sdl3ImguiSetup(win, ren);
        }
        if (win) {
            SDL_ShowWindow(win);

            /* Use logical presentation size if set (sdl3DrawSetup may set
               one to improve tablet zoom), otherwise fall back to render
               output size. */
            int tw = 0, th = 0;
            if (ren) {
                SDL_RendererLogicalPresentation mode;
                SDL_GetRenderLogicalPresentation(ren, &tw, &th, &mode);
            }
            if (tw <= 0 || th <= 0) {
                if (ren) SDL_GetCurrentRenderOutputSize(ren, &tw, &th);
            }
            if (tw <= 0 || th <= 0) {
                SDL_GetWindowSize(win, &tw, &th);
            }

            /* Query safe area for notch/Dynamic Island.
               SDL_GetWindowSafeArea returns values in window-point coordinates,
               so scale them to the renderer coordinate space. */
            int safeLeft = 0, safeTop = 0, safeRight = 0, safeBottom = 0;
            {
                SDL_Rect safeRect;
                int winW = 0, winH = 0;
                SDL_GetWindowSize(win, &winW, &winH);
                if (SDL_GetWindowSafeArea(win, &safeRect)) {
                    safeLeft   = safeRect.x;
                    safeTop    = safeRect.y;
                    safeRight  = winW - (safeRect.x + safeRect.w);
                    safeBottom = winH - (safeRect.y + safeRect.h);
                    if (winW > 0 && winH > 0) {
                        safeLeft   = safeLeft   * tw / winW;
                        safeTop    = safeTop    * th / winH;
                        safeRight  = safeRight  * tw / winW;
                        safeBottom = safeBottom * th / winH;
                    }
                    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Safe area insets: L=%d T=%d R=%d B=%d",
                            safeLeft, safeTop, safeRight, safeBottom);
                }
            }

            touchInputSetup(tw, th, safeLeft, safeTop, safeRight, safeBottom);
            WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Touch coordinate space: %dx%d", tw, th);
        }
    }

    /* GCVirtualController removed — using SDL touch input instead */

    isInMenu = FALSE;
    finishedLoop = FALSE;
    ClientSim *cs = humanSim;

    /* Handle lobby if server uses lobby mode */
    if (cs && clientSimIsInLobby(cs) &&
        (clientSimGetNetStatus(cs) == netLobby || clientSimGetNetStatus(cs) == netLobbyCountdown)) {
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Entering lobby");
        sdl3ImguiCleanup();
        const DialogBackend *db = dialogBackendGet();
        int lobbyResult = db->lobbyShow(cs);
        if (lobbyResult == 0) {
            WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Left lobby, cleaning up");
            gameFrontEnd(&keys, FALSE, TRUE);
            clientMutexDestroy();
            sdl3DrawCleanup();
            soundCleanup();
            SDL_Quit();
            return 0;
        }
        /* lobbyResult == 1: game started. The UDP transport's
         * CTRL_GAME_PHASE LOBBY→RUNNING watcher already installed
         * the map onto the ClientSim, so we fall straight through
         * to the per-frame game-tick loop below. */
        clientSimSetNetStatus(cs, netRunning);
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Lobby complete, game starting");
        {
            SDL_Window *win = sdl3DrawGetWindow();
            SDL_Renderer *ren = sdl3DrawGetRenderer();
            if (win && ren) {
                sdl3ImguiSetup(win, ren);
                }
        }
    }

    windowApplyMenuChecks(cs);

    if (soundEffects == TRUE) {
        soundKeepalive(useSoundKeepalive);
    }

    guiMessageSetHandler(sdl3MessageHandler);

    oldTick = SDL_GetTicks();

    /* Flush any stale render state left over from the dialog phase.
       The dialog loop destroys textures and restores logical presentation
       after its last SDL_RenderPresent, which can leave the Metal command
       queue in an inconsistent state.  A clean clear+present here ensures
       the renderer starts fresh before the game loop's first
       SDL_SetRenderTarget call. */
    {
        SDL_Renderer *ren = sdl3DrawGetRenderer();
        if (ren) {
            SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
            SDL_RenderClear(ren);
            SDL_RenderPresent(ren);
        }
    }

    lastFrameTime = SDL_GetTicks();

    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Starting main loop");

    /* Main game loop */
#define IOS_FRAME_CAP_MS 16
    while (!winboloQuit && !finishedLoop) {
        Uint64 frameCapStart = SDL_GetTicks();

        /* Process SDL events */
        {
            SDL_Event ev;
            if (paused) {
                if (SDL_WaitEvent(&ev)) {
                    if (uiModeIsTablet()) {
                        if (ev.type == SDL_EVENT_FINGER_DOWN || ev.type == SDL_EVENT_FINGER_UP || ev.type == SDL_EVENT_FINGER_MOTION) {
                            int tw = 0, th = 0;
                            SDL_Renderer *ren = sdl3DrawGetRenderer();
                            if (ren) { SDL_RendererLogicalPresentation m; SDL_GetRenderLogicalPresentation(ren, &tw, &th, &m); }
                            if (tw <= 0 || th <= 0) SDL_GetWindowSize(sdl3DrawGetWindow(), &tw, &th);
                            inputTouchProcessEvent(&ev, tw, th);
                        }
                    } else {
                        touchInputProcessEvent(&ev);
                    }
                    if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                        winboloQuit = TRUE;
                    } else if (ev.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
                        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Resuming from background");
                        paused = FALSE;
                        lastFrameTime = SDL_GetTicks();
                        gameTickAccum = 0.0;
                        soundSetMuted(FALSE);
                    }
                }
                continue;
            }
            while (SDL_PollEvent(&ev)) {
                SDL_Event rawEv = ev;
                /* Convert coordinates to logical presentation space for ImGui */
                SDL_Renderer *ren = sdl3DrawGetRenderer();
                if (ren) {
                    SDL_ConvertEventToRenderCoordinates(ren, &ev);
                }
                /* Forward to ImGui so dialogs receive mouse/touch input */
                sdl3ImguiForwardEvent(&ev);

                /* Always route finger events to the touch input system in
                   tablet mode — ImGui buttons (build bar, etc.) use
                   inputTouchConsumeTapInRect for hit-testing, so the
                   touch system must see every event regardless of
                   ImGui's WantCaptureMouse state.  Non-tablet mode
                   still gates on WantCaptureMouse to avoid conflicts
                   with dialog input. */
                if (uiModeIsTablet()) {
                    if (rawEv.type == SDL_EVENT_FINGER_DOWN || rawEv.type == SDL_EVENT_FINGER_UP || rawEv.type == SDL_EVENT_FINGER_MOTION) {
                        int tw = 0, th = 0;
                        if (ren) { SDL_RendererLogicalPresentation m; SDL_GetRenderLogicalPresentation(ren, &tw, &th, &m); }
                        if (tw <= 0 || th <= 0) SDL_GetWindowSize(sdl3DrawGetWindow(), &tw, &th);
                        inputTouchProcessEvent(&rawEv, tw, th);
                    }
                } else if (!sdl3ImguiWantCaptureMouse()) {
                    touchInputProcessEvent(&ev);
                }
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                    winboloQuit = TRUE;
                } else if (ev.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
                    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Entering background, pausing");
                    paused = TRUE;
                    soundSetMuted(TRUE);
                } else if (ev.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
                    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Resuming from background");
                    paused = FALSE;
                    lastFrameTime = SDL_GetTicks();
                    gameTickAccum = 0.0;
                    soundSetMuted(FALSE);
                }
            }
        }

        /* Game tick accumulation */
        if (clientSimHasTransport(cs)) {
            Uint64 now = SDL_GetTicks();
            double elapsed = (double)(now - lastFrameTime);
            lastFrameTime = now;
            gameTickAccum += elapsed;

            while (gameTickAccum >= GAME_TICK_LENGTH) {
                gameTickAccum -= GAME_TICK_LENGTH;
                windowRunGameTick(cs);
            }
        }

        /* Render */
        tick = SDL_GetTicks();
        clientMutexWaitFor();
        if (finishedLoop == FALSE) {
            clientSimRenderPrepare(cs, tick);
            clientRenderFrame(cs, redraw);
        }
        clientMutexRelease();
        /* The in-window map overview draws from the snapshot the frame above
           filled, now that the lock is off. Nothing on iOS turns that mode on
           today — every path that does is compiled out here — but the call is
           a no-op when no frame is waiting, and leaving it out is how the map
           would silently stop drawing if one ever did. */
        sdl3DrawFlushOverviewInWindow();
        dwSysFrame += (SDL_GetTicks() - tick);

        /* Touch overlay (skip in tablet mode — ImGui overlay handles it) */
        if (!uiModeIsTablet()) {
            SDL_Renderer *ren = sdl3DrawGetRenderer();
            if (ren) {
                touchInputRender(ren);
            }
        }
        sdl3ImguiPumpAndRender(cs);
        {
            SDL_Renderer *ren = sdl3DrawGetRenderer();
            if (ren) SDL_RenderPresent(ren);
        }

        /* Cap to 60 FPS */
        {
            Uint64 frameElapsed = SDL_GetTicks() - frameCapStart;
            if (frameElapsed < IOS_FRAME_CAP_MS) {
                SDL_Delay((Uint32)(IOS_FRAME_CAP_MS - frameElapsed));
            }
        }
    }

    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Main loop ended, cleaning up");

    sdl3ImguiCleanup();
    gameFrontEnd(&keys, TRUE, winboloQuit);

    if (!winboloQuit) {
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Returning to menu (windowNewGame)");
        /* Restart the pre-game dialogs and re-enter the game loop */
        if (gameFrontStart("", &keys, TRUE, NULL)) {
            goto ios_game_start;
        }
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[iOS] gameFrontStart failed after leave game");
    }

    clientMutexDestroy();
    sdl3DrawCleanup();
    soundCleanup();
    SDL_Quit();
    return 0;
}

/* -------------------------------------------------------
 * windowRunGameTick — one half-step of the shared client tick, plus the
 * driver-side work around it (disconnect check, bot brains, AI, stats)
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
    static bool inBrain = FALSE;
    static BYTE t2 = 0;
    bool used = FALSE;
    bool brainRunning;

    brainRunning = brainHandlerIsBrainRunning();

    /* Check if the UDP server has disconnected or timed out.
     * Only check for UDP transports (serverSim == NULL means not local). */
    if (clientSimHasTransport(cs) && gameFrontGetServerSim() == NULL &&
        clientSimGetConnectState(cs) == CLIENT_CONNECT_SERVER_SHUTDOWN) {
        clientSimConnectionLost(cs);
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          langGetText(NETERR_LOSTCONNECTION_RETURN_MENU),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        finishedLoop = TRUE;
        winboloQuit = FALSE;
        return;
    }

    /* Tick bot brains for a local game.  This binary links no server_static,
     * so nothing else drives them and the client tick owns them here.
     * serverSimBotTick queues a keys packet and a game packet per bot, so it
     * belongs once per full frame, on the half-step that is about to be the
     * game half: a step that returned false was the keys half (or a lobby
     * step, which does not advance the cadence), so the next one is the game
     * half.  Skipped in lobby and countdown for the same reason the shared
     * step skips them — the server runs no half-steps there, so queued bot
     * input would only pile up. */
    if (prevStepWasGameTick == FALSE) {
        netStatus ns = clientSimGetNetStatus(cs);
        if (ns != netLobby && ns != netLobbyCountdown) {
            ServerSim *serverSim = gameFrontGetServerSim();
            if (serverSim != NULL && serverSimGetNumBots(serverSim) > 0) {
                serverSimBotTick(serverSim, clientSimGetAiType(cs));
            }
        }
    }

    /* One keys/game/lobby half-step lives in the shared client-frontend tick
     * core (desktop and web call the same one); the brain run and the
     * per-second stat rollover below stay here in the driver.  The main loop
     * owns the catch-up, calling this once per owed half-step. */
    ttick = SDL_GetTicks();
    if (clientFrontRunTickStep(cs)) {
        t2++;
        ticks++;
        used = TRUE;
        prevStepWasGameTick = TRUE;
    } else {
        prevStepWasGameTick = FALSE;
    }
    dwSysGame += (SDL_GetTicks() - ttick);

    /* AI */
    if (used == TRUE && inBrain == FALSE && brainRunning == TRUE &&
        clientSimGetNetStatus(cs) != netFailed) {
        clientMutexWaitFor();
        inBrain = TRUE;
        clientMutexRelease();
        ttick = SDL_GetTicks();
        brainHandlerRun();
        dwSysBrain += SDL_GetTicks() - ttick;
        clientMutexWaitFor();
        inBrain = FALSE;
        clientMutexRelease();
    }

    if (t2 >= 19) {
        netSecond();
        dwSysFrameTotal = dwSysFrame;
        dwSysGameTotal = dwSysGame;
        dwSysBrainTotal = dwSysBrain;
        dwSysBrain = 0;
        dwSysFrame = 0;
        dwSysGame = 0;
        t2 = 0;
    }
}

/* -------------------------------------------------------
 * Window management functions (matching winbolo.h)
 * ------------------------------------------------------- */

void *windowWnd(void) { return NULL; }
void windowReCreate(void) {}
void windowSetQuitting(void) { winboloQuit = TRUE; finishedLoop = TRUE; }

void windowApplyMenuChecks(ClientSim *cs) {
    clientSimSetGunsight(cs, showGunsight);
    clientSimSetAutoScroll(cs, autoScrollingEnabled);
    clientSimSetLabelOwnTank(cs, labelSelf);
    clientSimSetLabelMessage(cs, labelMsg);
    clientSimSetLabelTankLabel(cs, labelTank);
    clientSimShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
    clientSimShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
    clientSimShowMessages(cs, MSG_AI, showAIMessages);
    clientSimShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
    clientSimShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
    clientSimSetAllowNewPlayers(cs, allowNewPlayers);
}

int windowGetDrawTime(void) { return (int)dwSysFrameTotal; }
int windowGetNetTime(void) { return netGetNetTime(); }
int windowGetAiTime(void) { return (int)dwSysBrainTotal; }
int windowGetSimTime(void) { return (int)dwSysGameTotal; }

void windowGetKeys(keyItems *value) { *value = keys; }
void windowSetKeys(keyItems *value) { keys = *value; }

void windowSetZoomFactor(BYTE amount) { zoomFactor = amount; }
BYTE windowGetZoomFactor(void) { return zoomFactor; }
void windowZoomChange(BYTE amount, bool fromDragResize) {
    (void)fromDragResize;  /* iOS doesn't use resize detection */
    if (amount == zoomFactor) return;
    drawBusy = TRUE;
    clientMutexWaitFor();
    sdl3ImguiCleanup();
    sdl3DrawCleanup();
    sdl3DrawSetup(amount);
    SDL_Window *w = sdl3DrawGetWindow();
    SDL_Renderer *r = sdl3DrawGetRenderer();
    if (w && r) {
        sdl3ImguiSetup(w, r);
    }
    clientMutexRelease();
    drawBusy = FALSE;
    windowSetZoomFactor(amount);
}

void windowSetFrameRate(int newFrameRate, bool setTimer) {
    (void)setTimer;
    frameRate = newFrameRate;
}

void windowShowGunsight_toggle(void) {
    showGunsight = !showGunsight;
    if (humanSim) clientSimSetGunsight(humanSim, showGunsight);
}

void windowSoundEffects_toggle(void) { soundEffects = !soundEffects; }
void windowBackgroundSoundChange_toggle(void) { backgroundSound = !backgroundSound; }
void windowSoundKeepalive(void) { useSoundKeepalive = !useSoundKeepalive; }
void windowSetSoundVolume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    soundVolume = pct;
    soundSetVolume(pct);
}
void windowAutomaticScrolling_toggle(ClientSim *cs) {
    autoScrollingEnabled = !autoScrollingEnabled;
    if (cs) clientSimSetAutoScroll(cs, autoScrollingEnabled);
}

void windowShowPillLabels_toggle(void) { showPillLabels = !showPillLabels; }
void windowShowBaseLabels_toggle(void) { showBaseLabels = !showBaseLabels; }
void windowLabelOwnTank_toggle(ClientSim *cs) {
    labelSelf = !labelSelf;
    if (cs) clientSimSetLabelOwnTank(cs, labelSelf);
}

void windowSetMessageLabelLen(ClientSim *cs, labelLen newLen) {
    labelMsg = newLen;
    if (cs) clientSimSetLabelMessage(cs, labelMsg);
}

void windowSetTankLabelLen(ClientSim *cs, labelLen newLen) {
    labelTank = newLen;
    if (cs) clientSimSetLabelTankLabel(cs, labelTank);
}

void windowMenuAllowNewPlayers_toggle(void) { allowNewPlayers = !allowNewPlayers; }
void windowMenuNewswire_toggle(ClientSim *cs) { showNewswireMessages = !showNewswireMessages; (void)cs; }
void windowMenuAssistant_toggle(ClientSim *cs) { showAssistantMessages = !showAssistantMessages; (void)cs; }
void windowMenuAI_toggle(ClientSim *cs) { showAIMessages = !showAIMessages; (void)cs; }
void windowMenuNetwork_toggle(ClientSim *cs) { showNetworkStatusMessages = !showNetworkStatusMessages; (void)cs; }
void windowMenuNetworkDebug_toggle(ClientSim *cs) { showNetworkDebugMessages = !showNetworkDebugMessages; (void)cs; }

void windowNewGame(void) {
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] windowNewGame: leaving game");
  winboloQuit = FALSE;
  finishedLoop = TRUE;
}
void windowQuit(void) {}

void windowShowGameInfo(windowShowRequest req) { (void)req; }
void windowShowSysInfo(windowShowRequest req) { (void)req; }
void windowShowNetInfo(windowShowRequest req) { (void)req; }
void windowShowSetPlayerName(windowShowRequest req) { (void)req; }
void windowShowSendMessages(windowShowRequest req) { (void)req; }
void windowShowSetKeys(windowShowRequest req) { (void)req; }
void windowShowAboutBox(void) {}

bool windowShowAllianceRequest(void) { return FALSE; }
void windowDisableSound(void) { soundEffects = FALSE; useSoundKeepalive = FALSE; }
bool windowGetBackgroundSound(void) { return backgroundSound; }

void windowRedrawAll(ClientSim *cs) {
    clientMutexWaitFor();
    sdl3DrawRedrawAll(cs, clientSimGetCurrentBuildSelect(cs), NULL, showPillLabels, showBaseLabels);
    clientMutexRelease();
}

void windowSaveMap(ClientSim *cs) { (void)cs; }

void windowKeyPressed(ClientSim *cs, int keyCode) {
    if (keyCode == keys.kiTankView) clientSimTankView(cs);
    else if (keyCode == keys.kiPillView) clientSimPillView(cs, 0, 0);
}

void windowButtonAdd(int keyCode) { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
    (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) {}
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* Pause hooks driven from sdl3imgui.cpp. All three exist for desktop
   situations iOS does not have: a Steam Deck pause menu, a lost controller,
   and the tutorial overlay's solo freeze. The app already stops ticking when
   it is backgrounded, which is the only pause a phone has. */
void windowControllerLostPause(ClientSim *cs, bool active) { (void)cs; (void)active; }
void windowDeckPause(ClientSim *cs, bool active) { (void)cs; (void)active; }
void windowTutorialPause(ClientSim *cs, bool active) { (void)cs; (void)active; }

/* -------------------------------------------------------
 * Frontend callbacks (matching Android android_frontend.c)
 * ------------------------------------------------------- */

/* The ClientSim that owns the visible player-list UI. Bot, background-game
   and spectator sims also fire frontEnd* callbacks; registering the human's
   sim here lets the ones that care tell them apart. */
static ClientSim *s_activeUiCs = NULL;

void frontEndSetActiveClientSim(ClientSim *cs) {
    if (cs != s_activeUiCs) {
        /* Drop the previous game's latched build target: it is the square a
           tap builds at, and it outlives the ClientSim that set it, so
           without this the first tap of the next game is dispatched to a
           tile chosen in the last one. */
        buildCursorReset();
    }
    s_activeUiCs = cs;
}

void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
    if (drawBusy == FALSE) {
        BYTE cursorX, cursorY;
        bool showCursor = clientSimGetCursorPos(cs, &cursorX, &cursorY);
        /* Resolve the reticle through the shared build cursor, as every other
           frontend does — the tap-to-build handler latches the tapped square
           there, so the reticle has to read it back from the same place. */
        bool cursorFaint = false;
        showCursor = buildCursorResolveReticle(cs, showCursor, cursorX, cursorY,
                                               &cursorX, &cursorY, &cursorFaint);
        sdl3DrawSetCursorFaint(cursorFaint);
        sdl3DrawSetNetFailed(clientSimGetNetStatus(cs) == netFailed);
        sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                           NULL, showPillLabels, showBaseLabels,
                           srtDelay, isPillView, edgeX, edgeY,
                           showCursor, cursorX, cursorY);
    }
}

void frontEndUpdateTankStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour, BYTE trees) {
    (void)cs;
    if (armour > TANK_FULL_ARMOUR) {
        armour = 0;
    }
    sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
    (void)cs;
    sdl3DrawStatusBaseBars(0, 0, shells, mines, armour, FALSE);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
    (void)cs;
    if (soundEffects) soundPlayEffect(value);
}

void windowPlaySound(sndEffects value) {
    if (soundEffects) soundPlayEffect(value);
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
    (void)cs;
    sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
    sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
    (void)cs;
    sdl3DrawStatusTank(tankNum, ts);
    sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
    (void)cs;
    sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
    sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
    (void)cs;
    if (drawBusy == FALSE) sdl3DrawMessages(0, 0, top, bottom);
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
    (void)cs;
    if (drawBusy == FALSE) sdl3DrawKillsDeaths(0, 0, kills, deaths);
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
    (void)cs;
    clientMutexWaitFor();
    sdl3DrawSetManStatus(0, 0, isDead, angle);
    clientMutexRelease();
}

void frontEndManClear(ClientSim *cs) {
    (void)cs;
    clientMutexWaitFor();
    sdl3DrawSetManClear();
    sdl3DrawCopyManStatus(0, 0);
    clientMutexRelease();
}

void frontEndDrawDownload(ClientSim *cs, bool justBlack) {
    if (drawBusy == FALSE) {
        sdl3DrawDownloadScreen(cs, NULL, justBlack);
    }
}

void frontEndDrawReturningToLobby(ClientSim *cs) {
    if (drawBusy == FALSE) {
        sdl3DrawReturningToLobby(cs);
    }
}

void frontEndAudioReturningToLobby(bool active) {
    soundSetReturningToLobby(active);
}

void frontEndGameOver(ClientSim *cs) {
    (void)cs;
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Game over (time limit expired)");
    finishedLoop = TRUE;
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
    (void)cs;
    sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
    char cc[3];
    if (!clientSimIsRunning(cs)) {
        cc[0] = 'X'; cc[1] = 'X'; cc[2] = '\0';
        sdl3ImguiSetPlayer((unsigned char)value, str, cc);
        return;
    }
    cc[0] = countryCode[0];
    cc[1] = countryCode[1];
    cc[2] = '\0';
    sdl3ImguiSetPlayer((unsigned char)value, str, cc);
    sdl3ImguiUpdatePlayerMeta((unsigned char)value, ping, clientType, clientFlags);
}

void frontEndUpdatePlayerPing(ClientSim *cs, playerNumbers value, uint16_t ping) {
    if (!clientSimIsRunning(cs)) return;
    sdl3ImguiUpdatePlayerPing((unsigned char)value, ping);
}

/* Runs off the snapshot path, so it stays cheap. The badge cache is seeded by
   frontEndSetPlayer above; refreshing it mid-game is desktop-only. */
void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value,
                               uint8_t clientType, uint8_t clientFlags) {
    (void)cs; (void)value; (void)clientType; (void)clientFlags;
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
    (void)cs;
    sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

void frontEndApplyLocalTankPrefs(struct ClientSim *cs) { (void)cs; }

void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled) { (void)enabled; }

void frontEndRedrawAll(ClientSim *cs) { windowRedrawAll(cs); }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
    showGunsight = !isShown;
    clientSimSetGunsight(cs, showGunsight);
}

void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
}

bool frontEndTutorial(BYTE pos) {
    (void)pos;
    return FALSE;
}

void frontEndTutorialReset(void) { }

/* -------------------------------------------------------
 * Misc required symbols
 * ------------------------------------------------------- */
time_t serverMainGetTicks(void) { return ticks; }
time_t windowsGetTicks(void) { return ticks; }
int winboloCC(void) { return 0; }

void *dialogAllianceCreate(void) { return NULL; }
void dialogAllianceDestroy(void *dlg) { (void)dlg; }
void dialogAllianceSetName(char *playerName, BYTE playerNum) {
    (void)playerName; (void)playerNum;
}

bool winUtilWBSubDirExist(char *subDirName) {
    struct stat st;
    if (stat(subDirName, &st) == 0 && S_ISDIR(st.st_mode)) return TRUE;
    return FALSE;
}
