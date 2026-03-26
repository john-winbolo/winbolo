/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Brain Core
 *Filename:      braincore.c
 *Author:        John Morrison
 *Purpose:
 *  Shared Lua brain helpers used by both client and server.
 *  Extracted from luabrainshandler.c — no client globals.
 *********************************************************/

#include <string.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include "global.h"
#include "brain.h"
#include "screen.h"
#include "util.h"
#include "braincore.h"
#include "brain_pathfinder.h"

/* ------------------------------------------------------------------ */
/* clock_us — high-resolution timer for Lua profiling                  */
/* ------------------------------------------------------------------ */

static int l_clock_us(lua_State *L) {
  Uint64 now  = SDL_GetPerformanceCounter();
  Uint64 freq = SDL_GetPerformanceFrequency();
  /* Return microseconds as a Lua number (double has 53 bits of mantissa,
   * good for ~285 years of microseconds before precision loss). */
  double us = (double)now / (double)freq * 1000000.0;
  lua_pushnumber(L, us);
  return 1;
}

/* ------------------------------------------------------------------ */
/* get_terrain C closure with upvalue                                   */
/* ------------------------------------------------------------------ */

static int l_get_terrain_upvalue(lua_State *L) {
  const BYTE **worldPtrPtr = (const BYTE **)lua_touserdata(L, lua_upvalueindex(1));
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  if (worldPtrPtr == NULL || *worldPtrPtr == NULL || x < 0 || x > 255 || y < 0 || y > 255) {
    lua_pushinteger(L, 0);
  } else {
    lua_pushinteger(L, (*worldPtrPtr)[y * 256 + x]);
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Constant registration                                               */
/* ------------------------------------------------------------------ */

void brainCoreRegisterConstants(lua_State *L) {
  /* Key command bits */
  lua_pushinteger(L, 1 << KEY_faster);    lua_setglobal(L, "KEY_FASTER");
  lua_pushinteger(L, 1 << KEY_slower);    lua_setglobal(L, "KEY_SLOWER");
  lua_pushinteger(L, 1 << KEY_turnleft);  lua_setglobal(L, "KEY_TURNLEFT");
  lua_pushinteger(L, 1 << KEY_turnright); lua_setglobal(L, "KEY_TURNRIGHT");
  lua_pushinteger(L, 1 << KEY_morerange); lua_setglobal(L, "KEY_MORERANGE");
  lua_pushinteger(L, 1 << KEY_lessrange); lua_setglobal(L, "KEY_LESSRANGE");
  lua_pushinteger(L, 1 << KEY_shoot);     lua_setglobal(L, "KEY_SHOOT");
  lua_pushinteger(L, 1 << KEY_dropmine);  lua_setglobal(L, "KEY_DROPMINE");
  lua_pushinteger(L, 1 << KEY_TankView);  lua_setglobal(L, "KEY_TANKVIEW");
  lua_pushinteger(L, 1 << KEY_PillView);  lua_setglobal(L, "KEY_PILLVIEW");

  /* Build modes */
  lua_pushinteger(L, BUILDMODE_FARM);     lua_setglobal(L, "BUILDMODE_FARM");
  lua_pushinteger(L, BUILDMODE_ROAD);     lua_setglobal(L, "BUILDMODE_ROAD");
  lua_pushinteger(L, BUILDMODE_BUILD);    lua_setglobal(L, "BUILDMODE_BUILD");
  lua_pushinteger(L, BUILDMODE_PBOX);     lua_setglobal(L, "BUILDMODE_PBOX");
  lua_pushinteger(L, BUILDMODE_MINE);     lua_setglobal(L, "BUILDMODE_MINE");

  /* Terrain type values */
  lua_pushinteger(L, BBUILDING);          lua_setglobal(L, "TERRAIN_BUILDING");
  lua_pushinteger(L, BRIVER);             lua_setglobal(L, "TERRAIN_RIVER");
  lua_pushinteger(L, BSWAMP);             lua_setglobal(L, "TERRAIN_SWAMP");
  lua_pushinteger(L, BCRATER);            lua_setglobal(L, "TERRAIN_CRATER");
  lua_pushinteger(L, BROAD);              lua_setglobal(L, "TERRAIN_ROAD");
  lua_pushinteger(L, BFOREST);            lua_setglobal(L, "TERRAIN_FOREST");
  lua_pushinteger(L, BRUBBLE);            lua_setglobal(L, "TERRAIN_RUBBLE");
  lua_pushinteger(L, BGRASS);             lua_setglobal(L, "TERRAIN_GRASS");
  lua_pushinteger(L, BHALFBUILDING);      lua_setglobal(L, "TERRAIN_HALFBUILDING");
  lua_pushinteger(L, BBOAT);              lua_setglobal(L, "TERRAIN_BOAT");
  lua_pushinteger(L, BDEEPSEA);           lua_setglobal(L, "TERRAIN_DEEPSEA");
  lua_pushinteger(L, BREFBASE_T);         lua_setglobal(L, "TERRAIN_REFBASE");
  lua_pushinteger(L, BPILLBOX_T);         lua_setglobal(L, "TERRAIN_PILLBOX");

  /* Terrain flag bits */
  lua_pushinteger(L, TERRAIN_MASK);       lua_setglobal(L, "TERRAIN_MASK");
  lua_pushinteger(L, TERRAIN_TANK_VIS);   lua_setglobal(L, "TERRAIN_TANK_VIS");
  lua_pushinteger(L, TERRAIN_PILL_VIS);   lua_setglobal(L, "TERRAIN_PILL_VIS");
  lua_pushinteger(L, TERRAIN_MINE);       lua_setglobal(L, "TERRAIN_MINE_FLAG");

  /* Object types */
  lua_pushinteger(L, OBJECT_TANK);        lua_setglobal(L, "OBJECT_TANK");
  lua_pushinteger(L, OBJECT_SHOT);        lua_setglobal(L, "OBJECT_SHOT");
  lua_pushinteger(L, OBJECT_PILLBOX);     lua_setglobal(L, "OBJECT_PILLBOX");
  lua_pushinteger(L, OBJECT_REFBASE);     lua_setglobal(L, "OBJECT_REFBASE");
  lua_pushinteger(L, OBJECT_BUILDMAN);    lua_setglobal(L, "OBJECT_BUILDMAN");
  lua_pushinteger(L, OBJECT_PARACHUTE);   lua_setglobal(L, "OBJECT_PARACHUTE");

  /* Object info flags */
  lua_pushinteger(L, OBJECT_HOSTILE);     lua_setglobal(L, "OBJECT_HOSTILE");
  lua_pushinteger(L, OBJECT_NEUTRAL);     lua_setglobal(L, "OBJECT_NEUTRAL");

  /* Neutral player sentinel */
  lua_pushinteger(L, NEUTRAL_PLAYER);     lua_setglobal(L, "NEUTRAL_PLAYER");

  /* Game event type constants */
  lua_pushinteger(L, EVENT_PILL_CAPTURED);   lua_setglobal(L, "EVENT_PILL_CAPTURED");
  lua_pushinteger(L, EVENT_BASE_CAPTURED);   lua_setglobal(L, "EVENT_BASE_CAPTURED");
  lua_pushinteger(L, EVENT_TANK_KILLED);     lua_setglobal(L, "EVENT_TANK_KILLED");
  lua_pushinteger(L, EVENT_SOUND);           lua_setglobal(L, "EVENT_SOUND");
  lua_pushinteger(L, EVENT_PILL_UPDATE);     lua_setglobal(L, "EVENT_PILL_UPDATE");
  lua_pushinteger(L, EVENT_BASE_UPDATE);     lua_setglobal(L, "EVENT_BASE_UPDATE");
  lua_pushinteger(L, EVENT_PLAYER_LEAVE);    lua_setglobal(L, "EVENT_PLAYER_LEAVE");
  lua_pushinteger(L, EVENT_ASSISTANT_MSG);   lua_setglobal(L, "EVENT_ASSISTANT_MSG");
  lua_pushinteger(L, EVENT_LGM_LOST);        lua_setglobal(L, "EVENT_LGM_LOST");
  lua_pushinteger(L, EVENT_SOUND_TANK_HIT);  lua_setglobal(L, "EVENT_SOUND_TANK_HIT");
  lua_pushinteger(L, EVENT_SOUND_SHOOT);     lua_setglobal(L, "EVENT_SOUND_SHOOT");

  /* Assistant message ID constants */
  lua_pushinteger(L, ASSIST_MSG_MAN_DEAD);           lua_setglobal(L, "ASSIST_MSG_MAN_DEAD");
  lua_pushinteger(L, ASSIST_MSG_NO_TREE);            lua_setglobal(L, "ASSIST_MSG_NO_TREE");
  lua_pushinteger(L, ASSIST_MSG_NO_BUILD);           lua_setglobal(L, "ASSIST_MSG_NO_BUILD");
  lua_pushinteger(L, ASSIST_MSG_NO_BUILD_BOAT);      lua_setglobal(L, "ASSIST_MSG_NO_BUILD_BOAT");
  lua_pushinteger(L, ASSIST_MSG_INSUFFICIENT_TREES);  lua_setglobal(L, "ASSIST_MSG_INSUFFICIENT_TREES");
  lua_pushinteger(L, ASSIST_MSG_BUILDTANK);          lua_setglobal(L, "ASSIST_MSG_BUILDTANK");
  lua_pushinteger(L, ASSIST_MSG_PILL_NO_REPAIR);     lua_setglobal(L, "ASSIST_MSG_PILL_NO_REPAIR");
  lua_pushinteger(L, ASSIST_MSG_NO_PILLS);           lua_setglobal(L, "ASSIST_MSG_NO_PILLS");
  lua_pushinteger(L, ASSIST_MSG_INSUFFICIENT_MINES);  lua_setglobal(L, "ASSIST_MSG_INSUFFICIENT_MINES");
  lua_pushinteger(L, ASSIST_MSG_PILL_ON_MINE);       lua_setglobal(L, "ASSIST_MSG_PILL_ON_MINE");
  lua_pushinteger(L, ASSIST_MSG_TANK_SUNK);          lua_setglobal(L, "ASSIST_MSG_TANK_SUNK");

  /* Sound effect constants */
  lua_pushinteger(L, shootSelf);          lua_setglobal(L, "SND_SHOOT_SELF");
  lua_pushinteger(L, shootNear);          lua_setglobal(L, "SND_SHOOT_NEAR");
  lua_pushinteger(L, shotTreeNear);       lua_setglobal(L, "SND_SHOT_TREE_NEAR");
  lua_pushinteger(L, shotTreeFar);        lua_setglobal(L, "SND_SHOT_TREE_FAR");
  lua_pushinteger(L, shotBuildingNear);   lua_setglobal(L, "SND_SHOT_BUILDING_NEAR");
  lua_pushinteger(L, shotBuildingFar);    lua_setglobal(L, "SND_SHOT_BUILDING_FAR");
  lua_pushinteger(L, hitTankNear);        lua_setglobal(L, "SND_HIT_TANK_NEAR");
  lua_pushinteger(L, hitTankFar);         lua_setglobal(L, "SND_HIT_TANK_FAR");
  lua_pushinteger(L, hitTankSelf);        lua_setglobal(L, "SND_HIT_TANK_SELF");
  lua_pushinteger(L, bubbles);            lua_setglobal(L, "SND_BUBBLES");
  lua_pushinteger(L, tankSinkNear);       lua_setglobal(L, "SND_TANK_SINK_NEAR");
  lua_pushinteger(L, tankSinkFar);        lua_setglobal(L, "SND_TANK_SINK_FAR");
  lua_pushinteger(L, bigExplosionNear);   lua_setglobal(L, "SND_BIG_EXPLOSION_NEAR");
  lua_pushinteger(L, bigExplosionFar);    lua_setglobal(L, "SND_BIG_EXPLOSION_FAR");
  lua_pushinteger(L, farmingTreeNear);    lua_setglobal(L, "SND_FARMING_TREE_NEAR");
  lua_pushinteger(L, farmingTreeFar);     lua_setglobal(L, "SND_FARMING_TREE_FAR");
  lua_pushinteger(L, manBuildingNear);    lua_setglobal(L, "SND_MAN_BUILDING_NEAR");
  lua_pushinteger(L, manBuildingFar);     lua_setglobal(L, "SND_MAN_BUILDING_FAR");
  lua_pushinteger(L, manDyingNear);       lua_setglobal(L, "SND_MAN_DYING_NEAR");
  lua_pushinteger(L, manDyingFar);        lua_setglobal(L, "SND_MAN_DYING_FAR");
  lua_pushinteger(L, manLayingMineNear);  lua_setglobal(L, "SND_MAN_LAYING_MINE_NEAR");
  lua_pushinteger(L, mineExplosionNear);  lua_setglobal(L, "SND_MINE_EXPLOSION_NEAR");
  lua_pushinteger(L, mineExplosionFar);   lua_setglobal(L, "SND_MINE_EXPLOSION_FAR");
  lua_pushinteger(L, shootFar);           lua_setglobal(L, "SND_SHOOT_FAR");

  /* High-resolution timer for profiling */
  lua_pushcfunction(L, l_clock_us);       lua_setglobal(L, "clock_us");
}

void brainCoreRegisterGetTerrain(lua_State *L, const BYTE **worldPtr) {
  lua_pushlightuserdata(L, (void *)worldPtr);
  lua_pushcclosure(L, l_get_terrain_upvalue, 1);
  lua_setglobal(L, "get_terrain");
}

/* ------------------------------------------------------------------ */
/* BrainInfo marshaling                                                */
/* ------------------------------------------------------------------ */

void brainCorePushInfo(lua_State *L, const BrainInfo *info) {
  int i;

  lua_newtable(L);

  /* Version */
  lua_pushinteger(L, info->BoloVersion);    lua_setfield(L, -2, "bolo_version");
  lua_pushinteger(L, info->InfoVersion);    lua_setfield(L, -2, "info_version");

  /* Player roster */
  lua_pushinteger(L, info->player_number);  lua_setfield(L, -2, "player_number");
  lua_pushinteger(L, info->num_players);    lua_setfield(L, -2, "num_players");
  lua_pushinteger(L, info->max_players);    lua_setfield(L, -2, "max_players");
  lua_pushinteger(L, info->max_pillboxes);  lua_setfield(L, -2, "max_pillboxes");
  lua_pushinteger(L, info->max_refbases);   lua_setfield(L, -2, "max_refbases");

  /* Player names */
  lua_newtable(L);
  if (info->playernames != NULL) {
    const u_char *base = (const u_char *)info->playernames;
    for (i = 0; i < info->max_players; i++) {
      const u_char *ps  = base + (size_t)i * PLAYER_NAME_LEN;
      int           len = (int)ps[0];
      if (len > 0 && len < PLAYER_NAME_LEN) {
        lua_pushlstring(L, (const char *)(ps + 1), (size_t)len);
      } else {
        lua_pushstring(L, "");
      }
      lua_rawseti(L, -2, i + 1);
    }
  }
  lua_setfield(L, -2, "player_names");

  /* Alliance bitmask */
  lua_pushinteger(L, info->allies ? *(info->allies) : 0);
  lua_setfield(L, -2, "allies");

  /* Tank state */
  lua_pushinteger(L, info->tankx);          lua_setfield(L, -2, "tankx");
  lua_pushinteger(L, info->tanky);          lua_setfield(L, -2, "tanky");
  lua_pushinteger(L, info->direction);      lua_setfield(L, -2, "direction");
  lua_pushinteger(L, info->speed);          lua_setfield(L, -2, "speed");
  lua_pushinteger(L, info->inboat);          lua_setfield(L, -2, "inboat");
  lua_pushinteger(L, info->hidden);          lua_setfield(L, -2, "hidden");
  lua_pushinteger(L, info->shells);         lua_setfield(L, -2, "shells");
  lua_pushinteger(L, info->mines);          lua_setfield(L, -2, "mines");
  lua_pushinteger(L, info->armour);         lua_setfield(L, -2, "armour");
  lua_pushinteger(L, info->trees);          lua_setfield(L, -2, "trees");
  lua_pushinteger(L, info->carriedpills);   lua_setfield(L, -2, "carried_pills");
  lua_pushinteger(L, info->carriedbases);   lua_setfield(L, -2, "carried_bases");
  lua_pushinteger(L, info->gunrange);       lua_setfield(L, -2, "gunrange");
  lua_pushboolean(L, info->reload != 0);    lua_setfield(L, -2, "reload");
  lua_pushboolean(L, info->newtank != 0);   lua_setfield(L, -2, "newtank");
  lua_pushboolean(L, info->tankobstructed != 0); lua_setfield(L, -2, "tank_obstructed");

  /* Nearest friendly base, or nil */
  if (info->base != NULL) {
    lua_newtable(L);
    lua_pushinteger(L, info->base->x >> 8); lua_setfield(L, -2, "x");
    lua_pushinteger(L, info->base->y >> 8); lua_setfield(L, -2, "y");
    lua_pushinteger(L, info->base->idnum);  lua_setfield(L, -2, "idnum");
    lua_pushinteger(L, info->base_shells);  lua_setfield(L, -2, "shells");
    lua_pushinteger(L, info->base_mines);   lua_setfield(L, -2, "mines");
    lua_pushinteger(L, info->base_armour);  lua_setfield(L, -2, "armour");
  } else {
    lua_pushnil(L);
  }
  lua_setfield(L, -2, "base");

  /* Builder man (LGM) state */
  lua_pushinteger(L, info->man_status);     lua_setfield(L, -2, "man_status");
  lua_pushinteger(L, info->man_direction);  lua_setfield(L, -2, "man_direction");
  lua_pushinteger(L, info->man_x);          lua_setfield(L, -2, "man_x");
  lua_pushinteger(L, info->man_y);          lua_setfield(L, -2, "man_y");
  lua_pushinteger(L, info->manobstructed);  lua_setfield(L, -2, "man_obstructed");

  /* View rectangle */
  lua_pushinteger(L, info->view_top);       lua_setfield(L, -2, "view_top");
  lua_pushinteger(L, info->view_left);      lua_setfield(L, -2, "view_left");
  lua_pushinteger(L, info->view_height);    lua_setfield(L, -2, "view_height");
  lua_pushinteger(L, info->view_width);     lua_setfield(L, -2, "view_width");

  /* pillview */
  lua_pushinteger(L, info->pillview ? *(info->pillview) : 0x8000);
  lua_setfield(L, -2, "pillview");

  /* viewdata */
  if (info->viewdata != NULL) {
    lua_pushlstring(L, (const char *)info->viewdata,
                    (size_t)(info->view_width * info->view_height));
  } else {
    lua_pushstring(L, "");
  }
  lua_setfield(L, -2, "viewdata");

  /* Visible objects */
  lua_newtable(L);
  for (i = 0; i < info->num_objects; i++) {
    lua_newtable(L);
    lua_pushinteger(L, info->objects[i].object);    lua_setfield(L, -2, "type");
    lua_pushinteger(L, info->objects[i].x);         lua_setfield(L, -2, "x");
    lua_pushinteger(L, info->objects[i].y);         lua_setfield(L, -2, "y");
    lua_pushinteger(L, info->objects[i].idnum);     lua_setfield(L, -2, "idnum");
    lua_pushinteger(L, info->objects[i].direction); lua_setfield(L, -2, "direction");
    lua_pushinteger(L, info->objects[i].info);      lua_setfield(L, -2, "info");
    lua_pushinteger(L, info->objects[i].speed);     lua_setfield(L, -2, "speed");
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "objects");

  /* Received message */
  if (info->message != NULL) {
    char msgBuf[256];
    lua_newtable(L);
    lua_pushinteger(L, info->message->sender);
    lua_setfield(L, -2, "sender");
    lua_pushinteger(L, info->message->receivers ? *(info->message->receivers) : 0);
    lua_setfield(L, -2, "receivers");
    if (info->message->message != NULL && info->message->message[0] != 0) {
      utilPtoCString((char *)info->message->message, msgBuf);
      lua_pushstring(L, msgBuf);
    } else {
      lua_pushstring(L, "");
    }
    lua_setfield(L, -2, "text");
  } else {
    lua_pushnil(L);
  }
  lua_setfield(L, -2, "message");

  /* Game info */
  lua_newtable(L);
  lua_pushstring(L, (const char *)info->gameinfo.mapname.c);
  lua_setfield(L, -2, "mapname");
  lua_pushinteger(L, info->gameinfo.gametype);          lua_setfield(L, -2, "gametype");
  lua_pushinteger(L, info->gameinfo.hidden_mines);      lua_setfield(L, -2, "hidden_mines");
  lua_pushboolean(L, info->gameinfo.allow_AI);          lua_setfield(L, -2, "allow_ai");
  lua_pushboolean(L, info->gameinfo.assist_AI);         lua_setfield(L, -2, "assist_ai");
  lua_pushinteger(L, info->gameinfo.start_delay);       lua_setfield(L, -2, "start_delay");
  lua_pushinteger(L, info->gameinfo.time_limit);        lua_setfield(L, -2, "time_limit");
  lua_setfield(L, -2, "gameinfo");

  /* Current key state */
  lua_pushinteger(L, info->holdkeys ? *(info->holdkeys) : 0);
  lua_setfield(L, -2, "holdkeys");
  lua_pushinteger(L, info->tapkeys ? *(info->tapkeys) : 0);
  lua_setfield(L, -2, "tapkeys");

  /* Server tick and assistant message */
  lua_pushinteger(L, info->server_tick);
  lua_setfield(L, -2, "server_tick");
  lua_pushinteger(L, info->assistant_msg);
  lua_setfield(L, -2, "assistant_msg");

  /* Game events */
  lua_newtable(L);
  for (i = 0; i < info->num_events; i++) {
    lua_newtable(L);
    lua_pushinteger(L, info->events[i].type);
    lua_setfield(L, -2, "type");
    lua_newtable(L);
    {
      int dlen = gameEventDataSize(info->events[i].type);
      int j;
      for (j = 0; j < dlen; j++) {
        lua_pushinteger(L, info->events[i].data[j]);
        lua_rawseti(L, -2, j + 1);
      }
    }
    lua_setfield(L, -2, "data");
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "events");
}

/* ------------------------------------------------------------------ */
/* Output extraction                                                   */
/* ------------------------------------------------------------------ */

void brainCoreExtractOutput(lua_State *L, BrainInfo *info) {
  if (!lua_istable(L, -1)) {
    return;
  }

  /* holdkeys */
  lua_getfield(L, -1, "holdkeys");
  if (lua_isinteger(L, -1) && info->holdkeys) {
    *(info->holdkeys) = (uint32_t)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* tapkeys */
  lua_getfield(L, -1, "tapkeys");
  if (lua_isinteger(L, -1) && info->tapkeys) {
    *(info->tapkeys) = (uint32_t)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* build */
  lua_getfield(L, -1, "build");
  if (lua_istable(L, -1) && info->build) {
    lua_getfield(L, -1, "x");
    info->build->x = (MAP_X)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "y");
    info->build->y = (MAP_Y)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "action");
    info->build->action = (BUILDMODE)lua_tointeger(L, -1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  /* wantallies */
  lua_getfield(L, -1, "wantallies");
  if (lua_isinteger(L, -1) && info->wantallies) {
    *(info->wantallies) = (PlayerBitMap)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* messagedest */
  lua_getfield(L, -1, "messagedest");
  if (lua_isinteger(L, -1) && info->messagedest) {
    *(info->messagedest) = (PlayerBitMap)lua_tointeger(L, -1);
  }
  lua_pop(L, 1);

  /* sendmessage */
  lua_getfield(L, -1, "sendmessage");
  if (lua_isstring(L, -1) && info->sendmessage) {
    size_t      len;
    const char *s = lua_tolstring(L, -1, &len);
    if (len > 0 && len <= 253) {
      info->sendmessage[0] = (u_char)len;
      memcpy(info->sendmessage + 1, s, len);
      info->sendmessage[len + 1] = '\0';
    }
  }
  lua_pop(L, 1);
}

/* ------------------------------------------------------------------ */
/* Brain method invocation                                             */
/* ------------------------------------------------------------------ */

bool brainCoreCallThink(lua_State *L, BrainInfo *info) {
  int top = lua_gettop(L);

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    fprintf(stderr, "brainCore: 'brain' global is not a table\n");
    lua_settop(L, top);
    return false;
  }

  lua_getfield(L, -1, "think");
  if (!lua_isfunction(L, -1)) {
    fprintf(stderr, "brainCore: brain.think is not a function\n");
    lua_settop(L, top);
    return false;
  }

  brainCorePushInfo(L, info);

  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    fprintf(stderr, "brainCore: brain.think() error: %s\n", lua_tostring(L, -1));
    lua_settop(L, top);
    return false;
  }

  brainCoreExtractOutput(L, info);
  lua_settop(L, top);
  return true;
}

bool brainCoreCallMethod(lua_State *L, BrainInfo *info, const char *method) {
  int top = lua_gettop(L);

  lua_getglobal(L, "brain");
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    return false;
  }

  lua_getfield(L, -1, method);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    return true; /* optional — not an error */
  }

  brainCorePushInfo(L, info);

  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    fprintf(stderr, "brainCore: brain.%s() error: %s\n",
            method, lua_tostring(L, -1));
    lua_settop(L, top);
    return false;
  }

  lua_settop(L, top);
  return true;
}

