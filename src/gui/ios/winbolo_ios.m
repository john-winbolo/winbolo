/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include "client_frontend_render.h"
#include "client_frontend_tutorial.h"
#include "client_frontend_common.h"
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
#include "../voice.h"
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
int  soundVolume = 50;         /* MENU / Sound Volume — the game sounds */
int  windowMasterVolume = 100; /* MENU / Master Volume — sounds and voice */
/* gamefront.c reads and writes this through the prefs on every platform;
   the desktop defines it in winbolo.c. */
bool positionalSound = TRUE;
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

/* Set while a tutorial message is up in a solo game, as on the desktop: the
   game ticks stop, the hosted server is paused and the sound is muted, and
   the mute state found on the way in is put back on the way out. */
static bool s_tutorialPaused = FALSE;
static bool s_tutorialPrevMuted = FALSE;

/* A joined game over UDP survives a stay in the background only while the
   server still holds the slot. The server drops a client it has heard
   nothing from for CLIENT_TIMEOUT_TICKS (1000) of its 20 ms ticks, which is
   20 s (bolo/internal/netpacks.h, transport_udp_server.c); a backgrounded
   client sends nothing. A stay shorter than this keeps the game, which
   carries on with the next tick's ping; a longer one goes straight back to
   the menu on return instead of waiting out the client's own timeout in a
   game that can no longer be played. The margin covers the time the radio
   takes to come back. The stay is measured on the wall clock, because
   SDL_GetTicks does not advance while the device sleeps and the server's
   clock does. */
#define IOS_BACKGROUND_KEEP_GAME_MS 15000
static SDL_Time s_backgroundStartNs = 0;

