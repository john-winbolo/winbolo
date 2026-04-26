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

/* Theme detail level — how much vector / hand-crafted PNG quality
 * the renderer pulls from a theme.
 *   PixelateNormal: SVG point-sampled at 1× regardless of zoom.
 *                   Atlas NEAREST.  Pure pixel-art look.
 *   PixelateToZoom: prefer hand-crafted N-<name>.png (e.g. 32-, 48-,
 *                   64-) at the active atlas zoom; fall back to
 *                   smaller prefixes, then to the SVG point-sampled
 *                   at 1×.  Atlas NEAREST.
 *   MaxDetail:      rasterize SVG at the atlas zoom with anti-
 *                   aliasing.  Atlas LINEAR sampling for smooth
 *                   rotation. */
typedef enum {
    GFX_THEME_DETAIL_PIXELATE_NORMAL    = 0,
    GFX_THEME_DETAIL_PIXELATE_ZOOM      = 1,
    GFX_THEME_DETAIL_MAX_DETAIL         = 2,
    /* Same atlas + sampling treatment as MAX_DETAIL but also
     * forces smooth-path shells regardless of the user's persisted
     * AllowSmoothShells preference. */
    GFX_THEME_DETAIL_MAX_DETAIL_SMOOTH  = 3,
} GfxThemeDetail;

void           gfxSettingsSetThemeDetail(GfxThemeDetail d);
GfxThemeDetail gfxSettingsGetThemeDetail(void);

/* True when the active ThemeDetail uses the Max Detail rendering
 * path (atlas LINEAR/PIXELART, AA SVG bake, runtime _00 rotation
 * etc.).  Both MAX_DETAIL and MAX_DETAIL_SMOOTH return true. */
bool gfxSettingsThemeDetailIsMax(void);

/* Cosmetic: when true, shells render at full sub-wu (1/256-tile)
 * precision instead of snapping to the game-pixel grid.  Makes flight
 * paths look smoother / more direct but breaks pixel-art alignment
 * for the shell sprite.  Default off. */
void gfxSettingsSetAllowSmoothShells(bool allow);
bool gfxSettingsGetAllowSmoothShells(void);

/* Effective smooth-shells flag for the renderer: returns true when
 * AllowSmoothShells is set OR ThemeDetail is MAX_DETAIL_SMOOTH.
 * The persisted AllowSmoothShells value is preserved either way. */
bool gfxSettingsEffectiveSmoothShells(void);

/* True when the active ThemeDetail wants ALL animated sprites
 * (tanks, shells, LGMs) to render at full sub-wu (1/256-tile)
 * precision instead of snapping to the game-pixel grid.  Currently
 * only MAX_DETAIL_SMOOTH enables this. */
bool gfxSettingsAllSmoothMotion(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_SETTINGS_H */
