/*
 * The shared shell list builder (test_screen_bullets_build.c).
 *
 * clientSimBuildShellList turns other players' shells (the forward-projected
 * layer, or for a bot the raw server snapshots) and the local player's own
 * predictions into screenBullets entries. The classic view asks for its 17x17
 * viewport with the scroll offset subtracted from each square; the map
 * overview asks for the whole map with nothing subtracted. Each list is pinned
 * entry for entry — square, sub-pixel, frame and world bytes — against the
 * arithmetic the two views used to carry separately: a shell outside the
 * viewport is absent from the classic list and present in the whole-map one,
 * an expired prediction is in neither, a projected shell of the local
 * player's is skipped, and a bot's list reads the server snapshots instead of
 * the projected layer.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h" /* the shell arrays and the bot flag, written directly */
#include "screenbullet.h"
#include "shells.h"              /* SHELL_DEATH */
#include "util.h"                /* utilGetDir — the direction a shell frame is picked by */
#include "viewport_types.h"      /* MAIN_BACK_BUFFER_SIZE_X / Y */
#include "test_harness.h"

/* The local slot, and where the classic viewport is parked. */
#define ME      2
#define VIEW_X  100
#define VIEW_Y  60

/* The far edges the classic caller passes: exclusive, one short of the back
 * buffer, and cast to a BYTE the way clientRenderFrame does it. */
#define VIEW_RIGHT  ((BYTE)(VIEW_X + MAIN_BACK_BUFFER_SIZE_X - 1))
#define VIEW_BOTTOM ((BYTE)(VIEW_Y + MAIN_BACK_BUFFER_SIZE_Y - 1))

/* A world co-ordinate: the map square in the high byte, the offset inside
 * the square in the low one. */
#define WORLD_AT(square, sub) ((WORLD)(((square) << TANK_SHIFT_MAPSIZE) | (sub)))

/* One shell as the test places it. angle is the wire's 0-255 byte for the
 * projected layer and the snapshots; the predicted array keeps a TURNTYPE. */
typedef struct Shot {
    WORLD   x;
    WORLD   y;
    uint8_t angle;
    BYTE    owner;
} Shot;

typedef struct Prediction {
    WORLD    x;
    WORLD    y;
    TURNTYPE angle;
    uint8_t  length;
} Prediction;

/* Other players' shells, as the projected layer holds them. [1] is the local
 * player's and is never listed. [2] sits on the column just past the
 * viewport's exclusive right edge, [3] is on the far side of the map. */
static const Shot PROJECTED[] = {
    { WORLD_AT(VIEW_X + 2,  0x7A), WORLD_AT(VIEW_Y + 3,  0x15),  40, 3  },
    { WORLD_AT(VIEW_X + 5,  0x80), WORLD_AT(VIEW_Y + 5,  0x80),  12, ME },
    { WORLD_AT(VIEW_X + 16, 0x01), WORLD_AT(VIEW_Y + 1,  0xF0), 200, 5  },
    { WORLD_AT(5,           0x33), WORLD_AT(200,         0xCC), 250, 7  },
    { WORLD_AT(VIEW_X,      0x00), WORLD_AT(VIEW_Y + 15, 0xFF),   0, 9  },
};

/* The local player's own shells. [1] has run out, [2] is one row above the
 * viewport. */
static const Prediction PREDICTED[] = {
    { WORLD_AT(VIEW_X + 8, 0x48), WORLD_AT(VIEW_Y + 8, 0xB0), 100.0f, 20          },
    { WORLD_AT(VIEW_X + 9, 0x10), WORLD_AT(VIEW_Y + 9, 0x10),  64.0f, SHELL_DEATH },
    { WORLD_AT(VIEW_X + 3, 0x20), WORLD_AT(VIEW_Y - 1, 0x99), 180.0f, 5           },
};

/* What a bot's ClientSim holds instead of the projected layer. [1] is the
 * local player's, [2] is outside the viewport. */
static const Shot SNAPSHOTS[] = {
    { WORLD_AT(VIEW_X + 11, 0x3C), WORLD_AT(VIEW_Y + 2,  0xD2),  77, 4  },
    { WORLD_AT(VIEW_X + 6,  0x00), WORLD_AT(VIEW_Y + 6,  0x00),  30, ME },
    { WORLD_AT(VIEW_X + 40, 0x00), WORLD_AT(VIEW_Y + 40, 0x00), 128, 6  },
};

typedef struct Expected {
    BYTE mx, my, px, py, frame, wx, wy;
} Expected;

