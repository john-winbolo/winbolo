#pragma once

#ifdef __APPLE__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Build and install the native macOS NSMenu on NSApp.mainMenu.
 * Safe to call once after the main SDL_Window and ImGui context exist.
 * `win` is the main game window (saved for future menu items that need
 * to drive AppKit on a specific window); `clientSim` is the ClientSim
 * pointer that future trampolines will route to. Either may be NULL —
 * the skeleton phase only wires Quit, which needs neither. Callers that
 * do not have a ClientSim in scope should pass NULL and route the
 * pointer through a separate setter once it is live. */
void mac_menubar_install(struct SDL_Window *win, void *clientSim);

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
    bool hideMainView;
    bool noOwnLabel;       /* !labelSelf — drawn checked when own-tank label is hidden */
    int  labelMsg;         /* lblShort / lblLong; matched against item tag */
    int  labelTank;        /* lblNone / lblShort / lblLong; matched against item tag */
    char deviceLabel[64];  /* Full Edit > Device menu item title, e.g. "Device iPhone" */
    /* WinBolo menu */
    bool allowNewPlayers;
    bool soundEffects;
    bool backgroundSound;
    bool useSoundKeepalive;
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
};

/* Push the snapshot into the native menu. Walks cached NSMenuItem
 * pointers and writes .state / .enabled / .title from `s`. Cheap to
 * call every frame; NULL is a no-op. */
void mac_menubar_refresh(const struct MacMenuState *s);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
