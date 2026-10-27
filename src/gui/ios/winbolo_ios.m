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
#import <UIKit/UIKit.h>
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
#include "bolo_rand.h"
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
#include "ios_discovery.h"
#include "ios_netpath.h"
#include "../../winbolonet/http.h"
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
 * iPad: the desktop layout, the classic game screen with the menu bar,
 * scaled to fit. The phone always runs the touch layout. See uiModeDetect.
 * ------------------------------------------------------- */

bool iosDeviceIsPad(void) {
    return [UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPad;
}

/* -------------------------------------------------------
 * Globals (matching winbolo.h externs)
 * ------------------------------------------------------- */

bool isTutorial = FALSE;
/* The Frame Rate choice (a FrameLimit). iOS paces its own screen, so it
   is kept only for the shared code that reads it. */
int frameRate = FRAME_LIMIT_DEFAULT;

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

/* The solo-game freezes (a tutorial message, the pause overlay, the
   controller-disconnected dialog; the last two are drawn only in the iPad's
   desktop layout) are held in client_frontend_common.c, one body with the
   desktop: clientFrontSoloPause and clientFrontAnyPause. */

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

/* A game this device hosts for other players keeps running in the background
   for as long as iOS lets the app run: the server ticks on its own timer
   thread, and the main loop keeps ticking the host's own client, without
   drawing, so the server does not drop the host after 20 s of silence. iOS
   gives about 30 s once asked (beginBackgroundTask). With
   IOS_HOST_BACKGROUND_END_S seconds left the server is shut down, which
   tells every player the game has ended, and the time is handed back; the
   host's return then goes to the menu through the check in
   iosResumeForeground. A return before then carries on. */
#define IOS_HOST_BACKGROUND_END_S 5.0
/* The shutdown's WinBolo.net calls (the session's quit, the round-log
   upload) run on the main thread and wait for their replies, up to 30 s
   each. In the background they get a deadline this many seconds before the
   time runs out (httpSetDeadline), so a slow or unreachable WinBolo.net
   leaves the session open instead of iOS ending the app. */
#define IOS_HOST_SHUTDOWN_MARGIN_S 1.5
static UIBackgroundTaskIdentifier s_hostBackgroundTask;
static bool s_hostBackgroundActive = false;  /* s_hostBackgroundTask is held */

/* -------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------- */
extern void sdl3MessageHandler(const char *message, const char *title);
static void windowRunGameTick(ClientSim *cs);
static void iosEnterBackground(ClientSim *cs);
static void iosResumeForeground(ClientSim *cs);
static void iosClearBackgroundPause(ClientSim *cs);
static void iosHostBackgroundFrame(ClientSim *cs);
void windowSuspendBackground(ClientSim *cs);

/* SDL never queues the app lifecycle events (SDL_SendAppEvent hands them to
   event watches only, so they are handled before iOS's call returns), so
   the game loop's own event handling never sees them. This watch acts on
   them while a game is up (s_lifecycleCs). Going to the background is
   handled here, so the next frame is not drawn. The iPad and the iPhone
   both stop as soon as the app stops being active (Control Center, the
   notification shade, Slide Over, Siri), not only once it is in the
   background: a solo game freezes, a network game carries on without this
   screen. Coming back posts s_resumeEventType, which wakes
   the paused loop on the main thread and resumes there, where the resume's
   message box may run. SDL sends each event twice here; both handlers
   ignore the second. */
static ClientSim *s_lifecycleCs = NULL;
static Uint32 s_resumeEventType = 0;

static bool SDLCALL iosLifecycleWatch(void *userdata, SDL_Event *ev) {
    (void)userdata;
    if (s_lifecycleCs == NULL) return true;
    switch (ev->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        windowSuspendBackground(s_lifecycleCs);
        break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        if (s_resumeEventType != 0) {
            SDL_Event resume;
            SDL_zero(resume);
            resume.type = s_resumeEventType;
            SDL_PushEvent(&resume);
        }
        break;
    default:
        break;
    }
    return true;
}
void windowTutorialPause(ClientSim *cs, bool active);
void windowDeckPause(ClientSim *cs, bool active);
void windowControllerLostPause(ClientSim *cs, bool active);

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

    /* The game's random numbers come from bolo_rand, not the C library's
       rand(), so it is bolo_rand that needs the seed, as on the desktop.
       Left unseeded its state is all zero and every draw is 0: every Open
       respawn took the same start. */
    bolo_srand((uint64_t)time(NULL) ^ (uint64_t)getpid());

    /* The iPad turns to portrait as well as landscape (Info.plist allows it
       there only). Without the hint SDL keeps a window that is not resizable
       in the orientation it was created in, and iPadOS then shrinks the
       landscape app into the portrait screen. Every screen reads the window
       size each frame, so a turn only changes the size. */
    if (iosDeviceIsPad()) {
        SDL_SetHint(SDL_HINT_ORIENTATIONS,
                    "LandscapeLeft LandscapeRight Portrait PortraitUpsideDown");
        /* The iPad's classic screen has no menu bar and is played by touch:
           the touch controls come up when it is touched and go when a key
           is pressed (input_touch.c). */
        inputTouchSetClassicControlsOffered(true);
    }

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

    /* Keep a recording or winbolo:// link the system hands over in any loop,
       and at a cold launch, for the welcome screen (ios_drops.c). Before
       anything pumps events. */
    {
        extern void iosDropsInstall(void);
        iosDropsInstall();
    }

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

    /* The LAN search browses through Bonjour here: iOS refuses the raw mDNS
       socket the desktop uses unless the app holds Apple's multicast
       entitlement. */
    iosDiscoveryInstall();
    /* Which network the device is on, so the host is warned before hosting
       an Internet game on mobile data. */
    iosNetPathInstall();
    /* The move to and from the background reaches the game only through an
       event watch (see iosLifecycleWatch). */
    s_resumeEventType = SDL_RegisterEvents(1);
    SDL_AddEventWatch(iosLifecycleWatch, NULL);

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

    /* The thread manager's mutex, as the desktop creates it right after the
       client mutex (winbolo.c). Without it the handle is NULL, SDL_LockMutex
       does nothing, and the lock the host timer thread and the main thread
       take around the local server is fictional: each overwrites the owner,
       and the timer thread died on serverSimApplyCommand's assertion that
       it held the mutex, at the first bot command of a single-player game. */
    if (threadsCreate(FALSE) == FALSE) {
        WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[iOS] Failed to create the threads mutex");
        clientMutexDestroy();
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
        threadsDestroy();
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
    if (clientFrontInLobbyPhase(cs)) {
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
            threadsDestroy();
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
    bool returnToLobby = FALSE;
    s_lifecycleCs = cs;
    while (!winboloQuit && !finishedLoop) {
        Uint64 frameCapStart = SDL_GetTicks();

        /* Process SDL events */
        {
            SDL_Event ev;
            if (paused && s_hostBackgroundActive) {
                iosHostBackgroundFrame(cs);
                continue;
            }
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
                    }
                    if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                        winboloQuit = TRUE;
                    } else if (ev.type == s_resumeEventType) {
                        /* Posted by iosLifecycleWatch on WILL or DID enter
                           foreground: a short interruption (Control Center,
                           the notification shade, Slide Over, Siri) comes
                           back with DID alone. */
                        iosResumeForeground(cs);
                    }
                }
                continue;
            }
            if (!uiModeIsTablet()) {
                /* The iPad's desktop layout takes the desktop's event
                   handling whole: menu shortcuts, the keys that act on a
                   press, click to build, the wheel and Key Setup's capture.
                   The move to and from the background is iosLifecycleWatch's. */
                sdl3ImguiProcessEvents(cs);
            } else while (SDL_PollEvent(&ev)) {
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
                   ImGui's WantCaptureMouse state. */
                if (rawEv.type == SDL_EVENT_FINGER_DOWN || rawEv.type == SDL_EVENT_FINGER_UP || rawEv.type == SDL_EVENT_FINGER_MOTION ||
                    rawEv.type == SDL_EVENT_FINGER_CANCELED) {
                    int tw = 0, th = 0;
                    if (ren) { SDL_RendererLogicalPresentation m; SDL_GetRenderLogicalPresentation(ren, &tw, &th, &m); }
                    if (tw <= 0 || th <= 0) SDL_GetWindowSize(sdl3DrawGetWindow(), &tw, &th);
                    inputTouchProcessEvent(&rawEv, tw, th);
                }
                if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
                    winboloQuit = TRUE;
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
                if (clientFrontAnyPause() || clientFrontTutorialStepShowing()) {
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

        /* The round has handed the game back to the lobby (a vote to return,
           or a game over): leave the loop before drawing, and show the lobby
           screen from the top, as the desktop does. Drawn here instead, the
           lobby would go through the game frame's in-game lobby (the web
           client's), which in the touch layout sizes it in window points
           inside the frame's larger logical units. */
        if (clientFrontInLobbyPhase(cs)) {
            returnToLobby = TRUE;
            break;
        }

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

        /* The touch layout's overlay is drawn by sdl3ImguiPumpAndRender; the
           desktop layout has none. */
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

    s_lifecycleCs = NULL;

    /* A hosted game that ends in the background (a time limit, say) leaves
       the loop still paused, and nothing else clears the pause: the lobby,
       the shutdown and the menu would run as if in the background, and the
       next game's first frame would wait in SDL_WaitEvent. The lifecycle
       watch does nothing from here on, so clear it now. */
    if (paused) {
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Game ended while in the background; clearing the pause");
        iosClearBackgroundPause(cs);
        s_backgroundStartNs = 0;
    }

    /* Back to the lobby screen: the game and its ClientSim stay, as on the
       desktop. Not if the player quit in the same frame. */
    if (returnToLobby && !winboloQuit && !windowIsQuitting()) {
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Round over, returning to the lobby");
        gameFrontSaveTankPrefs(cs);
        sdl3ImguiCleanup();
        goto ios_game_start;
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
    windowDeckPause(NULL, false);
    windowControllerLostPause(NULL, false);

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
    threadsDestroy();
    return 0;
}

/* -------------------------------------------------------
 * iosEnterBackground / iosResumeForeground — pause the game while the app
 * is in the background, and on return either carry on or, for a joined game
 * the server has dropped by now, go back to the menu at once
 * ------------------------------------------------------- */
/* Ends the background time asked for a hosted game. endGame shuts the
   server down first, which sends every player the server's shutdown. */
static void iosHostBackgroundEnd(bool endGame) {
    if (!s_hostBackgroundActive) return;
    if (endGame && gameFrontHasLocalServer()) {
        /* The Simulator reports no limit (DBL_MAX): no deadline there. */
        double left = [UIApplication sharedApplication].backgroundTimeRemaining;
        bool limited = (left < 1e9);
        if (limited) {
            httpSetDeadline((long)((left - IOS_HOST_SHUTDOWN_MARGIN_S) * 1000.0));
        }
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                    "[iOS] Background time nearly out; ending the hosted game "
                    "(%.1f s left)", limited ? left : -1.0);
        gameFrontShutdownServer();
        if (limited) {
            httpClearDeadline();
        }
        WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Hosted game ended");
    }
    [[UIApplication sharedApplication] endBackgroundTask:s_hostBackgroundTask];
    s_hostBackgroundActive = false;
}

static void iosEnterBackground(ClientSim *cs) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Entering background, pausing");
    paused = TRUE;
    /* The shared suspend (client_frontend_common.c), as on the desktop: in a
       solo game the hosted server is paused too, and the sound is muted with
       the state it had kept for the way back. */
    clientFrontSuspend(cs);
    if (!SDL_GetCurrentTime(&s_backgroundStartNs)) {
        s_backgroundStartNs = 0;
    }
    /* Hosting for other players: ask for background time (see
       IOS_HOST_BACKGROUND_END_S). The expiry handler is the fallback for a
       loop that did not get round to ending the game itself. */
    if (!s_hostBackgroundActive &&
        gameFrontHasLocalServer() && !clientFrontIsSoloSession(cs)) {
        s_hostBackgroundTask = [[UIApplication sharedApplication]
            beginBackgroundTaskWithName:@"WinBolo hosted game"
                      expirationHandler:^{ iosHostBackgroundEnd(true); }];
        s_hostBackgroundActive = (s_hostBackgroundTask != UIBackgroundTaskInvalid);
        if (s_hostBackgroundActive) {
            /* Unlimited while the app is still only resigning active, and
               always in the Simulator. */
            double left = [UIApplication sharedApplication].backgroundTimeRemaining;
            if (left > 1e9) {
                WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                            "[iOS] Hosting: keeping the game running in the background (no limit reported)");
            } else {
                WB_LOG_INFO(WB_LOG_CAT_PLATFORM,
                            "[iOS] Hosting: keeping the game running in the background (%.0f s)",
                            left);
            }
            lastFrameTime = SDL_GetTicks();
            gameTickAccum = 0.0;
        }
    }
}