/* ------------------------------------------------------------------ */
/* C Pathfinder Lua wrappers (cpf_* globals)                           */
/* ------------------------------------------------------------------ */

/* Helper: extract BrainPathfinder* from upvalue 1 (pointer-to-pointer) */
#define CPF_GET(L) \
  BrainPathfinder **ppf = (BrainPathfinder **)lua_touserdata(L, lua_upvalueindex(1)); \
  BrainPathfinder *pf = (ppf && *ppf) ? *ppf : NULL; \
  if (!pf) return 0

static int l_cpf_set_terrain_cost(lua_State *L) {
  CPF_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float cost = (float)luaL_checknumber(L, 2);
  brainPathfinderSetTerrainCost(pf, type, cost);
  return 0;
}

static int l_cpf_set_boat_cost(lua_State *L) {
  CPF_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float cost = (float)luaL_checknumber(L, 2);
  brainPathfinderSetBoatCost(pf, type, cost);
  return 0;
}

static int l_cpf_set_terrain_speed(lua_State *L) {
  CPF_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float speed = (float)luaL_checknumber(L, 2);
  brainPathfinderSetTerrainSpeed(pf, type, speed);
  return 0;
}

static int l_cpf_set_config(lua_State *L) {
  CPF_GET(L);
  const char *key = luaL_checkstring(L, 1);
  float value = (float)luaL_checknumber(L, 2);
  brainPathfinderSetConfig(pf, key, value);
  return 0;
}

