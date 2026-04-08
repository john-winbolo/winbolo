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
 *Name:          ML Brain
 *Filename:      ml_brain.c
 *Purpose:
 *  ONNX Runtime inference for ML-trained tank brains.
 *  Converts BrainInfo → WinBoloObs → ONNX tensors,
 *  runs inference, maps output to tank controls.
 *********************************************************/

#if defined(HAVE_ONNXRUNTIME) && !defined(__EMSCRIPTEN__)

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "ml_brain.h"
#include "obs_builder.h"
#include "../gym/winbolo_gym.h"

#include <onnxruntime_c_api.h>

/* Process-wide ORT environment (expensive to create, shared) */
static const OrtApi    *g_ortApi = NULL;
static OrtEnv          *g_ortEnv = NULL;

/* ── JSONL debug logger ── */
static FILE *g_logFile = NULL;
static int   g_logMapDumped = 0;

static void mlLogOpen(void) {
    if (g_logFile) return;
    g_logFile = fopen("ml_brain_log.jsonl", "w");
    if (g_logFile)
        fprintf(stderr, "[ML] Log opened: ml_brain_log.jsonl\n");
    else
        fprintf(stderr, "[ML] Failed to open log file\n");
}

static void mlLogClose(void) {
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
}

/* Dump the full map once (bounding box of non-deep-sea terrain) */
static void mlLogDumpMap(const BrainInfo *info) {
    if (!g_logFile || g_logMapDumped || !info->theWorld) return;
    g_logMapDumped = 1;

    const TERRAIN *world = info->theWorld;
    int x1 = 255, y1 = 255, x2 = 0, y2 = 0;
    for (int my = 0; my < 256; my++) {
        for (int mx = 0; mx < 256; mx++) {
            TERRAIN t = world[my * 256 + mx] & TERRAIN_MASK;
            if (t != BDEEPSEA) {
                if (mx < x1) x1 = mx;
                if (mx > x2) x2 = mx;
                if (my < y1) y1 = my;
                if (my > y2) y2 = my;
            }
        }
    }
    if (x2 < x1) return;
    /* 1-tile margin */
    if (x1 > 0) x1--;
    if (y1 > 0) y1--;
    if (x2 < 255) x2++;
    if (y2 < 255) y2++;

    fprintf(g_logFile, "{\"type\":\"map\",\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d,\"rows\":[",
            x1, y1, x2, y2);
    for (int my = y1; my <= y2; my++) {
        if (my > y1) fprintf(g_logFile, ",");
        fprintf(g_logFile, "\"");
        for (int mx = x1; mx <= x2; mx++) {
            fprintf(g_logFile, "%02x", world[my * 256 + mx]);
        }
        fprintf(g_logFile, "\"");
    }
    fprintf(g_logFile, "]}\n");
    fflush(g_logFile);
}

/* Dump bases and pillboxes from the WinBoloObs (has structured lists) */
static void mlLogDumpWorld(const WinBoloObs *obs) {
    if (!g_logFile) return;

    fprintf(g_logFile, "{\"type\":\"world\",\"bases\":[");
    for (int i = 0; i < obs->num_bases; i++) {
        if (i > 0) fprintf(g_logFile, ",");
        fprintf(g_logFile, "{\"id\":%d,\"x\":%d,\"y\":%d,\"owner\":%d,\"shells\":%d,\"mines\":%d,\"armour\":%d}",
                i, obs->bases[i].tx, obs->bases[i].ty, obs->bases[i].owner,
                obs->bases[i].shells, obs->bases[i].mines, obs->bases[i].armour);
    }
    fprintf(g_logFile, "],\"pills\":[");
    for (int i = 0; i < obs->num_pillboxes; i++) {
        if (i > 0) fprintf(g_logFile, ",");
        fprintf(g_logFile, "{\"id\":%d,\"x\":%d,\"y\":%d,\"owner\":%d,\"armor\":%d}",
                i, obs->pillboxes[i].tx, obs->pillboxes[i].ty,
                obs->pillboxes[i].owner, obs->pillboxes[i].armor);
    }
    fprintf(g_logFile, "]}\n");
    fflush(g_logFile);
}