/* One frame of a hosted game in the background: the events that matter, and
   the host's own client ticks so its connection to the server stays alive.
   Nothing is drawn; iOS ends an app that uses the GPU in the background. */
static void iosHostBackgroundFrame(ClientSim *cs) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
            winboloQuit = TRUE;
        } else if (ev.type == s_resumeEventType) {
            iosResumeForeground(cs);
        }
    }
    if (!paused || winboloQuit) return;
    /* The Simulator never limits background time (it reports DBL_MAX), so
       there the game runs on until the host comes back. */
    if ([UIApplication sharedApplication].backgroundTimeRemaining <
        IOS_HOST_BACKGROUND_END_S) {
        iosHostBackgroundEnd(true);
        return;
    }
    Uint64 now = SDL_GetTicks();
    gameTickAccum += (double)(now - lastFrameTime);
    lastFrameTime = now;
    while (gameTickAccum >= GAME_TICK_LENGTH) {
        gameTickAccum -= GAME_TICK_LENGTH;
        windowRunGameTick(cs);
    }
    SDL_Delay(IOS_FRAME_CAP_MS);
}

/* Undoes iosEnterBackground: clears the pause, hands any background time
   back without shutting the server down, and runs the shared resume. Used by
   iosResumeForeground and by the main loop's exit. */
