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
 *
 *   input_packet.h - EVENT_PING's data size and reliability, which the
 *                    server's writer and the client's reader both read out
 *                    of the same two switches.
 */

#include <math.h>
#include <string.h>

#include "input_packet.h"
#include "../../src/gui/ping_kinds.h"
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

    /* The whole four-slot array: the first slot that fires wins, and an
     * array of unbound slots reports nothing. */
    slots[0] = ctrlRmb;
    slots[1] = altRmb;
    slots[2] = PING_BIND_NONE;
    slots[3] = PING_BIND_NONE;
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
