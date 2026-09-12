/*
 * The init table a bot is created with, and the -bot-init text that
 * fills one.
 *
 * A bot's configuration travels as a parameter of the call that makes
 * its brain VM (serverSimCreateBot -> botManagerAddBot ->
 * luaBrainInstanceCreate), and brainCoreSetInitTable is the step that
 * puts it in front of the brain as the BRAIN_INIT global. That last
 * step is what these tests drive: the unit binary stubs the brain
 * create (test_stubs.c) rather than loading a brain from disk, so the
 * fixture brain here is a Lua chunk run on a VM of the test's own,
 * reading BRAIN_INIT the way a real brain's init does and writing what
 * it saw into a global the test reads back.
 *
 * Three tests:
 *   - bot_init_table_two_bots_keep_own:
 *       two VMs are handed different tables before either brain runs,
 *       and each brain sees its own. This is the property the staged
 *       consume-once global could not offer — whoever staged last
 *       before a create won.
 *   - bot_init_table_empty_when_none:
 *       no table (NULL) leaves BRAIN_INIT an empty table rather than
 *       nil, so a brain can read a key off it without a type check.
 *   - bot_init_arg_text_to_table:
 *       the mapping -bot-init's [arg] suffix goes through: ';'
 *       separated pairs, a bare word as the value "1", the caps the
 *       table refuses past, and the log form round-tripping.
 */

#include <stdio.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "test_harness.h"
#include "braincore.h"
#include "scenario_table.h"

/* The fixture brain: reads its own BRAIN_INIT and writes the pairs it
 * saw, sorted so the string is stable, into BRAIN_INIT_SEEN. */
static const char *FIXTURE_BRAIN =
    "local t = rawget(_G, 'BRAIN_INIT')\n"
    "if type(t) ~= 'table' then BRAIN_INIT_SEEN = 'not-a-table' return end\n"
    "local keys = {}\n"
    "for k in pairs(t) do keys[#keys + 1] = k end\n"
    "table.sort(keys)\n"
    "local out = {}\n"
    "for _, k in ipairs(keys) do out[#out + 1] = k .. '=' .. tostring(t[k]) end\n"
    "BRAIN_INIT_SEEN = table.concat(out, ';')\n";

/* Runs the fixture brain on L and copies BRAIN_INIT_SEEN into out.
 * Returns false if the chunk failed or left no string. */
static bool fixture_run(lua_State *L, char *out, size_t outCap) {
    const char *seen;

    out[0] = '\0';
    if (luaL_dostring(L, FIXTURE_BRAIN) != 0) return false;
    lua_getglobal(L, "BRAIN_INIT_SEEN");
    seen = lua_tostring(L, -1);
    if (seen == NULL) { lua_pop(L, 1); return false; }
    snprintf(out, outCap, "%s", seen);
    lua_pop(L, 1);
    return true;
}

int run_bot_init_table_two_bots_keep_own(void) {
    lua_State *a;
    lua_State *b;
    ScnTable initA;
    ScnTable initB;
    char seen[256];

    scnTableClear(&initA);
    scnTableClear(&initB);
    UT_ASSERT(scnTableSet(&initA, "role", "scout"));
    UT_ASSERT(scnTableSet(&initA, "deprive", "100"));
    UT_ASSERT(scnTableSet(&initB, "role", "captain"));

    a = luaL_newstate();
    b = luaL_newstate();
    UT_ASSERT(a != NULL && b != NULL);
    luaL_openlibs(a);
    luaL_openlibs(b);

    /* Both VMs are handed their table before either brain runs, which
     * is where a consume-once stage would have lost the first one. */
    brainCoreSetInitTable(a, &initA);
    brainCoreSetInitTable(b, &initB);

    UT_ASSERT(fixture_run(a, seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "deprive=100;role=scout") == 0,
                  "first bot saw '%s'", seen);
    UT_ASSERT(fixture_run(b, seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "role=captain") == 0,
                  "second bot saw '%s'", seen);

    /* And the other order reads the same, so nothing was consumed. */
    UT_ASSERT(fixture_run(b, seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "role=captain") == 0,
                  "second bot re-read '%s'", seen);
    UT_ASSERT(fixture_run(a, seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "deprive=100;role=scout") == 0,
                  "first bot re-read '%s'", seen);

    lua_close(a);
    lua_close(b);
    return 0;
}

int run_bot_init_table_empty_when_none(void) {
    lua_State *L;
    ScnTable empty;
    char seen[256];

    L = luaL_newstate();
    UT_ASSERT(L != NULL);
    luaL_openlibs(L);

    /* No table at all. */
    brainCoreSetInitTable(L, NULL);
    UT_ASSERT(fixture_run(L, seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "") == 0, "NULL init gave '%s'", seen);

    /* A brain reading a key off it gets nil rather than an error. */
    UT_ASSERT_MSG(luaL_dostring(L,
                      "BRAIN_INIT_MISSING = (BRAIN_INIT.nosuchkey == nil)\n") == 0,
                  "reading a missing key raised an error");
    lua_getglobal(L, "BRAIN_INIT_MISSING");
    UT_ASSERT(lua_toboolean(L, -1));
    lua_pop(L, 1);

    /* A table with no pairs reads the same as no table. */
    scnTableClear(&empty);
    brainCoreSetInitTable(L, &empty);
    UT_ASSERT(fixture_run(L, seen, sizeof(seen)));
    UT_ASSERT_MSG(strcmp(seen, "") == 0, "empty init gave '%s'", seen);

    lua_close(L);
    return 0;
}