static int l_cpf_clear_danger(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearDanger(pf);
  return 0;
}

static int l_cpf_stamp_pill(lua_State *L) {
  CPF_GET(L);
  int cx = (int)luaL_checkinteger(L, 1);
  int cy = (int)luaL_checkinteger(L, 2);
  int radius = (int)luaL_checkinteger(L, 3);
  float base_danger = (float)luaL_checknumber(L, 4);
  float anger = (float)luaL_checknumber(L, 5);
  brainPathfinderStampPill(pf, cx, cy, radius, base_danger, anger);
  return 0;
}

static int l_cpf_set_danger(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  float value = (float)luaL_checknumber(L, 3);
  brainPathfinderSetDanger(pf, x, y, value);
  return 0;
}

static int l_cpf_set_overlay(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  float value = (float)luaL_checknumber(L, 3);
  brainPathfinderSetOverlay(pf, x, y, value);
  return 0;
}

static int l_cpf_clear_overlay(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearOverlay(pf);
  return 0;
}

static int l_cpf_clear_influence(lua_State *L) {
  CPF_GET(L);
  brainPathfinderClearInfluence(pf);
  return 0;
}

static int l_cpf_stamp_influence(lua_State *L) {
  CPF_GET(L);
  int cx = (int)luaL_checkinteger(L, 1);
  int cy = (int)luaL_checkinteger(L, 2);
  int radius = (int)luaL_checkinteger(L, 3);
  int strength = (int)luaL_checkinteger(L, 4);
  brainPathfinderStampInfluence(pf, cx, cy, radius, strength);
  return 0;
}

