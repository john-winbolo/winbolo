/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Ping Pie
 *Filename:      ping_pie.h
 *Purpose:
 *  The smart-ping pie menu's geometry, on its own so it can
 *  be tested without a window: which slice a cursor offset
 *  lands in, and where each slice's icon goes.
 *
 *  The pie is five equal slices around a dead zone. Caution
 *  sits at the top and the rest run clockwise from it, the
 *  order Andrew asked for and the order the icons are drawn
 *  in. The dead zone is the standard ping: releasing there —
 *  or tapping without moving — sends the plain marker, which
 *  is why the centre is not a slice.
 *
 *  Screen axes: x right, y DOWN, the way both SDL and ImGui
 *  hand them over. Angles here are measured clockwise from
 *  straight up, so slice 0 is centred on 0 and the boundary
 *  between the last slice and the first runs through the top
 *  — the wrap the selection test has to get right.
 *********************************************************/

#ifndef WINBOLO_PING_PIE_H
#define WINBOLO_PING_PIE_H

#include <math.h>
#include <stdbool.h>

#include "../ping_kinds.h"   /* PING_KIND_* */

/* MSVC's <math.h> only defines M_PI under _USE_MATH_DEFINES, which a
 * translation unit including this header may not have set. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The five slices, clockwise from the top. The standard ping is not here: it
 * is the centre. */
#define PING_PIE_SLICES 5

static const unsigned char kPingPieSlices[PING_PIE_SLICES] = {
    PING_KIND_CAUTION,
    PING_KIND_ASSIST,
    PING_KIND_ATTACK,
    PING_KIND_ON_MY_WAY,
    PING_KIND_BOT_COMMAND
};

/* Layout in unscaled pixels — the caller multiplies by the UI scale. The dead
 * zone is generous enough that a press-and-release with a shaky hand still
 * reads as the standard ping, and the ring is wide enough that each icon has
 * room without the menu covering the whole view. */
#define PING_PIE_DEADZONE_PX  26.0f
#define PING_PIE_RADIUS_PX    92.0f
#define PING_PIE_ICON_PX      30.0f

/* Where a slice's icon sits: partway out along the slice's centre line. */
#define PING_PIE_ICON_RING    0.68f

/* The angle, clockwise from up in radians, of slice `i`'s centre line. */
static inline float pingPieSliceAngle(int i) {
    return (float)((2.0 * M_PI / PING_PIE_SLICES) * (double)i);
}

/* The offset of slice `i`'s icon centre from the pie centre, in the same
 * pixels as PING_PIE_RADIUS_PX. */
static inline void pingPieIconOffset(int i, float radius,
                                     float *outDx, float *outDy) {
    float a = pingPieSliceAngle(i);
    float r = radius * PING_PIE_ICON_RING;
    if (outDx) *outDx = r * sinf(a);
    if (outDy) *outDy = -r * cosf(a);   /* y grows downward */
}

/* Which slice a cursor offset from the pie centre lands in, or -1 for the
 * dead zone. dx/dy are in screen pixels (y down). */
static inline int pingPieSliceAt(float dx, float dy, float deadZone) {
    float ang;
    float span;
    int slice;
    if (dx * dx + dy * dy <= deadZone * deadZone) return -1;
    /* atan2(dx, -dy) is the angle clockwise from straight up, in (-pi, pi]. */
    ang = atan2f(dx, -dy);
    span = (float)(2.0 * M_PI / PING_PIE_SLICES);
    /* Rotate by half a slice before flooring so slice 0 straddles the top
     * rather than starting there, then fold the negative half of atan2's
     * range back around. */
    ang += span * 0.5f;
    if (ang < 0.0f) ang += (float)(2.0 * M_PI);
    slice = (int)(ang / span);
    if (slice < 0) slice = 0;
    if (slice >= PING_PIE_SLICES) slice = PING_PIE_SLICES - 1;
    return slice;
}

/* The ping kind a cursor offset selects: the slice under it, or the standard
 * ping when the cursor is in (or back in) the dead zone. This is the whole
 * selection rule — a release runs it once and sends whatever it returns. */
static inline unsigned char pingPieKindAt(float dx, float dy, float deadZone) {
    int slice = pingPieSliceAt(dx, dy, deadZone);
    if (slice < 0) return PING_KIND_STANDARD;
    return kPingPieSlices[slice];
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_PIE_H */
