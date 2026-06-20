/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*********************************************************
 *Name:          Brain Lua Glue
 *Filename:      brain_lua_glue.h
 *Purpose:
 *  Public surface for the GUI Lua-brain glue
 *  (src/gui/sdl3/luabrainshandler.{h,c}). Consolidates the
 *  pathfinder, world-sim, brain-core registration, and
 *  ml-brain interfaces the GUI consumes into a single
 *  public header. The deeper internal headers
 *  (brain_pathfinder.h, brain_worldsim.h, braincore.h,
 *  ml_brain.h) stay internal — only the surface the GUI
 *  actually invokes is exposed here, via opaque forward
 *  decls plus function prototypes.
 *********************************************************/

#ifndef BRAIN_LUA_GLUE_H
#define BRAIN_LUA_GLUE_H

#include "global.h"
#include "brain.h"          /* For BrainInfo */
#include "brain_overlay.h"  /* For OverlayCmdBuffer */

#ifdef __cplusplus
extern "C" {
#endif

struct lua_State;
struct ClientSim;

/* Opaque pointer types — full definitions live in the internal headers. */
#ifndef BRAINPATHFINDER_TYPEDEF
#define BRAINPATHFINDER_TYPEDEF
typedef struct BrainPathfinder BrainPathfinder;
#endif
#ifndef BRAINWORLDSIM_TYPEDEF
#define BRAINWORLDSIM_TYPEDEF
typedef struct BrainWorldSim   BrainWorldSim;
#endif
typedef struct MLBrainInstance MLBrainInstance;

/* ---- Pathfinder lifecycle (brain_pathfinder.c) ---- */
BrainPathfinder *brainPathfinderCreate(void);
void             brainPathfinderDestroy(BrainPathfinder *pf);
void             brainPathfinderSetMap(BrainPathfinder *pf, const BYTE *map);
void             brainPathfinderDijkstraPreheat(BrainPathfinder *pf);

/* ---- World-sim lifecycle (brain_worldsim.c) ---- */
BrainWorldSim *brainWorldSimCreate(void);
void           brainWorldSimDestroy(BrainWorldSim *sim);
void           brainWorldSimSetMap(BrainWorldSim *sim, const BYTE *map);

/* ---- Brain core: Lua-side registration + invocation (braincore.c) ----
 *
 * Each Register* function installs a binding into the given Lua VM.
 * CallThink / CallMethod run the brain's think / named function and
 * marshal BrainInfo in/out. */
void brainCoreRegisterConstants(struct lua_State *L);
void brainCoreRegisterGetTerrain(struct lua_State *L, const BYTE **worldPtr);
void brainCoreRegisterPathfinder(struct lua_State *L, BrainPathfinder **pfPtr);
void brainCoreRegisterWorldSim(struct lua_State *L, BrainWorldSim **wsPtr);
void brainCoreRegisterOverlay(struct lua_State *L, OverlayCmdBuffer **bufPtr);
void brainCoreRegisterVizRegister(struct lua_State *L);
void brainCoreRegisterPanelRegister(struct lua_State *L);
void brainCoreRegisterShotSimPoiRegister(struct lua_State *L);
void brainCoreRegisterVizDetail(struct lua_State *L);
bool brainCoreCallThink(struct lua_State *L, BrainInfo *info, bool *out_killed);
bool brainCoreCallMethod(struct lua_State *L, BrainInfo *info, const char *method);

/* ---- ONNX ML brain (ml_brain.c) ----
 *
 * All three functions are no-ops when compiled without
 * HAVE_ONNXRUNTIME (matching the stub pattern in the
 * internal ml_brain.h). The Singleton wrappers used by the
 * GUI live in luabrainshandler.c on top of these. */
#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)
MLBrainInstance *mlBrainCreate(const char *onnx_path);
bool             mlBrainTick(MLBrainInstance *inst, struct ClientSim *cs, BrainInfo *info);
void             mlBrainDestroy(MLBrainInstance *inst);
#else
static inline MLBrainInstance *mlBrainCreate(const char *onnx_path) {
    (void)onnx_path;
    return NULL;
}
static inline bool mlBrainTick(MLBrainInstance *inst, struct ClientSim *cs, BrainInfo *info) {
    (void)inst; (void)cs; (void)info;
    return false;
}
static inline void mlBrainDestroy(MLBrainInstance *inst) {
    (void)inst;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* BRAIN_LUA_GLUE_H */
