/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* Map Maker preset names, in the order of the preset table in
 * src/bolo/mapgen_maker.c. */
static const langid s_presetNameIds[] = {
    STR_MAPGEN_PRESET_BERSERKIR,
    STR_MAPGEN_PRESET_SLUGGO_TOURNAMENT,
    STR_MAPGEN_PRESET_SLUGDEATH,
    STR_MAPGEN_PRESET_SLUGFEST,
    STR_MAPGEN_PRESET_SLUGGO_ADVENTUROUS,
    STR_MAPGEN_PRESET_SLUGGO_SYMMETRIC,
    STR_MAPGEN_PRESET_STOOGEFEST,
    STR_MAPGEN_PRESET_BLACK_LIGHTNING,
    STR_MAPGEN_PRESET_BL_ISLANDS,
    STR_MAPGEN_PRESET_DEATHHAND,
    STR_MAPGEN_PRESET_MEGAWATT_TC,
    STR_MAPGEN_PRESET_MEGAWATT_ISLANDS,
    STR_MAPGEN_PRESET_MOSH_PIT,
    STR_MAPGEN_PRESET_PALPATINE,
    STR_MAPGEN_PRESET_CHEW_TOY,
};
#define NUM_PRESET_NAMES ((int)(sizeof(s_presetNameIds) / sizeof(s_presetNameIds[0])))

