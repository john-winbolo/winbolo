/*
 * The brain's terrain window at the map edge.
 *
 * brainDataMakeInfo hands brainDataMakeViewData an inclusive rect. The 29x29
 * tank window has its origin clamped to 226 so the far edge cannot wrap, which
 * means a tank on map row 240 or below is described by topPos 226 / bottomPos
 * 255, and a tank on column 240 or beyond by leftPos 226 / rightPos 255. Those
 * are the rects this covers.
 *
 * 255 is the value that broke it: the loop counters were BYTE, `count <= 255`
 * is true for every value a BYTE holds, and the fill never returned. It ran
 * inside the brain's first data pass, so a client whose tank sat down there
 * never reached its game loop at all — it spun at 100% of a core with its
 * socket unread until something killed it. Play does not put a tank near the
 * edge, but that first pass runs before the tank has an authoritative
 * position: with no starts list yet, startsGetStart returns without writing
 * and tankCreate places the tank on uninitialised locals, which landed at x or
 * y >= 240 about one join in eight. The baseline suite saw that as a timeout
 * on centralize_events_game_over_udp.
 *
 * A test that returns is the assertion. The buffer check is the other half:
 * the rect must actually be filled, not stepped over. An earlier form of the
 * same bug wrapped the far edge instead of clamping it, which left leftPos >
 * rightPos and skipped every row, and the brain was handed the buffer
 * untouched.
 */

#include <string.h>

#include "global.h"
#include "brain_data.h"            /* brainDataMakeViewData */
#include "client_sim.h"            /* clientSimAlloc/Create/Destroy */
#include "test_harness.h"

/* Same size brainDataMakeInfo allocates: the rects are inclusive on both ends,
 * so a 29-wide window covers 30 squares. */
#define VIEW_BUF_SIZE (30 * 30)

/* 0xEE is not a terrain byte the fill can produce, so a square still holding
 * it was never written. */
#define UNFILLED 0xEE

static void bvd_fill_rect(ClientSim *cs, BYTE left, BYTE right, BYTE top,
                          BYTE bottom, const char *what) {
    BYTE buff[VIEW_BUF_SIZE];

    memset(buff, UNFILLED, sizeof(buff));
    brainDataMakeViewData(cs, buff, left, right, top, bottom);
    UT_ASSERT_MSG(buff[0] != UNFILLED,
                  "%s: rect %u,%u..%u,%u left the first square unwritten",
                  what, (unsigned)left, (unsigned)top, (unsigned)right,
                  (unsigned)bottom);
}

int run_brain_view_data_edge_rect(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);

    /* Away from the edges: the ordinary case, and the control for the three
     * below. */
    bvd_fill_rect(cs, 100, 129, 100, 129, "interior");

    /* Bottom edge — the one the baseline scenario hit. */
    bvd_fill_rect(cs, 142, 171, 226, 255, "bottom edge");

    /* Right edge. */
    bvd_fill_rect(cs, 226, 255, 142, 171, "right edge");

    /* Both at once, for a tank in the bottom-right corner. */
    bvd_fill_rect(cs, 226, 255, 226, 255, "bottom-right corner");

    clientSimDestroy(cs);
    return 0;
}