/* Per-tick log line */
static void mlLogTick(const BrainInfo *info, const WinBoloObs *obs,
                      int throttle, int steering, int shoot, int mine,
                      int gun_range, int build_act, int build_rx_i, int build_ry_i) {
    if (!g_logFile) return;

    int tank_mx = info->tankx >> 8;
    int tank_my = info->tanky >> 8;

    fprintf(g_logFile,
        "{\"type\":\"tick\",\"t\":%u,"
        "\"tx\":%u,\"ty\":%u,\"mx\":%d,\"my\":%d,"
        "\"dir\":%d,\"spd\":%d,\"boat\":%d,"
        "\"arm\":%d,\"sh\":%d,\"mi\":%d,\"tr\":%d,\"cpill\":%d,"
        "\"accel\":%d,\"steer\":%d,\"shoot\":%d,\"mine\":%d,\"grange\":%d,"
        "\"bact\":%d,\"brx\":%d,\"bry\":%d,"
        "\"dead\":%d,\"nent\":%d",
        obs->tick,
        info->tankx, info->tanky, tank_mx, tank_my,
        info->direction, info->speed, info->inboat,
        info->armour, info->shells, info->mines, info->trees, info->carriedpills,
        throttle, steering, shoot, mine, gun_range,
        build_act, build_rx_i - 14, build_ry_i - 14,
        obs->dead, obs->num_entities);

    /* Entities — only log nearby ones (within 10 tiles) to keep lines reasonable */
    fprintf(g_logFile, ",\"ents\":[");
    int first = 1;
    for (int i = 0; i < obs->num_entities && i < WBGYM_MAX_ENTITIES; i++) {
        const WinBoloEntity *e = &obs->entities[i];
        if (e->rx * e->rx + e->ry * e->ry > 100.0f) continue; /* skip far entities */
        if (!first) fprintf(g_logFile, ",");
        first = 0;
        fprintf(g_logFile, "{\"t\":%d,\"a\":%d,\"rx\":%.1f,\"ry\":%.1f,\"str\":%.2f}",
                e->type, e->allegiance, e->rx, e->ry, e->strength);
    }
    fprintf(g_logFile, "]");

    /* Base ownership snapshot (compact) */
    fprintf(g_logFile, ",\"bases\":[");
    for (int i = 0; i < obs->num_bases; i++) {
        if (i > 0) fprintf(g_logFile, ",");
        fprintf(g_logFile, "%d", obs->bases[i].owner);
    }
    fprintf(g_logFile, "]");

    /* Pill ownership snapshot (compact) */
    fprintf(g_logFile, ",\"pills\":[");
    for (int i = 0; i < obs->num_pillboxes; i++) {
        if (i > 0) fprintf(g_logFile, ",");
        fprintf(g_logFile, "%d", obs->pillboxes[i].owner);
    }
    fprintf(g_logFile, "]");

    fprintf(g_logFile, "}\n");

    /* Flush every 50 ticks for performance */
    if (obs->tick % 50 == 0) fflush(g_logFile);
}

/* Tensor dimensions */
#define SPATIAL_H    WBGYM_SPATIAL_SIZE   /* 29 */
#define SPATIAL_W    WBGYM_SPATIAL_SIZE   /* 29 */
#define TERRAIN_C    2                    /* terrain type + mines */
#define SCALAR_DIM   WBGYM_NUM_SCALARS   /* 26 */
#define ENTITY_DIM   7                   /* rx, ry, type/15, allegiance/3, direction, speed, strength */
#define SOUND_DIM    4                   /* rx/40, ry/40, type/6, allegiance/3 */
#define MAX_ENTITIES WBGYM_MAX_ENTITIES  /* 256 */
#define MAX_SOUNDS   WBGYM_MAX_SOUNDS    /* 32 */
#define LOGITS_DIM   77                  /* MultiDiscrete [3,3,2,2,3,6,29,29] */
#define NUM_INPUTS   6

