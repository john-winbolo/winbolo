/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_imgui.cpp
 * Purpose:
 *   ImGui integration for the map editor. Provides a
 *   C-compatible wrapper around ImGui C++ calls.
 *********************************************************/

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifdef _WIN32
#include <WinSock2.h>
#endif

#include <SDL3/SDL.h>
#include <string.h>
#include <cstdio>

#include "imgui.h"
#include "imgui_theme.h"
#include "imgui_fonts.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "mapeditor_imgui.h"
#include "mapgen.h"
#include "mapgen_maze.h"
#include "mapeditor_text.h"
#include "mapeditor_fonts.h"
#include "mapeditor_validate.h"
#include "mapeditor_stats.h"
#include "mapeditor_stamp.h"
#include "mapeditor_wbn_open.h"  /* meWbnOpenAvailable — empty unless MAPEDITOR_WBN_OPEN */
#include "tilenum.h"
#include "../gui/lang.h"
#include "../gui/tiles.h"
#include "../gui/sdl3/minimap_render.h"
#include "../gui/sdl3/dialogs/dialog_footer.h"

static SDL_Renderer *s_meRenderer = nullptr;

/* The window the editor draws into, kept the way the renderer above is: the
 * scenario panel's script view needs SDL text input running on it, and that
 * takes the window. Embedded, this is WinBolo's own window. */
static SDL_Window *s_meWindow = nullptr;

/* Whether this file is what turned SDL text input on, so it never stops input
 * something else asked for. */
static bool s_meStartedTextInput = false;

/* The scenario panel's script view is a vendored widget that reads characters
 * out of io.InputQueueCharacters. That queue fills from SDL's text-input
 * events, and SDL3 sends none until text input has been started on the window.
 * The widget asks for them by assigning io.WantTextInput, which imgui
 * recomputes every frame from its own InputText state, so the assignment never
 * reaches the backend and no character ever arrives. That is why the arrow
 * keys, Enter and Backspace worked in that view and letters did not: those
 * come in as key events, a separate path.
 *
 * The backend starts and stops text input off imgui's IME data, which imgui
 * hands it only when that data changes, so this acts on the change rather than
 * calling every frame. SDL's own flag is read each time because the backend
 * stops text input when a field elsewhere in the editor loses focus, and the
 * view has to get it back after that. */
static void meApplyScriptTextInput(bool want) {
    if (s_meWindow == nullptr) {
        return;
    }

    if (want) {
        if (!SDL_TextInputActive(s_meWindow)) {
            SDL_StartTextInput(s_meWindow);
        }
        s_meStartedTextInput = true;
        return;
    }

    if (!s_meStartedTextInput) {
        return;
    }
    /* A field somewhere else in the editor is taking text. Leave it running
     * and hand it back on a later frame rather than taking the keyboard off
     * something the user is typing into. */
    if (ImGui::GetIO().WantTextInput) {
        return;
    }
    if (SDL_TextInputActive(s_meWindow)) {
        SDL_StopTextInput(s_meWindow);
    }
    s_meStartedTextInput = false;
}

/* Track display size to reposition windows on resize */
static float s_lastDisplayW = 0;
static float s_lastDisplayH = 0;

static bool meDisplaySizeChanged(void) {
    ImGuiIO &io = ImGui::GetIO();
    if (io.DisplaySize.x != s_lastDisplayW || io.DisplaySize.y != s_lastDisplayH) {
        s_lastDisplayW = io.DisplaySize.x;
        s_lastDisplayH = io.DisplaySize.y;
        return true;
    }
    return false;
}

/* Forward declarations for tool icon cleanup */
#define ME_NUM_DRAWING_TOOLS 11
static SDL_Texture *s_toolIcons[ME_NUM_DRAWING_TOOLS];
static bool s_toolIconsLoaded = false;

/* Text tool icon texture */
static SDL_Texture *s_textToolIcon = nullptr;
static bool s_textToolIconLoaded = false;

void mapEditorImguiInit(SDL_Window *window, SDL_Renderer *renderer) {
    mapEditorImguiSetRenderer(renderer);
    s_meRenderer = renderer;
    s_meWindow = window;
    s_meStartedTextInput = false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    imguiLoadBoloFont(18.0f);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
}

void mapEditorImguiShutdown(void) {
    /* Embedded, the window is WinBolo's and outlives the editor, so text input
     * the script view started does not stay on after the editor has gone. */
    if (s_meStartedTextInput && s_meWindow != nullptr &&
        SDL_TextInputActive(s_meWindow)) {
        SDL_StopTextInput(s_meWindow);
    }
    s_meStartedTextInput = false;

    /* Clean up tool icon textures */
    for (int i = 0; i < ME_NUM_DRAWING_TOOLS; i++) {
        if (s_toolIcons[i]) {
            SDL_DestroyTexture(s_toolIcons[i]);
            s_toolIcons[i] = nullptr;
        }
    }
    s_toolIconsLoaded = false;

    if (s_textToolIcon) { SDL_DestroyTexture(s_textToolIcon); s_textToolIcon = nullptr; }
    s_textToolIconLoaded = false;

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    s_meRenderer = nullptr;
    s_meWindow = nullptr;
}

bool mapEditorImguiProcessEvent(const SDL_Event *event) {
    return ImGui_ImplSDL3_ProcessEvent(event);
}

bool mapEditorImguiWantKeyboard(void) {
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool mapEditorImguiWantMouse(void) {
    return ImGui::GetIO().WantCaptureMouse;
}

static bool s_displayResized = false;

void mapEditorImguiNewFrame(void) {
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    s_displayResized = meDisplaySizeChanged();
    /* After ImGui::NewFrame, so io.WantTextInput is this frame's answer, and
     * after the backend's own text-input pass, so a start here is not undone
     * in the same frame. The mark is the frame that just ended, which is soon
     * enough: the view has to be clicked into before a character is typed. */
    meApplyScriptTextInput(mapEditorImguiScriptViewDrew());
}

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
                           bool *showStampLibrary, bool *showScenario,
                           bool fromMainMenu,
                           int zoomStepIndex, int zoomStepCount,
                           const float *zoomStepValues) {
#ifdef __APPLE__
    /* No-op on macOS — the native NSMenu shim owns the menu bar. The
     * caller in mapeditor.c does NOT invoke this function on Apple; it
     * calls me_mac_menubar_consume_actions() instead, which is what
     * zeroes `action` and fills it from the NSMenu pending-action
     * buffer. Doing the memset here would race with NSMenu clicks
     * arriving between frames, so we leave `action` untouched. */
    (void)action; (void)recentFiles; (void)numRecent;
    (void)showGrid; (void)showMines; (void)showPillRanges; (void)dirty;
    (void)canUndo; (void)canRedo; (void)hasSelection;
    (void)showTerrain; (void)showTools; (void)showInspector;
    (void)showObjects; (void)showOverview; (void)showStats;
    (void)showStampLibrary; (void)showScenario; (void)fromMainMenu;
    (void)zoomStepIndex; (void)zoomStepCount; (void)zoomStepValues;
    return;
#else
    memset(action, 0, sizeof(*action));
    action->openRecentIndex = -1;

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(langGetText(STR_MENU_FILE))) {
            if (ImGui::MenuItem(langGetText(STR_MENU_NEW), "Ctrl+N")) {
                action->wantNew = true;
            }
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_OPEN), "Ctrl+O")) {
                action->wantOpen = true;
            }
#ifdef MAPEDITOR_WBN_OPEN
            /* Greyed out when the WBN HTTP layer failed to come up. */
            if (ImGui::MenuItem(langGetText(STR_LV_MENU_OPEN_WBN), nullptr,
                                false, meWbnOpenAvailable())) {
                action->wantOpenWbn = true;
            }
#endif
            if (ImGui::BeginMenu(langGetText(STR_MAPEDIT_MENU_RECENT), numRecent > 0)) {
                for (int i = 0; i < numRecent; i++) {
                    /* Show just the filename, not the full path */
                    const char *name = recentFiles[i];
                    const char *slash = strrchr(name, '/');
                    if (!slash) slash = strrchr(name, '\\');
                    if (slash) name = slash + 1;
                    ImGui::PushID(i);
                    if (ImGui::MenuItem(name)) {
                        action->openRecentIndex = i;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", recentFiles[i]);
                    }
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_SAVE), "Ctrl+S")) {
                action->wantSave = true;
            }
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_SAVEAS), "Ctrl+Shift+S")) {
                action->wantSaveAs = true;
            }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_EXPORT))) {
                action->wantExportPNG = true;
            }
#endif
            ImGui::Separator();
            if (fromMainMenu) {
                if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_RETURN))) {
                    action->wantExit = true;
                }
            } else {
                if (ImGui::MenuItem(langGetText(STR_MENU_EXIT), "Ctrl+Q")) {
                    action->wantExit = true;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MENU_EDIT))) {
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_UNDO), "Ctrl+Z", false, canUndo)) {
                action->wantUndo = true;
            }
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_REDO), "Ctrl+Y", false, canRedo)) {
                action->wantRedo = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_CUT), "Ctrl+X", false, hasSelection)) {
                action->wantCut = true;
            }
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_COPY), "Ctrl+C", false, hasSelection)) {
                action->wantCopy = true;
            }
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_PASTE), "Ctrl+V")) {
                action->wantPaste = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MAPEDIT_MENU_MAP))) {
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_RANDOMMAP), nullptr)) {
                action->wantGenerate = true;
            }
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_TEXT), nullptr)) {
                action->wantText = true;
            }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_IMPORTIMG), nullptr)) {
                action->wantImageImport = true;
            }
#endif
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_VALIDATE), "Ctrl+Shift+V")) {
                action->wantValidate = true;
            }
            ImGui::Separator();
            {
                MessageArgs scopeArgs = {};
                const char *scope = langGetText(hasSelection
                                                ? STR_MAPEDIT_SCOPE_SELECTION
                                                : STR_MAPEDIT_SCOPE_FULLMAP);
                SDL_strlcpy(scopeArgs.string1, scope, sizeof(scopeArgs.string1));
                if (ImGui::MenuItem(langGetTextFmt(STR_MAPEDIT_MENU_MIRROR_H, &scopeArgs))) {
                    action->wantSymMirrorH = true;
                }
                if (ImGui::MenuItem(langGetTextFmt(STR_MAPEDIT_MENU_MIRROR_V, &scopeArgs))) {
                    action->wantSymMirrorV = true;
                }
                if (ImGui::MenuItem(langGetTextFmt(STR_MAPEDIT_MENU_ROTATE_90, &scopeArgs))) {
                    action->wantSymRotate90 = true;
                }
                if (ImGui::MenuItem(langGetTextFmt(STR_MAPEDIT_MENU_ROTATE_180, &scopeArgs))) {
                    action->wantSymRotate180 = true;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MAPEDIT_MENU_OPTIONS))) {
            /* Zoom submenu */
            if (ImGui::BeginMenu(langGetText(STR_LV_ZOOM))) {
                for (int i = 0; i < zoomStepCount; i++) {
                    float val = zoomStepValues[i];
                    char label[32];
                    if (val == (float)(int)val) {
                        snprintf(label, sizeof(label), "%dx", (int)val);
                    } else {
                        snprintf(label, sizeof(label), "%.1fx", val);
                    }
                    if (ImGui::MenuItem(label, NULL, i == zoomStepIndex)) {
                        action->wantZoomSet = true;
                        action->zoomSetIndex = i;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(langGetText(STR_LV_ZOOM_IN), "+", false, zoomStepIndex < zoomStepCount - 1)) {
                    action->wantZoomIn = true;
                }
                if (ImGui::MenuItem(langGetText(STR_LV_ZOOM_OUT), "-", false, zoomStepIndex > 0)) {
                    action->wantZoomOut = true;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_CENTER), "Home")) {
                action->wantCenter = true;
            }
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_POINT_STARTS))) {
                action->wantPointStarts = true;
            }
            ImGui::Separator();
            bool grid = showGrid;
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_SHOWGRID), "G", &grid)) {
                action->toggleGrid = true;
            }
            bool mines = showMines;
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_SHOWMINES), "M", &mines)) {
                action->toggleMines = true;
            }
            bool pillRanges = showPillRanges;
            if (ImGui::MenuItem(langGetText(STR_MAPEDIT_MENU_SHOWPILLRANGES), NULL, &pillRanges)) {
                action->togglePillRanges = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(langGetText(STR_MAPEDIT_MENU_WINDOW))) {
            ImGui::MenuItem(langGetText(STR_MAPEDIT_WIN_TERRAIN),   "Ctrl+1", showTerrain);
            ImGui::MenuItem(langGetText(STR_MAPEDIT_WIN_TOOLS),     "Ctrl+2", showTools);
            ImGui::MenuItem(langGetText(STR_MAPEDIT_WIN_INSPECTOR), "Ctrl+3", showInspector);
            ImGui::MenuItem(langGetText(STR_MAPEDIT_WIN_OBJECTS),   "Ctrl+4", showObjects);
            ImGui::MenuItem(langGetText(STR_MAPEDIT_WIN_OVERVIEW),  "Ctrl+5", showOverview);
            ImGui::MenuItem(langGetText(STR_MAPEDIT_WIN_STATS), "Ctrl+7", showStats);
            ImGui::MenuItem(langGetText(STR_MAPEDIT_WIN_STAMP_LIB), "Ctrl+6", showStampLibrary);
            ImGui::MenuItem(langGetText(STR_MAPEDIT_SCENARIO_TITLE), "Ctrl+8", showScenario);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
#endif /* __APPLE__ */
}

