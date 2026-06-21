/*
 * Copyright (c) 1998-2026 John Morrison.
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
 *Name:          ML Brain
 *Filename:      ml_brain.h
 *Purpose:
 *  ONNX Runtime inference wrapper for ML-trained brains.
 *  Loads .onnx model files and runs per-tick inference
 *  to produce tank controls from BrainInfo observations.
 *
 *  All functions are no-ops when compiled without
 *  HAVE_ONNXRUNTIME.
 *********************************************************/

#ifndef ML_BRAIN_H
#define ML_BRAIN_H

#include "brain.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;
typedef struct MLBrainInstance MLBrainInstance;

#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)

/*********************************************************
 *NAME:          mlBrainCreate
 *PURPOSE:
 *  Creates an ML brain instance from an ONNX model file.
 *  Initializes the process-wide OrtEnv on first call.
 *  Returns NULL on failure.
 *
 *ARGUMENTS:
 *  onnx_path - Path to the .onnx model file
 *********************************************************/
MLBrainInstance *mlBrainCreate(const char *onnx_path);

/*********************************************************
 *NAME:          mlBrainTick
 *PURPOSE:
 *  Runs one inference cycle: builds multi-view observations
 *  from BrainInfo + ClientSim, runs the ONNX model, and
 *  writes outputs to holdkeys/tapkeys/build in the BrainInfo.
 *  Returns false on inference error.
 *
 *ARGUMENTS:
 *  inst - ML brain instance
 *  cs   - ClientSim pointer for multi-view obs gathering
 *  info - BrainInfo with populated inputs; outputs written
 *         to holdkeys/tapkeys/build
 *********************************************************/
bool mlBrainTick(MLBrainInstance *inst, struct ClientSim *cs, BrainInfo *info);

/*********************************************************
 *NAME:          mlBrainDestroy
 *PURPOSE:
 *  Destroys an ML brain instance and frees resources.
 *  The process-wide OrtEnv is never freed.
 *
 *ARGUMENTS:
 *  inst - Instance to destroy (may be NULL)
 *********************************************************/
void mlBrainDestroy(MLBrainInstance *inst);

#else /* !HAVE_ONNXRUNTIME || __EMSCRIPTEN__ */

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

#endif /* HAVE_ONNXRUNTIME */

#ifdef __cplusplus
}
#endif

#endif /* ML_BRAIN_H */
