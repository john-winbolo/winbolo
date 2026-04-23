/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_imgui.h
 * Purpose:
 *   C-compatible interface for ImGui calls used by the
 *   map editor. Implementation is in mapeditor_imgui.cpp.
 *********************************************************/

#ifndef MAPEDITOR_IMGUI_H
#define MAPEDITOR_IMGUI_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <limits.h>

#ifndef ME_PATH_MAX
#define ME_PATH_MAX 1024
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ImGui context and backends for the editor. */
void mapEditorImguiInit(SDL_Window *window, SDL_Renderer *renderer);

/* Set the renderer used for shared UI elements (lock icons, etc.).
 * Called automatically by mapEditorImguiInit; call this separately when
 * using mapGenImguiControls outside the map editor (e.g. map chooser). */
void mapEditorImguiSetRenderer(SDL_Renderer *renderer);

/* Shut down ImGui backends and destroy context. */
void mapEditorImguiShutdown(void);

/* Process an SDL event through ImGui. Returns true if ImGui consumed it. */
bool mapEditorImguiProcessEvent(const SDL_Event *event);

/* Returns true if ImGui wants to capture keyboard input this frame. */
bool mapEditorImguiWantKeyboard(void);

/* Returns true if ImGui wants to capture mouse input this frame. */
bool mapEditorImguiWantMouse(void);

/* Begin a new ImGui frame. */
void mapEditorImguiNewFrame(void);

/* Menu action flags returned from the menu bar each frame. */
typedef struct {
    bool wantExit;
    bool wantNew;
    bool wantOpen;
    bool wantSave;
    bool wantSaveAs;
    bool wantCenter;
    bool wantPointStarts;
    bool toggleGrid;
    bool toggleMines;
    bool togglePillRanges;
    bool wantCut;
    bool wantCopy;
    bool wantPaste;
    bool wantUndo;
    bool wantRedo;
    bool wantValidate;
    bool wantSymMirrorH;
    bool wantSymMirrorV;
    bool wantSymRotate90;
    bool wantSymRotate180;
    bool wantGenerate;
    bool wantText;
    bool wantImageImport;
    bool wantExportPNG;
    int  openRecentIndex;   /* -1 = none, else index into recent files */
} MapEditorMenuAction;

/* Render the menu bar. Populates action with what the user clicked.
 * recentFiles/numRecent: array of recent file paths for the submenu.
 * showGrid/showMines: current toggle state for checkmarks.
 * dirty: true if map has unsaved changes (affects exit/new/open prompts). */
void mapEditorImguiMenuBar(MapEditorMenuAction *action,
                           const char recentFiles[][ME_PATH_MAX],
                           int numRecent,
                           bool showGrid, bool showMines, bool showPillRanges,
                           bool dirty,
                           bool canUndo, bool canRedo,
                           bool hasSelection,
                           bool *showTerrain, bool *showTools,
                           bool *showInspector, bool *showObjects,
                           bool *showOverview, bool *showStats,
                           bool *showStampLibrary,
                           bool fromMainMenu);

/* Render the unsaved changes modal dialog.
 * Must be called every frame after OpenPopup("Unsaved Changes").
 * Returns: 0 = still open, 1 = Yes (save), 2 = No (discard), 3 = Cancel */
int mapEditorImguiUnsavedModal(void);

/* Render an error modal dialog (e.g. validation errors).
 * Returns true when the user dismisses it. */
bool mapEditorImguiErrorModal(const char *message);

/* Render the status bar at the bottom of the window.
 * tileX/tileY: tile coordinate under cursor (-1 if none)
 * zoomLevel:   current zoom level (e.g. 0.5, 1.0, 2.0)
 * dirty:       true if map has unsaved changes
 * startInfo:   optional string like "Start 3/8" (NULL for none) */
void mapEditorImguiStatusBar(int tileX, int tileY, float zoomLevel,
                             bool dirty, const char *startInfo);

/* Tool types */
#define ME_TOOL_PENCIL      0
#define ME_TOOL_LINE        1
#define ME_TOOL_RECT        2
#define ME_TOOL_RECT_FILL   3
#define ME_TOOL_OVAL        4
#define ME_TOOL_OVAL_FILL   5
#define ME_TOOL_SELECT      6
#define ME_TOOL_FILL        7
#define ME_TOOL_PLACE_BASE  8
#define ME_TOOL_PLACE_PILL  9
#define ME_TOOL_PLACE_START 10
#define ME_TOOL_MAZE        11
#define ME_TOOL_GENERATE    12
#define ME_TOOL_WAND        13
#define ME_TOOL_COUNT       14

/* Mine pattern modes for the mine tool */
#define ME_MINE_CHECKER     0   /* Checkerboard pattern */
#define ME_MINE_FULL        1   /* Mine every valid tile */
#define ME_MINE_RANDOM      2   /* Random sparse mines */
#define ME_MINE_CLEAR       3   /* Remove mines */
#define ME_MINE_PATTERN_COUNT 4

