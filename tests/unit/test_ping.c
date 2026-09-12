/*
 * Smart-ping geometry, bindings and wire shape (test_ping.c).
 *
 * Three headers with no state, no SDL window and no game behind them, and
 * the shape of the event they end up producing:
 *
 *   ping_binding.h - packing a chord (modifiers plus a key OR a mouse
 *                    button) into the plain int keyItems and the prefs file
 *                    both store, and matching it against what the player
 *                    just pressed. The exact-modifier rule is the part worth
 *                    pinning: Ctrl+Right Mouse must not fire on a bare right
 *                    click, and a bare binding must not fire while a
 *                    modifier is held for something else.
 *
 *   ping_pie.h     - which slice a cursor offset lands in. The wrap at the
 *                    top is the interesting case: Caution is centred on
 *                    straight up, so its slice straddles the discontinuity
 *                    in atan2's range.
 *
 *   ping_edge.h    - where the line from the viewer's tank to an off-screen
 *                    ping leaves the game rectangle, and the bar laid along
 *                    that border. All four sides, a corner, and the
 *                    on-screen case that must produce no marker at all.
 *                    Then how big that bar is for a ping that far away, and
 *                    where the sender's name goes beside it: the placement
 *                    has to keep the name inside the view on every border
 *                    and in every corner, because a name half off the screen
 *                    is the one thing the indicator exists to say.
 *
 *   input_packet.h - EVENT_PING's data size and reliability, which the
 *                    server's writer and the client's reader both read out
 *                    of the same two switches.
 *
 *   ping_kinds.h   - the per-kind table, the fade curve, and shortening a
 *                    sender's name to what a marker draws: the cut counts
 *                    UTF-8 characters rather than bytes, so a Cyrillic or
 *                    emoji name loses whole characters and never half of
 *                    one.
 *
 *   ping_sounds.h  - which sound a received ping plays, and the fallback to
 *                    ping_default for a kind that has no file of its own.
 *                    The rule is a pure function of the kind and a bitmap of
 *                    what the sound backend found, so it can be checked here
 *                    with no audio device and no files on disk.
 */

#include <math.h>
#include <string.h>

#include "input_packet.h"
#include "../../src/gui/ping_kinds.h"
#include "../../src/gui/ping_sounds.h"
#include "../../src/gui/sdl3/ping_binding.h"
#include "../../src/gui/sdl3/ping_pie.h"
#include "../../src/gui/sdl3/ping_edge.h"
#include "test_harness.h"

/* SDL's right mouse button. Spelled out rather than included so this file
 * needs no SDL header. */
#define PT_MOUSE_RIGHT 3
#define PT_MOUSE_LEFT  1

/* An arbitrary scancode (SDL_SCANCODE_V) — any value below the mouse base
 * would do; a real one keeps the intent readable. */
#define PT_SCANCODE_V  25

int run_ping_binding_encode_decode(void) {
    int b;

    /* A mouse chord: modifiers in the high bits, the mouse code in the low
     * ones, and both halves recoverable. */
    b = pingBindingEncode(PING_BIND_MOD_CTRL, pingBindingMouseCode(PT_MOUSE_RIGHT));
    UT_ASSERT(pingBindingIsSet(b));
    UT_ASSERT(pingBindingIsMouse(b));
    UT_ASSERT_MSG(pingBindingMouseButton(b) == PT_MOUSE_RIGHT,
                  "button = %d", pingBindingMouseButton(b));
    UT_ASSERT_MSG(pingBindingScancode(b) == 0,
                  "a mouse binding names no scancode, got %d",
                  pingBindingScancode(b));
    UT_ASSERT(pingBindingMods(b) == PING_BIND_MOD_CTRL);

    /* A key chord with two modifiers. */
    b = pingBindingEncode(PING_BIND_MOD_ALT | PING_BIND_MOD_SHIFT, PT_SCANCODE_V);
    UT_ASSERT(pingBindingIsSet(b));
    UT_ASSERT(!pingBindingIsMouse(b));
    UT_ASSERT_MSG(pingBindingScancode(b) == PT_SCANCODE_V,
                  "scancode = %d", pingBindingScancode(b));
    UT_ASSERT(pingBindingMouseButton(b) == 0);
    UT_ASSERT(pingBindingMods(b) == (PING_BIND_MOD_ALT | PING_BIND_MOD_SHIFT));

    /* No modifiers is a valid chord, not an unbound slot. */
    b = pingBindingEncode(0, PT_SCANCODE_V);
    UT_ASSERT(pingBindingIsSet(b));
    UT_ASSERT(pingBindingMods(b) == 0);

    /* Code 0 is unbound however many modifiers ride with it — a slot that
     * fires on nothing must not read as bound. */
    UT_ASSERT(pingBindingEncode(PING_BIND_MOD_CTRL, 0) == PING_BIND_NONE);
    UT_ASSERT(!pingBindingIsSet(PING_BIND_NONE));
    UT_ASSERT(pingBindingMouseButton(PING_BIND_NONE) == 0);
    UT_ASSERT(pingBindingScancode(PING_BIND_NONE) == 0);

    /* The mouse space starts one past the largest scancode SDL defines, so
     * the two can never be confused. */
    UT_ASSERT_MSG(PING_BIND_MOUSE_BASE == 512,
                  "mouse base moved to %d — saved bindings would change "
                  "meaning", PING_BIND_MOUSE_BASE);
    UT_ASSERT(pingBindingMouseCode(PT_MOUSE_LEFT) > PING_BIND_MOUSE_BASE);

    /* The shipped defaults are Ctrl and Alt with the right mouse button. */
    UT_ASSERT(pingBindingIsMouse(pingBindingEncode(PING_BIND_MOD_CTRL,
                  pingBindingMouseCode(PT_MOUSE_RIGHT))));
    return 0;
}