static void iosClearBackgroundPause(ClientSim *cs) {
    paused = FALSE;
    iosHostBackgroundEnd(false);
    lastFrameTime = SDL_GetTicks();
    gameTickAccum = 0.0;
    /* The shared resume: the server pause is recomputed and the sound put
       back as the suspend found it, so a pause freeze still up stays muted. */
    if (clientFrontResumeBegin()) {
        clientFrontResumeEnd(cs);
    }
}

static void iosResumeForeground(ClientSim *cs) {
    /* WILL and then DID both arrive after a full background; the second
       finds nothing paused. */
    if (!paused) return;
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[iOS] Resuming from background");
    /* Back in time: the hosted game carries on; hand the time back. */
    iosClearBackgroundPause(cs);

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
    /* The iPad's desktop layout reaches this from the menu bar's Window Size
       items. docs/ARCHITECTURE.md ("Renderer / window lifetime") forbids
       recreating the window and renderer for a zoom change: other modules'
       textures were made on the renderer and would be left pointing at a
       destroyed one. So the desktop layout reconfigures in place, as the
       desktop does. The touch layout keeps its rebuild below. */
    if (!uiModeIsTablet()) {
        drawBusy = TRUE;
        clientMutexWaitFor();
        sdl3DrawReconfigureZoom(amount);
        clientMutexRelease();
        drawBusy = FALSE;
        windowSetZoomFactor(amount);
        return;
    }
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

void windowSetFrameRate(int newFrameRate, bool inGame) {
    (void)inGame;
    frameRate = frameLimitValid(newFrameRate) ? newFrameRate : FRAME_LIMIT_DEFAULT;
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

/* windowMenuAllowNewPlayers_toggle and the five message toggles are in
   client_frontend_common.c: they also tell the running game, which these
   copies did not. */

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
void windowShowSendMessages(windowShowRequest req) { sdl3ImguiShowSendMsg(req == wsrOpen); }
void windowShowSetKeys(windowShowRequest req) { if (req == wsrOpen) sdl3ImguiShowKeySetup(); }
void windowShowAboutBox(void) {}

/* Alliance requests from other players are shown, as on the desktop; FALSE
   here had them answered "ignored" in every network game. */
bool windowShowAllianceRequest(void) { return TRUE; }
void windowDisableSound(void) { soundEffects = FALSE; useSoundKeepalive = FALSE; }
bool windowGetBackgroundSound(void) { return backgroundSound; }

void windowRedrawAll(ClientSim *cs) {
    clientMutexWaitFor();
    sdl3DrawRedrawAll(cs, clientSimGetCurrentBuildSelect(cs), NULL, showPillLabels, showBaseLabels);
    clientMutexRelease();
}

/* No save dialog on iOS: the map goes into Documents under a made-up name,
   where the Files app and the map editor's Open both find it. */
void windowSaveMap(ClientSim *cs) {
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!docs || !docs[0]) {
        imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_WBERR_SAVEMAP),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
        return;
    }
    char dir[FILENAME_MAX];
    SDL_strlcpy(dir, docs, sizeof(dir));
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') dir[--n] = '\0';
    clientFrontSaveMapAutoNamed(cs, dir);
}

