/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor.c
 * Purpose:
 *   Core map editor module. Loads a map (or creates a
 *   blank one), renders it with zoom/pan, and provides
 *   an ImGui menu bar and status bar.
 *********************************************************/

#include "mapeditor.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "mapeditor_imgui.h"
#include "mapgen.h"
#include "mapgen_maze.h"
#include "mapeditor_symmetry.h"
#include "mapeditor_text.h"
#include "mapeditor_fonts.h"
#include "mapeditor_image.h"
#include "mapeditor_stamp.h"
#include "mapeditor_undo.h"
#include "mapeditor_export.h"
#include "mapeditor_wbn_open.h"  /* meWbnOpen* — empty unless MAPEDITOR_WBN_OPEN (WinBolo client build) */
#include "macos_pinch.h"
#ifdef __APPLE__
#include "platform/mac_menubar.h"
#endif

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
#include <SDL3_ttf/SDL_ttf.h>
#endif
#include "mapeditor_validate.h"
#include "mapeditor_stats.h"
#include "mapeditor_scenario_check.h" /* MapEditorState holds a check by value */
#include "mapeditor_scenario_form.h" /* MapEditorState holds a form by value */
#include "mapeditor_scenario_pack.h" /* the container a map's scenario is read
                                      * from and written back to */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#include "global.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "screencalc.h"
#include "tilenum.h"
#include "../gui/tiles.h"
#include "../gui/lang.h"
#include "../gui/sdl3/minimap_render.h"
#include "../gui/sdl3/sprite_positions.h"

/* From tileloader.h */
extern SDL_Surface *tileLoaderBuildSheet(int tileSize);

#define ME_MIN_ZOOM 1
#define ME_MAX_ZOOM 16
#define ME_SCROLL_SPEED 4
/* Ceiling on how large the tile atlas is rasterized. At 4 the sheet is
   1984x704 (5.6MB) and one rebuild rasterizes 307 SVGs; past that the extra
   detail is beyond what the eye gets back out of a magnified tile. */
#define ME_ATLAS_MAX_SCALE 4
#define ME_MAX_RECENT_FILES 10
#define ME_FILL_LIMIT 65536

/* Sub-1x zoom support: stepped zoom table from 0.5x to 16x */
static const float zoomSteps[] = {
    0.5f, 0.6f, 0.7f, 0.8f, 0.9f,
    1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f,
    9.0f, 10.0f
};
#define ZOOM_STEP_COUNT (sizeof(zoomSteps) / sizeof(zoomSteps[0]))
#define ZOOM_STEP_1X 5  /* index of 1.0 in the table */

/* Maximum clipboard dimensions */
#define ME_CLIP_MAX 256

/* Pending file operation — set before an SDL file dialog opens,
 * processed when the async callback delivers a result. */
typedef enum {
    FILE_OP_NONE = 0,
    FILE_OP_OPEN,
    FILE_OP_SAVE_AS,
    FILE_OP_SAVE_THEN_NEW,     /* Save current, then create new */
    FILE_OP_SAVE_THEN_OPEN,    /* Save current, then open another */
    FILE_OP_SAVE_THEN_EXIT,    /* Save current, then exit */
    FILE_OP_SAVE_THEN_OPEN_WBN,/* Save current, then open the WinBolo.net chooser */
    FILE_OP_SAVE_SCENARIO_MOD  /* Write the scenario as a .scenario of its own */
} FileOp;

/* Map editor state */
typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *tilesTex;
    /* Tile size the atlas is rasterized at, as a multiple of TILE_SIZE_X.
       Atlas cell coordinates (mapViewPosX/Y and the mine/boat constants) are
       expressed at 1x, so every source rect scales by this — see meAtlasSrc. */
    int          sheetScale;

    /* Map data — heap-allocated, owned by the editor */
    map       mp;
    pillboxes pb;
    bases     bs;
    starts    ss;

    /* Camera */
    WORLD viewCenterX;
    WORLD viewCenterY;
    int   zoomFactor;

    /* Stepped zoom (supports sub-1x via offscreen render target) */
    int    zoomStepIndex;
    float  zoomLevel;
    SDL_Texture *offscreenTex;
    int    offscreenW, offscreenH;

    /* Mouse drag-to-pan (middle or right button) */
    bool  dragging;
    bool  rightDown;
    bool  rightDragged;
    float dragLastX;
    float dragLastY;

    /* Mouse position */
    float mouseX;
    float mouseY;

    /* Dirty flag — set on any map modification */
    bool  dirty;

    /* Current file path (empty string = untitled/new) */
    char  currentFilePath[ME_PATH_MAX];

    /* Options */
    bool  showGrid;
    bool  showMines;
    bool  showPillRanges;
    int   currentStartIndex;

    /* Async file dialog state */
    FileOp pendingFileOp;
    bool   fileDialogPending;
    bool   fileDialogGotResult;
    char   fileDialogResult[ME_PATH_MAX];

    /* Recent files */
    char   recentFiles[ME_MAX_RECENT_FILES][ME_PATH_MAX];
    int    numRecentFiles;

    /* Error modal message (empty = no error to show) */
    char   errorMessage[256];
    bool   errorModalOpen;

    /* Deferred action after unsaved-changes modal */
    FileOp deferredAction;

    /* Path to open after unsaved-changes modal (for recent files) */
    char   pendingOpenPath[ME_PATH_MAX];

    /* --- Phase 3: Terrain painting tools --- */

    /* Active terrain index (into meTerrainEntries[]) */
    int   activeTerrainIdx;
    bool  mineMode;
    int   minePattern;   /* ME_MINE_FULL/CHECKER/RANDOM/CLEAR */

    /* Active drawing tool */
    int   activeTool;   /* ME_TOOL_* */

    /* Pencil tool state */
    bool  isPainting;
    int   paintLastX;
    int   paintLastY;

    /* Shape tool state (line, rect, oval) */
    bool  isDrawing;
    int   drawStartX;
    int   drawStartY;
    int   drawCurX;
    int   drawCurY;

    /* Preview buffer for shape tools — tiles to highlight before commit */
    int   previewCount;
    int   previewX[ME_FILL_LIMIT];
    int   previewY[ME_FILL_LIMIT];

    /* Selection tool state */
    bool  hasSelection;
    bool  isSelecting;
    int   selX1, selY1, selX2, selY2;

    /* Wand selection mask — true for tiles within the BFS region */
    bool  hasSelMask;
    bool  selMask[256][256];

    /* Clipboard */
    bool  hasClipboard;
    int   clipW, clipH;
    BYTE  clipTerrain[ME_CLIP_MAX][ME_CLIP_MAX];
    /* Object arrays in clipboard (full structs with x/y as relative offsets) */
    int      clipNumPills;
    pillbox  clipPills[MAX_PILLS];
    int      clipNumBases;
    base     clipBases[MAX_BASES];
    int      clipNumStarts;
    start    clipStarts[MAX_STARTS];

    /* Paste mode */
    bool  isPasting;

    /* Undo/redo */
    UndoStack undoStack;

    /* --- Phase 5: Object placement and selection --- */

    /* Object selection */
    int   selectedObjKind;   /* ME_SEL_NONE / ME_SEL_BASE / ME_SEL_PILL / ME_SEL_START */
    int   selectedObjIndex;

    /* Object dragging */
    bool  isDraggingObj;
    int   dragObjOrigX;
    int   dragObjOrigY;

    /* Inspector undo batching state */
    MEInspectorState inspState;

    /* Status message (brief, clears after a few seconds) */
    char  statusMessage[128];
    Uint64 statusMessageTime;

    /* --- Phase 6: Minimap and navigation --- */

    /* Minimap */
    SDL_Texture *minimapTex;
    uint32_t minimapPixels[256][256];
    bool minimapDirty;

    /* Tab cycling through objects */
    int tabCycleIndex;

    /* Go To Coordinates dialog */
    bool openGotoDialog;

    /* --- Phase 7: Validation --- */
    ValidateResult lastValidation;
    bool showValidationPanel;
    bool validationErrorModalOpen;

    /* --- Phase 11: Map generation --- */
    bool showGenerateDialog;
    bool generateSelectionScope;
    MapGenConfig genConfig;

    /* --- Maze tool --- */
    int        mazePreviewCount;
    int        mazePreviewX[ME_FILL_LIMIT];
    int        mazePreviewY[ME_FILL_LIMIT];
    BYTE       mazePreviewTerrain[ME_FILL_LIMIT];
    MazeConfig mazeToolConfig;
    int        mazeDragOriginX;
    int        mazeDragOriginY;

    /* --- Generate tool drag preview --- */
    int        genDragOriginX;
    int        genDragOriginY;
    int        genPreviewCount;
    int        genPreviewX[ME_FILL_LIMIT];
    int        genPreviewY[ME_FILL_LIMIT];
    BYTE       genPreviewTerrain[ME_FILL_LIMIT];

    /* Panel visibility */
    bool showTerrain;
    bool showTools;
    bool showInspector;
    bool showObjects;
    bool showOverview;

    /* --- Statistics panel --- */
    MapStats mapStats;
    bool showStatsPanel;
    bool statsDirty;

    /* --- Scenario panel --- */
    MEScenarioState scn;
    /* The manifest the panel's metadata, lobby and rules forms edit. Pack into
     * Map is where it goes, so edits in it are unsaved work like any other and
     * count towards the unsaved-changes prompt. Emptied whenever the map
     * changes. */
    MEScenarioForm scnForm;
    /* What the validator last said about the script in the pane. Emptied
     * whenever the map changes or the script is re-read, so the markers never
     * outlive the text they were found in. */
    MEScenarioCheck scnCheck;
    /* The scenario the open map already carried, so a map save puts it back:
     * mapWrite truncates the file and would otherwise take it off the end. */
    MEScenarioPacked scnPacked;
    int  scnView;            /* one of MEScenarioView */
    bool showScenario;

    /* --- Text tool --- */
    bool showTextDialog;
    TextConfig textConfig;
    FontList fontList;
    bool     fontListReady;
    int      textFontSource;     /* 0 = built-in bitmap, 1 = TTF system font */
    int      textFontFamily;     /* Index into deduplicated family list */
    int      textFontStyleIdx;   /* Index into styles for selected family */
    int      textTTFPixelSize;   /* Font size in pixels (8-64) */

    /* --- Image import --- */
    bool showImageImportDialog;
    ImageImportConfig imageImportCfg;

    /* --- Stamp library --- */
    StampLibrary stampLib;
    bool showStampLibrary;
    bool showSaveStampDialog;
    char saveStampName[64];

    /* --- Export as PNG --- */
    bool showExportDialog;
    ExportConfig exportCfg;
    char exportPath[ME_PATH_MAX];

    /* --- Brush size --- */
    int   brushSize;      /* index into ME_BRUSH_SIZES (0-4) */
    int   brushShape;     /* ME_BRUSH_SQUARE or ME_BRUSH_CIRCLE */
    bool *brushSeen;      /* 256*256 dedup buffer for brush operations */

    bool  fromMainMenu;
    bool  quit;
    /* Why the loop is ending.  Leaving the editor and quitting the
     * application both set quit, and embedded they mean different things:
     * one hands the window back to WinBolo, the other ends it. */
    bool  exitIsAppQuit;
} MapEditorState;

/* Read back by mapEditorAppQuitRequested() once the run has returned — the
 * editor state is freed by then, so the answer outlives it here. */
static bool s_appQuitRequested = FALSE;

bool mapEditorAppQuitRequested(void) {
    return s_appQuitRequested;
}

/* Read a neighbour tile for adjacency calculation, treating bases as ROAD
 * and stripping mine variants. Same logic as mapViewNeighbour in mapview.c
 * but reads from raw map/bases structs instead of GameSim. */
static BYTE meNeighbour(MapEditorState *ed, BYTE nx, BYTE ny) {
    if (basesExistPos(&ed->bs, nx, ny) == TRUE) return ROAD;
    BYTE t = ed->mp->mapItem[nx][ny];
    if (t >= MINE_START && t <= MINE_END) return (BYTE)(t - MINE_SUBTRACT);
    return t;
}

/* Adjacency-aware tile calculation for a map position.
 * Same logic as mapViewCalcSquare in mapview.c but operates directly
 * on the editor's map/pillbox/base structs. */
static BYTE meCalcTile(MapEditorState *ed, BYTE xValue, BYTE yValue) {
    /* Pillbox check — show tile matching the pillbox's armour level */
    if (pillsExistPos(&ed->pb, xValue, yValue) == TRUE) {
        static const BYTE pillTileForArmour[16] = {
            PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
            PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
            PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
            PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
        };
        BYTE armour = pillsGetArmourPos(&ed->pb, xValue, yValue);
        if (armour <= 15) {
            return pillTileForArmour[armour];
        }
        return PILL_EVIL_15;
    }

    /* Base check */
    if (basesExistPos(&ed->bs, xValue, yValue) == TRUE) {
        return BASE_NEUTRAL;
    }

    /* Start check — starts are always on deep sea */
    if (startsExistPos(&ed->ss, xValue, yValue) == TRUE) {
        return DEEP_SEA_SOLID;
    }

    BYTE currentPos = ed->mp->mapItem[xValue][yValue];

    /* Strip mine variant for terrain lookup */
    if (currentPos >= MINE_START && currentPos <= MINE_END) {
        currentPos = (BYTE)(currentPos - MINE_SUBTRACT);
    }

    BYTE aboveLeft  = meNeighbour(ed, (BYTE)(xValue-1), (BYTE)(yValue-1));
    BYTE above      = meNeighbour(ed, xValue,            (BYTE)(yValue-1));
    BYTE aboveRight = meNeighbour(ed, (BYTE)(xValue+1), (BYTE)(yValue-1));
    BYTE leftPos    = meNeighbour(ed, (BYTE)(xValue-1), yValue);
    BYTE rightPos   = meNeighbour(ed, (BYTE)(xValue+1), yValue);
    BYTE belowLeft  = meNeighbour(ed, (BYTE)(xValue-1), (BYTE)(yValue+1));
    BYTE below      = meNeighbour(ed, xValue,            (BYTE)(yValue+1));
    BYTE belowRight = meNeighbour(ed, (BYTE)(xValue+1), (BYTE)(yValue+1));

    BYTE result;
    switch (currentPos) {
    case ROAD:
        result = screenCalcRoad(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
        break;
    case BUILDING:
        result = screenCalcBuilding(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
        break;
    case FOREST:
        result = screenCalcForest(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
        break;
    case RIVER:
        result = screenCalcRiver(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
        break;
    case DEEP_SEA:
        result = screenCalcDeepSea(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
        break;
    case BOAT:
        result = screenCalcBoat(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
        break;
    case CRATER:
        result = screenCalcCrater(aboveLeft, above, aboveRight, leftPos, rightPos, belowLeft, below, belowRight);
        break;
    default:
        result = currentPos;
        break;
    }
    return result;
}

/* -------------------------------------------------------
 * Terrain entries table — matches the ImGui palette order
 * ------------------------------------------------------- */
static const struct {
    const char *name;
    BYTE rawValue;
    BYTE iconTile;
    bool canMine;
} meTerrainEntries[] = {
    { "Deep Sea",      DEEP_SEA,      DEEP_SEA_SOLID,  false },
    { "Grass",         GRASS,         GRASS,           true  },
    { "Forest",        FOREST,        FOREST_SINGLE,   true  },
    { "Road",          ROAD,          ROAD_SOLID,      true  },
    { "Building",      BUILDING,      BUILD_SOLID,     false },
    { "Half Building", HALFBUILDING,  HALFBUILDING,    false },
    { "River",         RIVER,         RIVER_SOLID,     false },
    { "Swamp",         SWAMP,         SWAMP,           true  },
    { "Crater",        CRATER,        CRATER_SINGLE,   true  },
    { "Rubble",        RUBBLE,        RUBBLE,          true  },
    { "Boat",          BOAT,          BOAT_0,          false },
};
#define ME_NUM_TERRAINS 11
#define ME_TERRAIN_MINE 11  /* Special terrain index = mine tool */

/* Check if a base terrain type supports mines. */
static bool meCanMineTerrain(BYTE terrain) {
    if (terrain >= MINE_START && terrain <= MINE_END)
        terrain = (BYTE)(terrain - MINE_SUBTRACT);
    switch (terrain) {
    case GRASS: case FOREST: case ROAD: case SWAMP: case CRATER: case RUBBLE:
        return true;
    default:
        return false;
    }
}

/* Simple hash for deterministic "random" mine placement. */
static bool meRandomMineAt(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393u + y * 668265263u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (h % 3) == 0;  /* ~33% density */
}

/* Get the effective terrain value for painting at a specific tile.
 * When mine tool is active, modifies the existing terrain at (x,y). */
static BYTE meEffectiveTerrainAt(MapEditorState *ed, int x, int y) {
    if (ed->mineMode) {
        /* Mine tool: modify existing terrain at this position */
        BYTE existing = ed->mp->mapItem[x][y];
        BYTE base = existing;
        if (base >= MINE_START && base <= MINE_END)
            base = (BYTE)(base - MINE_SUBTRACT);

        if (ed->minePattern == ME_MINE_CLEAR) {
            /* Remove mine: return base terrain */
            return base;
        }

        if (!meCanMineTerrain(base)) return existing;  /* Can't mine this terrain */

        /* Check pattern */
        bool placeMine = true;
        if (ed->minePattern == ME_MINE_CHECKER) {
            placeMine = ((x + y) % 2 == 0);
        } else if (ed->minePattern == ME_MINE_RANDOM) {
            placeMine = meRandomMineAt(x, y);
        }

        if (placeMine) {
            return (BYTE)(base + MINE_SUBTRACT);
        }
        return existing;  /* Leave unchanged for non-mine pattern slots */
    }

    /* Normal terrain painting */
    BYTE raw = meTerrainEntries[ed->activeTerrainIdx].rawValue;
    return raw;
}

/* Legacy wrapper for code that doesn't have coordinates (preview rendering). */
static BYTE meEffectiveTerrain(MapEditorState *ed) {
    if (ed->mineMode) {
        /* For preview, just return grass+mine as representative */
        return (ed->minePattern == ME_MINE_CLEAR) ? GRASS : MINE_GRASS;
    }
    BYTE raw = meTerrainEntries[ed->activeTerrainIdx].rawValue;
    return raw;
}

/* Check if a tile coordinate is within the editable (non-border) area. */
static bool meIsEditable(int x, int y) {
    return x > MAP_MINE_EDGE_LEFT && x < MAP_MINE_EDGE_RIGHT &&
           y > MAP_MINE_EDGE_TOP  && y < MAP_MINE_EDGE_BOTTOM;
}

/* Check if terrain at (tx,ty) is traversable land (for base/pill placement). */
static bool meIsTraversableLand(MapEditorState *ed, int tx, int ty) {
    BYTE terrain = ed->mp->mapItem[tx][ty];
    if (terrain >= MINE_START && terrain <= MINE_END)
        terrain = (BYTE)(terrain - MINE_SUBTRACT);
    switch (terrain) {
    case GRASS: case ROAD: case SWAMP: case CRATER:
    case RUBBLE: case FOREST:
        return true;
    default:
        return false;
    }
}

/* Check if a start already exists at (tx, ty). */
static bool meStartExistsAt(MapEditorState *ed, int tx, int ty) {
    for (int i = 0; i < ed->ss->numStarts; i++) {
        if (ed->ss->item[i].x == tx && ed->ss->item[i].y == ty)
            return true;
    }
    return false;
}

/* Set a status bar message (auto-clears after a few seconds). */
static void meSetStatus(MapEditorState *ed, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ed->statusMessage, sizeof(ed->statusMessage), fmt, ap);
    va_end(ap);
    ed->statusMessageTime = SDL_GetTicks();
}

/* -------------------------------------------------------
 * Minimap rebuild — 256x256 pixel overview of the map
 * ------------------------------------------------------- */
static void mapEditorRebuildMinimap(MapEditorState *ed) {
    /* Use shared renderer for terrain + border/mine darkening.
     * ed->minimapPixels is uint32_t[256][256] in RGBA packed format,
     * which is the same memory layout as uint8_t[256*256*4] row-major. */
    static const uint8_t pillCol[3]  = {255,  50,  50};
    static const uint8_t baseCol[3]  = { 50,  50, 255};
    static const uint8_t startCol[3] = { 50, 255,  50};

    {
        MapPreview *terrainView = clientMapPreviewWrap(ed->mp, NULL, NULL, NULL);
        /* MINIMAP_EDIT_PALETTE: this panel is a diagram of the map being
           built, where every terrain wants its own colour, rather than a
           small picture of one being played. */
        minimapRenderPixels(terrainView,
                            (uint8_t *)ed->minimapPixels, NULL,
                            MINIMAP_DARKEN_BORDER | MINIMAP_DARKEN_MINES |
                            MINIMAP_EDIT_PALETTE);
        clientMapPreviewDestroy(terrainView);
    }

    /* Draw unselected objects with editor-specific colours */
    {
        MapPreview *objectsView = clientMapPreviewWrap(NULL, ed->pb, ed->bs, ed->ss);
        minimapDrawObjects((uint8_t *)ed->minimapPixels,
                           objectsView,
                           pillCol, baseCol, startCol);
        clientMapPreviewDestroy(objectsView);
    }

    /* Overlay selected object in yellow */
    #define ME_MINIMAP_DOT(px, py, cr, cg, cb) do { \
        for (int dy = -1; dy <= 1; dy++) { \
            for (int dx = -1; dx <= 1; dx++) { \
                int mx = (px) + dx, my = (py) + dy; \
                if (mx >= 0 && mx < 256 && my >= 0 && my < 256) { \
                    int idx = (my * 256 + mx) * 4; \
                    ((uint8_t *)ed->minimapPixels)[idx]   = (cr); \
                    ((uint8_t *)ed->minimapPixels)[idx+1] = (cg); \
                    ((uint8_t *)ed->minimapPixels)[idx+2] = (cb); \
                    ((uint8_t *)ed->minimapPixels)[idx+3] = 255;  \
                } \
            } \
        } \
    } while(0)

    if (ed->selectedObjKind == ME_SEL_BASE && ed->selectedObjIndex >= 0 &&
        ed->selectedObjIndex < ed->bs->numBases) {
        ME_MINIMAP_DOT(ed->bs->item[ed->selectedObjIndex].x,
                       ed->bs->item[ed->selectedObjIndex].y, 255, 255, 0);
    }
    if (ed->selectedObjKind == ME_SEL_PILL && ed->selectedObjIndex >= 0 &&
        ed->selectedObjIndex < ed->pb->numPills) {
        ME_MINIMAP_DOT(ed->pb->item[ed->selectedObjIndex].x,
                       ed->pb->item[ed->selectedObjIndex].y, 255, 255, 0);
    }
    if (ed->selectedObjKind == ME_SEL_START && ed->selectedObjIndex >= 0 &&
        ed->selectedObjIndex < ed->ss->numStarts) {
        ME_MINIMAP_DOT(ed->ss->item[ed->selectedObjIndex].x,
                       ed->ss->item[ed->selectedObjIndex].y, 255, 255, 0);
    }
    #undef ME_MINIMAP_DOT

    SDL_UpdateTexture(ed->minimapTex, NULL, ed->minimapPixels, 256 * sizeof(uint32_t));
}

/* Validate placement of a base at (tx,ty). Returns NULL if OK, else error string. */
static const char *meValidateBasePlacement(MapEditorState *ed, int tx, int ty) {
    if (!meIsEditable(tx, ty)) return "outside playable area";
    if (!meIsTraversableLand(ed, tx, ty)) return "not traversable land";
    if (basesExistPos(&ed->bs, (BYTE)tx, (BYTE)ty)) return "base already here";
    if (pillsExistPos(&ed->pb, (BYTE)tx, (BYTE)ty)) return "pillbox already here";
    if (ed->bs->numBases >= MAX_BASES) return "max 16 bases reached";
    return NULL;
}

/* Validate placement of a pillbox at (tx,ty). Returns NULL if OK, else error string. */
static const char *meValidatePillPlacement(MapEditorState *ed, int tx, int ty) {
    if (!meIsEditable(tx, ty)) return "outside playable area";
    if (!meIsTraversableLand(ed, tx, ty)) return "not traversable land";
    if (basesExistPos(&ed->bs, (BYTE)tx, (BYTE)ty)) return "base already here";
    if (pillsExistPos(&ed->pb, (BYTE)tx, (BYTE)ty)) return "pillbox already here";
    if (ed->pb->numPills >= MAX_PILLS) return "max 16 pillboxes reached";
    return NULL;
}

/* Validate placement of a start at (tx,ty). Returns NULL if OK, else error string. */
static const char *meValidateStartPlacement(MapEditorState *ed, int tx, int ty) {
    if (!meIsEditable(tx, ty)) return "outside playable area";
    BYTE terrain = ed->mp->mapItem[tx][ty];
    if (terrain != DEEP_SEA) return "must be deep sea";
    if (meStartExistsAt(ed, tx, ty)) return "start already here";
    if (ed->ss->numStarts >= MAX_STARTS) return "max 16 starts reached";
    return NULL;
}

/* Check if a tile coordinate is within the 256x256 map. */
static bool meIsInMap(int x, int y) {
    return x >= 0 && x <= 255 && y >= 0 && y <= 255;
}

/* Set a single map tile if it's in the editable area, with undo recording.
 * Returns true if modified. */
static bool meSetTile(MapEditorState *ed, int x, int y, BYTE terrain) {
    if (!meIsEditable(x, y)) return false;
    undoRecordTile(&ed->undoStack, ed->mp, (BYTE)x, (BYTE)y, terrain);
    ed->mp->mapItem[x][y] = terrain;
    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    return true;
}

/* -------------------------------------------------------
 * Bresenham's line algorithm — calls callback for each tile
 * ------------------------------------------------------- */
static void meBresenhamLine(int x0, int y0, int x1, int y1,
                            void (*callback)(int x, int y, void *ud), void *ud) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        callback(x0, y0, ud);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Paint a brush-sized stamp centered on (cx, cy). */
static void meBrushStamp(MapEditorState *ed, int cx, int cy) {
    int size = ME_BRUSH_SIZES[ed->brushSize];
    if (size == 1) {
        meSetTile(ed, cx, cy, meEffectiveTerrainAt(ed, cx, cy));
        return;
    }
    int r = size / 2;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (ed->brushShape == ME_BRUSH_CIRCLE) {
                if (size <= 3) {
                    /* Manhattan distance for small sizes (diamond) */
                    if (abs(dx) + abs(dy) > r) continue;
                } else {
                    /* Euclidean distance for larger sizes (circle) */
                    if (dx * dx + dy * dy > r * r) continue;
                }
            }
            meSetTile(ed, cx + dx, cy + dy, meEffectiveTerrainAt(ed, cx + dx, cy + dy));
        }
    }
}

/* Callback for Bresenham line: stamp brush at each point (with dedup) */
static void meBrushPaintCallback(int x, int y, void *ud) {
    MapEditorState *ed = (MapEditorState *)ud;
    int size = ME_BRUSH_SIZES[ed->brushSize];
    int r = size / 2;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int tx = x + dx, ty = y + dy;
            if (tx < 0 || tx > 255 || ty < 0 || ty > 255) continue;
            if (ed->brushShape == ME_BRUSH_CIRCLE) {
                if (size <= 3) {
                    if (abs(dx) + abs(dy) > r) continue;
                } else {
                    if (dx * dx + dy * dy > r * r) continue;
                }
            }
            if (ed->brushSeen[ty * 256 + tx]) continue;
            ed->brushSeen[ty * 256 + tx] = true;
            meSetTile(ed, tx, ty, meEffectiveTerrainAt(ed, tx, ty));
        }
    }
}

