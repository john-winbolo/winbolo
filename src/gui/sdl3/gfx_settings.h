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

#ifdef __cplusplus
}
#endif

#endif /* GFX_SETTINGS_H */