int run_ping_binding_match(void) {
    int ctrlRmb = pingBindingEncode(PING_BIND_MOD_CTRL,
                                    pingBindingMouseCode(PT_MOUSE_RIGHT));
    int altRmb  = pingBindingEncode(PING_BIND_MOD_ALT,
                                    pingBindingMouseCode(PT_MOUSE_RIGHT));
    int bareV   = pingBindingEncode(0, PT_SCANCODE_V);
    int rmbCode = pingBindingMouseCode(PT_MOUSE_RIGHT);
    int slots[PING_BIND_SLOTS];

    /* Exactly its own chord fires it. */
    UT_ASSERT(pingBindingMatches(ctrlRmb, rmbCode, PING_BIND_MOD_CTRL));

    /* The modifier test is exact in both directions. */
    UT_ASSERT_MSG(!pingBindingMatches(ctrlRmb, rmbCode, 0),
                  "Ctrl+RMB must not fire on a bare right click");
    UT_ASSERT_MSG(!pingBindingMatches(ctrlRmb, rmbCode,
                                      PING_BIND_MOD_CTRL | PING_BIND_MOD_SHIFT),
                  "Ctrl+RMB must not fire on Ctrl+Shift+RMB");
    UT_ASSERT_MSG(!pingBindingMatches(bareV, PT_SCANCODE_V, PING_BIND_MOD_CTRL),
                  "a bare binding must not fire while Ctrl is held");

    /* A different code never fires it, whatever the modifiers. */
    UT_ASSERT(!pingBindingMatches(ctrlRmb, pingBindingMouseCode(PT_MOUSE_LEFT),
                                  PING_BIND_MOD_CTRL));
    UT_ASSERT(!pingBindingMatches(ctrlRmb, PT_SCANCODE_V, PING_BIND_MOD_CTRL));

    /* An unbound slot matches nothing at all, including code 0. */
    UT_ASSERT(!pingBindingMatches(PING_BIND_NONE, 0, 0));
    UT_ASSERT(!pingBindingMatches(PING_BIND_NONE, rmbCode, 0));

    /* The whole menu-slot array: the first slot that fires wins, and an
     * array of unbound slots reports nothing. */
    slots[0] = ctrlRmb;
    slots[1] = altRmb;
    slots[2] = PING_BIND_NONE;
    UT_ASSERT(pingBindingMatchAny(slots, PING_BIND_SLOTS, rmbCode,
                                  PING_BIND_MOD_CTRL) == 0);
    UT_ASSERT(pingBindingMatchAny(slots, PING_BIND_SLOTS, rmbCode,
                                  PING_BIND_MOD_ALT) == 1);
    UT_ASSERT(pingBindingMatchAny(slots, PING_BIND_SLOTS, rmbCode,
                                  PING_BIND_MOD_SHIFT) < 0);
    memset(slots, 0, sizeof(slots));
    UT_ASSERT(pingBindingMatchAny(slots, PING_BIND_SLOTS, rmbCode, 0) < 0);
    return 0;
}

int run_ping_binding_direct(void) {
    int direct[PING_BIND_DIRECT_SLOTS];
    int menu[PING_BIND_SLOTS];
    int rmbCode = pingBindingMouseCode(PT_MOUSE_RIGHT);
    int ctrlRmb = pingBindingEncode(PING_BIND_MOD_CTRL, rmbCode);
    int i;

    /* The direct array is indexed by the kind itself, which is the only
     * reason a lookup can return the kind rather than a slot number. A kind
     * added to the wire without a slot added here would silently lose its
     * row. */
    UT_ASSERT_MSG(PING_BIND_DIRECT_SLOTS == PING_KIND_COUNT,
                  "PING_BIND_DIRECT_SLOTS = %d but PING_KIND_COUNT = %d",
                  PING_BIND_DIRECT_SLOTS, PING_KIND_COUNT);

    /* All unbound is the shipped state: nothing fires. */
    memset(direct, 0, sizeof(direct));
    UT_ASSERT(pingBindingDirectKind(direct, PING_BIND_DIRECT_SLOTS,
                                    rmbCode, 0) < 0);
    UT_ASSERT(pingBindingDirectKind(direct, PING_BIND_DIRECT_SLOTS,
                                    PT_SCANCODE_V, PING_BIND_MOD_CTRL) < 0);

    /* Each slot answers with its own kind, and with the same exact-modifier
     * rule the menu slots use. */
    for (i = 0; i < PING_BIND_DIRECT_SLOTS; i++) {
        memset(direct, 0, sizeof(direct));
        direct[i] = pingBindingEncode(PING_BIND_MOD_SHIFT, PT_SCANCODE_V);
        UT_ASSERT_MSG(pingBindingDirectKind(direct, PING_BIND_DIRECT_SLOTS,
                                            PT_SCANCODE_V,
                                            PING_BIND_MOD_SHIFT) == i,
                      "slot %d did not answer with its own kind", i);
        UT_ASSERT(pingBindingDirectKind(direct, PING_BIND_DIRECT_SLOTS,
                                        PT_SCANCODE_V, 0) < 0);
        UT_ASSERT(pingBindingDirectKind(direct, PING_BIND_DIRECT_SLOTS,
                                        PT_SCANCODE_V,
                                        PING_BIND_MOD_SHIFT |
                                        PING_BIND_MOD_CTRL) < 0);
    }

    /* The standard ping is slot 0, so the first row of the direct group is
     * the plain marker — the same thing the pie's centre sends. */
    memset(direct, 0, sizeof(direct));
    direct[PING_KIND_STANDARD] = ctrlRmb;
    UT_ASSERT(pingBindingDirectKind(direct, PING_BIND_DIRECT_SLOTS, rmbCode,
                                    PING_BIND_MOD_CTRL) == PING_KIND_STANDARD);

    /* A chord on a direct slot AND a menu slot: both report a match, and the
     * caller is required to ask the direct one first, so that is the one that
     * sends. This pins that both answers really are available — the
     * precedence itself lives in ping_overlay.cpp. */
    memset(menu, 0, sizeof(menu));
    menu[0] = ctrlRmb;
    direct[PING_KIND_ATTACK] = ctrlRmb;
    direct[PING_KIND_STANDARD] = PING_BIND_NONE;
    UT_ASSERT(pingBindingDirectKind(direct, PING_BIND_DIRECT_SLOTS, rmbCode,
                                    PING_BIND_MOD_CTRL) == PING_KIND_ATTACK);
    UT_ASSERT(pingBindingMatchAny(menu, PING_BIND_SLOTS, rmbCode,
                                  PING_BIND_MOD_CTRL) == 0);
    return 0;
}

int run_ping_binding_format(void) {
    char buf[64];
    int  b;

    b = pingBindingEncode(PING_BIND_MOD_CTRL,
                          pingBindingMouseCode(PT_MOUSE_RIGHT));
    pingBindingFormat(b, "Ctrl", "Alt", "Shift", "Right Mouse", "None",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Ctrl+Right Mouse") == 0, "got \"%s\"", buf);

    /* Fixed modifier order regardless of how the chord was built. */
    b = pingBindingEncode(PING_BIND_MOD_SHIFT | PING_BIND_MOD_ALT |
                          PING_BIND_MOD_CTRL, PT_SCANCODE_V);
    pingBindingFormat(b, "Ctrl", "Alt", "Shift", "V", "None",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Ctrl+Alt+Shift+V") == 0, "got \"%s\"", buf);

    /* No modifiers: just the code's name, no leading separator. */
    b = pingBindingEncode(0, PT_SCANCODE_V);
    pingBindingFormat(b, "Ctrl", "Alt", "Shift", "V", "None",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "V") == 0, "got \"%s\"", buf);

    /* Unbound shows the unbound word on its own. */
    pingBindingFormat(PING_BIND_NONE, "Ctrl", "Alt", "Shift", "V", "None",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "None") == 0, "got \"%s\"", buf);

    /* A buffer too small truncates and still terminates. */
    b = pingBindingEncode(PING_BIND_MOD_CTRL, PT_SCANCODE_V);
    memset(buf, 'x', sizeof(buf));
    pingBindingFormat(b, "Ctrl", "Alt", "Shift", "V", "None", buf, 4);
    UT_ASSERT_MSG(strlen(buf) == 3, "len = %d", (int)strlen(buf));
    UT_ASSERT_MSG(strcmp(buf, "Ctr") == 0, "got \"%s\"", buf);
    return 0;
}