/* Callback: add tile to preview buffer */
static void mePreviewTileCallback(int x, int y, void *ud) {
    MapEditorState *ed = (MapEditorState *)ud;
    if (ed->previewCount < ME_FILL_LIMIT && meIsInMap(x, y)) {
        ed->previewX[ed->previewCount] = x;
        ed->previewY[ed->previewCount] = y;
        ed->previewCount++;
    }
}

/* Callback: expand each Bresenham point into the brush footprint for preview (with dedup) */
static void meBrushPreviewCallback(int x, int y, void *ud) {
    MapEditorState *ed = (MapEditorState *)ud;
    int size = ME_BRUSH_SIZES[ed->brushSize];
    int r = size / 2;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int tx = x + dx, ty = y + dy;
            if (tx < 0 || tx > 255 || ty < 0 || ty > 255) continue;
            if (ed->brushShape == ME_BRUSH_CIRCLE) {
                if (size <= 3) {
                    if (abs(dx) + abs(dy) > r) continue;
                } else {
                    if (dx * dx + dy * dy > r * r) continue;
                }
            }
            if (ed->brushSeen[ty * 256 + tx]) continue;
            ed->brushSeen[ty * 256 + tx] = true;
            mePreviewTileCallback(tx, ty, ed);
        }
    }
}

/* -------------------------------------------------------
 * Midpoint ellipse algorithm — calls callback for each tile
 * ------------------------------------------------------- */
static void meEllipseOutline(int cx, int cy, int rx, int ry,
                             void (*callback)(int, int, void*), void *ud) {
    if (rx == 0 && ry == 0) { callback(cx, cy, ud); return; }
    if (rx == 0) {
        for (int y = cy - ry; y <= cy + ry; y++) callback(cx, y, ud);
        return;
    }
    if (ry == 0) {
        for (int x = cx - rx; x <= cx + rx; x++) callback(x, cy, ud);
        return;
    }

    long rx2 = (long)rx * rx;
    long ry2 = (long)ry * ry;
    long x = 0, y = ry;
    long px = 0, py = 2 * rx2 * y;

    /* Region 1 */
    long p = (long)(ry2 - rx2 * ry + 0.25 * rx2);
    while (px < py) {
        callback((int)(cx + x), (int)(cy + y), ud);
        callback((int)(cx - x), (int)(cy + y), ud);
        callback((int)(cx + x), (int)(cy - y), ud);
        callback((int)(cx - x), (int)(cy - y), ud);
        x++;
        px += 2 * ry2;
        if (p < 0) {
            p += ry2 + px;
        } else {
            y--;
            py -= 2 * rx2;
            p += ry2 + px - py;
        }
    }

    /* Region 2 */
    p = (long)(ry2 * (x + 0.5) * (x + 0.5) + rx2 * (y - 1) * (y - 1) - rx2 * ry2);
    while (y >= 0) {
        callback((int)(cx + x), (int)(cy + y), ud);
        callback((int)(cx - x), (int)(cy + y), ud);
        callback((int)(cx + x), (int)(cy - y), ud);
        callback((int)(cx - x), (int)(cy - y), ud);
        y--;
        py -= 2 * rx2;
        if (p > 0) {
            p += rx2 - py;
        } else {
            x++;
            px += 2 * ry2;
            p += rx2 - py + px;
        }
    }
}

static void meEllipseFilled(int cx, int cy, int rx, int ry,
                            void (*callback)(int, int, void*), void *ud) {
    if (rx == 0 && ry == 0) { callback(cx, cy, ud); return; }
    for (int y = -ry; y <= ry; y++) {
        /* For each scanline, compute the x extent */
        /* x^2/rx^2 + y^2/ry^2 <= 1 => x <= rx * sqrt(1 - y^2/ry^2) */
        double yf = (double)y / (double)(ry > 0 ? ry : 1);
        double xf = (double)rx * sqrt(1.0 - yf * yf);
        int xext = (int)(xf + 0.5);
        for (int x = -xext; x <= xext; x++) {
            callback(cx + x, cy + y, ud);
        }
    }
}

/* -------------------------------------------------------
 * Flood fill (BFS)
 * ------------------------------------------------------- */
static void meFloodFill(MapEditorState *ed, int startX, int startY, BYTE fillTerrain) {
    if (!meIsEditable(startX, startY)) return;

    BYTE targetRaw = ed->mp->mapItem[startX][startY];
    /* Strip mine from target for comparison */
    BYTE targetTerrain = targetRaw;
    if (targetTerrain >= MINE_START && targetTerrain <= MINE_END)
        targetTerrain = (BYTE)(targetTerrain - MINE_SUBTRACT);

    /* Strip mine from fill terrain for comparison */
    BYTE fillBase = fillTerrain;
    if (fillBase >= MINE_START && fillBase <= MINE_END)
        fillBase = (BYTE)(fillBase - MINE_SUBTRACT);

    /* Compare raw values — if filling same terrain type (including mine state), nothing to do */
    if (targetRaw == fillTerrain) return;

    /* BFS using a simple queue */
    typedef struct { int x, y; } Coord;
    Coord *queue = (Coord *)malloc(ME_FILL_LIMIT * sizeof(Coord));
    if (!queue) return;

    /* Visited bitmap */
    bool *visited = (bool *)calloc(256 * 256, sizeof(bool));
    if (!visited) { free(queue); return; }

    int head = 0, tail = 0;
    queue[tail].x = startX;
    queue[tail].y = startY;
    tail++;
    visited[startY * 256 + startX] = true;

    int count = 0;
    while (head < tail && count < ME_FILL_LIMIT) {
        Coord c = queue[head++];
        BYTE raw = ed->mp->mapItem[c.x][c.y];
        BYTE base = raw;
        if (base >= MINE_START && base <= MINE_END)
            base = (BYTE)(base - MINE_SUBTRACT);

        if (base != targetTerrain) continue;

        meSetTile(ed, c.x, c.y, fillTerrain);
        count++;

        /* 4-connected neighbours */
        static const int dx[] = {0, 0, -1, 1};
        static const int dy[] = {-1, 1, 0, 0};
        for (int d = 0; d < 4; d++) {
            int nx = c.x + dx[d];
            int ny = c.y + dy[d];
            if (!meIsEditable(nx, ny)) continue;
            if (visited[ny * 256 + nx]) continue;
            visited[ny * 256 + nx] = true;
            if (tail < ME_FILL_LIMIT) {
                queue[tail].x = nx;
                queue[tail].y = ny;
                tail++;
            }
        }
    }

    free(visited);
    free(queue);
}

/* Mine-mode flood fill: spread across same-terrain tiles, apply/remove mines
 * according to the active mine pattern. */
static void meMineFloodFill(MapEditorState *ed, int startX, int startY) {
    if (!meIsEditable(startX, startY)) return;

    BYTE startRaw = ed->mp->mapItem[startX][startY];
    BYTE startBase = startRaw;
    if (startBase >= MINE_START && startBase <= MINE_END)
        startBase = (BYTE)(startBase - MINE_SUBTRACT);

    if (!meCanMineTerrain(startBase) && ed->minePattern != ME_MINE_CLEAR)
        return;

    typedef struct { int x, y; } Coord;
    Coord *queue = (Coord *)malloc(ME_FILL_LIMIT * sizeof(Coord));
    if (!queue) return;
    bool *visited = (bool *)calloc(256 * 256, sizeof(bool));
    if (!visited) { free(queue); return; }

    int head = 0, tail = 0;
    queue[tail].x = startX;
    queue[tail].y = startY;
    tail++;
    visited[startY * 256 + startX] = true;

    int count = 0;
    while (head < tail && count < ME_FILL_LIMIT) {
        Coord c = queue[head++];
        BYTE raw = ed->mp->mapItem[c.x][c.y];
        BYTE base = raw;
        if (base >= MINE_START && base <= MINE_END)
            base = (BYTE)(base - MINE_SUBTRACT);
        if (base != startBase) continue;

        BYTE newVal = meEffectiveTerrainAt(ed, c.x, c.y);
        if (newVal != raw) {
            meSetTile(ed, c.x, c.y, newVal);
        }
        count++;

        static const int dx[] = {0, 0, -1, 1};
        static const int dy[] = {-1, 1, 0, 0};
        for (int d = 0; d < 4; d++) {
            int nx = c.x + dx[d];
            int ny = c.y + dy[d];
            if (!meIsEditable(nx, ny)) continue;
            if (visited[ny * 256 + nx]) continue;
            visited[ny * 256 + nx] = true;
            if (tail < ME_FILL_LIMIT) {
                queue[tail].x = nx;
                queue[tail].y = ny;
                tail++;
            }
        }
    }

    free(visited);
    free(queue);
}

/* -------------------------------------------------------
 * Magic wand selection (BFS — same terrain contiguous region)
 * ------------------------------------------------------- */
static void meWandSelect(MapEditorState *ed, int startX, int startY) {
    if (!meIsEditable(startX, startY)) return;

    BYTE targetRaw = ed->mp->mapItem[startX][startY];
    BYTE targetTerrain = targetRaw;
    if (targetTerrain >= MINE_START && targetTerrain <= MINE_END)
        targetTerrain = (BYTE)(targetTerrain - MINE_SUBTRACT);

    typedef struct { int x, y; } Coord;
    Coord *queue = (Coord *)malloc(ME_FILL_LIMIT * sizeof(Coord));
    if (!queue) return;

    bool *visited = (bool *)calloc(256 * 256, sizeof(bool));
    if (!visited) { free(queue); return; }

    memset(ed->selMask, 0, sizeof(ed->selMask));

    int head = 0, tail = 0;
    queue[tail].x = startX;
    queue[tail].y = startY;
    tail++;
    visited[startY * 256 + startX] = true;

    int minX = startX, maxX = startX;
    int minY = startY, maxY = startY;
    int count = 0;

    while (head < tail && count < ME_FILL_LIMIT) {
        Coord c = queue[head++];
        BYTE raw = ed->mp->mapItem[c.x][c.y];
        BYTE base = raw;
        if (base >= MINE_START && base <= MINE_END)
            base = (BYTE)(base - MINE_SUBTRACT);
        if (base != targetTerrain) continue;

        if (c.x < minX) minX = c.x;
        if (c.x > maxX) maxX = c.x;
        if (c.y < minY) minY = c.y;
        if (c.y > maxY) maxY = c.y;
        count++;

        static const int dx[] = {0, 0, -1, 1};
        static const int dy[] = {-1, 1, 0, 0};
        for (int d = 0; d < 4; d++) {
            int nx = c.x + dx[d];
            int ny = c.y + dy[d];
            if (!meIsEditable(nx, ny)) continue;
            if (visited[ny * 256 + nx]) continue;
            visited[ny * 256 + nx] = true;
            if (tail < ME_FILL_LIMIT) {
                queue[tail].x = nx;
                queue[tail].y = ny;
                tail++;
            }
        }
    }

    if (count > 0) {
        /* Clear mask within bounding box, then mark matched tiles */
        for (int x = minX; x <= maxX; x++)
            for (int y = minY; y <= maxY; y++)
                ed->selMask[x][y] = false;

        /* Re-scan visited array for matched tiles */
        for (int y = minY; y <= maxY; y++) {
            for (int x = minX; x <= maxX; x++) {
                if (!visited[y * 256 + x]) continue;
                BYTE raw = ed->mp->mapItem[x][y];
                BYTE base = raw;
                if (base >= MINE_START && base <= MINE_END)
                    base = (BYTE)(base - MINE_SUBTRACT);
                if (base == targetTerrain)
                    ed->selMask[x][y] = true;
            }
        }

        ed->hasSelection = true;
        ed->hasSelMask = true;
        ed->selX1 = minX;
        ed->selY1 = minY;
        ed->selX2 = maxX;
        ed->selY2 = maxY;
    }

    free(visited);
    free(queue);
}

/* -------------------------------------------------------
 * Build preview tiles for shape tools
 * ------------------------------------------------------- */
static void meBuildLinePreview(MapEditorState *ed) {
    ed->previewCount = 0;
    if (ed->brushSize > 0) {
        memset(ed->brushSeen, 0, 256 * 256 * sizeof(bool));
        meBresenhamLine(ed->drawStartX, ed->drawStartY, ed->drawCurX, ed->drawCurY,
                        meBrushPreviewCallback, ed);
    } else {
        meBresenhamLine(ed->drawStartX, ed->drawStartY, ed->drawCurX, ed->drawCurY,
                        mePreviewTileCallback, ed);
    }
}

static void meBuildRectPreview(MapEditorState *ed) {
    ed->previewCount = 0;
    int x0 = ed->drawStartX < ed->drawCurX ? ed->drawStartX : ed->drawCurX;
    int y0 = ed->drawStartY < ed->drawCurY ? ed->drawStartY : ed->drawCurY;
    int x1 = ed->drawStartX > ed->drawCurX ? ed->drawStartX : ed->drawCurX;
    int y1 = ed->drawStartY > ed->drawCurY ? ed->drawStartY : ed->drawCurY;

    if (ed->activeTool == ME_TOOL_RECT_FILL) {
        for (int y = y0; y <= y1 && ed->previewCount < ME_FILL_LIMIT; y++) {
            for (int x = x0; x <= x1 && ed->previewCount < ME_FILL_LIMIT; x++) {
                mePreviewTileCallback(x, y, ed);
            }
        }
    } else {
        /* Perimeter only */
        for (int x = x0; x <= x1; x++) {
            mePreviewTileCallback(x, y0, ed);
            mePreviewTileCallback(x, y1, ed);
        }
        for (int y = y0 + 1; y < y1; y++) {
            mePreviewTileCallback(x0, y, ed);
            mePreviewTileCallback(x1, y, ed);
        }
    }
}

static void meBuildOvalPreview(MapEditorState *ed) {
    ed->previewCount = 0;
    int rx = abs(ed->drawCurX - ed->drawStartX);
    int ry = abs(ed->drawCurY - ed->drawStartY);
    if (ed->activeTool == ME_TOOL_OVAL_FILL) {
        meEllipseFilled(ed->drawStartX, ed->drawStartY, rx, ry,
                        mePreviewTileCallback, ed);
    } else {
        meEllipseOutline(ed->drawStartX, ed->drawStartY, rx, ry,
                         mePreviewTileCallback, ed);
    }
}

/* Commit preview tiles to the map (undo-aware) */
static void meCommitPreview(MapEditorState *ed) {
    undoBeginCommand(&ed->undoStack);
    for (int i = 0; i < ed->previewCount; i++) {
        int px = ed->previewX[i];
        int py = ed->previewY[i];
        meSetTile(ed, px, py, meEffectiveTerrainAt(ed, px, py));
    }
    undoEndCommand(&ed->undoStack);
    ed->previewCount = 0;
    ed->isDrawing = false;
}

/* -------------------------------------------------------
 * Tile atlas scaling
 * ------------------------------------------------------- */

/* Integer magnification the canvas is drawn at, in renderer pixels.
   zoomLevel is what the user picked; the window's pixel density folds in on
   top so a tile keeps the same physical size on a HiDPI display and simply
   gets more pixels to say it with. Sub-1x levels magnify by 1 (times density)
   and reach their size through the offscreen downscale instead. */
static int meRenderZoom(const MapEditorState *ed, float pxScale) {
    int base = (ed->zoomLevel >= 1.0f) ? (int)ed->zoomLevel : 1;
    int density = (int)(pxScale + 0.5f);
    if (density < 1) density = 1;
    return base * density;
}

/* On-screen size of one map tile, in renderer pixels.

   At zoom >= 1 that is the render zoom outright. Below 1x the canvas is drawn
   at the render zoom into the offscreen and then blitted down by zoomLevel, so
   both factors apply. Anything turning a screen distance into a map distance
   divides by this: the drag-pan and the overview's viewport rectangle. Both
   used 16 * zoomLevel, which was right while the canvas was measured in points
   and is short by the pixel density now that it is measured in pixels. */
static float meScreenTilePixels(const MapEditorState *ed) {
    float sub = (ed->zoomLevel < 1.0f) ? ed->zoomLevel : 1.0f;
    return (float)TILE_SIZE_X * (float)ed->zoomFactor * sub;
}

/* Atlas scale to rasterize at for a given render zoom. Capped, and reduced to
   a divisor of the render zoom: whatever the atlas does not supply is made up
   by magnifying it, and only a whole-number ratio magnifies evenly. A render
   zoom of 6 takes a 3x atlas doubled, not a 4x atlas at 1.5:1. */
static int meAtlasScaleFor(int renderZoom) {
    int s = (renderZoom > ME_ATLAS_MAX_SCALE) ? ME_ATLAS_MAX_SCALE : renderZoom;
    while (s > 1 && (renderZoom % s) != 0) s--;
    return (s < 1) ? 1 : s;
}

/* Re-rasterize the tile atlas when the size a tile is drawn at changes. The
   sprites are SVG, so rasterizing at the drawn size keeps detail that
   magnifying a 16px sheet cannot recover — what sdl3LoadTiles already does for
   the game with gZoomFactor. Costs one pass over 307 sprites, so it runs only
   when the scale actually moves, and keeps the existing atlas on failure. */
static void meEnsureAtlas(MapEditorState *ed, int renderZoom) {
    int want = meAtlasScaleFor(renderZoom);
    if (want == ed->sheetScale && ed->tilesTex) return;

    SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X * want);
    if (!sheet) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(ed->renderer, sheet);
    SDL_DestroySurface(sheet);
    if (!tex) return;
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    if (ed->tilesTex) SDL_DestroyTexture(ed->tilesTex);
    ed->tilesTex = tex;
    ed->sheetScale = want;
}

/* Source rect for one atlas cell, given its 1x coordinates. */
static SDL_FRect meAtlasSrc(const MapEditorState *ed, int x1x, int y1x) {
    float s = (float)ed->sheetScale;
    SDL_FRect r = { (float)x1x * s, (float)y1x * s,
                    (float)TILE_SIZE_X * s, (float)TILE_SIZE_Y * s };
    return r;
}

/* -------------------------------------------------------
 * Render preview tiles (semi-transparent)
 * ------------------------------------------------------- */
static void meRenderPreview(MapEditorState *ed, int screenW, int screenH) {
    if (ed->previewCount == 0) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    if (ed->mineMode) {
        /* Mine tool preview: show mine sprite on valid tiles, or X for clear */
        SDL_SetTextureAlphaMod(ed->tilesTex, 128);
        for (int i = 0; i < ed->previewCount; i++) {
            int mx = ed->previewX[i];
            int my = ed->previewY[i];
            if (!meIsInMap(mx, my)) continue;
            BYTE existing = ed->mp->mapItem[mx][my];
            BYTE base = existing;
            if (base >= MINE_START && base <= MINE_END)
                base = (BYTE)(base - MINE_SUBTRACT);

            float dx = (float)(mx * tileSize - camPX) * zf;
            float dy = (float)(my * tileSize - camPY) * zf;
            if (dx + scaledTile < 0 || dx > screenW ||
                dy + scaledTile < 0 || dy > screenH) continue;

            SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };

            if (ed->minePattern == ME_MINE_CLEAR) {
                /* Show red tint for mine removal */
                if (existing >= MINE_START && existing <= MINE_END) {
                    SDL_SetTextureAlphaMod(ed->tilesTex, 255);
                    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);
                    SDL_SetRenderDrawColor(ed->renderer, 255, 80, 80, 100);
                    SDL_RenderFillRect(ed->renderer, &dest);
                    SDL_SetTextureAlphaMod(ed->tilesTex, 128);
                }
            } else if (meCanMineTerrain(base)) {
                /* Check if mine would be placed at this position */
                bool placeMine = true;
                if (ed->minePattern == ME_MINE_CHECKER)
                    placeMine = ((mx + my) % 2 == 0);
                else if (ed->minePattern == ME_MINE_RANDOM)
                    placeMine = meRandomMineAt(mx, my);

                if (placeMine) {
                    SDL_FRect mineSrc = meAtlasSrc(ed, MINE_X, MINE_Y);
                    SDL_RenderTexture(ed->renderer, ed->tilesTex, &mineSrc, &dest);
                }
            }
        }
        SDL_SetTextureAlphaMod(ed->tilesTex, 255);
    } else {
        /* Normal terrain preview */
        BYTE iconTile = meTerrainEntries[ed->activeTerrainIdx].iconTile;

        SDL_SetTextureAlphaMod(ed->tilesTex, 128);

        for (int i = 0; i < ed->previewCount; i++) {
            int mx = ed->previewX[i];
            int my = ed->previewY[i];
            float dx = (float)(mx * tileSize - camPX) * zf;
            float dy = (float)(my * tileSize - camPY) * zf;

            if (dx + scaledTile < 0 || dx > screenW ||
                dy + scaledTile < 0 || dy > screenH) continue;

            SDL_FRect src = meAtlasSrc(ed, mapViewPosX[iconTile],
                                       mapViewPosY[iconTile]);
            SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
            SDL_RenderTexture(ed->renderer, ed->tilesTex, &src, &dest);
        }

        SDL_SetTextureAlphaMod(ed->tilesTex, 255);
    }
}

/* -------------------------------------------------------
 * Render maze preview tiles (semi-transparent, per-tile terrain)
 * ------------------------------------------------------- */
static BYTE meTerrainToIconTile(BYTE terrain);

static void meRenderMazePreview(MapEditorState *ed, int screenW, int screenH) {
    if (ed->mazePreviewCount == 0) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    SDL_SetTextureAlphaMod(ed->tilesTex, 128);

    for (int i = 0; i < ed->mazePreviewCount; i++) {
        int mx = ed->mazePreviewX[i];
        int my = ed->mazePreviewY[i];
        float dx = (float)(mx * tileSize - camPX) * zf;
        float dy = (float)(my * tileSize - camPY) * zf;

        if (dx + scaledTile < 0 || dx > screenW ||
            dy + scaledTile < 0 || dy > screenH) continue;

        BYTE iconTile = meTerrainToIconTile(ed->mazePreviewTerrain[i]);

        SDL_FRect src = meAtlasSrc(ed, mapViewPosX[iconTile],
                                   mapViewPosY[iconTile]);
        SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
        SDL_RenderTexture(ed->renderer, ed->tilesTex, &src, &dest);
    }

    SDL_SetTextureAlphaMod(ed->tilesTex, 255);
}

/* -------------------------------------------------------
 * Map terrain type to a solid icon tile for preview rendering
 * ------------------------------------------------------- */
static BYTE meTerrainToIconTile(BYTE terrain) {
    switch (terrain) {
    case BUILDING:     return BUILD_SOLID;
    case HALFBUILDING: return 8;  /* HALFBUILDING tile */
    case ROAD:         return ROAD_SOLID;
    case RIVER:        return RIVER_SOLID;
    case GRASS:        return 7;  /* GRASS tile */
    case FOREST:       return 164; /* FOREST_SINGLE */
    case SWAMP:        return 2;  /* SWAMP tile */
    case CRATER:       return 3;  /* CRATER tile */
    case RUBBLE:       return 6;  /* RUBBLE tile */
    case BOAT:         return 138; /* BOAT tile */
    case DEEP_SEA:     return DEEP_SEA_SOLID;
    default:
        if (terrain >= MINE_START && terrain <= MINE_GRASS) {
            /* Mined terrain — show base terrain */
            return meTerrainToIconTile(terrain - MINE_START + SWAMP);
        }
        return ROAD_SOLID;
    }
}

/* -------------------------------------------------------
 * Generate preview tiles for the generate tool.
 * Runs the generator on a temporary map and extracts tiles.
 * ------------------------------------------------------- */
static void meBuildGenPreview(MapEditorState *ed, int x0, int y0, int x1, int y1) {
    /* Clamp to valid map edges */
    if (x0 <= MAP_MINE_EDGE_LEFT)   x0 = MAP_MINE_EDGE_LEFT + 1;
    if (y0 <= MAP_MINE_EDGE_TOP)    y0 = MAP_MINE_EDGE_TOP + 1;
    if (x1 >= MAP_MINE_EDGE_RIGHT)  x1 = MAP_MINE_EDGE_RIGHT - 1;
    if (y1 >= MAP_MINE_EDGE_BOTTOM) y1 = MAP_MINE_EDGE_BOTTOM - 1;

    if (x0 > x1 || y0 > y1) {
        ed->genPreviewCount = 0;
        return;
    }

    /* Allocate a temp map for generation */
    map tmpMap = (map)malloc(sizeof(struct mapObj));
    if (!tmpMap) { ed->genPreviewCount = 0; return; }

    /* Fill with DEEP_SEA and zero network fields */
    memset(tmpMap, 0, sizeof(struct mapObj));
    memset(tmpMap->mapItem, DEEP_SEA, MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);

    /* Setup temp objects */
    struct basesObj tmpBases;  memset(&tmpBases, 0, sizeof(tmpBases));
    struct pillsObj tmpPills;  memset(&tmpPills, 0, sizeof(tmpPills));
    struct startsObj tmpStarts; memset(&tmpStarts, 0, sizeof(tmpStarts));

    /* Configure generation bounds */
    MapGenConfig tmpCfg = ed->genConfig;
    tmpCfg.x1 = x0; tmpCfg.y1 = y0;
    tmpCfg.x2 = x1; tmpCfg.y2 = y1;
    /* Force no objects for preview */
    tmpCfg.bases = 0; tmpCfg.pills = 0; tmpCfg.starts = 0;

    mapGenRun(tmpMap, &tmpBases, &tmpPills, &tmpStarts, &tmpCfg);

    /* Extract all tiles into preview arrays */
    int count = 0;
    for (int x = x0; x <= x1 && count < ME_FILL_LIMIT; x++) {
        for (int y = y0; y <= y1 && count < ME_FILL_LIMIT; y++) {
            ed->genPreviewX[count] = x;
            ed->genPreviewY[count] = y;
            ed->genPreviewTerrain[count] = tmpMap->mapItem[x][y];
            count++;
        }
    }
    ed->genPreviewCount = count;

    /* Also update genConfig bounds so the settings panel can show them */
    ed->genConfig.x1 = x0; ed->genConfig.y1 = y0;
    ed->genConfig.x2 = x1; ed->genConfig.y2 = y1;

    free(tmpMap);
}

/* -------------------------------------------------------
 * Render generate preview tiles (semi-transparent)
 * ------------------------------------------------------- */
