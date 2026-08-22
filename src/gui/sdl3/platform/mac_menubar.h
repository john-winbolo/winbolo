/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#ifdef __APPLE__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Per-player slot state mirrored into the native Players menu. Populated
 * each frame from the in-window caches; mac_menubar_refresh() swaps a
 * rich custom view in/out depending on `enabled` and updates the view's
 * contents (checkmark, flag, platform/WBN/Steam icons, name, ping). */
struct MacPlayerSlot {
    bool enabled;          /* whether slot is occupied */
    bool checked;          /* selection state */
    int  pflags;           /* PLAYER_FLAG_* bits */
    int  ptype;            /* ClientType enum value */
    int  ping;             /* fresh ping in ms (0 = unknown) */
    int  pingBand;         /* PingBand enum value (0 = PING_BAND_NONE) */
    char name[33];         /* player name (null-terminated) */
    char country[3];       /* ISO 3166-1 alpha-2 (null-terminated) */
};

/* Build and install the native macOS NSMenu on NSApp.mainMenu.
 * Safe to call once after the main SDL_Window and ImGui context exist.
 * `win` is the main game window (saved for future menu items that need
 * to drive AppKit on a specific window); `clientSim` is the ClientSim
 * pointer that future trampolines will route to. Either may be NULL —
 * the skeleton phase only wires Quit, which needs neither. Callers that
 * do not have a ClientSim in scope should pass NULL and route the
 * pointer through a separate setter once it is live. */
void mac_menubar_install(struct SDL_Window *win, void *clientSim);

/* Install only the macOS Dock-icon menu (no main menu-bar items).
 * Safe to call as soon as NSApp.delegate exists — typically right
 * after sdl3DrawSetup so the Dock menu is live from the welcome
 * screen, well before mac_menubar_install() is called from
 * sdl3ImguiSetup at game start. Idempotent. */
void mac_menubar_install_dock_menu(void);

/* Update the ClientSim pointer the menubar trampolines route to.
 * Safe to call every frame; the value is stashed verbatim. NULL is
 * acceptable (trampolines that need it will no-op). */
void mac_menubar_set_clientsim(void *clientSim);

/* Snapshot of UI state that the native menu mirrors. Populated by
 * sdl3imgui.cpp once per frame and passed to mac_menubar_refresh() to
 * sync NSMenuItem .state / .enabled / .title with the in-window menu. */
struct MacMenuState {
    /* Edit menu */
    int  frameRate;        /* current FRAME_RATE_* value; matched against item tag */
    int  zoomFactor;       /* current ZOOM_FACTOR_* value; matched against item tag */
    bool fit1x;            /* whether 1x window size fits on the current display */
    bool fit2x;            /* 2x */
    bool fit3x;            /* 3x */
    bool fit4x;            /* 4x */
    bool smoothScrolling;
    bool autoScrolling;
    bool showGunsight;
    bool showPillLabels;
    bool showBaseLabels;
    bool noOwnLabel;       /* !labelSelf — drawn checked when own-tank label is hidden */
    int  labelMsg;         /* lblShort / lblLong; matched against item tag */
    int  labelTank;        /* lblNone / lblShort / lblLong; matched against item tag */
    /* WinBolo menu */
    bool allowNewPlayers;
    bool soundEffects;
    bool backgroundSound;
    bool useSoundKeepalive;
    int  soundVolume;          /* 0-100; Volume submenu shows preset checkmark */
    bool newswireMessages;
    bool assistantMessages;
    bool aiMessages;
    bool networkStatusMessages;
    bool networkDebugMessages;
    /* File menu popouts */
    bool sysInfoOpen;
    bool netInfoOpen;
    bool gameInfoOpen;
    /* Players menu popouts */
    bool sendMsgOpen;
    /* Alliance gating — mirrors the in-window Players menu's pre-compute
     * so the native Request/Leave Alliance items grey out identically. */
    bool canRequest;       /* any unallied, checked peer eligible to request */
    bool hasAllies;        /* self has at least one current ally */
    bool inCooldown;       /* request cooldown window currently active */
    /* In-game vote gating — mirrors the same pre-compute (running flag,
     * two-team check, unassigned-team check) used by the in-window
     * Players menu so the native Vote: items enable/disable in lockstep. */
    bool voteRunning;          /* clientSimGetNetStatus(cs) == netRunning */
    bool voteCanSurrender;     /* running && exactly 2 active human teams && self on a team */
    /* Per-player slots — 16 fixed entries, indexed by player number.
     * mac_menubar_refresh() reads `enabled` to swap the slot's NSMenuItem
     * between a numeric "1".."16" placeholder and a rich WBPlayerSlotView. */
    struct MacPlayerSlot players[16];
    /* Brains submenu — data-driven; item count and names change at runtime.
     * The Manual entry is always present; the Settings entry is added only
     * when a Lua brain is running (ONNX has no settings). The parent menu
     * is enabled-gated on aiActive so the user sees the Brains title even
     * when the local tank is not an AI. */
    bool aiActive;             /* clientSimGetAiType(cs) != aiNone — parent enable */
    bool brainRunning;         /* luaBrainIsRunning() */
    int  brainRunIdx;          /* luaBrainGetRunningIndex(); -1 if none */
    int  brainCount;           /* min(luaBrainGetNum(), 16) — snapshot cap */
    bool brainSettingsShown;   /* brainRunning && !mlBrainSingletonIsRunning() */
    char brainNames[16][64];   /* first brainCount entries valid; trailing entries undefined */
};

/* Push the snapshot into the native menu. Walks cached NSMenuItem
 * pointers and writes .state / .enabled / .title from `s`. Cheap to
 * call every frame; NULL is a no-op. */
void mac_menubar_refresh(const struct MacMenuState *s);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
