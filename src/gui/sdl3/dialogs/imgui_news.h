/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_news
 * Filename:      imgui_news.h
 * Purpose:
 *   Welcome-screen news popup. Polls the wbn_news async
 *   fetch handle each frame, opens a consent dialog the
 *   first time the player launches with a populated feed,
 *   auto-opens the news modal when AutoShow=show and a
 *   newer item exists, and surfaces the unread state to
 *   the welcome screen's "News" mini-button for the
 *   AutoShow=dontShow case. Lives in the SDL3 client only.
 *********************************************************/

#ifndef IMGUI_NEWS_H
#define IMGUI_NEWS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when the popup is currently open. */
bool newsPopupIsOpen(void);

/* True when the fetched feed contains items with id > LastSeenId.
 * Used by the welcome-screen "News" mini-button to render an
 * unread dot. Returns false while the fetch is in flight, failed,
 * or every item has already been seen. */
bool newsPopupHasUnread(void);

/* Kick a fetch in the background; safe to call repeatedly. Starts
 * at most one fetch per program run.                            */
void newsPopupKickFetch(void);

/* Driver — called every welcome-screen frame. Handles:
 *   - polling the fetch
 *   - showing the first-run consent dialog when pref==unset
 *   - opening the news modal when there is news the user hasn't seen
 *   - rendering the modal if open
 *   - writing last_seen_id on close                                 */
void newsPopupTick(void);

/* "View News" button handler — forces the modal open with whatever
 * items are currently cached (or "loading…" if the fetch is still
 * in flight).                                                       */
void newsPopupOpenManual(void);

/* Free the cached fetch + image cache. Called once during GUI
 * teardown.                                                         */
void newsPopupShutdown(void);

/* [NEWS] AutoShow accessors. `value` must be the literal "show" or
 * "dontShow"; passing "unset" is undefined for external callers. */
const char *newsPrefGetAutoShow(void);
void        newsPrefSetAutoShow(const char *value);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_NEWS_H */