/* What a shell at (x, y) heading angle becomes on a list whose squares are
 * relative to (originX, originY). Spelled out from the meaning of the bytes
 * rather than copied from the builder: the square is the high byte of the
 * world co-ordinate, the world byte is the low one, and the pixel offset is
 * that byte's top four bits. */
static Expected expected_entry(WORLD x, WORLD y, TURNTYPE angle,
                               int originX, int originY) {
    Expected e;
    e.mx    = (BYTE)((x >> TANK_SHIFT_MAPSIZE) - originX);
    e.my    = (BYTE)((y >> TANK_SHIFT_MAPSIZE) - originY);
    e.wx    = (BYTE)x;
    e.wy    = (BYTE)y;
    e.px    = (BYTE)(e.wx >> 4);
    e.py    = (BYTE)(e.wy >> 4);
    e.frame = (BYTE)(utilGetDir(angle) + SHELL_START_EXPLODE + 1);
    return e;
}

static Expected expected_shot(const Shot *s, int originX, int originY) {
    return expected_entry(s->x, s->y, (TURNTYPE)s->angle, originX, originY);
}

static Expected expected_prediction(const Prediction *p, int originX, int originY) {
    return expected_entry(p->x, p->y, p->angle, originX, originY);
}

/* Compares the list against the entries in the order the builder added them.
 * screenBulletsAddItem pushes onto the front, so the first item read back is
 * the last one added. */
static int list_matches(const screenBullets *sb, const Expected *exp, int n,
                        const char *what) {
    int count = screenBulletsGetNumEntries(sb);
    int i;

    UT_ASSERT_MSG(count == n, "%s: %d entries, expected %d", what, count, n);
    for (i = 0; i < n; i++) {
        const Expected *e = &exp[n - 1 - i];
        BYTE mx = 0, my = 0, px = 0, py = 0, frame = 0, wx = 0, wy = 0;

        screenBulletsGetItem(sb, i + 1, &mx, &my, &px, &py, &frame);
        screenBulletsGetSubPixel(sb, i + 1, &wx, &wy);
        UT_ASSERT_MSG(mx == e->mx && my == e->my,
                      "%s: item %d square (%u,%u), expected (%u,%u)",
                      what, i + 1, mx, my, e->mx, e->my);
        UT_ASSERT_MSG(px == e->px && py == e->py,
                      "%s: item %d pixel (%u,%u), expected (%u,%u)",
                      what, i + 1, px, py, e->px, e->py);
        UT_ASSERT_MSG(frame == e->frame,
                      "%s: item %d frame %u, expected %u",
                      what, i + 1, frame, e->frame);
        UT_ASSERT_MSG(wx == e->wx && wy == e->wy,
                      "%s: item %d world (%u,%u), expected (%u,%u)",
                      what, i + 1, wx, wy, e->wx, e->wy);
    }
    return 0;
}

/* TRUE when some entry sits on the given (already relative) square. */
static bool list_has_square(const screenBullets *sb, BYTE mx, BYTE my) {
    int count = screenBulletsGetNumEntries(sb);
    int i;

    for (i = 1; i <= count; i++) {
        BYTE emx = 0, emy = 0, px = 0, py = 0, frame = 0;
        screenBulletsGetItem(sb, i, &emx, &emy, &px, &py, &frame);
        if (emx == mx && emy == my) {
            return true;
        }
    }
    return false;
}

static void add_projected(ClientSim *cs, const Shot *s) {
    ProjectedShell *ps = &cs->projectedShells[cs->projectedShellCount++];
    memset(ps, 0, sizeof(*ps));
    ps->fx     = (float)s->x;
    ps->fy     = (float)s->y;
    ps->angle  = s->angle;
    ps->owner  = s->owner;
    ps->length = 10;
    ps->active = true;
}

static void add_predicted(ClientSim *cs, const Prediction *p) {
    PredictedShell *ps = &cs->predictedShells[cs->predictedShellCount++];
    memset(ps, 0, sizeof(*ps));
    ps->x      = p->x;
    ps->y      = p->y;
    ps->fx     = (float)p->x;
    ps->fy     = (float)p->y;
    ps->angle  = p->angle;
    ps->length = p->length;
    ps->owner  = ME;
    ps->active = true;
}

static void add_snapshot(ClientSim *cs, const Shot *s) {
    ShellSnapshot *ss = &cs->serverShellSnaps[cs->serverShellCount++];
    memset(ss, 0, sizeof(*ss));
    ss->worldX = s->x;
    ss->worldY = s->y;
    ss->angle  = s->angle;
    ss->owner  = s->owner;
    ss->length = 10;
}

/* Builds the list the classic view asks for: its viewport, squares relative
 * to the scroll offset. */
