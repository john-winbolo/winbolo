/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The names and the panel parser the log viewer compiles in.
 *
 * The standalone and wasm viewers link no bolo_static, so they build
 * sim_rules.c and scenario_panel.c from source. This case calls the
 * entry points the viewer uses through public headers only: a rule's
 * name from its index and back, and a two-primitive display list.
 *
 * The list is written out by hand, not built with scnPanelWrite, so the
 * bytes say what travels rather than what the writer happens to emit.
 */
#include <stdint.h>
#include <string.h>

#include "scenario_panel.h"
#include "sim_rules_names.h"
#include "test_harness.h"

/* rect: x=1, y=2, w=3, h=4, colour=RED(5), fill=0
 * line: x0=0, y0=0, x1=127, y1=127, colour=WHITE(2)
 * Total: 7 + 6 = 13 bytes. */
static const uint8_t kTwoPrimitives[] = {
    0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00,
    0x02, 0x00, 0x00, 0x7F, 0x7F, 0x02
};

int run_lv_rule_names_and_panel_parse(void) {
    const char *name;
    ScnPanelList list;

    name = simRulesRuleName(0);
    UT_ASSERT(name != NULL);
    UT_ASSERT(strcmp(name, "tank_reload_ticks") == 0);
    UT_ASSERT(simRulesRuleIndex("tank_reload_ticks") == 0);

    UT_ASSERT(sizeof(kTwoPrimitives) == 13);
    UT_ASSERT(scnPanelParse(kTwoPrimitives, (uint16_t)sizeof(kTwoPrimitives),
                            &list) == SCN_PANEL_OK);
    UT_ASSERT(list.count == 2);
    UT_ASSERT(list.items[0].op == SCN_PANEL_OP_RECT);
    UT_ASSERT(list.items[1].op == SCN_PANEL_OP_LINE);

    return 0;
}
