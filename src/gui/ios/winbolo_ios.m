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

#include "../../bolo/frontend.h"
#include "../../bolo/players.h"
#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"
#include "../../bolo/input_packet.h"
#include "../../bolo/transport.h"
#include "../../bolo/transport_udp.h"
#include "../../bolo/bot_manager.h"
#include "../../bolo/gui_message.h"
#include "../../server/server_sim.h"
#include "../../server/threads.h"
#include "../brainsHandler.h"
#include "../clientmutex.h"
#include "../gamefront.h"
#include "../lang.h"
#include "../winbolo.h"
#include "../sound.h"
#include "sdl3draw.h"
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
bool allowNewPlayers = TRUE;

bool showNewswireMessages = TRUE;
bool showAssistantMessages = TRUE;
bool showAIMessages = FALSE;
bool showNetworkStatusMessages = TRUE;
bool showNetworkDebugMessages = FALSE;

bool autoScrollingEnabled = FALSE;
bool smoothScrollingEnabled = FALSE;  /* Touch platform: arrow-key smooth scroll inactive */
BYTE zoomFactor = ZOOM_FACTOR_NORMAL;

bool showPillLabels = FALSE;
bool showBaseLabels = FALSE;
bool labelSelf = TRUE;
labelLen labelMsg = lblShort;
labelLen labelTank = lblShort;

bool isInMenu = FALSE;
bool hideMainView = FALSE;

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

/* -------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------- */
extern void sdl3MessageHandler(const char *message, const char *title);
static void windowRunGameTick(ClientSim *cs);

/* -------------------------------------------------------
 * Helper: sync snapshot from transport
 * ------------------------------------------------------- */
static void iosSyncSnapshot(ClientSim *cs, Transport *transport, BYTE myPlayerNum) {
    SnapshotHeader snapHdr;
    TankSnapshot snapTanks[MAX_TANKS];
    ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
    PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
    GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];
    if (transport->getSnapshot(transport->ctx, myPlayerNum,
                               &snapHdr, snapTanks, MAX_TANKS,
                               snapShells, MAX_SNAPSHOT_SHELLS,
                               snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                               snapBases, MAX_SNAPSHOT_BASES,
                               snapPills, MAX_SNAPSHOT_PILLS,
                               snapEvents, MAX_SNAPSHOT_EVENTS)) {
        clientSimSyncFromSnapshot(cs, &snapHdr, snapTanks, snapHdr.tankCount,
                                snapShells, snapHdr.shellCount,
                                snapTkExplosions, snapHdr.tkExplosionCount,
                                snapBases, snapHdr.baseCount,
                                snapPills, snapHdr.pillCount,
                                snapEvents, snapHdr.reliableEventCount,
                                myPlayerNum);
    }
}

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
        if (err) SDL_Log("AVAudioSession setCategory failed: %s", [[err localizedDescription] UTF8String]);
        [session setActive:YES error:&err];
        if (err) SDL_Log("AVAudioSession setActive failed: %s", [[err localizedDescription] UTF8String]);
    }

    SDL_Init(0);

    /* Change working directory to the app bundle */
    const char *basePath = SDL_GetBasePath();
    if (basePath) {
        chdir(basePath);
        SDL_Log("Base path: %s", basePath);
    }

    initWinboloTimer();

    if (clientMutexCreate() == FALSE) {
        SDL_Log("[iOS] Failed to create client mutex");
        return 1;
    }

    dialogBackendInit();

    SDL_Log("[iOS] Starting gameFrontStart...");
    if (gameFrontStart("", &keys, FALSE, NULL) == FALSE) {
        SDL_Log("[iOS] gameFrontStart FAILED");
        endWinboloTimer();
        clientMutexDestroy();
        SDL_Quit();
        return 1;
    }
    SDL_Log("[iOS] gameFrontStart OK");

