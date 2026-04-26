/* gfx_settings — see header. */

#include "gfx_settings.h"

#include <math.h>

/* Animation style is now locked to Pixel Nearest — kept as state for
 * any code still calling gfxSettingsWuToGamePixel(). */
static GfxAnimStyle    s_animStyle        = GFX_ANIM_PIXEL_NEAREST;
static GfxThemeDetail  s_themeDetail      = GFX_THEME_DETAIL_PIXELATE_NORMAL;
static bool            s_allowSmoothShells = false;

void           gfxSettingsSetThemeDetail(GfxThemeDetail d) { s_themeDetail = d; }
GfxThemeDetail gfxSettingsGetThemeDetail(void)             { return s_themeDetail; }

void gfxSettingsSetAllowSmoothShells(bool allow) { s_allowSmoothShells = allow; }
bool gfxSettingsGetAllowSmoothShells(void)       { return s_allowSmoothShells; }

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
