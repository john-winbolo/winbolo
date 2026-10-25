/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_mobile_files.h
 * Purpose:       The map editor's file picker on iOS and
 *                Android, where SDL has no native open or
 *                save dialog. Open lists the files of one
 *                kind in the player's own folder and the maps
 *                bundled with the app; save asks for a name
 *                and writes into the player's folder. Which
 *                folders those are is the platform's answer
 *                (mapeditor_mobile_files_platform.h). Drawn
 *                inside the editor's ImGui frame.
 *                Also the one control a touch screen adds for
 *                want of a keyboard: a button that deletes the
 *                selected base, pill or start (Delete on the
 *                desktop).
 *********************************************************/

#ifndef MAPEDITOR_MOBILE_FILES_H
#define MAPEDITOR_MOBILE_FILES_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start a pick. save false lists files ending in ".<ext>"; save true asks
 * for a name, offering suggestedName (may be NULL or ""), and adds ".<ext>"
 * when the name has none. Replaces any pick still open. */
void meMobileFilesBegin(bool save, const char *ext, const char *suggestedName);

/* Draw the picker for this frame. Returns true on the frame it closes, with
 * the chosen full path in outPath, or "" when the player cancelled. */
bool meMobileFilesRender(char *outPath, size_t outCap);

/* True when path is inside the player's own folder, so a plain Save can
 * write to it; a map opened from the bundled set cannot be written back. */
bool meMobileFilesIsWritable(const char *path);

/* Draw the Delete button for a selected base, pill or start, in the window's
 * top-right corner below the menu bar. True on the frame it is tapped. */
bool meMobileObjectDeleteButton(void);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_MOBILE_FILES_H */