static int l_cpf_influence_at(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int16_t value = brainPathfinderInfluenceAt(pf, x, y);
  lua_pushinteger(L, (int)value);
  return 1;
}

static int l_cpf_path_to(lua_State *L) {
  int next_x = -1, next_y = -1;
  int status;
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = lua_toboolean(L, 5);
  int shells = (int)luaL_checkinteger(L, 6);
  int trees = (int)luaL_checkinteger(L, 7);
  int mines = (int)luaL_optinteger(L, 8, 0);
  int armour = (int)luaL_optinteger(L, 9, 40);
  int budget = (int)luaL_optinteger(L, 10, 1500);
  status = brainPathfinderPathTo(pf, sx, sy, dx, dy,
                                  in_boat, shells, trees,
                                  mines, armour, budget,
                                  &next_x, &next_y);
  lua_pushinteger(L, status);
  lua_pushinteger(L, next_x);
  lua_pushinteger(L, next_y);
  return 3;
}

static int l_cpf_cost_to(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
  int shells = (int)luaL_checkinteger(L, 6);
  int trees = (int)luaL_checkinteger(L, 7);
  int mines = (int)luaL_checkinteger(L, 8);
  int armour = (int)luaL_checkinteger(L, 9);
  int budget = (int)luaL_optinteger(L, 10, 4000);
  float cost = brainPathfinderCostTo(pf, sx, sy, dx, dy, in_boat,
                                      shells, trees, mines, armour, budget);
  lua_pushnumber(L, (double)cost);
  return 1;
}