struct MLBrainInstance {
    OrtSession        *session;
    OrtSessionOptions *sessionOpts;
    OrtMemoryInfo     *memInfo;

    /* Pre-allocated input buffers */
    float  terrain[1 * SPATIAL_H * SPATIAL_W * TERRAIN_C];  /* [1,29,29,2] */
    float  scalar[1 * SCALAR_DIM];                          /* [1,26] */
    float  entities[1 * MAX_ENTITIES * ENTITY_DIM];         /* [1,256,7] */
    float  sounds[1 * MAX_SOUNDS * SOUND_DIM];              /* [1,32,4] */
    float  entity_mask[1 * MAX_ENTITIES];                   /* [1,256] */
    float  sound_mask[1 * MAX_SOUNDS];                      /* [1,32] */

    /* Pre-allocated output buffer */
    float  logits[LOGITS_DIM];
};

static bool ensureOrtEnv(void) {
    if (g_ortApi != NULL) return true;

    g_ortApi = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (g_ortApi == NULL) return false;

    OrtStatus *status = g_ortApi->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "WinBolo", &g_ortEnv);
    if (status != NULL) {
        g_ortApi->ReleaseStatus(status);
        g_ortApi = NULL;
        return false;
    }
    return true;
}

#define ORT_CHECK(expr) do { \
    OrtStatus *_s = (expr); \
    if (_s != NULL) { \
        fprintf(stderr, "ONNX Runtime error: %s\n", g_ortApi->GetErrorMessage(_s)); \
        g_ortApi->ReleaseStatus(_s); \
        goto cleanup; \
    } \
} while(0)

MLBrainInstance *mlBrainCreate(const char *onnx_path) {
    if (!ensureOrtEnv()) return NULL;

    MLBrainInstance *inst = (MLBrainInstance *)calloc(1, sizeof(MLBrainInstance));
    if (inst == NULL) return NULL;

    ORT_CHECK(g_ortApi->CreateSessionOptions(&inst->sessionOpts));
    ORT_CHECK(g_ortApi->SetIntraOpNumThreads(inst->sessionOpts, 1));
    ORT_CHECK(g_ortApi->SetSessionGraphOptimizationLevel(inst->sessionOpts, ORT_ENABLE_BASIC));

#ifdef _WIN32
    /* Windows uses wide strings for model paths */
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, onnx_path, -1, NULL, 0);
        wchar_t *wpath = (wchar_t *)malloc(wlen * sizeof(wchar_t));
        if (wpath == NULL) goto cleanup;
        MultiByteToWideChar(CP_UTF8, 0, onnx_path, -1, wpath, wlen);
        OrtStatus *s = g_ortApi->CreateSession(g_ortEnv, wpath, inst->sessionOpts, &inst->session);
        free(wpath);
        if (s != NULL) {
            fprintf(stderr, "ONNX Runtime error: %s\n", g_ortApi->GetErrorMessage(s));
            g_ortApi->ReleaseStatus(s);
            goto cleanup;
        }
    }
#else
    ORT_CHECK(g_ortApi->CreateSession(g_ortEnv, onnx_path, inst->sessionOpts, &inst->session));
#endif

    ORT_CHECK(g_ortApi->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &inst->memInfo));

    /* Open JSONL logger */
    mlLogOpen();

    return inst;

cleanup:
    if (inst->session) g_ortApi->ReleaseSession(inst->session);
    if (inst->sessionOpts) g_ortApi->ReleaseSessionOptions(inst->sessionOpts);
    if (inst->memInfo) g_ortApi->ReleaseMemoryInfo(inst->memInfo);
    free(inst);
    return NULL;
}


/* Helper: argmax within a slice of floats */
static int argmax(const float *arr, int n) {
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (arr[i] > arr[best]) best = i;
    }
    return best;
}