int run_ping_pie_slices(void) {
    const float dead = 20.0f;
    const float r    = 80.0f;
    int i;

    /* The dead zone is the standard ping, and so is the exact centre. */
    UT_ASSERT(pingPieSliceAt(0.0f, 0.0f, dead) < 0);
    UT_ASSERT(pingPieSliceAt(dead * 0.7f, 0.0f, dead) < 0);
    UT_ASSERT(pingPieKindAt(0.0f, 0.0f, dead) == PING_KIND_STANDARD);
    UT_ASSERT(pingPieKindAt(0.0f, -dead * 0.5f, dead) == PING_KIND_STANDARD);

    /* Straight up is Caution — the slice that straddles the wrap in atan2's
     * range, so a naive implementation lands one slice out here. y is
     * negative for "up" because screen y grows downward. */
    UT_ASSERT_MSG(pingPieKindAt(0.0f, -r, dead) == PING_KIND_CAUTION,
                  "straight up = %d", pingPieKindAt(0.0f, -r, dead));
    /* Either side of straight up, still inside Caution's slice: half a slice
     * is 36 degrees, so 30 degrees off is comfortably inside it. */
    UT_ASSERT(pingPieKindAt(r * 0.5f, -r * 0.866f, dead) == PING_KIND_CAUTION);
    UT_ASSERT(pingPieKindAt(-r * 0.5f, -r * 0.866f, dead) == PING_KIND_CAUTION);

    /* Each slice's own centre line selects that slice, in order, clockwise
     * from the top. This is what makes the pie's icons and its hit test the
     * same layout rather than two that happen to agree. */
    for (i = 0; i < PING_PIE_SLICES; i++) {
        float a = pingPieSliceAngle(i);
        float dx = sinf(a) * r;
        float dy = -cosf(a) * r;
        UT_ASSERT_MSG(pingPieSliceAt(dx, dy, dead) == i,
                      "slice %d centre selected %d", i,
                      pingPieSliceAt(dx, dy, dead));
        UT_ASSERT_MSG(pingPieKindAt(dx, dy, dead) == kPingPieSlices[i],
                      "slice %d kind mismatch", i);
    }

    /* The order Andrew asked for: Caution at the top, then clockwise. */
    UT_ASSERT(kPingPieSlices[0] == PING_KIND_CAUTION);
    UT_ASSERT(kPingPieSlices[1] == PING_KIND_ASSIST);
    UT_ASSERT(kPingPieSlices[2] == PING_KIND_ATTACK);
    UT_ASSERT(kPingPieSlices[3] == PING_KIND_ON_MY_WAY);
    UT_ASSERT(kPingPieSlices[4] == PING_KIND_BOT_COMMAND);

    /* Every slice reachable, no kind reachable twice, and the standard ping
     * never on the ring — it is the centre. */
    for (i = 0; i < PING_PIE_SLICES; i++) {
        int j;
        UT_ASSERT(kPingPieSlices[i] != PING_KIND_STANDARD);
        for (j = i + 1; j < PING_PIE_SLICES; j++) {
            UT_ASSERT_MSG(kPingPieSlices[i] != kPingPieSlices[j],
                          "slices %d and %d are the same kind", i, j);
        }
    }
    /* Icon offsets sit inside the ring, on the slice's own centre line. */
    for (i = 0; i < PING_PIE_SLICES; i++) {
        float ox = 0.0f, oy = 0.0f;
        pingPieIconOffset(i, r, &ox, &oy);
        UT_ASSERT(pingPieSliceAt(ox, oy, dead) == i);
        UT_ASSERT(sqrtf(ox * ox + oy * oy) < r);
    }
    return 0;
}

/* The game rectangle every edge case below is measured against: a 400x300
 * box at (100, 50), with the tank in the middle of it. */
#define PE_RX  100.0f
#define PE_RY   50.0f
#define PE_RW  400.0f
#define PE_RH  300.0f
#define PE_TX  (PE_RX + PE_RW * 0.5f)
#define PE_TY  (PE_RY + PE_RH * 0.5f)
#define PE_BAR  40.0f

static int pe_marker(float px, float py, PingEdgeMarker *out) {
    return pingEdgeMarker(PE_RX, PE_RY, PE_RW, PE_RH, PE_TX, PE_TY,
                          px, py, PE_BAR, out) ? 1 : 0;
}

int run_ping_edge_sides(void) {
    PingEdgeMarker m;

    /* A ping inside the rectangle has a world marker of its own, so there is
     * no edge marker to draw. Corners of the rectangle count as inside. */
    UT_ASSERT(!pe_marker(PE_TX, PE_TY, &m));
    UT_ASSERT(!pe_marker(PE_RX, PE_RY, &m));
    UT_ASSERT(!pe_marker(PE_RX + PE_RW, PE_RY + PE_RH, &m));
    UT_ASSERT(!pe_marker(PE_RX + 1.0f, PE_RY + PE_RH - 1.0f, &m));

    /* Straight out of each side. The bar lies along that border, centred on
     * the crossing, and its two ends are the bar's length apart. */
    UT_ASSERT(pe_marker(PE_TX - 5000.0f, PE_TY, &m));
    UT_ASSERT_MSG(m.side == PING_EDGE_LEFT, "side = %d", (int)m.side);
    UT_ASSERT_MSG(m.x0 == PE_RX && m.x1 == PE_RX, "bar off the left border");
    UT_ASSERT_MSG(fabsf(m.cy - PE_TY) < 0.01f, "cy = %f", (double)m.cy);
    UT_ASSERT_MSG(fabsf((m.y1 - m.y0) - PE_BAR) < 0.01f,
                  "bar length = %f", (double)(m.y1 - m.y0));

    UT_ASSERT(pe_marker(PE_TX + 5000.0f, PE_TY, &m));
    UT_ASSERT_MSG(m.side == PING_EDGE_RIGHT, "side = %d", (int)m.side);
    UT_ASSERT(m.x0 == PE_RX + PE_RW && m.x1 == PE_RX + PE_RW);
    UT_ASSERT(fabsf(m.cy - PE_TY) < 0.01f);

    UT_ASSERT(pe_marker(PE_TX, PE_TY - 5000.0f, &m));
    UT_ASSERT_MSG(m.side == PING_EDGE_TOP, "side = %d", (int)m.side);
    UT_ASSERT(m.y0 == PE_RY && m.y1 == PE_RY);
    UT_ASSERT(fabsf(m.cx - PE_TX) < 0.01f);
    UT_ASSERT(fabsf((m.x1 - m.x0) - PE_BAR) < 0.01f);

    UT_ASSERT(pe_marker(PE_TX, PE_TY + 5000.0f, &m));
    UT_ASSERT_MSG(m.side == PING_EDGE_BOTTOM, "side = %d", (int)m.side);
    UT_ASSERT(m.y0 == PE_RY + PE_RH && m.y1 == PE_RY + PE_RH);
    UT_ASSERT(fabsf(m.cx - PE_TX) < 0.01f);
    return 0;
}