static void meRenderGenPreview(MapEditorState *ed, int screenW, int screenH) {
    if (ed->genPreviewCount == 0) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    /* Full opacity — the preview covers the entire region (including deep sea),
     * so semi-transparent rendering would make rivers indistinguishable from
     * the deep sea underneath. */

    for (int i = 0; i < ed->genPreviewCount; i++) {
        int mx = ed->genPreviewX[i];
        int my = ed->genPreviewY[i];
        float dx = (float)(mx * tileSize - camPX) * zf;
        float dy = (float)(my * tileSize - camPY) * zf;

        if (dx + scaledTile < 0 || dx > screenW ||
            dy + scaledTile < 0 || dy > screenH) continue;

        BYTE iconTile = meTerrainToIconTile(ed->genPreviewTerrain[i]);

        SDL_FRect src = meAtlasSrc(ed, mapViewPosX[iconTile],
                                   mapViewPosY[iconTile]);
        SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
        SDL_RenderTexture(ed->renderer, ed->tilesTex, &src, &dest);
    }
}

/* -------------------------------------------------------
 * Render selection rectangle
 * ------------------------------------------------------- */
static void meRenderSelection(MapEditorState *ed, int screenW, int screenH) {
    if (!ed->hasSelection && !ed->isSelecting) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    int sx1 = ed->selX1, sy1 = ed->selY1;
    int sx2 = ed->selX2, sy2 = ed->selY2;
    if (sx1 > sx2) { int t = sx1; sx1 = sx2; sx2 = t; }
    if (sy1 > sy2) { int t = sy1; sy1 = sy2; sy2 = t; }

    float left   = (float)(sx1 * tileSize - camPX) * zf;
    float top    = (float)(sy1 * tileSize - camPY) * zf;
    float right  = (float)((sx2 + 1) * tileSize - camPX) * zf;
    float bottom = (float)((sy2 + 1) * tileSize - camPY) * zf;

    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);

    /* Semi-transparent fill */
    SDL_SetRenderDrawColor(ed->renderer, 100, 150, 255, 40);
    SDL_FRect fillRect = { left, top, right - left, bottom - top };
    SDL_RenderFillRect(ed->renderer, &fillRect);

    /* Solid border */
    SDL_SetRenderDrawColor(ed->renderer, 100, 150, 255, 200);
    SDL_RenderRect(ed->renderer, &fillRect);
}

/* -------------------------------------------------------
 * Render the scenario's named regions
 * ------------------------------------------------------- */

/* The rectangles the scenario panel's tags view names, over the squares they
 * cover, while that view is open. The camera arithmetic is meRenderSelection's
 * above; the colour is not, so a region and the selection a region's bounds are
 * taken from are told apart on the map. */
static void meRenderScenarioRegions(MapEditorState *ed, int screenW,
                                     int screenH) {
    if (!ed->showScenario || ed->scnView != ME_SCENARIO_VIEW_TAGS) return;
    if (ed->scnForm.manifest.numRegions == 0) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < (int)ed->scnForm.manifest.numRegions; i++) {
        const ScnManifestRegion *r = &ed->scnForm.manifest.regions[i];

        /* The top-left square is inclusive and the size counts squares, so the
           far edge is the square past the last one. */
        float left   = (float)((int)r->x * tileSize - camPX) * zf;
        float top    = (float)((int)r->y * tileSize - camPY) * zf;
        float right  = (float)(((int)r->x + (int)r->w) * tileSize - camPX) * zf;
        float bottom = (float)(((int)r->y + (int)r->h) * tileSize - camPY) * zf;
        SDL_FRect rect = { left, top, right - left, bottom - top };

        SDL_SetRenderDrawColor(ed->renderer, 255, 170, 60, 40);
        SDL_RenderFillRect(ed->renderer, &rect);
        SDL_SetRenderDrawColor(ed->renderer, 255, 170, 60, 220);
        SDL_RenderRect(ed->renderer, &rect);

        if (r->name[0] != '\0') {
            SDL_SetRenderDrawColor(ed->renderer, 255, 220, 160, 255);
            SDL_RenderDebugText(ed->renderer, left + 2.0f, top + 2.0f, r->name);
        }
    }
}

/* -------------------------------------------------------
 * Render paste preview
 * ------------------------------------------------------- */
static void meRenderPastePreview(MapEditorState *ed, int screenW, int screenH,
                                  int hoverMX, int hoverMY) {
    if (!ed->isPasting || !ed->hasClipboard) return;
    if (hoverMX < 0 || hoverMY < 0) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    SDL_SetTextureAlphaMod(ed->tilesTex, 128);

    for (int cy = 0; cy < ed->clipH; cy++) {
        for (int cx = 0; cx < ed->clipW; cx++) {
            int mx = hoverMX + cx;
            int my = hoverMY + cy;
            if (!meIsInMap(mx, my)) continue;

            BYTE raw = ed->clipTerrain[cx][cy];
            if (raw == ME_TRANSPARENT) continue;  /* skip transparent cells */
            BYTE base = raw;
            if (base >= MINE_START && base <= MINE_END)
                base = (BYTE)(base - MINE_SUBTRACT);

            /* Find representative icon tile for the terrain type */
            BYTE iconTile = base; /* default: use raw value as tile */
            for (int i = 0; i < ME_NUM_TERRAINS; i++) {
                if (meTerrainEntries[i].rawValue == base) {
                    iconTile = meTerrainEntries[i].iconTile;
                    break;
                }
            }

            float dx = (float)(mx * tileSize - camPX) * zf;
            float dy = (float)(my * tileSize - camPY) * zf;

            if (dx + scaledTile < 0 || dx > screenW ||
                dy + scaledTile < 0 || dy > screenH) continue;

            SDL_FRect src = meAtlasSrc(ed, mapViewPosX[iconTile],
                                       mapViewPosY[iconTile]);
            SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
            SDL_RenderTexture(ed->renderer, ed->tilesTex, &src, &dest);
        }
    }

    SDL_SetTextureAlphaMod(ed->tilesTex, 255);

    /* Draw outline around paste region */
    float left   = (float)(hoverMX * tileSize - camPX) * zf;
    float top    = (float)(hoverMY * tileSize - camPY) * zf;
    float right  = (float)((hoverMX + ed->clipW) * tileSize - camPX) * zf;
    float bottom = (float)((hoverMY + ed->clipH) * tileSize - camPY) * zf;
    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ed->renderer, 255, 255, 0, 200);
    SDL_FRect outline = { left, top, right - left, bottom - top };
    SDL_RenderRect(ed->renderer, &outline);
}

/* -------------------------------------------------------
 * Render border zone indicator (red tint on non-editable tile under cursor)
 * ------------------------------------------------------- */
static void meRenderBorderIndicator(MapEditorState *ed, int screenW, int screenH,
                                     int hoverMX, int hoverMY) {
    if (hoverMX < 0 || hoverMY < 0) return;
    if (meIsEditable(hoverMX, hoverMY)) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    float dx = (float)(hoverMX * tileSize - camPX) * zf;
    float dy = (float)(hoverMY * tileSize - camPY) * zf;

    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ed->renderer, 255, 0, 0, 100);
    SDL_FRect r = { dx, dy, (float)scaledTile, (float)scaledTile };
    SDL_RenderFillRect(ed->renderer, &r);
}

/* -------------------------------------------------------
 * Clipboard operations
 * ------------------------------------------------------- */
static void meCopySelection(MapEditorState *ed) {
    if (!ed->hasSelection) return;

    int x0 = ed->selX1 < ed->selX2 ? ed->selX1 : ed->selX2;
    int y0 = ed->selY1 < ed->selY2 ? ed->selY1 : ed->selY2;
    int x1 = ed->selX1 > ed->selX2 ? ed->selX1 : ed->selX2;
    int y1 = ed->selY1 > ed->selY2 ? ed->selY1 : ed->selY2;

    int w = x1 - x0 + 1;
    int h = y1 - y0 + 1;
    if (w > ME_CLIP_MAX) w = ME_CLIP_MAX;
    if (h > ME_CLIP_MAX) h = ME_CLIP_MAX;

    ed->clipW = w;
    ed->clipH = h;

    /* Copy terrain — mask-aware for wand selections */
    for (int cx = 0; cx < w; cx++) {
        for (int cy = 0; cy < h; cy++) {
            int mx = x0 + cx;
            int my = y0 + cy;
            if (ed->hasSelMask && !ed->selMask[mx][my]) {
                ed->clipTerrain[cx][cy] = ME_TRANSPARENT;
            } else if (meIsInMap(mx, my)) {
                ed->clipTerrain[cx][cy] = ed->mp->mapItem[mx][my];
            } else {
                ed->clipTerrain[cx][cy] = DEEP_SEA;
            }
        }
    }

    /* Copy objects within selection (full structs, x/y as relative offsets) */
    ed->clipNumPills = 0;
    for (int i = 0; i < ed->pb->numPills && ed->clipNumPills < MAX_PILLS; i++) {
        int px = ed->pb->item[i].x;
        int py = ed->pb->item[i].y;
        if (px >= x0 && px <= x1 && py >= y0 && py <= y1) {
            ed->clipPills[ed->clipNumPills] = ed->pb->item[i];
            ed->clipPills[ed->clipNumPills].x = (BYTE)(px - x0);
            ed->clipPills[ed->clipNumPills].y = (BYTE)(py - y0);
            ed->clipNumPills++;
        }
    }

    ed->clipNumBases = 0;
    for (int i = 0; i < ed->bs->numBases && ed->clipNumBases < MAX_BASES; i++) {
        int bx = ed->bs->item[i].x;
        int by = ed->bs->item[i].y;
        if (bx >= x0 && bx <= x1 && by >= y0 && by <= y1) {
            ed->clipBases[ed->clipNumBases] = ed->bs->item[i];
            ed->clipBases[ed->clipNumBases].x = (BYTE)(bx - x0);
            ed->clipBases[ed->clipNumBases].y = (BYTE)(by - y0);
            ed->clipNumBases++;
        }
    }

    ed->clipNumStarts = 0;
    for (int i = 0; i < ed->ss->numStarts && ed->clipNumStarts < MAX_STARTS; i++) {
        int sx = ed->ss->item[i].x;
        int sy = ed->ss->item[i].y;
        if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) {
            ed->clipStarts[ed->clipNumStarts] = ed->ss->item[i];
            ed->clipStarts[ed->clipNumStarts].x = (BYTE)(sx - x0);
            ed->clipStarts[ed->clipNumStarts].y = (BYTE)(sy - y0);
            ed->clipNumStarts++;
        }
    }

    ed->hasClipboard = true;
}

static void meCutSelection(MapEditorState *ed) {
    if (!ed->hasSelection) return;
    meCopySelection(ed);

    int x0 = ed->selX1 < ed->selX2 ? ed->selX1 : ed->selX2;
    int y0 = ed->selY1 < ed->selY2 ? ed->selY1 : ed->selY2;
    int x1 = ed->selX1 > ed->selX2 ? ed->selX1 : ed->selX2;
    int y1 = ed->selY1 > ed->selY2 ? ed->selY1 : ed->selY2;

    undoBeginCommand(&ed->undoStack);

    /* Fill selection with DEEP_SEA (skip non-masked tiles for wand selections) */
    for (int x = x0; x <= x1; x++) {
        for (int y = y0; y <= y1; y++) {
            if (ed->hasSelMask && !ed->selMask[x][y]) continue;
            meSetTile(ed, x, y, DEEP_SEA);
        }
    }

    /* Remove objects within selection */
    for (int i = ed->pb->numPills - 1; i >= 0; i--) {
        int px = ed->pb->item[i].x;
        int py = ed->pb->item[i].y;
        if (px >= x0 && px <= x1 && py >= y0 && py <= y1) {
            undoRecordObjRemove(&ed->undoStack, OBJ_PILL, i, &ed->pb->item[i]);
            for (int j = i; j < ed->pb->numPills - 1; j++) {
                ed->pb->item[j] = ed->pb->item[j + 1];
            }
            ed->pb->numPills--;
        }
    }

    for (int i = ed->bs->numBases - 1; i >= 0; i--) {
        int bx = ed->bs->item[i].x;
        int by = ed->bs->item[i].y;
        if (bx >= x0 && bx <= x1 && by >= y0 && by <= y1) {
            undoRecordObjRemove(&ed->undoStack, OBJ_BASE, i, &ed->bs->item[i]);
            for (int j = i; j < ed->bs->numBases - 1; j++) {
                ed->bs->item[j] = ed->bs->item[j + 1];
            }
            ed->bs->numBases--;
        }
    }

    for (int i = ed->ss->numStarts - 1; i >= 0; i--) {
        int sx = ed->ss->item[i].x;
        int sy = ed->ss->item[i].y;
        if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) {
            undoRecordObjRemove(&ed->undoStack, OBJ_START, i, &ed->ss->item[i]);
            for (int j = i; j < ed->ss->numStarts - 1; j++) {
                ed->ss->item[j] = ed->ss->item[j + 1];
            }
            ed->ss->numStarts--;
        }
    }

    undoEndCommand(&ed->undoStack);
    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    ed->tabCycleIndex = 0;
}

static void meCommitPaste(MapEditorState *ed, int originX, int originY) {
    if (!ed->hasClipboard) return;

    undoBeginCommand(&ed->undoStack);

    /* Paste terrain (skip ME_TRANSPARENT cells, e.g. from text tool or wand) */
    for (int cx = 0; cx < ed->clipW; cx++) {
        for (int cy = 0; cy < ed->clipH; cy++) {
            BYTE t = ed->clipTerrain[cx][cy];
            if (t == ME_TRANSPARENT) continue;
            int mx = originX + cx;
            int my = originY + cy;
            meSetTile(ed, mx, my, t);
        }
    }

    /* Paste objects (if room) — restore full struct, update x/y to destination */
    for (int i = 0; i < ed->clipNumPills && ed->pb->numPills < MAX_PILLS; i++) {
        int px = originX + ed->clipPills[i].x;
        int py = originY + ed->clipPills[i].y;
        if (meIsEditable(px, py)) {
            pillbox *p = &ed->pb->item[ed->pb->numPills];
            *p = ed->clipPills[i];
            p->x = (BYTE)px;
            p->y = (BYTE)py;
            ed->pb->active[ed->pb->numPills] = TRUE;
            undoRecordObjAdd(&ed->undoStack, OBJ_PILL, ed->pb->numPills, p);
            ed->pb->numPills++;
        }
    }

    for (int i = 0; i < ed->clipNumBases && ed->bs->numBases < MAX_BASES; i++) {
        int bx = originX + ed->clipBases[i].x;
        int by = originY + ed->clipBases[i].y;
        if (meIsEditable(bx, by)) {
            base *b = &ed->bs->item[ed->bs->numBases];
            *b = ed->clipBases[i];
            b->x = (BYTE)bx;
            b->y = (BYTE)by;
            ed->bs->active[ed->bs->numBases] = TRUE;
            undoRecordObjAdd(&ed->undoStack, OBJ_BASE, ed->bs->numBases, b);
            ed->bs->numBases++;
        }
    }

    for (int i = 0; i < ed->clipNumStarts && ed->ss->numStarts < MAX_STARTS; i++) {
        int sx = originX + ed->clipStarts[i].x;
        int sy = originY + ed->clipStarts[i].y;
        if (meIsEditable(sx, sy)) {
            start *s = &ed->ss->item[ed->ss->numStarts];
            *s = ed->clipStarts[i];
            s->x = (BYTE)sx;
            s->y = (BYTE)sy;
            ed->ss->active[ed->ss->numStarts] = TRUE;
            undoRecordObjAdd(&ed->undoStack, OBJ_START, ed->ss->numStarts, s);
            ed->ss->numStarts++;
        }
    }

    undoEndCommand(&ed->undoStack);
    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    ed->tabCycleIndex = 0;
    ed->isPasting = false;
}

/* Largest texture this renderer will allocate, for the sub-1x offscreen.
   Cached — one renderer per process, and this is read per frame and per
   pointer event. Metal and modern GL report 16384; the 4096 fallback is
   what the sub-1x path used to hardcode. */
static int meMaxOffscreen(SDL_Renderer *renderer) {
    static int cached = 0;
    if (cached == 0) {
        SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
        cached = (int)SDL_GetNumberProperty(
            props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096);
        if (cached < 1024) cached = 1024;
    }
    return cached;
}

/* Size of the offscreen texture the sub-1x path renders into for a given
   viewport. Shared by the renderer and the pointer mapping: when the cap
   bites, the offscreen holds less than a windowful of map, and the two must
   agree about that or clicks land on the wrong tile. */
static void meSubZoomRenderSize(MapEditorState *ed, int screenW, int screenH,
                                int *outW, int *outH) {
    int maxTex = meMaxOffscreen(ed->renderer);
    int w = (int)(screenW / ed->zoomLevel);
    int h = (int)(screenH / ed->zoomLevel);
    if (w > maxTex) w = maxTex;
    if (h > maxTex) h = maxTex;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *outW = w;
    *outH = h;
}

/* Ensure offscreen render target exists at the given dimensions. */
static void meEnsureOffscreen(MapEditorState *ed, int needW, int needH) {
    if (ed->offscreenTex && ed->offscreenW == needW && ed->offscreenH == needH)
        return;
    if (ed->offscreenTex) SDL_DestroyTexture(ed->offscreenTex);
    ed->offscreenTex = SDL_CreateTexture(ed->renderer,
        SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, needW, needH);
    /* This texture only ever blits down (it exists for the sub-1x steps), and
       LINEAR's average is the right filter for a non-integer downscale — 0.75x
       is 1.33:1, where NEAREST drifts the sampling phase and makes identical
       tiles look different across the map. State it rather than inheriting it
       from SDL3's renderer default. */
    SDL_SetTextureScaleMode(ed->offscreenTex, SDL_SCALEMODE_LINEAR);
    ed->offscreenW = needW;
    ed->offscreenH = needH;
}

/* Convert screen coordinates to map tile coordinates.
 * Returns true if the position is within the 256x256 map. */
static bool meScreenToMap(MapEditorState *ed, float sx, float sy,
                          int screenW, int screenH,
                          int *outMX, int *outMY) {
    /* For sub-1x zoom, convert screen coords and viewport to offscreen-texture
       space. The blit is an exact zoomLevel scale, so the coordinate divides by
       it; the viewport comes from meSubZoomRenderSize so a capped offscreen is
       mapped the same way it was drawn. */
    if (ed->zoomLevel < 1.0f) {
        sx /= ed->zoomLevel;
        sy /= ed->zoomLevel;
        meSubZoomRenderSize(ed, screenW, screenH, &screenW, &screenH);
    }

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;

    /* Convert world center to pixel center */
    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;

    /* Camera top-left in pixel space */
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    /* Screen position to unscaled pixel position */
    int pixX = camPX + (int)(sx / zf);
    int pixY = camPY + (int)(sy / zf);

    if (pixX < 0 || pixY < 0) return false;

    int mx = pixX / tileSize;
    int my = pixY / tileSize;

    if (mx < 0 || mx > 255 || my < 0 || my > 255) return false;
    *outMX = mx;
    *outMY = my;
    return true;
}

/* Create a blank map: all DEEP_SEA. Border zone (0-20, 236-255) is
 * treated as mined by the game regardless of terrain value, so we
 * leave it as DEEP_SEA rather than MINE_GRASS. */
static void meCreateBlankMap(MapEditorState *ed) {
    mapCreate(&ed->mp);
    pillsCreate(&ed->pb);
    basesCreate(&ed->bs);
    startsCreate(&ed->ss);
    /* mapCreate fills with DEEP_SEA — border is already correct. */
}

/* Free existing map data. */
static void meFreeMapData(MapEditorState *ed) {
    mapDestroy(&ed->mp);
    pillsDestroy(&ed->pb);
    basesDestroy(&ed->bs);
    startsDestroy(&ed->ss);
}

/* -------------------------------------------------------
 * Recent files — stored in [MAPEDITOR] section of the prefs document
 * Keys: Recent1 … Recent10
 * ------------------------------------------------------- */
static void meLoadRecentFiles(MapEditorState *ed) {
    /* Existence is verified lazily on click — see meLoadFromPath +
     * meRemoveRecentFile. A startup fopen() on each entry blocks for
     * seconds when a path points at an unreachable share / dead drive. */
    ed->numRecentFiles = 0;
    for (int i = 0; i < ME_MAX_RECENT_FILES; i++) {
        char key[32];
        snprintf(key, sizeof(key), "Recent%d", i + 1);
        char val[ME_PATH_MAX];
        prefsGetString("MAPEDITOR", key, "", val, ME_PATH_MAX);
        if (val[0] == '\0') break;
        SDL_strlcpy(ed->recentFiles[ed->numRecentFiles], val, ME_PATH_MAX);
        ed->numRecentFiles++;
    }
}

static void meSaveRecentFiles(MapEditorState *ed) {
    for (int i = 0; i < ME_MAX_RECENT_FILES; i++) {
        char key[32];
        snprintf(key, sizeof(key), "Recent%d", i + 1);
        if (i < ed->numRecentFiles) {
            prefsSetString("MAPEDITOR", key, ed->recentFiles[i]);
        } else {
            prefsSetString("MAPEDITOR", key, "");
        }
    }
}

static void meRemoveRecentFile(MapEditorState *ed, const char *path) {
    for (int i = 0; i < ed->numRecentFiles; i++) {
        if (strcmp(ed->recentFiles[i], path) == 0) {
            for (int j = i; j < ed->numRecentFiles - 1; j++) {
                SDL_strlcpy(ed->recentFiles[j], ed->recentFiles[j + 1], ME_PATH_MAX);
            }
            ed->numRecentFiles--;
            meSaveRecentFiles(ed);
            return;
        }
    }
}

static void meAddRecentFile(MapEditorState *ed, const char *path) {
    /* Caller may pass a pointer into ed->recentFiles itself (e.g. open-recent
     * menu). The shifts below would corrupt that aliased memory mid-update,
     * so snapshot the path first. */
    char pathCopy[ME_PATH_MAX];
    SDL_strlcpy(pathCopy, path, ME_PATH_MAX);
    path = pathCopy;
    /* Remove existing entry for this path (if any) */
    for (int i = 0; i < ed->numRecentFiles; i++) {
        if (strcmp(ed->recentFiles[i], path) == 0) {
            /* Shift remaining entries down */
            for (int j = i; j < ed->numRecentFiles - 1; j++) {
                SDL_strlcpy(ed->recentFiles[j], ed->recentFiles[j + 1], ME_PATH_MAX);
            }
            ed->numRecentFiles--;
            break;
        }
    }
    /* Shift everything down to make room at position 0 */
    int count = ed->numRecentFiles;
    if (count >= ME_MAX_RECENT_FILES) count = ME_MAX_RECENT_FILES - 1;
    for (int i = count; i > 0; i--) {
        SDL_strlcpy(ed->recentFiles[i], ed->recentFiles[i - 1], ME_PATH_MAX);
    }
    SDL_strlcpy(ed->recentFiles[0], path, ME_PATH_MAX);
    ed->numRecentFiles = count + 1;
    meSaveRecentFiles(ed);
}

/* -------------------------------------------------------
 * Window title update
 * ------------------------------------------------------- */
static void meUpdateWindowTitle(MapEditorState *ed) {
    char title[ME_PATH_MAX + 64];
    const char *name = "Untitled";
    if (ed->currentFilePath[0]) {
        const char *slash = strrchr(ed->currentFilePath, '/');
        if (!slash) slash = strrchr(ed->currentFilePath, '\\');
        name = slash ? slash + 1 : ed->currentFilePath;
    }
    const char *prefix = ed->fromMainMenu ? "Map Editor" : "WinBolo Map Editor";
    if (ed->dirty) {
        snprintf(title, sizeof(title), "%s - %s *", prefix, name);
    } else {
        snprintf(title, sizeof(title), "%s - %s", prefix, name);
    }
    SDL_SetWindowTitle(ed->window, title);
}

/* -------------------------------------------------------
 * Symmetry operations
 * ------------------------------------------------------- */

/* Compute the symmetry operation region, clamped to playable area. */
static void meSymmetryRegion(MapEditorState *ed, int *x1, int *y1, int *x2, int *y2) {
    if (ed->hasSelection) {
        int a = ed->selX1, b = ed->selX2;
        int c = ed->selY1, d = ed->selY2;
        *x1 = a < b ? a : b; *x2 = a < b ? b : a;
        *y1 = c < d ? c : d; *y2 = c < d ? d : c;
        if (*x1 <= MAP_MINE_EDGE_LEFT)   *x1 = MAP_MINE_EDGE_LEFT + 1;
        if (*y1 <= MAP_MINE_EDGE_TOP)    *y1 = MAP_MINE_EDGE_TOP + 1;
        if (*x2 >= MAP_MINE_EDGE_RIGHT)  *x2 = MAP_MINE_EDGE_RIGHT - 1;
        if (*y2 >= MAP_MINE_EDGE_BOTTOM) *y2 = MAP_MINE_EDGE_BOTTOM - 1;
    } else {
        *x1 = MAP_MINE_EDGE_LEFT + 1;
        *y1 = MAP_MINE_EDGE_TOP + 1;
        *x2 = MAP_MINE_EDGE_RIGHT - 1;
        *y2 = MAP_MINE_EDGE_BOTTOM - 1;
    }
}

/* Count objects on invalid terrain after a symmetry operation. */
static int meCountInvalidObjects(MapEditorState *ed, int x1, int y1, int x2, int y2) {
    int count = 0;
    for (int i = 0; i < ed->bs->numBases; i++) {
        base *b = &ed->bs->item[i];
        if (b->x >= x1 && b->x <= x2 && b->y >= y1 && b->y <= y2) {
            if (!meIsTraversableLand(ed, b->x, b->y)) count++;
        }
    }
    for (int i = 0; i < ed->pb->numPills; i++) {
        pillbox *p = &ed->pb->item[i];
        if (p->x >= x1 && p->x <= x2 && p->y >= y1 && p->y <= y2) {
            if (!meIsTraversableLand(ed, p->x, p->y)) count++;
        }
    }
    for (int i = 0; i < ed->ss->numStarts; i++) {
        start *s = &ed->ss->item[i];
        if (s->x >= x1 && s->x <= x2 && s->y >= y1 && s->y <= y2) {
            if (!meIsTraversableLand(ed, s->x, s->y)) count++;
        }
    }
    return count;
}

/* Record undo for all tiles in the region that will change due to mirror H. */
static void meUndoRecordMirrorH(MapEditorState *ed, int x1, int y1, int x2, int y2) {
    for (int y = y1; y <= y2; y++) {
        for (int i = 0; i <= (x2 - x1) / 2; i++) {
            int lx = x1 + i, rx = x2 - i;
            if (lx >= rx) break;
            BYTE lT = ed->mp->mapItem[lx][y];
            BYTE rT = ed->mp->mapItem[rx][y];
            if (lT != rT) {
                undoRecordTile(&ed->undoStack, ed->mp, (BYTE)lx, (BYTE)y, rT);
                undoRecordTile(&ed->undoStack, ed->mp, (BYTE)rx, (BYTE)y, lT);
            }
        }
    }
}

/* Record undo for all tiles in the region that will change due to mirror V. */
static void meUndoRecordMirrorV(MapEditorState *ed, int x1, int y1, int x2, int y2) {
    for (int x = x1; x <= x2; x++) {
        for (int j = 0; j <= (y2 - y1) / 2; j++) {
            int ty = y1 + j, by = y2 - j;
            if (ty >= by) break;
            BYTE tT = ed->mp->mapItem[x][ty];
            BYTE bT = ed->mp->mapItem[x][by];
            if (tT != bT) {
                undoRecordTile(&ed->undoStack, ed->mp, (BYTE)x, (BYTE)ty, bT);
                undoRecordTile(&ed->undoStack, ed->mp, (BYTE)x, (BYTE)by, tT);
            }
        }
    }
}

