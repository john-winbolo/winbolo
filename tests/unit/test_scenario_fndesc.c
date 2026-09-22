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
 * run_scenario_fnstub_forms
 *      — the stub each row hands the author: which kinds carry the trailing
 *        scripted parameter, the shape every row's stub has, and what a row
 *        off the end and a buffer too small answer
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

/* The row a function is on, by the name the catalogue spells it with. The
 * order of the two lists is not this case's business — every check below
 * names the function it is about and asks for its row. */
static size_t sfdRowOf(const char *name) {
    const ScnLuaFnRow *rows;
    size_t             count = 0;
    size_t             i;

    rows = scenarioLuaFunctions(&count);
    for (i = 0; i < count; i++) {
        if (strcmp(rows[i].name, name) == 0) {
            return i;
        }
    }
    return count; /* no such function; the caller's assert says so */
}

/* True when text ends with the line "end" and has no line after it. */
static int sfdEndsWithEnd(const char *text) {
    size_t len = strlen(text);

    return len >= 4 && strcmp(text + len - 4, "\nend") == 0;
}

/* True when the stub's parameter list ends with the scripted flag. Read off
 * the closing parenthesis rather than off the whole text, so a function
 * whose own last parameter were one day called something ending in the same
 * letters could not pass it. */
static int sfdTakesScripted(const char *stub) {
    const char *close = strchr(stub, ')');
    const char *want  = ", scripted";
    size_t      len   = strlen(want);

    if (close == NULL || (size_t)(close - stub) < len) {
        return 0;
    }
    return strncmp(close - len, want, len) == 0;
}

