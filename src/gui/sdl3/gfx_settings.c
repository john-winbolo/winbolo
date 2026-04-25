/* gfx_settings — see header. */

#include "gfx_settings.h"

#include <math.h>

static GfxAnimStyle s_animStyle = GFX_ANIM_PIXEL_FLOOR;

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