int mapEditorImguiUnsavedModal(void) {
    int result = 0;
    static bool s_unsavedOpen = true; s_unsavedOpen = true;
    if (ImGui::BeginPopupModal(langGetText(STR_MAPEDIT_UNSAVED_TITLE), &s_unsavedOpen,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_UNSAVED_BLURB));
        /* [Cancel: stay][Destructive: No=discard][Primary: Yes=save]
         * Return codes preserved: 1=save, 2=discard, 3=cancel. */
        int f = WBUI::DialogFooter3(langGetText(STR_CANCEL),
                                    langGetText(STR_NO),
                                    langGetText(STR_YES));
        if (f == WBUI::FOOTER_CONFIRM) {
            result = 1; ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_DESTRUCTIVE) {
            result = 2; ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_CANCEL) {
            result = 3; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return result;
}

bool mapEditorImguiErrorModal(const char *message) {
    bool dismissed = false;
    static bool s_meErrOpen = true; s_meErrOpen = true;
    if (ImGui::BeginPopupModal(langGetText(STR_ERR_TITLE), &s_meErrOpen,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("%s", message);
        int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                                   langGetText(STR_OK));
        if (f != WBUI::FOOTER_NONE) {
            dismissed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return dismissed;
}

void mapEditorImguiStatusBar(int tileX, int tileY, float zoomLevel,
                             bool dirty, const char *startInfo) {
    ImGuiIO &io = ImGui::GetIO();
    float barH = ImGui::GetFrameHeightWithSpacing();
    float winW = io.DisplaySize.x;
    float winH = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, winH - barH));
    ImGui::SetNextWindowSize(ImVec2(winW, barH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 2.0f));
    ImGui::Begin("##StatusBar", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    if (tileX >= 0 && tileY >= 0) {
        MessageArgs args = {};
        args.number = tileX;
        args.number2 = tileY;
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_STATUS_TILE, &args));
    } else {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_STATUS_TILE_NONE));
    }
    ImGui::SameLine(winW * 0.3f);
    {
        MessageArgs args = {};
        if (zoomLevel < 1.0f)
            SDL_snprintf(args.string1, sizeof(args.string1), "%.1fx", zoomLevel);
        else
            SDL_snprintf(args.string1, sizeof(args.string1), "%dx", (int)zoomLevel);
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_STATUS_ZOOM, &args));
    }
    ImGui::SameLine(winW * 0.5f);
    if (dirty) {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_STATUS_MODIFIED));
    }
    if (startInfo) {
        ImGui::SameLine(winW * 0.7f);
        ImGui::Text("%s", startInfo);
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}

/* -------------------------------------------------------
 * Terrain Palette — the 10 paintable terrain types
 * ------------------------------------------------------- */

static float s_terrainWindowBottom = 20.0f;

#include "../gui/sdl3/sprite_positions.h"

/* The 11 paintable terrain entries. Keep in sync with mapeditor.c meTerrainEntries.
 * `name` is a stable English string used as the ImGui widget ID;
 * `nameId` is the localized display label used in tooltips. */
static const struct {
    const char *name;
    langid      nameId;
    unsigned char rawValue;
    unsigned char iconTile;
    bool canMine;
    const char *shortcut;
} s_terrainEntries[] = {
    { "Deep Sea",      STR_MAPEDIT_TERR_DEEPSEA,       0xFF, 113, false, "0" },
    { "Grass",         STR_MAPEDIT_TERR_GRASS,         7,    7,   true,  "1" },
    { "Forest",        STR_MAPEDIT_TERR_FOREST,        5,    164, true,  "2" },
    { "Road",          STR_MAPEDIT_TERR_ROAD,          4,    27,  true,  "3" },
    { "Building",      STR_MAPEDIT_TERR_BUILDING,      0,    57,  false, "4" },
    { "Half Building", STR_MAPEDIT_TERR_HALFBUILDING,  8,    8,   false, "9" },
    { "River",         STR_MAPEDIT_TERR_RIVER,         1,    101, false, "5" },
    { "Swamp",         STR_MAPEDIT_TERR_SWAMP,         2,    2,   true,  "6" },
    { "Crater",        STR_MAPEDIT_TERR_CRATER,        3,    3,   true,  "7" },
    { "Rubble",        STR_MAPEDIT_TERR_RUBBLE,        6,    6,   true,  "8" },
    { "Boat",          STR_MAPEDIT_TERR_BOAT,          9,    138, false, "`" },
};
#define ME_NUM_TERRAINS 11

bool mapEditorImguiTerrainPalette(void *tilesTex, int tileSize,
                                  int *activeTerrain, bool *mineMode,
                                  int *minePattern,
                                  bool *p_open) {
    bool changed = false;
    const float atlasW = 496.0f;
    const float atlasH = 176.0f;
    const float btnSize = 32.0f;

    ImGuiCond posCond = s_displayResized ? ImGuiCond_Always : ImGuiCond_Once;
    ImGui::SetNextWindowPos(ImVec2(0, 20), posCond);
    ImGui::SetNextWindowSize(ImVec2(110, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin(langGetText(STR_MAPEDIT_WIN_TERRAIN), p_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    for (int i = 0; i < ME_NUM_TERRAINS; i++) {
        int tile = s_terrainEntries[i].iconTile;
        float u0 = (float)mapViewPosX[tile] / atlasW;
        float v0 = (float)mapViewPosY[tile] / atlasH;
        float u1 = u0 + (float)tileSize / atlasW;
        float v1 = v0 + (float)tileSize / atlasH;

        ImGui::PushID(i);
        bool selected = (*activeTerrain == i && !*mineMode);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.85f, 1.0f));
        }

        if (ImGui::ImageButton(s_terrainEntries[i].name,
                               (ImTextureID)tilesTex,
                               ImVec2(btnSize, btnSize),
                               ImVec2(u0, v0), ImVec2(u1, v1))) {
            *activeTerrain = i;
            *mineMode = false;
            changed = true;
        }
        if (selected) {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(rMin, rMax,
                IM_COL32(80, 160, 255, 255), 0.0f, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)", langGetText(s_terrainEntries[i].nameId), s_terrainEntries[i].shortcut);
        }

        if (selected) {
            ImGui::PopStyleColor(3);
        }
        ImGui::PopID();

        /* 2-column grid */
        if (i % 2 == 0) ImGui::SameLine();
    }

    /* Mine tool icon — next to Boat in the 2-column grid */
    {
        /* MINE_X = 19 * tileSize, MINE_Y = 3 * tileSize */
        float mineU0 = (19.0f * tileSize) / atlasW;
        float mineV0 = (3.0f * tileSize) / atlasH;
        float mineU1 = mineU0 + (float)tileSize / atlasW;
        float mineV1 = mineV0 + (float)tileSize / atlasH;

        ImGui::PushID("mine_tool");
        bool mineSelected = *mineMode;
        if (mineSelected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.30f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        }

        if (ImGui::ImageButton("Mine",
                               (ImTextureID)tilesTex,
                               ImVec2(btnSize, btnSize),
                               ImVec2(mineU0, mineV0), ImVec2(mineU1, mineV1))) {
            *mineMode = !*mineMode;
            changed = true;
        }
        if (mineSelected) {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(rMin, rMax,
                IM_COL32(255, 80, 80, 255), 0.0f, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (N)", langGetText(STR_MAPEDIT_TERR_MINETOOL));
        }
        if (mineSelected) {
            ImGui::PopStyleColor(3);
        }
        ImGui::PopID();

        /* Mine pattern combo — only shown when mine tool is active */
        if (*mineMode) {
            const char *patternNames[ME_MINE_PATTERN_COUNT] = {
                langGetText(STR_MAPEDIT_MINE_CHECKERED),
                langGetText(STR_MAPEDIT_MINE_FULL),
                langGetText(STR_MAPEDIT_MINE_RANDOM),
                langGetText(STR_MAPEDIT_MINE_CLEAR),
            };
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##MinePattern", minePattern, patternNames, ME_MINE_PATTERN_COUNT)) {
                changed = true;
            }
        }
    }

    s_terrainWindowBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
    return changed;
}

/* -------------------------------------------------------
 * Tools Toolbar — SVG icon buttons in a 2-column grid
 * ------------------------------------------------------- */

#include "nanosvg.h"
#include "nanosvgrast.h"

static const struct {
    int         toolIdx;
    const char *svgFile;
    const char *name;     /* stable English string used as widget ID */
    langid      nameId;   /* localized name shown in tooltips */
    const char *shortcut;
} s_drawingTools[ME_NUM_DRAWING_TOOLS] = {
    { ME_TOOL_PENCIL,    "data/ui/mapeditor/pencil.svg",             "Pencil",           STR_MAPEDIT_TOOL_PENCIL,    "P" },
    { ME_TOOL_LINE,      "data/ui/mapeditor/line.svg",               "Line",             STR_MAPEDIT_TOOL_LINE,      "L" },
    { ME_TOOL_RECT,      "data/ui/mapeditor/rectangle.svg",          "Rectangle",        STR_MAPEDIT_TOOL_RECT,      "R" },
    { ME_TOOL_RECT_FILL, "data/ui/mapeditor/filled_rectangle.svg",   "Filled Rectangle", STR_MAPEDIT_TOOL_RECTFILL,  "Shift+R" },
    { ME_TOOL_OVAL,      "data/ui/mapeditor/oval.svg",               "Oval",             STR_MAPEDIT_TOOL_OVAL,      "O" },
    { ME_TOOL_OVAL_FILL, "data/ui/mapeditor/filled_oval.svg",        "Filled Oval",      STR_MAPEDIT_TOOL_OVALFILL,  "Shift+O" },
    { ME_TOOL_SELECT,    "data/ui/mapeditor/selection.svg",          "Selection",        STR_MAPEDIT_TOOL_SELECT,    "S" },
    { ME_TOOL_FILL,      "data/ui/mapeditor/fill.svg",               "Fill",             STR_MAPEDIT_TOOL_FILL,      "F" },
    { ME_TOOL_MAZE,      "data/ui/mapeditor/maze.svg",               "Maze",             STR_MAPEDIT_TOOL_MAZE,      "M" },
    { ME_TOOL_GENERATE,  "data/ui/mapeditor/random.svg",             "Generate",         STR_MAPEDIT_TOOL_GENERATE,  "Shift+G" },
    { ME_TOOL_WAND,      "data/ui/mapeditor/wand.svg",              "Wand",              STR_MAPEDIT_TOOL_WAND,      "W" },
};

static void meLoadToolIcons(void) {
    if (s_toolIconsLoaded) return;
    s_toolIconsLoaded = true;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) return;

    const int iconPx = 32;

    for (int i = 0; i < ME_NUM_DRAWING_TOOLS; i++) {
        NSVGimage *img = nsvgParseFromFile(s_drawingTools[i].svgFile, "px", 96.0f);
        if (!img) continue;
        if (img->width < 1.0f || img->height < 1.0f) { nsvgDelete(img); continue; }

        float scale = (float)iconPx / (img->width > img->height ? img->width : img->height);
        int w = iconPx, h = iconPx;
        unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
        if (!pixels) { nsvgDelete(img); continue; }

        memset(pixels, 0, (size_t)(w * h * 4));

        /* Center the icon if it's not square */
        float ox = (iconPx - img->width * scale) * 0.5f;
        float oy = (iconPx - img->height * scale) * 0.5f;
        nsvgRasterize(rast, img, ox, oy, scale, pixels, w, h, w * 4);
        nsvgDelete(img);

        /* Tint all pixels white so the icon works on dark backgrounds:
         * keep the alpha from nanosvg, set RGB to 255. */
        for (int p = 0; p < w * h; p++) {
            unsigned char a = pixels[p * 4 + 3];
            pixels[p * 4 + 0] = 255; /* R */
            pixels[p * 4 + 1] = 255; /* G */
            pixels[p * 4 + 2] = 255; /* B */
            pixels[p * 4 + 3] = a;
        }

        SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32,
                                                   pixels, w * 4);
        if (surf) {
            s_toolIcons[i] = SDL_CreateTextureFromSurface(s_meRenderer, surf);
            SDL_DestroySurface(surf);
        }
        SDL_free(pixels);
    }

    nsvgDeleteRasterizer(rast);
}