int run_scenario_fnstub_forms(void) {
    char   stub[ME_SCN_FN_STUB_MAX];
    char   short_buffer[ME_SCN_FN_STUB_MAX];
    size_t count = 0;
    size_t want;
    size_t i;

    (void)scenarioLuaFunctions(&count);
    UT_ASSERT_MSG(count > 0, "the catalogue answered no rows at all");

    /* An event hook comes off the queue, where the entry carries the mark
       saying whether the scenario caused the fact, so the host hands it one
       argument more than the catalogue's own names. A stub without it has a
       body reading a flag that is not in its parameter list. */
    want = meScnFnStub(sfdRowOf("on_chat"), stub, sizeof(stub));
    UT_ASSERT_MSG(want > 0 && want < sizeof(stub),
                  "on_chat wanted %d bytes", (int)want);
    UT_ASSERT_MSG(
        strcmp(stub, "function on_chat(p, text, scripted)\nend") == 0,
        "on_chat's stub is \"%s\"", stub);

    /* The three kinds that are handed no such flag. A lifecycle hook is
       made where the round reaches a point of its own, a region hook is a
       difference between two samples of where the tanks are, and a policy
       is a question. None of them has an event behind it. */
    want = meScnFnStub(sfdRowOf("on_tick"), stub, sizeof(stub));
    UT_ASSERT_MSG(strcmp(stub, "function on_tick(tick)\nend") == 0,
                  "on_tick's stub is \"%s\"", stub);
    UT_ASSERT_MSG(!sfdTakesScripted(stub), "on_tick's stub took a flag");

    want = meScnFnStub(sfdRowOf("on_enter_region"), stub, sizeof(stub));
    UT_ASSERT_MSG(strcmp(stub, "function on_enter_region(p, name)\nend") == 0,
                  "on_enter_region's stub is \"%s\"", stub);
    UT_ASSERT_MSG(!sfdTakesScripted(stub),
                  "on_enter_region's stub took a flag");

    want = meScnFnStub(sfdRowOf("can_build"), stub, sizeof(stub));
    UT_ASSERT_MSG(
        strcmp(stub, "function can_build(p, action, x, y, n)\nend") == 0,
        "can_build's stub is \"%s\"", stub);
    UT_ASSERT_MSG(!sfdTakesScripted(stub), "can_build's stub took a flag");

    /* A function that takes nothing has an empty parameter list rather than
       one with a separator or a space left in it. */
    want = meScnFnStub(sfdRowOf("on_setup"), stub, sizeof(stub));
    UT_ASSERT_MSG(strcmp(stub, "function on_setup()\nend") == 0,
                  "on_setup's stub is \"%s\"", stub);

    /* Every row makes something an author can type over: its own name, a
       parameter list however empty, and the line that closes it. */
    for (i = 0; i < count; i++) {
        const char *name = meScnFnName(i);

        want = meScnFnStub(i, stub, sizeof(stub));
        UT_ASSERT_MSG(want > 0 && want < sizeof(stub),
                      "row %d wanted %d bytes, which %d does not hold",
                      (int)i, (int)want, (int)sizeof(stub));
        UT_ASSERT_MSG(strlen(stub) == want,
                      "row %d wrote %d bytes and answered %d",
                      (int)i, (int)strlen(stub), (int)want);
        UT_ASSERT_MSG(name[0] != '\0', "row %d has no name", (int)i);
        UT_ASSERT_MSG(strstr(stub, name) != NULL,
                      "row %d's stub \"%s\" does not name %s", (int)i, stub,
                      name);
        UT_ASSERT_MSG(strchr(stub, '(') != NULL && strchr(stub, ')') != NULL,
                      "row %d's stub \"%s\" has no parameter list", (int)i,
                      stub);
        UT_ASSERT_MSG(sfdEndsWithEnd(stub),
                      "row %d's stub \"%s\" does not end on a line reading "
                      "end", (int)i, stub);
    }

    /* A row off the end writes nothing and says so, at the first one past
       the catalogue and well beyond it. */
    stub[0] = 'x';
    stub[1] = '\0';
    UT_ASSERT_MSG(meScnFnStub(count, stub, sizeof(stub)) == 0,
                  "row %d is past the catalogue and answered a stub",
                  (int)count);
    UT_ASSERT_MSG(stub[0] == '\0', "a row past the end wrote \"%s\"", stub);
    UT_ASSERT_MSG(meScnFnStub(count + 1000, stub, sizeof(stub)) == 0,
                  "a row well past the catalogue answered a stub");

    /* A buffer one byte short of the whole thing. What fits is written and
       terminated, and the answer is what a complete one would have taken,
       which is how a caller tells the two apart. */
    want = meScnFnStub(sfdRowOf("on_tank_spawned"), stub, sizeof(stub));
    UT_ASSERT_MSG(want > 1 && want < sizeof(short_buffer),
                  "on_tank_spawned wanted %d bytes", (int)want);
    short_buffer[0] = 'x';
    UT_ASSERT_MSG(meScnFnStub(sfdRowOf("on_tank_spawned"), short_buffer, want) ==
                      want,
                  "a short buffer changed what the answer said was wanted");
    UT_ASSERT_MSG(strlen(short_buffer) == want - 1,
                  "a buffer of %d holds %d bytes and %d were written",
                  (int)want, (int)want - 1, (int)strlen(short_buffer));
    UT_ASSERT_MSG(strncmp(short_buffer, stub, want - 1) == 0,
                  "the truncated stub \"%s\" is not the start of \"%s\"",
                  short_buffer, stub);

    /* And the smallest buffer of all, which holds the terminator alone. */
    short_buffer[0] = 'x';
    UT_ASSERT_MSG(meScnFnStub(sfdRowOf("on_setup"), short_buffer, 1) > 0,
                  "a one-byte buffer answered that nothing was wanted");
    UT_ASSERT_MSG(short_buffer[0] == '\0', "a one-byte buffer was not terminated");

    /* The parameter list on its own is the same list the stub carries, so
       the row a list draws and the stub it inserts cannot disagree. */
    want = meScnFnParamList(sfdRowOf("on_chat"), short_buffer, sizeof(short_buffer));
    UT_ASSERT_MSG(strcmp(short_buffer, "(p, text, scripted)") == 0,
                  "on_chat's parameter list is \"%s\"", short_buffer);
    UT_ASSERT_MSG(want == strlen(short_buffer), "the list answered %d for %d bytes",
                  (int)want, (int)strlen(short_buffer));

    /* A policy says what it answers and a hook does not, which is what a
       row shows the difference with. */
    UT_ASSERT_MSG(meScnFnReturns(sfdRowOf("can_build"))[0] != '\0',
                  "can_build is a policy and answered no returns line");
    UT_ASSERT_MSG(meScnFnReturns(sfdRowOf("on_chat"))[0] == '\0',
                  "on_chat is a hook and answered a returns line");
    UT_ASSERT_MSG(meScnFnReturns(count)[0] == '\0',
                  "a row past the catalogue answered a returns line");

    /* One count behind the list, the descriptions and the stubs. */
    UT_ASSERT_MSG(meScnFnCount() == count,
                  "the catalogue holds %d rows and the list offers %d",
                  (int)count, (int)meScnFnCount());
    UT_ASSERT_MSG(meScnFnName(count)[0] == '\0',
                  "a row past the catalogue answered a name");

    return 0;
}