int run_ping_edge_corner(void) {
    PingEdgeMarker m;

    /* Far out beyond the top-left corner. The ray leaves through whichever
     * border it reaches first, and the bar is slid along that border so both
     * ends stay inside the rectangle — at a corner it sits flush into it
     * rather than hanging half off the screen. */
    UT_ASSERT(pe_marker(PE_TX - 5000.0f, PE_TY - 5000.0f, &m));
    UT_ASSERT_MSG(m.side == PING_EDGE_LEFT || m.side == PING_EDGE_TOP,
                  "side = %d", (int)m.side);
    UT_ASSERT_MSG(m.x0 >= PE_RX - 0.01f && m.x1 <= PE_RX + PE_RW + 0.01f,
                  "bar x %f..%f outside the rectangle",
                  (double)m.x0, (double)m.x1);
    UT_ASSERT_MSG(m.y0 >= PE_RY - 0.01f && m.y1 <= PE_RY + PE_RH + 0.01f,
                  "bar y %f..%f outside the rectangle",
                  (double)m.y0, (double)m.y1);

    /* Same for the other three corners. */
    UT_ASSERT(pe_marker(PE_TX + 5000.0f, PE_TY - 5000.0f, &m));
    UT_ASSERT(m.x0 >= PE_RX - 0.01f && m.x1 <= PE_RX + PE_RW + 0.01f);
    UT_ASSERT(m.y0 >= PE_RY - 0.01f && m.y1 <= PE_RY + PE_RH + 0.01f);
    UT_ASSERT(pe_marker(PE_TX - 5000.0f, PE_TY + 5000.0f, &m));
    UT_ASSERT(m.x0 >= PE_RX - 0.01f && m.x1 <= PE_RX + PE_RW + 0.01f);
    UT_ASSERT(m.y0 >= PE_RY - 0.01f && m.y1 <= PE_RY + PE_RH + 0.01f);
    UT_ASSERT(pe_marker(PE_TX + 5000.0f, PE_TY + 5000.0f, &m));
    UT_ASSERT(m.x0 >= PE_RX - 0.01f && m.x1 <= PE_RX + PE_RW + 0.01f);
    UT_ASSERT(m.y0 >= PE_RY - 0.01f && m.y1 <= PE_RY + PE_RH + 0.01f);

    /* Exactly diagonal from a corner of the rectangle: the crossing is the
     * corner itself, and the bar must still be wholly inside. */
    UT_ASSERT(pe_marker(PE_RX - 100.0f, PE_RY - 75.0f, &m));
    UT_ASSERT(m.x0 >= PE_RX - 0.01f && m.x1 <= PE_RX + PE_RW + 0.01f);
    UT_ASSERT(m.y0 >= PE_RY - 0.01f && m.y1 <= PE_RY + PE_RH + 0.01f);

    /* A degenerate rectangle produces nothing rather than dividing by it. */
    UT_ASSERT(!pingEdgeMarker(PE_RX, PE_RY, 0.0f, PE_RH, PE_TX, PE_TY,
                              PE_TX + 100.0f, PE_TY, PE_BAR, &m));
    UT_ASSERT(!pingEdgeMarker(PE_RX, PE_RY, PE_RW, PE_RH, PE_TX, PE_TY,
                              PE_TX + 100.0f, PE_TY, PE_BAR, NULL));
    return 0;
}

/* The size an edge marker is drawn at, from how far away the ping is. The
 * knobs themselves live in ping_kinds.h; what is pinned here is the shape of
 * the ramp between them, which is what makes a near ping read as near. */