/* Record undo for all tiles in the region that will change due to rotate 90. */
static void meUndoRecordRotate90(MapEditorState *ed, int x1, int y1, int x2, int y2) {
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int clearN = w > h ? w : h;

    /* Build a temporary buffer with the post-rotation state for the affected area.
       The union of old (w×h) and new (h×w) regions is clearN×clearN. */
    BYTE *post = (BYTE *)malloc((size_t)clearN * (size_t)clearN);
    if (!post) return;

    /* Initialize to DEEP_SEA (cleared area) */
    memset(post, DEEP_SEA, (size_t)clearN * (size_t)clearN);

    /* Compute rotated values: src(sx,sy) -> dest(sy, w-1-sx) */
    for (int sx = 0; sx < w; sx++)
        for (int sy = 0; sy < h; sy++)
            post[sy * clearN + (w - 1 - sx)] = ed->mp->mapItem[x1 + sx][y1 + sy];

    /* Record each tile in the union that changes */
    for (int dx = 0; dx < clearN && (x1 + dx) <= 255; dx++) {
        for (int dy = 0; dy < clearN && (y1 + dy) <= 255; dy++) {
            BYTE cur = ed->mp->mapItem[x1 + dx][y1 + dy];
            BYTE newVal = post[dx * clearN + dy];
            if (cur != newVal) {
                undoRecordTile(&ed->undoStack, ed->mp, (BYTE)(x1 + dx), (BYTE)(y1 + dy), newVal);
            }
        }
    }

    free(post);
}

/* Record undo for all tiles in the region that will change due to rotate 180. */
static void meUndoRecordRotate180(MapEditorState *ed, int x1, int y1, int x2, int y2) {
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int total = w * h;
    for (int idx = 0; idx < total / 2; idx++) {
        int ix = idx % w;
        int iy = idx / w;
        int ax = x1 + ix, ay = y1 + iy;
        int bx = x2 - ix, by = y2 - iy;
        BYTE aT = ed->mp->mapItem[ax][ay];
        BYTE bT = ed->mp->mapItem[bx][by];
        if (aT != bT) {
            undoRecordTile(&ed->undoStack, ed->mp, (BYTE)ax, (BYTE)ay, bT);
            undoRecordTile(&ed->undoStack, ed->mp, (BYTE)bx, (BYTE)by, aT);
        }
    }
}

/* Record undo for all objects in the region for a given symmetry operation.
 * opType: 0=mirrorH, 1=mirrorV, 2=rotate90, 3=rotate180 */
static void meUndoRecordSymObjects(MapEditorState *ed, int x1, int y1, int x2, int y2, int opType) {
    int n = x2 - x1 + 1;
    for (int i = 0; i < ed->bs->numBases; i++) {
        base *b = &ed->bs->item[i];
        if (b->x >= x1 && b->x <= x2 && b->y >= y1 && b->y <= y2) {
            base old = *b;
            base nw = *b;
            switch (opType) {
            case 0: nw.x = (BYTE)(x1 + (x2 - b->x)); break;
            case 1: nw.y = (BYTE)(y1 + (y2 - b->y)); break;
            case 2: { int rx = b->x - x1, ry = b->y - y1; nw.x = (BYTE)(x1 + ry); nw.y = (BYTE)(y1 + (n - 1 - rx)); } break;
            case 3: nw.x = (BYTE)(x1 + (x2 - b->x)); nw.y = (BYTE)(y1 + (y2 - b->y)); break;
            }
            undoRecordObjModify(&ed->undoStack, OBJ_BASE, i, &old, &nw);
        }
    }
    for (int i = 0; i < ed->pb->numPills; i++) {
        pillbox *p = &ed->pb->item[i];
        if (p->x >= x1 && p->x <= x2 && p->y >= y1 && p->y <= y2) {
            pillbox old = *p;
            pillbox nw = *p;
            switch (opType) {
            case 0: nw.x = (BYTE)(x1 + (x2 - p->x)); break;
            case 1: nw.y = (BYTE)(y1 + (y2 - p->y)); break;
            case 2: { int rx = p->x - x1, ry = p->y - y1; nw.x = (BYTE)(x1 + ry); nw.y = (BYTE)(y1 + (n - 1 - rx)); } break;
            case 3: nw.x = (BYTE)(x1 + (x2 - p->x)); nw.y = (BYTE)(y1 + (y2 - p->y)); break;
            }
            undoRecordObjModify(&ed->undoStack, OBJ_PILL, i, &old, &nw);
        }
    }
    for (int i = 0; i < ed->ss->numStarts; i++) {
        start *s = &ed->ss->item[i];
        if (s->x >= x1 && s->x <= x2 && s->y >= y1 && s->y <= y2) {
            start old = *s;
            start nw = *s;
            switch (opType) {
            case 0: nw.x = (BYTE)(x1 + (x2 - s->x)); nw.dir = (BYTE)((16 - s->dir) % 16); break;
            case 1: nw.y = (BYTE)(y1 + (y2 - s->y)); nw.dir = (BYTE)((8 - s->dir + 16) % 16); break;
            case 2: { int rx = s->x - x1, ry = s->y - y1; nw.x = (BYTE)(x1 + ry); nw.y = (BYTE)(y1 + (n - 1 - rx)); nw.dir = (BYTE)((s->dir + 4) % 16); } break;
            case 3: nw.x = (BYTE)(x1 + (x2 - s->x)); nw.y = (BYTE)(y1 + (y2 - s->y)); nw.dir = (BYTE)((s->dir + 8) % 16); break;
            }
            undoRecordObjModify(&ed->undoStack, OBJ_START, i, &old, &nw);
        }
    }
}

/* Post-operation: warn if objects ended up on invalid terrain. */
static void meSymmetryPostCheck(MapEditorState *ed, int x1, int y1, int x2, int y2) {
    int bad = meCountInvalidObjects(ed, x1, y1, x2, y2);
    if (bad > 0) {
        meSetStatus(ed, "Warning: %d object(s) on invalid terrain after symmetry - run Validate to review", bad);
    }
}

static void meEditorMirrorH(MapEditorState *ed) {
    int x1, y1, x2, y2;
    meSymmetryRegion(ed, &x1, &y1, &x2, &y2);

    undoBeginCommand(&ed->undoStack);
    meUndoRecordMirrorH(ed, x1, y1, x2, y2);
    meUndoRecordSymObjects(ed, x1, y1, x2, y2, 0);
    symmetryMirrorH(ed->mp, ed->bs, ed->pb, ed->ss, x1, y1, x2, y2);
    undoEndCommand(&ed->undoStack);

    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    meSymmetryPostCheck(ed, x1, y1, x2, y2);
}

static void meEditorMirrorV(MapEditorState *ed) {
    int x1, y1, x2, y2;
    meSymmetryRegion(ed, &x1, &y1, &x2, &y2);

    undoBeginCommand(&ed->undoStack);
    meUndoRecordMirrorV(ed, x1, y1, x2, y2);
    meUndoRecordSymObjects(ed, x1, y1, x2, y2, 1);
    symmetryMirrorV(ed->mp, ed->bs, ed->pb, ed->ss, x1, y1, x2, y2);
    undoEndCommand(&ed->undoStack);

    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    meSymmetryPostCheck(ed, x1, y1, x2, y2);
}

static void meEditorRotate90(MapEditorState *ed) {
    int x1, y1, x2, y2;
    meSymmetryRegion(ed, &x1, &y1, &x2, &y2);

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;

    undoBeginCommand(&ed->undoStack);
    meUndoRecordRotate90(ed, x1, y1, x2, y2);
    meUndoRecordSymObjects(ed, x1, y1, x2, y2, 2);
    if (symmetryRotate90(ed->mp, ed->bs, ed->pb, ed->ss, x1, y1, x2, y2)) {
        /* Update selection bounds to the new rotated dimensions */
        if (ed->hasSelection) {
            ed->selX2 = ed->selX1 + h - 1;
            ed->selY2 = ed->selY1 + w - 1;
        }
    }
    undoEndCommand(&ed->undoStack);

    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    meSymmetryPostCheck(ed, x1, y1, x2, y2);
}

static void meEditorRotate180(MapEditorState *ed) {
    int x1, y1, x2, y2;
    meSymmetryRegion(ed, &x1, &y1, &x2, &y2);

    undoBeginCommand(&ed->undoStack);
    meUndoRecordRotate180(ed, x1, y1, x2, y2);
    meUndoRecordSymObjects(ed, x1, y1, x2, y2, 3);
    symmetryRotate180(ed->mp, ed->bs, ed->pb, ed->ss, x1, y1, x2, y2);
    undoEndCommand(&ed->undoStack);

    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    meSymmetryPostCheck(ed, x1, y1, x2, y2);
}

/* -------------------------------------------------------
 * Validation
 * ------------------------------------------------------- */

/* Run full validation and store results. Shows the validation panel. */
static void meRunValidation(MapEditorState *ed) {
    validateResultFree(&ed->lastValidation);
    ed->lastValidation = mapEditorValidate(ed->mp, ed->bs, ed->pb, ed->ss);
    ed->showValidationPanel = true;
}

/* Pre-save check: run validation, block save if errors. Returns true if OK. */
static bool meValidateForSave(MapEditorState *ed) {
    validateResultFree(&ed->lastValidation);
    ed->lastValidation = mapEditorValidate(ed->mp, ed->bs, ed->pb, ed->ss);
    if (ed->lastValidation.errorCount > 0) {
        ed->showValidationPanel = true;
        ed->validationErrorModalOpen = true;
        return false;
    }
    return true;
}

/* -------------------------------------------------------
 * Save to a specific path. Returns true on success.
 * ------------------------------------------------------- */
static bool meSaveToPath(MapEditorState *ed, const char *path) {
    if (!meValidateForSave(ed)) return false;
    if (!mapWrite((char *)path, &ed->mp, &ed->pb, &ed->bs, &ed->ss)) {
        snprintf(ed->errorMessage, sizeof(ed->errorMessage),
                 "Failed to write map file:\n%s", path);
        return false;
    }
    SDL_strlcpy(ed->currentFilePath, path, ME_PATH_MAX);
    ed->dirty = false;
    ed->undoStack.savedCommandIndex = ed->undoStack.count;
    meAddRecentFile(ed, path);
    meUpdateWindowTitle(ed);

    /* The script lives beside the map, so an edited one is written with it.
     * The map is on disk by now, so a script that will not write is shown
     * the way any other editor error is and the save still counts as done.
     * With the script untouched there is nothing to write, but the state
     * still follows the map to the name it was saved under. */
    if (meScenarioDirty(&ed->scn)) {
        if (!meScenarioSaveForMap(&ed->scn, path)) {
            /* The state says what stopped it — a write that failed, or a
             * refusal to write over a script it could not open. */
            snprintf(ed->errorMessage, sizeof(ed->errorMessage), "%s\n%s",
                     ed->scn.status, ed->scn.scriptPath);
        } else {
            /* Written, so checked: a script is validated every time it
             * reaches the disk, whichever Save put it there. */
            meScenarioCheckRun(&ed->scnCheck, ed->scn.script,
                               ed->scn.scriptLen,
                               ed->scn.scriptPath[0] != '\0'
                                   ? ed->scn.scriptPath
                                   : ME_SCENARIO_CHECK_UNNAMED,
                               &ed->scnForm.manifest);
        }
    } else {
        meScenarioAdoptPath(&ed->scn, path);
    }

    /* mapWrite truncated the file, so a container that was on it is gone. Put
     * back the one the map came with — the bytes that were already there,
     * which need no checking. What the forms hold may have been edited and is
     * written by Pack into Map, which checks it first. Saved under a new name,
     * the new file gets the chunk by this same path. */
    if (ed->scnPacked.present) {
        char packErr[ME_SCENARIO_PACK_ERR_LEN];
        char line[256];

        if (meScenarioPackedRestore(&ed->scnPacked, path, packErr,
                                    sizeof(packErr))) {
            meScenarioSetStatus(&ed->scn,
                                langGetText(STR_MAPEDIT_SCENARIO_CHUNK_KEPT));
        } else {
            /* The map is written either way, so this is reported the way a
             * failed script write is and the save still counts. */
            snprintf(line, sizeof(line), "%s: %s",
                     langGetText(STR_MAPEDIT_SCENARIO_CHUNK_LOST), packErr);
            meScenarioSetStatus(&ed->scn, line);
            snprintf(ed->errorMessage, sizeof(ed->errorMessage), "%s\n%s",
                     langGetText(STR_MAPEDIT_SCENARIO_CHUNK_LOST), path);
        }
    }
    return true;
}

/* -------------------------------------------------------
 * The scenario already packed into a map, into the panel.
 * ------------------------------------------------------- */
/* A map can carry its scenario inside it: a container after the map data with
 * the manifest and the script in it. The forms take that manifest, or the next
 * pack writes a blank one over the author's.
 *
 * The pane takes the packed script only where there is no loose
 * X.scenario.lua beside the map: a loose script overrides the packed one when
 * a server loads the map, so it overrides it here too. Saving the pane still
 * writes the loose file, which is the dev loop — edit, reload, play, pack when
 * done.
 *
 * Called after meScenarioSetMap and meScenarioFormReset, which are what decide
 * whether there is a loose script and leave the form empty for this to fill.
 * A plain map is the ordinary case and says nothing. */
static void meLoadPackedScenario(MapEditorState *ed, const char *path) {
    ScenarioManifest packed;
    char            *script    = NULL;
    size_t           scriptLen = 0;
    bool             found     = false;
    char             err[ME_SCENARIO_PACK_ERR_LEN];
    char             line[256];

    if (path == NULL || path[0] == '\0') {
        return;
    }

    if (!meScenarioReadFromMap(path, &packed, &script, &scriptLen, &found, err,
                               sizeof(err))) {
        /* The map itself read, so this is a container that would not open
         * rather than a map that would not. It goes on the panel's status line
         * and not into a modal over the map. */
        snprintf(line, sizeof(line), "%s: %s",
                 langGetText(STR_MAPEDIT_SCENARIO_PACK_READ_FAILED), err);
        meScenarioSetStatus(&ed->scn, line);
        return;
    }
    if (!found) {
        return;
    }

    ed->scnForm.manifest = packed;
    ed->scnForm.dirty    = false;

    if (!ed->scn.fileOnDisk && script != NULL) {
        meScenarioSetPackedScript(&ed->scn, script, scriptLen);
    }
    /* Kept so a map save can put the chunk back: mapWrite truncates the file.
     * These are the bytes that were on it, not what the forms now hold. */
    meScenarioPackedSet(&ed->scnPacked, &packed, script, scriptLen);
    free(script);
}

/* -------------------------------------------------------
 * Load a map from path. Returns true on success.
 * ------------------------------------------------------- */
static bool meLoadFromPath(MapEditorState *ed, const char *path) {
    map newMp;
    pillboxes newPb;
    bases newBs;
    starts newSs;

    mapCreate(&newMp);
    pillsCreate(&newPb);
    basesCreate(&newBs);
    startsCreate(&newSs);

    if (!mapRead((char *)path, &newMp, &newPb, &newBs, &newSs)) {
        mapDestroy(&newMp);
        pillsDestroy(&newPb);
        basesDestroy(&newBs);
        startsDestroy(&newSs);
        snprintf(ed->errorMessage, sizeof(ed->errorMessage),
                 "Failed to read map file:\n%s", path);
        meRemoveRecentFile(ed, path);
        return false;
    }

    /* Replace current map data */
    meFreeMapData(ed);
    ed->mp = newMp;
    ed->pb = newPb;
    ed->bs = newBs;
    ed->ss = newSs;

    SDL_strlcpy(ed->currentFilePath, path, ME_PATH_MAX);
    ed->dirty = false;
    ed->currentStartIndex = 0;
    undoStackClear(&ed->undoStack);
    ed->selectedObjKind = ME_SEL_NONE;
    ed->selectedObjIndex = -1;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    ed->tabCycleIndex = 0;
    meAddRecentFile(ed, path);
    /* The script beside the new map replaces whatever was being edited, and
     * the manifest goes with it: it belongs to the map that was open. */
    meScenarioSetMap(&ed->scn, path);
    meScenarioFormReset(&ed->scnForm);
    meScenarioCheckClear(&ed->scnCheck);
    /* Whatever the last map carried is not this map's. A map that opens with
     * no container leaves this empty, so its save writes none. */
    meScenarioPackedClear(&ed->scnPacked);
    /* And whatever the map itself carries, which the empty form above is
     * waiting for. */
    meLoadPackedScenario(ed, path);
    meUpdateWindowTitle(ed);
    return true;
}

#ifdef MAPEDITOR_WBN_OPEN
/* -------------------------------------------------------
 * Load a map from an in-memory .map image (a WinBolo.net
 * download). Returns true on success. The map has no file
 * behind it, so currentFilePath is cleared and Save routes
 * through Save As; nothing is added to the recent list.
 * ------------------------------------------------------- */
static bool meLoadFromMemory(MapEditorState *ed, const unsigned char *bytes,
                             int len, const char *displayName) {
    map newMp;
    pillboxes newPb;
    bases newBs;
    starts newSs;

    mapCreate(&newMp);
    pillsCreate(&newPb);
    basesCreate(&newBs);
    startsCreate(&newSs);

    if (!mapReadFromMemory((const BYTE *)bytes, len, &newMp, &newPb, &newBs, &newSs)) {
        mapDestroy(&newMp);
        pillsDestroy(&newPb);
        basesDestroy(&newBs);
        startsDestroy(&newSs);
        snprintf(ed->errorMessage, sizeof(ed->errorMessage),
                 "Failed to read map from WinBolo.net:\n%s", displayName);
        return false;
    }

    /* Replace current map data */
    meFreeMapData(ed);
    ed->mp = newMp;
    ed->pb = newPb;
    ed->bs = newBs;
    ed->ss = newSs;

    ed->currentFilePath[0] = '\0';
    ed->dirty = false;
    ed->currentStartIndex = 0;
    undoStackClear(&ed->undoStack);
    ed->selectedObjKind = ME_SEL_NONE;
    ed->selectedObjIndex = -1;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    ed->tabCycleIndex = 0;
    /* A download has no file behind it, so there is nowhere for a script to
     * sit beside it until the map is saved. */
    meScenarioSetMap(&ed->scn, "");
    meScenarioFormReset(&ed->scnForm);
    meScenarioCheckClear(&ed->scnCheck);
    /* Whatever the last map carried is not this map's. A map that opens with
     * no container leaves this empty, so its save writes none. */
    meScenarioPackedClear(&ed->scnPacked);
    meUpdateWindowTitle(ed);
    return true;
}
#endif /* MAPEDITOR_WBN_OPEN */

/* -------------------------------------------------------
 * New blank map
 * ------------------------------------------------------- */
static void meDoNew(MapEditorState *ed) {
    meFreeMapData(ed);
    meCreateBlankMap(ed);
    ed->currentFilePath[0] = '\0';
    ed->dirty = false;
    ed->currentStartIndex = 0;
    undoStackClear(&ed->undoStack);
    ed->viewCenterX = 128 << 8;
    ed->viewCenterY = 128 << 8;
    ed->selectedObjKind = ME_SEL_NONE;
    ed->selectedObjIndex = -1;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    ed->tabCycleIndex = 0;
    /* A blank map has no file yet, so it has no script either. */
    meScenarioSetMap(&ed->scn, "");
    meScenarioFormReset(&ed->scnForm);
    meScenarioCheckClear(&ed->scnCheck);
    /* Whatever the last map carried is not this map's. A map that opens with
     * no container leaves this empty, so its save writes none. */
    meScenarioPackedClear(&ed->scnPacked);
    meUpdateWindowTitle(ed);
}

/* -------------------------------------------------------
 * SDL3 async file dialog callbacks
 * ------------------------------------------------------- */
static void SDLCALL meFileDialogCallback(void *userdata,
                                          const char *const *filelist,
                                          int filter) {
    MapEditorState *ed = (MapEditorState *)userdata;
    (void)filter;
    ed->fileDialogResult[0] = '\0';
    if (filelist && filelist[0]) {
        SDL_strlcpy(ed->fileDialogResult, filelist[0], ME_PATH_MAX);
    }
    ed->fileDialogGotResult = true;
    ed->fileDialogPending = false;
}

static void meShowOpenDialog(MapEditorState *ed, FileOp op) {
    SDL_DialogFileFilter filters[] = {
        { "Map Files", "map" },
        { "All Files", "*" },
    };
    ed->pendingFileOp = op;
    ed->fileDialogPending = true;
    ed->fileDialogGotResult = false;
    SDL_ShowOpenFileDialog(meFileDialogCallback, ed, ed->window, filters, 2, NULL, false);
}

static void meShowSaveDialog(MapEditorState *ed, FileOp op) {
    SDL_DialogFileFilter filters[] = {
        { "Map Files", "map" },
    };
    ed->pendingFileOp = op;
    ed->fileDialogPending = true;
    ed->fileDialogGotResult = false;
    SDL_ShowSaveFileDialog(meFileDialogCallback, ed, ed->window, filters, 1, NULL);
}

/* The same dialog for the other kind of file the editor writes: a scenario
 * that plays over any map rather than a map. The name it opens on is the
 * manifest's own, so a scenario called Fast Reload is offered as
 * Fast Reload.scenario. */
static void meShowSaveModDialog(MapEditorState *ed) {
    SDL_DialogFileFilter filters[] = {
        { "Scenario Files", "scenario" },
    };
    char suggested[ME_PATH_MAX];

    if (!meScenarioModFileName(&ed->scnForm.manifest, suggested,
                               sizeof(suggested))) {
        suggested[0] = '\0';
    }
    ed->pendingFileOp = FILE_OP_SAVE_SCENARIO_MOD;
    ed->fileDialogPending = true;
    ed->fileDialogGotResult = false;
    SDL_ShowSaveFileDialog(meFileDialogCallback, ed, ed->window, filters, 1,
                           suggested[0] != '\0' ? suggested : NULL);
}

/* -------------------------------------------------------
 * Render the map tiles for the current viewport.
 * ------------------------------------------------------- */
static void meRenderTiles(MapEditorState *ed, int screenW, int screenH) {
    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;

    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    int camMX = camPX / tileSize;
    int camMY = camPY / tileSize;
    if (camPX < 0) camMX--;
    if (camPY < 0) camMY--;
    int edgeX = (camPX - camMX * tileSize) * zf;
    int edgeY = (camPY - camMY * tileSize) * zf;

    int tilesW = screenW / scaledTile + 3;
    int tilesH = screenH / scaledTile + 3;
    if (tilesW > 256) tilesW = 256;
    if (tilesH > 256) tilesH = 256;

    for (int x = 0; x < tilesW; x++) {
        for (int y = 0; y < tilesH; y++) {
            int mapX = camMX + x;
            int mapY = camMY + y;

            BYTE tileNum;
            if (mapX < 0 || mapX > 255 || mapY < 0 || mapY > 255) {
                tileNum = DEEP_SEA_SOLID;
            } else {
                tileNum = meCalcTile(ed, (BYTE)mapX, (BYTE)mapY);
            }

            SDL_FRect src = meAtlasSrc(ed, mapViewPosX[tileNum],
                                       mapViewPosY[tileNum]);
            SDL_FRect dest = {
                (float)(x * scaledTile - edgeX),
                (float)(y * scaledTile - edgeY),
                (float)scaledTile,
                (float)scaledTile
            };
            SDL_RenderTexture(ed->renderer, ed->tilesTex, &src, &dest);

            /* Mine overlay — blit the mine sprite from the atlas */
            if (ed->showMines && mapX >= 0 && mapX <= 255 && mapY >= 0 && mapY <= 255) {
                BYTE raw = ed->mp->mapItem[mapX][mapY];
                bool hasMine = (raw >= MINE_START && raw <= MINE_END);
                /* Border zone is implicitly mined by the game */
                if (!hasMine &&
                    (mapX <= MAP_MINE_EDGE_LEFT || mapX >= MAP_MINE_EDGE_RIGHT ||
                     mapY <= MAP_MINE_EDGE_TOP  || mapY >= MAP_MINE_EDGE_BOTTOM)) {
                    hasMine = true;
                }
                if (hasMine) {
                    SDL_FRect mineSrc = meAtlasSrc(ed, MINE_X, MINE_Y);
                    SDL_RenderTexture(ed->renderer, ed->tilesTex, &mineSrc, &dest);
                }
            }
        }
    }
}

/* Render grid lines between tiles. */
static void meRenderGrid(MapEditorState *ed, int screenW, int screenH) {
    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;

    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    int camMX = camPX / tileSize;
    int camMY = camPY / tileSize;
    if (camPX < 0) camMX--;
    if (camPY < 0) camMY--;
    int edgeX = (camPX - camMX * tileSize) * zf;
    int edgeY = (camPY - camMY * tileSize) * zf;

    int tilesW = screenW / scaledTile + 3;
    int tilesH = screenH / scaledTile + 3;

    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ed->renderer, 255, 255, 255, 40);

    /* Vertical lines */
    for (int x = 0; x <= tilesW; x++) {
        float px = (float)(x * scaledTile - edgeX);
        SDL_RenderLine(ed->renderer, px, 0, px, (float)screenH);
    }
    /* Horizontal lines */
    for (int y = 0; y <= tilesH; y++) {
        float py = (float)(y * scaledTile - edgeY);
        SDL_RenderLine(ed->renderer, 0, py, (float)screenW, py);
    }
}

/* Boat sprite atlas coordinates — not in mapViewPosX/Y (those only cover
 * terrain tiles), so we keep a small local table. */
static const int meBoatAtlasX[16] = {
    TANK_SELFBOAT_0_X,  TANK_SELFBOAT_1_X,  TANK_SELFBOAT_2_X,  TANK_SELFBOAT_3_X,
    TANK_SELFBOAT_4_X,  TANK_SELFBOAT_5_X,  TANK_SELFBOAT_6_X,  TANK_SELFBOAT_7_X,
    TANK_SELFBOAT_8_X,  TANK_SELFBOAT_9_X,  TANK_SELFBOAT_10_X, TANK_SELFBOAT_11_X,
    TANK_SELFBOAT_12_X, TANK_SELFBOAT_13_X, TANK_SELFBOAT_14_X, TANK_SELFBOAT_15_X
};
static const int meBoatAtlasY[16] = {
    TANK_SELFBOAT_0_Y,  TANK_SELFBOAT_1_Y,  TANK_SELFBOAT_2_Y,  TANK_SELFBOAT_3_Y,
    TANK_SELFBOAT_4_Y,  TANK_SELFBOAT_5_Y,  TANK_SELFBOAT_6_Y,  TANK_SELFBOAT_7_Y,
    TANK_SELFBOAT_8_Y,  TANK_SELFBOAT_9_Y,  TANK_SELFBOAT_10_Y, TANK_SELFBOAT_11_Y,
    TANK_SELFBOAT_12_Y, TANK_SELFBOAT_13_Y, TANK_SELFBOAT_14_Y, TANK_SELFBOAT_15_Y
};

