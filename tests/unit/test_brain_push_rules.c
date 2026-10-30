/*
 * Tests for the info.rules table that brainCorePushInfo in
 * src/bolo/braincore.c writes for each think (issue #469).
 *
 * The rules change only when a scenario changes them, so the push writes
 * info.rules again only when the rules differ from the copy it wrote last.
 * info.mods and info.reload_ticks are still written on every push.
 *
 *   brain_push_rules_written_on_change:
 *     pushes the same rules twice, then changed rules. A value that a
 *     test puts in info.rules between the first two pushes stays, which
 *     shows the second push did not write the table. The third push
 *     writes the changed value. mods and reload_ticks follow every push.
 */

#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "test_harness.h"
#include "brain.h"
#include "braincore.h"

/* Push info and read info.<table>.<field> as an integer. Leaves the
 * stack as it was. */
static lua_Integer push_and_read(lua_State *L, const BrainInfo *info,
                                 const char *table, const char *field) {
    lua_Integer v;
    brainCorePushInfo(L, info);
    if (table != NULL) {
        lua_getfield(L, -1, table);
        lua_getfield(L, -1, field);
        v = lua_tointeger(L, -1);
        lua_pop(L, 3);
    } else {
        lua_getfield(L, -1, field);
        v = lua_tointeger(L, -1);
        lua_pop(L, 2);
    }
    return v;
}

/* Set info.rules.<field> = value, as a brain could. */
static void poke_rule(lua_State *L, const BrainInfo *info,
                      const char *field, lua_Integer value) {
    brainCorePushInfo(L, info);
    lua_getfield(L, -1, "rules");
    lua_pushinteger(L, value);
    lua_setfield(L, -2, field);
    lua_pop(L, 2);
}

int run_brain_push_rules_written_on_change(void) {
    static BrainInfo info;
    lua_State *L = luaL_newstate();
    UT_ASSERT_MSG(L != NULL, "luaL_newstate failed");
    luaL_openlibs(L);

    memset(&info, 0, sizeof(info));
    info.rules.tank_full_shells = 40;
    info.rules.pill_range       = 2048;
    info.mods.speed   = 100;
    info.reload_ticks = 13;

    /* First push writes the rules. */
    UT_ASSERT_MSG(push_and_read(L, &info, "rules", "tank_full_shells") == 40,
                  "first push did not write tank_full_shells");
    UT_ASSERT_MSG(push_and_read(L, &info, "rules", "pill_range") == 2048,
                  "first push did not write pill_range");

    /* Same rules: the push leaves info.rules as it is. */
    poke_rule(L, &info, "tank_full_shells", 999);
    UT_ASSERT_MSG(push_and_read(L, &info, "rules", "tank_full_shells") == 999,
                  "push with the same rules wrote info.rules again");

    /* mods and reload_ticks follow every push. */
    info.mods.speed   = 150;
    info.reload_ticks = 9;
    UT_ASSERT_MSG(push_and_read(L, &info, "mods", "speed") == 150,
                  "mods.speed did not follow the push");
    UT_ASSERT_MSG(push_and_read(L, &info, NULL, "reload_ticks") == 9,
                  "reload_ticks did not follow the push");

    /* Changed rules: the push writes the whole table again. */
    info.rules.pill_range = 1024;
    UT_ASSERT_MSG(push_and_read(L, &info, "rules", "pill_range") == 1024,
                  "changed pill_range was not written");
    UT_ASSERT_MSG(push_and_read(L, &info, "rules", "tank_full_shells") == 40,
                  "rewrite did not put back tank_full_shells");

    UT_ASSERT_MSG(lua_gettop(L) == 0, "stack not balanced: %d", lua_gettop(L));
    lua_close(L);
    return 0;
}
