#pragma once

#ifdef __APPLE__

#include <stdbool.h>

#include "../mapeditor_imgui.h"   /* MapEditorMenuAction, ME_PATH_MAX */

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Snapshot of in-window editor state mirrored into the native NSMenu
 * each frame. mapeditor.c populates this once per frame and passes it
 * to me_mac_menubar_refresh(). Plain struct — never reach back into
 * ImGui from the Objective-C++ side. */
struct MeMenuState {
    bool fromMainMenu;     /* embedded — controls File menu's last item */
    bool canUndo;
    bool canRedo;
    bool hasSelection;     /* Cut/Copy enable + Map > Mirror/Rotate scope label */
    /* Options checkmarks */
    bool showGrid;
    bool showMines;
    bool showPillRanges;
    /* Window submenu checkmarks */
    bool showTerrain;
    bool showTools;
    bool showInspector;
    bool showObjects;
    bool showOverview;
    bool showStats;
    bool showStampLibrary;
    /* Recent files — paths are borrowed; the shim copies only what it
     * needs each frame (basename for the label, full path for tooltip). */
    int  numRecent;
    const char *recentFiles[16];   /* first numRecent slots valid */
    /* Zoom submenu — per-step values can change between frames if the
     * user wheel-zooms, so the shim rebuilds the submenu when count
     * changes and re-checks the active item every frame. */
    int  zoomStepIndex;
    int  zoomStepCount;
    const float *zoomStepValues;   /* zoomStepCount entries */
};

/* Build and install the native macOS NSMenu on NSApp.mainMenu. The
 * previously installed mainMenu (if any) is stashed for restore by
 * me_mac_menubar_uninstall().
 *
 * Standalone (no prior mainMenu): builds the "Map Editor" app menu
 * with Quit / Hide / Hide Others / Show All / Services.
 *
 * Embedded (a prior mainMenu exists): temporarily borrows the host's
 * first menu item — AppKit enforces a 1:1 NSMenuItem↔submenu binding,
 * so the host's submenu is detached and reattached on uninstall.
 *
 * `win` is the editor's SDL_Window — currently unused but accepted so
 * the API matches the WinBolo / Log Viewer shims and future menu items
 * that need an NSWindow can grab it via SDL_GetProperty. */
void me_mac_menubar_install(struct SDL_Window *win);

/* Restore the previously saved NSApp.mainMenu. Symmetric with install.
 * After this returns, NSApp's menu bar is whatever it was before the
 * editor took over. */
void me_mac_menubar_uninstall(void);

/* Push the snapshot into the native menu: enable flags, checkmarks,
 * Map > Mirror/Rotate scope labels, Recent submenu contents, Zoom
 * submenu contents. Cheap to call every frame; NULL is a no-op. */
void me_mac_menubar_refresh(const struct MeMenuState *s);

/* Merge pending NSMenu trampoline clicks into `action` and clear the
 * pending buffer. Called once per frame by mapeditor.c in place of
 * mapEditorImguiMenuBar() on macOS. Zeroes `action` first (the in-
 * window version does the same) then ORs/sets any clicks captured
 * between the previous frame and now. NULL is a no-op. */
void me_mac_menubar_consume_actions(MapEditorMenuAction *action);

#ifdef __cplusplus
}
#endif

#endif /* __APPLE__ */
