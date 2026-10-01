/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#ifdef __APPLE__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;
struct LogViewerState;

/* Snapshot of in-window menu state mirrored into the native NSMenu each
 * frame. The shim reads this struct from C/C++ callers without pulling
 * AppKit headers into them. lv_mac_menubar_refresh() walks the cached
 * NSMenuItem pointers and writes .state / .enabled. */
struct LvMenuState {
    bool isLoaded;             /* Save Map, Action items, zoom enable */
    bool playIsPlaying;        /* split for Play / Pause enable */
    bool modeInformation;      /* Mode submenu checkmark */
    bool useTeamColours;       /* Use Team Colours checkmark */
    bool gameViewActive;       /* gameView — disables Use Team Colours */
    bool tankCentred;          /* checkbox */
    bool hideLobby;            /* checkbox */
    bool showRegions;          /* checkbox */
    bool hasRegions;           /* the recording declares regions — enables Regions */
    bool soundEffects;         /* checkbox */
    int  soundVolume;          /* 0-100; Volume submenu checkmark */
    bool dnsLookups;           /* checkbox */
    bool showControls;
    bool showEvents;
    bool showGameInfo;
    bool showItemInfo;
    bool showComments;
    bool showScenarioPanel;
    int  zoomStepIndex;       /* current zoom step */
    int  zoomStepCount;        /* total zoom steps; >0 */
    bool fromMainMenu;         /* embedded — affects File menu's last item */
};

/* Build and install the native macOS NSMenu on NSApp.mainMenu. The
 * previously installed mainMenu (if any) is stashed for restore by
 * lv_mac_menubar_uninstall(). `win` is the LogViewer's main SDL_Window
 * — saved for window-size lookups when centring zoom; may be NULL on
 * embedded if the shim is installed before the LogViewer window is
 * borrowed (in which case zoom-centring falls back to a sane default).
 * `lvState` is the LogViewerState pointer the trampolines read/mutate;
 * required for Save Map, Use Team Colours, Sound Effects, and the
 * Return-to-Main-Menu label. */
void lv_mac_menubar_install(struct SDL_Window *win, struct LogViewerState *lvState);

/* Restore the previously saved NSApp.mainMenu. After this, NSApp's
 * menu bar is whatever it was before install. Symmetric with install. */
void lv_mac_menubar_uninstall(void);

/* Push the snapshot into the native menu. Cheap to call every frame;
 * NULL is a no-op. */
void lv_mac_menubar_refresh(const struct LvMenuState *s);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