static int l_cpf_estimate_cost(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = lua_toboolean(L, 5);
  float cost = brainPathfinderEstimateCost(pf, sx, sy, dx, dy, in_boat);
  lua_pushnumber(L, (double)cost);
  return 1;
}

static int l_cpf_danger_at(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  float value = brainPathfinderDangerAt(pf, x, y);
  lua_pushnumber(L, (double)value);
  return 1;
}

static int l_cpf_lgm_travel_ticks(lua_State *L) {
  CPF_GET(L);
  WORLD sx = (WORLD)luaL_checkinteger(L, 1);
  WORLD sy = (WORLD)luaL_checkinteger(L, 2);
  WORLD dx = (WORLD)luaL_checkinteger(L, 3);
  WORLD dy = (WORLD)luaL_checkinteger(L, 4);
  BYTE blessX = (BYTE)luaL_checkinteger(L, 5);
  BYTE blessY = (BYTE)luaL_checkinteger(L, 6);
  int maxTicks = (int)luaL_checkinteger(L, 7);
  int stuckTicks = (int)luaL_checkinteger(L, 8);
  int result = brainPathfinderLgmTravelTicks(pf, sx, sy, dx, dy,
                                              blessX, blessY,
                                              maxTicks, stuckTicks);
  lua_pushinteger(L, result);
  return 1;
}