int run_bot_init_arg_text_to_table(void) {
    ScnTable t;
    char longKey[SCN_TABLE_KEY_LEN + 8];
    char text[512];
    char formatted[512];
    int i;

    /* The shape the flag's syntax describes: [ammoless;deprive=100]. */
    UT_ASSERT(scnTableFromArgText("ammoless;deprive=100", &t));
    UT_ASSERT_MSG(t.count == 2, "expected 2 pairs, got %d", (int)t.count);
    UT_ASSERT(scnTableGet(&t, "ammoless") != NULL);
    UT_ASSERT_MSG(strcmp(scnTableGet(&t, "ammoless"), "1") == 0,
                  "bare token should be the value 1, got '%s'",
                  scnTableGet(&t, "ammoless"));
    UT_ASSERT(scnTableGet(&t, "deprive") != NULL);
    UT_ASSERT(strcmp(scnTableGet(&t, "deprive"), "100") == 0);
    UT_ASSERT(scnTableGet(&t, "normal") == NULL);

    /* Spaces are dropped and ',' separates as well as ';', so a suffix
     * written either way reaches the brain the same. */
    UT_ASSERT(scnTableFromArgText(" role = scout , deprive=100 ", &t));
    UT_ASSERT_MSG(t.count == 2, "expected 2 pairs, got %d", (int)t.count);
    UT_ASSERT(strcmp(scnTableGet(&t, "role"), "scout") == 0);

    /* Empty tokens are skipped, not stored. */
    UT_ASSERT(scnTableFromArgText("a;;b", &t));
    UT_ASSERT_MSG(t.count == 2, "expected 2 pairs, got %d", (int)t.count);

    /* A repeated key is one pair holding the last value. */
    UT_ASSERT(scnTableFromArgText("x=1;x=2", &t));
    UT_ASSERT_MSG(t.count == 1, "expected 1 pair, got %d", (int)t.count);
    UT_ASSERT(strcmp(scnTableGet(&t, "x"), "2") == 0);

    /* No text is an empty table, not a failure. */
    UT_ASSERT(scnTableFromArgText(NULL, &t));
    UT_ASSERT(t.count == 0);
    UT_ASSERT(scnTableFromArgText("", &t));
    UT_ASSERT(t.count == 0);

    /* A key past the field refuses that pair and says so, keeping the
     * rest of the line. */
    memset(longKey, 'k', sizeof(longKey) - 1);
    longKey[sizeof(longKey) - 1] = '\0';
    snprintf(text, sizeof(text), "%s=1;role=scout", longKey);
    UT_ASSERT(!scnTableFromArgText(text, &t));
    UT_ASSERT_MSG(t.count == 1, "expected the one pair that fits, got %d",
                  (int)t.count);
    UT_ASSERT(scnTableGet(&t, "role") != NULL);

    /* One past SCN_TABLE_MAX pairs is refused, and the table holds the
     * first SCN_TABLE_MAX. */
    text[0] = '\0';
    {
        size_t at = 0;
        for (i = 0; i <= SCN_TABLE_MAX; i++) {
            int n = snprintf(text + at, sizeof(text) - at, "%sk%d=%d",
                             (i > 0 ? ";" : ""), i, i);
            if (n <= 0) break;
            at += (size_t)n;
        }
    }
    UT_ASSERT(!scnTableFromArgText(text, &t));
    UT_ASSERT_MSG(t.count == SCN_TABLE_MAX, "expected %d pairs, got %d",
                  SCN_TABLE_MAX, (int)t.count);
    UT_ASSERT(scnTableGet(&t, "k0") != NULL);
    UT_ASSERT(scnTableGet(&t, "k16") == NULL);

    /* The log form the servers print round-trips back to the same
     * table. */
    UT_ASSERT(scnTableFromArgText("ammoless;deprive=100", &t));
    scnTableFormat(&t, formatted, sizeof(formatted));
    UT_ASSERT_MSG(strcmp(formatted, "ammoless=1;deprive=100") == 0,
                  "formatted as '%s'", formatted);
    UT_ASSERT(scnTableFromArgText(formatted, &t));
    UT_ASSERT(t.count == 2);
    UT_ASSERT(strcmp(scnTableGet(&t, "deprive"), "100") == 0);

    /* An empty table formats to nothing, which is what the servers
     * test before printing an init= suffix at all. */
    scnTableClear(&t);
    scnTableFormat(&t, formatted, sizeof(formatted));
    UT_ASSERT(formatted[0] == '\0');

    return 0;
}
