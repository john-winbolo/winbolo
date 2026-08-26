/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Log viewer host stubs (wasm)
 *Filename:      lv_stubs_wasm.c
 *Purpose:
 *  The half of the log viewer host the browser build does
 *  not compile.  src/wasm links the decode/render TUs plus
 *  lv_embed.c, but not logviewer.c (the modal viewer and the
 *  spectator loop), nor the viewer's ImGui panels
 *  (imgui_context, imgui_events, imgui_item_info,
 *  imgui_game_info, imgui_comments, imgui_main_menu), nor
 *  sound.c.  imgui_comments.cpp and imgui_logviewer_menu.cpp
 *  reach WinBolo.net through http.c, which carries a hard
 *  #error under Emscripten because it embeds the WBN signing
 *  key, so those cannot be built here at all.
 *
 *  screen.c, draw.c and lv_embed.c still call back into that
 *  surface.  Everything below is either a no-op that the
 *  embed's own state makes unreachable or inert, or — for the
 *  two entry points the decoder's behaviour depends on — the
 *  same body logviewer.c has.  Each carries the path that
 *  reaches it.
 *
 *  Declarations come from the real headers where one exists;
 *  the lv_frontEnd* / lv_updateItem / lv_startOfLog /
 *  lv_window* forwarders have no header of their own and are
 *  written to match screen.c's hand-declarations exactly.
 *
 *  This is a log viewer translation unit that happens to live
 *  under src/wasm: it includes backend.h and no bolo header,
 *  so the two worlds' screenObj layouts never meet here.
 *********************************************************/

/* Spelled out relative to src/logviewer rather than leaning on the target's
 * include path: src/gui/sdl3/draw.h and src/gui/sound.h are on that path
 * ahead of the log viewer's, and they are the bolo-world headers this
 * translation unit must not see. */
#include "../logviewer/logviewer.h"
#include "../logviewer/backend.h"
#include "../logviewer/draw.h"
#include "../logviewer/sound.h"
#include "../logviewer/imgui/imgui_comments.h"
#include "../logviewer/imgui/imgui_context.h"
#include "../logviewer/imgui/imgui_events.h"
#include "../logviewer/imgui/imgui_main_menu.h"

/* ----- Playback drawing ------------------------------------------------
 * Reached every frame: lvEmbedFrameTexture -> lv_screenUpdate ->
 * lv_frontEndDrawMainScreen.  This is what fills the render target the host
 * then draws, so it forwards to lv_drawMainScreen exactly as logviewer.c
 * does rather than being stubbed out. */
void lv_frontEndDrawMainScreen(screen *value, screenMines *mineView, screenTanks *tks,
                               screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                               int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
    lv_drawMainScreen(value, mineView, tks, gs, sBullet, lgms,
                      FALSE, FALSE, srtDelay, isPillView, edgeX, edgeY, FALSE, 0, 0);
}

/* ----- End of log ------------------------------------------------------
 * Reached from lv_screenLogTick on LOG_QUIT, so it clears the playing flag
 * the same way logviewer.c does — without it the replay timer keeps
 * rescheduling itself past the end of the round.  The state pointer comes
 * from lv_screenGetState() because g_lv's declaration (lv_host.h) is
 * internal to src/logviewer. */
void lv_finished(void) {
    LogViewerState *lv = lv_screenGetState();
    if (lv != NULL) {
        lv->playIsPlaying = FALSE;
    }
}

/* Start-of-log notification. Empty in logviewer.c too — the viewer's controls
 * panel reads isLoaded each frame instead. */
void lv_startOfLog(void) {
}

/* ----- Sound -----------------------------------------------------------
 * Reached from lv_soundDist on every logged sound event.  logviewer.c only
 * forwards it when isSoundsPlaying is set, and lvEmbedBegin clears that — the
 * embed never calls lv_soundSetup — so the browser build drops the effect
 * here and leaves sound.c out of the link. */
void lv_frontEndPlaySound(sndEffects value) {
    (void)value;
}

/* Reached only through lvHostTeardownCommon(withImGui = true), which is the
 * standalone viewer's and the spectator's teardown; lvEmbedEnd passes false. */
void lv_soundCleanup(void) {
}

/* ----- Viewer panel forwarders -----------------------------------------
 * logviewer.c hands these straight to ImGui panels that only exist inside the
 * modal viewer's own context.  The browser build has no such panels, so the
 * decoder's notifications have nowhere to go and are dropped. */
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor,
                                   BYTE versionRevision, char *mapName, BYTE gameType,
                                   bool hiddenMines, BYTE aiType, int32_t startDelay,
                                   int32_t timeLimit, BYTE *wbnKey, int32_t startTime) {
    (void)clear; (void)versionMajor; (void)versionMinor; (void)versionRevision;
    (void)mapName; (void)gameType; (void)hiddenMines; (void)aiType;
    (void)startDelay; (void)timeLimit; (void)wbnKey; (void)startTime;
}

void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y,
                   BYTE armour, BYTE shells, BYTE mines, bool inTank) {
    (void)itemType; (void)itemNumber; (void)owner; (void)x; (void)y;
    (void)armour; (void)shells; (void)mines; (void)inTank;
}

void lv_windowAddEvent(int eventType, char *msg) {
    (void)eventType; (void)msg;
}

void lv_windowRemoveEventsAfter(uint32_t timeMs) {
    (void)timeMs;
}

/* Called by lvEmbedBegin and lv_windowStop to empty the events list. Nothing
 * holds one here, so there is nothing to clear. */
void lv_imgui_events_clear(void) {
}

/* ----- Viewer ImGui context --------------------------------------------
 * Both reached only through lvHostTeardownCommon(withImGui = true) — the
 * viewer's own ImGui context, which the embed never brings up (the host's
 * frame is already live).  lv_imgui_comments_shutdown belongs to the
 * WinBolo.net comments panel, which is not compiled for the browser at all. */
void lv_imgui_comments_shutdown(void) {
}

void lv_imgui_context_shutdown(void) {
}

/* draw.c offsets the game area by the viewer's menu bar height. It reads the
 * offset as 0 whenever draw is in embed mode (lvDrawMenuBarOffset short-
 * circuits on the embedded flag), which is the only mode the browser build
 * runs, so this is never reached; 0.0f matches what a viewer with no rendered
 * menu bar would report. */
float lv_imgui_get_menu_bar_height(void) {
    return 0.0f;
}