/* Render start positions as tank sprites. */
static void meRenderStarts(MapEditorState *ed, int screenW, int screenH) {
    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;

    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    SDL_SetTextureAlphaMod(ed->tilesTex, 200);

    for (BYTE i = 0; i < ed->ss->numStarts; i++) {
        /* Skip if being dragged (rendered at cursor instead) */
        if (ed->isDraggingObj && ed->selectedObjKind == ME_SEL_START &&
            ed->selectedObjIndex == i) continue;

        start *s = &ed->ss->item[i];
        float dx = (float)((int)s->x * tileSize - camPX) * zf;
        float dy = (float)((int)s->y * tileSize - camPY) * zf;

        if (dx + scaledTile < 0 || dx > screenW ||
            dy + scaledTile < 0 || dy > screenH) continue;

        int dir = startsConvertDir((s->dir < 16) ? s->dir : 0);
        SDL_FRect src = meAtlasSrc(ed, meBoatAtlasX[dir], meBoatAtlasY[dir]);
        SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
        SDL_RenderTexture(ed->renderer, ed->tilesTex, &src, &dest);
    }

    SDL_SetTextureAlphaMod(ed->tilesTex, 255);
}

/* Render selection highlight around the selected object. */
static void meRenderObjSelection(MapEditorState *ed, int screenW, int screenH) {
    if (ed->selectedObjKind == ME_SEL_NONE) return;
    if (ed->isDraggingObj) return; /* Don't show highlight on original position while dragging */

    int tx = -1, ty = -1;
    if (ed->selectedObjKind == ME_SEL_BASE && ed->selectedObjIndex < ed->bs->numBases) {
        tx = ed->bs->item[ed->selectedObjIndex].x;
        ty = ed->bs->item[ed->selectedObjIndex].y;
    } else if (ed->selectedObjKind == ME_SEL_PILL && ed->selectedObjIndex < ed->pb->numPills) {
        tx = ed->pb->item[ed->selectedObjIndex].x;
        ty = ed->pb->item[ed->selectedObjIndex].y;
    } else if (ed->selectedObjKind == ME_SEL_START && ed->selectedObjIndex < ed->ss->numStarts) {
        tx = ed->ss->item[ed->selectedObjIndex].x;
        ty = ed->ss->item[ed->selectedObjIndex].y;
    } else {
        return;
    }

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;
    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    float dx = (float)(tx * tileSize - camPX) * zf;
    float dy = (float)(ty * tileSize - camPY) * zf;

    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ed->renderer, 0, 255, 255, 255);
    /* Draw 2px border by drawing two nested rects */
    SDL_FRect outer = { dx - 1, dy - 1, (float)scaledTile + 2, (float)scaledTile + 2 };
    SDL_RenderRect(ed->renderer, &outer);
    SDL_FRect inner = { dx, dy, (float)scaledTile, (float)scaledTile };
    SDL_RenderRect(ed->renderer, &inner);
}

/* -------------------------------------------------------
 * Render pillbox range circles (9-tile radius from center)
 * ------------------------------------------------------- */
static void meRenderPillRanges(MapEditorState *ed, int screenW, int screenH) {
    if (!ed->showPillRanges) return;
    if (ed->pb->numPills == 0) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    /* 9-tile radius in pixels, scaled by zoom */
    float radiusPx = 9.0f * tileSize * zf;
    /* Offset to center of tile */
    float halfTile = tileSize * zf * 0.5f;

    SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ed->renderer, 255, 0, 0, 160);

    /* Draw circle as line segments */
    #define PILL_RANGE_SEGMENTS 64
    for (int i = 0; i < ed->pb->numPills; i++) {
        int px = ed->pb->item[i].x;
        int py = ed->pb->item[i].y;
        float cx = (float)(px * tileSize - camPX) * zf + halfTile;
        float cy = (float)(py * tileSize - camPY) * zf + halfTile;

        SDL_FPoint pts[PILL_RANGE_SEGMENTS + 1];
        for (int s = 0; s <= PILL_RANGE_SEGMENTS; s++) {
            float angle = (float)s * (2.0f * 3.14159265f / PILL_RANGE_SEGMENTS);
            pts[s].x = cx + radiusPx * SDL_cosf(angle);
            pts[s].y = cy + radiusPx * SDL_sinf(angle);
        }
        SDL_RenderLines(ed->renderer, pts, PILL_RANGE_SEGMENTS + 1);
    }
    #undef PILL_RANGE_SEGMENTS
}

static BYTE meComputeDirToLand(MapEditorState *ed, int sx, int sy);

/* Render placement preview or drag preview at (hoverMX, hoverMY). */
static void meRenderObjPreview(MapEditorState *ed, int screenW, int screenH,
                                int hoverMX, int hoverMY) {
    if (hoverMX < 0 || hoverMY < 0) return;

    int toolOrDrag = -1;
    BYTE previewTile = 0;
    int boatDir = -1; /* >= 0 means use boat atlas instead of mapViewPos */
    const char *validErr = NULL;

    if (ed->isDraggingObj) {
        /* Drag preview */
        if (ed->selectedObjKind == ME_SEL_BASE) {
            previewTile = BASE_NEUTRAL;
            validErr = meValidateBasePlacement(ed, hoverMX, hoverMY);
            /* Allow dropping on original position */
            if (hoverMX == ed->dragObjOrigX && hoverMY == ed->dragObjOrigY)
                validErr = NULL;
        } else if (ed->selectedObjKind == ME_SEL_PILL) {
            {
                static const BYTE pillTileForArmour[16] = {
                    PILL_EVIL_0,  PILL_EVIL_1,  PILL_EVIL_2,  PILL_EVIL_3,
                    PILL_EVIL_4,  PILL_EVIL_5,  PILL_EVIL_6,  PILL_EVIL_7,
                    PILL_EVIL_8,  PILL_EVIL_9,  PILL_EVIL_10, PILL_EVIL_11,
                    PILL_EVIL_12, PILL_EVIL_13, PILL_EVIL_14, PILL_EVIL_15
                };
                BYTE a = ed->pb->item[ed->selectedObjIndex].armour;
                previewTile = (a <= 15) ? pillTileForArmour[a] : PILL_EVIL_15;
            }
            validErr = meValidatePillPlacement(ed, hoverMX, hoverMY);
            if (hoverMX == ed->dragObjOrigX && hoverMY == ed->dragObjOrigY)
                validErr = NULL;
        } else if (ed->selectedObjKind == ME_SEL_START) {
            BYTE dir = ed->ss->item[ed->selectedObjIndex].dir;
            boatDir = startsConvertDir((dir < 16) ? dir : 0);
            validErr = meValidateStartPlacement(ed, hoverMX, hoverMY);
            if (hoverMX == ed->dragObjOrigX && hoverMY == ed->dragObjOrigY)
                validErr = NULL;
        } else {
            return;
        }
        toolOrDrag = 1;
    } else if (ed->activeTool == ME_TOOL_PLACE_BASE) {
        previewTile = BASE_NEUTRAL;
        validErr = meValidateBasePlacement(ed, hoverMX, hoverMY);
        toolOrDrag = 1;
    } else if (ed->activeTool == ME_TOOL_PLACE_PILL) {
        previewTile = PILL_EVIL_15;
        validErr = meValidatePillPlacement(ed, hoverMX, hoverMY);
        toolOrDrag = 1;
    } else if (ed->activeTool == ME_TOOL_PLACE_START) {
        BYTE previewDir = meComputeDirToLand(ed, hoverMX, hoverMY);
        boatDir = startsConvertDir(previewDir);
        validErr = meValidateStartPlacement(ed, hoverMX, hoverMY);
        toolOrDrag = 1;
    }

    if (toolOrDrag < 0) return;

    int zf = ed->zoomFactor;
    int tileSize = TILE_SIZE_X;
    int scaledTile = tileSize * zf;
    int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
    int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
    int camPX = centerPX - screenW / (2 * zf);
    int camPY = centerPY - screenH / (2 * zf);

    float dx = (float)(hoverMX * tileSize - camPX) * zf;
    float dy = (float)(hoverMY * tileSize - camPY) * zf;

    if (validErr) {
        SDL_SetTextureColorMod(ed->tilesTex, 255, 100, 100);
    }
    SDL_SetTextureAlphaMod(ed->tilesTex, 128);

    SDL_FRect src;
    if (boatDir >= 0) {
        src = meAtlasSrc(ed, meBoatAtlasX[boatDir], meBoatAtlasY[boatDir]);
    } else {
        src = meAtlasSrc(ed, mapViewPosX[previewTile],
                         mapViewPosY[previewTile]);
    }
    SDL_FRect dest = { dx, dy, (float)scaledTile, (float)scaledTile };
    SDL_RenderTexture(ed->renderer, ed->tilesTex, &src, &dest);

    SDL_SetTextureAlphaMod(ed->tilesTex, 255);
    if (validErr) {
        SDL_SetTextureColorMod(ed->tilesTex, 255, 255, 255);
    }
}

/* Check if click at (mx, my) hits an existing object.
 * Returns ME_SEL_NONE if no object hit, else sets *outKind and *outIndex. */
static bool meHitTestObject(MapEditorState *ed, int mx, int my,
                             int *outKind, int *outIndex) {
    /* Pillboxes first (rendered on top) */
    for (int i = 0; i < ed->pb->numPills; i++) {
        if (ed->pb->item[i].x == mx && ed->pb->item[i].y == my) {
            *outKind = ME_SEL_PILL;
            *outIndex = i;
            return true;
        }
    }
    /* Then bases */
    for (int i = 0; i < ed->bs->numBases; i++) {
        if (ed->bs->item[i].x == mx && ed->bs->item[i].y == my) {
            *outKind = ME_SEL_BASE;
            *outIndex = i;
            return true;
        }
    }
    /* Then starts */
    for (int i = 0; i < ed->ss->numStarts; i++) {
        if (ed->ss->item[i].x == mx && ed->ss->item[i].y == my) {
            *outKind = ME_SEL_START;
            *outIndex = i;
            return true;
        }
    }
    return false;
}

/* Delete the currently selected object. */
static void meDeleteSelectedObj(MapEditorState *ed) {
    if (ed->selectedObjKind == ME_SEL_NONE) return;

    undoBeginCommand(&ed->undoStack);
    int idx = ed->selectedObjIndex;

    if (ed->selectedObjKind == ME_SEL_BASE && idx < ed->bs->numBases) {
        undoRecordObjRemove(&ed->undoStack, OBJ_BASE, idx, &ed->bs->item[idx]);
        for (int j = idx; j < ed->bs->numBases - 1; j++)
            ed->bs->item[j] = ed->bs->item[j + 1];
        ed->bs->numBases--;
    } else if (ed->selectedObjKind == ME_SEL_PILL && idx < ed->pb->numPills) {
        undoRecordObjRemove(&ed->undoStack, OBJ_PILL, idx, &ed->pb->item[idx]);
        for (int j = idx; j < ed->pb->numPills - 1; j++)
            ed->pb->item[j] = ed->pb->item[j + 1];
        ed->pb->numPills--;
    } else if (ed->selectedObjKind == ME_SEL_START && idx < ed->ss->numStarts) {
        undoRecordObjRemove(&ed->undoStack, OBJ_START, idx, &ed->ss->item[idx]);
        for (int j = idx; j < ed->ss->numStarts - 1; j++)
            ed->ss->item[j] = ed->ss->item[j + 1];
        ed->ss->numStarts--;
    }

    undoEndCommand(&ed->undoStack);
    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;
    ed->tabCycleIndex = 0;
    ed->selectedObjKind = ME_SEL_NONE;
    ed->selectedObjIndex = -1;
}

/* -------------------------------------------------------
 * Action dispatch helpers — called from menu, keyboard, or
 * after an unsaved-changes modal resolves.
 * ------------------------------------------------------- */

/* Everything a New, an Open or an Exit would throw away: the map itself, the
 * script in the pane and the manifest behind the metadata, lobby and rules
 * forms. Every site that asks whether to prompt asks this, so all of them
 * count the same three. */
static bool meAnythingDirty(const MapEditorState *ed) {
    return ed->dirty || meScenarioDirty(&ed->scn) ||
           meScenarioFormDirty(&ed->scnForm);
}

/* Begin "New" — checks dirty flag, may open modal. */
static void meActionNew(MapEditorState *ed) {
    if (meAnythingDirty(ed)) {
        ed->deferredAction = FILE_OP_SAVE_THEN_NEW;
        /* The modal will be opened by the main loop via OpenPopup */
    } else {
        meDoNew(ed);
    }
}

/* Begin "Open" — checks dirty flag, may open modal. */
static void meActionOpen(MapEditorState *ed) {
    ed->pendingOpenPath[0] = '\0';
    if (meAnythingDirty(ed)) {
        ed->deferredAction = FILE_OP_SAVE_THEN_OPEN;
    } else {
        meShowOpenDialog(ed, FILE_OP_OPEN);
    }
}

#ifdef MAPEDITOR_WBN_OPEN
/* Begin "Open from WinBolo.net" — checks dirty flag, may open modal. */
static void meActionOpenWbn(MapEditorState *ed) {
    ed->pendingOpenPath[0] = '\0';
    if (meAnythingDirty(ed)) {
        ed->deferredAction = FILE_OP_SAVE_THEN_OPEN_WBN;
    } else {
        meWbnOpenShow();
    }
}
#endif /* MAPEDITOR_WBN_OPEN */

/* Begin "Save" — saves to current path or falls through to Save As. */
static void meActionSave(MapEditorState *ed) {
    if (ed->currentFilePath[0]) {
        meSaveToPath(ed, ed->currentFilePath);
    } else {
        meShowSaveDialog(ed, FILE_OP_SAVE_AS);
    }
}

/* Begin "Save As" — always shows file dialog. */
static void meActionSaveAs(MapEditorState *ed) {
    meShowSaveDialog(ed, FILE_OP_SAVE_AS);
}

/* Begin "Exit" — checks dirty flag, may open modal. */
static void meActionExit(MapEditorState *ed) {
    if (meAnythingDirty(ed)) {
        ed->deferredAction = FILE_OP_SAVE_THEN_EXIT;
    } else {
        ed->quit = true;
    }
}

/* What the scenario panel's tags view needs to know about the open map: how
 * many pills, bases and starts it holds, where each one sits, and the
 * rectangle the selection tool is holding, which is where a region's bounds
 * come from.
 *
 * The panel reads none of it for itself. pillsGetNumPills, basesGetNumBases
 * and startsGetNumStarts are bolo internals and mapeditor_scenario_imgui.cpp
 * has no reach into them, so this file — which has — copies out the numbers
 * and hands them over. Indices stay the editor's own, counting from 0; the
 * form module is what turns one into the manifest's 1-based entry. */
static void meScenarioFillMapInfo(MapEditorState *ed, MEScenarioMapInfo *info) {
    memset(info, 0, sizeof(*info));

    info->numPills = ed->pb->numPills;
    if (info->numPills > MAX_PILLS) info->numPills = MAX_PILLS;
    for (int i = 0; i < info->numPills; i++) {
        info->pillX[i] = ed->pb->item[i].x;
        info->pillY[i] = ed->pb->item[i].y;
    }

    info->numBases = ed->bs->numBases;
    if (info->numBases > MAX_BASES) info->numBases = MAX_BASES;
    for (int i = 0; i < info->numBases; i++) {
        info->baseX[i] = ed->bs->item[i].x;
        info->baseY[i] = ed->bs->item[i].y;
    }

    info->numStarts = ed->ss->numStarts;
    if (info->numStarts > MAX_STARTS) info->numStarts = MAX_STARTS;
    for (int i = 0; i < info->numStarts; i++) {
        info->startX[i] = ed->ss->item[i].x;
        info->startY[i] = ed->ss->item[i].y;
    }

    /* The selection is two corners in whichever order they were dragged; a
     * region is a top-left and a count of squares. */
    if (ed->hasSelection) {
        int x0 = ed->selX1 < ed->selX2 ? ed->selX1 : ed->selX2;
        int y0 = ed->selY1 < ed->selY2 ? ed->selY1 : ed->selY2;
        int x1 = ed->selX1 > ed->selX2 ? ed->selX1 : ed->selX2;
        int y1 = ed->selY1 > ed->selY2 ? ed->selY1 : ed->selY2;

        info->hasSelection = true;
        info->selX = x0;
        info->selY = y0;
        info->selW = x1 - x0 + 1;
        info->selH = y1 - y0 + 1;
    }
}

/* -------------------------------------------------------
 * Writing the scenario: on to the map, or out as a mod.
 * ------------------------------------------------------- */
/* What has to be true before either write, in the order -pack makes it true:
 * the script in the pane checks out, and the manifest that is about to be
 * written agrees with the table that script declares. The host fills the
 * scenario global from the manifest, runs the chunk and refuses the load when
 * the table left behind says something else, so a package written past this
 * check is one a server turns down when a round starts on it.
 *
 * A pane with nothing in it is not a script with a problem: there is no script
 * at all, and the manifest stands on its own — which is what a rules-only
 * scenario is. Anything with bytes in it is checked, and any issue is a
 * refusal, with the problems going to the list the script view draws.
 *
 * modPath NULL writes the chunk on to the open map; a path writes a mod
 * there. Nothing is written by either until both checks pass. */
static void meWriteScenario(MapEditorState *ed, const char *modPath) {
    ScenarioManifest toWrite;
    char             key[SCN_VALIDATE_KEY_LEN];
    char             err[ME_SCENARIO_PACK_ERR_LEN];
    char             line[256];
    bool             ok;

    /* A mod is written from a copy of the form's manifest — bound false, no
     * tags, no regions — and the comparison runs against that same copy, so a
     * script that declares bound = true is caught here rather than at load.
     *
     * The copy is made before the check, because the check pushes it: the
     * server pushes the manifest it loaded the package with, and pushing the
     * form's instead would hold a mod's script against a table the mod file
     * will not carry. */
    if (modPath != NULL) {
        meScenarioModManifest(&ed->scnForm.manifest, &toWrite);
    } else {
        toWrite = ed->scnForm.manifest;
    }

    if (ed->scn.scriptLen > 0 && ed->scn.script != NULL) {
        meScenarioCheckRun(&ed->scnCheck, ed->scn.script, ed->scn.scriptLen,
                           ed->scn.scriptPath[0] != '\0'
                               ? ed->scn.scriptPath
                               : ME_SCENARIO_CHECK_UNNAMED,
                           &toWrite);
        if (ed->scnCheck.result.count > 0) {
            snprintf(line, sizeof(line), "%s (%u)",
                     langGetText(STR_MAPEDIT_SCENARIO_PACK_ISSUES),
                     (unsigned)ed->scnCheck.result.count);
            meScenarioSetStatus(&ed->scn, line);
            return;
        }
    } else {
        meScenarioCheckClear(&ed->scnCheck);
    }

    if (ed->scnCheck.hasRun && ed->scnCheck.result.haveManifest &&
        !meScenarioManifestAgrees(&toWrite, &ed->scnCheck.result.manifest, key,
                                  sizeof(key), err, sizeof(err))) {
        snprintf(line, sizeof(line), "%s: %s — %s",
                 langGetText(STR_MAPEDIT_SCENARIO_PACK_CONFLICT), key, err);
        meScenarioSetStatus(&ed->scn, line);
        return;
    }

    if (modPath != NULL) {
        ok = meScenarioWriteMod(&toWrite, ed->scn.script, ed->scn.scriptLen,
                                modPath, err, sizeof(err));
    } else {
        ok = meScenarioPackIntoMap(&toWrite, ed->scn.script, ed->scn.scriptLen,
                                   ed->currentFilePath, err, sizeof(err));
    }

    if (!ok) {
        snprintf(line, sizeof(line), "%s: %s",
                 langGetText(STR_MAPEDIT_SCENARIO_PACK_FAILED), err);
        meScenarioSetStatus(&ed->scn, line);
        return;
    }

    if (modPath != NULL) {
        meScenarioSetStatus(&ed->scn,
                            langGetText(STR_MAPEDIT_SCENARIO_MOD_SAVED));
    } else {
        /* The manifest is on the map now, so the form is no longer ahead of
         * anything. The line says the loose script still wins, because an
         * author who packs and then tests would otherwise be running the file
         * beside the map without being told. */
        ed->scnForm.dirty = false;
        /* What is on the map now is what a later map save has to put back,
         * rather than the container the file was opened with. */
        meScenarioPackedSet(&ed->scnPacked, &toWrite, ed->scn.script,
                            ed->scn.scriptLen);
        meScenarioSetStatus(&ed->scn, langGetText(STR_MAPEDIT_SCENARIO_PACKED));
    }
}

/* The save behind the unsaved-changes prompt's Yes: the map to path, and then
 * the forms' manifest on to it when the author has edits in them.
 *
 * The pack runs after meSaveToPath has returned rather than inside it, because
 * mapWrite truncates the file and meScenarioPackedRestore then puts the
 * container the map came with back on the end; a pack from inside would be the
 * one written over. meWriteScenario says on the panel's status line what it did
 * and clears the form only when the write went through, so a form still dirty
 * afterwards is a refusal. That is a save that did not finish: the caller drops
 * the New, Open or Exit it was saving for, and the reason goes to the error
 * popup the way a script that would not write does. */
static bool meSaveAndPackForm(MapEditorState *ed, const char *path) {
    if (!meSaveToPath(ed, path)) return false;
    if (!meScenarioFormDirty(&ed->scnForm)) return true;

    meWriteScenario(ed, NULL);
    if (meScenarioFormDirty(&ed->scnForm)) {
        snprintf(ed->errorMessage, sizeof(ed->errorMessage), "%s\n%s",
                 ed->scn.status, path);
        return false;
    }
    return true;
}

/* Process a completed file dialog result. */
static void meHandleFileDialogResult(MapEditorState *ed) {
    ed->fileDialogGotResult = false;
    const char *path = ed->fileDialogResult;
    FileOp op = ed->pendingFileOp;
    ed->pendingFileOp = FILE_OP_NONE;

    if (path[0] == '\0') {
        /* User cancelled the dialog */
        return;
    }

    switch (op) {
    case FILE_OP_OPEN:
        meLoadFromPath(ed, path);
        break;
    case FILE_OP_SAVE_AS:
        meSaveToPath(ed, path);
        break;
    case FILE_OP_SAVE_THEN_NEW:
        if (meSaveAndPackForm(ed, path)) {
            meDoNew(ed);
        }
        break;
    case FILE_OP_SAVE_THEN_OPEN:
        if (meSaveAndPackForm(ed, path)) {
            meShowOpenDialog(ed, FILE_OP_OPEN);
        }
        break;
#ifdef MAPEDITOR_WBN_OPEN
    case FILE_OP_SAVE_THEN_OPEN_WBN:
        if (meSaveAndPackForm(ed, path)) {
            meWbnOpenShow();
        }
        break;
#endif
    case FILE_OP_SAVE_THEN_EXIT:
        if (meSaveAndPackForm(ed, path)) {
            ed->quit = true;
        }
        break;
    case FILE_OP_SAVE_SCENARIO_MOD:
        /* A mod is a file of its own and touches neither the map nor the
         * script beside it. */
        meWriteScenario(ed, path);
        break;
    default:
        break;
    }
}

/* Handle the unsaved-changes modal result.
 * choice: 1=Yes(save), 2=No(discard), 3=Cancel */
