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
#include "brain_overlay.h"
#include "shells.h"
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
  lua_pushboolean(L, info->inboat);         lua_setfield(L, -2, "inboat");
  lua_pushboolean(L, info->hidden);         lua_setfield(L, -2, "hidden");
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
    { FILE *ef = fopen("brain_error.log", "a"); if (ef) { fprintf(ef, "brain global is not a table (type=%d)\n", lua_type(L, -1)); fclose(ef); } }
    lua_settop(L, top);
    return false;
  }

  lua_getfield(L, -1, "think");
  if (!lua_isfunction(L, -1)) {
    fprintf(stderr, "brainCore: brain.think is not a function\n");
    { FILE *ef = fopen("brain_error.log", "a"); if (ef) { fprintf(ef, "brain.think is not a function (type=%d)\n", lua_type(L, -1)); fclose(ef); } }
    lua_settop(L, top);
    return false;
  }

  brainCorePushInfo(L, info);

  if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
    const char *errMsg = lua_tostring(L, -1);
    fprintf(stderr, "brainCore: brain.think() error: %s\n", errMsg ? errMsg : "(unknown)");
    /* Write to brain_error.log so errors are never lost */
    {
      FILE *ef = fopen("brain_error.log", "a");
      if (ef) {
        fprintf(ef, "brain.think() error: %s\n", errMsg ? errMsg : "(unknown)");
        fclose(ef);
      }
    }
    /* Route error through Lua print() so BrainTest Print Output captures it */
    lua_getglobal(L, "print");
    if (lua_isfunction(L, -1)) {
      lua_pushfstring(L, "[FATAL] brain.think() error: %s", errMsg ? errMsg : "(unknown)");
      lua_pcall(L, 1, 0, 0);
    } else {
      lua_pop(L, 1);
    }
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
    const char *errMsg = lua_tostring(L, -1);
    fprintf(stderr, "brainCore: brain.%s() error: %s\n",
            method, errMsg ? errMsg : "(unknown)");
    {
      FILE *ef = fopen("brain_error.log", "a");
      if (ef) {
        fprintf(ef, "brain.%s() error: %s\n", method, errMsg ? errMsg : "(unknown)");
        fclose(ef);
      }
    }
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

/* Batch-load the entire danger grid from a Lua table keyed by mkey
 * (my*256 + mx) -> value. Clears the grid first. One C call replaces
 * the per-entry cpf_set_danger loop driven from Lua. */