/* -------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------- */
extern void sdl3MessageHandler(const char *message, const char *title);
static void windowRunGameTick(ClientSim *cs);
static void iosEnterBackground(void);
static void iosResumeForeground(ClientSim *cs);
void windowTutorialPause(ClientSim *cs, bool active);

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
    /* A winbolo://host:port link on the command line joins that game from a
       cold start, as it does on the desktop. Any other argument is ignored:
       the map file the desktop accepts there has no meaning in a bundle. */
    const char *cmdLine = "";
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "winbolo://", 10) == 0) {
            cmdLine = argv[i];
            break;
        }
    }

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

    /* Configure audio session.

       SDL's CoreAudio backend sets the category itself every time it opens or
       closes a device, so what is set here only lasts until sound.c opens the
       game sounds.  With game sounds alone open, SDL picks Playback, mixed with
       other apps' audio and lowering it.  When voice opens the microphone, SDL
       moves the session to PlayAndRecord with DefaultToSpeaker (the speaker
       rather than the earpiece), Bluetooth HFP and A2DP, and AirPlay - the
       options voice chat needs - and keeps it there, since voice.c never
       closes the microphone once it has it.  The game sounds pause for a
       moment while it does so.

       The session is not forced to PlayAndRecord from launch (which the hint
       SDL_HINT_AUDIO_CATEGORY = "playandrecord" would do): with Bluetooth HFP
       allowed, a headset that offers both HFP and A2DP is routed over HFP, so
       a player who never uses voice would hear the game in call quality.

       Set here to the same category and options SDL picks for game sounds,
       so other apps' audio is not stopped at launch and SDL finds nothing to
       change when it opens the first device.  Setting the category does not
       ask for microphone permission; that only happens when voice first opens
       the microphone, so a player who never turns voice on is never asked. */
    {
        NSError *err = nil;
        AVAudioSession *session = [AVAudioSession sharedInstance];
        [session setCategory:AVAudioSessionCategoryPlayback
                        mode:AVAudioSessionModeDefault
                     options:AVAudioSessionCategoryOptionMixWithOthers |
                             AVAudioSessionCategoryOptionDuckOthers
                       error:&err];
        if (err) WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "AVAudioSession setCategory failed: %s", [[err localizedDescription] UTF8String]);
        [session setActive:YES error:&err];
        if (err) WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "AVAudioSession setActive failed: %s", [[err localizedDescription] UTF8String]);
    }

    SDL_Init(0);

    /* Logging is off unless WINBOLO_LOG is set in the environment, as on the
       desktop; with it set the file lands in the app's preferences
       directory as winbolo.log. Without this call nothing the WB_LOG
       macros write reaches the console or a file. */
    wb_log_init("WinBolo", "WinBolo", "winbolo.log");
    atexit(wb_log_shutdown);

    /* The bot brain worker pool, sized from the CPU cores as on the
       desktop. Without it every brain thinks serially on the server tick
       thread. */
    if (!serverSimBotPoolInit(0)) {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[iOS] serverSimBotPoolInit failed");
        return 1;
    }

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

    /* Voice runs for the life of the process, as on the desktop: it comes up
       before gameFrontStart, which applies the saved voice settings to it and
       runs the pre-game dialogs that already expect it.  It opens playback
       only; the microphone waits until voice is switched on.  A device that
       will not open is not fatal - voice stays disabled. */
    voiceInit();

    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Starting gameFrontStart...");
    if (gameFrontStart(cmdLine, &keys, FALSE, NULL) == FALSE) {
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
        if (lobbyResult == 0 && !windowIsQuitting()) {
            /* The player left the lobby rather than quitting: back to the
               menu, as the desktop does. No tank exists in the lobby, so the
               game is ended as not played. */
            WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Left lobby, returning to the menu");
            gameFrontEnd(&keys, FALSE, FALSE);
            if (gameFrontStart("", &keys, TRUE, NULL)) {
                goto ios_game_start;
            }
            WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[iOS] gameFrontStart failed after leaving the lobby");
        }
        if (lobbyResult == 0) {
            WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Left lobby, cleaning up");
            gameFrontEnd(&keys, FALSE, TRUE);
            clientMutexDestroy();
            sdl3DrawCleanup();
            soundCleanup();
            voiceCleanup();
            serverSimBotPoolDestroy();
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
                        if (ev.type == SDL_EVENT_FINGER_DOWN || ev.type == SDL_EVENT_FINGER_UP || ev.type == SDL_EVENT_FINGER_MOTION ||
                            ev.type == SDL_EVENT_FINGER_CANCELED) {
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
                        iosResumeForeground(cs);
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
                    if (rawEv.type == SDL_EVENT_FINGER_DOWN || rawEv.type == SDL_EVENT_FINGER_UP || rawEv.type == SDL_EVENT_FINGER_MOTION ||
                        rawEv.type == SDL_EVENT_FINGER_CANCELED) {
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
                    iosEnterBackground();
                } else if (ev.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
                    iosResumeForeground(cs);
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
                /* A tutorial message is up, or the tick just run queued one:
                   stop here and drop the owed time, so the interval the
                   message is up is not fast-forwarded afterwards. */
                if (s_tutorialPaused || clientFrontTutorialStepShowing()) {
                    gameTickAccum = 0.0;
                    break;
                }
                gameTickAccum -= GAME_TICK_LENGTH;
                windowRunGameTick(cs);
            }
        }

        /* Voice encode/decode runs here, on the main thread, beside the game
           tick, once a frame as on the desktop - the codec state has no lock
           of its own. */
        voiceTick(cs);

        /* Render */
        tick = SDL_GetTicks();
        clientMutexWaitFor();
        if (finishedLoop == FALSE) {
            clientFrontRenderPrepare(cs, tick);
            clientRenderFrame(cs, redraw);
        }
        clientMutexRelease();
        /* The in-window map overview draws from the snapshot the frame above
           filled, now that the lock is off. The tablet overlay's map button
           turns the mode on, and the map is drawn inside the game view. The
           call is a no-op when no frame is waiting. */
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
        /* Tutorial: count the presented frame (the intro waits for a few)
           and show the respawn message the server may have flagged. Both do
           nothing outside the tutorial. */
        frontEndTutorialNotePresentedFrame();
        clientFrontTutorialPollRespawn();

        /* Cap to 60 FPS */
        {
            Uint64 frameElapsed = SDL_GetTicks() - frameCapStart;
            if (frameElapsed < IOS_FRAME_CAP_MS) {
                SDL_Delay((Uint32)(IOS_FRAME_CAP_MS - frameElapsed));
            }
        }
    }

    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Main loop ended, cleaning up");

    /* Release the remote talkers with the game they belong to - player
       numbers are handed out afresh next time, so a stale decoder would be
       fed someone else's voice. */
    voiceReset();

    sdl3ImguiCleanup();
    gameFrontEnd(&keys, TRUE, winboloQuit);
    /* A step left showing, or its pause, would stop the next game's ticks. */
    frontEndTutorialReset();
    windowTutorialPause(NULL, false);

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
    voiceCleanup();
    serverSimBotPoolDestroy();
    SDL_Quit();
    return 0;
}