static void meHandleUnsavedChoice(MapEditorState *ed, int choice) {
    FileOp action = ed->deferredAction;
    ed->deferredAction = FILE_OP_NONE;

    if (choice == 3) {
        /* Cancel — do nothing.  The exit this modal was asked about is off,
         * so the quit that may have started it is off with it. */
        if (action == FILE_OP_SAVE_THEN_EXIT) ed->exitIsAppQuit = false;
        return;
    }

    if (choice == 1) {
        /* Yes — save first */
        if (ed->currentFilePath[0]) {
            /* The map and, when the forms hold edits, the manifest with it. */
            if (!meSaveAndPackForm(ed, ed->currentFilePath)) return;
            /* Save succeeded, now do the deferred action */
        } else {
            /* No current path — need Save As dialog, keep deferred action */
            meShowSaveDialog(ed, action);
            return;
        }
    }

    /* Either No (discard) or Yes (save succeeded) — do the deferred action */
    switch (action) {
    case FILE_OP_SAVE_THEN_NEW:
        meDoNew(ed);
        break;
    case FILE_OP_SAVE_THEN_OPEN:
        if (ed->pendingOpenPath[0]) {
            meLoadFromPath(ed, ed->pendingOpenPath);
            ed->pendingOpenPath[0] = '\0';
        } else {
            meShowOpenDialog(ed, FILE_OP_OPEN);
        }
        break;
#ifdef MAPEDITOR_WBN_OPEN
    case FILE_OP_SAVE_THEN_OPEN_WBN:
        meWbnOpenShow();
        break;
#endif
    case FILE_OP_SAVE_THEN_EXIT:
        ed->quit = true;
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------
 * Center map contents — shift all tiles and objects so
 * the bounding box of content is centered around (128,128).
 * ------------------------------------------------------- */
/*---------------------------------------------------------
 * meComputeDirToLand
 *   Compute Bolo-format direction from (sx,sy) toward nearest land mass.
 *---------------------------------------------------------*/
static BYTE meComputeDirToLand(MapEditorState *ed, int sx, int sy) {
    #define CDL_RADIUS 30
    float totalX = 0, totalY = 0;
    int count = 0;

    for (int dx = -CDL_RADIUS; dx <= CDL_RADIUS; dx++) {
        int nx = sx + dx;
        if (nx < 0 || nx > 255) continue;
        for (int dy = -CDL_RADIUS; dy <= CDL_RADIUS; dy++) {
            int ny = sy + dy;
            if (ny < 0 || ny > 255) continue;
            if (dx * dx + dy * dy > CDL_RADIUS * CDL_RADIUS) continue;
            if (ed->mp->mapItem[nx][ny] != DEEP_SEA) {
                totalX += (float)nx;
                totalY += (float)ny;
                count++;
            }
        }
    }
    #undef CDL_RADIUS

    if (count > 0) {
        float cx = totalX / (float)count;
        float cy = totalY / (float)count;
        float fdx = cx - (float)sx;
        float fdy = cy - (float)sy;
        if (fdx != 0.0f || fdy != 0.0f) {
            float angle = atan2f(fdx, -fdy);
            if (angle < 0.0f) angle += 6.283185307f;
            int dir = (int)(angle / 6.283185307f * 16.0f + 0.5f) % 16;
            return startsConvertDir((BYTE)dir);
        }
    }
    return 0; /* default: East in Bolo format */
}

/*---------------------------------------------------------
 * mePointStartsToLand
 *   For each start, compute the direction toward the nearest
 *   large land mass by finding the center-of-mass of all
 *   non-deep-sea tiles within a search radius.
 *---------------------------------------------------------*/
static void mePointStartsToLand(MapEditorState *ed) {
    if (ed->ss->numStarts == 0) {
        meSetStatus(ed, "No starts placed");
        return;
    }

    for (int i = 0; i < ed->ss->numStarts; i++) {
        ed->ss->item[i].dir = meComputeDirToLand(ed, ed->ss->item[i].x, ed->ss->item[i].y);
    }
    meSetStatus(ed, "Pointed %d starts toward land", ed->ss->numStarts);
}

static void meCenterMapContents(MapEditorState *ed) {
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    bool found = false;

    /* Scan tiles for non-deep-sea content (skip mine border zone) */
    for (int x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (int y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
            BYTE t = ed->mp->mapItem[x][y];
            if (t != DEEP_SEA) {
                if (x < minX) minX = x;
                if (y < minY) minY = y;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
                found = true;
            }
        }
    }

    /* Include objects in bounding box */
    for (int i = 0; i < ed->pb->numPills; i++) {
        int x = ed->pb->item[i].x, y = ed->pb->item[i].y;
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        found = true;
    }
    for (int i = 0; i < ed->bs->numBases; i++) {
        int x = ed->bs->item[i].x, y = ed->bs->item[i].y;
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        found = true;
    }
    for (int i = 0; i < ed->ss->numStarts; i++) {
        int x = ed->ss->item[i].x, y = ed->ss->item[i].y;
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        found = true;
    }

    if (!found) {
        meSetStatus(ed, "Nothing to center — map is empty");
        return;
    }

    /* Compute offset to center the bounding box around (128,128) */
    int contentCX = (minX + maxX) / 2;
    int contentCY = (minY + maxY) / 2;
    int dx = 128 - contentCX;
    int dy = 128 - contentCY;

    if (dx == 0 && dy == 0) {
        meSetStatus(ed, "Map content is already centered");
        return;
    }

    /* Check that shifted content stays within the playable area */
    int newMinX = minX + dx, newMinY = minY + dy;
    int newMaxX = maxX + dx, newMaxY = maxY + dy;
    if (newMinX <= MAP_MINE_EDGE_LEFT || newMaxX >= MAP_MINE_EDGE_RIGHT ||
        newMinY <= MAP_MINE_EDGE_TOP  || newMaxY >= MAP_MINE_EDGE_BOTTOM) {
        meSetStatus(ed, "Cannot center — content would exceed playable area");
        return;
    }

    /* Build shifted tile map in a temp buffer */
    BYTE (*temp)[MAP_ARRAY_SIZE] = malloc(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
    if (!temp) {
        meSetStatus(ed, "Center failed — out of memory");
        return;
    }
    for (int x = 0; x < MAP_ARRAY_SIZE; x++)
        for (int y = 0; y < MAP_ARRAY_SIZE; y++)
            temp[x][y] = DEEP_SEA;

    /* Preserve mine border */
    for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
            if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                temp[x][y] = ed->mp->mapItem[x][y];
            }
        }
    }

    /* Copy shifted content */
    for (int x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (int y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
            BYTE t = ed->mp->mapItem[x][y];
            if (t != DEEP_SEA) {
                temp[x + dx][y + dy] = t;
            }
        }
    }

    /* Record undo — record every tile that differs between old and new */
    undoBeginCommand(&ed->undoStack);
    for (int x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
        for (int y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
            if (temp[x][y] != ed->mp->mapItem[x][y]) {
                undoRecordTile(&ed->undoStack, ed->mp, (BYTE)x, (BYTE)y, temp[x][y]);
            }
        }
    }

    /* Apply the new tile data */
    memcpy(ed->mp->mapItem, temp, MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
    free(temp);

    /* Record old object states, shift, then record new states */
    for (int i = 0; i < ed->pb->numPills; i++) {
        pillbox old = ed->pb->item[i];
        ed->pb->item[i].x = (BYTE)(old.x + dx);
        ed->pb->item[i].y = (BYTE)(old.y + dy);
        undoRecordObjModify(&ed->undoStack, OBJ_PILL, i, &old, &ed->pb->item[i]);
    }
    for (int i = 0; i < ed->bs->numBases; i++) {
        base old = ed->bs->item[i];
        ed->bs->item[i].x = (BYTE)(old.x + dx);
        ed->bs->item[i].y = (BYTE)(old.y + dy);
        undoRecordObjModify(&ed->undoStack, OBJ_BASE, i, &old, &ed->bs->item[i]);
    }
    for (int i = 0; i < ed->ss->numStarts; i++) {
        start old = ed->ss->item[i];
        ed->ss->item[i].x = (BYTE)(old.x + dx);
        ed->ss->item[i].y = (BYTE)(old.y + dy);
        undoRecordObjModify(&ed->undoStack, OBJ_START, i, &old, &ed->ss->item[i]);
    }
    undoEndCommand(&ed->undoStack);

    ed->dirty = true;
    ed->minimapDirty = true;
    ed->statsDirty = true;

    /* Move camera to new center */
    ed->viewCenterX = 128 << 8;
    ed->viewCenterY = 128 << 8;

    meSetStatus(ed, "Centered map (shifted %+d, %+d)", dx, dy);
}

/* -------------------------------------------------------
 * Main editor loop
 * ------------------------------------------------------- */
void mapEditorRun(SDL_Window *window, SDL_Renderer *renderer, const char *mapPath, bool fromMainMenu) {
    MapEditorState *ed = calloc(1, sizeof(*ed));
    ed->window = window;
    ed->renderer = renderer;
    ed->fromMainMenu = fromMainMenu;

    {
        int winW = 0, winH = 0, outW = 0, outH = 0;
        SDL_GetWindowSize(window, &winW, &winH);
        SDL_GetRenderOutputSize(renderer, &outW, &outH);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
            "mapEditorRun: window %dx%d pt, render output %dx%d px, density %.2f",
            winW, winH, outW, outH, SDL_GetWindowPixelDensity(window));
    }

    ed->zoomFactor = 2;
    ed->zoomStepIndex = ZOOM_STEP_1X + 1;  /* 2x = index 6 */
    ed->zoomLevel = 2.0f;
    ed->offscreenTex = NULL;
    ed->quit = false;
    ed->exitIsAppQuit = false;
    s_appQuitRequested = FALSE;
    ed->showMines = true;
    ed->showGrid = false;
    ed->showPillRanges = false;
    ed->selectedObjKind = ME_SEL_NONE;
    ed->selectedObjIndex = -1;
    ed->showTerrain = true;
    ed->showTools = true;
    ed->showInspector = true;
    ed->showObjects = true;
    ed->showOverview = true;
    ed->mazeToolConfig = mazeDefaultConfig();
    ed->textConfig = textDefaultConfig();
    ed->fontListReady = false;
    ed->textFontSource = 0;
    ed->textFontFamily = 0;
    ed->textFontStyleIdx = 0;
    ed->textTTFPixelSize = 24;
    ed->imageImportCfg.numColors = 10;
    ed->exportCfg.mode = ME_EXPORT_PREVIEW;
    ed->exportCfg.previewSize = ME_PREVIEW_512;
    ed->exportCfg.showObjects = true;
    ed->exportCfg.showMines = true;
    ed->exportCfg.showGrid = false;
    ed->brushSeen = calloc(256 * 256, sizeof(bool));
    meScenarioInit(&ed->scn);
    meScenarioFormInit(&ed->scnForm);
    meScenarioCheckInit(&ed->scnCheck);
    meScenarioPackedInit(&ed->scnPacked);
    ed->scnView = ME_SCENARIO_VIEW_SCRIPT;

    /* Stamp library */
    {
        const char *base = SDL_GetBasePath();
        char dataDir[512];
        snprintf(dataDir, sizeof(dataDir), "%sdata", base ? base : "");
        stampLibraryInit(&ed->stampLib, dataDir);
    }

    /* Center camera at tile (128, 128) */
    ed->viewCenterX = 128 << 8;
    ed->viewCenterY = 128 << 8;

    /* Initialize map view lookup tables */
    mapViewInit();

    /* Build the tile atlas at 1x. The frame loop re-rasterizes it to match the
       zoom and pixel density (meEnsureAtlas); this just guarantees there is an
       atlas before the first frame, and gives the failure an exit path. */
    ed->sheetScale = 1;
    SDL_Surface *sheet = tileLoaderBuildSheet(TILE_SIZE_X);
    if (!sheet) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET, "mapEditorRun: tileLoaderBuildSheet failed");
        free(ed);
        return;
    }
    ed->tilesTex = SDL_CreateTextureFromSurface(renderer, sheet);
    SDL_DestroySurface(sheet);
    if (!ed->tilesTex) {
        WB_LOG_ERROR(WB_LOG_CAT_ASSET, "mapEditorRun: SDL_CreateTextureFromSurface failed");
        free(ed);
        return;
    }
    SDL_SetTextureScaleMode(ed->tilesTex, SDL_SCALEMODE_NEAREST);

    /* Load recent files from INI */
    meLoadRecentFiles(ed);

    /* Load or create map */
    if (mapPath) {
        mapCreate(&ed->mp);
        pillsCreate(&ed->pb);
        basesCreate(&ed->bs);
        startsCreate(&ed->ss);
        if (!mapRead((char *)mapPath, &ed->mp, &ed->pb, &ed->bs, &ed->ss)) {
            WB_LOG_WARN(WB_LOG_CAT_MAP, "mapEditorRun: failed to load map '%s', creating blank", mapPath);
            mapDestroy(&ed->mp);
            pillsDestroy(&ed->pb);
            basesDestroy(&ed->bs);
            startsDestroy(&ed->ss);
            meCreateBlankMap(ed);
        } else {
            SDL_strlcpy(ed->currentFilePath, mapPath, ME_PATH_MAX);
            meAddRecentFile(ed, mapPath);
        }
    } else {
        meCreateBlankMap(ed);
    }
    /* Whatever the editor opened with, the script that goes with it. A blank
     * map leaves currentFilePath empty, which clears the pane. */
    meScenarioSetMap(&ed->scn, ed->currentFilePath);
    meScenarioFormReset(&ed->scnForm);
    meScenarioCheckClear(&ed->scnCheck);
    /* Whatever the last map carried is not this map's. A map that opens with
     * no container leaves this empty, so its save writes none. */
    meScenarioPackedClear(&ed->scnPacked);
    meLoadPackedScenario(ed, ed->currentFilePath);
    meUpdateWindowTitle(ed);

    /* Create minimap texture */
    ed->minimapTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, 256, 256);
    ed->minimapDirty = true;
    ed->statsDirty = true;

    /* Initialize ImGui */
    mapEditorImguiInit(window, renderer);

#ifdef __APPLE__
    /* Install the native NSMenu after ImGui + window are up. The shim
     * saves any previously-installed mainMenu so embedded entry from
     * WinBolo (which has its own NSMenu installed) restores cleanly on
     * exit. Standalone has no prior mainMenu — the shim builds a
     * "Map Editor" app menu with Quit/Hide instead. */
    me_mac_menubar_install(window);