void windowButtonAdd(int keyCode) { (void)keyCode; }
void windowButtonRemove(int keyCode) { (void)keyCode; }
void windowMouseClick(int xWin, int yWin, int xPos, int yPos) {
    (void)xWin; (void)yWin; (void)xPos; (void)yPos;
}
void windowAllowPlayerNameChange(bool allow) { (void)allow; }

/* The touch layout's Full Screen choice, from Settings. An iOS window always
   fills the screen, so there is no window to switch: the flag is the choice
   alone, and with it on the in-window map fills the screen in place of the
   game view's square (sdl3DrawIsTabletMapFullScreen). Bringing the map up
   when it is chosen in a game is sdl3imgui.cpp's, after the frame. */
void windowFullScreenChoose(bool on) {
    gameFrontFullScreen = on;
    gameFrontSaveCurrentPrefs();
}

/* Window geometry hooks sdl3ImguiProcessEvents calls in the iPad's desktop
   layout. An iOS window always fills the screen: there is no position to
   remember and no size to correct. */

/* Gray or black bars beside the classic screen in the iPad's desktop
   layout, from Settings; the web's body. */
void windowLetterboxBarsGray_toggle(void) {
    letterboxBarsGray = !letterboxBarsGray;
    gameFrontSaveCurrentPrefs();
}
void windowSaveCurrentPosition(void) {}
void windowComputeAspectCorrectSize(int actualW, int actualH, int actualX, int actualY,
                                    int *outW, int *outH, int *outX, int *outY) {
    *outW = actualW;
    *outH = actualH;
    *outX = actualX;
    *outY = actualY;
}