bool mapEditorImguiToolbar(int *activeTool,
                           void *tilesTex, int tileSize,
                           int *brushSize, int *brushShape,
                           bool *p_open, bool *wantText) {
    bool changed = false;
    const float atlasW = 496.0f;
    const float atlasH = 176.0f;
    const float btnSize = 28.0f;

    meLoadToolIcons();

    /* Position below terrain window; reposition on display resize */
    static int s_toolsPosFrames = 0;
    if (s_toolsPosFrames < 3 || s_displayResized) {
        ImGui::SetNextWindowPos(ImVec2(0, s_terrainWindowBottom));
        if (s_toolsPosFrames < 3) s_toolsPosFrames++;
    }
    ImGui::SetNextWindowSize(ImVec2(120, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin(langGetText(STR_MAPEDIT_WIN_TOOLS), p_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    /* Drawing tools — SVG icon buttons in 2-column grid */
    for (int i = 0; i < ME_NUM_DRAWING_TOOLS; i++) {
        ImGui::PushID(i);
        bool selected = (*activeTool == s_drawingTools[i].toolIdx);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.85f, 1.0f));
        }

        if (s_toolIcons[i]) {
            if (ImGui::ImageButton(s_drawingTools[i].name,
                                   (ImTextureID)s_toolIcons[i],
                                   ImVec2(btnSize, btnSize))) {
                *activeTool = s_drawingTools[i].toolIdx;
                changed = true;
            }
        } else {
            /* Fallback: text button if SVG failed to load */
            if (ImGui::Button(langGetText(s_drawingTools[i].nameId), ImVec2(btnSize + 8, btnSize + 8))) {
                *activeTool = s_drawingTools[i].toolIdx;
                changed = true;
            }
        }
        if (selected) {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(rMin, rMax,
                IM_COL32(80, 160, 255, 255), 0.0f, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)", langGetText(s_drawingTools[i].nameId), s_drawingTools[i].shortcut);
        }

        if (selected) {
            ImGui::PopStyleColor(3);
        }
        ImGui::PopID();

        /* 2-column grid: SameLine after odd columns (0, 2, 4, 6) */
        if (i % 2 == 0) ImGui::SameLine();
    }

    /* Text tool button — opens dialog, not a drawing tool */
    {
        if (!s_textToolIconLoaded) {
            s_textToolIconLoaded = true;
            NSVGrasterizer *rast = nsvgCreateRasterizer();
            if (rast) {
                NSVGimage *img = nsvgParseFromFile("data/ui/mapeditor/text.svg", "px", 96.0f);
                if (img && img->width >= 1.0f && img->height >= 1.0f) {
                    const int iconPx = 32;
                    float scale = (float)iconPx / (img->width > img->height ? img->width : img->height);
                    unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(iconPx * iconPx * 4));
                    if (pixels) {
                        memset(pixels, 0, (size_t)(iconPx * iconPx * 4));
                        float ox = (iconPx - img->width * scale) * 0.5f;
                        float oy = (iconPx - img->height * scale) * 0.5f;
                        nsvgRasterize(rast, img, ox, oy, scale, pixels, iconPx, iconPx, iconPx * 4);
                        for (int p = 0; p < iconPx * iconPx; p++) {
                            unsigned char a = pixels[p * 4 + 3];
                            pixels[p * 4 + 0] = 255;
                            pixels[p * 4 + 1] = 255;
                            pixels[p * 4 + 2] = 255;
                            pixels[p * 4 + 3] = a;
                        }
                        SDL_Surface *surf = SDL_CreateSurfaceFrom(iconPx, iconPx, SDL_PIXELFORMAT_RGBA32,
                                                                   pixels, iconPx * 4);
                        if (surf) {
                            s_textToolIcon = SDL_CreateTextureFromSurface(s_meRenderer, surf);
                            SDL_DestroySurface(surf);
                        }
                        SDL_free(pixels);
                    }
                }
                if (img) nsvgDelete(img);
                nsvgDeleteRasterizer(rast);
            }
        }

        ImGui::PushID(200);
        if (s_textToolIcon) {
            if (ImGui::ImageButton("Text", (ImTextureID)s_textToolIcon,
                                   ImVec2(btnSize, btnSize))) {
                if (wantText) *wantText = true;
            }
        } else {
            if (ImGui::Button(langGetText(STR_MAPEDIT_TOOL_TEXT), ImVec2(btnSize + 8, btnSize + 8))) {
                if (wantText) *wantText = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(STR_MAPEDIT_TOOL_TEXT_TIP));
        }
        ImGui::PopID();
    }

    /* Brush size selector */
    ImGui::Separator();
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_BRUSH));
    static const char *sizeLabels[] = { "1", "3", "5", "7", "9" };
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##BrushSize", brushSize, sizeLabels, ME_BRUSH_SIZE_COUNT)) {
        changed = true;
    }
    ImGui::RadioButton(langGetText(STR_MAPEDIT_BRUSH_SQUARE), brushShape, ME_BRUSH_SQUARE);
    ImGui::SameLine();
    ImGui::RadioButton(langGetText(STR_MAPEDIT_BRUSH_CIRCLE), brushShape, ME_BRUSH_CIRCLE);

    /* Object placement tools — icon buttons from tile atlas */
    ImGui::Separator();
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_WIN_OBJECTS));

    struct { int tileX; int tileY; int toolIdx; langid tooltipId; const char *shortcut; const char *idStr; } objTools[] = {
        { mapViewPosX[BASE_NEUTRAL],  mapViewPosY[BASE_NEUTRAL],  ME_TOOL_PLACE_BASE,  STR_MAPEDIT_OBJTOOL_BASE,  "B", "Base" },
        { mapViewPosX[PILL_EVIL_15],  mapViewPosY[PILL_EVIL_15],  ME_TOOL_PLACE_PILL,  STR_MAPEDIT_OBJTOOL_PILL,  "I", "Pillbox" },
        { TANK_SELFBOAT_0_X,         TANK_SELFBOAT_0_Y,          ME_TOOL_PLACE_START, STR_MAPEDIT_OBJTOOL_START, "T", "Start" },
    };

    for (int i = 0; i < 3; i++) {
        float u0 = (float)objTools[i].tileX / atlasW;
        float v0 = (float)objTools[i].tileY / atlasH;
        float u1 = u0 + (float)tileSize / atlasW;
        float v1 = v0 + (float)tileSize / atlasH;

        ImGui::PushID(100 + i);
        bool selected = (*activeTool == objTools[i].toolIdx);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.85f, 1.0f));
        }
        if (ImGui::ImageButton(objTools[i].idStr,
                               (ImTextureID)tilesTex,
                               ImVec2(btnSize, btnSize),
                               ImVec2(u0, v0), ImVec2(u1, v1))) {
            *activeTool = objTools[i].toolIdx;
            changed = true;
        }
        if (selected) {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(rMin, rMax,
                IM_COL32(80, 160, 255, 255), 0.0f, 0, 2.0f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)", langGetText(objTools[i].tooltipId), objTools[i].shortcut);
        }
        if (selected) {
            ImGui::PopStyleColor(3);
        }
        ImGui::PopID();

        /* Row 1: Base + Pillbox, Row 2: Start */
        if (i == 0) ImGui::SameLine();
    }

    ImGui::End();
    return changed;
}

/* -------------------------------------------------------
 * Inspector Panel
 * ------------------------------------------------------- */

#include "mapeditor_undo.h"
#include "types.h"

#define ME_NUM_OWNERS 17

static const char *s_dirLabels[] = {
    "E", "ENE", "NE", "NNE", "N", "NNW", "NW", "WNW",
    "W", "WSW", "SW", "SSW", "S", "SSE", "SE", "ESE"
};

/* Convert owner byte (0-15 or 0xFF) to combo index (0=Neutral, 1-16=Player 0-15) */
static int ownerToCombo(unsigned char owner) {
    if (owner == 0xFF) return 0;
    if (owner < 16) return owner + 1;
    return 0;
}

static unsigned char comboToOwner(int combo) {
    if (combo == 0) return 0xFF;
    return (unsigned char)(combo - 1);
}

static float s_inspectorWindowBottom = 20.0f;

bool mapEditorImguiInspector(int selKind, int selIndex,
                              void *basesPtr, void *pillsPtr, void *startsPtr,
                              MEInspectorState *inspState,
                              void *undoStackPtr,
                              bool *p_open) {
    bool modified = false;
    bases bs = (bases)basesPtr;
    pillboxes pb = (pillboxes)pillsPtr;
    starts ss = (starts)startsPtr;
    UndoStack *undo = (UndoStack *)undoStackPtr;

    ImGuiCond posCond = s_displayResized ? ImGuiCond_Always : ImGuiCond_Once;
    ImGuiIO &iio = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(iio.DisplaySize.x - 260, 20), posCond);
    ImGui::SetNextWindowSize(ImVec2(250, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin(langGetText(STR_MAPEDIT_WIN_INSPECTOR), p_open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    if (selKind == ME_SEL_NONE || selKind < 0) {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_INSP_NOSEL));
        s_inspectorWindowBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
        ImGui::End();
        return false;
    }

    /* Build localized owner name list (Neutral + Player 0..15) */
    char ownerLabels[ME_NUM_OWNERS][32];
    const char *ownerLabelPtrs[ME_NUM_OWNERS];
    SDL_strlcpy(ownerLabels[0], langGetText(STR_MAPEDIT_OWNER_NEUTRAL), sizeof(ownerLabels[0]));
    ownerLabelPtrs[0] = ownerLabels[0];
    for (int oi = 1; oi < ME_NUM_OWNERS; oi++) {
        MessageArgs oargs = {};
        oargs.number = oi - 1;
        SDL_strlcpy(ownerLabels[oi], langGetTextFmt(STR_MAPEDIT_OWNER_PLAYER, &oargs), sizeof(ownerLabels[oi]));
        ownerLabelPtrs[oi] = ownerLabels[oi];
    }

    if (selKind == ME_SEL_BASE && selIndex >= 0 && selIndex < bs->numBases) {
        base *b = &bs->item[selIndex];
        {
            MessageArgs args = {};
            args.number = selIndex;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_INSP_BASE_HASH, &args));
        }
        {
            MessageArgs args = {};
            args.number = b->x;
            args.number2 = b->y;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_INSP_POSITION, &args));
        }

        int ownerCombo = ownerToCombo(b->owner);
        if (ImGui::Combo(langGetText(STR_MAPEDIT_INSP_OWNER), &ownerCombo, ownerLabelPtrs, ME_NUM_OWNERS)) {
            base old = *b;
            b->owner = comboToOwner(ownerCombo);
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_BASE, selIndex, &old, b);
            undoEndCommand(undo);
            modified = true;
        }

        /* Armour slider */
        int armour = b->armour;
        if (ImGui::SliderInt(langGetText(STR_MAPEDIT_INSP_ARMOUR), &armour, 0, 90)) {
            b->armour = (unsigned char)armour;
        }
        if (ImGui::IsItemActivated()) {
            inspState->editing = true;
            memcpy(inspState->snapshot, b, sizeof(base));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            base old;
            memcpy(&old, inspState->snapshot, sizeof(base));
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_BASE, selIndex, &old, b);
            undoEndCommand(undo);
            inspState->editing = false;
            modified = true;
        }

        /* Shells slider */
        int shells = b->shells;
        if (ImGui::SliderInt(langGetText(STR_TABLET_SHELLS), &shells, 0, 90)) {
            b->shells = (unsigned char)shells;
        }
        if (ImGui::IsItemActivated()) {
            inspState->editing = true;
            memcpy(inspState->snapshot, b, sizeof(base));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            base old;
            memcpy(&old, inspState->snapshot, sizeof(base));
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_BASE, selIndex, &old, b);
            undoEndCommand(undo);
            inspState->editing = false;
            modified = true;
        }

        /* Mines slider */
        int mines = b->mines;
        if (ImGui::SliderInt(langGetText(STR_TABLET_MINES), &mines, 0, 90)) {
            b->mines = (unsigned char)mines;
        }
        if (ImGui::IsItemActivated()) {
            inspState->editing = true;
            memcpy(inspState->snapshot, b, sizeof(base));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            base old;
            memcpy(&old, inspState->snapshot, sizeof(base));
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_BASE, selIndex, &old, b);
            undoEndCommand(undo);
            inspState->editing = false;
            modified = true;
        }
    } else if (selKind == ME_SEL_PILL && selIndex >= 0 && selIndex < pb->numPills) {
        pillbox *p = &pb->item[selIndex];
        {
            MessageArgs args = {};
            args.number = selIndex;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_INSP_PILL_HASH, &args));
        }
        {
            MessageArgs args = {};
            args.number = p->x;
            args.number2 = p->y;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_INSP_POSITION, &args));
        }

        int ownerCombo = ownerToCombo(p->owner);
        if (ImGui::Combo(langGetText(STR_MAPEDIT_INSP_OWNER), &ownerCombo, ownerLabelPtrs, ME_NUM_OWNERS)) {
            pillbox old = *p;
            p->owner = comboToOwner(ownerCombo);
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_PILL, selIndex, &old, p);
            undoEndCommand(undo);
            modified = true;
        }

        int armour = p->armour;
        if (ImGui::SliderInt(langGetText(STR_MAPEDIT_INSP_ARMOUR), &armour, 0, 15)) {
            p->armour = (unsigned char)armour;
        }
        if (ImGui::IsItemActivated()) {
            inspState->editing = true;
            memcpy(inspState->snapshot, p, sizeof(pillbox));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            pillbox old;
            memcpy(&old, inspState->snapshot, sizeof(pillbox));
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_PILL, selIndex, &old, p);
            undoEndCommand(undo);
            inspState->editing = false;
            modified = true;
        }

        int speed = p->speed;
        if (ImGui::SliderInt(langGetText(STR_MAPEDIT_INSP_SPEED), &speed, 1, 200)) {
            p->speed = (unsigned char)speed;
        }
        if (ImGui::IsItemActivated()) {
            inspState->editing = true;
            memcpy(inspState->snapshot, p, sizeof(pillbox));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            pillbox old;
            memcpy(&old, inspState->snapshot, sizeof(pillbox));
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_PILL, selIndex, &old, p);
            undoEndCommand(undo);
            inspState->editing = false;
            modified = true;
        }
    } else if (selKind == ME_SEL_START && selIndex >= 0 && selIndex < ss->numStarts) {
        start *s = &ss->item[selIndex];
        {
            MessageArgs args = {};
            args.number = selIndex;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_INSP_START_HASH, &args));
        }
        {
            MessageArgs args = {};
            args.number = s->x;
            args.number2 = s->y;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_INSP_POSITION, &args));
        }

        int dir = s->dir;
        if (ImGui::SliderInt(langGetText(STR_MAPEDIT_INSP_DIR), &dir, 0, 15)) {
            s->dir = (unsigned char)dir;
        }
        if (ImGui::IsItemActivated()) {
            inspState->editing = true;
            memcpy(inspState->snapshot, s, sizeof(start));
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            start old;
            memcpy(&old, inspState->snapshot, sizeof(start));
            undoBeginCommand(undo);
            undoRecordObjModify(undo, OBJ_START, selIndex, &old, s);
            undoEndCommand(undo);
            inspState->editing = false;
            modified = true;
        }
        if (s->dir < 16) {
            ImGui::Text("%s", s_dirLabels[s->dir]);
        }
    } else {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_INSP_NOSEL));
    }

    s_inspectorWindowBottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::End();
    return modified;
}