static int l_cpf_lgm_travel_ticks_map(lua_State *L) {
  CPF_GET(L);
  BYTE smx = (BYTE)luaL_checkinteger(L, 1);
  BYTE smy = (BYTE)luaL_checkinteger(L, 2);
  BYTE dmx = (BYTE)luaL_checkinteger(L, 3);
  BYTE dmy = (BYTE)luaL_checkinteger(L, 4);
  BYTE blessX = (BYTE)luaL_checkinteger(L, 5);
  BYTE blessY = (BYTE)luaL_checkinteger(L, 6);
  int maxTicks = (int)luaL_checkinteger(L, 7);
  int stuckTicks = (int)luaL_checkinteger(L, 8);
  int result = brainPathfinderLgmTravelTicksMap(pf, smx, smy, dmx, dmy,
                                                 blessX, blessY,
                                                 maxTicks, stuckTicks);
  lua_pushinteger(L, result);
  return 1;
}

static int l_cpf_estimate_tank_travel_ticks(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = lua_toboolean(L, 5);
  int maxTicks = (int)luaL_optinteger(L, 6, 4000);
  int stuckTicks = (int)luaL_optinteger(L, 7, 200);
  int result = brainPathfinderEstimateTankTravelTicks(pf, sx, sy, dx, dy,
                                                       in_boat,
                                                       maxTicks, stuckTicks);
  lua_pushinteger(L, result);
  return 1;
}