/* The move to and from the background, as sdl3ImguiProcessEvents reports it
   in the iPad's desktop layout; the touch layout's own event loop above does
   the same on the events it reads. */
void windowSuspendBackground(ClientSim *cs) {
    if (paused) return;
    /* The same as the touch layout's: the time away starts here, for the
       network game check on the way back. */
    iosEnterBackground(cs);
}

void windowResumeForeground(ClientSim *cs) {
    iosResumeForeground(cs);
}

/* One solo freeze's open or close edge, through the shared body. Closing
   drops the owed tick time, so a held key, a held touch or the press that
   closed the overlay does not run on into the game. */
static void iosSoloPause(ClientSim *cs, ClientFrontPause which, bool active) {
    if (clientFrontSoloPause(cs, which, active)) {
        gameTickAccum = 0.0;
        lastFrameTime = SDL_GetTicks();
    }
}

/* Pause hooks driven from sdl3imgui.cpp: the controller-disconnected dialog
   and the pause overlay (Escape or a controller's Start), both drawn in the
   iPad's desktop layout, and the tutorial message overlay, driven by its
   open/close edge in the render path. Solo games only, as on the desktop. */
void windowControllerLostPause(ClientSim *cs, bool active) {
    iosSoloPause(cs, CLIENT_FRONT_PAUSE_CONTROLLER_LOST, active);
}
void windowDeckPause(ClientSim *cs, bool active) {
    iosSoloPause(cs, CLIENT_FRONT_PAUSE_DECK, active);
}
void windowTutorialPause(ClientSim *cs, bool active) {
    iosSoloPause(cs, CLIENT_FRONT_PAUSE_TUTORIAL, active);
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

/* The three status callbacks also fire from the hosted server's tick on
   the SDL timer thread, where a renderer call races the main thread's
   present. The per-frame render pass repaints every icon from sim state,
   so off the render thread the direct draw is skipped, as the desktop
   driver does. */
void frontEndStatusPillbox(ClientSim *cs, BYTE pillNum, pillAlliance pb) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (!sdl3DrawOnRenderThread()) return;
    sdl3DrawStatusPillbox(pillNum, pb, showPillLabels);
    sdl3DrawCopyPillsStatus(0, 0);
}