static int l_cpf_load_danger(lua_State *L) {
  CPF_GET(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  brainPathfinderClearDanger(pf);
  lua_pushnil(L);
  while (lua_next(L, 1) != 0) {
    lua_Integer k = luaL_checkinteger(L, -2);
    float v = (float)luaL_checknumber(L, -1);
    int x = (int)(k & 255);
    int y = (int)((k >> 8) & 255);
    brainPathfinderSetDanger(pf, x, y, v);
    lua_pop(L, 1);
  }
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
  int in_boat = (int)luaL_checkinteger(L, 5);
  int shells = (int)luaL_checkinteger(L, 6);
  int trees = (int)luaL_checkinteger(L, 7);
  int mines = (int)luaL_optinteger(L, 8, 0);
  int armour = (int)luaL_optinteger(L, 9, 40);
  int budget = (int)luaL_optinteger(L, 10, 1500);

  /* Log Lua stack trace to A* log if enabled */
  if (pf->astarLog) {
    FILE *alog = (FILE *)pf->astarLog;
    luaL_traceback(L, L, NULL, 1);
    fprintf(alog, "CALLER: boat=%d %s\n", in_boat, lua_tostring(L, -1));
    lua_pop(L, 1);
    fflush(alog);
  }

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

static int l_cpf_cost_to_reset(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int in_boat = (int)luaL_checkinteger(L, 3);
  int shells = (int)luaL_checkinteger(L, 4);
  int trees = (int)luaL_checkinteger(L, 5);
  int mines = (int)luaL_checkinteger(L, 6);
  int armour = (int)luaL_checkinteger(L, 7);
  brainPathfinderCostToReset(pf, sx, sy, in_boat, shells, trees, mines, armour);
  return 0;
}

static int l_cpf_cost_to_incremental(lua_State *L) {
  CPF_GET(L);
  int dx = (int)luaL_checkinteger(L, 1);
  int dy = (int)luaL_checkinteger(L, 2);
  int budget = (int)luaL_optinteger(L, 3, 16000);
  float cost = brainPathfinderCostToIncremental(pf, dx, dy, budget);
  lua_pushnumber(L, (double)cost);
  return 1;
}

/* cpf_dijkstra_from(sx, sy, in_boat, shells, trees, mines, armour)
 * Runs full Dijkstra from the source. Returns three values:
 *   us       - wall-clock microseconds the search took
 *   expanded - number of nodes expanded
 *   peak_open - peak heap size during the search
 * After it returns, internal g_cost holds min cost to every reachable tile,
 * but no lookup helper is exposed yet — this is a perf-measurement entry. */
static int l_cpf_dijkstra_from(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int in_boat = (int)luaL_checkinteger(L, 3);
  int shells = (int)luaL_checkinteger(L, 4);
  int trees = (int)luaL_checkinteger(L, 5);
  int mines = (int)luaL_checkinteger(L, 6);
  int armour = (int)luaL_checkinteger(L, 7);
  int expanded = 0, peak_open = 0;
  double us = brainPathfinderDijkstraFrom(pf, sx, sy, in_boat,
                                           shells, trees, mines, armour,
                                           &expanded, &peak_open);
  lua_pushnumber(L, us);
  lua_pushinteger(L, expanded);
  lua_pushinteger(L, peak_open);
  return 3;
}

/* cpf_dijkstra_start(slate, tick, sx, sy, in_boat, shells, trees, mines, armour,
 *                    max_cost, exact, danger_scale, kind) */
static int l_cpf_dijkstra_start(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  uint32_t tick = (uint32_t)luaL_checkinteger(L, 2);
  int sx = (int)luaL_checkinteger(L, 3);
  int sy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
  int shells = (int)luaL_checkinteger(L, 6);
  int trees = (int)luaL_checkinteger(L, 7);
  int mines = (int)luaL_checkinteger(L, 8);
  int armour = (int)luaL_checkinteger(L, 9);
  float max_cost = (float)luaL_optnumber(L, 10, 0.0);
  int exact;
  if (lua_isnoneornil(L, 11)) {
    exact = 1;
  } else if (lua_isboolean(L, 11)) {
    exact = lua_toboolean(L, 11) ? 1 : 0;
  } else {
    exact = (int)luaL_checkinteger(L, 11) != 0 ? 1 : 0;
  }
  float danger_scale = (float)luaL_optnumber(L, 12, 1.0);
  int kind = (int)luaL_optinteger(L, 13, 0);
  brainPathfinderDijkstraStart(pf, slate, tick, sx, sy, in_boat,
                                shells, trees, mines, armour,
                                max_cost, exact, danger_scale, kind);
  return 0;
}

/* cpf_dijkstra_step(slate, tick, budget) */
static int l_cpf_dijkstra_step(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  uint32_t tick = (uint32_t)luaL_checkinteger(L, 2);
  int budget = (int)luaL_checkinteger(L, 3);
  int done = brainPathfinderDijkstraStep(pf, slate, tick, budget);
  int expanded = 0, peak_open = 0, d = 0;
  brainPathfinderDijkstraStatus(pf, slate, &expanded, &peak_open, &d);
  lua_pushboolean(L, done);
  lua_pushinteger(L, expanded);
  lua_pushinteger(L, peak_open);
  return 3;
}

/* cpf_dijkstra_cost_at(slate, x, y, boat) */
static int l_cpf_dijkstra_cost_at(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int boat = (int)luaL_optinteger(L, 4, 0);
  float cost = brainPathfinderDijkstraCostAt(pf, slate, x, y, boat);
  lua_pushnumber(L, (double)cost);
  return 1;
}

/* cpf_dijkstra_lookup_by_kind(kind, x, y, boat)
 * Searches all slates of given kind in started_tick descending order,
 * returns first finite cost. Newer searches win even if still running. */
static int l_cpf_dijkstra_lookup_by_kind(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int x = (int)luaL_checkinteger(L, 2);
  int y = (int)luaL_checkinteger(L, 3);
  int boat = (int)luaL_optinteger(L, 4, 0);
  float cost = brainPathfinderDijkstraLookupByKind(pf, kind, x, y, boat);
  lua_pushnumber(L, (double)cost);
  return 1;
}

/* cpf_dijkstra_next_step(kind, sx, sy, dx, dy) → nx, ny or nil */
static int l_cpf_dijkstra_next_step(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int sx = (int)luaL_checkinteger(L, 2);
  int sy = (int)luaL_checkinteger(L, 3);
  int dx = (int)luaL_checkinteger(L, 4);
  int dy = (int)luaL_checkinteger(L, 5);
  int nx = -1, ny = -1;
  if (brainPathfinderDijkstraNextStep(pf, kind, sx, sy, dx, dy, &nx, &ny)) {
    lua_pushinteger(L, nx);
    lua_pushinteger(L, ny);
    return 2;
  }
  lua_pushnil(L);
  return 1;
}

/* cpf_dijkstra_trace_path(kind, dx, dy) → array of {x=, y=} or nil */
static int l_cpf_dijkstra_trace_path(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  int dx = (int)luaL_checkinteger(L, 2);
  int dy = (int)luaL_checkinteger(L, 3);
  int slate = brainPathfinderDijkstraFindBest(pf, kind);
  if (slate < 0) { lua_pushnil(L); return 1; }
  int path_x[512], path_y[512];
  int n = brainPathfinderDijkstraTracePath(pf, slate, dx, dy, path_x, path_y, 512);
  if (n <= 0) { lua_pushnil(L); return 1; }
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, path_x[i]);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, path_y[i]);
    lua_setfield(L, -2, "y");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

/* cpf_dijkstra_pick_reuse_slate(kind)
 * Returns the slate index the brain should reuse next when starting a
 * search of the given kind. Picks an unused slate first, then the
 * oldest slate of matching kind, then the oldest slate overall. */
static int l_cpf_dijkstra_pick_reuse_slate(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  lua_pushinteger(L, brainPathfinderDijkstraPickReuseSlate(pf, kind));
  return 1;
}

/* cpf_dijkstra_find_best(kind) → slate index of freshest slate, or -1. */
static int l_cpf_dijkstra_find_best(lua_State *L) {
  CPF_GET(L);
  int kind = (int)luaL_checkinteger(L, 1);
  lua_pushinteger(L, brainPathfinderDijkstraFindBest(pf, kind));
  return 1;
}

/* cpf_dijkstra_status(slate)
 * Returns: active, done, kind, started_tick, completed_tick,
 *          expanded, peak_open, src_x, src_y, in_boat, danger_scale. */
static int l_cpf_dijkstra_status(lua_State *L) {
  CPF_GET(L);
  int slate = (int)luaL_checkinteger(L, 1);
  const DijkstraSlate *s = brainPathfinderDijkstraGetSlate(pf, slate);
  if (!s) {
    /* Push 11 nils so the caller can rely on the count */
    for (int i = 0; i < 11; i++) lua_pushnil(L);
    return 11;
  }
  lua_pushboolean(L, s->active);
  lua_pushboolean(L, s->done);
  lua_pushinteger(L, s->kind);
  lua_pushinteger(L, s->started_tick);
  lua_pushinteger(L, s->completed_tick);
  lua_pushinteger(L, s->expanded);
  lua_pushinteger(L, s->peak_open);
  lua_pushinteger(L, s->src_x);
  lua_pushinteger(L, s->src_y);
  lua_pushinteger(L, s->in_boat);
  lua_pushnumber(L, s->danger_scale);
  return 11;
}

/* cpf_rebuild_edge_costs()
 * Force a rebuild of the precomputed neighbor edge cost grid. Normally
 * happens lazily on the first Dijkstra start after a map change; this
 * lets the brain trigger it explicitly (e.g. after a base capture or
 * pillbox demolition that changes terrain). */
static int l_cpf_rebuild_edge_costs(lua_State *L) {
  CPF_GET(L);
  brainPathfinderRebuildEdgeCosts(pf);
  return 0;
}

/* cpf_simulate_shot(origin_wx, origin_wy, target_wx, target_wy,
 *                   shooter_type=TANK, sight_len=0)
 *   -> { {mx=..., my=...}, ... }
 * Stateless wrapper over brainPathfinderSimulateShot (no pf instance
 * needed — uses only static physics constants). */
static int l_cpf_simulate_shot(lua_State *L) {
  WORLD ox = (WORLD)luaL_checkinteger(L, 1);
  WORLD oy = (WORLD)luaL_checkinteger(L, 2);
  WORLD tx = (WORLD)luaL_checkinteger(L, 3);
  WORLD ty = (WORLD)luaL_checkinteger(L, 4);
  int shooter   = (int)luaL_optinteger(L, 5, BRAIN_SHOT_SHOOTER_TANK);
  int sight_len = (int)luaL_optinteger(L, 6, 0);

  BrainShotTile tiles[64];
  int n = brainPathfinderSimulateShot(ox, oy, tx, ty, shooter, sight_len,
                                      tiles, (int)(sizeof(tiles)/sizeof(tiles[0])));
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, tiles[i].mx); lua_setfield(L, -2, "mx");
    lua_pushinteger(L, tiles[i].my); lua_setfield(L, -2, "my");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

/* cpf_simulate_shot_angle(origin_wx, origin_wy, angle,
 *                          shooter_type=TANK, sight_len=0)
 *   -> { {mx=..., my=...}, ... }
 * Same as cpf_simulate_shot but takes the firing angle directly
 * (0..255 bradians). Use this when you want a bit-exact match to a
 * real shell — pass info.direction so the sim doesn't have to
 * round-trip through atan2 + lroundf. */
static int l_cpf_simulate_shot_angle(lua_State *L) {
  WORLD ox = (WORLD)luaL_checkinteger(L, 1);
  WORLD oy = (WORLD)luaL_checkinteger(L, 2);
  int angle    = (int)luaL_checkinteger(L, 3);
  int shooter  = (int)luaL_optinteger(L, 4, BRAIN_SHOT_SHOOTER_TANK);
  int sight_len= (int)luaL_optinteger(L, 5, 0);

  BrainShotTile tiles[64];
  int n = brainPathfinderSimulateShotAngle(ox, oy, angle, shooter, sight_len,
                                           tiles, (int)(sizeof(tiles)/sizeof(tiles[0])));
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, tiles[i].mx); lua_setfield(L, -2, "mx");
    lua_pushinteger(L, tiles[i].my); lua_setfield(L, -2, "my");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}