ios_game_start:
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
                    SDL_Log("[iOS] Safe area insets: L=%d T=%d R=%d B=%d",
                            safeLeft, safeTop, safeRight, safeBottom);
                }
            }

            touchInputSetup(tw, th, safeLeft, safeTop, safeRight, safeBottom);
            SDL_Log("[iOS] Touch coordinate space: %dx%d", tw, th);
        }
    }

    /* GCVirtualController removed — using SDL touch input instead */

    isInMenu = FALSE;
    finishedLoop = FALSE;
    ClientSim *cs = humanSim;

    /* Handle lobby if server uses lobby mode */
    if (cs && clientSimIsInLobby(cs) &&
        (clientSimGetNetStatus(cs) == netLobby || clientSimGetNetStatus(cs) == netLobbyCountdown)) {
        SDL_Log("[iOS] Entering lobby");
        sdl3ImguiCleanup();
        const DialogBackend *db = dialogBackendGet();
        int lobbyResult = db->lobbyShow(cs);
        if (lobbyResult == 0) {
            SDL_Log("[iOS] Left lobby, cleaning up");
            gameFrontEnd(&keys, FALSE, TRUE);
            endWinboloTimer();
            clientMutexDestroy();
            sdl3DrawCleanup();
            soundCleanup();
            SDL_Quit();
            return 0;
        }
        if (!gameFrontLoadDeferredMap(cs)) {
            SDL_Log("[iOS] Failed to load deferred map");
            gameFrontEnd(&keys, FALSE, TRUE);
            endWinboloTimer();
            clientMutexDestroy();
            sdl3DrawCleanup();
            soundCleanup();
            SDL_Quit();
            return 0;
        }
        clientSimSetNetStatus(cs, netRunning);
        SDL_Log("[iOS] Lobby complete, game starting");
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

    oldTick = winboloTimer();

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

    SDL_Log("[iOS] Starting main loop");

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
                        SDL_Log("[iOS] Resuming from background");
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
                    SDL_Log("[iOS] Entering background, pausing");
                    paused = TRUE;
                    soundSetMuted(TRUE);
                } else if (ev.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
                    SDL_Log("[iOS] Resuming from background");
                    paused = FALSE;
                    lastFrameTime = SDL_GetTicks();
                    gameTickAccum = 0.0;
                    soundSetMuted(FALSE);
                }
            }
        }

        /* Game tick accumulation */
        if (gameFrontGetTransport() != NULL) {
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
        tick = winboloTimer();
        clientMutexWaitFor();
        if (finishedLoop == FALSE) {
            screenUpdateCS(cs, redraw);
        }
        clientMutexRelease();
        dwSysFrame += (winboloTimer() - tick);

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

    SDL_Log("[iOS] Main loop ended, cleaning up");

    screenLeaveGame();
    sdl3ImguiCleanup();
    gameFrontEnd(&keys, TRUE, winboloQuit);

    if (!winboloQuit) {
        SDL_Log("[iOS] Returning to menu (windowNewGame)");
        /* Restart the pre-game dialogs and re-enter the game loop */
        if (gameFrontStart("", &keys, TRUE, NULL)) {
            goto ios_game_start;
        }
        SDL_Log("[iOS] gameFrontStart failed after leave game");
    }

    endWinboloTimer();
    clientMutexDestroy();
    sdl3DrawCleanup();
    soundCleanup();
    SDL_Quit();
    return 0;
}

/* -------------------------------------------------------
 * windowRunGameTick — game logic (matches Android)
 * ------------------------------------------------------- */