static int l_cpf_find_front_line(lua_State *L) {
  int i;
  int max_points = 512;
  int out_x[512], out_y[512];
  int count;
  CPF_GET(L);
  count = brainPathfinderFindFrontLine(pf, out_x, out_y, max_points);

  lua_createtable(L, count * 2, 0);
  for (i = 0; i < count; i++) {
    lua_pushinteger(L, out_x[i]);
    lua_rawseti(L, -2, i * 2 + 1);
    lua_pushinteger(L, out_y[i]);
    lua_rawseti(L, -2, i * 2 + 2);
  }
  return 1;
}

static int l_cpf_trace_path(lua_State *L) {
  CPF_GET(L);
  int path_x[64], path_y[64];
  int count = brainPathfinderTracePath(pf, path_x, path_y, 64);
  lua_createtable(L, count, 0);
  for (int i = 0; i < count; i++) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, path_x[i]);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, path_y[i]);
    lua_setfield(L, -2, "y");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

void brainCoreRegisterPathfinder(lua_State *L, BrainPathfinder **pfPtr) {
  static const struct { const char *name; lua_CFunction func; } funcs[] = {
    { "cpf_set_terrain_cost",  l_cpf_set_terrain_cost },
    { "cpf_set_boat_cost",     l_cpf_set_boat_cost },
    { "cpf_set_terrain_speed", l_cpf_set_terrain_speed },
    { "cpf_set_config",        l_cpf_set_config },
    { "cpf_clear_danger",      l_cpf_clear_danger },
    { "cpf_stamp_pill",        l_cpf_stamp_pill },
    { "cpf_set_danger",        l_cpf_set_danger },
    { "cpf_set_overlay",       l_cpf_set_overlay },
    { "cpf_clear_overlay",     l_cpf_clear_overlay },
    { "cpf_clear_influence",   l_cpf_clear_influence },
    { "cpf_stamp_influence",   l_cpf_stamp_influence },
    { "cpf_influence_at",      l_cpf_influence_at },
    { "cpf_path_to",           l_cpf_path_to },
    { "cpf_cost_to",           l_cpf_cost_to },
    { "cpf_estimate_cost",     l_cpf_estimate_cost },
    { "cpf_danger_at",             l_cpf_danger_at },
    { "cpf_lgm_travel_ticks",      l_cpf_lgm_travel_ticks },
    { "cpf_lgm_travel_ticks_map",  l_cpf_lgm_travel_ticks_map },
    { "cpf_estimate_tank_travel_ticks", l_cpf_estimate_tank_travel_ticks },
    { "cpf_trace_path",            l_cpf_trace_path },
    { "cpf_find_front_line",       l_cpf_find_front_line },
    { NULL, NULL }
  };
  int i;
  for (i = 0; funcs[i].name != NULL; i++) {
    lua_pushlightuserdata(L, (void *)pfPtr);
    lua_pushcclosure(L, funcs[i].func, 1);
    lua_setglobal(L, funcs[i].name);
  }
}

/* ------------------------------------------------------------------ */
/* C World Simulator Lua wrappers (wsim_* globals)                     */
/* ------------------------------------------------------------------ */

#define WSIM_GET(L) \
  BrainWorldSim **pps = (BrainWorldSim **)lua_touserdata(L, lua_upvalueindex(1)); \
  BrainWorldSim *ws = (pps && *pps) ? *pps : NULL; \
  if (!ws) return 0

static int l_wsim_clear(lua_State *L) {
  WSIM_GET(L);
  brainWorldSimClear(ws);
  return 0;
}

static int l_wsim_set_terrain_speed(lua_State *L) {
  WSIM_GET(L);
  int type = (int)luaL_checkinteger(L, 1);
  float speed = (float)luaL_checknumber(L, 2);
  brainWorldSimSetTerrainSpeed(ws, type, speed);
  return 0;
}