void frontEndStatusTank(ClientSim *cs, BYTE tankNum, tankAlliance ts) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (!sdl3DrawOnRenderThread()) return;
    sdl3DrawStatusTank(tankNum, ts);
    sdl3DrawCopyTanksStatus(0, 0);
}

void frontEndStatusBase(ClientSim *cs, BYTE baseNum, baseAlliance bs) {
    if (!clientFrontIsActiveUiSim(cs)) return;
    if (!sdl3DrawOnRenderThread()) return;
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

/* frontEndGameOver is in client_frontend_common.c; these are this driver's
   parts of it. As on the desktop, every time-limit game over leaves the game:
   a single-player round on iOS connects passively, as on the desktop, so the
   web's test for a practice round that ends in its lobby does not apply. */
bool windowGameOverEndsGame(ClientSim *cs) {
    (void)cs;
    return TRUE;
}

void windowGameOverStopTicks(void) {
}

void windowGameOverLeave(void) {
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

/* The auto-slowdown and auto-hide-gunsight prefs, from gamefront.c, as on
   the desktop. */
void frontEndApplyLocalTankPrefs(struct ClientSim *cs) {
    extern bool useAutoslow;
    extern bool useAutohide;
    if (cs == NULL) return;
    clientSimSetTankAutoSlowdown(cs, useAutoslow);
    clientSimSetTankAutoHideGunsight(cs, useAutohide);
}

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
    if (playerName != NULL) {
        sdl3ImguiShowAllianceRequest(playerName, playerNum);
    }
}

bool winUtilWBSubDirExist(char *subDirName) {
    struct stat st;
    if (stat(subDirName, &st) == 0 && S_ISDIR(st.st_mode)) return TRUE;
    return FALSE;
}
