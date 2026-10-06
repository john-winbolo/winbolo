/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Memory figures for the dedicated server's memory report: how big each
 * bot's Lua heap is, how many items sit in the world's object lists, and
 * (on request only) which of a bot's Lua tables hold the most entries.
 *
 * Every call reads a bot's lua_State, so the caller must hold the server
 * mutex and call between ticks, when no bot-think worker is running. */

#ifndef BOT_MEM_REPORT_H
#define BOT_MEM_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "global.h"

struct ServerSim;
struct GameSim;

/* Items in a sim's world lists. Each is a linked list that grows while
 * things happen and shrinks as they expire, so the counts should stay
 * small and flat in a long game. */
typedef struct {
  uint32_t shells;
  uint32_t explosions;
  uint32_t tankExplosions;
  uint32_t minesExplosions;
  uint32_t floodFill;
  uint32_t building;
  uint32_t rubble;
  uint32_t swamp;
  uint32_t grass;
} BotMemListCounts;

/* Count the items in each of the sim's world lists. */
void botMemCountLists(const struct GameSim *gs, BotMemListCounts *out);

/* Add b's counts to a's. */
void botMemAddLists(BotMemListCounts *a, const BotMemListCounts *b);

/* The bot's Lua heap in bytes (lua_gc LUA_GCCOUNT / LUA_GCCOUNTB).
 * Returns false when playerNum has no running brain. */
bool botMemLuaHeapBytes(struct ServerSim *sim, BYTE playerNum,
                        size_t *outBytes);

/* Sum of the world-list counts of every running bot's own ClientSim.
 * Returns the number of bots counted. */
int botMemSumBotClientLists(struct ServerSim *sim, BotMemListCounts *out);

/* Walk every table reachable from the bot's Lua registry (globals,
 * loaded modules, function upvalues, metatables) and write the topN
 * tables with the most entries to `emit`, one line each, as
 * "<entries> entries, <KB> KB of strings: <path>". Expensive: it visits
 * the whole heap and builds a visited set inside the bot's own state,
 * so it is for the on-demand deep report only. Returns false when
 * playerNum has no running brain. */
bool botMemLuaDeepWalk(struct ServerSim *sim, BYTE playerNum, int topN,
                       void (*emit)(void *ctx, const char *line),
                       void *ctx);

#endif /* BOT_MEM_REPORT_H */
