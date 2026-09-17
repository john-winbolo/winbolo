/*
 * The description beside each scenario function: one line per row of the
 * catalogue, for the list the editor shows them in.
 *
 * What this case can hold is the shape — that there is a description for
 * every function and none for anything else, and that the ids are the block
 * lang.h set aside. It cannot hold the English: test_stubs.c:38 answers
 * every langGetText in this binary with a fixed "?", because lang.c is not
 * linked here. So an in-range row is checked for being a string that is not
 * the empty one an out-of-range row answers, which is the difference the
 * accessor is making, and the sentences themselves are read by a person.
 *
 * run_scenario_fndesc_table
 *      — a description per catalogue row and no more, and the ids behind
 *        them distinct and inside the block
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "scenario_lua.h"                /* the catalogue and its two lists */
#include "lang.h"                        /* STR_SCNFN_DESC_* */
#include "mapeditor_scenario_fndesc.h"   /* meScnFnDescription */
#include "test_harness.h"

/* The ids the description table is built from, expanded here off the same
 * two lists in the same order, so this is that list read a second way
 * rather than a second list of ids. */
static const langid kSfdIds[] = {
#define SFD_HOOK_ID(id, name, kind, params) STR_SCNFN_DESC_##id,
    SCN_HOOK_LIST(SFD_HOOK_ID)
#undef SFD_HOOK_ID
#define SFD_POLICY_ID(id, name, params, returns) STR_SCNFN_DESC_##id,
    SCN_POLICY_LIST(SFD_POLICY_ID)
#undef SFD_POLICY_ID
};

#define SFD_ID_COUNT (sizeof(kSfdIds) / sizeof(kSfdIds[0]))

/* The ends of the block lang.h set aside for these. */
#define SFD_ID_FIRST 2435
#define SFD_ID_LAST  2469

int run_scenario_fndesc_table(void) {
    size_t count = 0;
    size_t i;
    size_t j;

    (void)scenarioLuaFunctions(&count);
    UT_ASSERT_MSG(count == SFD_ID_COUNT,
                  "the catalogue holds %d rows and there are %d description "
                  "ids", (int)count, (int)SFD_ID_COUNT);

    /* Every row has one. A row the table is too short for would answer the
       empty string here, the same as a row off the end. */
    for (i = 0; i < count; i++) {
        const char *desc = meScnFnDescription(i);

        UT_ASSERT_MSG(desc != NULL, "row %d answered nothing at all", (int)i);
        UT_ASSERT_MSG(desc[0] != '\0',
                      "row %d has no description; it answered the empty "
                      "string a row past the end answers", (int)i);
    }

    /* And nothing past them, at the first row off the end and well beyond
       it. The second reads no further into the table than the first, and is
       here because an accessor that compared with the wrong operator would
       pass the first on its own. */
    UT_ASSERT_MSG(meScnFnDescription(count)[0] == '\0',
                  "row %d is past the catalogue and answered a description",
                  (int)count);
    UT_ASSERT_MSG(meScnFnDescription(count + 1000)[0] == '\0',
                  "a row well past the catalogue answered a description");

    /* The block is where lang.h says it is, and each id is its own. Two
       rows sharing one id would show the same line beside two functions. */
    UT_ASSERT_MSG((int)STR_SCNFN_DESC_SETUP == SFD_ID_FIRST,
                  "the block starts at %d, expected %d",
                  (int)STR_SCNFN_DESC_SETUP, SFD_ID_FIRST);
    UT_ASSERT_MSG((int)STR_SCNFN_DESC_DAMAGE_SCALE == SFD_ID_LAST,
                  "the block ends at %d, expected %d",
                  (int)STR_SCNFN_DESC_DAMAGE_SCALE, SFD_ID_LAST);

    for (i = 0; i < SFD_ID_COUNT; i++) {
        UT_ASSERT_MSG((int)kSfdIds[i] >= SFD_ID_FIRST &&
                          (int)kSfdIds[i] <= SFD_ID_LAST,
                      "the id at row %d is %d, outside %d to %d", (int)i,
                      (int)kSfdIds[i], SFD_ID_FIRST, SFD_ID_LAST);
        for (j = 0; j < i; j++) {
            UT_ASSERT_MSG(kSfdIds[j] != kSfdIds[i],
                          "rows %d and %d both read id %d", (int)j, (int)i,
                          (int)kSfdIds[i]);
        }
    }

    return 0;
}