#endif

    /* macOS trackpad pinch-to-zoom */
    macOSPinchZoomInit();

    /* Track whether the unsaved-changes modal needs to be opened */
    bool openUnsavedModal = false;

    /* Main loop */
    const bool *keystate = SDL_GetKeyboardState(NULL);

    while (!ed->quit) {
        /* Canvas coordinates are renderer pixels; SDL reports mouse positions
           in window points. With SDL_WINDOW_HIGH_PIXEL_DENSITY those differ by
           the window's pixel density (2 on a Retina display), so convert every
           pointer coordinate at the boundary and take the viewport from the
           renderer rather than the window. Re-read per frame: dragging the
           window to a display with a different density changes it. */
        float pxScale = SDL_GetWindowPixelDensity(window);
        if (pxScale <= 0.0f) pxScale = 1.0f;

        /* Fold the density into the magnification so a tile keeps its physical
           size and gains pixels rather than being stretched. Set before the
           events so anything that hit-tests this frame sees it; the atlas that
           matches is rasterized below, after any zoom change has landed. */
        ed->zoomFactor = meRenderZoom(ed, pxScale);

        /* Process events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            mapEditorImguiProcessEvent(&ev);

            switch (ev.type) {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                /* Embedded, this is WinBolo's window the editor is drawing
                 * into; a close request meant for anything else is not ours. */
                if (ev.window.windowID != SDL_GetWindowID(ed->window)) break;
                /* fall through */
            case SDL_EVENT_QUIT:
                /* Cmd+Q, Alt+F4, the close box.  All of them end the
                 * application, embedded as well as standalone — the way back
                 * to WinBolo is File > Return to Menu, or Escape. */
                ed->exitIsAppQuit = true;
                if (meAnythingDirty(ed)) {
                    ed->deferredAction = FILE_OP_SAVE_THEN_EXIT;
                    openUnsavedModal = true;
                } else {
                    ed->quit = true;
                }
                break;

            case SDL_EVENT_KEY_DOWN: {
                if (ev.key.repeat) break;
                if (mapEditorImguiWantKeyboard()) break;

                SDL_Keymod mod = SDL_GetModState();
                bool ctrl = (mod & SDL_KMOD_CTRL) != 0;
                bool shift = (mod & SDL_KMOD_SHIFT) != 0;

                if (ctrl) {
                    switch (ev.key.key) {
                    case SDLK_N:
                        meActionNew(ed);
                        if (ed->deferredAction) openUnsavedModal = true;
                        break;
                    case SDLK_O:
                        meActionOpen(ed);
                        if (ed->deferredAction) openUnsavedModal = true;
                        break;
                    case SDLK_S:
                        if (shift) meActionSaveAs(ed);
                        else       meActionSave(ed);
                        break;
                    case SDLK_C:
                        meCopySelection(ed);
                        break;
                    case SDLK_X:
                        meCutSelection(ed);
                        break;
                    case SDLK_V:
                        if (shift) {
                            /* Ctrl+Shift+V = Validate */
                            meRunValidation(ed);
                        } else if (ed->hasClipboard) {
                            ed->isPasting = true;
                        }
                        break;
                    case SDLK_Z:
                        if (shift) {
                            /* Ctrl+Shift+Z = redo */
                            if (redoApply(&ed->undoStack, ed->mp, ed->pb, ed->bs, ed->ss)) {
                                ed->dirty = (ed->undoStack.count != ed->undoStack.savedCommandIndex);
                                ed->minimapDirty = true;
                                ed->statsDirty = true;
                                ed->selectedObjKind = ME_SEL_NONE;
                                ed->selectedObjIndex = -1;
                                meUpdateWindowTitle(ed);
                            }
                        } else {
                            /* Ctrl+Z = undo */
                            if (undoApply(&ed->undoStack, ed->mp, ed->pb, ed->bs, ed->ss)) {
                                ed->dirty = (ed->undoStack.count != ed->undoStack.savedCommandIndex);
                                ed->minimapDirty = true;
                                ed->statsDirty = true;
                                ed->selectedObjKind = ME_SEL_NONE;
                                ed->selectedObjIndex = -1;
                                meUpdateWindowTitle(ed);
                            }
                        }
                        break;
                    case SDLK_Y:
                        /* Ctrl+Y = redo */
                        if (redoApply(&ed->undoStack, ed->mp, ed->pb, ed->bs, ed->ss)) {
                            ed->dirty = (ed->undoStack.count != ed->undoStack.savedCommandIndex);
                            ed->minimapDirty = true;
                            ed->statsDirty = true;
                            ed->selectedObjKind = ME_SEL_NONE;
                            ed->selectedObjIndex = -1;
                            meUpdateWindowTitle(ed);
                        }
                        break;
                    case SDLK_G:
                        /* Ctrl+G = Go To Coordinates */
                        ed->openGotoDialog = true;
                        break;
                    case SDLK_Q:
                        if (!ed->fromMainMenu) {
                            ed->exitIsAppQuit = true;
                            meActionExit(ed);
                            if (ed->deferredAction) openUnsavedModal = true;
                        }
                        break;
                    /* Ctrl+1-8 = toggle panel visibility */
                    case SDLK_1: ed->showTerrain    = !ed->showTerrain;    break;
                    case SDLK_2: ed->showTools      = !ed->showTools;      break;
                    case SDLK_3: ed->showInspector  = !ed->showInspector;  break;
                    case SDLK_4: ed->showObjects    = !ed->showObjects;    break;
                    case SDLK_5: ed->showOverview     = !ed->showOverview;     break;
                    case SDLK_6: ed->showStampLibrary = !ed->showStampLibrary; break;
                    case SDLK_7: ed->showStatsPanel   = !ed->showStatsPanel;   break;
                    case SDLK_8: ed->showScenario     = !ed->showScenario;     break;
                    default:
                        break;
                    }
                    break;
                }

                switch (ev.key.key) {
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    if (ed->zoomStepIndex < (int)ZOOM_STEP_COUNT - 1) ed->zoomStepIndex++;
                    ed->zoomLevel = zoomSteps[ed->zoomStepIndex];
                    ed->zoomFactor = meRenderZoom(ed, pxScale);
                    break;
                case SDLK_LEFTBRACKET:
                    if (ed->brushSize > 0) ed->brushSize--;
                    break;
                case SDLK_RIGHTBRACKET:
                    if (ed->brushSize < ME_BRUSH_SIZE_COUNT - 1) ed->brushSize++;
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    if (ed->zoomStepIndex > 0) ed->zoomStepIndex--;
                    ed->zoomLevel = zoomSteps[ed->zoomStepIndex];
                    ed->zoomFactor = meRenderZoom(ed, pxScale);
                    break;
                case SDLK_HOME:
                    ed->viewCenterX = 128 << 8;
                    ed->viewCenterY = 128 << 8;
                    break;
                case SDLK_ESCAPE:
                    if (ed->isPasting) {
                        ed->isPasting = false;
                    } else if (ed->hasSelection) {
                        ed->hasSelection = false;
                        ed->hasSelMask = false;
                    } else if (ed->fromMainMenu) {
                        ed->exitIsAppQuit = false;
                        meActionExit(ed);
                        if (ed->deferredAction) openUnsavedModal = true;
                    }
                    break;
                case SDLK_G:
                    if (shift) {
                        ed->activeTool = ME_TOOL_GENERATE;
                    } else {
                        ed->showGrid = !ed->showGrid;
                    }
                    break;
                case SDLK_M:
                    if (shift) {
                        ed->showMines = !ed->showMines;
                    } else {
                        ed->activeTool = ME_TOOL_MAZE;
                    }
                    break;
                case SDLK_N: ed->mineMode = !ed->mineMode; break;
                /* Terrain tool shortcuts */
                case SDLK_P: ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_L: ed->activeTool = ME_TOOL_LINE; break;
                case SDLK_R: ed->activeTool = shift ? ME_TOOL_RECT_FILL : ME_TOOL_RECT; break;
                case SDLK_O: ed->activeTool = shift ? ME_TOOL_OVAL_FILL : ME_TOOL_OVAL; break;
                case SDLK_F: ed->activeTool = ME_TOOL_FILL; break;
                case SDLK_S: ed->activeTool = ME_TOOL_SELECT; break;
                case SDLK_W: ed->activeTool = ME_TOOL_WAND; break;
                /* Object placement tool shortcuts */
                case SDLK_B: ed->activeTool = ME_TOOL_PLACE_BASE; break;
                case SDLK_I: ed->activeTool = ME_TOOL_PLACE_PILL; break;
                case SDLK_T: ed->activeTool = ME_TOOL_PLACE_START; break;
                /* Terrain shortcuts — ergonomic order, also switch to pencil */
                /* 1=Grass, 2=Forest, 3=Road, 4=Building, 5=River,
                   6=Swamp, 7=Crater, 8=Rubble, 9=HalfBuilding, 0=DeepSea, `=Boat */
                case SDLK_1: ed->activeTerrainIdx = 1; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_2: ed->activeTerrainIdx = 2; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_3: ed->activeTerrainIdx = 3; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_4: ed->activeTerrainIdx = 4; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_5: ed->activeTerrainIdx = 6; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_6: ed->activeTerrainIdx = 7; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_7: ed->activeTerrainIdx = 8; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_8: ed->activeTerrainIdx = 9; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_9: ed->activeTerrainIdx = 5; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_0: ed->activeTerrainIdx = 0; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_GRAVE: ed->activeTerrainIdx = 10; ed->mineMode = false; if (ed->activeTool >= ME_TOOL_PLACE_BASE) ed->activeTool = ME_TOOL_PENCIL; break;
                case SDLK_DELETE:
                case SDLK_BACKSPACE:
                    meDeleteSelectedObj(ed);
                    break;
                case SDLK_TAB: {
                    int total = ed->bs->numBases + ed->pb->numPills + ed->ss->numStarts;
                    if (total > 0) {
                        if (shift) {
                            ed->tabCycleIndex = (ed->tabCycleIndex - 1 + total) % total;
                        } else {
                            ed->tabCycleIndex = (ed->tabCycleIndex + 1) % total;
                        }
                        int idx = ed->tabCycleIndex;
                        if (idx < ed->bs->numBases) {
                            ed->selectedObjKind = ME_SEL_BASE;
                            ed->selectedObjIndex = idx;
                            ed->viewCenterX = (WORLD)(ed->bs->item[idx].x << 8);
                            ed->viewCenterY = (WORLD)(ed->bs->item[idx].y << 8);
                        } else if (idx < ed->bs->numBases + ed->pb->numPills) {
                            int pi = idx - ed->bs->numBases;
                            ed->selectedObjKind = ME_SEL_PILL;
                            ed->selectedObjIndex = pi;
                            ed->viewCenterX = (WORLD)(ed->pb->item[pi].x << 8);
                            ed->viewCenterY = (WORLD)(ed->pb->item[pi].y << 8);
                        } else {
                            int si = idx - ed->bs->numBases - ed->pb->numPills;
                            ed->selectedObjKind = ME_SEL_START;
                            ed->selectedObjIndex = si;
                            ed->viewCenterX = (WORLD)(ed->ss->item[si].x << 8);
                            ed->viewCenterY = (WORLD)(ed->ss->item[si].y << 8);
                        }
                        ed->minimapDirty = true;
                        ed->statsDirty = true;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION: {
                float motionX = ev.motion.x * pxScale;
                float motionY = ev.motion.y * pxScale;
                ed->mouseX = motionX;
                ed->mouseY = motionY;

                /* Tool-specific mouse motion handling */
                if (!mapEditorImguiWantMouse()) {
                    int screenW2, screenH2;
                    SDL_GetRenderOutputSize(renderer, &screenW2, &screenH2);
                    int mx, my;
                    if (meScreenToMap(ed, motionX, motionY, screenW2, screenH2, &mx, &my)) {
                        /* Pencil: paint along drag using Bresenham */
                        if (ed->isPainting && ed->activeTool == ME_TOOL_PENCIL) {
                            if (ed->brushSize > 0)
                                memset(ed->brushSeen, 0, 256 * 256 * sizeof(bool));
                            meBresenhamLine(ed->paintLastX, ed->paintLastY, mx, my,
                                            meBrushPaintCallback, ed);
                            ed->paintLastX = mx;
                            ed->paintLastY = my;
                        }
                        /* Shape tools: update preview */
                        if (ed->isDrawing) {
                            ed->drawCurX = mx;
                            ed->drawCurY = my;
                            switch (ed->activeTool) {
                            case ME_TOOL_LINE: meBuildLinePreview(ed); break;
                            case ME_TOOL_RECT:
                            case ME_TOOL_RECT_FILL: meBuildRectPreview(ed); break;
                            case ME_TOOL_OVAL:
                            case ME_TOOL_OVAL_FILL: meBuildOvalPreview(ed); break;
                            case ME_TOOL_MAZE: {
                                /* Compute drag rectangle */
                                int x0 = ed->mazeDragOriginX < mx ? ed->mazeDragOriginX : mx;
                                int y0 = ed->mazeDragOriginY < my ? ed->mazeDragOriginY : my;
                                int x1 = ed->mazeDragOriginX > mx ? ed->mazeDragOriginX : mx;
                                int y1 = ed->mazeDragOriginY > my ? ed->mazeDragOriginY : my;
                                ed->mazePreviewCount = mazeGeneratePreview(
                                    x0, y0, x1, y1,
                                    ed->mazePreviewX, ed->mazePreviewY,
                                    ed->mazePreviewTerrain,
                                    ME_FILL_LIMIT, &ed->mazeToolConfig);
                                break;
                            }
                            case ME_TOOL_GENERATE: {
                                /* Compute drag rectangle */
                                int x0 = ed->genDragOriginX < mx ? ed->genDragOriginX : mx;
                                int y0 = ed->genDragOriginY < my ? ed->genDragOriginY : my;
                                int x1 = ed->genDragOriginX > mx ? ed->genDragOriginX : mx;
                                int y1 = ed->genDragOriginY > my ? ed->genDragOriginY : my;
                                meBuildGenPreview(ed, x0, y0, x1, y1);
                                break;
                            }
                            }
                        }
                        /* Selection: update drag rect */
                        if (ed->isSelecting) {
                            ed->selX2 = mx;
                            ed->selY2 = my;
                        }
                    }
                }

                if (ed->rightDown) ed->rightDragged = true;
                if (ed->dragging) {
                    float dx = motionX - ed->dragLastX;
                    float dy = motionY - ed->dragLastY;
                    ed->dragLastX = motionX;
                    ed->dragLastY = motionY;
                    float tilePixelsF = meScreenTilePixels(ed);
                    int wmoveX = (int)(dx * 256.0f / tilePixelsF);
                    int wmoveY = (int)(dy * 256.0f / tilePixelsF);
                    int cx = (int)ed->viewCenterX - wmoveX;
                    int cy = (int)ed->viewCenterY - wmoveY;
                    if (cx < 0) cx = 0;
                    if (cy < 0) cy = 0;
                    if (cx > 65535) cx = 65535;
                    if (cy > 65535) cy = 65535;
                    ed->viewCenterX = (WORLD)cx;
                    ed->viewCenterY = (WORLD)cy;
                }
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                if (mapEditorImguiWantMouse()) break;
                if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    ed->dragging = true;
                    ed->dragLastX = ev.button.x * pxScale;
                    ed->dragLastY = ev.button.y * pxScale;
                    break;
                }
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    ed->rightDown = true;
                    ed->rightDragged = false;
                    ed->dragging = true;
                    ed->dragLastX = ev.button.x * pxScale;
                    ed->dragLastY = ev.button.y * pxScale;
                    break;
                }
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int screenW2, screenH2;
                    SDL_GetRenderOutputSize(renderer, &screenW2, &screenH2);
                    int mx, my;
                    if (!meScreenToMap(ed, ev.button.x * pxScale, ev.button.y * pxScale,
                                       screenW2, screenH2, &mx, &my))
                        break;

                    /* Handle paste mode click */
                    if (ed->isPasting) {
                        int clipW = ed->clipW;
                        int clipH = ed->clipH;
                        meCommitPaste(ed, mx, my);
                        ed->hasSelection = true;
                        ed->hasSelMask = false;
                        ed->selX1 = mx;
                        ed->selY1 = my;
                        ed->selX2 = mx + clipW - 1;
                        ed->selY2 = my + clipH - 1;
                        break;
                    }

                    /* Check for object hit (selection/dragging) — takes priority over terrain tools */
                    int hitKind, hitIndex;
                    bool hitObj = meHitTestObject(ed, mx, my, &hitKind, &hitIndex);

                    if (hitObj) {
                        /* If clicking on already-selected object, start drag */
                        if (ed->selectedObjKind == hitKind && ed->selectedObjIndex == hitIndex) {
                            ed->isDraggingObj = true;
                            ed->dragObjOrigX = mx;
                            ed->dragObjOrigY = my;
                        } else {
                            /* Select the object */
                            ed->selectedObjKind = hitKind;
                            ed->selectedObjIndex = hitIndex;
                        }
                        break; /* Consume click — don't paint terrain */
                    }

                    /* No object hit — clear selection (unless using a placement tool) */
                    if (ed->activeTool != ME_TOOL_PLACE_BASE &&
                        ed->activeTool != ME_TOOL_PLACE_PILL &&
                        ed->activeTool != ME_TOOL_PLACE_START) {
                        ed->selectedObjKind = ME_SEL_NONE;
                        ed->selectedObjIndex = -1;
                    }

                    /* Handle placement tools */
                    if (ed->activeTool == ME_TOOL_PLACE_BASE) {
                        const char *err = meValidateBasePlacement(ed, mx, my);
                        if (err) {
                            meSetStatus(ed, "Cannot place base: %s", err);
                        } else {
                            undoBeginCommand(&ed->undoStack);
                            base *b = &ed->bs->item[ed->bs->numBases];
                            memset(b, 0, sizeof(*b));
                            b->x = (BYTE)mx;
                            b->y = (BYTE)my;
                            b->owner = NEUTRAL;
                            b->armour = 90;
                            b->shells = 90;
                            b->mines = 90;
                            ed->bs->active[ed->bs->numBases] = TRUE;
                            ed->bs->numBases++;
                            undoRecordObjAdd(&ed->undoStack, OBJ_BASE, ed->bs->numBases - 1, b);
                            undoEndCommand(&ed->undoStack);
                            ed->dirty = true;
                            ed->minimapDirty = true;
                            ed->statsDirty = true;
                            ed->tabCycleIndex = 0;
                            ed->selectedObjKind = ME_SEL_BASE;
                            ed->selectedObjIndex = ed->bs->numBases - 1;
                        }
                        break;
                    }
                    if (ed->activeTool == ME_TOOL_PLACE_PILL) {
                        const char *err = meValidatePillPlacement(ed, mx, my);
                        if (err) {
                            meSetStatus(ed, "Cannot place pillbox: %s", err);
                        } else {
                            undoBeginCommand(&ed->undoStack);
                            pillbox *p = &ed->pb->item[ed->pb->numPills];
                            memset(p, 0, sizeof(*p));
                            p->x = (BYTE)mx;
                            p->y = (BYTE)my;
                            p->owner = NEUTRAL;
                            p->armour = 15;
                            p->speed = 50;
                            ed->pb->active[ed->pb->numPills] = TRUE;
                            ed->pb->numPills++;
                            undoRecordObjAdd(&ed->undoStack, OBJ_PILL, ed->pb->numPills - 1, p);
                            undoEndCommand(&ed->undoStack);
                            ed->dirty = true;
                            ed->minimapDirty = true;
                            ed->statsDirty = true;
                            ed->tabCycleIndex = 0;
                            ed->selectedObjKind = ME_SEL_PILL;
                            ed->selectedObjIndex = ed->pb->numPills - 1;
                        }
                        break;
                    }
                    if (ed->activeTool == ME_TOOL_PLACE_START) {
                        const char *err = meValidateStartPlacement(ed, mx, my);
                        if (err) {
                            meSetStatus(ed, "Cannot place start: %s", err);
                        } else {
                            undoBeginCommand(&ed->undoStack);
                            start *s = &ed->ss->item[ed->ss->numStarts];
                            s->x = (BYTE)mx;
                            s->y = (BYTE)my;
                            s->dir = meComputeDirToLand(ed, mx, my);
                            ed->ss->active[ed->ss->numStarts] = TRUE;
                            ed->ss->numStarts++;
                            undoRecordObjAdd(&ed->undoStack, OBJ_START, ed->ss->numStarts - 1, s);
                            undoEndCommand(&ed->undoStack);
                            ed->dirty = true;
                            ed->minimapDirty = true;
                            ed->statsDirty = true;
                            ed->tabCycleIndex = 0;
                            ed->selectedObjKind = ME_SEL_START;
                            ed->selectedObjIndex = ed->ss->numStarts - 1;
                        }
                        break;
                    }

                    switch (ed->activeTool) {
                    case ME_TOOL_PENCIL:
                        undoBeginCommand(&ed->undoStack);
                        meBrushStamp(ed, mx, my);
                        ed->isPainting = true;
                        ed->paintLastX = mx;
                        ed->paintLastY = my;
                        break;
                    case ME_TOOL_LINE:
                    case ME_TOOL_RECT:
                    case ME_TOOL_RECT_FILL:
                    case ME_TOOL_OVAL:
                    case ME_TOOL_OVAL_FILL:
                        ed->isDrawing = true;
                        ed->drawStartX = mx;
                        ed->drawStartY = my;
                        ed->drawCurX = mx;
                        ed->drawCurY = my;
                        ed->previewCount = 0;
                        break;
                    case ME_TOOL_FILL:
                        undoBeginCommand(&ed->undoStack);
                        if (ed->mineMode) {
                            meMineFloodFill(ed, mx, my);
                        } else {
                            meFloodFill(ed, mx, my, meEffectiveTerrain(ed));
                        }
                        undoEndCommand(&ed->undoStack);
                        break;
                    case ME_TOOL_WAND:
                        meWandSelect(ed, mx, my);
                        break;
                    case ME_TOOL_SELECT:
                        ed->isSelecting = true;
                        ed->hasSelection = false;
                        ed->hasSelMask = false;
                        ed->selX1 = mx;
                        ed->selY1 = my;
                        ed->selX2 = mx;
                        ed->selY2 = my;
                        break;
                    case ME_TOOL_MAZE:
                        ed->isDrawing = true;
                        ed->mazeDragOriginX = mx;
                        ed->mazeDragOriginY = my;
                        ed->drawStartX = mx;
                        ed->drawStartY = my;
                        ed->drawCurX = mx;
                        ed->drawCurY = my;
                        ed->mazePreviewCount = 0;
                        /* Compute deterministic seed from drag origin */
                        ed->mazeToolConfig.seed = (uint32_t)((mx * 31337) ^ (my * 7919) ^ 1);
                        break;
                    case ME_TOOL_GENERATE:
                        /* Initialize config on first use */
                        if (ed->genConfig.seed == 0) {
                            ed->genConfig = mapGenDefaultConfig(MAPGEN_NATURAL);
                            mapGenNaturalStyleDefaults(MAPGEN_STYLE_INLAND, &ed->genConfig);
                        }
                        ed->isDrawing = true;
                        ed->genDragOriginX = mx;
                        ed->genDragOriginY = my;
                        ed->drawStartX = mx;
                        ed->drawStartY = my;
                        ed->drawCurX = mx;
                        ed->drawCurY = my;
                        ed->genPreviewCount = 0;
                        /* Compute deterministic seed from drag origin */
                        ed->genConfig.seed = (uint32_t)((mx * 31337) ^ (my * 7919) ^ 1);
                        break;
                    }
                }
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    ed->dragging = false;
                }
                if (ev.button.button == SDL_BUTTON_RIGHT) {
                    ed->rightDown = false;
                    ed->dragging = false;
                }
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    /* Handle object drag drop */
                    if (ed->isDraggingObj) {
                        ed->isDraggingObj = false;
                        int screenW2, screenH2;
                        SDL_GetRenderOutputSize(renderer, &screenW2, &screenH2);
                        int mx, my;
                        if (meScreenToMap(ed, ev.button.x * pxScale, ev.button.y * pxScale,
                                          screenW2, screenH2, &mx, &my)) {
                            /* Drop on original = no-op */
                            if (mx != ed->dragObjOrigX || my != ed->dragObjOrigY) {
                                const char *err = NULL;
                                if (ed->selectedObjKind == ME_SEL_BASE) {
                                    err = meValidateBasePlacement(ed, mx, my);
                                } else if (ed->selectedObjKind == ME_SEL_PILL) {
                                    err = meValidatePillPlacement(ed, mx, my);
                                } else if (ed->selectedObjKind == ME_SEL_START) {
                                    err = meValidateStartPlacement(ed, mx, my);
                                }
                                if (err) {
                                    meSetStatus(ed, "Cannot move: %s", err);
                                } else {
                                    int idx = ed->selectedObjIndex;
                                    undoBeginCommand(&ed->undoStack);
                                    if (ed->selectedObjKind == ME_SEL_BASE && idx < ed->bs->numBases) {
                                        base old = ed->bs->item[idx];
                                        ed->bs->item[idx].x = (BYTE)mx;
                                        ed->bs->item[idx].y = (BYTE)my;
                                        undoRecordObjModify(&ed->undoStack, OBJ_BASE, idx, &old, &ed->bs->item[idx]);
                                    } else if (ed->selectedObjKind == ME_SEL_PILL && idx < ed->pb->numPills) {
                                        pillbox old = ed->pb->item[idx];
                                        ed->pb->item[idx].x = (BYTE)mx;
                                        ed->pb->item[idx].y = (BYTE)my;
                                        undoRecordObjModify(&ed->undoStack, OBJ_PILL, idx, &old, &ed->pb->item[idx]);
                                    } else if (ed->selectedObjKind == ME_SEL_START && idx < ed->ss->numStarts) {
                                        start old = ed->ss->item[idx];
                                        ed->ss->item[idx].x = (BYTE)mx;
                                        ed->ss->item[idx].y = (BYTE)my;
                                        undoRecordObjModify(&ed->undoStack, OBJ_START, idx, &old, &ed->ss->item[idx]);
                                    }
                                    undoEndCommand(&ed->undoStack);
                                    ed->dirty = true;
                                    ed->minimapDirty = true;
                                    ed->statsDirty = true;
                                }
                            }
                        }
                    }
                    if (ed->isPainting) {
                        undoEndCommand(&ed->undoStack);
                        ed->isPainting = false;
                    }
                    if (ed->isDrawing) {
                        if (ed->activeTool == ME_TOOL_MAZE && ed->mazePreviewCount > 0) {
                            /* Commit maze preview with per-tile terrain */
                            undoBeginCommand(&ed->undoStack);
                            for (int i = 0; i < ed->mazePreviewCount; i++) {
                                meSetTile(ed, ed->mazePreviewX[i],
                                          ed->mazePreviewY[i],
                                          ed->mazePreviewTerrain[i]);
                            }
                            undoEndCommand(&ed->undoStack);
                            ed->mazePreviewCount = 0;
                            ed->isDrawing = false;
                        } else if (ed->activeTool == ME_TOOL_GENERATE) {
                            if (ed->genPreviewCount > 0) {
                                /* Commit generate preview with per-tile terrain */
                                undoBeginCommand(&ed->undoStack);
                                for (int i = 0; i < ed->genPreviewCount; i++) {
                                    meSetTile(ed, ed->genPreviewX[i],
                                              ed->genPreviewY[i],
                                              ed->genPreviewTerrain[i]);
                                }
                                undoEndCommand(&ed->undoStack);
                            }
                            ed->genPreviewCount = 0;
                            ed->isDrawing = false;
                        } else {
                            meCommitPreview(ed);
                        }
                    }
                    if (ed->isSelecting) {
                        ed->isSelecting = false;
                        /* Normalize selection */
                        if (ed->selX1 != ed->selX2 || ed->selY1 != ed->selY2) {
                            ed->hasSelection = true;
                            ed->hasSelMask = false;
                        } else {
                            /* Single tile click — still a valid selection */
                            ed->hasSelection = true;
                            ed->hasSelMask = false;
                        }
                    }
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (mapEditorImguiWantMouse()) break;
                if (ev.wheel.y > 0) {
                    if (ed->zoomStepIndex < (int)ZOOM_STEP_COUNT - 1) ed->zoomStepIndex++;
                } else if (ev.wheel.y < 0) {
                    if (ed->zoomStepIndex > 0) ed->zoomStepIndex--;
                }
                ed->zoomLevel = zoomSteps[ed->zoomStepIndex];
                ed->zoomFactor = meRenderZoom(ed, pxScale);
                break;

            case SDL_EVENT_DROP_FILE: {
                const char *dropPath = ev.drop.data;
                if (dropPath) {
                    if (meAnythingDirty(ed)) {
                        SDL_strlcpy(ed->pendingOpenPath, dropPath, ME_PATH_MAX);
                        ed->deferredAction = FILE_OP_SAVE_THEN_OPEN;
                        openUnsavedModal = true;
                    } else {
                        meLoadFromPath(ed, dropPath);
                    }
                }
                break;
            }
            }
        }

        /* Handle completed file dialog */
        if (ed->fileDialogGotResult) {
            meHandleFileDialogResult(ed);
        }

        /* Trackpad pinch-to-zoom (macOS) */
        if (!mapEditorImguiWantMouse()) {
            float pinch = macOSPinchZoomConsume();
            if (pinch != 0.0f) {
                /* Accumulate small pinch deltas into stepped zoom changes */
                static float pinchAccum = 0.0f;
                pinchAccum += pinch;
                while (pinchAccum > 0.15f) {
                    if (ed->zoomStepIndex < (int)ZOOM_STEP_COUNT - 1) ed->zoomStepIndex++;
                    pinchAccum -= 0.15f;
                }
                while (pinchAccum < -0.15f) {
                    if (ed->zoomStepIndex > 0) ed->zoomStepIndex--;
                    pinchAccum += 0.15f;
                }
                ed->zoomLevel = zoomSteps[ed->zoomStepIndex];
                ed->zoomFactor = meRenderZoom(ed, pxScale);
            }
        }

        /* Arrow key scrolling (continuous while held) */
        if (!mapEditorImguiWantKeyboard()) {
            if (keystate[SDL_SCANCODE_LEFT] || keystate[SDL_SCANCODE_RIGHT] ||
                keystate[SDL_SCANCODE_UP] || keystate[SDL_SCANCODE_DOWN]) {
                int scrollAmount = ME_SCROLL_SPEED * 256 / 3;
                int cx = (int)ed->viewCenterX;
                int cy = (int)ed->viewCenterY;
                if (keystate[SDL_SCANCODE_LEFT])  cx -= scrollAmount;
                if (keystate[SDL_SCANCODE_RIGHT]) cx += scrollAmount;
                if (keystate[SDL_SCANCODE_UP])    cy -= scrollAmount;
                if (keystate[SDL_SCANCODE_DOWN])  cy += scrollAmount;
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                if (cx > 65535) cx = 65535;
                if (cy > 65535) cy = 65535;
                ed->viewCenterX = (WORLD)cx;
                ed->viewCenterY = (WORLD)cy;
            }
        }

        /* Rebuild minimap if dirty */
        if (ed->minimapDirty && ed->minimapTex) {
            mapEditorRebuildMinimap(ed);
            ed->minimapDirty = false;
        }

        /* Recompute basic stats when dirty and panel is visible */
        if (ed->showStatsPanel && ed->statsDirty) {
            mapStatsComputeBasic(&ed->mapStats, ed->mp, ed->bs, ed->pb, ed->ss);
            ed->statsDirty = false;
            ed->mapStats.spatialValid = false;
        }

        /* Render. The viewport is the renderer's, not the window's: under
           SDL_WINDOW_HIGH_PIXEL_DENSITY they differ by the pixel density. */
        int screenW, screenH;
        SDL_GetRenderOutputSize(renderer, &screenW, &screenH);

        /* Sub-1x zoom: render at 1x into larger offscreen texture, then blit scaled-down */
        bool subZoom = (ed->zoomLevel < 1.0f);
        int renderW = screenW, renderH = screenH;

        if (subZoom) {
            meSubZoomRenderSize(ed, screenW, screenH, &renderW, &renderH);
            /* Clear the window as well: when renderW/H hit the texture-size
               cap the blit below covers less than the whole window, and the
               remainder would otherwise show the previous frame. */
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, NULL);
            meEnsureOffscreen(ed, renderW, renderH);
            SDL_SetRenderTarget(renderer, ed->offscreenTex);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */
        } else {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, NULL); /* avoid SDL3 Metal sampler bug in SDL_RenderClear */
        }

        /* Rasterize the atlas at the size tiles are about to be drawn at. Runs
           here rather than before the event pump so a zoom change made this
           frame is already reflected. */
        meEnsureAtlas(ed, ed->zoomFactor);

        /* ImGui's SDL3 renderer backend forces every texture it draws to its
           current sampler (LINEAR by default), and the terrain palette draws
           tile icons straight out of this atlas. Re-assert NEAREST each frame
           or the zoomed canvas samples across atlas cell borders and shows a
           dark grid between tiles. */
        SDL_SetTextureScaleMode(ed->tilesTex, SDL_SCALEMODE_NEAREST);

        meRenderTiles(ed, renderW, renderH);
        if (ed->showGrid) meRenderGrid(ed, renderW, renderH);
        meRenderStarts(ed, renderW, renderH);
        meRenderPillRanges(ed, renderW, renderH);

        /* Compute hover tile for overlays (uses actual screen dims for mouse mapping) */
        int hoverMX = -1, hoverMY = -1;
        meScreenToMap(ed, ed->mouseX, ed->mouseY, screenW, screenH, &hoverMX, &hoverMY);

        /* Render tool overlays (before ImGui) */
        if (ed->isDrawing) meRenderPreview(ed, renderW, renderH);
        if (ed->mazePreviewCount > 0) meRenderMazePreview(ed, renderW, renderH);
        if (ed->genPreviewCount > 0) meRenderGenPreview(ed, renderW, renderH);
        meRenderSelection(ed, renderW, renderH);
        meRenderScenarioRegions(ed, renderW, renderH);
        meRenderPastePreview(ed, renderW, renderH, hoverMX, hoverMY);
        meRenderBorderIndicator(ed, renderW, renderH, hoverMX, hoverMY);
        meRenderObjSelection(ed, renderW, renderH);
        meRenderObjPreview(ed, renderW, renderH, hoverMX, hoverMY);

        /* Brush cursor preview for pencil tool */
        if (ed->activeTool == ME_TOOL_PENCIL && ed->brushSize > 0 &&
            hoverMX >= 0 && hoverMY >= 0 && !mapEditorImguiWantMouse()) {
            int zf = ed->zoomFactor;
            int tileSize = TILE_SIZE_X;
            int scaledTile = tileSize * zf;
            int centerPX = ((int)ed->viewCenterX * tileSize) >> 8;
            int centerPY = ((int)ed->viewCenterY * tileSize) >> 8;
            int camPX = centerPX - renderW / (2 * zf);
            int camPY = centerPY - renderH / (2 * zf);
            int size = ME_BRUSH_SIZES[ed->brushSize];
            int r = size / 2;

            SDL_SetRenderDrawBlendMode(ed->renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ed->renderer, 255, 255, 255, 60);
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    if (ed->brushShape == ME_BRUSH_CIRCLE) {
                        if (size <= 3) {
                            if (abs(dx) + abs(dy) > r) continue;
                        } else {
                            if (dx * dx + dy * dy > r * r) continue;
                        }
                    }
                    int tx = hoverMX + dx;
                    int ty = hoverMY + dy;
                    if (!meIsInMap(tx, ty)) continue;
                    float px = (float)(tx * tileSize - camPX) * zf;
                    float py = (float)(ty * tileSize - camPY) * zf;
                    SDL_FRect rect = { px, py, (float)scaledTile, (float)scaledTile };
                    SDL_RenderFillRect(ed->renderer, &rect);
                }
            }
        }

        if (subZoom) {
            SDL_SetRenderTarget(renderer, NULL);
            /* Destination is renderW/H scaled by exactly zoomLevel, not the
               whole window. They are the same thing until the offscreen hits
               the texture-size cap; past it, stretching a short texture across
               the full window would put the view at some scale other than
               zoomLevel, which meScreenToMap cannot account for. */
            SDL_FRect src = { 0, 0, (float)renderW, (float)renderH };
            SDL_FRect dst = { 0, 0,
                              (float)renderW * ed->zoomLevel,
                              (float)renderH * ed->zoomLevel };
            SDL_RenderTexture(renderer, ed->offscreenTex, &src, &dst);
        }

        /* ImGui overlay */
        mapEditorImguiNewFrame();

        /* Open the unsaved-changes modal if requested */
        if (openUnsavedModal) {
            ImGui_OpenPopup(langGetText(STR_MAPEDIT_UNSAVED_TITLE));
            openUnsavedModal = false;
        }

        /* Open error modal if there's a new error message */
        if (ed->errorMessage[0] && !ed->errorModalOpen) {
            ImGui_OpenPopup(langGetText(STR_ERR_TITLE));
            ed->errorModalOpen = true;
        }

        /* Menu bar */
        MapEditorMenuAction menuAction;
#ifdef __APPLE__
        /* On macOS the menu lives in the system menu bar via the
         * NSMenu shim. Push state in (enable gates, checkmarks,
         * Recent/Zoom contents, scope labels) and drain any clicks
         * the user made between frames into `menuAction`. The
         * in-window mapEditorImguiMenuBar is a no-op on Apple. */
        {
            struct MeMenuState mes;
            memset(&mes, 0, sizeof(mes));
            mes.fromMainMenu     = ed->fromMainMenu;
            mes.canUndo          = undoCanUndo(&ed->undoStack);
            mes.canRedo          = undoCanRedo(&ed->undoStack);
            mes.hasSelection     = ed->hasSelection;
            mes.showGrid         = ed->showGrid;
            mes.showMines        = ed->showMines;
            mes.showPillRanges   = ed->showPillRanges;
            mes.showTerrain      = ed->showTerrain;
            mes.showTools        = ed->showTools;
            mes.showInspector    = ed->showInspector;
            mes.showObjects      = ed->showObjects;
            mes.showOverview     = ed->showOverview;
            mes.showStats        = ed->showStatsPanel;
            mes.showStampLibrary = ed->showStampLibrary;
            mes.showScenario     = ed->showScenario;
            int nRecent = ed->numRecentFiles;
            if (nRecent > (int)(sizeof(mes.recentFiles) / sizeof(mes.recentFiles[0]))) {
                nRecent = (int)(sizeof(mes.recentFiles) / sizeof(mes.recentFiles[0]));
            }
            mes.numRecent = nRecent;
            for (int i = 0; i < nRecent; i++) {
                mes.recentFiles[i] = ed->recentFiles[i];
            }
            mes.zoomStepIndex  = ed->zoomStepIndex;
            mes.zoomStepCount  = (int)ZOOM_STEP_COUNT;
            mes.zoomStepValues = zoomSteps;
            me_mac_menubar_refresh(&mes);
            me_mac_menubar_consume_actions(&menuAction);
        }
#else
        mapEditorImguiMenuBar(&menuAction, ed->recentFiles, ed->numRecentFiles,
                              ed->showGrid, ed->showMines, ed->showPillRanges,
                              ed->dirty,
                              undoCanUndo(&ed->undoStack), undoCanRedo(&ed->undoStack),
                              ed->hasSelection,
                              &ed->showTerrain, &ed->showTools,
                              &ed->showInspector, &ed->showObjects,
                              &ed->showOverview, &ed->showStatsPanel,
                              &ed->showStampLibrary, &ed->showScenario,
                              ed->fromMainMenu,
                              ed->zoomStepIndex, (int)ZOOM_STEP_COUNT,
                              zoomSteps);
#endif

        /* Handle menu actions */
        if (menuAction.wantNew) {
            meActionNew(ed);
            if (ed->deferredAction) openUnsavedModal = true;
        }
        if (menuAction.wantOpen) {
            meActionOpen(ed);
            if (ed->deferredAction) openUnsavedModal = true;
        }
#ifdef MAPEDITOR_WBN_OPEN
        if (menuAction.wantOpenWbn) {
            meActionOpenWbn(ed);
            if (ed->deferredAction) openUnsavedModal = true;
        }
