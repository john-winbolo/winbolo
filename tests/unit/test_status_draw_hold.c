/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The game window's held status panel draws (status_draw_hold.h) and which
 * slot of its text texture cache a string uses (text_cache_pick.h).
 *
 *   status_draw_hold — a status panel draw asked for between the start of a
 *       pass round the game loop and the frame waits. It is dropped when the
 *       frame repaints the panels, and one redraw is owed when it does not.
 *       A draw asked for outside a pass, or while the map overview owns the
 *       window, does not wait.
 *   text_cache_slot_pick — text reuses the slot rendered from the same
 *       renderer, face, size, colour and string; other text takes an empty
 *       slot first and with none empty the slot drawn longest ago.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "status_draw_hold.h"
#include "text_cache_pick.h"
#include "test_harness.h"

int run_status_draw_hold(void) {
    StatusDrawHold h;

    /* Outside a pass a draw goes out as asked, and nothing is owed. */
    memset(&h, 0, sizeof(h));
    UT_ASSERT(!statusDrawHoldAsk(&h, false));
    UT_ASSERT(!statusDrawHoldEnd(&h));

    /* A draw asked for before a frame that repaints the panels is dropped. */
    statusDrawHoldBegin(&h);
    UT_ASSERT(statusDrawHoldAsk(&h, false));
    UT_ASSERT(statusDrawHoldAsk(&h, false));
    statusDrawHoldRepainted(&h);
    UT_ASSERT(!statusDrawHoldEnd(&h));

    /* After the pass, draws go out as asked again. */
    UT_ASSERT(!statusDrawHoldAsk(&h, false));

    /* A draw asked for in a pass that draws no frame is owed once. */
    statusDrawHoldBegin(&h);
    UT_ASSERT(statusDrawHoldAsk(&h, false));
    UT_ASSERT(statusDrawHoldEnd(&h));
    UT_ASSERT(!statusDrawHoldEnd(&h));

    /* The frame of one pass does not count for the next. */
    statusDrawHoldBegin(&h);
    statusDrawHoldRepainted(&h);
    UT_ASSERT(!statusDrawHoldEnd(&h));
    statusDrawHoldBegin(&h);
    UT_ASSERT(statusDrawHoldAsk(&h, false));
    UT_ASSERT(statusDrawHoldEnd(&h));

    /* A pass with no draw asked for owes nothing, frame or no frame. */
    statusDrawHoldBegin(&h);
    UT_ASSERT(!statusDrawHoldEnd(&h));
    statusDrawHoldBegin(&h);
    statusDrawHoldRepainted(&h);
    UT_ASSERT(!statusDrawHoldEnd(&h));

    /* While the map overview owns the window a draw is not held, and none
       is owed for it. */
    statusDrawHoldBegin(&h);
    UT_ASSERT(!statusDrawHoldAsk(&h, true));
    UT_ASSERT(!statusDrawHoldEnd(&h));

    /* Leaving the loop part way through a pass drops what was held. */
    statusDrawHoldBegin(&h);
    UT_ASSERT(statusDrawHoldAsk(&h, false));
    statusDrawHoldReset(&h);
    UT_ASSERT(!statusDrawHoldAsk(&h, false));
    UT_ASSERT(!statusDrawHoldEnd(&h));
    return 0;
}

/* A key for text in the test's first renderer and face, at 13 pixels, in
   grey. The addresses are only compared, never read. */
static int tc_rendererA, tc_rendererB, tc_fontA, tc_fontB;

static TextCacheKey tc_key(const char *str) {
    TextCacheKey k;
    k.renderer = &tc_rendererA;
    k.font = &tc_fontA;
    k.fontSize = 13.0f;
    k.r = 200;
    k.g = 200;
    k.b = 200;
    k.a = 255;
    k.str = str;
    return k;
}

int run_text_cache_slot_pick(void) {
    TextCacheEntry slots[4];
    TextCacheKey want;
    bool hit = true;
    char seven[] = "7"; /* the same text at another address */
    int i;

    /* No slots: nothing to pick. */
    want = tc_key("7");
    UT_ASSERT(textCachePick(NULL, 0, &want, &hit) == -1 && !hit);

    /* All empty: the first slot, and not a hit. */
    memset(slots, 0, sizeof(slots));
    hit = true;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 0 && !hit);

    /* An empty slot is taken before a used one, however old the used one. */
    slots[0].key = tc_key("0");
    slots[0].lastUsed = 1;
    slots[1].key = tc_key("1");
    slots[1].lastUsed = 2;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 2 && !hit);
    memset(&slots[0], 0, sizeof(slots[0]));
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 0 && !hit);

    /* All used: the one drawn longest ago, wherever it is. */
    for (i = 0; i < 4; i++) {
        static const char *const text[4] = { "0", "1", "2", "3" };
        slots[i].key = tc_key(text[i]);
    }
    slots[0].lastUsed = 9;
    slots[1].lastUsed = 7;
    slots[2].lastUsed = 3;
    slots[3].lastUsed = 8;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 2 && !hit);
    slots[2].lastUsed = 10;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 1 && !hit);

    /* The same text is a hit, by its characters and not its address, even
       in the slot drawn longest ago. */
    slots[1].key = tc_key("7");
    want = tc_key(seven);
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 1 && hit);

    /* Another renderer, face, size or colour is not a hit: the slot drawn
       longest ago goes, which here is another one. */
    slots[1].lastUsed = 20;
    want = tc_key("7");
    want.renderer = &tc_rendererB;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 3 && !hit);
    want = tc_key("7");
    want.font = &tc_fontB;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 3 && !hit);
    want = tc_key("7");
    want.fontSize = 26.0f;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 3 && !hit);
    want = tc_key("7");
    want.a = 128;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 3 && !hit);
    want = tc_key("7");
    want.g = 0;
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 3 && !hit);

    /* Unchanged, it is a hit again. */
    want = tc_key("7");
    UT_ASSERT(textCachePick(slots, 4, &want, &hit) == 1 && hit);
    return 0;
}