static int l_wsim_add_pill(lua_State *L) {
  WSIM_GET(L);
  int mx     = (int)luaL_checkinteger(L, 1);
  int my     = (int)luaL_checkinteger(L, 2);
  int health = (int)luaL_checkinteger(L, 3);
  float anger = (float)luaL_checknumber(L, 4);
  int owner  = (int)luaL_checkinteger(L, 5);
  int pill_id = (int)luaL_checkinteger(L, 6);
  brainWorldSimAddPill(ws, mx, my, health, anger, owner, pill_id);
  return 0;
}

static int l_wsim_add_tank(lua_State *L) {
  WSIM_GET(L);
  int wx      = (int)luaL_checkinteger(L, 1);
  int wy      = (int)luaL_checkinteger(L, 2);
  int dir     = (int)luaL_checkinteger(L, 3);
  int speed   = (int)luaL_checkinteger(L, 4);
  int is_ours = lua_toboolean(L, 5);
  int owner   = (int)luaL_checkinteger(L, 6);
  int armour  = (int)luaL_checkinteger(L, 7);
  brainWorldSimAddTank(ws, wx, wy, dir, speed, is_ours, owner, armour);
  return 0;
}

static int l_wsim_set_path(lua_State *L) {
  WSimPathPoint pts[WSIM_MAX_PATH];
  int count = 0;
  int i;
  WSIM_GET(L);

  luaL_checktype(L, 1, LUA_TTABLE);
  count = (int)lua_rawlen(L, 1);
  if (count > WSIM_MAX_PATH) count = WSIM_MAX_PATH;

  for (i = 0; i < count; i++) {
    lua_rawgeti(L, 1, i + 1);
    lua_getfield(L, -1, "x");
    pts[i].mx = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "y");
    pts[i].my = (uint8_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_pop(L, 1); /* pop the sub-table */
  }

  brainWorldSimSetPath(ws, pts, count);
  return 0;
}

static int l_wsim_set_attack_target(lua_State *L) {
  WSIM_GET(L);
  int idx = (int)luaL_checkinteger(L, 1);
  brainWorldSimSetAttackTarget(ws, idx);
  return 0;
}

static int l_wsim_set_lgm(lua_State *L) {
  WSIM_GET(L);
  int dispatch_tick = (int)luaL_checkinteger(L, 1);
  int dest_mx       = (int)luaL_checkinteger(L, 2);
  int dest_my       = (int)luaL_checkinteger(L, 3);
  int speed         = (int)luaL_optinteger(L, 4, 4);
  brainWorldSimSetLGM(ws, dispatch_tick, dest_mx, dest_my, speed);
  return 0;
}

static int l_wsim_run(lua_State *L) {
  WSimResult r;
  int i;
  WSIM_GET(L);

  int max_ticks = (int)luaL_optinteger(L, 1, 300);
  r = brainWorldSimRun(ws, max_ticks);

  /* Build result table */
  lua_createtable(L, 0, 10);

  lua_pushinteger(L, r.armour_remaining);
  lua_setfield(L, -2, "armour");

  lua_pushinteger(L, r.damage_taken);
  lua_setfield(L, -2, "damage");

  lua_pushinteger(L, r.ticks_simulated);
  lua_setfield(L, -2, "ticks");

  lua_pushinteger(L, r.arrival_tick);
  lua_setfield(L, -2, "arrival");

  lua_pushboolean(L, r.killed);
  lua_setfield(L, -2, "killed");

  /* LGM fields: only present if LGM was dispatched */
  if (r.lgm_survived > 0) {
    lua_pushboolean(L, r.lgm_survived == 1);
    lua_setfield(L, -2, "lgm_survived");

    if (r.lgm_arrival_tick >= 0) {
      lua_pushinteger(L, r.lgm_arrival_tick);
      lua_setfield(L, -2, "lgm_arrival");
    }
    if (r.lgm_death_tick >= 0) {
      lua_pushinteger(L, r.lgm_death_tick);
      lua_setfield(L, -2, "lgm_death");
    }
  }

  /* Per-pill results array (only pills that were added) */
  lua_createtable(L, ws->num_pills, 0);
  for (i = 0; i < ws->num_pills; i++) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, ws->pills[i].pill_id);
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, r.pill_shots[i]);
    lua_setfield(L, -2, "shots");
    lua_pushinteger(L, r.pill_final_health[i]);
    lua_setfield(L, -2, "health");
    lua_pushinteger(L, r.pill_final_speed[i]);
    lua_setfield(L, -2, "speed");
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "pills");

  return 1;
}

void brainCoreRegisterWorldSim(lua_State *L, BrainWorldSim **wsPtr) {
  static const struct { const char *name; lua_CFunction func; } funcs[] = {
    { "wsim_clear",             l_wsim_clear },
    { "wsim_set_terrain_speed", l_wsim_set_terrain_speed },
    { "wsim_add_pill",          l_wsim_add_pill },
    { "wsim_add_tank",          l_wsim_add_tank },
    { "wsim_set_path",          l_wsim_set_path },
    { "wsim_set_attack_target", l_wsim_set_attack_target },
    { "wsim_set_lgm",           l_wsim_set_lgm },
    { "wsim_run",               l_wsim_run },
    { NULL, NULL }
  };
  int i;
  for (i = 0; funcs[i].name != NULL; i++) {
    lua_pushlightuserdata(L, (void *)wsPtr);
    lua_pushcclosure(L, funcs[i].func, 1);
    lua_setglobal(L, funcs[i].name);
  }
}