int run_ping_edge_size_from_distance(void) {
    float nearF, farF, midF, len;

    /* At or inside the near distance it is full size, at or past the far one
     * it is the minimum, and neither end runs past its knob. */
    nearF = pingEdgeSizeFactor(0.0f, PING_EDGE_NEAR_TILES, PING_EDGE_FAR_TILES);
    UT_ASSERT_MSG(nearF == 1.0f, "on top of the tank: %f", (double)nearF);
    nearF = pingEdgeSizeFactor(PING_EDGE_NEAR_TILES, PING_EDGE_NEAR_TILES,
                               PING_EDGE_FAR_TILES);
    UT_ASSERT_MSG(nearF == 1.0f, "at the near knob: %f", (double)nearF);
    farF = pingEdgeSizeFactor(PING_EDGE_FAR_TILES, PING_EDGE_NEAR_TILES,
                              PING_EDGE_FAR_TILES);
    UT_ASSERT_MSG(farF == 0.0f, "at the far knob: %f", (double)farF);
    farF = pingEdgeSizeFactor(10000.0f, PING_EDGE_NEAR_TILES,
                              PING_EDGE_FAR_TILES);
    UT_ASSERT_MSG(farF == 0.0f, "off the map: %f", (double)farF);

    /* In between it falls, and never rises with distance. */
    midF = pingEdgeSizeFactor((PING_EDGE_NEAR_TILES + PING_EDGE_FAR_TILES) *
                              0.5f, PING_EDGE_NEAR_TILES, PING_EDGE_FAR_TILES);
    UT_ASSERT_MSG(midF > 0.0f && midF < 1.0f, "mid = %f", (double)midF);
    {
        float prev = 1.0f;
        float d;
        for (d = 0.0f; d <= 80.0f; d += 1.0f) {
            float f = pingEdgeSizeFactor(d, PING_EDGE_NEAR_TILES,
                                         PING_EDGE_FAR_TILES);
            UT_ASSERT_MSG(f <= prev + 1e-6f,
                          "size went up at %f tiles: %f after %f",
                          (double)d, (double)f, (double)prev);
            prev = f;
        }
    }

    /* The ramp is not a straight line: half way out in distance is already
     * well under half size, so the difference between "just off the view" and
     * "across the map" is spent where the player can see it. */
    UT_ASSERT_MSG(midF < 0.5f, "mid should be past halfway down: %f",
                  (double)midF);

    /* A near/far pair the wrong way round is refused into full size rather
     * than dividing by nothing. */
    UT_ASSERT(pingEdgeSizeFactor(50.0f, 64.0f, 64.0f) == 1.0f);
    UT_ASSERT(pingEdgeSizeFactor(50.0f, 64.0f, 8.0f) == 1.0f);

    /* Pixels for a fraction: the two ends are the knobs themselves, a
     * fraction between them lands between them, and an out-of-range fraction
     * is clamped rather than extrapolated. */
    len = pingEdgeSizeFor(1.0f, PING_EDGE_LENGTH_MIN_PX,
                          PING_EDGE_LENGTH_MAX_PX);
    UT_ASSERT_MSG(len == PING_EDGE_LENGTH_MAX_PX, "near bar = %f", (double)len);
    len = pingEdgeSizeFor(0.0f, PING_EDGE_LENGTH_MIN_PX,
                          PING_EDGE_LENGTH_MAX_PX);
    UT_ASSERT_MSG(len == PING_EDGE_LENGTH_MIN_PX, "far bar = %f", (double)len);
    len = pingEdgeSizeFor(0.5f, PING_EDGE_LENGTH_MIN_PX,
                          PING_EDGE_LENGTH_MAX_PX);
    UT_ASSERT(len > PING_EDGE_LENGTH_MIN_PX && len < PING_EDGE_LENGTH_MAX_PX);
    UT_ASSERT(pingEdgeSizeFor(-3.0f, 10.0f, 50.0f) == 10.0f);
    UT_ASSERT(pingEdgeSizeFor(9.0f, 10.0f, 50.0f) == 50.0f);

    /* The icon never drops below what a glyph needs to still be a glyph. */
    UT_ASSERT(PING_EDGE_ICON_MIN_PX >= 12.0f);
    UT_ASSERT(pingEdgeSizeFor(0.0f, PING_EDGE_ICON_MIN_PX,
                              PING_EDGE_ICON_MAX_PX) >= 12.0f);

    /* A far ping really is smaller than a near one, both parts of it. */
    UT_ASSERT(pingEdgeSizeFor(farF, PING_EDGE_LENGTH_MIN_PX,
                              PING_EDGE_LENGTH_MAX_PX) <
              pingEdgeSizeFor(nearF, PING_EDGE_LENGTH_MIN_PX,
                              PING_EDGE_LENGTH_MAX_PX));
    UT_ASSERT(pingEdgeSizeFor(farF, PING_EDGE_ICON_MIN_PX,
                              PING_EDGE_ICON_MAX_PX) <
              pingEdgeSizeFor(nearF, PING_EDGE_ICON_MIN_PX,
                              PING_EDGE_ICON_MAX_PX));
    return 0;
}

/* Where the icon and the sender's name sit around an edge bar, on each of the
 * four borders and in a corner. The name is the thing a player reads off the
 * indicator, so what matters is that it is always on the inside of the
 * rectangle and never hanging off it. */
int run_ping_edge_name_anchor(void) {
    PingEdgeMarker  m;
    PingEdgeNameBox box;
    const float     thick  = 10.0f;
    const float     iconPx = 24.0f;
    const float     gap    = 3.0f;
    const float     textW  = 60.0f;
    const float     textH  = 14.0f;
    float ix, iy;

    /* The icon steps in off its border and stays on the bar's centre line. */
    UT_ASSERT(pe_marker(PE_TX - 5000.0f, PE_TY, &m));
    pingEdgeIconCentre(&m, thick, iconPx, gap, &ix, &iy);
    UT_ASSERT_MSG(ix > PE_RX && ix < PE_RX + PE_RW, "icon x = %f", (double)ix);
    UT_ASSERT_MSG(fabsf(ix - (PE_RX + thick * 0.5f + iconPx * 0.5f + gap)) <
                  0.01f, "icon x = %f", (double)ix);
    UT_ASSERT_MSG(fabsf(iy - m.cy) < 0.01f, "icon y = %f", (double)iy);

    /* Left border: the name runs to the right of the icon, into the view, on
     * the icon's own centre line. */
    UT_ASSERT(pingEdgeNameAnchor(PING_EDGE_LEFT, ix, iy, iconPx, gap,
                                 textW, textH,
                                 PE_RX, PE_RY, PE_RW, PE_RH, &box));
    UT_ASSERT(!box.clamped);
    UT_ASSERT_MSG(fabsf(box.x - (ix + iconPx * 0.5f + gap)) < 0.01f,
                  "name x = %f", (double)box.x);
    UT_ASSERT_MSG(fabsf((box.y + textH * 0.5f) - iy) < 0.01f,
                  "name y = %f", (double)box.y);

    /* Right border: the mirror image — the name is to the LEFT of the icon,
     * which is what keeps it inside the rectangle. */
    UT_ASSERT(pe_marker(PE_TX + 5000.0f, PE_TY, &m));
    pingEdgeIconCentre(&m, thick, iconPx, gap, &ix, &iy);
    UT_ASSERT(pingEdgeNameAnchor(PING_EDGE_RIGHT, ix, iy, iconPx, gap,
                                 textW, textH,
                                 PE_RX, PE_RY, PE_RW, PE_RH, &box));
    UT_ASSERT(!box.clamped);
    UT_ASSERT_MSG(fabsf((box.x + textW) - (ix - iconPx * 0.5f - gap)) < 0.01f,
                  "name x = %f", (double)box.x);
    UT_ASSERT(box.x >= PE_RX && box.x + textW <= PE_RX + PE_RW);

    /* Top border: under the icon and centred on it. */
    UT_ASSERT(pe_marker(PE_TX, PE_TY - 5000.0f, &m));
    pingEdgeIconCentre(&m, thick, iconPx, gap, &ix, &iy);
    UT_ASSERT_MSG(fabsf(iy - (PE_RY + thick * 0.5f + iconPx * 0.5f + gap)) <
                  0.01f, "icon y = %f", (double)iy);
    UT_ASSERT(pingEdgeNameAnchor(PING_EDGE_TOP, ix, iy, iconPx, gap,
                                 textW, textH,
                                 PE_RX, PE_RY, PE_RW, PE_RH, &box));
    UT_ASSERT(!box.clamped);
    UT_ASSERT_MSG(fabsf((box.x + textW * 0.5f) - ix) < 0.01f,
                  "name x = %f", (double)box.x);
    UT_ASSERT_MSG(box.y > iy, "name should be under the icon: %f",
                  (double)box.y);

    /* Bottom border: above the icon, so it never runs off the bottom. */
    UT_ASSERT(pe_marker(PE_TX, PE_TY + 5000.0f, &m));
    pingEdgeIconCentre(&m, thick, iconPx, gap, &ix, &iy);
    UT_ASSERT(pingEdgeNameAnchor(PING_EDGE_BOTTOM, ix, iy, iconPx, gap,
                                 textW, textH,
                                 PE_RX, PE_RY, PE_RW, PE_RH, &box));
    UT_ASSERT(!box.clamped);
    UT_ASSERT_MSG((box.y + textH) < iy, "name should be above the icon: %f",
                  (double)box.y);
    UT_ASSERT(box.y >= PE_RY && box.y + textH <= PE_RY + PE_RH);

    /* A corner. The bar is already slid flush into it, so a name centred on
     * the icon would hang past the end of the rectangle; it is clamped back
     * in, and says so. */
    UT_ASSERT(pe_marker(PE_TX - 5000.0f, PE_TY - 5000.0f, &m));
    pingEdgeIconCentre(&m, thick, iconPx, gap, &ix, &iy);
    UT_ASSERT(pingEdgeNameAnchor(m.side, ix, iy, iconPx, gap, textW, textH,
                                 PE_RX, PE_RY, PE_RW, PE_RH, &box));
    UT_ASSERT(box.x >= PE_RX - 0.01f);
    UT_ASSERT(box.y >= PE_RY - 0.01f);
    UT_ASSERT(box.x + textW <= PE_RX + PE_RW + 0.01f);
    UT_ASSERT(box.y + textH <= PE_RY + PE_RH + 0.01f);

    /* Every corner, on both borders that can serve it, with a name long
     * enough to be awkward: always wholly inside. */
    {
        const float longW = 180.0f;
        float cornerX[4] = { PE_TX - 5000.0f, PE_TX + 5000.0f,
                             PE_TX - 5000.0f, PE_TX + 5000.0f };
        float cornerY[4] = { PE_TY - 5000.0f, PE_TY - 5000.0f,
                             PE_TY + 5000.0f, PE_TY + 5000.0f };
        int c;
        for (c = 0; c < 4; c++) {
            UT_ASSERT(pe_marker(cornerX[c], cornerY[c], &m));
            pingEdgeIconCentre(&m, thick, iconPx, gap, &ix, &iy);
            UT_ASSERT(pingEdgeNameAnchor(m.side, ix, iy, iconPx, gap,
                                         longW, textH,
                                         PE_RX, PE_RY, PE_RW, PE_RH, &box));
            UT_ASSERT_MSG(box.x >= PE_RX - 0.01f &&
                          box.x + longW <= PE_RX + PE_RW + 0.01f,
                          "corner %d name x %f..%f outside the rectangle",
                          c, (double)box.x, (double)(box.x + longW));
            UT_ASSERT_MSG(box.y >= PE_RY - 0.01f &&
                          box.y + textH <= PE_RY + PE_RH + 0.01f,
                          "corner %d name y %f..%f outside the rectangle",
                          c, (double)box.y, (double)(box.y + textH));
        }
    }

    /* A name wider than the whole rectangle is pinned to the left edge, so at
     * least its start can be read, and reported as clamped. */
    UT_ASSERT(pingEdgeNameAnchor(PING_EDGE_TOP, PE_TX, PE_RY + 20.0f, iconPx,
                                 gap, PE_RW + 100.0f, textH,
                                 PE_RX, PE_RY, PE_RW, PE_RH, &box));
    UT_ASSERT(box.clamped);
    UT_ASSERT_MSG(box.x == PE_RX, "name x = %f", (double)box.x);

    /* No rectangle and no output: nothing to place against. */
    UT_ASSERT(!pingEdgeNameAnchor(PING_EDGE_TOP, PE_TX, PE_TY, iconPx, gap,
                                  textW, textH, PE_RX, PE_RY, 0.0f, PE_RH,
                                  &box));
    UT_ASSERT(!pingEdgeNameAnchor(PING_EDGE_TOP, PE_TX, PE_TY, iconPx, gap,
                                  textW, textH, PE_RX, PE_RY, PE_RW, PE_RH,
                                  NULL));
    return 0;
}

