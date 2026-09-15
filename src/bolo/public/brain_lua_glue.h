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
#include "brain.h"           /* For BrainInfo */
#include "brain_overlay.h"   /* For OverlayCmdBuffer */
#include "scenario_table.h"  /* ScnTable — the brain's init table */

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
void brainCoreSetInitTable(struct lua_State *L, const ScnTable *init);
bool brainCoreUpdateInitTable(struct lua_State *L, const ScnTable *init,
                              char *why, size_t whyLen);
void brainCoreRegisterGetTerrain(struct lua_State *L, const BYTE **worldPtr);
void brainCoreRegisterPathfinder(struct lua_State *L, BrainPathfinder **pfPtr);
/* Registry key brainCoreRegisterPathfinder parks the pathfinder under, and
 * the reader for it. For C that a brain links in beside braincore
 * (brains/<brain>/c): those modules register Lua functions of their own, so
 * they have no cpf_* upvalue, and every shot and movement rule hangs off the
 * pathfinder. NULL before a pathfinder is registered, or after it is torn
 * down; callers must handle that the way the cpf_* wrappers do. */
#define BRAINCORE_PATHFINDER_REGKEY "braincore_pathfinder"
BrainPathfinder *brainCoreGetPathfinder(struct lua_State *L);
void brainCoreRegisterWorldSim(struct lua_State *L, BrainWorldSim **wsPtr);
void brainCoreRegisterOverlay(struct lua_State *L, OverlayCmdBuffer **bufPtr);
void brainCoreRegisterVizRegister(struct lua_State *L);
void brainCoreRegisterPanelRegister(struct lua_State *L);
void brainCoreRegisterShotSimPoiRegister(struct lua_State *L);
void brainCoreRegisterVizDetail(struct lua_State *L);
/* Flush hook the crash-log writer calls before it writes, so a brain's own
 * buffered output reaches disk first. Installed by the same frontend code
 * that registers the bindings above (luabrainshandler.c hands it the brain
 * C library's naOptLogFlushSync), which is why the declaration belongs here
 * and not only on internal/braincore.h — that header is T2 and a frontend
 * never sees it. Full contract documented there. */
void brainCoreSetLogFlushHook(void (*fn)(void));
bool brainCoreCallThink(struct lua_State *L, BrainInfo *info, bool *out_killed);
bool brainCoreCallMethod(struct lua_State *L, BrainInfo *info, const char *method);

/* Per-think Lua allocation counter (braincore.c). Install swaps in a
 * counting allocator after brain.open; Uninstall must run before
 * lua_close so the counter context is freed. */
void brainCoreInstallAllocCounter(struct lua_State *L);
void brainCoreUninstallAllocCounter(struct lua_State *L);

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