bool mlBrainTick(MLBrainInstance *inst, struct ClientSim *cs, BrainInfo *info) {
    if (inst == NULL || inst->session == NULL) return false;

    /* Zero variable-length buffers before filling */
    memset(inst->entities, 0, sizeof(inst->entities));
    memset(inst->entity_mask, 0, sizeof(inst->entity_mask));
    memset(inst->sounds, 0, sizeof(inst->sounds));
    memset(inst->sound_mask, 0, sizeof(inst->sound_mask));

    /* Build multi-view observation from BrainInfo + ClientSim */
    WinBoloObs obs;
    obsBuildMultiView(cs, info, &obs);

    /* Terrain: interleave terrain + mines into [29][29][2] */
    for (int r = 0; r < SPATIAL_H; r++) {
        for (int c = 0; c < SPATIAL_W; c++) {
            inst->terrain[(r * SPATIAL_W + c) * 2 + 0] = obs.terrain[r][c];
            inst->terrain[(r * SPATIAL_W + c) * 2 + 1] = obs.mines_map[r][c];
        }
    }

    /* Scalars */
    memcpy(inst->scalar, obs.scalar, sizeof(float) * SCALAR_DIM);

    /* Entities */
    for (int i = 0; i < obs.num_entities && i < MAX_ENTITIES; i++) {
        WinBoloEntity *e = &obs.entities[i];
        float *row = &inst->entities[i * ENTITY_DIM];
        row[0] = e->rx;
        row[1] = e->ry;
        row[2] = (float)e->type / 6.0f;
        row[3] = (float)e->allegiance / 3.0f;
        row[4] = e->direction;
        row[5] = e->speed;
        row[6] = e->strength;
        inst->entity_mask[i] = 1.0f;
    }

    /* Sounds */
    for (int i = 0; i < obs.num_sounds && i < MAX_SOUNDS; i++) {
        WinBoloSoundEvent *s = &obs.sounds[i];
        float *row = &inst->sounds[i * SOUND_DIM];
        row[0] = s->rx / 40.0f;
        row[1] = s->ry / 40.0f;
        row[2] = (float)s->type / 6.0f;
        row[3] = (float)s->allegiance / 3.0f;
        inst->sound_mask[i] = 1.0f;
    }

    /* Create ORT tensors */
    OrtValue *inputTensors[NUM_INPUTS] = {NULL};
    OrtValue *outputTensor = NULL;

    int64_t terrainShape[]    = {1, SPATIAL_H, SPATIAL_W, TERRAIN_C};
    int64_t scalarShape[]     = {1, SCALAR_DIM};
    int64_t entitiesShape[]   = {1, MAX_ENTITIES, ENTITY_DIM};
    int64_t entityMaskShape[] = {1, MAX_ENTITIES};
    int64_t soundsShape[]     = {1, MAX_SOUNDS, SOUND_DIM};
    int64_t soundMaskShape[]  = {1, MAX_SOUNDS};

    ORT_CHECK(g_ortApi->CreateTensorWithDataAsOrtValue(
        inst->memInfo, inst->terrain, sizeof(inst->terrain),
        terrainShape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensors[0]));

    ORT_CHECK(g_ortApi->CreateTensorWithDataAsOrtValue(
        inst->memInfo, inst->scalar, sizeof(inst->scalar),
        scalarShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensors[1]));

    ORT_CHECK(g_ortApi->CreateTensorWithDataAsOrtValue(
        inst->memInfo, inst->entities, sizeof(inst->entities),
        entitiesShape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensors[2]));

    ORT_CHECK(g_ortApi->CreateTensorWithDataAsOrtValue(
        inst->memInfo, inst->entity_mask, sizeof(inst->entity_mask),
        entityMaskShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensors[3]));

    ORT_CHECK(g_ortApi->CreateTensorWithDataAsOrtValue(
        inst->memInfo, inst->sounds, sizeof(inst->sounds),
        soundsShape, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensors[4]));

    ORT_CHECK(g_ortApi->CreateTensorWithDataAsOrtValue(
        inst->memInfo, inst->sound_mask, sizeof(inst->sound_mask),
        soundMaskShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensors[5]));

    /* Run inference */
    const char *inputNames[]  = {"terrain", "scalar", "entities", "entity_mask", "sounds", "sound_mask"};
    const char *outputNames[] = {"logits"};

    ORT_CHECK(g_ortApi->Run(inst->session, NULL,
                            inputNames, (const OrtValue *const *)inputTensors, NUM_INPUTS,
                            outputNames, 1, &outputTensor));

    /* Extract output logits */
    float *outputData = NULL;
    ORT_CHECK(g_ortApi->GetTensorMutableData(outputTensor, (void **)&outputData));
    memcpy(inst->logits, outputData, sizeof(float) * LOGITS_DIM);

    /* Release tensors */
    for (int i = 0; i < NUM_INPUTS; i++) {
        if (inputTensors[i]) g_ortApi->ReleaseValue(inputTensors[i]);
    }
    g_ortApi->ReleaseValue(outputTensor);
    outputTensor = NULL;

    /* Decode actions: MultiDiscrete [3,3,2,2,3,6,29,29] */
    int throttle   = argmax(&inst->logits[0], 3);   /* offset 0:  3 */
    int steering   = argmax(&inst->logits[3], 3);   /* offset 3:  3 */
    int shoot      = argmax(&inst->logits[6], 2);   /* offset 6:  2 */
    int mine       = argmax(&inst->logits[8], 2);   /* offset 8:  2 */
    int gun_range  = argmax(&inst->logits[10], 3);  /* offset 10: 3 */
    int build_act  = argmax(&inst->logits[13], 6);  /* offset 13: 6 */
    int build_rx_i = argmax(&inst->logits[19], 29); /* offset 19: 29 */
    int build_ry_i = argmax(&inst->logits[48], 29); /* offset 48: 29 */

    /* JSONL logging */
    if (!g_logMapDumped) {
        mlLogDumpMap(info);
        mlLogDumpWorld(&obs);
    }
    mlLogTick(info, &obs, throttle, steering, shoot, mine,
              gun_range, build_act, build_rx_i, build_ry_i);

    uint32_t holdkeys = 0;
    uint32_t tapkeys = 0;

    if (throttle == 2) setkey(holdkeys, KEY_faster);
    if (throttle == 0) setkey(holdkeys, KEY_slower);
    if (steering == 0) setkey(holdkeys, KEY_turnleft);
    if (steering == 2) setkey(holdkeys, KEY_turnright);
    if (shoot == 1)    setkey(tapkeys, KEY_shoot);
    if (mine == 1)     setkey(tapkeys, KEY_dropmine);

    if (gun_range == 2) setkey(tapkeys, KEY_morerange);
    if (gun_range == 0) setkey(tapkeys, KEY_lessrange);

    *(info->holdkeys) = holdkeys;
    *(info->tapkeys) = tapkeys;

    /* Build action */
    if (build_act > 0 && info->build != NULL) {
        int tank_tx = info->tankx >> 8;
        int tank_ty = info->tanky >> 8;
        int build_rx = build_rx_i - 14;
        int build_ry = build_ry_i - 14;
        int abs_x = tank_tx + build_rx;
        int abs_y = tank_ty + build_ry;
        if (abs_x < 0) abs_x = 0;
        if (abs_x > 255) abs_x = 255;
        if (abs_y < 0) abs_y = 0;
        if (abs_y > 255) abs_y = 255;
        info->build->action = (BUILDMODE)build_act;
        info->build->x = (MAP_X)abs_x;
        info->build->y = (MAP_Y)abs_y;
    }

    return true;

cleanup:
    for (int i = 0; i < NUM_INPUTS; i++) {
        if (inputTensors[i]) g_ortApi->ReleaseValue(inputTensors[i]);
    }
    if (outputTensor) g_ortApi->ReleaseValue(outputTensor);
    return false;
}

void mlBrainDestroy(MLBrainInstance *inst) {
    if (inst == NULL) return;
    mlLogClose();
    if (inst->session) g_ortApi->ReleaseSession(inst->session);
    if (inst->sessionOpts) g_ortApi->ReleaseSessionOptions(inst->sessionOpts);
    if (inst->memInfo) g_ortApi->ReleaseMemoryInfo(inst->memInfo);
    free(inst);
}

#endif /* HAVE_ONNXRUNTIME && !__EMSCRIPTEN__ */