static void windowRunGameTick(ClientSim *cs) {
    static bool inBrain = FALSE;
    static bool justKeys = FALSE;
    static BYTE t2 = 0;
    static uint32_t simTickCounter = 0;
    tankButton tb;
    bool isShoot;
    bool isMine = FALSE;
    bool used = FALSE;
    bool brainRunning;
    Transport *transport;

    brainRunning = brainHandlerIsBrainRunning();
    isShoot = FALSE;
    tb = 0;

    transport = gameFrontGetTransport();
    if (transport == NULL) return;

    /* Check if the UDP server has disconnected or timed out.
     * Only check for UDP transports (serverSim == NULL means not local). */
    if (gameFrontGetServerSim() == NULL &&
        transportUdpClientGetJoinState(transport) == UDP_CLIENT_SERVER_SHUTDOWN) {
        screenConnectionLostCS(cs);
        imguiMessageBoxEx(DIALOG_BOX_TITLE,
                          langGetText(NETERR_LOSTCONNECTION_RETURN_MENU),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        finishedLoop = TRUE;
        winboloQuit = FALSE;
        return;
    }

    {
        BYTE myPlayerNum = gameFrontGetPlayerNum();
        if (justKeys == TRUE) {
            if (brainRunning == FALSE) {
                if (uiModeIsTablet()) {
                    inputTouchSetTankAngle(screenGetTank256DirCS(cs));
                    tb = inputTouchGetMovement();
                } else {
                    tb = touchInputGetKeys();
                }
            }
            InputPacket pkt;
            screenBuildInputPacketCS(cs, &pkt, tb, FALSE, FALSE, brainRunning, FALSE, myPlayerNum, simTickCounter);
            clientMutexWaitFor();
            clientSimKeysTick(cs, &pkt);
            clientMutexRelease();
            transport->recordInput(transport->ctx, &pkt);
            transport->tick(transport->ctx);
            clientMutexWaitFor();
            iosSyncSnapshot(cs, transport, myPlayerNum);
            clientMutexRelease();
            simTickCounter++;
            justKeys = FALSE;
        } else {
            t2++;
            if (brainRunning == FALSE) {
                if (uiModeIsTablet()) {
                    inputTouchSetTankAngle(screenGetTank256DirCS(cs));
                    tb = inputTouchGetMovement();
                    isShoot = inputTouchIsFirePressed();
                    isMine = inputTouchIsMinePressed();
                } else {
                    tb = touchInputGetKeys();
                    isShoot = touchInputIsFireKeyPressed();
                    isMine = touchInputShouldLayMine();
                }
            }
            InputPacket pkt;
            screenBuildInputPacketCS(cs, &pkt, tb, isShoot, isMine, brainRunning, TRUE, myPlayerNum, simTickCounter);
            if (brainRunning == FALSE) {
                if (uiModeIsTablet()) {
                    int gsChange = inputTouchGetGunsightChange();
                    if (gsChange > 0) pkt.flags |= (1 << INPUT_FLAG_GUNSIGHT_SHIFT);
                    else if (gsChange < 0) pkt.flags |= (2 << INPUT_FLAG_GUNSIGHT_SHIFT);
                } else {
                    int gsChange = touchInputGetGunsightChange();
                    if (gsChange > 0) pkt.flags |= (1 << INPUT_FLAG_GUNSIGHT_SHIFT);
                    else if (gsChange < 0) pkt.flags |= (2 << INPUT_FLAG_GUNSIGHT_SHIFT);
                }
            }
            clientMutexWaitFor();
            clientSimGameTick(cs, &pkt, brainRunning);
            clientMutexRelease();
            transport->sendInput(transport->ctx, &pkt);
            /* Tick bot brains before the sim tick (local game only) */
            {
                ServerSim *serverSim = gameFrontGetServerSim();
                if (serverSim != NULL && botManagerGetNumBots() > 0) {
                    botManagerTick(serverSim, screenGetAiTypeCS(cs));
                }
            }
            transport->tick(transport->ctx);
            clientMutexWaitFor();
            iosSyncSnapshot(cs, transport, myPlayerNum);
            clientSimDisplayTick(cs, brainRunning);
            clientMutexRelease();
            simTickCounter++;
            ticks++;
            justKeys = TRUE;
            used = TRUE;
        }
    }

    /* AI */
    if (used == TRUE && inBrain == FALSE && brainRunning == TRUE) {
        clientMutexWaitFor();
        inBrain = TRUE;
        clientMutexRelease();
        ttick = winboloTimer();
        brainHandlerRun();
        dwSysBrain += winboloTimer() - ttick;
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
    screenSetGunsightCS(cs, showGunsight);
    screenSetAutoScroll(cs, autoScrollingEnabled);
    screenSetLabelOwnTank(cs, labelSelf);
    screenSetMesageLabelLen(cs, labelMsg);
    screenSetTankLabelLen(cs, labelTank);
    screenShowMessages(cs, MSG_NEWSWIRE, showNewswireMessages);
    screenShowMessages(cs, MSG_ASSISTANT, showAssistantMessages);
    screenShowMessages(cs, MSG_AI, showAIMessages);
    screenShowMessages(cs, MSG_NETSTATUS, showNetworkStatusMessages);
    screenShowMessages(cs, MSG_NETWORK, showNetworkDebugMessages);
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
    if (humanSim) screenSetGunsightCS(humanSim, showGunsight);
}

void windowSoundEffects_toggle(void) { soundEffects = !soundEffects; }
void windowBackgroundSoundChange_toggle(void) { backgroundSound = !backgroundSound; }
void windowSoundKeepalive(void) { useSoundKeepalive = !useSoundKeepalive; }
void windowAutomaticScrolling_toggle(ClientSim *cs) {
    autoScrollingEnabled = !autoScrollingEnabled;
    if (cs) screenSetAutoScroll(cs, autoScrollingEnabled);
}

void windowShowPillLabels_toggle(void) { showPillLabels = !showPillLabels; }
void windowShowBaseLabels_toggle(void) { showBaseLabels = !showBaseLabels; }
void windowHideMainView_toggle(void) { hideMainView = !hideMainView; }
void windowLabelOwnTank_toggle(ClientSim *cs) {
    labelSelf = !labelSelf;
    if (cs) screenSetLabelOwnTank(cs, labelSelf);
}

void windowSetMessageLabelLen(ClientSim *cs, labelLen newLen) {
    labelMsg = newLen;
    if (cs) screenSetMesageLabelLen(cs, labelMsg);
}

void windowSetTankLabelLen(ClientSim *cs, labelLen newLen) {
    labelTank = newLen;
    if (cs) screenSetTankLabelLen(cs, labelTank);
}

void windowMenuAllowNewPlayers_toggle(void) { allowNewPlayers = !allowNewPlayers; }
void windowMenuNewswire_toggle(ClientSim *cs) { showNewswireMessages = !showNewswireMessages; (void)cs; }
void windowMenuAssistant_toggle(ClientSim *cs) { showAssistantMessages = !showAssistantMessages; (void)cs; }
void windowMenuAI_toggle(ClientSim *cs) { showAIMessages = !showAIMessages; (void)cs; }
void windowMenuNetwork_toggle(ClientSim *cs) { showNetworkStatusMessages = !showNetworkStatusMessages; (void)cs; }
void windowMenuNetworkDebug_toggle(ClientSim *cs) { showNetworkDebugMessages = !showNetworkDebugMessages; (void)cs; }

void windowNewGame(void) {
  SDL_Log("[iOS] windowNewGame: leaving game");
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
    sdl3DrawRedrawAll(cs, getBuildCurrentSelectCS(cs), NULL, showPillLabels, showBaseLabels);
    clientMutexRelease();
}

void windowSaveMap(ClientSim *cs) { (void)cs; }

void windowKeyPressed(ClientSim *cs, int keyCode) {
    if (keyCode == keys.kiTankView) screenTankViewCS(cs);
    else if (keyCode == keys.kiPillView) screenPillViewCS(cs, 0, 0);
}

void windowButtonAdd(int keyCode) { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
    (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) {}
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* -------------------------------------------------------
 * Frontend callbacks (matching Android android_frontend.c)
 * ------------------------------------------------------- */

void frontEndDrawMainScreen(ClientSim *cs, screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, tank *tank, int edgeX, int edgeY) {
    if (hideMainView == FALSE && drawBusy == FALSE) {
        BYTE cursorX, cursorY;
        bool showCursor = screenGetCursorPosCS(cs, &cursorX, &cursorY);
        sdl3DrawSetNetFailed(clientSimGetNetStatus(cs) == netFailed);
        sdl3DrawMainScreen(cs, value, mineView, tks, gs, sBullet, lgms,
                           NULL, showPillLabels, showBaseLabels,
                           srtDelay, isPillView, edgeX, edgeY,
                           showCursor, cursorX, cursorY, tank);
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
    if (hideMainView == FALSE && drawBusy == FALSE) {
        sdl3DrawDownloadScreen(cs, NULL, justBlack);
    }
}

void frontEndGameOver(ClientSim *cs) {
    (void)cs;
    SDL_Log("[iOS] Game over (time limit expired)");
    finishedLoop = TRUE;
}

void frontEndClearPlayer(playerNumbers value) {
    sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
    char cc[3];
    if (!screenGetGameRunningCS(cs)) {
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

void frontEndSetPlayerCheckState(playerNumbers value, bool isChecked) {
    sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled) { (void)enabled; }

void frontEndRedrawAll(ClientSim *cs) { windowRedrawAll(cs); }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
    showGunsight = !isShown;
    screenSetGunsightCS(cs, showGunsight);
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
