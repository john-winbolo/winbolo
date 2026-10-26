/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLIENT_FRONTEND_COMMON_H
#define CLIENT_FRONTEND_COMMON_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

/* The active-sim test every guarded frontEnd* callback makes first. The
 * ClientSim on screen is registered through frontEndSetActiveClientSim
 * (frontend.h), whose one body is in client_frontend_common.c; bot,
 * background-game and spectator sims fire the same callbacks, and theirs must
 * not write into the on-screen UI. True when cs is the registered sim, or when
 * none is registered yet (bootstrapping), so a callback goes ahead. */
bool clientFrontIsActiveUiSim(const struct ClientSim *cs);

/* Whether the player is in the lobby or its countdown: before a game, or
 * after a round has handed the game back to the lobby (a vote to return, or
 * a game over). A driver that shows the lobby as a screen of its own
 * (imguiLobbyShow: the desktop and iOS) tests this before its game loop, and
 * in the loop every frame, where a true answer ends the loop without drawing
 * that frame and goes back to the lobby screen. The web client draws the
 * lobby inside its game frame instead and does not use it. */
bool clientFrontInLobbyPhase(struct ClientSim *cs);

/* Save the running game's map into dir, which is created if missing, under a
 * name made from the map's name and the time (Everard_20261016_031500.map),
 * and tell the player where it went or that it failed. For a player with no
 * file dialog: the desktop under a controller, and iOS. */
void clientFrontSaveMapAutoNamed(struct ClientSim *cs, const char *dir);

/* The solo-game freezes. Each pauses the game ticks, the hosted server and
 * the sound while its overlay or state is up in a solo game (single player or
 * the tutorial), and hands the sound back as it found it. A multiplayer game
 * keeps running. The suspend is the one that also engages outside a solo
 * game: the app going to the background. One body for the desktop and iOS;
 * the web client's pause hooks are no-ops and do not use these. */
typedef enum {
    CLIENT_FRONT_PAUSE_SUSPEND = 0,     /* app in the background */
    CLIENT_FRONT_PAUSE_OVERLAY,         /* the Steam overlay */
    CLIENT_FRONT_PAUSE_CONTROLLER_LOST, /* the controller-disconnected dialog */
    CLIENT_FRONT_PAUSE_DECK,            /* the pause menu (Start / Escape) */
    CLIENT_FRONT_PAUSE_TUTORIAL,        /* a tutorial message */
    CLIENT_FRONT_PAUSE_SAVE_DIALOG,     /* the native save-map dialog */
    CLIENT_FRONT_PAUSE_COUNT
} ClientFrontPause;

/* A solo session: a single-player game. */
bool clientFrontIsSoloSession(struct ClientSim *cs);

/* One solo freeze's open (active) or close edge. Opening does nothing outside
 * a solo game or when already open. Returns true when it closed a freeze that
 * was open: the caller then drops the tick time owed for the paused interval,
 * so it is not run in one burst. */
bool clientFrontSoloPause(struct ClientSim *cs, ClientFrontPause which, bool active);

/* The background suspend, which engages in any game. clientFrontSuspend
 * returns true when it newly suspended. A resume is two calls around the
 * driver's own handling of the network game it comes back to:
 * clientFrontResumeBegin clears the suspend and returns whether there was
 * one, clientFrontResumeEnd recomputes the server pause and puts the sound
 * back as the suspend found it. */
bool clientFrontSuspend(struct ClientSim *cs);
bool clientFrontResumeBegin(void);
void clientFrontResumeEnd(struct ClientSim *cs);

/* Whether any freeze or the suspend is on: the driver runs no game ticks. */
bool clientFrontAnyPause(void);

/* Hide the main menu's Tutorial button, and save that, once a shipped
 * tutorial's script says the player has finished it (game.tutorial_done).
 * Only in a single-player game, and only when the running map's own script
 * is in data/maps/tutorials. Call once a frame with the client mutex free. */
void clientFrontPollTutorialDone(void);

/* The time-limit game over (frontEndGameOver), one body for every driver that
 * compiles this file. The driver supplies what differs:
 *   windowGameOverEndsGame - whether this game over leaves the game at all
 *                            (the web's practice round ends in its lobby);
 *   windowGameOverStopTicks - stop the game's tick timers before the message;
 *   windowGameOverLeave    - end the game loop once the message is dismissed. */
bool windowGameOverEndsGame(struct ClientSim *cs);
void windowGameOverStopTicks(void);
void windowGameOverLeave(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_COMMON_H */