static const char *presetLabel(int preset) {
    if (preset >= 0 && preset < NUM_PRESET_NAMES) {
        return langGetText(s_presetNameIds[preset]);
    }
    const char *name = mapGenPresetName(preset);
    return name ? name : "?";
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

/* --- Panel locks --- */

/* The locks of the controls inside the parameter panel for one
 * generator type. Only these locks have a button. The controls above
 * the panel (generator, each type's style choices, size, bases,
 * pillboxes and starts) have no lock: Randomize never changes them. */
static uint64_t panelLocks(int genType) {
    switch (genType) {
        case MAPGEN_TOURNAMENT:
            return MAPGEN_LOCK_T_LANDMASS | MAPGEN_LOCK_T_ROUGHNESS |
                   MAPGEN_LOCK_T_ROADS | MAPGEN_LOCK_T_WATERBARRIER;
        case MAPGEN_NATURAL:
            return MAPGEN_LOCK_N_GRASS | MAPGEN_LOCK_N_FOREST |
                   MAPGEN_LOCK_N_BUILDING | MAPGEN_LOCK_N_SWAMP | MAPGEN_LOCK_N_RIVER |
                   MAPGEN_LOCK_N_BOAT | MAPGEN_LOCK_N_MINES | MAPGEN_LOCK_N_RIVERCOUNT |
                   MAPGEN_LOCK_N_CITYCOUNT | MAPGEN_LOCK_N_MAZECOUNT;
        case MAPGEN_MAZE:
            return MAPGEN_LOCK_M_WALLTHICK | MAPGEN_LOCK_M_CORRIDOR |
                   MAPGEN_LOCK_M_ENTRIES | MAPGEN_LOCK_M_CITYROOMS;
        case MAPGEN_FRACTAL:
            return MAPGEN_LOCK_F_LAND | MAPGEN_LOCK_F_ROUGHNESS |
                   MAPGEN_LOCK_F_DETAIL | MAPGEN_LOCK_F_COAST | MAPGEN_LOCK_F_LAYERS |
                   MAPGEN_LOCK_F_RIVERS | MAPGEN_LOCK_F_MINES;
        case MAPGEN_MAKER:
            return MAPGEN_LOCK_K_WATER | MAPGEN_LOCK_K_FOREST |
                   MAPGEN_LOCK_K_ROAD | MAPGEN_LOCK_K_BUILDING | MAPGEN_LOCK_K_ISLANDS |
                   MAPGEN_LOCK_K_MINES | MAPGEN_LOCK_K_DETAIL;
        default:
            return 0;
    }
}

/* The locks of the Map Maker style panel: the outline and one for
 * each style lever. */
static const uint64_t kStyleLocks =
    MAPGEN_LOCK_K_OUTLINE |
    MAPGEN_LOCK_KS_FIRST * ((1ull << MAPGEN_KSTYLE_COUNT) - 1);

/* Every lock that has a button for some generator type, and the locks
 * of the style panel. Other bits (generator, seed, size, bases,
 * pillboxes, starts, preset, symmetry, style, maze algorithm and
 * terrains) have no button, so they are cleared and no code reads
 * them. */
static uint64_t allPanelLocks(void) {
    uint64_t m = kStyleLocks;
    for (int t = 0; t < MAPGEN_TYPE_COUNT; t++) m |= panelLocks(t);
    return m;
}

/* --- Restore-locked helpers --- */

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

/* A random named outline (never Varied) from a preset's outline set,
 * or -1 if the set is empty. */
static int pickPresetOutline(int preset, uint32_t *rng) {
    int set = mapGenPresetOutlineSet(preset);
    int opts[MAPGEN_OUTLINE_COUNT], no = 0;
    for (int i = 0; i < MAPGEN_OUTLINE_VARIED; i++) {
        if (set & (1 << i)) opts[no++] = i;
    }
    if (no == 0) return -1;
    if (no == 1) return opts[0];
    return opts[mapGenXorshift32(rng) % (uint32_t)no];
}

/* A seed for the UI's own random picks, from the clock. */
static uint32_t uiRngSeed(void) {
    uint64_t now = SDL_GetTicksNS();
    uint32_t rng = (uint32_t)now ^ (uint32_t)(now >> 32) ^ 0x9E3779B9u;
    return rng ? rng : 1;
}

/* After the UI applies a preset: a preset outline of Varied becomes
 * one named outline from the preset's set, so the player sees which
 * one. A locked outline is not changed. The generator, the seed
 * format and the server still accept Varied. */
static void nameVariedOutline(MapGenConfig *cfg) {
    auto &k = cfg->params.maker;
    if (k.outline != MAPGEN_OUTLINE_VARIED) return;
    if (cfg->locks & MAPGEN_LOCK_K_OUTLINE) return;
    uint32_t rng = uiRngSeed();
    int o = pickPresetOutline(k.preset, &rng);
    if (o >= 0) k.outline = o;
}

/* After a preset is picked: put back the locked panel values. The
 * preset always sets the symmetry, size, bases, pillboxes and
 * starts, and sets each style lever to From seed. A locked style
 * value comes back only if the new preset allows it. */
static void restoreLockedMakerParams(MapGenConfig *cfg,
                                     const MapGenConfig *saved) {
    uint64_t lk = cfg->locks;
    auto &k = cfg->params.maker;
    const auto &s = saved->params.maker;
    if (lk & MAPGEN_LOCK_K_OUTLINE)  k.outline = s.outline;
    if (lk & MAPGEN_LOCK_K_WATER)    k.waterPct = s.waterPct;
    if (lk & MAPGEN_LOCK_K_FOREST)   k.forestPct = s.forestPct;
    if (lk & MAPGEN_LOCK_K_ROAD)     k.roadPct = s.roadPct;
    if (lk & MAPGEN_LOCK_K_BUILDING) k.buildingPct = s.buildingPct;
    if (lk & MAPGEN_LOCK_K_ISLANDS)  k.islands = s.islands;
    if (lk & MAPGEN_LOCK_K_MINES)    k.mineDensityPct = s.mineDensityPct;
    if (lk & MAPGEN_LOCK_K_DETAIL)   k.detail = s.detail;
    for (int i = 0; i < MAPGEN_KSTYLE_COUNT; i++) {
        if ((lk & (MAPGEN_LOCK_KS_FIRST << i)) &&
            mapGenStyleAllowed(k.preset, i, s.style[i])) {
            k.style[i] = s.style[i];
        }
    }
}

/* --- Randomize --- */

/* A value in [centre - half, centre + half], cut to [lo, hi]. */
static int bandPick(uint32_t *rng, int centre, int half, int lo, int hi) {
    int a = centre - half, b = centre + half;
    if (a < lo) a = lo;
    if (b > hi) b = hi;
    if (b < a) return a;
    return a + (int)(mapGenXorshift32(rng) % (uint32_t)(b - a + 1));
}

/* The panel's Randomize: a new seed every time, and each unlocked
 * panel value in a small band round the style's own value (the Map
 * Maker preset, the Natural style, or the type's defaults). The Map
 * Maker outline takes one of the preset's own outlines. The band
 * is round the style value, not the current value, so many clicks
 * stay in the style. The generator type, seed format, the style
 * choices, size and the counts above the panel do not change. */
static void randomizePanel(MapGenConfig *cfg) {
    uint32_t rng = uiRngSeed();
    uint32_t seed;
    do {
        seed = mapGenXorshift32(&rng);
    } while (seed == 0 || seed == cfg->seed);
    cfg->seed = seed;

    const uint64_t lk = cfg->locks;

    /* The style values the bands are round. */
    MapGenConfig base = mapGenDefaultConfig(cfg->genType);
    if (cfg->genType == MAPGEN_MAKER) {
        mapGenApplyPreset(cfg->params.maker.preset, &base);
    } else if (cfg->genType == MAPGEN_NATURAL) {
        mapGenNaturalStyleDefaults(cfg->params.natural.mapStyle, &base);
    }

    if (cfg->genType == MAPGEN_TOURNAMENT) {
        auto &t = cfg->params.tournament;
        const auto &b = base.params.tournament;
        if (!(lk & MAPGEN_LOCK_T_LANDMASS))  t.landMassPct = bandPick(&rng, b.landMassPct, 3, 1, 25);
        if (!(lk & MAPGEN_LOCK_T_ROUGHNESS)) t.roughness = bandPick(&rng, b.roughness, 1, 0, MAPGEN_ROUGH_COUNT - 1);
        if (!(lk & MAPGEN_LOCK_T_ROADS))     t.includeRoads = (mapGenXorshift32(&rng) & 1) != 0;
        if (!(lk & MAPGEN_LOCK_T_WATERBARRIER))
            t.waterBarrier = bandPick(&rng, b.waterBarrier, 1, 0, 5);
    } else if (cfg->genType == MAPGEN_NATURAL) {
        auto &n = cfg->params.natural;
        const auto &b = base.params.natural;
        int *pcts[] = { &n.grassPct, &n.forestPct, &n.buildingPct, &n.swampPct, &n.riverPct };
        const int basePcts[] = { b.grassPct, b.forestPct, b.buildingPct, b.swampPct, b.riverPct };
        const uint64_t pctLocks[] = { MAPGEN_LOCK_N_GRASS, MAPGEN_LOCK_N_FOREST,
                                      MAPGEN_LOCK_N_BUILDING, MAPGEN_LOCK_N_SWAMP,
                                      MAPGEN_LOCK_N_RIVER };
        for (int i = 0; i < 5; i++) {
            if (!(lk & pctLocks[i])) *pcts[i] = bandPick(&rng, basePcts[i], 10, 0, 100);
        }
        /* The five terrain values share 100%: take the excess off the
         * largest unlocked value, one point at a time. */
        for (;;) {
            int total = 0, big = -1;
            for (int i = 0; i < 5; i++) {
                total += *pcts[i];
                if (!(lk & pctLocks[i]) && *pcts[i] > 0 &&
                    (big < 0 || *pcts[i] > *pcts[big])) big = i;
            }
            if (total <= 100 || big < 0) break;
            (*pcts[big])--;
        }
        if (!(lk & MAPGEN_LOCK_N_BOAT))       n.boatPct = bandPick(&rng, b.boatPct, 10, 0, 100);
        if (!(lk & MAPGEN_LOCK_N_MINES))      n.mineDensityPct = bandPick(&rng, b.mineDensityPct, 1, 0, 5);
        if (!(lk & MAPGEN_LOCK_N_RIVERCOUNT)) n.riverCount = bandPick(&rng, b.riverCount, 1, 0, 5);
        if (!(lk & MAPGEN_LOCK_N_CITYCOUNT))  n.cityCount = bandPick(&rng, b.cityCount, 1, 0, 10);
        if (!(lk & MAPGEN_LOCK_N_MAZECOUNT))  n.mazeCount = bandPick(&rng, b.mazeCount, 1, 0, 5);
    } else if (cfg->genType == MAPGEN_MAZE) {
        auto &m = cfg->params.maze;
        const auto &b = base.params.maze;
        if (!(lk & MAPGEN_LOCK_M_WALLTHICK)) m.wallThick = bandPick(&rng, b.wallThick, 1, 1, 2);
        if (!(lk & MAPGEN_LOCK_M_CORRIDOR))  m.corridorWidth = bandPick(&rng, b.corridorWidth, 1, 1, 2);
        if (!(lk & MAPGEN_LOCK_M_ENTRIES))   m.entries = bandPick(&rng, b.entries, 2, 1, 8);
        if (!(lk & MAPGEN_LOCK_M_CITYROOMS)) m.cityRooms = bandPick(&rng, b.cityRooms, 1, 0, 5);
    } else if (cfg->genType == MAPGEN_FRACTAL) {
        auto &f = cfg->params.fractal;
        const auto &b = base.params.fractal;
        if (!(lk & MAPGEN_LOCK_F_LAND))      f.landPct = bandPick(&rng, b.landPct, 10, 5, 80);
        if (!(lk & MAPGEN_LOCK_F_ROUGHNESS)) f.roughness = bandPick(&rng, b.roughness, 1, 1, 10);
        if (!(lk & MAPGEN_LOCK_F_DETAIL))    f.detail = bandPick(&rng, b.detail, 1, 1, 5);
        if (!(lk & MAPGEN_LOCK_F_COAST))     f.coastJaggedness = bandPick(&rng, b.coastJaggedness, 1, 0, 10);
        if (!(lk & MAPGEN_LOCK_F_LAYERS))    f.terrainLayers = bandPick(&rng, b.terrainLayers, 1, 2, 6);
        if (!(lk & MAPGEN_LOCK_F_RIVERS))    f.rivers = (mapGenXorshift32(&rng) & 1) != 0;
        /* Lakes has no lock of its own and follows Rivers. */
        f.lakes = f.rivers ? (mapGenXorshift32(&rng) & 1) != 0 : false;
        if (!(lk & MAPGEN_LOCK_F_MINES))     f.mineDensityPct = bandPick(&rng, b.mineDensityPct, 1, 0, 5);
    } else if (cfg->genType == MAPGEN_MAKER) {
        auto &k = cfg->params.maker;
        const auto &b = base.params.maker;
        /* The outline is in the style panel: Randomize style changes
         * it, not this. */
        if (!(lk & MAPGEN_LOCK_K_WATER))    k.waterPct = bandPick(&rng, b.waterPct, 10, 0, 100);
        if (!(lk & MAPGEN_LOCK_K_FOREST))   k.forestPct = bandPick(&rng, b.forestPct, 10, 0, 100);
        if (!(lk & MAPGEN_LOCK_K_ROAD))     k.roadPct = bandPick(&rng, b.roadPct, 10, 0, 100);
        if (!(lk & MAPGEN_LOCK_K_BUILDING)) k.buildingPct = bandPick(&rng, b.buildingPct, 10, 0, 100);
        /* Islands above 1 cuts the land of a formal or town preset
         * into separate pieces with deep channels, which none of
         * their real maps have. Only the island presets get a band. */
        if (!(lk & MAPGEN_LOCK_K_ISLANDS)) {
            k.islands = mapGenPresetIsIsland(k.preset)
                            ? bandPick(&rng, b.islands, 1, 1, 8)
                            : b.islands;
        }
        /* Mines stay at the preset's value: a band round 0 would put
         * mines on maps whose maker never used them. */
        if (!(lk & MAPGEN_LOCK_K_MINES))    k.mineDensityPct = b.mineDensityPct;
        if (!(lk & MAPGEN_LOCK_K_DETAIL))   k.detail = bandPick(&rng, b.detail, 1, 0, 10);
    }
}

/* --- Style panel (Map Maker) --- */

/* True if the style panel shows the lever: the preset varies it.
 * Shape is not shown for quarter turns, because the symmetry is
 * above the panels and no Randomize changes it. */
static bool styleLeverShown(const MapGenConfig *cfg, int lever) {
    const auto &k = cfg->params.maker;
    if (!mapGenStyleRange(k.preset, lever, nullptr, nullptr)) return false;
    if (lever == MAPGEN_KSTYLE_SHAPE) {
        /* Quarter turns make the land square. */
        return k.symmetry != MAPGEN_MSYM_ROTATE4;
    }
    return true;
}

/* True if the lever can change the map with the current outline. Only
 * the octagon has cut corners. The panel still shows Corner cut for
 * other outlines, greyed out, and keeps its value, so that the panel
 * does not change shape when the outline changes. */
static bool styleLeverActive(const MapGenConfig *cfg, int lever) {
    const auto &k = cfg->params.maker;
    if (lever != MAPGEN_KSTYLE_CUT) return true;
    if (k.outline == MAPGEN_OUTLINE_OCTAGON) return true;
    return k.outline == MAPGEN_OUTLINE_VARIED &&
           (mapGenPresetOutlineSet(k.preset) & (1 << MAPGEN_OUTLINE_OCTAGON)) != 0;
}

/* Randomize style: an unlocked outline takes one of the preset's own
 * outlines (named, never Varied), and each unlocked lever that the
 * panel shows takes one of the values the preset allows (never From
 * seed). The seed and every other value stay. */
static void randomizeStyle(MapGenConfig *cfg) {
    uint32_t rng = uiRngSeed();
    auto &k = cfg->params.maker;
    if (!(cfg->locks & MAPGEN_LOCK_K_OUTLINE)) {
        int o = pickPresetOutline(k.preset, &rng);
        if (o >= 0) k.outline = o;
    }
    for (int lever = 0; lever < MAPGEN_KSTYLE_COUNT; lever++) {
        int lo, hi, count = 0, pick;
        if (cfg->locks & (MAPGEN_LOCK_KS_FIRST << lever)) continue;
        if (!styleLeverShown(cfg, lever)) continue;
        if (!mapGenStyleRange(k.preset, lever, &lo, &hi)) continue;
        for (int v = lo; v <= hi; v++) {
            if (mapGenStyleAllowed(k.preset, lever, v)) count++;
        }
        if (count == 0) continue;
        pick = (int)(mapGenXorshift32(&rng) % (uint32_t)count);
        for (int v = lo; v <= hi; v++) {
            if (!mapGenStyleAllowed(k.preset, lever, v)) continue;
            if (pick-- == 0) {
                k.style[lever] = v;
                break;
            }
        }
    }
}

/* A style lever with named values: From seed, then each value the
 * preset allows. names[v - 1] is the name of value v. */
static void styleCombo(MapGenConfig *cfg, int lever, langid labelId,
                       langid tipId, const langid *names) {
    auto &k = cfg->params.maker;
    int lo, hi;
    if (!mapGenStyleRange(k.preset, lever, &lo, &hi)) return;
    int *v = &k.style[lever];
    const char *fromSeed = langGetText(STR_MAPGEN_STYLE_FROM_SEED);
    const char *preview = (*v >= lo && *v <= hi) ? langGetText(names[*v - 1]) : fromSeed;
    if (ImGui::BeginCombo(langGetText(labelId), preview)) {
        if (ImGui::Selectable(fromSeed, *v == 0)) *v = 0;
        if (*v == 0) ImGui::SetItemDefaultFocus();
        for (int i = lo; i <= hi; i++) {
            if (!mapGenStyleAllowed(k.preset, lever, i)) continue;
            bool selected = (*v == i);
            ImGui::PushID(i);
            if (ImGui::Selectable(langGetText(names[i - 1]), selected)) *v = i;
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(tipId));
    }
}

/* A style lever with a range of whole numbers. The stop below the
 * lowest value stands for From seed, the same as the Size slider's
 * Full stop. */
static void styleSlider(MapGenConfig *cfg, int lever, langid labelId, langid tipId) {
    auto &k = cfg->params.maker;
    int lo, hi;
    if (!mapGenStyleRange(k.preset, lever, &lo, &hi)) return;
    int v = (k.style[lever] == 0) ? lo - 1 : k.style[lever];
    char fmt[96];
    if (v < lo) {
        /* The name is shown as the slider's format: double any '%'. */
        const char *t = langGetText(STR_MAPGEN_STYLE_FROM_SEED);
        size_t n = 0;
        for (; *t && n + 2 < sizeof(fmt); t++) {
            if (*t == '%') fmt[n++] = '%';
            fmt[n++] = *t;
        }
        fmt[n] = '\0';
    } else {
        SDL_snprintf(fmt, sizeof(fmt), "%%d");
    }
    if (ImGui::SliderInt(langGetText(labelId), &v, lo - 1, hi, fmt)) {
        k.style[lever] = (v < lo) ? 0 : v;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(tipId));
    }
}

