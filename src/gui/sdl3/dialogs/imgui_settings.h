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

#ifdef __cplusplus
extern "C" {
#endif

/* Show the pre-game settings dialog as a blocking modal loop. */
void imguiSettingsShow(void);

typedef struct SettingsRenderCtx {
    struct ClientSim *cs;       /* NULL pre-game; live sim in-game */
    bool inGame;                /* true = in-game overlay shell */
    /* outputs the section sets, handled by the shell after the frame: */
    bool wantKeySetup;          /* Set Keys pressed */
    bool wantAtlasRebuild;      /* a section needs a font-atlas rebuild
                                   (CJK language pick OR UI-scale change) */
    bool wantClose;             /* a section requested close (e.g. Tutorial) */
    bool prefsDirty;            /* a section mutated a persisted setting */
} SettingsRenderCtx;

/* Render the shared Labels / Sound / Messages settings categories.  Called by
 * both the pre-game dialog and the in-game settings overlay so the two can't
 * drift.  Reads ctx->cs (NULL pre-game, no live sim). */
void imguiSettingsRenderCommonSections(SettingsRenderCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_SETTINGS_H */