/* -------------------------------------------------------
 * Object List Panel
 * ------------------------------------------------------- */

void mapEditorImguiObjectList(void *basesPtr, void *pillsPtr,
                               void *startsPtr,
                               int selKind, int selIndex,
                               int *clickedKind, int *clickedIndex,
                               int *panX, int *panY,
                               bool *p_open) {
    bases bs = (bases)basesPtr;
    pillboxes pb = (pillboxes)pillsPtr;
    starts ss = (starts)startsPtr;

    *clickedKind = ME_SEL_NONE;
    *clickedIndex = -1;
    *panX = -1;
    *panY = -1;

    ImGuiCond posCond = s_displayResized ? ImGuiCond_Always : ImGuiCond_Once;
    ImGuiIO &oio = ImGui::GetIO();
    /* If Inspector is visible, stack below it; otherwise use top-right */
    float objY = (s_inspectorWindowBottom > 20.0f) ? s_inspectorWindowBottom : 20.0f;
    ImGui::SetNextWindowPos(ImVec2(oio.DisplaySize.x - 260, objY), posCond);
    ImGui::SetNextWindowSize(ImVec2(250, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin(langGetText(STR_MAPEDIT_WIN_OBJECTS), p_open, ImGuiWindowFlags_NoCollapse);

    /* Bases section */
    {
        MessageArgs hargs = {};
        hargs.number = bs->numBases;
        hargs.number2 = MAX_BASES;
        const char *header = langGetTextFmt(STR_MAPEDIT_OBJ_BASES_COUNT, &hargs);
        if (bs->numBases >= MAX_BASES) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        }
        bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);
        if (bs->numBases >= MAX_BASES) {
            ImGui::PopStyleColor();
        }
        if (open) {
            for (int i = 0; i < bs->numBases; i++) {
                char ownerBuf[32];
                if (bs->item[i].owner != 0xFF) {
                    MessageArgs oa = {};
                    oa.number = bs->item[i].owner;
                    SDL_strlcpy(ownerBuf, langGetTextFmt(STR_MAPEDIT_OWNER_PLAYER, &oa), sizeof(ownerBuf));
                } else {
                    SDL_strlcpy(ownerBuf, langGetText(STR_MAPEDIT_OWNER_NEUTRAL), sizeof(ownerBuf));
                }
                MessageArgs args = {};
                args.number = i;
                args.number2 = bs->item[i].x;
                args.number3 = bs->item[i].y;
                SDL_strlcpy(args.string1, ownerBuf, sizeof(args.string1));
                bool isSel = (selKind == ME_SEL_BASE && selIndex == i);
                if (ImGui::Selectable(langGetTextFmt(STR_MAPEDIT_OBJ_BASE_ROW, &args), isSel)) {
                    *clickedKind = ME_SEL_BASE;
                    *clickedIndex = i;
                    *panX = bs->item[i].x;
                    *panY = bs->item[i].y;
                }
            }
        }
    }

    /* Pillboxes section */
    {
        MessageArgs hargs = {};
        hargs.number = pb->numPills;
        hargs.number2 = MAX_PILLS;
        const char *header = langGetTextFmt(STR_MAPEDIT_OBJ_PILLS_COUNT, &hargs);
        if (pb->numPills >= MAX_PILLS) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        }
        bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);
        if (pb->numPills >= MAX_PILLS) {
            ImGui::PopStyleColor();
        }
        if (open) {
            for (int i = 0; i < pb->numPills; i++) {
                char ownerBuf[32];
                if (pb->item[i].owner != 0xFF) {
                    MessageArgs oa = {};
                    oa.number = pb->item[i].owner;
                    SDL_strlcpy(ownerBuf, langGetTextFmt(STR_MAPEDIT_OWNER_PLAYER, &oa), sizeof(ownerBuf));
                } else {
                    SDL_strlcpy(ownerBuf, langGetText(STR_MAPEDIT_OWNER_NEUTRAL), sizeof(ownerBuf));
                }
                MessageArgs args = {};
                args.number = i;
                args.number2 = pb->item[i].x;
                args.number3 = pb->item[i].y;
                args.number4 = pb->item[i].armour;
                SDL_strlcpy(args.string1, ownerBuf, sizeof(args.string1));
                bool isSel = (selKind == ME_SEL_PILL && selIndex == i);
                if (ImGui::Selectable(langGetTextFmt(STR_MAPEDIT_OBJ_PILL_ROW, &args), isSel)) {
                    *clickedKind = ME_SEL_PILL;
                    *clickedIndex = i;
                    *panX = pb->item[i].x;
                    *panY = pb->item[i].y;
                }
            }
        }
    }

    /* Starts section */
    {
        MessageArgs hargs = {};
        hargs.number = ss->numStarts;
        hargs.number2 = MAX_STARTS;
        const char *header = langGetTextFmt(STR_MAPEDIT_OBJ_STARTS_COUNT, &hargs);
        if (ss->numStarts >= MAX_STARTS) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        }
        bool open = ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen);
        if (ss->numStarts >= MAX_STARTS) {
            ImGui::PopStyleColor();
        }
        if (open) {
            for (int i = 0; i < ss->numStarts; i++) {
                const char *dirStr = ss->item[i].dir < 16 ? s_dirLabels[ss->item[i].dir] : "?";
                MessageArgs args = {};
                args.number = i;
                args.number2 = ss->item[i].x;
                args.number3 = ss->item[i].y;
                SDL_strlcpy(args.string1, dirStr, sizeof(args.string1));
                bool isSel = (selKind == ME_SEL_START && selIndex == i);
                if (ImGui::Selectable(langGetTextFmt(STR_MAPEDIT_OBJ_START_ROW, &args), isSel)) {
                    *clickedKind = ME_SEL_START;
                    *clickedIndex = i;
                    *panX = ss->item[i].x;
                    *panY = ss->item[i].y;
                }
            }
        }
    }

    ImGui::End();
}

/* -------------------------------------------------------
 * Overview Minimap Window
 * ------------------------------------------------------- */

void mapEditorImguiOverview(void *minimapTex,
                             int viewCenterX, int viewCenterY,
                             float tilePixels, int screenW, int screenH,
                             int *navX, int *navY,
                             bool *p_open) {
    *navX = -1;
    *navY = -1;

    if (!minimapTex) return;

    ImVec2 winSize(280, 300);
    ImGui::SetNextWindowSize(winSize, ImGuiCond_FirstUseEver);
    ImGuiIO &io = ImGui::GetIO();
    ImGuiCond posCond = s_displayResized ? ImGuiCond_Always : ImGuiCond_Once;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - winSize.x - 10,
                                   io.DisplaySize.y - winSize.y - 10),
                            posCond);
    ImGui::Begin(langGetText(STR_MAPEDIT_WIN_OVERVIEW), p_open, ImGuiWindowFlags_NoCollapse);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float displaySize = avail.x < avail.y ? avail.x : avail.y;
    if (displaySize < 64) displaySize = 64;

    ImGui::Image((ImTextureID)minimapTex, ImVec2(displaySize, displaySize));

    /* Click/drag to navigate */
    ImVec2 imgMin = ImGui::GetItemRectMin();
    float scale = 256.0f / displaySize;

    if (ImGui::IsItemActive() && ImGui::IsMouseDown(0)) {
        ImVec2 mouse = ImGui::GetMousePos();
        int tileX = (int)((mouse.x - imgMin.x) * scale);
        int tileY = (int)((mouse.y - imgMin.y) * scale);
        if (tileX < 0) tileX = 0;
        if (tileX > 255) tileX = 255;
        if (tileY < 0) tileY = 0;
        if (tileY > 255) tileY = 255;
        *navX = tileX;
        *navY = tileY;
    }

    /* Draw viewport rectangle */
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    float tilePixelsF = (tilePixels > 0.0f) ? tilePixels : (float)TILE_SIZE_X;
    float tilesW = (float)screenW / tilePixelsF;
    float tilesH = (float)screenH / tilePixelsF;
    float camTX = (float)(viewCenterX >> 8);
    float camTY = (float)(viewCenterY >> 8);

    float invScale = displaySize / 256.0f;
    float rectX = imgMin.x + (camTX - tilesW * 0.5f) * invScale;
    float rectY = imgMin.y + (camTY - tilesH * 0.5f) * invScale;
    float rectW = tilesW * invScale;
    float rectH = tilesH * invScale;

    drawList->AddRect(ImVec2(rectX, rectY), ImVec2(rectX + rectW, rectY + rectH),
                      IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);

    ImGui::End();
}

/* -------------------------------------------------------
 * Go To Coordinates Dialog
 * ------------------------------------------------------- */

bool mapEditorImguiGotoDialog(int *gotoX, int *gotoY) {
    static int inputX = 128;
    static int inputY = 128;
    bool confirmed = false;

    static bool s_gotoOpen = true; s_gotoOpen = true;
    if (ImGui::BeginPopupModal(langGetText(STR_MAPEDIT_GOTO_TITLE), &s_gotoOpen,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoMove)) {
        /* Focus X field on first appearance */
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputInt(langGetText(STR_MAPEDIT_GOTO_X), &inputX);
        ImGui::InputInt(langGetText(STR_MAPEDIT_GOTO_Y), &inputY);

        int f = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                   langGetText(STR_OK),
                                   /*enterConfirms*/ true);
        if (f == WBUI::FOOTER_CONFIRM) {
            /* Clamp to valid range */
            if (inputX < 0) inputX = 0;
            if (inputX > 255) inputX = 255;
            if (inputY < 0) inputY = 0;
            if (inputY > 255) inputY = 255;
            *gotoX = inputX;
            *gotoY = inputY;
            confirmed = true;
            ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_CANCEL) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    return confirmed;
}

/* -------------------------------------------------------
 * Validation Results Panel
 * ------------------------------------------------------- */