/* -------------------------------------------------------
 * iosEnterBackground / iosResumeForeground — pause the game while the app
 * is in the background, and on return either carry on or, for a joined game
 * the server has dropped by now, go back to the menu at once
 * ------------------------------------------------------- */
static void iosEnterBackground(void) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Entering background, pausing");
    paused = TRUE;
    soundSetMuted(TRUE);
    if (!SDL_GetCurrentTime(&s_backgroundStartNs)) {
        s_backgroundStartNs = 0;
    }
}

static void iosResumeForeground(ClientSim *cs) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Resuming from background");
    paused = FALSE;
    lastFrameTime = SDL_GetTicks();
    gameTickAccum = 0.0;
    soundSetMuted(FALSE);

    /* Single player keeps its game: its server is in this process and was
       paused with it. With no start time the client's own timeout still
       ends a dead game within ten seconds. */
    SDL_Time nowNs = 0;
    if (cs == NULL || !clientSimHasTransport(cs) || gameFrontGetServerSim() != NULL ||
        clientSimGetNetType(cs) != netUdp || s_backgroundStartNs == 0 ||
        !SDL_GetCurrentTime(&nowNs)) {
        s_backgroundStartNs = 0;
        return;
    }
    Sint64 awayMs = (Sint64)((nowNs - s_backgroundStartNs) / SDL_NS_PER_MS);
    s_backgroundStartNs = 0;
    if (awayMs < IOS_BACKGROUND_KEEP_GAME_MS) {
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                    "[iOS] Back after %lld ms; keeping the network game",
                    (long long)awayMs);
        return;
    }
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                "[iOS] Back after %lld ms; the server has dropped this client",
                (long long)awayMs);
    /* The same path as the lost-connection check in windowRunGameTick. */
    clientSimConnectionLost(cs);
    imguiMessageBoxEx(DIALOG_BOX_TITLE,
                      langGetText(NETERR_LOSTCONNECTION_RETURN_MENU),
                      IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    finishedLoop = TRUE;
    winboloQuit = FALSE;
}