/* --- Layout helpers --- */

/* Slider / combo / input widths default to 50% of the available
 * column width, with the label (rendered to the right of each
 * widget) getting the other 50%. As the column shrinks, the
 * widget compresses ahead of the label: we ensure at least
 * kLabelReserve px is always left for the label text, so once
 * the panel gets narrow the widget starts losing more pixels
 * than the label area. Min widget 40 px so sliders don't
 * collapse to a track-only hairline. The seed InputText has its
 * own explicit SetNextItemWidth(200) and is not affected. */
static void pushControlWidth(void) {
    const float kLabelReserve = 100.0f;
    const float kWidgetMin    = 40.0f;
    float availW = ImGui::GetContentRegionAvail().x;
    float widgetW = availW * 0.5f;
    if (widgetW > availW - kLabelReserve) {
        widgetW = availW - kLabelReserve;
    }
    if (widgetW < kWidgetMin) widgetW = kWidgetMin;
    ImGui::PushItemWidth(widgetW);
}

/* Put the next button on this line if it fits, else on a new line. */
static void sameLineIfFits(const char *label) {
    float btnW = ImGui::CalcTextSize(label).x
               + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < btnW) ImGui::NewLine();
}

static ImVec4 mixColour(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

/* --- Main controls function --- */

bool mapGenImguiControls(MapGenConfig *cfg) {
    bool generated = false;
    static char seedBuf[64] = "";
    static bool seedFieldActive = false;

    /* Only the panel locks have buttons. Clear any other bit so that
     * no old value can hold a control that has no lock button. */
    cfg->locks &= allPanelLocks();

    MapGenConfig prevCfg = *cfg;

    if (!seedFieldActive) {
        mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
    }

    pushControlWidth();

    /* ---- Above the panel: the controls that pick the kind of map,
     * its size and the counts of bases, pillboxes and starts.
     * They have no lock and Randomize does not change them. ---- */

    /* Generator type */
    int prevGenType = cfg->genType;
    const char *s_genTypeNames[MAPGEN_TYPE_COUNT] = {
        langGetText(STR_MAPGEN_TYPE_TOURNAMENT),
        langGetText(STR_MAPGEN_TYPE_NATURAL),
        langGetText(STR_MAPGEN_TYPE_MAZE),
        langGetText(STR_MAPGEN_TYPE_FRACTAL),
        langGetText(STR_MAPGEN_TYPE_MAKER),
    };
    ImGui::Combo(langGetText(STR_MAPGEN_GENERATOR), &cfg->genType, s_genTypeNames, MAPGEN_TYPE_COUNT);
    if (cfg->genType != prevGenType) {
        static MapGenConfig savedConfigs[MAPGEN_TYPE_COUNT];
        static bool hasSaved[MAPGEN_TYPE_COUNT] = {};
        savedConfigs[prevGenType] = *cfg;
        savedConfigs[prevGenType].genType = prevGenType;
        hasSaved[prevGenType] = true;

        uint32_t savedSeed = cfg->seed;
        uint64_t savedLocks = cfg->locks;
        int savedBases = cfg->bases;
        int savedPills = cfg->pills;
        int savedStarts = cfg->starts;
        int savedSize = cfg->size;
        int savedX1 = cfg->x1, savedY1 = cfg->y1;
        int savedX2 = cfg->x2, savedY2 = cfg->y2;

        /* The first switch to Map Maker takes the size and counts of
         * its first preset, the same as picking that preset. Otherwise
         * they carry over. */
        bool firstMaker = cfg->genType == MAPGEN_MAKER && !hasSaved[cfg->genType];
        if (hasSaved[cfg->genType]) {
            *cfg = savedConfigs[cfg->genType];
        } else {
            *cfg = mapGenDefaultConfig(cfg->genType);
        }
        cfg->seed = savedSeed;
        cfg->locks = savedLocks;
        if (!firstMaker) {
            cfg->bases = savedBases;
            cfg->pills = savedPills;
            cfg->starts = savedStarts;
            cfg->size = savedSize;
        } else {
            nameVariedOutline(cfg);
        }
        cfg->x1 = savedX1; cfg->y1 = savedY1;
        cfg->x2 = savedX2; cfg->y2 = savedY2;
    }

    /* The style choices of each type. */
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

    } else if (cfg->genType == MAPGEN_NATURAL) {
        auto &n = cfg->params.natural;
        /* The style sets the values in the panel, the same as a Map
         * Maker preset; locked values are then put back. */
        int prevStyle = n.mapStyle;
        const char *s_mapStyleNames[MAPGEN_STYLE_COUNT] = {
            langGetText(STR_MAPGEN_STYLE_OCEAN),
            langGetText(STR_MAPGEN_STYLE_CONTINENT),
            langGetText(STR_MAPGEN_STYLE_ISLANDS),
            langGetText(STR_MAPGEN_STYLE_ARCHIPELAGO),
            langGetText(STR_MAPGEN_STYLE_INLAND),
        };
        ImGui::Combo(langGetText(STR_MAPGEN_MAP_STYLE), &n.mapStyle, s_mapStyleNames, MAPGEN_STYLE_COUNT);
        if (n.mapStyle != prevStyle) {
            MapGenConfig saved = *cfg;
            mapGenNaturalStyleDefaults(n.mapStyle, cfg);
            restoreLockedNaturalParams(cfg, &saved);
        }

    } else if (cfg->genType == MAPGEN_MAZE) {
        auto &m = cfg->params.maze;
        const char *s_mazeAlgoNames[MAZE_ALGO_COUNT] = {
            langGetText(STR_MAPEDIT_ALGO_LABYRINTH),
            langGetText(STR_MAPEDIT_ALGO_OPEN),
        };
        ImGui::Combo(langGetText(STR_MAPEDIT_ALGORITHM), &m.algo, s_mazeAlgoNames, MAZE_ALGO_COUNT);
        terrainCombo(langGetText(STR_MAPEDIT_WALLTERRAIN), &m.wallTerrain);
        terrainCombo(langGetText(STR_MAPEDIT_CORRIDORTERRAIN), &m.corridorTerrain);

    } else if (cfg->genType == MAPGEN_MAKER) {
        auto &k = cfg->params.maker;

        int presetCount = mapGenPresetCount();
        if (ImGui::BeginCombo(langGetText(STR_MAPGEN_PRESET), presetLabel(k.preset))) {
            for (int i = 0; i < presetCount; i++) {
                bool selected = (i == k.preset);
                if (ImGui::Selectable(presetLabel(i), selected) && !selected) {
                    MapGenConfig saved = *cfg;
                    mapGenApplyPreset(i, cfg);
                    restoreLockedMakerParams(cfg, &saved);
                    nameVariedOutline(cfg);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(STR_MAPGEN_PRESET_TIP));
        }

        const char *s_msymNames[MAPGEN_MSYM_COUNT] = {
            langGetText(STR_MAPGEN_MSYM_NONE),
            langGetText(STR_MAPGEN_MSYM_MIRROR),
            langGetText(STR_MAPGEN_MSYM_ROT2),
            langGetText(STR_MAPGEN_MSYM_MIRROR4),
            langGetText(STR_MAPGEN_MSYM_ROT4),
        };
        ImGui::Combo(langGetText(STR_MAPGEN_SYMMETRY), &k.symmetry, s_msymNames, MAPGEN_MSYM_COUNT);
    }

    /* Size, for every generator type. The slider's highest stop, one
     * above the largest square (MAPGEN_SIZE_MAX + 1), stands for Full
     * (the whole region). */
    {
        int sizeVal = (cfg->size == MAPGEN_SIZE_FULL) ? MAPGEN_SIZE_MAX + 1 : cfg->size;
        char sizeFmt[32];
        if (sizeVal > MAPGEN_SIZE_MAX) {
            SDL_snprintf(sizeFmt, sizeof(sizeFmt), "%s", langGetText(STR_MAPGEN_SIZE_FULL));
        } else {
            SDL_snprintf(sizeFmt, sizeof(sizeFmt), "%%d");
        }
        if (ImGui::SliderInt(langGetText(STR_MAPGEN_SIZE), &sizeVal,
                             MAPGEN_SIZE_MIN, MAPGEN_SIZE_MAX + 1, sizeFmt)) {
            cfg->size = (sizeVal > MAPGEN_SIZE_MAX) ? MAPGEN_SIZE_FULL : sizeVal;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", langGetText(STR_MAPGEN_SIZE_TIP));
        }
    }

    /* Bases, pillboxes and starts, for every generator type. A Map
     * Maker preset sets them; Randomize does not change them. */
    ImGui::SliderInt(langGetText(STR_TABLET_BASES), &cfg->bases, 0, 16);
    ImGui::SliderInt(langGetText(STR_TABLET_PILLBOXES), &cfg->pills, 0, 16);
    ImGui::SliderInt(langGetText(STR_MAPEDIT_STATS_STARTS), &cfg->starts, 0, 16);

    /* Matched with the pushControlWidth above. */
    ImGui::PopItemWidth();

    /* ---- The style panel, for Map Maker only: the choices the seed
     * makes for each map of the preset. Each one is From seed or a
     * value the preset allows. Randomize style, Lock all and Unlock
     * all act only on the controls in it. ---- */
    if (cfg->genType == MAPGEN_MAKER) {
        auto &k = cfg->params.maker;
        /* A lever the panel does not show cannot change the map: keep
         * it at From seed, so that the seed does not hold it. */
        for (int lever = 0; lever < MAPGEN_KSTYLE_COUNT; lever++) {
            if (!styleLeverShown(cfg, lever)) k.style[lever] = 0;
        }

        ImGui::Spacing();
        {
            const ImGuiStyle &st = ImGui::GetStyle();
            ImVec4 bg = mixColour(st.Colors[ImGuiCol_WindowBg], st.Colors[ImGuiCol_Header], 0.15f);
            bg.w = 1.0f;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
            ImGui::PushStyleColor(ImGuiCol_Border, st.Colors[ImGuiCol_Header]);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        }
        ImGui::BeginChild("##mapgen_style", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding |
                              ImGuiChildFlags_NavFlattened,
                          ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        pushControlWidth();
        {
            MessageArgs args = {};
            SDL_strlcpy(args.string1, presetLabel(k.preset), sizeof(args.string1));
            ImGui::TextUnformatted(langGetTextFmt(STR_MAPGEN_STYLE_TITLE, &args));
        }

        /* The outline is always in the panel, so the buttons always
         * show. */
        {
            if (ImGui::Button(langGetText(STR_MAPGEN_STYLE_RANDOMIZE))) {
                randomizeStyle(cfg);
                mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
                generated = true;
            }
            sameLineIfFits(langGetText(STR_MAPGEN_LOCK_ALL));
            if (ImGui::Button(langGetText(STR_MAPGEN_LOCK_ALL))) {
                cfg->locks |= kStyleLocks;
            }
            sameLineIfFits(langGetText(STR_MAPGEN_UNLOCK_ALL));
            if (ImGui::Button(langGetText(STR_MAPGEN_UNLOCK_ALL))) {
                cfg->locks &= ~kStyleLocks;
            }

            ImGui::Separator();

            /* Outline: in the style panel, because Randomize style
             * changes it. The list holds only the named outlines. A
             * seed the player types can still hold Varied (the seed
             * picks one of the preset's outlines); then the preview
             * reads "Varied" until an outline is picked. */
            const char *s_outlineNames[MAPGEN_OUTLINE_VARIED] = {
                langGetText(STR_MAPGEN_OUTLINE_BOX),
                langGetText(STR_MAPGEN_OUTLINE_OCTAGON),
                langGetText(STR_MAPGEN_OUTLINE_ROUND),
                langGetText(STR_MAPGEN_OUTLINE_DIAMOND),
                langGetText(STR_MAPGEN_OUTLINE_CROSS),
                langGetText(STR_MAPGEN_OUTLINE_CLOVER),
                langGetText(STR_MAPGEN_OUTLINE_ISLAND),
            };
            const char *outlinePreview =
                (k.outline >= 0 && k.outline < MAPGEN_OUTLINE_VARIED)
                    ? s_outlineNames[k.outline]
                    : langGetText(STR_MAPGEN_OUTLINE_VARIED);
            if (ImGui::BeginCombo(langGetText(STR_MAPGEN_OUTLINE), outlinePreview)) {
                for (int i = 0; i < MAPGEN_OUTLINE_VARIED; i++) {
                    bool selected = (i == k.outline);
                    if (ImGui::Selectable(s_outlineNames[i], selected)) k.outline = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            lockButton("lk_koutline", &cfg->locks, MAPGEN_LOCK_K_OUTLINE);

            static const langid s_roadNames[MAPGEN_ROADS_COUNT] = {
                STR_MAPGEN_ROADS_AXES, STR_MAPGEN_ROADS_DIAG,
                STR_MAPGEN_ROADS_AXES_DIAG, STR_MAPGEN_ROADS_AXES_RING,
                STR_MAPGEN_ROADS_DIAG_RING, STR_MAPGEN_ROADS_RING,
            };
            static const langid s_coreNames[2] = {
                STR_MAPEDIT_TERR_BUILDING, STR_MAPEDIT_TERR_HALFBUILDING,
            };
            static const langid s_rimNames[2] = {
                STR_MAPGEN_RIM_FOREST, STR_MAPGEN_RIM_GRASS,
            };
            static const langid s_yardNames[3] = {
                STR_MAPGEN_YARD_NONE, STR_MAPGEN_YARD_BUILDING, STR_MAPGEN_YARD_FOREST,
            };
            if (styleLeverShown(cfg, MAPGEN_KSTYLE_ROADS)) {
                styleCombo(cfg, MAPGEN_KSTYLE_ROADS, STR_MAPGEN_STYLE_ROADS,
                           STR_MAPGEN_STYLE_ROADS_TIP, s_roadNames);
                lockButton("lk_sroads", &cfg->locks, MAPGEN_LOCK_KS_ROADS);
            }
            if (styleLeverShown(cfg, MAPGEN_KSTYLE_SHAPE)) {
                styleSlider(cfg, MAPGEN_KSTYLE_SHAPE, STR_MAPGEN_STYLE_SHAPE,
                            STR_MAPGEN_STYLE_SHAPE_TIP);
                lockButton("lk_sshape", &cfg->locks, MAPGEN_LOCK_KS_SHAPE);
            }
            if (styleLeverShown(cfg, MAPGEN_KSTYLE_CUT)) {
                const bool active = styleLeverActive(cfg, MAPGEN_KSTYLE_CUT);
                ImGui::BeginDisabled(!active);
                styleSlider(cfg, MAPGEN_KSTYLE_CUT, STR_MAPGEN_STYLE_CUT,
                            STR_MAPGEN_STYLE_CUT_TIP);
                ImGui::EndDisabled();
                lockButton("lk_scut", &cfg->locks, MAPGEN_LOCK_KS_CUT);
            }
            if (styleLeverShown(cfg, MAPGEN_KSTYLE_CORE)) {
                styleCombo(cfg, MAPGEN_KSTYLE_CORE, STR_MAPGEN_STYLE_CORE,
                           STR_MAPGEN_STYLE_CORE_TIP, s_coreNames);
                lockButton("lk_score", &cfg->locks, MAPGEN_LOCK_KS_CORE);
            }
            if (styleLeverShown(cfg, MAPGEN_KSTYLE_RIM)) {
                styleCombo(cfg, MAPGEN_KSTYLE_RIM, STR_MAPGEN_STYLE_RIM,
                           STR_MAPGEN_STYLE_RIM_TIP, s_rimNames);
                lockButton("lk_srim", &cfg->locks, MAPGEN_LOCK_KS_RIM);
            }
            if (styleLeverShown(cfg, MAPGEN_KSTYLE_YARD)) {
                styleCombo(cfg, MAPGEN_KSTYLE_YARD, STR_MAPGEN_STYLE_YARD,
                           STR_MAPGEN_STYLE_YARD_TIP, s_yardNames);
                lockButton("lk_syard", &cfg->locks, MAPGEN_LOCK_KS_YARD);
            }
        }

        /* Matched with the pushControlWidth inside the style panel. */
        ImGui::PopItemWidth();
        ImGui::EndChild();
    }

    /* ---- The parameter panel: a box with its own border and
     * background. Randomize, Lock all and Unlock all act only on the
     * controls in it. ---- */
    ImGui::Spacing();
    {
        const ImGuiStyle &st = ImGui::GetStyle();
        ImVec4 bg = mixColour(st.Colors[ImGuiCol_WindowBg], st.Colors[ImGuiCol_Header], 0.15f);
        bg.w = 1.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
        ImGui::PushStyleColor(ImGuiCol_Border, st.Colors[ImGuiCol_Header]);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    }
    ImGui::BeginChild("##mapgen_params", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding |
                          ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    /* The colours and padding are read when the child starts; pop them
     * here so the widgets inside keep the normal look. */
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    pushControlWidth();

    /* A title line so it is clear that the box holds what Randomize
     * values changes. */
    ImGui::TextUnformatted(langGetText(STR_MAPGEN_PANEL_TITLE));

    const uint64_t typeLocks = panelLocks(cfg->genType);
    if (ImGui::Button(langGetText(STR_MAPGEN_RANDOMIZE))) {
        randomizePanel(cfg);
        mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
        generated = true;
    }
    sameLineIfFits(langGetText(STR_MAPGEN_LOCK_ALL));
    if (ImGui::Button(langGetText(STR_MAPGEN_LOCK_ALL))) {
        cfg->locks |= typeLocks;
    }
    sameLineIfFits(langGetText(STR_MAPGEN_UNLOCK_ALL));
    if (ImGui::Button(langGetText(STR_MAPGEN_UNLOCK_ALL))) {
        cfg->locks &= ~typeLocks;
    }
    /* Seed: in the panel, because Randomize always changes it.
     * It has no lock. */
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText(langGetText(STR_MAPGEN_SEED), seedBuf, sizeof(seedBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (mapGenSeedToConfig(seedBuf, cfg)) {
            mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
        }
    }
    seedFieldActive = ImGui::IsItemActive();
    /* Copy Seed flows to a new line when the enclosing region is too
     * narrow to fit it — e.g. when the chooser's list/preview splitter
     * is dragged left to give the preview most of the width. */
    sameLineIfFits(langGetText(STR_DLGGAMEINFO_COPYSEED));
    if (ImGui::Button(langGetText(STR_DLGGAMEINFO_COPYSEED))) {
        mapGenConfigToSeed(cfg, seedBuf, sizeof(seedBuf));
        SDL_SetClipboardText(seedBuf);
    }

    ImGui::Separator();

    /* Generator-specific controls */
    if (cfg->genType == MAPGEN_TOURNAMENT) {
        auto &t = cfg->params.tournament;

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
        /* Water barrier: N-tile-thick RIVER (shallow-water) rim
         * around every landmass. 0 = none, 5 = thick coastline. */
        ImGui::SliderInt("Water Barrier", &t.waterBarrier, 0, 5);
        lockButton("lk_waterbarrier", &cfg->locks, MAPGEN_LOCK_T_WATERBARRIER);

    } else if (cfg->genType == MAPGEN_NATURAL) {
        auto &n = cfg->params.natural;

        ImGui::TextUnformatted(langGetText(STR_MAPGEN_TERRAIN_MIX));

        int *terrainPcts[] = { &n.grassPct, &n.forestPct, &n.buildingPct,
                               &n.swampPct, &n.riverPct };
        const uint64_t terrainLocks[] = {
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

    } else if (cfg->genType == MAPGEN_MAKER) {
        auto &k = cfg->params.maker;

        ImGui::SliderInt(langGetText(STR_MAPGEN_WATER_PCT), &k.waterPct, 0, 100);
        lockButton("lk_kwater", &cfg->locks, MAPGEN_LOCK_K_WATER);
        ImGui::SliderInt(langGetText(STR_MAPGEN_FOREST_PCT), &k.forestPct, 0, 100);
        lockButton("lk_kforest", &cfg->locks, MAPGEN_LOCK_K_FOREST);
        ImGui::SliderInt(langGetText(STR_MAPGEN_ROAD_PCT), &k.roadPct, 0, 100);
        lockButton("lk_kroad", &cfg->locks, MAPGEN_LOCK_K_ROAD);
        ImGui::SliderInt(langGetText(STR_MAPGEN_BUILDING_PCT), &k.buildingPct, 0, 100);
        lockButton("lk_kbldg", &cfg->locks, MAPGEN_LOCK_K_BUILDING);
        ImGui::SliderInt(langGetText(STR_MAPGEN_ISLANDS), &k.islands, 1, 8);
        lockButton("lk_kislands", &cfg->locks, MAPGEN_LOCK_K_ISLANDS);
        ImGui::SliderInt(langGetText(STR_MAPGEN_MINE_DENSITY), &k.mineDensityPct, 0, 5);
        lockButton("lk_kmines", &cfg->locks, MAPGEN_LOCK_K_MINES);
        ImGui::SliderInt(langGetText(STR_MAPGEN_DETAIL), &k.detail, 0, 10);
        lockButton("lk_kdetail", &cfg->locks, MAPGEN_LOCK_K_DETAIL);
    }

    /* Matched with the pushControlWidth inside the panel. */
    ImGui::PopItemWidth();
    ImGui::EndChild();

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
