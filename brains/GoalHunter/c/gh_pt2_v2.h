#ifndef GH_PT2_V2_H
#define GH_PT2_V2_H

/*********************************************************
 * GoalHunter pill take evaluator v2 (the "strip" evaluator), C port
 *
 * gh_pt2_v2 holds pilltake_v2.lua's M.evaluate_lua in C: the rings, the
 * aim and centre wedges, the masks, the exact rays (shadow.lua cast and
 * tank_shot), the chain and cover tests, the wall search with its memo
 * (PT2_WALL_MEMO), the start check (pt2_start.lua), the extra cover
 * walls, leeway, the rank sorts and the hold point. Every map and cost
 * read still goes through the functions in inp and inp.env, so the
 * result is the one the Lua gives.
 *
 * The evaluate can stop between units of work and go on later
 * (PT2_V2_SPLIT): gh_pt2_v2.begin makes a state, gh_pt2_v2.step runs it
 * until the slicer asks for a yield or the evaluate ends. The Lua
 * wrapper (pilltake_v2.lua M.evaluate_c) does the yield.
 *
 * pilltake_v2.lua calls it when the knob PT2_V2_C is on and the global
 * gh_pt2_v2 exists; an older exe has no gh_pt2_v2 and the Lua runs.
 *********************************************************/

#include <lua.h>

/* Registers the gh_pt2_v2 global. Call once per lua_State. */
void naPt2V2Register(lua_State *L);

#endif /* GH_PT2_V2_H */
