/* gfx_settings — see header. */

#include "gfx_settings.h"

#include <math.h>

/* Animation style locked to Pixel Nearest internally — kept as
 * state for any code still calling gfxSettingsWuToGamePixel(). */
static GfxAnimStyle       s_animStyle          = GFX_ANIM_PIXEL_NEAREST;
static GfxTileDetail      s_tileDetail         = GFX_TILE_DETAIL_CLASSIC;
static GfxAnimSmoothness  s_animSmoothness     = GFX_ANIM_SMOOTH_CLASSIC;
static bool               s_forceSmoothShells  = false;

void          gfxSettingsSetTileDetail(GfxTileDetail d) { s_tileDetail = d; }
GfxTileDetail gfxSettingsGetTileDetail(void)            { return s_tileDetail; }

void              gfxSettingsSetAnimSmoothness(GfxAnimSmoothness s) { s_animSmoothness = s; }
GfxAnimSmoothness gfxSettingsGetAnimSmoothness(void)                { return s_animSmoothness; }

void gfxSettingsSetForceSmoothShells(bool on) { s_forceSmoothShells = on; }
bool gfxSettingsGetForceSmoothShells(void)    { return s_forceSmoothShells; }

void gfxSettingsSetAnimStyle(GfxAnimStyle s) {
    s_animStyle = s;
}

GfxAnimStyle gfxSettingsGetAnimStyle(void) {
    return s_animStyle;
}

float gfxSettingsWuToGamePixel(int wu) {
    switch (s_animStyle) {
        case GFX_ANIM_PIXEL_NEAREST:
            return (float)((wu + 8) >> 4);
        case GFX_ANIM_SMOOTH:
            return (float)wu / 16.0f;
        case GFX_ANIM_PIXEL_FLOOR:
        default:
            return (float)(wu >> 4);
    }
}