/* Brush shape modes for pencil/line tools */
#define ME_BRUSH_SQUARE     0
#define ME_BRUSH_CIRCLE     1
#define ME_BRUSH_SIZE_COUNT 5
static const int ME_BRUSH_SIZES[ME_BRUSH_SIZE_COUNT] = {1, 3, 5, 7, 9};

/* Transparent sentinel for clipboard tiles — skipped during paste.
 * 0xFE is unused in the terrain value space (0-9, mined 10-15, 0xFF=DEEP_SEA). */
#define ME_TRANSPARENT      0xFE

/* Terrain palette entry — passed from C to the ImGui UI */
typedef struct {
    const char *name;
    unsigned char rawValue;   /* terrain constant from global.h */
    unsigned char iconTile;   /* tile number for the atlas icon  */
    bool          canMine;    /* true if mine variant is valid   */
} METerrainEntry;

/* Render the floating terrain palette window.
 * tilesTex:       the tile atlas SDL_Texture* (cast to void* for C compat)
 * tileSize:       base tile size in the atlas (16)
 * activeTerrain:  index into the terrain entries array (0-10, 11=mine tool)
 * mineMode:       current mine toggle state
 * minePattern:    current mine pattern (ME_MINE_FULL/CHECKER/RANDOM/CLEAR)
 * Returns true if activeTerrain or mineMode changed. */
bool mapEditorImguiTerrainPalette(void *tilesTex, int tileSize,
                                  int *activeTerrain, bool *mineMode,
                                  int *minePattern,
                                  bool *p_open);

/* Render the floating tools toolbar window.
 * activeTool: pointer to current tool index (ME_TOOL_*)
 * wantText: set to true if user clicked the Text tool button (opens dialog)
 * Returns true if activeTool changed. */
bool mapEditorImguiToolbar(int *activeTool,
                           void *tilesTex, int tileSize,
                           int *brushSize, int *brushShape,
                           bool *p_open, bool *wantText);

/* Object selection kind — matches ObjKind in mapeditor_undo.h */
#define ME_SEL_NONE  (-1)
#define ME_SEL_BASE  0
#define ME_SEL_PILL  1
#define ME_SEL_START 2

/* Inspector panel state — pre-edit snapshot for undo batching */
typedef struct {
    bool editing;        /* true while a slider is active */
    unsigned char snapshot[64]; /* pre-edit object data (base/pillbox/start) */
} MEInspectorState;

/* Render the inspector panel for the currently selected object.
 * Returns true if a property was modified (caller should set dirty + undo). */
bool mapEditorImguiInspector(int selKind, int selIndex,
                              void *bases, void *pills, void *starts,
                              MEInspectorState *inspState,
                              void *undoStack,
                              bool *p_open);

/* Render the object list panel.
 * Sets *clickedKind and *clickedIndex if user clicked an entry.
 * Sets *panX and *panY to the tile to pan to (or -1 if no pan). */
void mapEditorImguiObjectList(void *bases, void *pills,
                               void *starts,
                               int selKind, int selIndex,
                               int *clickedKind, int *clickedIndex,
                               int *panX, int *panY,
                               bool *p_open);

/* Render the Overview minimap window.
 * minimapTex: 256x256 streaming texture (SDL_Texture* cast to void*)
 * viewCenterX/Y: camera center in world coords (tile << 8)
 * zoomLevel: current zoom level (e.g. 0.5, 1.0, 2.0)
 * screenW/H: main window dimensions
 * navX/navY: output tile coords if the user clicked to navigate (-1 if none) */
void mapEditorImguiOverview(void *minimapTex,
                             int viewCenterX, int viewCenterY,
                             float zoomLevel, int screenW, int screenH,
                             int *navX, int *navY,
                             bool *p_open);

/* Render the "Go To Coordinates" modal dialog.
 * Returns true if the user confirmed, with gotoX/gotoY set to tile coords. */
bool mapEditorImguiGotoDialog(int *gotoX, int *gotoY);

/* Render the validation results panel.
 * issues/count: array of validation issues from mapEditorValidate().
 * visible: pointer to show/hide flag.
 * clickedLocType/clickedIndex/clickedX/clickedY: output navigation target
 * when the user clicks an issue (-1 if none clicked). */
void mapEditorImguiValidationPanel(const void *issues, int count,
                                    int errorCount, int warningCount,
                                    bool *visible,
                                    int *clickedLocType, int *clickedIndex,
                                    int *clickedX, int *clickedY);

/* Render the "Validation Errors" modal that blocks saving.
 * Returns true when the user dismisses it. */
bool mapEditorImguiValidationErrorModal(int errorCount);

