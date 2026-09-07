/*
 * imgui_game_info.h - ImGui game information window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef IMGUI_GAME_INFO_H
#define IMGUI_GAME_INFO_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Render the game info window
 * Should be called each frame when window is visible */
void lv_imgui_game_info_window(void);

/* Initialize game info state */
void lv_imgui_game_info_init(void);

/* Clear game info display (called when log is closed) */
void lv_imgui_game_info_clear(void);

/* Set game info from backend
 * This is called from lv_frontEndSetGameInformation
 * Using C types directly to avoid C++/C type conflicts */
void lv_imgui_game_info_set(int clear, unsigned char versionMajor, unsigned char versionMinor, unsigned char versionRevision,
                         char *mapName, unsigned char gameType, int hiddenMines, unsigned char aiType,
                         int32_t startDelay, int32_t timeLimit, unsigned char *wbnKey, int32_t startTime);

/* Set the lobby settings the recording carries: the raw log_GameSettings
 * payload, whose layout is written out in docs/replay-format.md. A payload
 * shorter than the fields the panel draws, or a length of 0, means the log
 * records no settings and those rows are left out. Independent of
 * lv_imgui_game_info_set — either may arrive first, and both are cleared by
 * lv_imgui_game_info_clear. */
void lv_imgui_game_info_set_settings(const unsigned char *payload, int len);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_GAME_INFO_H */