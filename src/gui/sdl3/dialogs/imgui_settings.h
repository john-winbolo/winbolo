/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          imgui_settings.h
 * Purpose:       ImGui pre-game settings dialog.
 *********************************************************/

#ifndef IMGUI_SETTINGS_H
#define IMGUI_SETTINGS_H

#include "../../lang.h"  /* LangFileEntry for the shared language picker */

#ifdef __cplusplus
extern "C" {
#endif

/* Show the pre-game settings dialog as a blocking modal loop. */
void imguiSettingsShow(void);

typedef struct SettingsRenderCtx {
    struct ClientSim *cs;       /* NULL pre-game; live sim in-game */
    bool inGame;                /* true = in-game overlay shell */
    unsigned char pendingZoom;  /* in-game window-size pick: a ZOOM_FACTOR_*, or
                                   255 = no change (0 is ZOOM_FACTOR_CUSTOM, a
                                   valid value, so it can't be the sentinel) */
    signed char pendingFullScreen;  /* -1 = no change, 0 = turn off, 1 = turn on */
    /* outputs the section sets, handled by the shell after the frame: */
    bool wantKeySetup;          /* Set Keys pressed */
    bool wantAtlasRebuild;      /* a section needs a font-atlas rebuild
                                   (CJK language pick OR UI-scale change) */
    bool wantSkinReload;        /* the skin changed; shell calls
                                   gameFrontReloadSkins() after the frame */
} SettingsRenderCtx;

/* Render the shared Display tab (frame rate, window size, UI scale, letterbox,
 * full screen, skin).  Window size and UI scale only apply in-game; their
 * results are returned via ctx->pendingZoom / ctx->wantAtlasRebuild for the
 * shell to act on after the frame, the full screen pick likewise via
 * ctx->pendingFullScreen — the window must not be moved mid-frame — and a skin
 * pick via ctx->wantSkinReload. */
void imguiSettingsRenderDisplayTab(SettingsRenderCtx *ctx);

/* Render the shared Sound tab (sound toggles, volume, and the voice section).
 * Every control applies through its own setter as it is changed, so the shell
 * has nothing to act on afterwards. */
void imguiSettingsRenderSoundTab(SettingsRenderCtx *ctx);

/* Render the shared Game/HUD tab (scrolling behaviour, gunsight).  ctx->cs is
 * NULL pre-game; the toggles tolerate it. */
void imguiSettingsRenderGameHudTab(SettingsRenderCtx *ctx);

/* Render the shared Hosting tab (settings for a game hosted from the finder).
 * Shown in both settings shells; renders the port, spectator, map-upload,
 * replay-logging and visibility rows, with the apply-note last. */
void imguiSettingsRenderHostingTab(SettingsRenderCtx *ctx);

/* Render the shared General tab (validated player name + WinBolo.net account).
 * On a successful name change it always persists via gameFrontSetPlayerName,
 * and updates the live sim too when ctx->cs is non-NULL. */
void imguiSettingsRenderGeneralTab(SettingsRenderCtx *ctx);

/* Seed the shared player-name edit buffer from the persisted name. Call when
 * opening a settings shell and after the pre-game modal rebuilds its context. */
void imguiSettingsSeedPlayerName(void);

/* Render the shared language picker (combo + info popup) into the current tab.
 * Each shell passes its own scanned entries and owns their lifecycle.  Sets
 * ctx->wantAtlasRebuild only when the pick changes the CJK font region. */
void imguiSettingsRenderLanguagePicker(LangFileEntry *entries, int count,
                                       SettingsRenderCtx *ctx);

/* Render the shared Controls tab content (the Set Keys button).  Sets
 * ctx->wantKeySetup when pressed; each shell launches key setup after the
 * frame in its own way.  In-game-only controls stay inline in that shell. */
void imguiSettingsRenderControlsTab(SettingsRenderCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_SETTINGS_H */
