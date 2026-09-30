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
 *  surface.  Everything below is a no-op that the embed's own
 *  state makes unreachable or inert, and each carries the path
 *  that reaches it.  The callbacks the decoder's behaviour does
 *  depend on are not here: they live in lv_embed.c, which this
 *  build links, so there is one copy of each.
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
 * include path: src/gui/sound.h is on that path ahead of the log viewer's,
 * and it is a bolo-world header this translation unit must not see. */
#include "../logviewer/backend.h"
#include "../logviewer/sound.h"
#include "../logviewer/imgui/imgui_comments.h"
#include "../logviewer/imgui/imgui_context.h"
#include "../logviewer/imgui/imgui_events.h"
#include "../logviewer/imgui/imgui_main_menu.h"

/* Read by draw.c's script-region overlay, which also returns early in embed
 * mode. The real flag lives in imgui_main_menu.cpp, which this build does not
 * compile, and the embed has no menu to turn it on. */
bool lv_g_show_regions = false;

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