int run_ping_rect_inset(void) {
    PingRect r;

    /* Nothing over the map: the rectangle comes back as it went in. */
    UT_ASSERT(pingRectInset(PE_RX, PE_RY, PE_RW, PE_RH, 0.0f, 0.0f, 0.0f, 0.0f,
                            &r));
    UT_ASSERT(r.x == PE_RX && r.y == PE_RY);
    UT_ASSERT(r.w == PE_RW && r.h == PE_RH);

    /* The overview's four panels, each eating its own edge. The origin moves
     * by the left and top strips only; the size loses both sides. */
    UT_ASSERT(pingRectInset(PE_RX, PE_RY, PE_RW, PE_RH,
                            30.0f, 10.0f, 50.0f, 20.0f, &r));
    UT_ASSERT_MSG(r.x == PE_RX + 30.0f, "x = %f", (double)r.x);
    UT_ASSERT_MSG(r.y == PE_RY + 10.0f, "y = %f", (double)r.y);
    UT_ASSERT_MSG(r.w == PE_RW - 80.0f, "w = %f", (double)r.w);
    UT_ASSERT_MSG(r.h == PE_RH - 30.0f, "h = %f", (double)r.h);

    /* A negative inset is read as none rather than growing the rectangle out
     * over the chrome. */
    UT_ASSERT(pingRectInset(PE_RX, PE_RY, PE_RW, PE_RH,
                            -40.0f, 0.0f, 0.0f, -5.0f, &r));
    UT_ASSERT(r.x == PE_RX && r.w == PE_RW);
    UT_ASSERT(r.y == PE_RY && r.h == PE_RH);

    /* Insets that would leave nothing: refused, and the rectangle is handed
     * back unshrunk so a bar somewhere still beats no bar at all. */
    UT_ASSERT(!pingRectInset(PE_RX, PE_RY, PE_RW, PE_RH,
                             PE_RW, 0.0f, PE_RW, 0.0f, &r));
    UT_ASSERT(r.x == PE_RX && r.w == PE_RW);
    UT_ASSERT(!pingRectInset(PE_RX, PE_RY, PE_RW, PE_RH,
                             0.0f, PE_RH * 0.6f, 0.0f, PE_RH * 0.6f, &r));
    UT_ASSERT(r.y == PE_RY && r.h == PE_RH);

    /* A degenerate rectangle, and no output at all. */
    UT_ASSERT(!pingRectInset(PE_RX, PE_RY, 0.0f, PE_RH, 0.0f, 0.0f, 0.0f, 0.0f,
                             &r));
    UT_ASSERT(!pingRectInset(PE_RX, PE_RY, PE_RW, PE_RH, 0.0f, 0.0f, 0.0f,
                             0.0f, NULL));

    /* The shrunk rectangle is what an edge marker is then laid along, so a
     * ping that is on the picture but under a panel still gets a bar. */
    {
        PingEdgeMarker m;
        UT_ASSERT(pingRectInset(PE_RX, PE_RY, PE_RW, PE_RH,
                                0.0f, 0.0f, 100.0f, 0.0f, &r));
        UT_ASSERT(pingEdgeMarker(r.x, r.y, r.w, r.h,
                                 r.x + r.w * 0.5f, r.y + r.h * 0.5f,
                                 PE_RX + PE_RW - 10.0f, PE_TY, PE_BAR, &m));
        UT_ASSERT_MSG(m.side == PING_EDGE_RIGHT, "side = %d", (int)m.side);
        UT_ASSERT_MSG(m.x0 == r.x + r.w,
                      "bar at %f, wanted the inset border %f",
                      (double)m.x0, (double)(r.x + r.w));
    }
    return 0;
}