void mapEditorImguiValidationPanel(const void *issuesPtr, int count,
                                    int errorCount, int warningCount,
                                    bool *visible,
                                    int *clickedLocType, int *clickedIndex,
                                    int *clickedX, int *clickedY) {
    const ValidateIssue *issues = (const ValidateIssue *)issuesPtr;
    *clickedLocType = -1;
    *clickedIndex = -1;
    *clickedX = -1;
    *clickedY = -1;

    if (!*visible) return;

    ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(langGetText(STR_MAPEDIT_VALIDATION), visible, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* Summary line */
    if (errorCount == 0 && warningCount == 0) {
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%s", langGetText(STR_MAPEDIT_VAL_NOISSUES));
    } else {
        if (errorCount > 0) {
            MessageArgs args = {};
            args.number = errorCount;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_VAL_ERRORS, &args));
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(langGetText(STR_MAPEDIT_VAL_NOERRORS));
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(", ");
        ImGui::SameLine();
        if (warningCount > 0) {
            MessageArgs args = {};
            args.number = warningCount;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.3f, 1.0f));
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_VAL_WARNINGS, &args));
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(langGetText(STR_MAPEDIT_VAL_NOWARNINGS));
        }
    }
    ImGui::Separator();

    /* Issue list */
    for (int i = 0; i < count; i++) {
        const ValidateIssue *iss = &issues[i];
        char label[192];
        if (iss->severity == VALIDATE_ERROR) {
            snprintf(label, sizeof(label), "[E] %s", iss->message);
        } else {
            snprintf(label, sizeof(label), "[W] %s", iss->message);
        }

        ImGui::PushID(i);
        if (iss->severity == VALIDATE_ERROR) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
        }

        if (ImGui::Selectable(label) && iss->locType != VALIDATE_LOC_NONE) {
            *clickedLocType = (int)iss->locType;
            *clickedIndex = iss->locIndex;
            *clickedX = iss->locX;
            *clickedY = iss->locY;
        }

        ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::End();
}

/* -------------------------------------------------------
 * Validation Error Modal (blocks saving)
 * ------------------------------------------------------- */

bool mapEditorImguiValidationErrorModal(int errorCount) {
    bool dismissed = false;
    static bool s_valErrOpen = true; s_valErrOpen = true;
    if (ImGui::BeginPopupModal(langGetText(STR_MAPEDIT_VAL_ERRORS_TITLE), &s_valErrOpen,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoMove)) {
        MessageArgs args = {};
        args.number = errorCount;
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_VAL_HASERRORS, &args));
        int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                                   langGetText(STR_OK));
        if (f != WBUI::FOOTER_NONE) {
            dismissed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return dismissed;
}

/* -------------------------------------------------------
 * Generate Map dialog
 * ------------------------------------------------------- */

/* mapGenImguiControls and its helpers (lock buttons, SVG icons,
 * restoreLocked* functions) live in mapgen_imgui.cpp so that iOS
 * and other targets can use them without the full map-editor UI. */

bool mapEditorImguiGenerateDialog(bool *open, MapGenConfig *cfg,
                                  bool hasSelection,
                                  int selX1, int selY1, int selX2, int selY2,
                                  bool selectionScope) {
    static bool wasOpen = false;
    bool justOpened = !wasOpen && *open;
    wasOpen = *open;

    const char *title = langGetText(selectionScope ? STR_MAPEDIT_GENRANDAREA_TITLE
                                                   : STR_MAPEDIT_GENRANDMAP_TITLE);
    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Once);
    if (!ImGui::Begin(title, open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        wasOpen = *open;
        return false;
    }

    /* Scope banner */
    {
        MessageArgs args = {};
        args.number = selX1;
        args.number2 = selY1;
        args.number3 = selX2;
        args.number4 = selY2;
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::TextUnformatted(langGetTextFmt(selectionScope
                                              ? STR_MAPEDIT_GENSCOPE_SELECTION
                                              : STR_MAPEDIT_GENSCOPE_FULLMAP, &args));
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    bool generated = mapGenImguiControls(cfg) || justOpened;

    /* Close is affirmative ("I'm done generating"), not a cancel. */
    int f = WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                               /*confirmLabel*/ langGetText(STR_CLOSE));
    if (f != WBUI::FOOTER_NONE) {
        *open = false;
    }

    wasOpen = *open;
    ImGui::End();
    return generated;
}

/* -------------------------------------------------------
 * Terrain Picker helper — used by text dialog
 * ------------------------------------------------------- */

static bool terrainPicker(const char *label, BYTE *terrain, bool allowNone,
                          void *tilesTex, int tileSize) {
    bool changed = false;
    const float atlasW = 496.0f;
    const float atlasH = 176.0f;
    const float btnSize = 24.0f;

    ImGui::Text("%s", label);

    if (allowNone) {
        ImGui::PushID(label);
        bool selected = (*terrain == ME_TRANSPARENT);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.85f, 1.0f));
        }
        if (ImGui::Button(langGetText(STR_NONE), ImVec2(btnSize + 8, btnSize))) {
            *terrain = ME_TRANSPARENT;
            changed = true;
        }
        if (selected) {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(rMin, rMax,
                IM_COL32(80, 160, 255, 255), 0.0f, 0, 2.0f);
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine();
        ImGui::PopID();
    }

    for (int i = 0; i < ME_NUM_TERRAINS; i++) {
        int tile = s_terrainEntries[i].iconTile;
        float u0 = (float)mapViewPosX[tile] / atlasW;
        float v0 = (float)mapViewPosY[tile] / atlasH;
        float u1 = u0 + (float)tileSize / atlasW;
        float v1 = v0 + (float)tileSize / atlasH;

        char id[64];
        snprintf(id, sizeof(id), "%s_%d", label, i);
        ImGui::PushID(id);

        bool selected = (*terrain == s_terrainEntries[i].rawValue);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.95f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.85f, 1.0f));
        }
        if (ImGui::ImageButton(s_terrainEntries[i].name,
                               (ImTextureID)tilesTex,
                               ImVec2(btnSize, btnSize),
                               ImVec2(u0, v0), ImVec2(u1, v1))) {
            *terrain = s_terrainEntries[i].rawValue;
            changed = true;
        }
        if (selected) {
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(rMin, rMax,
                IM_COL32(80, 160, 255, 255), 0.0f, 0, 2.0f);
            ImGui::PopStyleColor(3);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(s_terrainEntries[i].nameId));
        }
        ImGui::PopID();

        /* Wrap after 5 items per row */
        if ((i + (allowNone ? 1 : 0)) % 5 != 4) ImGui::SameLine();
    }
    ImGui::NewLine();

    return changed;
}

/* -------------------------------------------------------
 * Text Tool Dialog
 * ------------------------------------------------------- */

bool mapEditorImguiTextDialog(bool *open, TextConfig *cfg,
                              void *tilesTex, int tileSize,
                              FontList *fontList, bool *fontListReady,
                              int *fontSource, int *fontFamily,
                              int *fontStyleIdx, int *ttfPixelSize) {
    bool generated = false;

    static char s_textBuf[1024] = "";
    static bool s_firstOpen = true;
    static bool s_invalidatePreview = false;

    if (s_firstOpen) {
        if (cfg->text[0] != '\0') {
            SDL_strlcpy(s_textBuf, cfg->text, sizeof(s_textBuf));
        }
        s_firstOpen = false;
        s_invalidatePreview = true;
    }

    /* Lazy font list initialization */
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
    if (!*fontListReady) {
        fontListInit(fontList);
        *fontListReady = true;
    }
#endif

    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_Once);
    ImGui::Begin(langGetText(STR_MAPEDIT_TEXT_TITLE), open,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    /* Text input */
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_TEXT_LABEL));
    ImGui::InputTextMultiline("##text_input", s_textBuf, sizeof(s_textBuf),
                              ImVec2(-1, 80));

    /* Font source toggle */
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
    ImGui::RadioButton(langGetText(STR_MAPEDIT_TEXT_BUILTIN), fontSource, 0);
    ImGui::SameLine();
    ImGui::RadioButton(langGetText(STR_MAPEDIT_TEXT_SYSTEM), fontSource, 1);
