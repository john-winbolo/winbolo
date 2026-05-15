/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapgen_imgui.cpp
 * Purpose:
 *   Reusable ImGui controls for the map generator panel.
 *   Split from mapeditor_imgui.cpp so that the map chooser
 *   dialog can use it on all platforms (including iOS)
 *   without pulling in the full map editor UI.
 *********************************************************/

#include <SDL3/SDL.h>
#include <string.h>

#include "imgui.h"
#include "nanosvg.h"
#include "nanosvgrast.h"

#include "mapgen.h"
#include "mapgen_maze.h"
#include "mapeditor_imgui.h"
#include "../gui/lang.h"

/* --- Renderer for lock icon loading --- */

static SDL_Renderer *s_genRenderer = nullptr;

/* Lock/unlock icon textures */
static SDL_Texture *s_lockIcon = nullptr;
static SDL_Texture *s_unlockIcon = nullptr;
static bool s_lockIconsLoaded = false;

void mapEditorImguiSetRenderer(SDL_Renderer *renderer) {
    s_genRenderer = renderer;
    if (s_lockIcon)   { SDL_DestroyTexture(s_lockIcon);   s_lockIcon = nullptr; }
    if (s_unlockIcon) { SDL_DestroyTexture(s_unlockIcon); s_unlockIcon = nullptr; }
    s_lockIconsLoaded = false;
}

/* --- String tables --- */
/* Lang IDs are resolved at use time so language switches take effect immediately. */

/* Terrain types for maze wall/corridor combo (non-mined only) */
static const struct { langid nameId; int value; } s_terrainTypes[] = {
    { STR_MAPEDIT_TERR_BUILDING,     BUILDING },
    { STR_MAPEDIT_TERR_HALFBUILDING, HALFBUILDING },
    { STR_MAPEDIT_TERR_ROAD,         ROAD },
    { STR_MAPEDIT_TERR_GRASS,        GRASS },
    { STR_MAPEDIT_TERR_FOREST,       FOREST },
    { STR_MAPEDIT_TERR_RIVER,        RIVER },
    { STR_MAPEDIT_TERR_SWAMP,        SWAMP },
    { STR_MAPEDIT_TERR_CRATER,       CRATER },
    { STR_MAPEDIT_TERR_RUBBLE,       RUBBLE },
    { STR_MAPEDIT_TERR_BOAT,         BOAT },
    { STR_MAPEDIT_TERR_DEEPSEA,      DEEP_SEA },
};
#define NUM_TERRAIN_TYPES 11

