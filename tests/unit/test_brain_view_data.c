/*
 * The brain's terrain window at the map edge.
 *
 * brainDataMakeInfo hands brainDataMakeViewData an inclusive rect. The 29x29
 * tank window has its origin clamped to 227 so the far edge cannot wrap, which
 * means a tank on map row 241 or below is described by topPos 227 / bottomPos
 * 255, and a tank on the far columns by leftPos 227 / rightPos 255. Those are
 * the rects this covers.
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

/* Same size brainDataMakeInfo allocates. The rects below are 29 squares a
 * side, the size of the tank window, and the buffer is the largest any branch
 * of brainDataMakeInfo asks for. */
#define VIEW_BUF_SIZE (30 * 30)

/* 0xEE is not a terrain byte the fill can produce, so a square still holding
 * it was never written. */
#define UNFILLED 0xEE

/* Returns 0 when the fill returned and wrote the rect, 1 on a failed
 * assertion, which the caller propagates: UT_ASSERT_MSG returns 1 itself. */
static int bvd_fill_rect(ClientSim *cs, BYTE left, BYTE right, BYTE top,
                         BYTE bottom, const char *what) {
    BYTE buff[VIEW_BUF_SIZE];

    const int cells = (right - left + 1) * (bottom - top + 1);

    memset(buff, UNFILLED, sizeof(buff));
    brainDataMakeViewData(cs, buff, left, right, top, bottom);
    UT_ASSERT_MSG(buff[0] != UNFILLED,
                  "%s: rect %u,%u..%u,%u left the first square unwritten",
                  what, (unsigned)left, (unsigned)top, (unsigned)right,
                  (unsigned)bottom);
    /* The last square of the rect, which is what a BYTE write index could not
     * reach: it wrapped at 256 and left everything past that as the memset
     * zeros while overwriting the start of the buffer again. */
    UT_ASSERT_MSG(buff[cells - 1] != UNFILLED,
                  "%s: rect %u,%u..%u,%u stopped short — square %d of %d "
                  "unwritten",
                  what, (unsigned)left, (unsigned)top, (unsigned)right,
                  (unsigned)bottom, cells, cells);
    return 0;
}

int run_brain_view_data_edge_rect(void) {
    ClientSim *cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);

    /* Away from the edges: the ordinary case, and the control for the three
     * below. */
    if (bvd_fill_rect(cs, 100, 128, 100, 128, "interior") != 0) return 1;

    /* Bottom edge — the one the baseline scenario hit. */
    if (bvd_fill_rect(cs, 142, 170, 227, 255, "bottom edge") != 0) return 1;

    /* Right edge. */
    if (bvd_fill_rect(cs, 227, 255, 142, 170, "right edge") != 0) return 1;

    /* Both at once, for a tank in the bottom-right corner. */
    if (bvd_fill_rect(cs, 227, 255, 227, 255, "bottom-right corner") != 0) return 1;

    clientSimDestroy(cs);
    return 0;
}