#endif

    /* Shared across font UI and preview cache */
    static const char *s_familyNames[FONT_MAX_ENTRIES];
    static int s_numFamilies = 0;

    if (*fontSource == 0) {
        /* Built-in bitmap font controls */
        const char *s_fontSizes[TEXT_SIZE_COUNT] = {
            langGetText(STR_MAPEDIT_TEXT_FONTSIZE_S),
            langGetText(STR_MAPEDIT_TEXT_FONTSIZE_M),
            langGetText(STR_MAPEDIT_TEXT_FONTSIZE_L),
            langGetText(STR_MAPEDIT_TEXT_FONTSIZE_XL),
        };
        ImGui::Combo(langGetText(STR_MAPEDIT_TEXT_FONTSIZE), &cfg->fontSize, s_fontSizes, TEXT_SIZE_COUNT);

        const char *s_styles[TEXT_STYLE_COUNT] = {
            langGetText(STR_MAPEDIT_TEXT_STYLE_REGULAR),
            langGetText(STR_MAPEDIT_TEXT_STYLE_BOLD),
            langGetText(STR_MAPEDIT_TEXT_STYLE_ITALIC),
            langGetText(STR_MAPEDIT_TEXT_STYLE_BOLDITALIC),
        };
        ImGui::Combo(langGetText(STR_MAPEDIT_TEXT_STYLE), &cfg->style, s_styles, TEXT_STYLE_COUNT);
    }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
    else {
        /* TTF font controls */
        s_numFamilies = fontListGetFamilies(fontList, s_familyNames, FONT_MAX_ENTRIES);

        if (s_numFamilies > 0) {
            /* Build display names with (bundled) tag for combo */
            if (*fontFamily >= s_numFamilies) *fontFamily = 0;

            /* Font family combo — use a callback to generate display items */
            if (ImGui::BeginCombo(langGetText(STR_MAPEDIT_TEXT_FONTFAMILY),
                                  *fontFamily < s_numFamilies ? s_familyNames[*fontFamily] : "")) {
                for (int i = 0; i < s_numFamilies; i++) {
                    /* Check if this family is bundled */
                    bool isBundled = false;
                    for (int j = 0; j < fontList->count; j++) {
                        if (SDL_strcasecmp(fontList->entries[j].familyName, s_familyNames[i]) == 0) {
                            isBundled = fontList->entries[j].isBundled;
                            break;
                        }
                    }

                    const char *label;
                    char bundledBuf[256];
                    if (isBundled) {
                        MessageArgs ba = {};
                        SDL_strlcpy(ba.string1, s_familyNames[i], sizeof(ba.string1));
                        SDL_strlcpy(bundledBuf, langGetTextFmt(STR_MAPEDIT_TEXT_FONTBUNDLED, &ba), sizeof(bundledBuf));
                        label = bundledBuf;
                    } else {
                        label = s_familyNames[i];
                    }

                    bool selected = (*fontFamily == i);
                    if (ImGui::Selectable(label, selected)) {
                        *fontFamily = i;
                        *fontStyleIdx = 0;  /* Reset style when family changes */
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            /* Font style combo for selected family */
            if (*fontFamily < s_numFamilies) {
                int styleIndices[32];
                int numStyles = fontListFindStyles(fontList, s_familyNames[*fontFamily],
                                                   styleIndices, 32);
                if (numStyles > 0) {
                    if (*fontStyleIdx >= numStyles) *fontStyleIdx = 0;

                    const char *currentStyle = fontList->entries[styleIndices[*fontStyleIdx]].styleName;
                    if (ImGui::BeginCombo(langGetText(STR_MAPEDIT_TEXT_STYLE), currentStyle)) {
                        for (int i = 0; i < numStyles; i++) {
                            bool selected = (*fontStyleIdx == i);
                            if (ImGui::Selectable(fontList->entries[styleIndices[i]].styleName, selected)) {
                                *fontStyleIdx = i;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                }
            }

            /* Size slider */
            ImGui::SliderInt(langGetText(STR_MAPEDIT_TEXT_SIZEPX), ttfPixelSize, 8, 64);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
            ImGui::TextUnformatted(langGetText(STR_MAPEDIT_TEXT_NOFONTS));
            ImGui::PopStyleColor();
        }
    }
#endif

    ImGui::Separator();

    /* Terrain pickers */
    terrainPicker(langGetText(STR_MAPEDIT_TEXT_TERR_TEXT), &cfg->textTerrain, false, tilesTex, tileSize);
    terrainPicker(langGetText(STR_MAPEDIT_TEXT_TERR_EDGE), &cfg->edgeTerrain, true, tilesTex, tileSize);
    terrainPicker(langGetText(STR_MAPEDIT_TEXT_TERR_BG), &cfg->bgTerrain, true, tilesTex, tileSize);

    ImGui::Separator();

    /* Preview — cached to avoid per-frame rasterization and stack pressure */
    if (s_textBuf[0] != '\0') {
        static BYTE s_cachedPreview[256][256];
        static int s_cachedW = 0, s_cachedH = 0;
        static bool s_cachedOk = false;
        static uint32_t s_prevTextHash = 0;
        static int s_prevFontSource = -1;
        static int s_prevFontFamily = -1;
        static int s_prevFontStyle = -1;
        static int s_prevPixelSize = -1;
        static int s_prevFontSize = -1;
        static int s_prevStyleIdx = -1;
        static BYTE s_prevTextTerrain = 0;
        static BYTE s_prevEdgeTerrain = 0;
        static BYTE s_prevBgTerrain = 0;

        /* djb2 hash of text buffer */
        uint32_t textHash = 5381;
        for (const char *p = s_textBuf; *p; p++) {
            textHash = ((textHash << 5) + textHash) + (unsigned char)*p;
        }

        if (s_invalidatePreview) {
            s_prevTextHash = 0;
            s_invalidatePreview = false;
        }

        bool inputsChanged = (textHash != s_prevTextHash)
            || (*fontSource != s_prevFontSource)
            || (*fontFamily != s_prevFontFamily)
            || (*fontStyleIdx != s_prevFontStyle)
            || (*ttfPixelSize != s_prevPixelSize)
            || (cfg->fontSize != s_prevFontSize)
            || (cfg->style != s_prevStyleIdx)
            || (cfg->textTerrain != s_prevTextTerrain)
            || (cfg->edgeTerrain != s_prevEdgeTerrain)
            || (cfg->bgTerrain != s_prevBgTerrain);

        if (inputsChanged) {
            s_prevTextHash = textHash;
            s_prevFontSource = *fontSource;
            s_prevFontFamily = *fontFamily;
            s_prevFontStyle = *fontStyleIdx;
            s_prevPixelSize = *ttfPixelSize;
            s_prevFontSize = cfg->fontSize;
            s_prevStyleIdx = cfg->style;
            s_prevTextTerrain = cfg->textTerrain;
            s_prevEdgeTerrain = cfg->edgeTerrain;
            s_prevBgTerrain = cfg->bgTerrain;

            s_cachedOk = false;
            s_cachedW = 0;
            s_cachedH = 0;

            if (*fontSource == 0) {
                TextConfig previewCfg = *cfg;
                SDL_strlcpy(previewCfg.text, s_textBuf, sizeof(previewCfg.text));
                s_cachedOk = textRasterize(&previewCfg, s_cachedPreview, &s_cachedW, &s_cachedH);
            }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
            else {
                /* TTF preview — reuse s_familyNames/s_numFamilies from font combo */
                if (*fontFamily >= 0 && *fontFamily < s_numFamilies) {
                    int si[32];
                    int ns = fontListFindStyles(fontList, s_familyNames[*fontFamily], si, 32);
                    int idx = *fontStyleIdx;
                    if (idx >= 0 && idx < ns) {
                        FontEntry *fe = &fontList->entries[si[idx]];
                        s_cachedOk = textRasterizeTTF(fe->filePath, *ttfPixelSize,
                                                     s_textBuf,
                                                     cfg->textTerrain, cfg->edgeTerrain, cfg->bgTerrain,
                                                     s_cachedPreview, &s_cachedW, &s_cachedH);
                    }
                }
            }
#endif
        }

        int pw = s_cachedW, ph = s_cachedH;
        bool previewOk = s_cachedOk;

        if (previewOk && pw > 0 && ph > 0) {
            MessageArgs args = {};
            args.number = pw;
            args.number2 = ph;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_TEXT_PREVIEW, &args));

            float maxW = 300.0f, maxH = 200.0f;
            float tileScale = 1.0f;
            float sx = maxW / (float)pw;
            float sy = maxH / (float)ph;
            tileScale = sx < sy ? sx : sy;
            if (tileScale > (float)tileSize) tileScale = (float)tileSize;
            if (tileScale < 1.0f) tileScale = 1.0f;

            const float atlasW = 496.0f;
            const float atlasH = 176.0f;
            float cellSize = tileScale;
            ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList *dl = ImGui::GetWindowDrawList();

            for (int x = 0; x < pw; x++) {
                for (int y = 0; y < ph; y++) {
                    BYTE t = s_cachedPreview[x][y];
                    int iconTile = -1;
                    for (int k = 0; k < ME_NUM_TERRAINS; k++) {
                        if (s_terrainEntries[k].rawValue == t) {
                            iconTile = s_terrainEntries[k].iconTile;
                            break;
                        }
                    }
                    if (iconTile < 0) continue;

                    float u0 = (float)mapViewPosX[iconTile] / atlasW;
                    float v0 = (float)mapViewPosY[iconTile] / atlasH;
                    float u1 = u0 + (float)tileSize / atlasW;
                    float v1 = v0 + (float)tileSize / atlasH;

                    ImVec2 p0(origin.x + x * cellSize, origin.y + y * cellSize);
                    ImVec2 p1(p0.x + cellSize, p0.y + cellSize);
                    dl->AddImage((ImTextureID)tilesTex, p0, p1,
                                 ImVec2(u0, v0), ImVec2(u1, v1));
                }
            }

            ImGui::Dummy(ImVec2(pw * cellSize, ph * cellSize));
        } else if (s_textBuf[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextUnformatted(langGetText(STR_MAPEDIT_TEXT_TOOLARGE));
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();
    ImGui::Spacing();

    float buttonW = 100.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float totalBtnW = buttonW * 2 + spacing;
    float startX = (ImGui::GetContentRegionAvail().x - totalBtnW) * 0.5f;
    if (startX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + startX);

    if (ImGui::Button(langGetText(STR_MAPEDIT_GENERATE_BTN), ImVec2(buttonW, 0))) {
        SDL_strlcpy(cfg->text, s_textBuf, sizeof(cfg->text));
        generated = true;
        *open = false;
        s_firstOpen = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(buttonW, 0))) {
        *open = false;
        s_firstOpen = true;
    }

    ImGui::End();
    return generated;
}

bool mapEditorImguiMazeSettings(MazeConfig *cfg, bool isDragging,
                                int selX1, int selY1, int selX2, int selY2,
                                void *tilesTex, int tileSize) {
    const char *mazeAlgoNames[MAZE_ALGO_COUNT] = {
        langGetText(STR_MAPEDIT_ALGO_LABYRINTH),
        langGetText(STR_MAPEDIT_ALGO_OPEN),
    };
    ImGui::SetNextWindowSize(ImVec2(260, 0), ImGuiCond_Once);
    ImGui::Begin(langGetText(STR_MAPEDIT_MAZE_TITLE), nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    /* Selection info */
    if (isDragging) {
        int w = selX2 - selX1 + 1;
        int h = selY2 - selY1 + 1;
        MessageArgs args = {};
        args.number = selX1;
        args.number2 = selY1;
        args.number3 = selX2;
        args.number4 = selY2;
        SDL_snprintf(args.string1, sizeof(args.string1), "%dx%d", w, h);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_MAZE_REGION, &args));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_DRAG_HINT));
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    MazeConfig prev = *cfg;

    ImGui::Combo(langGetText(STR_MAPEDIT_ALGORITHM), &cfg->algo, mazeAlgoNames, MAZE_ALGO_COUNT);

    int wallIdx = cfg->wallThick - 1;
    ImGui::SliderInt(langGetText(STR_MAPEDIT_WALL), &wallIdx, 0, 1, wallIdx == 0 ? "1" : "2");
    cfg->wallThick = wallIdx + 1;

    int corrIdx = cfg->corridorWidth - 1;
    ImGui::SliderInt(langGetText(STR_MAPEDIT_CORRIDOR), &corrIdx, 0, 1, corrIdx == 0 ? "1" : "2");
    cfg->corridorWidth = corrIdx + 1;

    ImGui::SliderInt(langGetText(STR_MAPEDIT_ENTRIES), &cfg->entries, 1, 8);
    ImGui::SliderInt(langGetText(STR_MAPEDIT_ROOMS), &cfg->cityRooms, 0, 5);

    ImGui::Spacing();
    ImGui::Separator();
    terrainPicker(langGetText(STR_MAPEDIT_WALLTERRAIN), &cfg->wallTerrain, false, tilesTex, tileSize);
    ImGui::Spacing();
    terrainPicker(langGetText(STR_MAPEDIT_CORRIDORTERRAIN), &cfg->corridorTerrain, false, tilesTex, tileSize);

    ImGui::End();

    return memcmp(&prev, cfg, sizeof(MazeConfig)) != 0;
}

bool mapEditorImguiGenerateSettings(MapGenConfig *cfg, bool isDragging,
                                    int selX1, int selY1, int selX2, int selY2) {
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Once);
    ImGui::Begin(langGetText(STR_MAPEDIT_GENSETTINGS_TITLE), nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    /* Selection info */
    if (isDragging) {
        int w = selX2 - selX1 + 1;
        int h = selY2 - selY1 + 1;
        MessageArgs args = {};
        args.number = selX1;
        args.number2 = selY1;
        args.number3 = selX2;
        args.number4 = selY2;
        SDL_snprintf(args.string1, sizeof(args.string1), "%dx%d", w, h);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_MAZE_REGION, &args));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_DRAG_HINT));
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    bool changed = mapGenImguiControls(cfg);

    ImGui::End();
    return changed;
}

/* -------------------------------------------------------
 * Statistics panel
 * ------------------------------------------------------- */

/* Terrain type display order (sorted by typical frequency — actual sort is
 * by count at render time, but this list defines which types we know about) */
static const BYTE s_statTerrainTypes[] = {
    DEEP_SEA, GRASS, FOREST, SWAMP, RIVER, ROAD, CRATER, RUBBLE,
    BUILDING, HALFBUILDING, BOAT
};
#define STAT_TERRAIN_COUNT (sizeof(s_statTerrainTypes) / sizeof(s_statTerrainTypes[0]))

static const char *statTerrainName(BYTE t) {
    langid id;
    switch (t) {
    case BUILDING:     id = STR_MAPEDIT_TERR_BUILDING;     break;
    case RIVER:        id = STR_MAPEDIT_TERR_RIVER;        break;
    case SWAMP:        id = STR_MAPEDIT_TERR_SWAMP;        break;
    case CRATER:       id = STR_MAPEDIT_TERR_CRATER;       break;
    case ROAD:         id = STR_MAPEDIT_TERR_ROAD;         break;
    case FOREST:       id = STR_MAPEDIT_TERR_FOREST;       break;
    case RUBBLE:       id = STR_MAPEDIT_TERR_RUBBLE;       break;
    case GRASS:        id = STR_MAPEDIT_TERR_GRASS;        break;
    case HALFBUILDING: id = STR_MAPEDIT_TERRNAME_HALFBLD;  break;
    case BOAT:         id = STR_MAPEDIT_TERR_BOAT;         break;
    case DEEP_SEA:     id = STR_MAPEDIT_TERR_DEEPSEA;      break;
    default:           id = STR_UNKNOWN;                   break;
    }
    return langGetText(id);
}

/* Format an integer with comma separators (e.g. 46225 → "46,225") */
static void formatComma(int value, char *buf, int bufSize) {
    char raw[32];
    snprintf(raw, sizeof(raw), "%d", value);
    int len = (int)strlen(raw);
    int commas = (len - 1) / 3;
    int outLen = len + commas;
    if (outLen >= bufSize) { snprintf(buf, bufSize, "%d", value); return; }
    buf[outLen] = '\0';
    int src = len - 1, dst = outLen - 1, digits = 0;
    while (src >= 0) {
        buf[dst--] = raw[src--];
        digits++;
        if (digits % 3 == 0 && src >= 0)
            buf[dst--] = ',';
    }
}