#endif
        if (menuAction.wantSave) {
            meActionSave(ed);
        }
        if (menuAction.wantSaveAs) {
            meActionSaveAs(ed);
        }
        if (menuAction.wantExit) {
            ed->exitIsAppQuit = menuAction.wantQuitApp;
            meActionExit(ed);
            if (ed->deferredAction) openUnsavedModal = true;
        }
        if (menuAction.wantUndo) {
            if (undoApply(&ed->undoStack, ed->mp, ed->pb, ed->bs, ed->ss)) {
                ed->dirty = (ed->undoStack.count != ed->undoStack.savedCommandIndex);
                ed->minimapDirty = true;
                ed->statsDirty = true;
                ed->selectedObjKind = ME_SEL_NONE;
                ed->selectedObjIndex = -1;
                meUpdateWindowTitle(ed);
            }
        }
        if (menuAction.wantRedo) {
            if (redoApply(&ed->undoStack, ed->mp, ed->pb, ed->bs, ed->ss)) {
                ed->dirty = (ed->undoStack.count != ed->undoStack.savedCommandIndex);
                ed->minimapDirty = true;
                ed->statsDirty = true;
                ed->selectedObjKind = ME_SEL_NONE;
                ed->selectedObjIndex = -1;
                meUpdateWindowTitle(ed);
            }
        }
        if (menuAction.wantCopy) {
            meCopySelection(ed);
        }
        if (menuAction.wantCut) {
            meCutSelection(ed);
        }
        if (menuAction.wantPaste) {
            if (ed->hasClipboard) ed->isPasting = true;
        }
        if (menuAction.wantCenter) {
            meCenterMapContents(ed);
        }
        if (menuAction.wantPointStarts) {
            mePointStartsToLand(ed);
        }
        if (menuAction.toggleGrid) {
            ed->showGrid = !ed->showGrid;
        }
        if (menuAction.toggleMines) {
            ed->showMines = !ed->showMines;
        }
        if (menuAction.togglePillRanges) {
            ed->showPillRanges = !ed->showPillRanges;
        }
        /* Window submenu toggles — populated only by the macOS NSMenu
         * shim. The in-window ImGui menu mutates show* directly via
         * the &ed->showX pointers passed into mapEditorImguiMenuBar. */
        if (menuAction.wantToggleTerrain)      ed->showTerrain      = !ed->showTerrain;
        if (menuAction.wantToggleTools)        ed->showTools        = !ed->showTools;
        if (menuAction.wantToggleInspector)    ed->showInspector    = !ed->showInspector;
        if (menuAction.wantToggleObjects)      ed->showObjects      = !ed->showObjects;
        if (menuAction.wantToggleOverview)     ed->showOverview     = !ed->showOverview;
        if (menuAction.wantToggleStats)        ed->showStatsPanel   = !ed->showStatsPanel;
        if (menuAction.wantToggleStampLibrary) ed->showStampLibrary = !ed->showStampLibrary;
        if (menuAction.wantToggleScenario)     ed->showScenario     = !ed->showScenario;
        if (menuAction.wantZoomIn) {
            if (ed->zoomStepIndex < (int)ZOOM_STEP_COUNT - 1) ed->zoomStepIndex++;
            ed->zoomLevel = zoomSteps[ed->zoomStepIndex];
            ed->zoomFactor = meRenderZoom(ed, pxScale);
        }
        if (menuAction.wantZoomOut) {
            if (ed->zoomStepIndex > 0) ed->zoomStepIndex--;
            ed->zoomLevel = zoomSteps[ed->zoomStepIndex];
            ed->zoomFactor = meRenderZoom(ed, pxScale);
        }
        if (menuAction.wantZoomSet) {
            int idx = menuAction.zoomSetIndex;
            if (idx >= 0 && idx < (int)ZOOM_STEP_COUNT) {
                ed->zoomStepIndex = idx;
                ed->zoomLevel = zoomSteps[idx];
                ed->zoomFactor = meRenderZoom(ed, pxScale);
            }
        }
        if (menuAction.wantValidate) {
            meRunValidation(ed);
        }
        if (menuAction.wantSymMirrorH) {
            meEditorMirrorH(ed);
        }
        if (menuAction.wantSymMirrorV) {
            meEditorMirrorV(ed);
        }
        if (menuAction.wantSymRotate90) {
            meEditorRotate90(ed);
        }
        if (menuAction.wantSymRotate180) {
            meEditorRotate180(ed);
        }
        if (menuAction.wantText) {
            ed->showTextDialog = true;
        }
        if (menuAction.wantExportPNG) {
            ed->showExportDialog = true;
        }
        if (menuAction.wantImageImport) {
            ed->imageImportCfg.numColors = ed->imageImportCfg.numColors > 0 ? ed->imageImportCfg.numColors : 10;
            /* Pass current selection bounds for FIT_SELECTION mode */
            if (ed->hasSelection) {
                int sx1 = ed->selX1 < ed->selX2 ? ed->selX1 : ed->selX2;
                int sx2 = ed->selX1 < ed->selX2 ? ed->selX2 : ed->selX1;
                int sy1 = ed->selY1 < ed->selY2 ? ed->selY1 : ed->selY2;
                int sy2 = ed->selY1 < ed->selY2 ? ed->selY2 : ed->selY1;
                ed->imageImportCfg.selW = sx2 - sx1 + 1;
                ed->imageImportCfg.selH = sy2 - sy1 + 1;
            } else {
                ed->imageImportCfg.selW = 64;
                ed->imageImportCfg.selH = 64;
            }
            ed->showImageImportDialog = true;
        }
        if (menuAction.wantGenerate) {
            /* Only reset to defaults on first use; preserve previous settings */
            if (ed->genConfig.seed == 0) {
                ed->genConfig = mapGenDefaultConfig(MAPGEN_TOURNAMENT);
            }
            /* Full-map: always set bases/pills/starts to 16 and lock them */
            ed->genConfig.locks |= MAPGEN_LOCK_BASES | MAPGEN_LOCK_PILLS | MAPGEN_LOCK_STARTS;
            ed->genConfig.bases = 16;
            ed->genConfig.pills = 16;
            ed->genConfig.starts = 16;
            /* Refresh the seed so consecutive generates differ */
            ed->genConfig.seed = (uint32_t)SDL_GetTicksNS();
            /* Menu always uses full-map scope */
            ed->genConfig.x1 = 21;
            ed->genConfig.y1 = 21;
            ed->genConfig.x2 = 235;
            ed->genConfig.y2 = 235;
            ed->generateSelectionScope = false;
            ed->showGenerateDialog = true;
        }
        if (menuAction.openRecentIndex >= 0 && menuAction.openRecentIndex < ed->numRecentFiles) {
            const char *recentPath = ed->recentFiles[menuAction.openRecentIndex];
            if (meAnythingDirty(ed)) {
                SDL_strlcpy(ed->pendingOpenPath, recentPath, ME_PATH_MAX);
                ed->deferredAction = FILE_OP_SAVE_THEN_OPEN;
                openUnsavedModal = true;
            } else {
                meLoadFromPath(ed, recentPath);
            }
        }

        /* Terrain palette and tools toolbar */
        if (ed->showTerrain) {
            mapEditorImguiTerrainPalette(ed->tilesTex, TILE_SIZE_X,
                                         &ed->activeTerrainIdx, &ed->mineMode,
                                         &ed->minePattern,
                                         &ed->showTerrain);
        }
        if (ed->showTools) {
            bool toolbarWantText = false;
            mapEditorImguiToolbar(&ed->activeTool,
                                  ed->tilesTex, TILE_SIZE_X,
                                  &ed->brushSize, &ed->brushShape,
                                  &ed->showTools, &toolbarWantText);
            if (toolbarWantText) {
                ed->showTextDialog = true;
            }
        }

        /* Maze tool settings panel */
        if (ed->activeTool == ME_TOOL_MAZE) {
            int mx0 = ed->mazeDragOriginX < ed->drawCurX ? ed->mazeDragOriginX : ed->drawCurX;
            int my0 = ed->mazeDragOriginY < ed->drawCurY ? ed->mazeDragOriginY : ed->drawCurY;
            int mx1 = ed->mazeDragOriginX > ed->drawCurX ? ed->mazeDragOriginX : ed->drawCurX;
            int my1 = ed->mazeDragOriginY > ed->drawCurY ? ed->mazeDragOriginY : ed->drawCurY;
            bool mazeChanged = mapEditorImguiMazeSettings(&ed->mazeToolConfig,
                                       ed->isDrawing, mx0, my0, mx1, my1,
                                       ed->tilesTex, TILE_SIZE_X);
            /* If settings changed during drag, regenerate preview */
            if (mazeChanged && ed->isDrawing) {
                ed->mazePreviewCount = mazeGeneratePreview(
                    mx0, my0, mx1, my1,
                    ed->mazePreviewX, ed->mazePreviewY,
                    ed->mazePreviewTerrain,
                    ME_FILL_LIMIT, &ed->mazeToolConfig);
            }
        }

        /* Generate tool settings panel */
        if (ed->activeTool == ME_TOOL_GENERATE) {
            if (ed->genConfig.seed == 0) {
                ed->genConfig = mapGenDefaultConfig(MAPGEN_NATURAL);
                mapGenNaturalStyleDefaults(MAPGEN_STYLE_INLAND, &ed->genConfig);
            }
            bool genSettingsChanged = mapEditorImguiGenerateSettings(
                &ed->genConfig, ed->isDrawing,
                ed->genConfig.x1, ed->genConfig.y1,
                ed->genConfig.x2, ed->genConfig.y2);
            /* If settings changed during drag, regenerate preview */
            if (genSettingsChanged && ed->isDrawing) {
                meBuildGenPreview(ed,
                    ed->genConfig.x1, ed->genConfig.y1,
                    ed->genConfig.x2, ed->genConfig.y2);
            }
        }

        /* Inspector panel */
        if (ed->showInspector) {
            if (mapEditorImguiInspector(ed->selectedObjKind, ed->selectedObjIndex,
                                         ed->bs, ed->pb, ed->ss,
                                         &ed->inspState, &ed->undoStack,
                                         &ed->showInspector)) {
                ed->dirty = true;
                ed->minimapDirty = true;
                ed->statsDirty = true;
            }
        }

        /* Object list panel */
        if (ed->showObjects) {
            int clickKind, clickIdx, panX, panY;
            mapEditorImguiObjectList(ed->bs, ed->pb, ed->ss,
                                      ed->selectedObjKind, ed->selectedObjIndex,
                                      &clickKind, &clickIdx, &panX, &panY,
                                      &ed->showObjects);
            if (clickKind != ME_SEL_NONE) {
                ed->selectedObjKind = clickKind;
                ed->selectedObjIndex = clickIdx;
                if (panX >= 0 && panY >= 0) {
                    ed->viewCenterX = (WORLD)(panX << 8);
                    ed->viewCenterY = (WORLD)(panY << 8);
                }
            }
        }

        /* Overview minimap window */
        if (ed->showOverview) {
            int navX = -1, navY = -1;
            mapEditorImguiOverview(ed->minimapTex, ed->viewCenterX, ed->viewCenterY,
                                    meScreenTilePixels(ed), screenW, screenH,
                                    &navX, &navY,
                                    &ed->showOverview);
            if (navX >= 0 && navY >= 0) {
                ed->viewCenterX = (WORLD)(navX << 8);
                ed->viewCenterY = (WORLD)(navY << 8);
            }
        }

        /* Go To Coordinates dialog */
        if (ed->openGotoDialog) {
            ImGui_OpenPopup(langGetText(STR_MAPEDIT_GOTO_TITLE));
            ed->openGotoDialog = false;
        }
        {
            int gotoX = -1, gotoY = -1;
            if (mapEditorImguiGotoDialog(&gotoX, &gotoY)) {
                ed->viewCenterX = (WORLD)(gotoX << 8);
                ed->viewCenterY = (WORLD)(gotoY << 8);
            }
        }

#ifdef MAPEDITOR_WBN_OPEN
        /* Open from WinBolo.net dialog. A completed download hands
         * back the raw .map bytes; a parse failure lands in
         * errorMessage and surfaces through the error modal. */
        {
            unsigned char *wbnBytes = NULL;
            int wbnLen = 0;
            char wbnName[128];
            if (meWbnOpenFrame(ed->renderer, &wbnBytes, &wbnLen,
                               wbnName, sizeof(wbnName))) {
                meLoadFromMemory(ed, wbnBytes, wbnLen, wbnName);
                SDL_free(wbnBytes);
            }
        }
#endif

        /* Generate map dialog */
        if (ed->showGenerateDialog) {
            int gSelX1 = ed->genConfig.x1, gSelY1 = ed->genConfig.y1;
            int gSelX2 = ed->genConfig.x2, gSelY2 = ed->genConfig.y2;
            if (mapEditorImguiGenerateDialog(&ed->showGenerateDialog, &ed->genConfig,
                                              ed->hasSelection,
                                              gSelX1, gSelY1, gSelX2, gSelY2,
                                              ed->generateSelectionScope)) {

                undoBeginCommand(&ed->undoStack);

                /* Snapshot old terrain for undo */
                int gx1 = ed->genConfig.x1, gy1 = ed->genConfig.y1;
                int gx2 = ed->genConfig.x2, gy2 = ed->genConfig.y2;
                int gw = gx2 - gx1 + 1, gh = gy2 - gy1 + 1;
                BYTE *oldTerrain = (BYTE *)malloc((size_t)gw * (size_t)gh);
                if (oldTerrain) {
                    for (int x = gx1; x <= gx2; x++) {
                        for (int y = gy1; y <= gy2; y++) {
                            oldTerrain[(x - gx1) * gh + (y - gy1)] = ed->mp->mapItem[x][y];
                        }
                    }
                }

                /* Record undo for objects that will be removed */
                for (int i = ed->bs->numBases - 1; i >= 0; i--) {
                    base *b = &ed->bs->item[i];
                    if (b->x >= gx1 && b->x <= gx2 && b->y >= gy1 && b->y <= gy2) {
                        undoRecordObjRemove(&ed->undoStack, OBJ_BASE, i, b);
                    }
                }
                for (int i = ed->pb->numPills - 1; i >= 0; i--) {
                    pillbox *p = &ed->pb->item[i];
                    if (p->x >= gx1 && p->x <= gx2 && p->y >= gy1 && p->y <= gy2) {
                        undoRecordObjRemove(&ed->undoStack, OBJ_PILL, i, p);
                    }
                }
                for (int i = ed->ss->numStarts - 1; i >= 0; i--) {
                    start *s = &ed->ss->item[i];
                    if (s->x >= gx1 && s->x <= gx2 && s->y >= gy1 && s->y <= gy2) {
                        undoRecordObjRemove(&ed->undoStack, OBJ_START, i, s);
                    }
                }

                /* Run the generator */
                mapGenRun(ed->mp, ed->bs, ed->pb, ed->ss, &ed->genConfig);

                /* Record tile changes with correct new terrain for redo */
                if (oldTerrain) {
                    for (int x = gx1; x <= gx2; x++) {
                        for (int y = gy1; y <= gy2; y++) {
                            BYTE oldT = oldTerrain[(x - gx1) * gh + (y - gy1)];
                            BYTE newT = ed->mp->mapItem[x][y];
                            if (oldT != newT) {
                                /* Temporarily restore old value so undoRecordTile
                                 * captures it, then put the new value back. */
                                ed->mp->mapItem[x][y] = oldT;
                                undoRecordTile(&ed->undoStack, ed->mp, (BYTE)x, (BYTE)y, newT);
                                ed->mp->mapItem[x][y] = newT;
                            }
                        }
                    }
                    free(oldTerrain);
                }

                /* Record undo for newly-added objects */
                for (int i = 0; i < (int)ed->bs->numBases; i++) {
                    base *b = &ed->bs->item[i];
                    if (b->x >= gx1 && b->x <= gx2 && b->y >= gy1 && b->y <= gy2) {
                        undoRecordObjAdd(&ed->undoStack, OBJ_BASE, i, b);
                    }
                }
                for (int i = 0; i < (int)ed->pb->numPills; i++) {
                    pillbox *p = &ed->pb->item[i];
                    if (p->x >= gx1 && p->x <= gx2 && p->y >= gy1 && p->y <= gy2) {
                        undoRecordObjAdd(&ed->undoStack, OBJ_PILL, i, p);
                    }
                }
                for (int i = 0; i < (int)ed->ss->numStarts; i++) {
                    start *s = &ed->ss->item[i];
                    if (s->x >= gx1 && s->x <= gx2 && s->y >= gy1 && s->y <= gy2) {
                        undoRecordObjAdd(&ed->undoStack, OBJ_START, i, s);
                    }
                }

                undoEndCommand(&ed->undoStack);

                ed->dirty = true;
                ed->minimapDirty = true;
                ed->statsDirty = true;
                {
                    char seedStr[64];
                    mapGenConfigToSeed(&ed->genConfig, seedStr, sizeof(seedStr));
                    meSetStatus(ed, "Generated with seed: %s", seedStr);
                }
            }
        }

        /* Text tool dialog */
        if (ed->showTextDialog) {
            if (mapEditorImguiTextDialog(&ed->showTextDialog, &ed->textConfig,
                                          ed->tilesTex, TILE_SIZE_X,
                                          &ed->fontList, &ed->fontListReady,
                                          &ed->textFontSource, &ed->textFontFamily,
                                          &ed->textFontStyleIdx, &ed->textTTFPixelSize)) {
                BYTE tempTerrain[256][256];
                int tw = 0, th = 0;
                bool ok = false;

                if (ed->textFontSource == 0) {
                    /* Built-in bitmap font */
                    ok = textRasterize(&ed->textConfig, tempTerrain, &tw, &th);
                }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
                else {
                    /* TTF font */
                    const char *familyNames[FONT_MAX_ENTRIES];
                    int numFamilies = fontListGetFamilies(&ed->fontList, familyNames, FONT_MAX_ENTRIES);
                    if (ed->textFontFamily >= 0 && ed->textFontFamily < numFamilies) {
                        int styleIndices[32];
                        int numStyles = fontListFindStyles(&ed->fontList,
                                                           familyNames[ed->textFontFamily],
                                                           styleIndices, 32);
                        int si = ed->textFontStyleIdx;
                        if (si >= 0 && si < numStyles) {
                            FontEntry *fe = &ed->fontList.entries[styleIndices[si]];
                            /* Style is already embodied by the font file — pass NORMAL */
                            ok = textRasterizeTTF(fe->filePath, ed->textTTFPixelSize,
                                                  ed->textConfig.text,
                                                  ed->textConfig.textTerrain,
                                                  ed->textConfig.edgeTerrain,
                                                  ed->textConfig.bgTerrain,
                                                  tempTerrain, &tw, &th);
                        }
                    }
                }
#endif

                if (ok) {
                    ed->clipW = tw;
                    ed->clipH = th;
                    memcpy(ed->clipTerrain, tempTerrain, sizeof(ed->clipTerrain));
                    ed->clipNumPills = 0;
                    ed->clipNumBases = 0;
                    ed->clipNumStarts = 0;
                    ed->hasClipboard = true;
                    ed->isPasting = true;
                }
            }
        }

        /* Export as PNG dialog */
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
        if (ed->showExportDialog) {
            ed->exportPath[0] = '\0';
            if (mapEditorImguiExportDialog(&ed->showExportDialog, &ed->exportCfg,
                                            ed->exportPath, ME_PATH_MAX,
                                            ed->window)) {
                if (ed->exportPath[0] != '\0') {
                    bool ok = mapExportPNG(ed->exportPath, &ed->exportCfg,
                                           ed->renderer,
                                           ed->mp, ed->bs, ed->pb, ed->ss);
                    if (ok) {
                        meSetStatus(ed, "Exported PNG: %s", ed->exportPath);
                    } else {
                        meSetStatus(ed, "Export failed");
                    }
                }
            }
        }
#endif

        /* Image import dialog */
        if (ed->showImageImportDialog) {
            if (mapEditorImguiImageImportDialog(&ed->showImageImportDialog,
                                                &ed->imageImportCfg,
                                                ed->renderer)) {
                /* Import: copy result terrain into clipboard */
                ed->clipW = ed->imageImportCfg.resultW;
                ed->clipH = ed->imageImportCfg.resultH;
                memcpy(ed->clipTerrain, ed->imageImportCfg.resultTerrain,
                       sizeof(ed->clipTerrain));
                ed->clipNumPills = 0;
                ed->clipNumBases = 0;
                ed->clipNumStarts = 0;
                ed->hasClipboard = true;
                ed->isPasting = true;
                imageImportFree(&ed->imageImportCfg);
            }
        }

        /* Validation panel */
        {
            int vLocType = -1, vIdx = -1, vX = -1, vY = -1;
            mapEditorImguiValidationPanel(ed->lastValidation.issues,
                                           ed->lastValidation.count,
                                           ed->lastValidation.errorCount,
                                           ed->lastValidation.warningCount,
                                           &ed->showValidationPanel,
                                           &vLocType, &vIdx, &vX, &vY);
            if (vLocType > 0 && vX >= 0 && vY >= 0) {
                ed->viewCenterX = (WORLD)(vX << 8);
                ed->viewCenterY = (WORLD)(vY << 8);
                if (vLocType == 2 && vIdx >= 0) { /* VALIDATE_LOC_BASE */
                    ed->selectedObjKind = ME_SEL_BASE;
                    ed->selectedObjIndex = vIdx;
                } else if (vLocType == 3 && vIdx >= 0) { /* VALIDATE_LOC_PILL */
                    ed->selectedObjKind = ME_SEL_PILL;
                    ed->selectedObjIndex = vIdx;
                } else if (vLocType == 4 && vIdx >= 0) { /* VALIDATE_LOC_START */
                    ed->selectedObjKind = ME_SEL_START;
                    ed->selectedObjIndex = vIdx;
                }
                ed->minimapDirty = true;
                ed->statsDirty = true;
            }
        }

        /* Statistics panel */
        if (ed->showStatsPanel) {
            bool wantRefresh = false;
            mapEditorImguiStatsPanel(&ed->mapStats, &ed->showStatsPanel, &wantRefresh);
            if (wantRefresh) {
                mapStatsComputeSpatial(&ed->mapStats, ed->mp, ed->bs, ed->pb);
            }
        }

        /* Scenario panel. The panel reports what was clicked; reading and
         * writing the script file happens here. */
        if (ed->showScenario) {
            bool wantScriptSave = false;
            bool wantScriptReload = false;
            bool wantScriptValidate = false;
            bool wantPack = false;
            bool wantSaveMod = false;
            MEScenarioMapInfo scnMap;
            int scnClickKind, scnClickIdx, scnPanX, scnPanY;

            meScenarioFillMapInfo(ed, &scnMap);
            mapEditorImguiScenarioPanel(&ed->scn, &ed->scnForm, &ed->scnCheck,
                                        ed->currentFilePath, &ed->scnView,
                                        &ed->showScenario, &scnMap,
                                        ed->selectedObjKind,
                                        ed->selectedObjIndex, &scnClickKind,
                                        &scnClickIdx, &scnPanX, &scnPanY,
                                        &wantScriptSave,
                                        &wantScriptReload,
                                        &wantScriptValidate, &wantPack,
                                        &wantSaveMod);
            /* A tag row picks the object it names, the way the object list
             * does. */
            if (scnClickKind != ME_SEL_NONE) {
                ed->selectedObjKind = scnClickKind;
                ed->selectedObjIndex = scnClickIdx;
                if (scnPanX >= 0 && scnPanY >= 0) {
                    ed->viewCenterX = (WORLD)(scnPanX << 8);
                    ed->viewCenterY = (WORLD)(scnPanY << 8);
                }
            }
            if (wantScriptSave && ed->currentFilePath[0]) {
                /* A script that reached the disk is checked without being
                 * asked, so the author is told about a typo at the moment the
                 * file becomes the one a round would read. */
                if (meScenarioSaveForMap(&ed->scn, ed->currentFilePath)) {
                    wantScriptValidate = true;
                }
            }
            if (wantScriptReload) {
                meScenarioReload(&ed->scn);
                /* The text is not the text the markers were found in any
                 * more. */
                meScenarioCheckClear(&ed->scnCheck);
            }
            if (wantScriptValidate) {
                /* The forms' manifest goes on as the scenario global first, so
                 * Validate asks the question a server asks when it loads the
                 * pair rather than a stricter one about the script alone. */
                meScenarioCheckRun(&ed->scnCheck, ed->scn.script,
                                   ed->scn.scriptLen,
                                   ed->scn.scriptPath[0] != '\0'
                                       ? ed->scn.scriptPath
                                       : ME_SCENARIO_CHECK_UNNAMED,
                                   &ed->scnForm.manifest);
            }
            if (wantPack && ed->currentFilePath[0]) {
                meWriteScenario(ed, NULL);
            }
            if (wantSaveMod) {
                /* A mod needs a name of its own, so the write waits for the
                 * dialog the way Save As does. */
                meShowSaveModDialog(ed);
            }
        }

        /* Stamp library panel */
        if (ed->showStampLibrary) {
            bool wantSave = false;
            int stampIdx = mapEditorImguiStampLibrary(&ed->stampLib, ed->renderer,
                                                      ed->hasClipboard, &ed->showStampLibrary,
                                                      &wantSave, ed->saveStampName,
                                                      (int)sizeof(ed->saveStampName));
            if (wantSave) {
                ed->showSaveStampDialog = true;
            }
            if (stampIdx == -2) {
                /* Refresh requested (delete, import, or refresh button) */
                const char *base = SDL_GetBasePath();
                char dataDir[512];
                snprintf(dataDir, sizeof(dataDir), "%sdata", base ? base : "");
                stampLibraryRefresh(&ed->stampLib, dataDir);
            } else if (stampIdx >= 0 && stampIdx < ed->stampLib.count) {
                /* Load stamp into clipboard and enter paste mode */
                if (stampLoad(ed->stampLib.entries[stampIdx].filePath,
                              ed->clipTerrain, &ed->clipW, &ed->clipH,
                              ed->clipPills, &ed->clipNumPills,
                              ed->clipBases, &ed->clipNumBases,
                              ed->clipStarts, &ed->clipNumStarts)) {
                    ed->hasClipboard = true;
                    ed->isPasting = true;
                    meSetStatus(ed, "Loaded stamp: %s", ed->stampLib.entries[stampIdx].name);
                }
            }
        }

        /* Save stamp modal */
        if (ed->showSaveStampDialog) {
            ImGui_OpenPopup(langGetText(STR_MAPEDIT_STAMP_SAVE_TITLE));
            ed->showSaveStampDialog = false;
        }
        {
            int saveResult = mapEditorImguiSaveStampModal(ed->saveStampName,
                                                          (int)sizeof(ed->saveStampName));
            if (saveResult == 1) {
                /* Save confirmed — sanitize name and write file */
                char filename[128];
                int fi = 0;
                for (int si = 0; ed->saveStampName[si] && fi < 120; si++) {
                    char c = ed->saveStampName[si];
                    if (c == ' ') filename[fi++] = '_';
                    else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_')
                        filename[fi++] = c;
                }
                filename[fi] = '\0';
                if (fi == 0) {
                    snprintf(filename, sizeof(filename), "Untitled.bstamp");
                } else {
                    snprintf(filename + fi, sizeof(filename) - fi, ".bstamp");
                }

                char userDir[512];
                stampGetUserDir(userDir, sizeof(userDir));
                char fullPath[640];
                snprintf(fullPath, sizeof(fullPath), "%s/%s", userDir, filename);

                if (stampSave(fullPath, ed->clipW, ed->clipH, ed->clipTerrain,
                              ed->clipNumPills, ed->clipPills,
                              ed->clipNumBases, ed->clipBases,
                              ed->clipNumStarts, ed->clipStarts)) {
                    const char *base = SDL_GetBasePath();
                    char dataDir[512];
                    snprintf(dataDir, sizeof(dataDir), "%sdata", base ? base : "");
                    stampLibraryRefresh(&ed->stampLib, dataDir);
                    meSetStatus(ed, "Stamp saved: %s", ed->saveStampName);
                } else {
                    meSetStatus(ed, "Failed to save stamp");
                }
            }
        }

        /* Validation error modal (blocks saving) */
        if (ed->validationErrorModalOpen) {
            ImGui_OpenPopup(langGetText(STR_MAPEDIT_VAL_ERRORS_TITLE));
            ed->validationErrorModalOpen = false;
        }
        mapEditorImguiValidationErrorModal(ed->lastValidation.errorCount);

        /* Unsaved changes modal */
        int unsavedChoice = mapEditorImguiUnsavedModal();
        if (unsavedChoice > 0) {
            meHandleUnsavedChoice(ed, unsavedChoice);
        }

        /* Error modal */
        if (ed->errorModalOpen) {
            if (mapEditorImguiErrorModal(ed->errorMessage)) {
                ed->errorMessage[0] = '\0';
                ed->errorModalOpen = false;
            }
        }

        /* Status bar */
        char startInfo[128];
        const char *startInfoPtr = NULL;
        /* Show status message if recent (within 3 seconds), else show starts count */
        if (ed->statusMessage[0] && (SDL_GetTicks() - ed->statusMessageTime) < 3000) {
            startInfoPtr = ed->statusMessage;
        } else {
            ed->statusMessage[0] = '\0';
            if (ed->ss->numStarts > 0) {
                snprintf(startInfo, sizeof(startInfo), "Starts: %d", ed->ss->numStarts);
                startInfoPtr = startInfo;
            }
        }

        mapEditorImguiStatusBar(hoverMX, hoverMY, ed->zoomLevel, ed->dirty, startInfoPtr);

        mapEditorImguiRender();
        SDL_RenderPresent(renderer);
    }

    /* Cleanup */
    stampLibraryFree(&ed->stampLib);
    meScenarioFree(&ed->scn);
    meScenarioPackedClear(&ed->scnPacked);
    imageImportFree(&ed->imageImportCfg);
    validateResultFree(&ed->lastValidation);
    macOSPinchZoomDestroy();
#ifdef MAPEDITOR_WBN_OPEN
    /* The preview worker and any in-flight download must not outlive
     * the editor — the client goes back to the welcome screen. */
    meWbnOpenShutdown();
#endif
#ifdef __APPLE__
    /* Restore the previously-installed NSMenu (WinBolo's, when embedded;
     * empty stub when standalone since the process is exiting). */
    me_mac_menubar_uninstall();
#endif
    mapEditorImguiShutdown();
    s_appQuitRequested = ed->exitIsAppQuit;
    undoStackClear(&ed->undoStack);
    if (ed->offscreenTex) SDL_DestroyTexture(ed->offscreenTex);
    if (ed->minimapTex) SDL_DestroyTexture(ed->minimapTex);
    if (ed->tilesTex) SDL_DestroyTexture(ed->tilesTex);
    meFreeMapData(ed);
    free(ed->brushSeen);
    free(ed);
}