int run_ping_event_wire(void) {
    int k;

    /* [sender, kind, xHi, xLo, yHi, yLo]. The server's writer and the
     * client's reader both index those six bytes; a size that drifts here
     * silently truncates the coordinates on the wire. */
    UT_ASSERT_MSG(gameEventDataSize(EVENT_PING) == 6,
                  "EVENT_PING data size = %d", gameEventDataSize(EVENT_PING));
    UT_ASSERT(gameEventDataSize(EVENT_PING) <= GAME_EVENT_MAX_DATA);

    /* A ping is one-shot: there is no later state that would carry it, so a
     * dropped one is simply gone. It has to ride the reliable channel. */
    UT_ASSERT_MSG(gameEventIsReliable(EVENT_PING),
                  "EVENT_PING must be reliable");

    /* The id sits past the last one this build knows about, and the kind
     * space is closed. */
    UT_ASSERT(EVENT_PING == 20);
    UT_ASSERT(EVENT_PING > EVENT_BASE_STOCK);
    UT_ASSERT(PING_KIND_COUNT == 6);

    /* Every kind has a style row: a colour and an icon file. The table is
     * what the pie, the world marker, the edge marker and the replay all
     * read, so a kind with no row would draw as a hole in one of them. */
    for (k = 0; k < PING_KIND_COUNT; k++) {
        const PingKindStyle *st = pingKindStyle((unsigned char)k);
        UT_ASSERT_MSG(st == &kPingKindStyles[k], "kind %d style mismatch", k);
        UT_ASSERT_MSG(st->iconFile != NULL && st->iconFile[0] != '\0',
                      "kind %d has no icon file", k);
        UT_ASSERT_MSG((int)st->r + st->g + st->b > 0,
                      "kind %d is black on black", k);
    }
    /* An unknown kind falls back to the standard ping rather than reading
     * off the end — a ping from a newer build still draws something. */
    UT_ASSERT(pingKindStyle(PING_KIND_COUNT) == &kPingKindStyles[PING_KIND_STANDARD]);
    UT_ASSERT(pingKindStyle(255) == &kPingKindStyles[PING_KIND_STANDARD]);

    /* Solid until the fade window, a straight ramp through it, gone after. */
    UT_ASSERT(pingDisplayAlpha(0) == 1.0f);
    UT_ASSERT(pingDisplayAlpha(PING_DISPLAY_MS - PING_FADE_MS) == 1.0f);
    UT_ASSERT(pingDisplayAlpha(PING_DISPLAY_MS) == 0.0f);
    UT_ASSERT(pingDisplayAlpha(PING_DISPLAY_MS + 1000) == 0.0f);
    UT_ASSERT(pingDisplayAlpha(-1) == 0.0f);
    {
        float half = pingDisplayAlpha(PING_DISPLAY_MS - PING_FADE_MS / 2);
        UT_ASSERT_MSG(fabsf(half - 0.5f) < 0.01f, "mid-fade alpha = %f",
                      (double)half);
    }
    return 0;
}

int run_ping_sound_fallback(void) {
    int k;

    /* Every kind has its own effect id, and no two kinds share one — a
       shared id would silently give two kinds the same sound. */
    for (k = 0; k < PING_KIND_COUNT; k++) {
        int j;
        sndEffects e = pingSoundEffect((unsigned char)k);
        UT_ASSERT_MSG(e != pingDefault,
                      "kind %d has no sound of its own to ask for", k);
        UT_ASSERT_MSG(pingSoundKindOf(e) == (unsigned char)k,
                      "kind %d does not round-trip through its effect id", k);
        for (j = 0; j < k; j++) {
            UT_ASSERT_MSG(pingSoundEffect((unsigned char)j) != e,
                          "kinds %d and %d share one sound", j, k);
        }
    }

    /* The default is what the fallback produces, never something the
       fallback is applied to, and an ordinary sound is not a ping at all. */
    UT_ASSERT(pingSoundKindOf(pingDefault) == PING_SOUND_KIND_NONE);
    UT_ASSERT(pingSoundKindOf(lobbyReady) == PING_SOUND_KIND_NONE);
    UT_ASSERT(pingSoundKindOf(shootSelf) == PING_SOUND_KIND_NONE);

    /* Nothing found: every kind plays the default. This is the shipped game
       with only ping_default.wav in place, and the skin that replaces just
       that one file and so replaces all six. */
    for (k = 0; k < PING_KIND_COUNT; k++) {
        UT_ASSERT_MSG(pingSoundResolve((unsigned char)k, 0u) == pingDefault,
                      "kind %d with no file of its own should play the default",
                      k);
    }

    /* Everything found: every kind plays its own. */
    for (k = 0; k < PING_KIND_COUNT; k++) {
        unsigned int all = (1u << PING_KIND_COUNT) - 1u;
        UT_ASSERT_MSG(pingSoundResolve((unsigned char)k, all) ==
                          pingSoundEffect((unsigned char)k),
                      "kind %d with a file of its own should play it", k);
    }

    /* One file found: that kind alone leaves the default. The skin that
       ships ping_attack.wav and nothing else, and equally the game once
       someone drops ping_attack.wav into data/sounds/. */
    for (k = 0; k < PING_KIND_COUNT; k++) {
        unsigned int one = 1u << PING_KIND_ATTACK;
        sndEffects want = (k == PING_KIND_ATTACK) ? pingAttack : pingDefault;
        UT_ASSERT_MSG(pingSoundResolve((unsigned char)k, one) == want,
                      "kind %d resolved wrong with only the attack file", k);
    }

    /* The two files the game actually ships today. */
    {
        unsigned int shipped = 1u << PING_KIND_CAUTION;
        UT_ASSERT(pingSoundResolve(PING_KIND_CAUTION, shipped) == pingCaution);
        UT_ASSERT(pingSoundResolve(PING_KIND_STANDARD, shipped) == pingDefault);
        UT_ASSERT(pingSoundResolve(PING_KIND_ASSIST, shipped) == pingDefault);
        UT_ASSERT(pingSoundResolve(PING_KIND_ATTACK, shipped) == pingDefault);
        UT_ASSERT(pingSoundResolve(PING_KIND_ON_MY_WAY, shipped) == pingDefault);
        UT_ASSERT(pingSoundResolve(PING_KIND_BOT_COMMAND, shipped) == pingDefault);
    }

    /* A kind from a newer build clamps to the standard ping, the same
       fallback the style table and the newswire line make — so it is audible
       either as the standard sound or as the default, never as silence and
       never off the end of the mask. */
    UT_ASSERT(pingSoundEffect(PING_KIND_COUNT) == pingStandard);
    UT_ASSERT(pingSoundEffect(255) == pingStandard);
    UT_ASSERT(pingSoundResolve(PING_KIND_COUNT, 0u) == pingDefault);
    UT_ASSERT(pingSoundResolve(255, 1u << PING_KIND_STANDARD) == pingStandard);

    return 0;
}