void mapEditorImguiStatsPanel(const MapStats *stats, bool *p_open, bool *wantRefresh) {
    if (!*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(320, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(langGetText(STR_MAPEDIT_WIN_STATS), p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* --- Terrain Distribution --- */
    if (ImGui::CollapsingHeader(langGetText(STR_MAPEDIT_STATS_TERRAIN), ImGuiTreeNodeFlags_DefaultOpen)) {
        /* Sort terrain types by count descending */
        struct TerrainEntry { BYTE type; int count; float pct; };
        TerrainEntry entries[STAT_TERRAIN_COUNT];
        int numEntries = 0;
        for (int i = 0; i < (int)STAT_TERRAIN_COUNT; i++) {
            BYTE t = s_statTerrainTypes[i];
            if (stats->terrainCount[t] > 0) {
                entries[numEntries].type  = t;
                entries[numEntries].count = stats->terrainCount[t];
                entries[numEntries].pct   = stats->terrainPct[t];
                numEntries++;
            }
        }
        /* Simple insertion sort by count descending */
        for (int i = 1; i < numEntries; i++) {
            TerrainEntry tmp = entries[i];
            int j = i - 1;
            while (j >= 0 && entries[j].count < tmp.count) {
                entries[j + 1] = entries[j];
                j--;
            }
            entries[j + 1] = tmp;
        }

        for (int i = 0; i < numEntries; i++) {
            BYTE t = entries[i].type;
            char countBuf[32];
            formatComma(entries[i].count, countBuf, sizeof(countBuf));

            /* Get terrain color for progress bar */
            uint8_t r, g, b;
            minimapTerrainColor(t, &r, &g, &b);
            ImVec4 barColor((float)r / 255.0f, (float)g / 255.0f,
                            (float)b / 255.0f, 1.0f);

            char pctBuf[16];
            SDL_snprintf(pctBuf, sizeof(pctBuf), "%.1f", entries[i].pct * 100.0f);
            char overlayBuf[96];
            SDL_snprintf(overlayBuf, sizeof(overlayBuf), "%s  %s (%s%%)",
                         countBuf, statTerrainName(t), pctBuf);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
            ImGui::ProgressBar(entries[i].pct, ImVec2(-FLT_MIN, 0), overlayBuf);
            ImGui::PopStyleColor();
        }
    }

    /* --- Objects --- */
    if (ImGui::CollapsingHeader(langGetText(STR_MAPEDIT_WIN_OBJECTS), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto objLine = [](langid labelId, int count, int maxCount) {
            MessageArgs oargs = {};
            SDL_strlcpy(oargs.string1, langGetText(labelId), sizeof(oargs.string1));
            oargs.number = count;
            oargs.number2 = maxCount;
            ImVec4 col = (count < maxCount) ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                            : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_STATS_OBJ_LINE, &oargs));
            ImGui::PopStyleColor();
        };
        objLine(STR_TABLET_BASES,     stats->numBases,  MAX_BASES);
        objLine(STR_TABLET_PILLBOXES, stats->numPills,  MAX_PILLS);
        objLine(STR_MAPEDIT_STATS_STARTS, stats->numStarts, MAX_STARTS);
        ImGui::Separator();
        {
            MessageArgs args = {};
            args.number = stats->mineCount;
            SDL_snprintf(args.string1, sizeof(args.string1), "%.1f", stats->mineDensity * 100.0f);
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_STATS_MINES, &args));
        }
    }

    /* --- Spatial Analysis --- */
    if (ImGui::CollapsingHeader(langGetText(STR_MAPEDIT_STATS_SPATIAL), ImGuiTreeNodeFlags_DefaultOpen)) {
        auto pctLine = [](langid id, float v) {
            MessageArgs args = {};
            SDL_snprintf(args.string1, sizeof(args.string1), "%.1f", v * 100.0f);
            ImGui::TextUnformatted(langGetTextFmt(id, &args));
        };
        if (stats->spatialValid) {
            pctLine(STR_MAPEDIT_STATS_LAND_COVERAGE, stats->landPct);
            pctLine(STR_MAPEDIT_STATS_LARGEST,       stats->largestLandPct);
            ImGui::Separator();
            {
                MessageArgs args = {};
                SDL_snprintf(args.string1, sizeof(args.string1), "%.1f", stats->baseSpacing);
                ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_STATS_BASE_SPACING, &args));
            }
            {
                MessageArgs args = {};
                SDL_snprintf(args.string1, sizeof(args.string1), "%.1f", stats->pillSpacing);
                ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_STATS_PILL_SPACING, &args));
            }
            ImGui::Separator();
            {
                MessageArgs args = {};
                SDL_strlcpy(args.string1, stats->symLabel, sizeof(args.string1));
                SDL_snprintf(args.string2, sizeof(args.string2), "%.0f", stats->symBest * 100.0f);
                ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_STATS_SYMMETRY, &args));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                pctLine(STR_MAPEDIT_STATS_MIRROR_H,  stats->symH);
                pctLine(STR_MAPEDIT_STATS_MIRROR_V,  stats->symV);
                pctLine(STR_MAPEDIT_STATS_4CORNER,   stats->sym4);
                pctLine(STR_MAPEDIT_STATS_ROTATE180, stats->symR180);
                ImGui::EndTooltip();
            }
        } else {
            pctLine(STR_MAPEDIT_STATS_LAND_COVERAGE, stats->landPct);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextUnformatted(langGetText(STR_MAPEDIT_STATS_STALE));
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_MAPEDIT_STATS_REFRESH))) {
            *wantRefresh = true;
        }
    }

    ImGui::End();
}

/* -------------------------------------------------------
 * Image Import Dialog
 * ------------------------------------------------------- */

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)

/* Async file dialog callback for image browse */
static bool s_imgDialogOpen = false;
static bool s_imgBrowsePending = false;
static bool s_imgBrowseGotResult = false;
static char s_imgBrowseResult[512];

static void SDLCALL imgBrowseCallback(void *userdata, const char *const *filelist, int filter) {
    (void)userdata; (void)filter;
    s_imgBrowsePending = false;
    if (!s_imgDialogOpen) return; /* Dialog closed while browse was pending */
    s_imgBrowseGotResult = true;
    if (filelist && filelist[0]) {
        SDL_strlcpy(s_imgBrowseResult, filelist[0], sizeof(s_imgBrowseResult));
    } else {
        s_imgBrowseResult[0] = '\0';
    }
}

bool mapEditorImguiImageImportDialog(bool *open, ImageImportConfig *cfg,
                                     void *renderer) {
    if (!*open) {
        s_imgDialogOpen = false;
        return false;
    }
    s_imgDialogOpen = true;
    bool imported = false;

    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(langGetText(STR_MAPEDIT_IMG_TITLE), open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return false;
    }

    /* Handle async file dialog result */
    if (s_imgBrowseGotResult) {
        s_imgBrowseGotResult = false;
        if (s_imgBrowseResult[0]) {
            imageImportLoad(cfg, s_imgBrowseResult, (SDL_Renderer *)renderer);
            if (cfg->pixels) {
                imageImportAutoMap(cfg);
                imageImportConvert(cfg, (SDL_Renderer *)renderer);
            }
        }
    }

    /* File row */
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_IMG_FILE));
    ImGui::SameLine();
    if (cfg->filePath[0]) {
        /* Show just the filename */
        const char *name = SDL_strrchr(cfg->filePath, '/');
        const char *name2 = SDL_strrchr(cfg->filePath, '\\');
        if (name2 && (!name || name2 > name)) name = name2;
        ImGui::TextWrapped("%s", name ? name + 1 : cfg->filePath);
    } else {
        ImGui::TextDisabled("%s", langGetText(STR_MAPEDIT_IMG_NONE));
    }
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_MAPEDIT_BROWSE)) && !s_imgBrowsePending) {
        s_imgBrowsePending = true;
        s_imgBrowseGotResult = false;
        SDL_DialogFileFilter filters[] = {
            { "Image Files", "png;bmp;jpg;jpeg;gif;tga" },
            { "All Files", "*" },
        };
        SDL_ShowOpenFileDialog(imgBrowseCallback, nullptr, nullptr, filters, 2, nullptr, false);
    }

    /* Show error message if any */
    if (cfg->errorMsg[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("%s", cfg->errorMsg);
        ImGui::PopStyleColor();
    }

    if (cfg->pixels) {
        ImGui::Separator();

        /* Image info */
        {
            MessageArgs args = {};
            args.number = cfg->imgW;
            args.number2 = cfg->imgH;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_IMG_SOURCE, &args));
        }

        /* Previews side by side — display at fixed size fitting within 256x256 */
        if (cfg->previewTex || cfg->resultPreviewTex) {
            ImGui::TextUnformatted(langGetText(STR_MAPEDIT_IMG_PREVIEW_LBL));
            if (cfg->previewTex) {
                float tw, th;
                SDL_GetTextureSize(cfg->previewTex, &tw, &th);
                float s = 256.0f / (tw > th ? tw : th);
                if (s > 1.0f) s = 1.0f;
                ImGui::Image((ImTextureID)cfg->previewTex, ImVec2(tw * s, th * s));
            }
            if (cfg->resultPreviewTex) {
                ImGui::SameLine();
                ImGui::Text(" -> ");
                ImGui::SameLine();
                float tw, th;
                SDL_GetTextureSize(cfg->resultPreviewTex, &tw, &th);
                float s = 256.0f / (tw > th ? tw : th);
                if (s > 1.0f) s = 1.0f;
                ImGui::Image((ImTextureID)cfg->resultPreviewTex, ImVec2(tw * s, th * s));
            }
        }

        ImGui::Separator();

        /* Scale mode */
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_IMG_SCALEMODE));
        bool modeChanged = false;
        if (ImGui::RadioButton(langGetText(STR_MAPEDIT_IMG_FIT_SEL), cfg->scaleMode == ME_SCALE_FIT_SELECTION)) {
            cfg->scaleMode = ME_SCALE_FIT_SELECTION;
            modeChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(langGetText(STR_MAPEDIT_IMG_FIT_PLAY), cfg->scaleMode == ME_SCALE_FIT_PLAYABLE)) {
            cfg->scaleMode = ME_SCALE_FIT_PLAYABLE;
            modeChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("1:1", cfg->scaleMode == ME_SCALE_1TO1)) {
            cfg->scaleMode = ME_SCALE_1TO1;
            modeChanged = true;
        }

        if (modeChanged) {
            imageImportAutoMap(cfg);
            imageImportConvert(cfg, (SDL_Renderer *)renderer);
        }

        /* Output dimensions */
        {
            MessageArgs args = {};
            args.number = cfg->outW;
            args.number2 = cfg->outH;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPEDIT_IMG_OUTPUT, &args));
        }

        ImGui::Separator();

        /* Number of colors */
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_IMG_COLORS));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderInt("##numcolors", &cfg->numColors, 2, 16);
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_MAPEDIT_IMG_REDETECT))) {
            imageImportAutoMap(cfg);
            imageImportConvert(cfg, (SDL_Renderer *)renderer);
        }

        /* Color mapping section */
        if (cfg->numMappings > 0 && ImGui::CollapsingHeader(langGetText(STR_MAPEDIT_IMG_COLORMAP), ImGuiTreeNodeFlags_DefaultOpen)) {
            bool mappingChanged = false;
            for (int i = 0; i < cfg->numMappings; i++) {
                ImGui::PushID(i);

                /* Color swatch */
                ImVec4 col(cfg->mappings[i].r / 255.0f,
                           cfg->mappings[i].g / 255.0f,
                           cfg->mappings[i].b / 255.0f, 1.0f);
                ImGui::ColorButton("##color", col, 0, ImVec2(24, 24));
                ImGui::SameLine();

                /* Arrow */
                ImGui::Text("->");
                ImGui::SameLine();

                /* Terrain type dropdown */
                const char *currentName = "?";
                for (int k = 0; k < ME_NUM_TERRAINS; k++) {
                    if (s_terrainEntries[k].rawValue == cfg->mappings[i].terrain) {
                        currentName = langGetText(s_terrainEntries[k].nameId);
                        break;
                    }
                }
                ImGui::SetNextItemWidth(120);
                if (ImGui::BeginCombo("##terrain", currentName)) {
                    for (int k = 0; k < ME_NUM_TERRAINS; k++) {
                        bool selected = (s_terrainEntries[k].rawValue == cfg->mappings[i].terrain);
                        if (ImGui::Selectable(langGetText(s_terrainEntries[k].nameId), selected)) {
                            cfg->mappings[i].terrain = s_terrainEntries[k].rawValue;
                            mappingChanged = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();

                /* Enabled checkbox */
                if (ImGui::Checkbox("##enabled", &cfg->mappings[i].enabled)) {
                    mappingChanged = true;
                }

                ImGui::PopID();
            }
            if (mappingChanged) {
                imageImportConvert(cfg, (SDL_Renderer *)renderer);
            }
        }

        ImGui::Separator();

        /* Import / Cancel buttons */
        if (cfg->resultReady && ImGui::Button(langGetText(STR_MAPEDIT_IMPORT_BTN))) {
            imported = true;
            *open = false;
            s_imgDialogOpen = false;
        }
        if (cfg->resultReady) ImGui::SameLine();
    }

    if (ImGui::Button(langGetText(STR_CANCEL))) {
        imageImportFree(cfg);
        *open = false;
        s_imgDialogOpen = false;
    }

    ImGui::End();
    return imported;
}

#else /* Non-desktop stub */

bool mapEditorImguiImageImportDialog(bool *open, ImageImportConfig *cfg,
                                     void *renderer) {
    (void)open; (void)cfg; (void)renderer;
    return false;
}

#endif

/* -------------------------------------------------------
 * Stamp Library Panel
 * ------------------------------------------------------- */

/* Static state for stamp import file dialog */
static bool s_stampImportGotResult = false;
static char s_stampImportPath[512] = "";

static void stampImportCallback(void *userdata, const char *const *filelist, int filter) {
    (void)userdata; (void)filter;
    if (filelist && filelist[0]) {
        strncpy(s_stampImportPath, filelist[0], sizeof(s_stampImportPath) - 1);
        s_stampImportPath[sizeof(s_stampImportPath) - 1] = '\0';
        s_stampImportGotResult = true;
    }
}

static SDL_Texture *stampEnsureTexture(StampEntry *entry, SDL_Renderer *renderer) {
    if (entry->tex) return entry->tex;
    if (!entry->previewReady) return nullptr;

    entry->tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                   SDL_TEXTUREACCESS_STATIC,
                                   STAMP_PREVIEW_SIZE, STAMP_PREVIEW_SIZE);
    if (entry->tex) {
        SDL_UpdateTexture(entry->tex, nullptr, entry->previewPixels,
                          STAMP_PREVIEW_SIZE * 4);
    }
    return entry->tex;
}