/* Render the Generate Map dialog.
 * open: pointer to the open flag (set to false when dialog closes).
 * cfg: pointer to the MapGenConfig to edit and use for generation.
 * hasSelection: true if there's an active selection.
 * selX1..selY2: selection bounds (only valid when hasSelection is true).
 * Returns true if the user clicked Generate. */
#include "mapeditor_generate.h"
#include "mapeditor_maze.h"
#include "mapeditor_text.h"
/* Render the reusable map-generation controls panel (generator type, seed,
 * type-specific sliders with lock buttons, Randomize/Copy Seed).
 * Call this inside any ImGui container (window, child, etc.).
 * Returns true if the config changed and the caller should regenerate. */
bool mapGenImguiControls(MapGenConfig *cfg);

bool mapEditorImguiGenerateDialog(bool *open, MapGenConfig *cfg,
                                  bool hasSelection,
                                  int selX1, int selY1, int selX2, int selY2,
                                  bool selectionScope);

/* Render the maze tool settings panel.
 * Shown when the maze tool is active.
 * isDragging: true if user is currently dragging.
 * selX1..selY2: current drag region bounds.
 * Returns true if settings changed (caller should regenerate preview). */
bool mapEditorImguiMazeSettings(MazeConfig *cfg, bool isDragging,
                                int selX1, int selY1, int selX2, int selY2,
                                void *tilesTex, int tileSize);

/* Render the generate tool settings panel.
 * Shown when the generate tool is active.
 * isDragging: true if user is currently dragging to define region.
 * selX1..selY2: current region bounds (updated during drag).
 * Returns true if settings changed (caller should regenerate preview). */
bool mapEditorImguiGenerateSettings(MapGenConfig *cfg, bool isDragging,
                                    int selX1, int selY1, int selX2, int selY2);

/* Render the Text tool dialog.
 * fontList/fontListReady: font enumeration state (lazily initialized).
 * fontSource/fontFamily/fontStyleIdx/ttfPixelSize: TTF font selection state.
 * Returns true if the user clicked Generate. */
#include "mapeditor_fonts.h"
bool mapEditorImguiTextDialog(bool *open, TextConfig *cfg,
                              void *tilesTex, int tileSize,
                              FontList *fontList, bool *fontListReady,
                              int *fontSource, int *fontFamily,
                              int *fontStyleIdx, int *ttfPixelSize);

/* Render the Statistics panel.
 * stats: computed map statistics.
 * p_open: pointer to show/hide flag.
 * wantRefresh: set to true if user clicked "Refresh Spatial". */
#include "mapeditor_stats.h"
void mapEditorImguiStatsPanel(const MapStats *stats, bool *p_open, bool *wantRefresh);

/* Render the Image Import dialog.
 * open: pointer to the open flag (set to false when dialog closes).
 * cfg: pointer to ImageImportConfig to edit.
 * renderer: SDL_Renderer for texture creation.
 * Returns true if the user clicked Import (terrain data in cfg->resultTerrain). */
#include "mapeditor_image.h"
bool mapEditorImguiImageImportDialog(bool *open, ImageImportConfig *cfg,
                                     void *renderer);

/* Render the Stamp Library panel.
 * lib: opaque pointer to StampLibrary struct.
 * renderer: SDL_Renderer for lazy texture creation.
 * hasClipboard: true if clipboard has content (enables "Save Clipboard...").
 * p_open: pointer to panel visibility flag.
 * wantSave: set to true when user clicks "Save Clipboard...".
 * saveName: buffer for the stamp name in save dialog.
 * saveNameLen: size of saveName buffer.
 * Returns index of stamp clicked to load (-1 if none). */
#include "mapeditor_stamp.h"
int mapEditorImguiStampLibrary(StampLibrary *lib, void *renderer,
                               bool hasClipboard, bool *p_open,
                               bool *wantSave, char *saveName, int saveNameLen);

/* Render the Save Stamp modal dialog.
 * Returns: 0 = still open, 1 = confirmed, 2 = cancelled. */
int mapEditorImguiSaveStampModal(char *name, int nameLen);

/* Render the Export as PNG dialog.
 * open: pointer to the open flag (set to false when dialog closes).
 * cfg: pointer to ExportConfig to edit.
 * exportPath: output buffer for chosen file path (ME_PATH_MAX).
 * Returns true if export was triggered (path written to exportPath). */
#include "mapeditor_export.h"
bool mapEditorImguiExportDialog(bool *open, ExportConfig *cfg,
                                char *exportPath, int exportPathLen,
                                SDL_Window *window);

/* Open an ImGui popup by name. C wrapper for ImGui::OpenPopup(). */
void ImGui_OpenPopup(const char *name);

/* Finalize and present ImGui draw data. */
void mapEditorImguiRender(void);

#ifdef __cplusplus
}
#endif

#endif /* MAPEDITOR_IMGUI_H */