static int l_cpf_estimate_cost(lua_State *L) {
  CPF_GET(L);
  int sx = (int)luaL_checkinteger(L, 1);
  int sy = (int)luaL_checkinteger(L, 2);
  int dx = (int)luaL_checkinteger(L, 3);
  int dy = (int)luaL_checkinteger(L, 4);
  int in_boat = (int)luaL_checkinteger(L, 5);
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
  int in_boat = (int)luaL_checkinteger(L, 5);
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

/* cpf_dijkstra_shells_at(kind, x, y)
 * Returns shells remaining on arrival at (x,y) from the freshest Dijkstra
 * slate of the given kind, or nil if no slate has a finite cost there.
 * Valid after smart_cost when Dijkstra was the source of the cost. */
static int l_cpf_dijkstra_shells_at(lua_State *L) {
  CPF_GET(L);
  int kind  = (int)luaL_checkinteger(L, 1);
  int x     = (int)luaL_checkinteger(L, 2);
  int y     = (int)luaL_checkinteger(L, 3);
  int best  = brainPathfinderDijkstraFindBest(pf, kind);
  if (best < 0) { lua_pushnil(L); return 1; }
  DijkstraSlate *s = &pf->dij_slates[best];
  if (!s->shells_at) { lua_pushnil(L); return 1; }
  int node = y * 256 + x;  /* land node (boat=0) */
  float g = s->g_cost ? s->g_cost[node] : 1e30f;
  if (g >= 1e29f) { lua_pushnil(L); return 1; }  /* slate hasn't reached tile */
  lua_pushinteger(L, (lua_Integer)s->shells_at[node]);
  return 1;
}

/* cpf_astar_shells_at(x, y)
 * Returns shells remaining on arrival at (x,y) from the last cost_to call.
 * Valid after smart_cost when A* was used (Dijkstra off or tile not yet reached). */
static int l_cpf_astar_shells_at(lua_State *L) {
  CPF_GET(L);
  int x = (int)luaL_checkinteger(L, 1);
  int y = (int)luaL_checkinteger(L, 2);
  int node = y * 256 + x;  /* land node */
  lua_pushinteger(L, (lua_Integer)pf->shells_at[node]);
  return 1;
}

/* shell_debug_hits() → array of tables {wx=, wy=, owner=}
 * Returns the ring buffer of recent shell-collision world positions as
 * recorded by shellsUpdate in the C sim. The brain calls this each tick
 * and redraws overlays — because overlay commands are recorded per-frame,
 * replay scrubbing shows the exact set known at that historical tick. */
static int l_shell_debug_hits(lua_State *L) {
  int n = shellsDebugHitLogCount();
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) {
    int wx = 0, wy = 0;
    uint32_t tk = 0;
    uint8_t owner = 0;
    if (!shellsDebugHitLogGet(i, &wx, &wy, &tk, &owner)) continue;
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, wx);    lua_setfield(L, -2, "wx");
    lua_pushinteger(L, wy);    lua_setfield(L, -2, "wy");
    lua_pushinteger(L, owner); lua_setfield(L, -2, "owner");
    lua_rawseti(L, -2, i + 1);
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

/* cpf_serialize() -> string
 * Returns binary blob of the full pathfinder state (grids + Dijkstra slates). */
static int l_cpf_serialize(lua_State *L) {
  BrainPathfinder **pfPtr = (BrainPathfinder **)lua_touserdata(L, lua_upvalueindex(1));
  BrainPathfinder *pf = pfPtr ? *pfPtr : NULL;
  if (!pf) { lua_pushnil(L); return 1; }
  size_t blobSize = 0;
  unsigned char *blob = brainPathfinderSerialize(pf, &blobSize);
  if (!blob) { lua_pushnil(L); return 1; }
  lua_pushlstring(L, (const char *)blob, blobSize);
  free(blob);
  return 1;
}

/* cpf_deserialize(string) -> bool
 * Restores pathfinder state from a blob returned by cpf_serialize(). */
static int l_cpf_deserialize(lua_State *L) {
  BrainPathfinder **pfPtr = (BrainPathfinder **)lua_touserdata(L, lua_upvalueindex(1));
  BrainPathfinder *pf = pfPtr ? *pfPtr : NULL;
  if (!pf) { lua_pushboolean(L, 0); return 1; }
  size_t len = 0;
  const char *data = luaL_checklstring(L, 1, &len);
  int ok = brainPathfinderDeserialize(pf, (const unsigned char *)data, len);
  lua_pushboolean(L, ok);
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
    { "cpf_load_danger",       l_cpf_load_danger },
    { "cpf_set_overlay",       l_cpf_set_overlay },
    { "cpf_clear_overlay",     l_cpf_clear_overlay },
    { "cpf_clear_influence",   l_cpf_clear_influence },
    { "cpf_stamp_influence",   l_cpf_stamp_influence },
    { "cpf_influence_at",      l_cpf_influence_at },
    { "cpf_path_to",           l_cpf_path_to },
    { "cpf_cost_to",           l_cpf_cost_to },
    { "cpf_cost_to_reset",     l_cpf_cost_to_reset },
    { "cpf_cost_to_incremental", l_cpf_cost_to_incremental },
    { "cpf_dijkstra_from",     l_cpf_dijkstra_from },
    { "cpf_dijkstra_start",         l_cpf_dijkstra_start },
    { "cpf_dijkstra_step",          l_cpf_dijkstra_step },
    { "cpf_dijkstra_cost_at",       l_cpf_dijkstra_cost_at },
    { "cpf_dijkstra_lookup_by_kind", l_cpf_dijkstra_lookup_by_kind },
    { "cpf_dijkstra_next_step",     l_cpf_dijkstra_next_step },
    { "cpf_dijkstra_trace_path",    l_cpf_dijkstra_trace_path },
    { "cpf_dijkstra_pick_reuse_slate", l_cpf_dijkstra_pick_reuse_slate },
    { "cpf_dijkstra_find_best",     l_cpf_dijkstra_find_best },
    { "cpf_dijkstra_status",        l_cpf_dijkstra_status },
    { "cpf_rebuild_edge_costs", l_cpf_rebuild_edge_costs },
    { "cpf_estimate_cost",     l_cpf_estimate_cost },
    { "cpf_simulate_shot",        l_cpf_simulate_shot },
    { "cpf_simulate_shot_angle",  l_cpf_simulate_shot_angle },
    { "cpf_danger_at",             l_cpf_danger_at },
    { "cpf_lgm_travel_ticks",      l_cpf_lgm_travel_ticks },
    { "cpf_lgm_travel_ticks_map",  l_cpf_lgm_travel_ticks_map },
    { "cpf_estimate_tank_travel_ticks", l_cpf_estimate_tank_travel_ticks },
    { "cpf_dijkstra_shells_at",    l_cpf_dijkstra_shells_at },
    { "cpf_astar_shells_at",       l_cpf_astar_shells_at },
    { "cpf_trace_path",            l_cpf_trace_path },
    { "shell_debug_hits",          l_shell_debug_hits },
    { "cpf_find_front_line",       l_cpf_find_front_line },
    { "cpf_serialize",             l_cpf_serialize },
    { "cpf_deserialize",           l_cpf_deserialize },
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

  /* Per-hit position log */
  lua_createtable(L, r.num_hits, 0);
  for (i = 0; i < r.num_hits; i++) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, r.hits[i].mx);
    lua_setfield(L, -2, "mx");
    lua_pushinteger(L, r.hits[i].my);
    lua_setfield(L, -2, "my");
    lua_pushinteger(L, r.hits[i].armour_after);
    lua_setfield(L, -2, "armour");
    lua_pushinteger(L, r.hits[i].tick);
    lua_setfield(L, -2, "tick");
    lua_pushinteger(L, r.hits[i].pill_idx);
    lua_setfield(L, -2, "pill");
    lua_rawseti(L, -2, i + 1);
  }
  lua_setfield(L, -2, "hits");

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