int mapEditorImguiStampLibrary(StampLibrary *lib, void *rendererPtr,
                               bool hasClipboard, bool *p_open,
                               bool *wantSave, char *saveName, int saveNameLen) {
    SDL_Renderer *renderer = (SDL_Renderer *)rendererPtr;
    int loadIndex = -1;

    ImGui::SetNextWindowSize(ImVec2(340, 450), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(langGetText(STR_MAPEDIT_WIN_STAMP_LIB), p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return -1;
    }

    /* Determine layout: how many stamps per row */
    float avail = ImGui::GetContentRegionAvail().x;
    float thumbSize = 72.0f; /* 64 image + padding */
    int perRow = (int)(avail / thumbSize);
    if (perRow < 1) perRow = 1;

    /* Count bundled and user stamps */
    int bundledCount = 0, userCount = 0;
    for (int i = 0; i < lib->count; i++) {
        if (lib->entries[i].isBundled) bundledCount++;
        else userCount++;
    }

    /* Bundled stamps section */
    if (bundledCount > 0 && ImGui::CollapsingHeader(langGetText(STR_MAPEDIT_STAMP_BUNDLED), ImGuiTreeNodeFlags_DefaultOpen)) {
        int col = 0;
        for (int i = 0; i < lib->count; i++) {
            if (!lib->entries[i].isBundled) continue;
            StampEntry *entry = &lib->entries[i];

            if (col > 0) ImGui::SameLine();

            ImGui::BeginGroup();
            SDL_Texture *tex = stampEnsureTexture(entry, renderer);
            ImGui::PushID(i);
            if (tex) {
                if (ImGui::ImageButton("##stamp", (ImTextureID)tex,
                                       ImVec2(64, 64))) {
                    loadIndex = i;
                }
            } else {
                if (ImGui::Button("?##stamp", ImVec2(64, 64))) {
                    loadIndex = i;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%dx%d", entry->name, entry->width, entry->height);
            }
            ImGui::PopID();

            /* Name label (truncated) */
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 64.0f);
            ImGui::TextWrapped("%s", entry->name);
            ImGui::PopTextWrapPos();

            /* Size info */
            char info[64];
            snprintf(info, sizeof(info), "%dx%d", entry->width, entry->height);
            if (entry->numPills > 0 || entry->numBases > 0) {
                char extra[32] = "";
                if (entry->numBases > 0 && entry->numPills > 0) {
                    snprintf(extra, sizeof(extra), ", %db %dp",
                             entry->numBases, entry->numPills);
                } else if (entry->numBases > 0) {
                    snprintf(extra, sizeof(extra), ", %db", entry->numBases);
                } else {
                    snprintf(extra, sizeof(extra), ", %dp", entry->numPills);
                }
                strncat(info, extra, sizeof(info) - strlen(info) - 1);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
            ImGui::TextWrapped("%s", info);
            ImGui::PopStyleColor();

            ImGui::EndGroup();

            col++;
            if (col >= perRow) col = 0;
        }
    }

    /* User stamps section */
    if (ImGui::CollapsingHeader(langGetText(STR_MAPEDIT_STAMP_USER), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (userCount == 0) {
            ImGui::TextDisabled("%s", langGetText(STR_MAPEDIT_STAMP_NOUSER));
            ImGui::TextDisabled("%s", langGetText(STR_MAPEDIT_STAMP_HINT));
        } else {
            int col = 0;
            static int contextMenuIdx = -1;
            for (int i = 0; i < lib->count; i++) {
                if (lib->entries[i].isBundled) continue;
                StampEntry *entry = &lib->entries[i];

                if (col > 0) ImGui::SameLine();

                ImGui::BeginGroup();
                SDL_Texture *tex = stampEnsureTexture(entry, renderer);
                ImGui::PushID(i);
                if (tex) {
                    if (ImGui::ImageButton("##stamp", (ImTextureID)tex,
                                           ImVec2(64, 64))) {
                        loadIndex = i;
                    }
                } else {
                    if (ImGui::Button("?##stamp", ImVec2(64, 64))) {
                        loadIndex = i;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%dx%d", entry->name, entry->width, entry->height);
                }

                /* Right-click context menu for user stamps */
                if (ImGui::BeginPopupContextItem("##stampctx")) {
                    contextMenuIdx = i;
                    if (ImGui::MenuItem(langGetText(STR_MAPEDIT_DELETE))) {
                        if (remove(entry->filePath) == 0) {
                            loadIndex = -2; /* sentinel: refresh needed */
                        }
                        contextMenuIdx = -1;
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();

                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 64.0f);
                ImGui::TextWrapped("%s", entry->name);
                ImGui::PopTextWrapPos();

                char info[64];
                snprintf(info, sizeof(info), "%dx%d", entry->width, entry->height);
                if (entry->numPills > 0 || entry->numBases > 0) {
                    char extra[32] = "";
                    if (entry->numBases > 0 && entry->numPills > 0) {
                        snprintf(extra, sizeof(extra), ", %db %dp",
                                 entry->numBases, entry->numPills);
                    } else if (entry->numBases > 0) {
                        snprintf(extra, sizeof(extra), ", %db", entry->numBases);
                    } else {
                        snprintf(extra, sizeof(extra), ", %dp", entry->numPills);
                    }
                    strncat(info, extra, sizeof(info) - strlen(info) - 1);
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::TextWrapped("%s", info);
                ImGui::PopStyleColor();

                ImGui::EndGroup();

                col++;
                if (col >= perRow) col = 0;
            }
        }
    }

    ImGui::Separator();

    /* Bottom toolbar */
    {
        /* Save Clipboard button */
        if (!hasClipboard) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_MAPEDIT_STAMP_SAVECLIP))) {
            if (wantSave) *wantSave = true;
            if (saveName && saveNameLen > 0) {
                SDL_strlcpy(saveName, langGetText(STR_MAPEDIT_STAMP_UNTITLED), (size_t)saveNameLen);
            }
        }
        if (!hasClipboard) ImGui::EndDisabled();
        if (!hasClipboard && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", langGetText(STR_MAPEDIT_STAMP_TIP_COPY));
        }

        ImGui::SameLine();

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)
        /* Import button */
        if (ImGui::Button(langGetText(STR_MAPEDIT_STAMP_IMPORT))) {
            SDL_DialogFileFilter filters[] = {
                { "Stamp Files", "bstamp" },
                { "All Files", "*" },
            };
            SDL_ShowOpenFileDialog(stampImportCallback, nullptr, nullptr, filters, 2, nullptr, false);
        }

        /* Handle import result */
        if (s_stampImportGotResult) {
            s_stampImportGotResult = false;
            if (s_stampImportPath[0]) {
                /* Copy file to user stamps directory */
                char userDir[512];
                stampGetUserDir(userDir, sizeof(userDir));

                /* Extract filename from path */
                const char *fname = strrchr(s_stampImportPath, '/');
#ifdef _WIN32
                const char *bslash = strrchr(s_stampImportPath, '\\');
                if (bslash && (!fname || bslash > fname)) fname = bslash;
#endif
                if (fname) fname++;
                else fname = s_stampImportPath;

                char destPath[512];
                snprintf(destPath, sizeof(destPath), "%.255s/%.255s", userDir, fname);

                /* Copy file */
                FILE *src = fopen(s_stampImportPath, "rb");
                FILE *dst = fopen(destPath, "wb");
                bool copyOk = false;
                if (src && dst) {
                    char buf[4096];
                    size_t n;
                    copyOk = true;
                    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
                        if (fwrite(buf, 1, n, dst) != n) {
                            copyOk = false;
                            break;
                        }
                    }
                }
                if (src) fclose(src);
                if (dst) fclose(dst);
                if (!copyOk) {
                    remove(destPath); /* clean up partial file */
                }

                loadIndex = -2; /* signal refresh */
            }
        }

        ImGui::SameLine();
#endif

        /* Refresh button */
        if (ImGui::Button(langGetText(STR_DLGBROWSER_REFRESH))) {
            loadIndex = -2; /* signal refresh */
        }
    }

    ImGui::End();
    return loadIndex;
}

/* -------------------------------------------------------
 * Save Stamp Modal
 * ------------------------------------------------------- */

int mapEditorImguiSaveStampModal(char *name, int nameLen) {
    int result = 0;

    static bool s_stampOpen = true; s_stampOpen = true;
    if (ImGui::BeginPopupModal(langGetText(STR_MAPEDIT_STAMP_SAVE_TITLE), &s_stampOpen,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted(langGetText(STR_MAPEDIT_STAMP_SAVE_BLURB));
        ImGui::Spacing();

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        bool enter = ImGui::InputText(langGetText(STR_MAPEDIT_STAMP_SAVE_NAME), name, (size_t)nameLen,
                                       ImGuiInputTextFlags_EnterReturnsTrue);

        bool canSave = name[0] != '\0';

        int f = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                   langGetText(STR_MAPEDIT_SAVE_BTN),
                                   /*enterConfirms*/ true,
                                   /*showSeparator*/ true,
                                   /*confirmDisabled*/ !canSave);
        if (f == WBUI::FOOTER_CONFIRM || (enter && canSave)) {
            result = 1;
            ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_CANCEL) {
            result = 2;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    return result;
}

/* ---------------------------------------------------------------
 * Export as PNG dialog
 * --------------------------------------------------------------- */

/* File dialog callback for export — writes chosen path to a static buffer */
static char s_exportDialogResult[ME_PATH_MAX];
static bool s_exportDialogGotResult = false;
static bool s_exportDialogPending = false;

static void SDLCALL meExportFileDialogCallback(void *userdata,
                                                const char *const *filelist,
                                                int filter) {
    (void)userdata;
    (void)filter;
    s_exportDialogResult[0] = '\0';
    if (filelist && filelist[0]) {
        SDL_strlcpy(s_exportDialogResult, filelist[0], ME_PATH_MAX);
        /* Ensure .png extension */
        size_t len = strlen(s_exportDialogResult);
        if (len < 4 || SDL_strcasecmp(s_exportDialogResult + len - 4, ".png") != 0) {
            if (len + 4 < ME_PATH_MAX) {
                strcat(s_exportDialogResult, ".png");
            }
        }
    }
    s_exportDialogGotResult = true;
    s_exportDialogPending = false;
}

bool mapEditorImguiExportDialog(bool *open, ExportConfig *cfg,
                                char *exportPath, int exportPathLen,
                                SDL_Window *window) {
    bool triggered = false;

    /* Check if a file dialog result came in */
    if (s_exportDialogGotResult) {
        s_exportDialogGotResult = false;
        if (s_exportDialogResult[0] != '\0') {
            SDL_strlcpy(exportPath, s_exportDialogResult, (size_t)exportPathLen);
            triggered = true;
            *open = false;
            return triggered;
        }
    }

    if (!*open) return false;

    const char *exportTitle = langGetText(STR_MAPEDIT_EXPORT_TITLE);
    if (!ImGui::IsPopupOpen(exportTitle)) {
        ImGui::OpenPopup(exportTitle);
    }

    if (!ImGui::BeginPopupModal(exportTitle, open,
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove)) {
        return false;
    }

    /* Mode radio buttons */
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_EXPORT_MODE));
    ImGui::RadioButton(langGetText(STR_MAPEDIT_EXPORT_FULL), &cfg->mode, ME_EXPORT_FULL);
    ImGui::RadioButton(langGetText(STR_MAPEDIT_PREVIEW), &cfg->mode, ME_EXPORT_PREVIEW);

    ImGui::Spacing();

    /* Preview size (disabled when Full is selected) */
    if (cfg->mode == ME_EXPORT_FULL) ImGui::BeginDisabled();
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_EXPORT_PREVSIZE));
    ImGui::SameLine();
    ImGui::RadioButton("256x256", &cfg->previewSize, ME_PREVIEW_256);
    ImGui::SameLine();
    ImGui::RadioButton("512x512", &cfg->previewSize, ME_PREVIEW_512);
    if (cfg->mode == ME_EXPORT_FULL) ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Options */
    ImGui::TextUnformatted(langGetText(STR_MAPEDIT_EXPORT_OPTIONS));
    ImGui::Checkbox(langGetText(STR_MAPEDIT_EXPORT_SHOWOBJ), &cfg->showObjects);
    ImGui::Checkbox(langGetText(STR_MAPEDIT_EXPORT_SHOWMINES), &cfg->showMines);

    if (cfg->mode == ME_EXPORT_PREVIEW) ImGui::BeginDisabled();
    ImGui::Checkbox(langGetText(STR_MAPEDIT_EXPORT_SHOWGRID), &cfg->showGrid);
    if (cfg->mode == ME_EXPORT_PREVIEW) ImGui::EndDisabled();

    int f = WBUI::DialogFooter(langGetText(STR_CANCEL),
                               langGetText(STR_MAPEDIT_EXPORT_BTN));
    if (f == WBUI::FOOTER_CONFIRM) {
        if (!s_exportDialogPending) {
            SDL_DialogFileFilter filters[] = {
                { "PNG Images", "png" },
            };
            s_exportDialogPending = true;
            s_exportDialogGotResult = false;
            SDL_ShowSaveFileDialog(meExportFileDialogCallback, nullptr, window, filters, 1, nullptr);
        }
    } else if (f == WBUI::FOOTER_CANCEL) {
        *open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return triggered;
}

void ImGui_OpenPopup(const char *name) {
    ImGui::OpenPopup(name);
}

void mapEditorImguiRender(void) {
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s_meRenderer);
}