static bool terrainCombo(const char *label, int *terrain) {
    const char *preview = "?";
    for (int i = 0; i < NUM_TERRAIN_TYPES; i++) {
        if (s_terrainTypes[i].value == *terrain) {
            preview = langGetText(s_terrainTypes[i].nameId);
            break;
        }
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < NUM_TERRAIN_TYPES; i++) {
            bool selected = (s_terrainTypes[i].value == *terrain);
            if (ImGui::Selectable(langGetText(s_terrainTypes[i].nameId), selected)) {
                *terrain = s_terrainTypes[i].value;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

/* --- SVG icon helpers --- */

static SDL_Texture *genLoadSvgIcon(const char *path, int sizePx) {
    NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
    if (!img) return nullptr;
    if (img->width < 1.0f || img->height < 1.0f) { nsvgDelete(img); return nullptr; }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(img); return nullptr; }

    float scale = (float)sizePx / (img->width > img->height ? img->width : img->height);
    int w = sizePx, h = sizePx;
    unsigned char *pixels = (unsigned char *)SDL_malloc((size_t)(w * h * 4));
    if (!pixels) { nsvgDeleteRasterizer(rast); nsvgDelete(img); return nullptr; }

    memset(pixels, 0, (size_t)(w * h * 4));
    float ox = (sizePx - img->width * scale) * 0.5f;
    float oy = (sizePx - img->height * scale) * 0.5f;
    nsvgRasterize(rast, img, ox, oy, scale, pixels, w, h, w * 4);
    nsvgDelete(img);
    nsvgDeleteRasterizer(rast);

    /* Tint white, keep alpha */
    for (int p = 0; p < w * h; p++) {
        unsigned char a = pixels[p * 4 + 3];
        pixels[p * 4 + 0] = 255;
        pixels[p * 4 + 1] = 255;
        pixels[p * 4 + 2] = 255;
        pixels[p * 4 + 3] = a;
    }

    SDL_Texture *tex = nullptr;
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (surf) {
        tex = SDL_CreateTextureFromSurface(s_genRenderer, surf);
        SDL_DestroySurface(surf);
    }
    SDL_free(pixels);
    return tex;
}

static void genLoadLockIcons(void) {
    if (s_lockIconsLoaded) return;
    s_lockIconsLoaded = true;
    s_lockIcon   = genLoadSvgIcon("data/ui/mapeditor/locked.svg", 16);
    s_unlockIcon = genLoadSvgIcon("data/ui/mapeditor/unlock.svg", 16);
}

/* --- Lock button widget --- */

static void lockButton(const char *id, uint64_t *locks, uint64_t bit) {
    genLoadLockIcons();
    bool locked = (*locks & bit) != 0;
    SDL_Texture *icon = locked ? s_lockIcon : s_unlockIcon;

    ImGui::SameLine();
    ImGui::PushID(id);
    if (icon) {
        ImVec2 sz(16, 16);
        ImVec4 bg(0, 0, 0, 0);
        ImVec4 tint(1, 1, 1, locked ? 1.0f : 0.5f);
        if (ImGui::ImageButton(id, (ImTextureID)icon, sz, ImVec2(0,0), ImVec2(1,1), bg, tint)) {
            *locks ^= bit;
        }
    } else {
        if (ImGui::SmallButton(locked ? "L" : "U")) {
            *locks ^= bit;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(locked ? STR_MAPGEN_LOCKED_TIP : STR_MAPGEN_UNLOCKED_TIP));
    }
    ImGui::PopID();
}

/* --- Restore-locked helpers --- */

static void restoreLockedTournamentParams(MapGenConfig *cfg,
                                          const MapGenConfig *saved) {
    uint64_t lk = cfg->locks;
    auto &t = cfg->params.tournament;
    const auto &s = saved->params.tournament;
    if (lk & MAPGEN_LOCK_T_SYMMETRY)  t.symmetryMode = s.symmetryMode;
    if (lk & MAPGEN_LOCK_T_LANDMASS)  t.landMassPct = s.landMassPct;
    if (lk & MAPGEN_LOCK_T_ROUGHNESS) t.roughness = s.roughness;
    if (lk & MAPGEN_LOCK_T_ROADS)     t.includeRoads = s.includeRoads;
}

static void restoreLockedNaturalParams(MapGenConfig *cfg,
                                       const MapGenConfig *saved) {
    uint64_t lk = cfg->locks;
    auto &n = cfg->params.natural;
    const auto &s = saved->params.natural;
    if (lk & MAPGEN_LOCK_N_GRASS)      n.grassPct = s.grassPct;
    if (lk & MAPGEN_LOCK_N_FOREST)     n.forestPct = s.forestPct;
    if (lk & MAPGEN_LOCK_N_BUILDING)   n.buildingPct = s.buildingPct;
    if (lk & MAPGEN_LOCK_N_SWAMP)      n.swampPct = s.swampPct;
    if (lk & MAPGEN_LOCK_N_RIVER)      n.riverPct = s.riverPct;
    if (lk & MAPGEN_LOCK_N_BOAT)       n.boatPct = s.boatPct;
    if (lk & MAPGEN_LOCK_N_MINES)      n.mineDensityPct = s.mineDensityPct;
    if (lk & MAPGEN_LOCK_N_RIVERCOUNT) n.riverCount = s.riverCount;
    if (lk & MAPGEN_LOCK_N_CITYCOUNT)  n.cityCount = s.cityCount;
    if (lk & MAPGEN_LOCK_N_MAZECOUNT)  n.mazeCount = s.mazeCount;
}

static void restoreLockedMazeParams(MapGenConfig *cfg,
                                    const MapGenConfig *saved) {
    uint64_t lk = cfg->locks;
    auto &m = cfg->params.maze;
    const auto &s = saved->params.maze;
    if (lk & MAPGEN_LOCK_M_ALGO)      m.algo = s.algo;
    if (lk & MAPGEN_LOCK_M_WALLTHICK) m.wallThick = s.wallThick;
    if (lk & MAPGEN_LOCK_M_CORRIDOR)  m.corridorWidth = s.corridorWidth;
    if (lk & MAPGEN_LOCK_M_ENTRIES)   m.entries = s.entries;
    if (lk & MAPGEN_LOCK_M_CITYROOMS) m.cityRooms = s.cityRooms;
    if (lk & MAPGEN_LOCK_M_WALLTERR)  m.wallTerrain = s.wallTerrain;
    if (lk & MAPGEN_LOCK_M_CORRTERR)  m.corridorTerrain = s.corridorTerrain;
}

static void restoreLockedFractalParams(MapGenConfig *cfg,
                                       const MapGenConfig *saved) {
    uint64_t lk = cfg->locks;
    auto &f = cfg->params.fractal;
    const auto &s = saved->params.fractal;
    if (lk & MAPGEN_LOCK_F_LAND)      f.landPct = s.landPct;
    if (lk & MAPGEN_LOCK_F_ROUGHNESS) f.roughness = s.roughness;
    if (lk & MAPGEN_LOCK_F_DETAIL)    f.detail = s.detail;
    if (lk & MAPGEN_LOCK_F_COAST)     f.coastJaggedness = s.coastJaggedness;
    if (lk & MAPGEN_LOCK_F_LAYERS)    f.terrainLayers = s.terrainLayers;
    if (lk & MAPGEN_LOCK_F_RIVERS)    f.rivers = s.rivers;
    if (lk & MAPGEN_LOCK_F_MINES)     f.mineDensityPct = s.mineDensityPct;
}

#ifdef _MSC_VER
#pragma warning(suppress: 4505)
#endif
static void restoreLockedParams(MapGenConfig *cfg,
                                const MapGenConfig *saved) {
    uint64_t lk = cfg->locks;
    if (lk & MAPGEN_LOCK_BASES)  cfg->bases = saved->bases;
    if (lk & MAPGEN_LOCK_PILLS)  cfg->pills = saved->pills;
    if (lk & MAPGEN_LOCK_STARTS) cfg->starts = saved->starts;

    if (cfg->genType == MAPGEN_TOURNAMENT)
        restoreLockedTournamentParams(cfg, saved);
    else if (cfg->genType == MAPGEN_NATURAL)
        restoreLockedNaturalParams(cfg, saved);
    else if (cfg->genType == MAPGEN_MAZE)
        restoreLockedMazeParams(cfg, saved);
    else if (cfg->genType == MAPGEN_FRACTAL)
        restoreLockedFractalParams(cfg, saved);
}

/* --- Main controls function --- */

bool mapGenImguiControls(MapGenConfig *cfg) {
    bool generated = false;
    static char seedBuf[64] = "";
    static bool seedFieldActive = false;

    MapGenConfig prevCfg = *cfg;

    if (!seedFieldActive) {
        mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
    }

    /* Generator type */
    int prevGenType = cfg->genType;
    const char *s_genTypeNames[MAPGEN_TYPE_COUNT] = {
        langGetText(STR_MAPGEN_TYPE_TOURNAMENT),
        langGetText(STR_MAPGEN_TYPE_NATURAL),
        langGetText(STR_MAPGEN_TYPE_MAZE),
        langGetText(STR_MAPGEN_TYPE_FRACTAL),
    };
    ImGui::Combo(langGetText(STR_MAPGEN_GENERATOR), &cfg->genType, s_genTypeNames, MAPGEN_TYPE_COUNT);
    lockButton("lk_gentype", &cfg->locks, MAPGEN_LOCK_GENTYPE);
    if (cfg->genType != prevGenType) {
        static MapGenConfig savedConfigs[MAPGEN_TYPE_COUNT];
        static bool hasSaved[MAPGEN_TYPE_COUNT] = {false, false, false, false};
        savedConfigs[prevGenType] = *cfg;
        savedConfigs[prevGenType].genType = prevGenType;
        hasSaved[prevGenType] = true;

        uint32_t savedSeed = cfg->seed;
        uint64_t savedLocks = cfg->locks;
        int savedBases = cfg->bases;
        int savedPills = cfg->pills;
        int savedStarts = cfg->starts;
        int savedX1 = cfg->x1, savedY1 = cfg->y1;
        int savedX2 = cfg->x2, savedY2 = cfg->y2;

        if (hasSaved[cfg->genType]) {
            *cfg = savedConfigs[cfg->genType];
        } else {
            *cfg = mapGenDefaultConfig(cfg->genType);
        }
        cfg->seed = savedSeed;
        cfg->locks = savedLocks;
        cfg->bases = savedBases;
        cfg->pills = savedPills;
        cfg->starts = savedStarts;
        cfg->x1 = savedX1; cfg->y1 = savedY1;
        cfg->x2 = savedX2; cfg->y2 = savedY2;
    }

    /* Seed */
    ImGui::Separator();
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText(langGetText(STR_MAPGEN_SEED), seedBuf, sizeof(seedBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (mapGenSeedToConfig(seedBuf, cfg)) {
            mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
        }
    }
    seedFieldActive = ImGui::IsItemActive();
    lockButton("lk_seed", &cfg->locks, MAPGEN_LOCK_SEED);
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_MAPGEN_RANDOMIZE))) {
        uint32_t rng = (uint32_t)SDL_GetTicksNS();
        uint64_t lk = cfg->locks;
        if (!(lk & MAPGEN_LOCK_SEED))    cfg->seed = rng;
        int prevType = cfg->genType;
        if (!(lk & MAPGEN_LOCK_GENTYPE)) cfg->genType = (int)(mapGenXorshift32(&rng) % MAPGEN_TYPE_COUNT);
        if (cfg->genType != prevType) {
            int savedBases = cfg->bases, savedPills = cfg->pills, savedStarts = cfg->starts;
            MapGenConfig fresh = mapGenDefaultConfig(cfg->genType);
            cfg->params = fresh.params;
            cfg->bases = savedBases;
            cfg->pills = savedPills;
            cfg->starts = savedStarts;
        }
        if (!(lk & MAPGEN_LOCK_BASES))  cfg->bases = 1 + (int)(mapGenXorshift32(&rng) % 16);
        if (!(lk & MAPGEN_LOCK_PILLS))  cfg->pills = 1 + (int)(mapGenXorshift32(&rng) % 16);
        if (!(lk & MAPGEN_LOCK_STARTS)) cfg->starts = 1 + (int)(mapGenXorshift32(&rng) % 16);

        if (cfg->genType == MAPGEN_TOURNAMENT) {
            auto &t = cfg->params.tournament;
            if (!(lk & MAPGEN_LOCK_T_SYMMETRY))  t.symmetryMode = (int)(mapGenXorshift32(&rng) % MAPGEN_SYM_COUNT);
            if (!(lk & MAPGEN_LOCK_T_LANDMASS))  t.landMassPct = (int)(mapGenXorshift32(&rng) % 25) + 1;
            if (!(lk & MAPGEN_LOCK_T_ROUGHNESS)) t.roughness = (int)(mapGenXorshift32(&rng) % MAPGEN_ROUGH_COUNT);
            if (!(lk & MAPGEN_LOCK_T_ROADS))     t.includeRoads = (mapGenXorshift32(&rng) & 1) != 0;
        } else if (cfg->genType == MAPGEN_NATURAL) {
            auto &n = cfg->params.natural;
            if (!(lk & MAPGEN_LOCK_N_STYLE)) {
                MapGenConfig saved = *cfg;
                n.mapStyle = (int)(mapGenXorshift32(&rng) % MAPGEN_STYLE_COUNT);
                mapGenNaturalStyleDefaults(n.mapStyle, cfg);
                restoreLockedNaturalParams(cfg, &saved);
            }
            if (!(lk & MAPGEN_LOCK_N_GRASS))      n.grassPct = (int)(mapGenXorshift32(&rng) % 60);
            if (!(lk & MAPGEN_LOCK_N_FOREST))      n.forestPct = (int)(mapGenXorshift32(&rng) % 40);
            if (!(lk & MAPGEN_LOCK_N_BUILDING))    n.buildingPct = (int)(mapGenXorshift32(&rng) % 15);
            if (!(lk & MAPGEN_LOCK_N_SWAMP))       n.swampPct = (int)(mapGenXorshift32(&rng) % 20);
            if (!(lk & MAPGEN_LOCK_N_RIVER))       n.riverPct = (int)(mapGenXorshift32(&rng) % 20);
            if (!(lk & MAPGEN_LOCK_N_BOAT))        n.boatPct = (int)(mapGenXorshift32(&rng) % 16);
            if (!(lk & MAPGEN_LOCK_N_MINES))       n.mineDensityPct = (int)(mapGenXorshift32(&rng) % 6);
            if (!(lk & MAPGEN_LOCK_N_RIVERCOUNT))  n.riverCount = (int)(mapGenXorshift32(&rng) % 6);
            if (!(lk & MAPGEN_LOCK_N_CITYCOUNT))   n.cityCount = (int)(mapGenXorshift32(&rng) % 11);
            if (!(lk & MAPGEN_LOCK_N_MAZECOUNT))   n.mazeCount = (int)(mapGenXorshift32(&rng) % 6);
        } else if (cfg->genType == MAPGEN_MAZE) {
            auto &m = cfg->params.maze;
            if (!(lk & MAPGEN_LOCK_M_ALGO))      m.algo = (int)(mapGenXorshift32(&rng) % MAZE_ALGO_COUNT);
            if (!(lk & MAPGEN_LOCK_M_WALLTHICK))  m.wallThick = 1 + (int)(mapGenXorshift32(&rng) % 2);
            if (!(lk & MAPGEN_LOCK_M_CORRIDOR))   m.corridorWidth = 1 + (int)(mapGenXorshift32(&rng) % 2);
            if (!(lk & MAPGEN_LOCK_M_ENTRIES))    m.entries = 1 + (int)(mapGenXorshift32(&rng) % 8);
            if (!(lk & MAPGEN_LOCK_M_CITYROOMS))  m.cityRooms = (int)(mapGenXorshift32(&rng) % 6);
            if (!(lk & MAPGEN_LOCK_M_WALLTERR))   m.wallTerrain = s_terrainTypes[mapGenXorshift32(&rng) % NUM_TERRAIN_TYPES].value;
            if (!(lk & MAPGEN_LOCK_M_CORRTERR))   m.corridorTerrain = s_terrainTypes[mapGenXorshift32(&rng) % NUM_TERRAIN_TYPES].value;
        } else if (cfg->genType == MAPGEN_FRACTAL) {
            auto &f = cfg->params.fractal;
            if (!(lk & MAPGEN_LOCK_F_LAND))      f.landPct = 5 + (int)(mapGenXorshift32(&rng) % 76);
            if (!(lk & MAPGEN_LOCK_F_ROUGHNESS)) f.roughness = 1 + (int)(mapGenXorshift32(&rng) % 10);
            if (!(lk & MAPGEN_LOCK_F_DETAIL))    f.detail = 1 + (int)(mapGenXorshift32(&rng) % 5);
            if (!(lk & MAPGEN_LOCK_F_COAST))     f.coastJaggedness = (int)(mapGenXorshift32(&rng) % 11);
            if (!(lk & MAPGEN_LOCK_F_LAYERS))    f.terrainLayers = 2 + (int)(mapGenXorshift32(&rng) % 5);
            if (!(lk & MAPGEN_LOCK_F_RIVERS)) {
                f.rivers = (mapGenXorshift32(&rng) & 1) != 0;
            }
            if (f.rivers) f.lakes = (mapGenXorshift32(&rng) & 1) != 0;
            if (!f.rivers) f.lakes = false;
            if (!(lk & MAPGEN_LOCK_F_MINES))     f.mineDensityPct = (int)(mapGenXorshift32(&rng) % 4);
        }
        mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
        generated = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGGAMEINFO_COPYSEED))) {
        mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
        SDL_SetClipboardText(seedBuf);
    }

    /* Generator-specific controls */
    if (cfg->genType == MAPGEN_TOURNAMENT) {
        auto &t = cfg->params.tournament;

        const char *s_symModeNames[MAPGEN_SYM_COUNT] = {
            langGetText(STR_MAPGEN_SYM_4CORNER),
            langGetText(STR_MAPGEN_SYM_MIRROR_H),
            langGetText(STR_MAPGEN_SYM_MIRROR_V),
            langGetText(STR_MAPGEN_SYM_ROT180),
            langGetText(STR_MAPGEN_SYM_ROT90),
        };
        ImGui::Combo(langGetText(STR_MAPGEN_SYMMETRY), &t.symmetryMode, s_symModeNames, MAPGEN_SYM_COUNT);
        lockButton("lk_sym", &cfg->locks, MAPGEN_LOCK_T_SYMMETRY);
        ImGui::SliderInt(langGetText(STR_MAPGEN_LANDMASS_PCT), &t.landMassPct, 1, 25);
        lockButton("lk_land", &cfg->locks, MAPGEN_LOCK_T_LANDMASS);
        const char *s_roughnessNames[MAPGEN_ROUGH_COUNT] = {
            langGetText(STR_MAPGEN_ROUGH_LOW),
            langGetText(STR_MAPGEN_ROUGH_MED),
            langGetText(STR_MAPGEN_ROUGH_HIGH),
        };
        ImGui::Combo(langGetText(STR_MAPGEN_ROUGHNESS), &t.roughness, s_roughnessNames, MAPGEN_ROUGH_COUNT);
        lockButton("lk_rough", &cfg->locks, MAPGEN_LOCK_T_ROUGHNESS);
        ImGui::Checkbox(langGetText(STR_MAPGEN_INCLUDE_ROADS), &t.includeRoads);
        lockButton("lk_roads", &cfg->locks, MAPGEN_LOCK_T_ROADS);

    } else if (cfg->genType == MAPGEN_NATURAL) {
        auto &n = cfg->params.natural;

        int prevStyle = n.mapStyle;
        const char *s_mapStyleNames[MAPGEN_STYLE_COUNT] = {
            langGetText(STR_MAPGEN_STYLE_OCEAN),
            langGetText(STR_MAPGEN_STYLE_CONTINENT),
            langGetText(STR_MAPGEN_STYLE_ISLANDS),
            langGetText(STR_MAPGEN_STYLE_ARCHIPELAGO),
            langGetText(STR_MAPGEN_STYLE_INLAND),
        };
        ImGui::Combo(langGetText(STR_MAPGEN_MAP_STYLE), &n.mapStyle, s_mapStyleNames, MAPGEN_STYLE_COUNT);
        lockButton("lk_style", &cfg->locks, MAPGEN_LOCK_N_STYLE);
        if (n.mapStyle != prevStyle) {
            MapGenConfig saved = *cfg;
            mapGenNaturalStyleDefaults(n.mapStyle, cfg);
            restoreLockedNaturalParams(cfg, &saved);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted(langGetText(STR_MAPGEN_TERRAIN_MIX));

        int *terrainPcts[] = { &n.grassPct, &n.forestPct, &n.buildingPct,
                               &n.swampPct, &n.riverPct };
        const uint32_t terrainLocks[] = {
            MAPGEN_LOCK_N_GRASS, MAPGEN_LOCK_N_FOREST, MAPGEN_LOCK_N_BUILDING,
            MAPGEN_LOCK_N_SWAMP, MAPGEN_LOCK_N_RIVER };
        const char *terrainLabels[] = {
            langGetText(STR_MAPGEN_GRASS_PCT),
            langGetText(STR_MAPGEN_FOREST_PCT),
            langGetText(STR_MAPGEN_BUILDING_PCT),
            langGetText(STR_MAPGEN_SWAMP_PCT),
            langGetText(STR_MAPGEN_RIVER_PCT),
        };
        const char *terrainLockIds[] = { "lk_grass", "lk_forest", "lk_bldg",
                                         "lk_swamp", "lk_river" };
        const int numTerrainSliders = 5;

        for (int si = 0; si < numTerrainSliders; si++) {
            int prev = *terrainPcts[si];
            ImGui::SliderInt(terrainLabels[si], terrainPcts[si], 0, 100);
            lockButton(terrainLockIds[si], &cfg->locks, terrainLocks[si]);

            if (*terrainPcts[si] != prev) {
                int total = 0;
                for (int k = 0; k < numTerrainSliders; k++)
                    total += *terrainPcts[k];
                if (total > 100) {
                    int excess = total - 100;
                    int otherSum = 0;
                    for (int k = 0; k < numTerrainSliders; k++) {
                        if (k == si) continue;
                        if (!(cfg->locks & terrainLocks[k]))
                            otherSum += *terrainPcts[k];
                    }
                    if (otherSum > 0) {
                        int reduced = 0;
                        for (int k = 0; k < numTerrainSliders; k++) {
                            if (k == si) continue;
                            if (cfg->locks & terrainLocks[k]) continue;
                            int share = *terrainPcts[k] * excess / otherSum;
                            if (share > *terrainPcts[k]) share = *terrainPcts[k];
                            *terrainPcts[k] -= share;
                            reduced += share;
                        }
                        int leftover = excess - reduced;
                        for (int k = 0; k < numTerrainSliders && leftover > 0; k++) {
                            if (k == si) continue;
                            if (cfg->locks & terrainLocks[k]) continue;
                            int take = leftover < *terrainPcts[k] ? leftover : *terrainPcts[k];
                            *terrainPcts[k] -= take;
                            leftover -= take;
                        }
                    }
                }
            }
        }

        ImGui::SliderInt(langGetText(STR_MAPGEN_BOAT_PCT), &n.boatPct, 0, 100);
        lockButton("lk_boat", &cfg->locks, MAPGEN_LOCK_N_BOAT);

        int total = n.grassPct + n.forestPct + n.buildingPct + n.swampPct + n.riverPct;
        int remaining = 100 - total;
        if (remaining < 0) remaining = 0;
        {
            MessageArgs args = {};
            args.number = remaining;
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPGEN_REMAINING_GRASS, &args));
        }

        ImGui::Spacing();
        ImGui::SliderInt(langGetText(STR_MAPGEN_MINE_DENSITY), &n.mineDensityPct, 0, 5);
        lockButton("lk_mines", &cfg->locks, MAPGEN_LOCK_N_MINES);
        ImGui::SliderInt(langGetText(STR_MAPGEN_RIVER_COUNT), &n.riverCount, 0, 5);
        lockButton("lk_rcount", &cfg->locks, MAPGEN_LOCK_N_RIVERCOUNT);
        ImGui::SliderInt(langGetText(STR_MAPGEN_CITY_COUNT), &n.cityCount, 0, 10);
        lockButton("lk_ccount", &cfg->locks, MAPGEN_LOCK_N_CITYCOUNT);
        ImGui::SliderInt(langGetText(STR_MAPGEN_MAZE_COUNT), &n.mazeCount, 0, 5);
        lockButton("lk_mzcount", &cfg->locks, MAPGEN_LOCK_N_MAZECOUNT);

    } else if (cfg->genType == MAPGEN_MAZE) {
        auto &m = cfg->params.maze;

        const char *s_mazeAlgoNames[MAZE_ALGO_COUNT] = {
            langGetText(STR_MAPEDIT_ALGO_LABYRINTH),
            langGetText(STR_MAPEDIT_ALGO_OPEN),
        };
        ImGui::Combo(langGetText(STR_MAPEDIT_ALGORITHM), &m.algo, s_mazeAlgoNames, MAZE_ALGO_COUNT);
        lockButton("lk_malgo", &cfg->locks, MAPGEN_LOCK_M_ALGO);

        int wallIdx = m.wallThick - 1;
        ImGui::SliderInt(langGetText(STR_MAPGEN_WALL_THICKNESS), &wallIdx, 0, 1, wallIdx == 0 ? "1" : "2");
        m.wallThick = wallIdx + 1;
        lockButton("lk_mwall", &cfg->locks, MAPGEN_LOCK_M_WALLTHICK);

        int corrIdx = m.corridorWidth - 1;
        ImGui::SliderInt(langGetText(STR_MAPGEN_CORRIDOR_WIDTH), &corrIdx, 0, 1, corrIdx == 0 ? "1" : "2");
        m.corridorWidth = corrIdx + 1;
        lockButton("lk_mcorr", &cfg->locks, MAPGEN_LOCK_M_CORRIDOR);

        ImGui::SliderInt(langGetText(STR_MAPEDIT_ENTRIES), &m.entries, 1, 8);
        lockButton("lk_mentries", &cfg->locks, MAPGEN_LOCK_M_ENTRIES);

        ImGui::SliderInt(langGetText(STR_MAPGEN_CITY_ROOMS), &m.cityRooms, 0, 5);
        lockButton("lk_mrooms", &cfg->locks, MAPGEN_LOCK_M_CITYROOMS);

        ImGui::Spacing();
        terrainCombo(langGetText(STR_MAPEDIT_WALLTERRAIN), &m.wallTerrain);
        lockButton("lk_mwterr", &cfg->locks, MAPGEN_LOCK_M_WALLTERR);
        terrainCombo(langGetText(STR_MAPEDIT_CORRIDORTERRAIN), &m.corridorTerrain);
        lockButton("lk_mcterr", &cfg->locks, MAPGEN_LOCK_M_CORRTERR);

    } else if (cfg->genType == MAPGEN_FRACTAL) {
        auto &f = cfg->params.fractal;

        ImGui::SliderInt(langGetText(STR_MAPGEN_LAND_COVERAGE), &f.landPct, 5, 80);
        lockButton("lk_fland", &cfg->locks, MAPGEN_LOCK_F_LAND);
        ImGui::SliderInt(langGetText(STR_MAPGEN_ROUGHNESS), &f.roughness, 1, 10);
        lockButton("lk_frough", &cfg->locks, MAPGEN_LOCK_F_ROUGHNESS);
        ImGui::SliderInt(langGetText(STR_MAPGEN_DETAIL_PASSES), &f.detail, 1, 5);
        lockButton("lk_fdetail", &cfg->locks, MAPGEN_LOCK_F_DETAIL);
        ImGui::SliderInt(langGetText(STR_MAPGEN_COAST_JAG), &f.coastJaggedness, 0, 10);
        lockButton("lk_fcoast", &cfg->locks, MAPGEN_LOCK_F_COAST);
        ImGui::SliderInt(langGetText(STR_MAPGEN_TERRAIN_LAYERS), &f.terrainLayers, 2, 6);
        lockButton("lk_flayers", &cfg->locks, MAPGEN_LOCK_F_LAYERS);

        ImGui::Checkbox(langGetText(STR_MAPGEN_RIVERS), &f.rivers);
        lockButton("lk_frivers", &cfg->locks, MAPGEN_LOCK_F_RIVERS);
        if (!f.rivers) ImGui::BeginDisabled();
        ImGui::Checkbox(langGetText(STR_MAPGEN_LAKES), &f.lakes);
        if (!f.rivers) {
            f.lakes = false;
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::SliderInt(langGetText(STR_MAPGEN_MINE_DENSITY), &f.mineDensityPct, 0, 5);
        lockButton("lk_fmines", &cfg->locks, MAPGEN_LOCK_F_MINES);
    }

    /* Shared bases/pills/starts */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::SliderInt(langGetText(STR_TABLET_BASES), &cfg->bases, 0, 16);
    lockButton("lk_bases", &cfg->locks, MAPGEN_LOCK_BASES);
    ImGui::SliderInt(langGetText(STR_TABLET_PILLBOXES), &cfg->pills, 0, 16);
    lockButton("lk_pills", &cfg->locks, MAPGEN_LOCK_PILLS);
    ImGui::SliderInt(langGetText(STR_MAPEDIT_STATS_STARTS), &cfg->starts, 0, 16);
    lockButton("lk_starts", &cfg->locks, MAPGEN_LOCK_STARTS);

    /* Detect parameter changes (ignore lock-only changes) */
    if (!generated) {
        uint64_t savedLocks = prevCfg.locks;
        prevCfg.locks = cfg->locks;
        if (memcmp(&prevCfg, cfg, sizeof(MapGenConfig)) != 0) {
            generated = true;
        }
        prevCfg.locks = savedLocks;
    }

    return generated;
}