static void build_classic(ClientSim *cs, screenBullets *sb) {
    *sb = screenBulletsCreate();
    clientSimBuildShellList(cs, sb, VIEW_X, VIEW_RIGHT, VIEW_Y, VIEW_BOTTOM,
                            VIEW_X, VIEW_Y);
}

/* Builds the list the overview asks for: the whole map, absolute squares. */
static void build_whole_map(ClientSim *cs, screenBullets *sb) {
    *sb = screenBulletsCreate();
    clientSimBuildShellList(cs, sb, 0, MAP_ARRAY_SIZE, 0, MAP_ARRAY_SIZE, 0, 0);
}

int run_screen_bullets_build(void) {
    ClientSim *cs = clientSimAlloc();
    screenBullets sb;
    Expected exp[8];
    int n;
    size_t i;

    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);
    /* The builder tells the local player's shells apart by the interpolation
     * context's slot, the same field the two views read. */
    cs->interpCtx.localPlayer = ME;
    for (i = 0; i < sizeof(PROJECTED) / sizeof(PROJECTED[0]); i++) {
        add_projected(cs, &PROJECTED[i]);
    }
    for (i = 0; i < sizeof(PREDICTED) / sizeof(PREDICTED[0]); i++) {
        add_predicted(cs, &PREDICTED[i]);
    }

    /* The classic list: the viewport with the scroll origin taken off. The
     * two projected shells inside it, then the one live prediction inside it,
     * and nothing else. */
    build_classic(cs, &sb);
    n = 0;
    exp[n++] = expected_shot(&PROJECTED[0], VIEW_X, VIEW_Y);
    exp[n++] = expected_shot(&PROJECTED[4], VIEW_X, VIEW_Y);
    exp[n++] = expected_prediction(&PREDICTED[0], VIEW_X, VIEW_Y);
    UT_ASSERT(list_matches(&sb, exp, n, "classic") == 0);
    /* The shell on the column past the right edge is not on it, and neither
     * is the expired prediction. */
    UT_ASSERT(!list_has_square(&sb, 16, 1));
    UT_ASSERT(!list_has_square(&sb, 9, 9));
    screenBulletsDestroy(&sb);

    /* The whole map with nothing subtracted: every shell but the local
     * player's projected one and the expired prediction, on its absolute
     * square, in array order. */
    build_whole_map(cs, &sb);
    n = 0;
    exp[n++] = expected_shot(&PROJECTED[0], 0, 0);
    exp[n++] = expected_shot(&PROJECTED[2], 0, 0);
    exp[n++] = expected_shot(&PROJECTED[3], 0, 0);
    exp[n++] = expected_shot(&PROJECTED[4], 0, 0);
    exp[n++] = expected_prediction(&PREDICTED[0], 0, 0);
    exp[n++] = expected_prediction(&PREDICTED[2], 0, 0);
    UT_ASSERT(list_matches(&sb, exp, n, "whole map") == 0);
    UT_ASSERT(list_has_square(&sb, VIEW_X + 16, VIEW_Y + 1));
    UT_ASSERT(list_has_square(&sb, 5, 200));
    UT_ASSERT(list_has_square(&sb, VIEW_X + 3, VIEW_Y - 1));
    UT_ASSERT(!list_has_square(&sb, VIEW_X + 9, VIEW_Y + 9));
    screenBulletsDestroy(&sb);

    /* A bot's ClientSim: the server snapshots stand in for the projected
     * layer, which is left untouched and unread. The predictions are listed
     * as before. */
    cs->isBot = true;
    for (i = 0; i < sizeof(SNAPSHOTS) / sizeof(SNAPSHOTS[0]); i++) {
        add_snapshot(cs, &SNAPSHOTS[i]);
    }

    build_classic(cs, &sb);
    n = 0;
    exp[n++] = expected_shot(&SNAPSHOTS[0], VIEW_X, VIEW_Y);
    exp[n++] = expected_prediction(&PREDICTED[0], VIEW_X, VIEW_Y);
    UT_ASSERT(list_matches(&sb, exp, n, "bot classic") == 0);
    screenBulletsDestroy(&sb);

    build_whole_map(cs, &sb);
    n = 0;
    exp[n++] = expected_shot(&SNAPSHOTS[0], 0, 0);
    exp[n++] = expected_shot(&SNAPSHOTS[2], 0, 0);
    exp[n++] = expected_prediction(&PREDICTED[0], 0, 0);
    exp[n++] = expected_prediction(&PREDICTED[2], 0, 0);
    UT_ASSERT(list_matches(&sb, exp, n, "bot whole map") == 0);
    screenBulletsDestroy(&sb);

    clientSimDestroy(cs);
    return 0;
}