/* ------------------------------------------------------------------ */
/* Overlay drawing Lua wrappers (overlay_* globals)                    */
/* ------------------------------------------------------------------ */

#define OVL_GET(L) \
  OverlayCmdBuffer **ppb = (OverlayCmdBuffer **)lua_touserdata(L, lua_upvalueindex(1)); \
  OverlayCmdBuffer *buf = (ppb && *ppb) ? *ppb : NULL; \
  if (!buf) return 0

/* overlay_clear() */
static int l_overlay_clear(lua_State *L) {
  OVL_GET(L);
  overlayCmdBufferClear(buf);
  return 0;
}

/* overlay_line(x1, y1, x2, y2, r, g, b [, a [, viz_idx]])
 * viz_idx: optional uint8 index into BrainTest's VIZ_TOGGLES array,
 *          set by viz.lua wrappers so the renderer can filter
 *          recorded commands during playback. Defaults to 0xFF
 *          ("no viz_id" — never filtered). */
static int l_overlay_line(lua_State *L) {
  OVL_GET(L);
  float x1 = (float)luaL_checknumber(L, 1);
  float y1 = (float)luaL_checknumber(L, 2);
  float x2 = (float)luaL_checknumber(L, 3);
  float y2 = (float)luaL_checknumber(L, 4);
  int r = luaL_checkinteger(L, 5);
  int g = luaL_checkinteger(L, 6);
  int b = luaL_checkinteger(L, 7);
  int a = luaL_optinteger(L, 8, 255);
  int viz_idx = luaL_optinteger(L, 9, OVERLAY_VIZ_IDX_NONE);
  overlayCmdLine(buf, x1, y1, x2, y2, r, g, b, a);
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_rect(x1, y1, x2, y2, r, g, b [, a [, filled]]) */
static int l_overlay_rect(lua_State *L) {
  OVL_GET(L);
  float x1 = (float)luaL_checknumber(L, 1);
  float y1 = (float)luaL_checknumber(L, 2);
  float x2 = (float)luaL_checknumber(L, 3);
  float y2 = (float)luaL_checknumber(L, 4);
  int r = luaL_checkinteger(L, 5);
  int g = luaL_checkinteger(L, 6);
  int b = luaL_checkinteger(L, 7);
  int a = luaL_optinteger(L, 8, 255);
  int filled = lua_toboolean(L, 9);
  /* 10th arg (optional, default false): if true and rect is filled,
   * the overlay is drawn at full 1/256-tile precision instead of
   * being floored to the game-pixel grid. Used by overlays that
   * track sub-wu sprites (e.g. the LGM, which renders at sub-wu). */
  int subpixel = lua_toboolean(L, 10);
  /* 11th arg (optional): viz_idx for playback-time filtering. */
  int viz_idx = luaL_optinteger(L, 11, OVERLAY_VIZ_IDX_NONE);
  overlayCmdRect(buf, x1, y1, x2, y2, r, g, b, a, filled);
  if (subpixel && filled && buf && buf->count > 0) {
    buf->cmds[buf->count - 1].type = OVERLAY_CMD_RECT_FILL_SUBPIXEL;
  }
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_circle(cx, cy, radius, r, g, b [, a]) */
static int l_overlay_circle(lua_State *L) {
  OVL_GET(L);
  float cx = (float)luaL_checknumber(L, 1);
  float cy = (float)luaL_checknumber(L, 2);
  float radius = (float)luaL_checknumber(L, 3);
  int r = luaL_checkinteger(L, 4);
  int g = luaL_checkinteger(L, 5);
  int b = luaL_checkinteger(L, 6);
  int a = luaL_optinteger(L, 7, 255);
  int viz_idx = luaL_optinteger(L, 8, OVERLAY_VIZ_IDX_NONE);
  overlayCmdCircle(buf, cx, cy, radius, r, g, b, a);
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_text(x, y, text [, anchor [, r, g, b [, a]]])
 * anchor: "topleft", "topright", "bottomleft", "bottomright", "center" */
static int l_overlay_text(lua_State *L) {
  OVL_GET(L);
  float x = (float)luaL_checknumber(L, 1);
  float y = (float)luaL_checknumber(L, 2);
  const char *text = luaL_checkstring(L, 3);
  const char *anchorStr = luaL_optstring(L, 4, "topleft");
  int r = luaL_optinteger(L, 5, 255);
  int g = luaL_optinteger(L, 6, 255);
  int b = luaL_optinteger(L, 7, 255);
  int a = luaL_optinteger(L, 8, 255);
  float scale = (float)luaL_optnumber(L, 9, 1.0);
  int viz_idx = luaL_optinteger(L, 10, OVERLAY_VIZ_IDX_NONE);

  uint8_t anchor = OVERLAY_ANCHOR_TOPLEFT;
  if (strcmp(anchorStr, "topright") == 0)         anchor = OVERLAY_ANCHOR_TOPRIGHT;
  else if (strcmp(anchorStr, "bottomleft") == 0)  anchor = OVERLAY_ANCHOR_BOTTOMLEFT;
  else if (strcmp(anchorStr, "bottomright") == 0) anchor = OVERLAY_ANCHOR_BOTTOMRIGHT;
  else if (strcmp(anchorStr, "center") == 0)      anchor = OVERLAY_ANCHOR_CENTER;

  overlayCmdText(buf, x, y, text, anchor, r, g, b, a);
  /* Store scale in the radius field (unused for text) */
  if (buf && buf->count > 0) {
    buf->cmds[buf->count - 1].radius = scale;
  }
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* overlay_hud_text(offset_x, offset_y, text, anchor, r, g, b [, a])
 * anchor: "topleft", "topright", "bottomleft", "bottomright"
 * x/y are pixel offsets from the chosen corner */
static int l_overlay_hud_text(lua_State *L) {
  OVL_GET(L);
  float x = (float)luaL_checknumber(L, 1);
  float y = (float)luaL_checknumber(L, 2);
  const char *text = luaL_checkstring(L, 3);
  const char *anchorStr = luaL_optstring(L, 4, "topleft");
  int r = luaL_optinteger(L, 5, 255);
  int g = luaL_optinteger(L, 6, 255);
  int b = luaL_optinteger(L, 7, 255);
  int a = luaL_optinteger(L, 8, 255);
  int viz_idx = luaL_optinteger(L, 9, OVERLAY_VIZ_IDX_NONE);

  uint8_t anchor = OVERLAY_ANCHOR_TOPLEFT;
  if (strcmp(anchorStr, "topright") == 0)         anchor = OVERLAY_ANCHOR_TOPRIGHT;
  else if (strcmp(anchorStr, "bottomleft") == 0)  anchor = OVERLAY_ANCHOR_BOTTOMLEFT;
  else if (strcmp(anchorStr, "bottomright") == 0) anchor = OVERLAY_ANCHOR_BOTTOMRIGHT;
  else if (strcmp(anchorStr, "center") == 0)      anchor = OVERLAY_ANCHOR_CENTER;

  overlayCmdHudText(buf, x, y, text, anchor, r, g, b, a);
  overlayCmdSetLastVizIdx(buf, (uint8_t)viz_idx);
  return 0;
}

/* UNUSED ON THIS BRANCH — kept in lockstep with the BrainTest source
 * line so future merges don't conflict. Registers overlay_* Lua
 * globals (debug-shape drawing) backed by a per-brain OverlayCmdBuffer.
 * No caller wires up `bufPtr` here, so the brain's viz.lua wrappers
 * see overlay_* as nil and silently no-op (`if not overlay_text then
 * return end`). When BrainTest is wired in on a future branch, it
 * calls this from luabrainshandler.c after creating the brain instance. */
void brainCoreRegisterOverlay(lua_State *L, OverlayCmdBuffer **bufPtr) {
  static const struct { const char *name; lua_CFunction func; } funcs[] = {
    { "overlay_clear",    l_overlay_clear },
    { "overlay_line",     l_overlay_line },
    { "overlay_rect",     l_overlay_rect },
    { "overlay_circle",   l_overlay_circle },
    { "overlay_text",     l_overlay_text },
    { "overlay_hud_text", l_overlay_hud_text },
    { NULL, NULL }
  };
  int i;
  for (i = 0; funcs[i].name != NULL; i++) {
    lua_pushlightuserdata(L, (void *)bufPtr);
    lua_pushcclosure(L, funcs[i].func, 1);
    lua_setglobal(L, funcs[i].name);
  }
}

/* ------------------------------------------------------------------ */
/* Print capture (override Lua's print to also call a callback)        */
/* ------------------------------------------------------------------ */

typedef struct {
    BrainPrintCaptureFunc cb;
    void *ud;
    const uint32_t *tickPtr;
} PrintCaptureCtx;

static int l_captured_print(lua_State *L) {
  PrintCaptureCtx *ctx = (PrintCaptureCtx *)lua_touserdata(L, lua_upvalueindex(1));

  /* Build the output string (same as Lua's default print) */
  int n = lua_gettop(L);
  char buf[4096];
  int pos = 0;
  for (int i = 1; i <= n; i++) {
    if (i > 1 && pos < (int)sizeof(buf) - 1) buf[pos++] = '\t';
    const char *s = luaL_tolstring(L, i, NULL);
    if (s) {
      int slen = (int)strlen(s);
      int room = (int)sizeof(buf) - pos - 1;
      if (slen > room) slen = room;
      memcpy(buf + pos, s, slen);
      pos += slen;
    }
    lua_pop(L, 1); /* pop the tostring result */
  }
  buf[pos] = '\0';

  /* Write to stderr as usual */
  fprintf(stderr, "%s\n", buf);

  /* Call the capture callback */
  if (ctx->cb) {
    uint32_t tick = ctx->tickPtr ? *ctx->tickPtr : 0;
    ctx->cb(tick, buf, ctx->ud);
  }

  return 0;
}

/* Global print capture — applied to all new brain instances */
/* UNUSED ON THIS BRANCH — kept in lockstep with the BrainTest source.
 * Lua print() override that mirrors output to a callback (used by
 * BrainTest's log window to capture per-bot prints with tick context).
 * No caller invokes brainCoreSetGlobalPrintCapture or
 * brainCoreRegisterPrintCapture here, so Lua's print() retains its
 * default stderr behavior. */
BrainPrintCaptureFunc g_printCb = NULL;
void *g_printCbUd = NULL;
const uint32_t *g_printTickPtr = NULL;

void brainCoreSetGlobalPrintCapture(BrainPrintCaptureFunc cb, void *ud,
                                     const uint32_t *tickPtr) {
  g_printCb = cb;
  g_printCbUd = ud;
  g_printTickPtr = tickPtr;
}

void brainCoreRegisterPrintCapture(lua_State *L, BrainPrintCaptureFunc cb,
                                    void *ud, const uint32_t *tickPtr) {
  /* Allocate context as Lua userdata so it lives as long as the Lua state */
  PrintCaptureCtx *ctx = (PrintCaptureCtx *)lua_newuserdata(L, sizeof(PrintCaptureCtx));
  ctx->cb = cb;
  ctx->ud = ud;
  ctx->tickPtr = tickPtr;
  lua_pushcclosure(L, l_captured_print, 1);
  lua_setglobal(L, "print");
}

/* ── braintest_viz_register binding + callback hook ─────────────────
 * Brains call `braintest_viz_register(id, label, short, long, default)`
 * to surface their viz_ids in BrainTest's V dialog. The host
 * (BrainTest) sets a callback that actually populates the registry;
 * other hosts (WinBolo client, headless server) leave the callback
 * NULL and the binding silently returns -1, which the Lua wrapper
 * treats as "not running under BrainTest, fine, do nothing". */
static BrainVizRegisterFunc g_vizRegisterCb = NULL;

void brainCoreSetVizRegisterCallback(BrainVizRegisterFunc cb) {
  g_vizRegisterCb = cb;
}

static int l_braintest_viz_register(lua_State *L) {
  const char *id         = luaL_checkstring(L, 1);
  const char *label      = luaL_optstring(L, 2, id);
  const char *short_desc = luaL_optstring(L, 3, "");
  const char *long_desc  = luaL_optstring(L, 4, "");
  int default_on         = lua_toboolean(L, 5);
  /* When the 5th arg isn't supplied, lua_toboolean returns 0 (off).
   * Treat "missing" as "default to ON" — most viz_ids start visible.
   * Detect via lua_isnoneornil. */
  if (lua_isnoneornil(L, 5)) default_on = 1;
  int idx = -1;
  if (g_vizRegisterCb) {
    idx = g_vizRegisterCb(id, label, short_desc, long_desc, default_on);
  }
  lua_pushinteger(L, idx);
  return 1;
}

void brainCoreRegisterVizRegister(lua_State *L) {
  lua_pushcfunction(L, l_braintest_viz_register);
  lua_setglobal(L, "braintest_viz_register");
}

/* ── braintest_panel_register host hook (parallel of viz register) ── */
static BrainPanelRegisterFunc g_panelRegisterCb = NULL;

void brainCoreSetPanelRegisterCallback(BrainPanelRegisterFunc cb) {
  g_panelRegisterCb = cb;
}

static int l_braintest_panel_register(lua_State *L) {
  /* Signature: braintest_panel_register(name, type, lua_expr [, opts]).
   *   `type`     — optional ("text" if nil); namespaced by host
   *   `lua_expr` — required; Lua chunk that returns the body string
   *   `opts`     — optional table; recognized keys:
   *                  shortcut = "T"   → panel gets its own SDL window
   *                                      toggled by this key. Empty /
   *                                      missing → tab in the P window.
   *
   * Two-arg form (name, lua_expr) still supported for older brains:
   * type defaults to "text", no opts. */
  const char *name = luaL_checkstring(L, 1);
  const char *type = NULL;
  const char *lua_expr = NULL;
  const char *shortcut = NULL;
  int top = lua_gettop(L);
  if (top >= 3) {
    if (!lua_isnoneornil(L, 2)) type = luaL_checkstring(L, 2);
    lua_expr = luaL_checkstring(L, 3);
    if (top >= 4 && lua_istable(L, 4)) {
      lua_getfield(L, 4, "shortcut");
      if (lua_isstring(L, -1)) shortcut = lua_tostring(L, -1);
      lua_pop(L, 1);
    }
  } else {
    lua_expr = luaL_checkstring(L, 2);
  }
  int idx = -1;
  if (g_panelRegisterCb) {
    idx = g_panelRegisterCb(name, type, lua_expr, shortcut);
  }
  lua_pushinteger(L, idx);
  return 1;
}

void brainCoreRegisterPanelRegister(lua_State *L) {
  lua_pushcfunction(L, l_braintest_panel_register);
  lua_setglobal(L, "braintest_panel_register");
}