/* -------------------------------------------------------
 * windowRunGameTick — one half-step of the shared client tick, plus the
 * driver-side work around it (disconnect check, AI, stats)
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

    /* One keys/game/lobby half-step lives in the shared client-frontend tick
     * core (desktop and web call the same one); the brain run and the
     * per-second stat rollover below stay here in the driver.  The main loop
     * owns the catch-up, calling this once per owed half-step. */
    ttick = SDL_GetTicks();
    if (clientFrontRunTickStep(cs)) {
        t2++;
        ticks++;
        used = TRUE;
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
/* Raised only by windowSetQuitting, exactly as on the desktop: winboloQuit
 * cannot answer the question on its own, because the game loop sets it TRUE
 * on the way in as its default answer.  See windowIsQuitting in
 * src/gui/sdl3/winbolo.c. */
static bool quitRequested = FALSE;
void windowSetQuitting(void) { quitRequested = TRUE; winboloQuit = TRUE; finishedLoop = TRUE; }
bool windowIsQuitting(void)   { return quitRequested; }

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

/* windowShowGunsight_toggle, windowAutomaticScrolling_toggle,
   windowShowPillLabels_toggle, windowShowBaseLabels_toggle, windowKeyPressed
   and frontEndRedrawAll come from client_frontend_common.c, the one body
   every platform calls; see "Platform variants" in docs/ARCHITECTURE.md. */

void windowSoundEffects_toggle(void) { soundEffects = !soundEffects; }
void windowPositionalSound_toggle(void) { positionalSound = !positionalSound; }
void windowBackgroundSoundChange_toggle(void) { backgroundSound = !backgroundSound; }
void windowSoundKeepalive(void) { useSoundKeepalive = !useSoundKeepalive; }
void windowSetSoundVolume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    soundVolume = pct;
    soundSetEffectsVolume(pct);
}
void windowSetMasterVolume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    windowMasterVolume = pct;
    soundSetMasterVolume(pct);
    voiceSetMasterVolume((float)pct / 100.0f);
}
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

/* The Session Info panels, drawn in-window as the web's are. */
void windowShowGameInfo(windowShowRequest req) { sdl3ImguiShowGameInfo(req == wsrOpen); }
void windowShowSysInfo(windowShowRequest req) { sdl3ImguiShowSysInfo(req == wsrOpen); }
void windowShowNetInfo(windowShowRequest req) { sdl3ImguiShowNetInfo(req == wsrOpen); }
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

void windowButtonAdd(int keyCode) { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
    (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowStartTutorial(void) {}
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* Pause hooks driven from sdl3imgui.cpp. The first two exist for desktop
   situations iOS does not have: a Steam Deck pause menu and a lost
   controller. The app already stops ticking when it is backgrounded. */
void windowControllerLostPause(ClientSim *cs, bool active) { (void)cs; (void)active; }
void windowDeckPause(ClientSim *cs, bool active) { (void)cs; (void)active; }

/* Tutorial message overlay opened or closed, driven by its open/close edge in
   the render path. Solo games only, as on the desktop: the main loop stops
   ticking, the hosted server is paused and the sound muted while a message is
   up. Closing drops the owed tick time, so a held touch or the tap that
   dismissed the message does not run on into the game. */
void windowTutorialPause(ClientSim *cs, bool active) {
    if (active) {
        bool solo = clientFrontTutorialStepShowing() ||
                    (cs != NULL && clientSimIsSinglePlayer(cs));
        if (!solo || s_tutorialPaused) return;
        s_tutorialPaused = TRUE;
        gameFrontSetServerPaused(TRUE);
        s_tutorialPrevMuted = soundIsMuted();
        soundSetMuted(TRUE);
    } else {
        if (!s_tutorialPaused) return;
        s_tutorialPaused = FALSE;
        gameFrontSetServerPaused(FALSE);
        gameTickAccum = 0.0;
        lastFrameTime = SDL_GetTicks();
        soundSetMuted(s_tutorialPrevMuted);
    }
}

/* -------------------------------------------------------
 * Frontend callbacks (matching Android android_frontend.c)
 * ------------------------------------------------------- */

/* frontEndSetActiveClientSim and the active-sim test the callbacks here make
   (clientFrontIsActiveUiSim) come from client_frontend_common.c, the one body
   the desktop, web and iOS drivers call. */

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
    BYTE fullShells, fullMines, fullArmour, fullTrees;
    if (!clientFrontIsActiveUiSim(cs)) return;
    clientSimGetTankFullStats(cs, &fullShells, &fullMines, &fullArmour, &fullTrees);
    sdl3DrawStatusTankBars(0, 0, shells, mines, armour, trees,
                           fullShells, fullMines, fullArmour, fullTrees);
}

void frontEndUpdateBaseStatusBars(ClientSim *cs, BYTE shells, BYTE mines, BYTE armour) {
    BYTE fullShells, fullMines, fullArmour;
    if (!clientFrontIsActiveUiSim(cs)) return;
    clientSimGetBaseFullStats(cs, &fullShells, &fullMines, &fullArmour);
    sdl3DrawStatusBaseBars(0, 0, shells, mines, armour,
                           fullShells, fullMines, fullArmour, FALSE);
}

void frontEndPlaySound(ClientSim *cs, sndEffects value) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (soundEffects) soundPlayEffect(value);
}

