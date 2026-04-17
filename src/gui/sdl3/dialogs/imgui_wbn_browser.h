/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          imgui_wbn_browser.h
 * Purpose:       WinBolo.net log browser dialog.
 *                Browse, search, download, and comment on
 *                archived game logs from WinBolo.net.
 *********************************************************/

#ifndef IMGUI_WBN_BROWSER_H
#define IMGUI_WBN_BROWSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result actions from the WBN browser dialog */
#define WBN_BROWSER_CLOSE       0
#define WBN_BROWSER_PLAY_FILE   1
#define WBN_BROWSER_PLAY_MEMORY 2
#define WBN_BROWSER_OPEN_LOCAL  3

#ifndef FILENAME_MAX
#define FILENAME_MAX 260
#endif

typedef struct {
    int action;
    char filePath[FILENAME_MAX];
    uint8_t *memoryData;    /* for PLAY_MEMORY (caller takes ownership) */
    size_t memorySize;      /* for PLAY_MEMORY */
} WbnBrowserResult;

/* Show the WBN log browser dialog as a blocking modal loop.
 * Returns a result struct indicating what the user chose. */
WbnBrowserResult imguiWbnBrowserShow(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_WBN_BROWSER_H */
