#ifndef GH_PT2_OLD_H
#define GH_PT2_OLD_H

/*********************************************************
 * GoalHunter pill take v2 evaluator, C port
 *
 * gh_pt2_old.evaluate(inp, V2) is pilltake_v2_old.lua's M.evaluate_lua(inp) in C:
 * the ring search, the shell rays (shadow.lua cast / tank_shot), the chain
 * and cover tests, the blocker search, leeway, the rank sorts and the hold
 * point. Every map and cost read still goes through the functions in inp
 * and inp.env, called in the same order as the Lua does, so the result
 * and every side effect match the Lua version. V2 is the pilltake_v2_old
 * module (for V2.pill_name).
 *
 * pilltake_v2_old.lua calls it when the knob PT2_C_EVAL is on and the global
 * gh_pt2_old exists; an older exe has no gh_pt2_old and the Lua version runs.
 *********************************************************/

#include <lua.h>

/* Registers the gh_pt2_old global. Call once per lua_State. */
void naPt2OldRegister(lua_State *L);

/* Test hook: one shell walk with the given shell numbers, for the check
 * against the engine's shell (see gh_pt2_old.c). */
int naPt2OldTestWalk(int kind, const double sh[5], double sx, double sy, double tx, double ty,
                  double *angle, int *mx, int *my, int *t, int max, int *hit_t);

#endif /* GH_PT2_OLD_H */