void frontEndPlaySoundPan(ClientSim *cs, sndEffects value,
                          uint16_t gainL, uint16_t gainR) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (soundEffects) {
        /* The player has turned positional sound off, so every sound plays
           centred; the near/far variant is already chosen. */
        if (!positionalSound) {
            gainL = SOUND_GAIN_UNITY;
            gainR = SOUND_GAIN_UNITY;
        }
        soundPlayEffectPan(value, gainL, gainR);
    }
}

void windowPlaySound(sndEffects value) {
    if (soundEffects) soundPlayEffect(value);
}

void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
    sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    sdl3DrawStatusTank(tankNum, ts);
    sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    sdl3DrawStatusBase(baseNum, bs, showBaseLabels);
    sdl3DrawCopyBasesStatus(0, 0);
}

void frontEndMessages(ClientSim *cs, char *top, char *bottom) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (drawBusy == FALSE) sdl3DrawMessages(0, 0, top, bottom);
}

void frontEndKillsDeaths(ClientSim *cs, int kills, int deaths) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (drawBusy == FALSE) sdl3DrawKillsDeaths(0, 0, kills, deaths);
}

void frontEndManStatus(ClientSim *cs, bool isDead, TURNTYPE angle) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    clientMutexWaitFor();
    sdl3DrawSetManStatus(0, 0, isDead, angle);
    clientMutexRelease();
}

void frontEndManClear(ClientSim *cs) {
    if (!clientFrontIsActiveUiSim(cs)) return;
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
    if (!clientFrontIsActiveUiSim(cs)) return;
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Game over (time limit expired)");
    finishedLoop = TRUE;
}

void frontEndClearPlayer(struct ClientSim *cs, playerNumbers value) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    sdl3ImguiClearPlayer((unsigned char)value);
}

void frontEndSetPlayer(ClientSim *cs, playerNumbers value, char *str, const char *countryCode, uint16_t ping, uint8_t clientType, uint8_t clientFlags) {
    char cc[3];
    if (!clientFrontIsActiveUiSim(cs)) return;
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
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (!clientSimIsRunning(cs)) return;
    sdl3ImguiUpdatePlayerPing((unsigned char)value, ping);
}

/* Runs off the snapshot path, so it stays cheap: the badge cache the player
   list draws from is updated and nothing is drawn here. */
void frontEndUpdatePlayerFlags(ClientSim *cs, playerNumbers value,
                               uint8_t clientType, uint8_t clientFlags) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    sdl3ImguiUpdatePlayerFlags((unsigned char)value, clientType, clientFlags);
}

void frontEndSetPlayerCheckState(struct ClientSim *cs, playerNumbers value, bool isChecked) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    sdl3ImguiSetPlayerCheckState((unsigned char)value, isChecked);
}

void frontEndApplyLocalTankPrefs(struct ClientSim *cs) { (void)cs; }

void frontEndEnableRequestAllyMenu(bool enabled) { (void)enabled; }
void frontEndEnableLeaveAllyMenu(bool enabled) { (void)enabled; }

void frontEndShowGunsight(ClientSim *cs, bool isShown) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    showGunsight = !isShown;
    clientSimSetGunsight(cs, showGunsight);
}

void frontEndShowAllianceRequest(char *playerName, BYTE playerNum) {
    sdl3ImguiShowAllianceRequest(playerName, playerNum);
}

/* frontEndTutorial and frontEndTutorialReset come from
   client_frontend_common.c, the tutorial step sequencer every platform
   calls. */

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
