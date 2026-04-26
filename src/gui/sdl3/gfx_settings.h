/*
 * gfx_settings — runtime graphics options.
 *
 * Currently exposes:
 *   * Animation style — how sprite positions are quantised when the
 *     authoritative sim coord is sub-pixel (1/256 of a tile).
 *
 * Wired into the renderer wherever a world-coord → screen-coord
 * conversion happens for a sprite that has sub-pixel info.  The
 * default ("PixelFloor") matches the historical >>4 floor used
 * everywhere before this option existed.
 */

#ifndef GFX_SETTINGS_H
#define GFX_SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Sprite x = floor(world_unit / 16). Classic Bolo behaviour. */
    GFX_ANIM_PIXEL_FLOOR   = 0,
    /* Sprite x = round(world_unit / 16). Snaps to nearest game pixel. */
    GFX_ANIM_PIXEL_NEAREST = 1,
    /* Sprite x = world_unit / 16.0 (full sub-pixel). May blur for
     * pixel-art textures unless the theme is SVG-based. */
    GFX_ANIM_SMOOTH        = 2,
} GfxAnimStyle;

void          gfxSettingsSetAnimStyle(GfxAnimStyle s);
GfxAnimStyle  gfxSettingsGetAnimStyle(void);

/* Convert a world-unit (1/256 tile) coord to a game-pixel coord
 * using the active animation style. */
float gfxSettingsWuToGamePixel(int wu);

/* Tile Detail Level — how the tile atlas is built from the active
 * theme's assets.  Independent of motion (see GfxAnimSmoothness).
 *   Classic:     density 1 only.  Pixelation matches standard
 *                bolo's size.
 *   MatchToZoom: at the active gZoomFactor, use the highest density
 *                <= gZoomFactor for which the theme provides ALL
 *                tiles; fall back to a lower density that's full.
 *   HighDetail:  per sprite, use the highest density the theme
 *                actually provides for that sprite — partial
 *                coverage is fine. */
typedef enum {
    GFX_TILE_DETAIL_CLASSIC       = 0,
    GFX_TILE_DETAIL_MATCH_TO_ZOOM = 1,
    GFX_TILE_DETAIL_HIGH_DETAIL   = 2,
} GfxTileDetail;

/* Animation Smoothness — motion granularity for tanks / shells /
 * LGMs (builders).  Independent of tile detail.
 *   Classic:           game-pixel grid (16 wu/step).  Bolo's old feel.
 *   MatchToPixelation: sprites step at the same pixel size that the
 *                      tile detail produces (per sprite).
 *   Max:               finest motion the screen can show
 *                      (16 / gZoomFactor wu/step). */
typedef enum {
    GFX_ANIM_SMOOTH_CLASSIC             = 0,
    GFX_ANIM_SMOOTH_MATCH_TO_PIXELATION = 1,
    GFX_ANIM_SMOOTH_MAX                 = 2,
} GfxAnimSmoothness;

void              gfxSettingsSetTileDetail(GfxTileDetail d);
GfxTileDetail     gfxSettingsGetTileDetail(void);
void              gfxSettingsSetAnimSmoothness(GfxAnimSmoothness s);
GfxAnimSmoothness gfxSettingsGetAnimSmoothness(void);

/* "Force smooth path shells" override.  When ON shells move at MAX
 * smoothness regardless of AnimSmoothness (unless AnimSmoothness is
 * already MAX, in which case the toggle is hidden in the UI). */
void gfxSettingsSetForceSmoothShells(bool on);
bool gfxSettingsGetForceSmoothShells(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_SETTINGS_H */