/* Sender names built out of hex escapes rather than typed in, so the file
   stays plain ASCII and no compiler's idea of the source encoding can change
   what the test is checking.

   PT_CYR6 is six Cyrillic letters, twelve bytes; PT_CYR8 adds two more for
   eight letters in sixteen bytes. PT_GRIN is U+1F600, four bytes for one
   character - the widest sequence UTF-8 has. */
#define PT_CYR6  "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"
#define PT_CYR8  PT_CYR6 "\xD0\xB8\xD0\xBA"
#define PT_CYR2  "\xD0\x9F\xD1\x80"
#define PT_GRIN  "\xF0\x9F\x98\x80"

/* Some ellipsis other than PING_NAME_ELLIPSIS, so the tests below drive the
   `ellipsis` argument rather than only the default. Three dots is what a
   caller whose font has no U+2026 would pass; every renderer in the tree can
   draw the real one, so this lives here rather than in ping_kinds.h. */
#define PT_ELLIPSIS_ASCII "..."

int run_ping_name_truncate(void) {
    char out[PING_NAME_DISPLAY_MAX];

    /* Six is the cap, and it counts characters. */
    UT_ASSERT(PING_NAME_MAX_CHARS == 6);

    /* Shorter than the cap, and exactly at it: drawn as they are. */
    UT_ASSERT(strcmp(pingDisplayName("Bob", NULL, out, sizeof(out)), "Bob") == 0);
    UT_ASSERT(strcmp(pingDisplayName("Andrew", NULL, out, sizeof(out)),
                     "Andrew") == 0);

    /* One over, and well over: six characters and the ellipsis. */
    UT_ASSERT(strcmp(pingDisplayName("Andreww", NULL, out, sizeof(out)),
                     "Andrew" PING_NAME_ELLIPSIS) == 0);
    UT_ASSERT(strcmp(pingDisplayName("Bartholomew", NULL, out, sizeof(out)),
                     "Bartho" PING_NAME_ELLIPSIS) == 0);

    /* A caller that asks for a different ellipsis gets the same cut with its
       own tail on the end. */
    UT_ASSERT(strcmp(pingDisplayName("Bartholomew", PT_ELLIPSIS_ASCII,
                                     out, sizeof(out)), "Bartho...") == 0);

    /* Multibyte: six Cyrillic letters are twelve bytes and are six
       characters, so they are left alone; eight are cut after the sixth,
       which is a character boundary and not a byte one. */
    UT_ASSERT(strcmp(pingDisplayName(PT_CYR6, NULL, out, sizeof(out)),
                     PT_CYR6) == 0);
    UT_ASSERT(strcmp(pingDisplayName(PT_CYR8, NULL, out, sizeof(out)),
                     PT_CYR6 PING_NAME_ELLIPSIS) == 0);
    /* Nothing left over from a half-copied sequence: twelve bytes of letters
       and three of ellipsis. */
    UT_ASSERT_MSG(strlen(pingDisplayName(PT_CYR8, NULL, out, sizeof(out))) == 15,
                  "cut Cyrillic name is %d bytes", (int)strlen(out));

    /* Four-byte characters: seven of them are seven characters, cut to six,
       and the cut lands between sequences. */
    {
        const char *six   = PT_GRIN PT_GRIN PT_GRIN PT_GRIN PT_GRIN PT_GRIN;
        const char *seven = PT_GRIN PT_GRIN PT_GRIN PT_GRIN PT_GRIN PT_GRIN
                            PT_GRIN;
        UT_ASSERT(strcmp(pingDisplayName(six, NULL, out, sizeof(out)), six) == 0);
        UT_ASSERT(strcmp(pingDisplayName(seven, NULL, out, sizeof(out)),
                         PT_GRIN PT_GRIN PT_GRIN PT_GRIN PT_GRIN PT_GRIN
                         PING_NAME_ELLIPSIS) == 0);
        /* The buffer the callers all use is big enough for that worst case. */
        UT_ASSERT(strlen(out) < PING_NAME_DISPLAY_MAX);
    }

    /* Nothing to draw: an empty name and a NULL one both come back empty
       rather than as a lone ellipsis. */
    UT_ASSERT(strcmp(pingDisplayName("", NULL, out, sizeof(out)), "") == 0);
    UT_ASSERT(strcmp(pingDisplayName(NULL, NULL, out, sizeof(out)), "") == 0);

    /* No buffer at all, and a zero-sized one: an empty string back, and
       nothing written anywhere. */
    UT_ASSERT(pingDisplayName("Bartholomew", NULL, NULL, sizeof(out)) != NULL);
    UT_ASSERT(strcmp(pingDisplayName("Bartholomew", NULL, NULL, sizeof(out)),
                     "") == 0);
    out[0] = 'x';
    UT_ASSERT(strcmp(pingDisplayName("Bartholomew", NULL, out, 0), "") == 0);
    UT_ASSERT_MSG(out[0] == 'x', "a zero-size buffer was written to");

    /* A buffer too small for the whole result loses whole characters off the
       end rather than half of one, and is always terminated. */
    {
        char tiny[4];
        UT_ASSERT(strcmp(pingDisplayName("Bartholomew", PT_ELLIPSIS_ASCII,
                                         tiny, sizeof(tiny)), "...") == 0);
        UT_ASSERT(strcmp(pingDisplayName(PT_CYR8, NULL, tiny, sizeof(tiny)),
                         PING_NAME_ELLIPSIS) == 0);
    }
    {
        char one[1];
        UT_ASSERT(strcmp(pingDisplayName("Bartholomew", NULL, one, sizeof(one)),
                         "") == 0);
    }
    {
        /* Room for two of the Cyrillic letters and the ellipsis: a fifth byte
           of letters would split the third, so only four are kept. */
        char eight[8];
        UT_ASSERT(strcmp(pingDisplayName(PT_CYR8, NULL, eight, sizeof(eight)),
                         PT_CYR2 PING_NAME_ELLIPSIS) == 0);
    }

    return 0;
}
