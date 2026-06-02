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
 *Name:          Bot Worker Pool
 *Filename:      bot_worker_pool.h
 *Author:        John Morrison
 *Purpose:
 *  Fixed-size SDL3-backed thread pool for running per-bot
 *  jobs in parallel. Process-wide singleton; all entry
 *  points are called from the main thread only. The pool
 *  is a generic primitive — it knows nothing about bots
 *  beyond the integer index passed to each job.
 *
 *  Pre-work for the upcoming parallel brain-tick path. No
 *  caller is wired up yet; behaviour of every binary is
 *  unchanged until a future change dispatches work here.
 *********************************************************/

#ifndef BOT_WORKER_POOL_H
#define BOT_WORKER_POOL_H

#include "global.h"

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************
 *NAME:          BotJobFn
 *PURPOSE:
 *  Job function signature. The pool calls this once per
 *  dispatched index, passing the index and the shared
 *  userData pointer supplied to botWorkerPoolRun.
 *
 *  When the pool is uncreated or count<=1 the producer
 *  runs the job inline on its calling thread; otherwise
 *  one job runs inline on the calling thread and the rest
 *  run on worker threads in parallel.
 *********************************************************/
typedef void (*BotJobFn)(int botIndex, void *userData);

/*********************************************************
 *NAME:          botWorkerPoolCreate
 *PURPOSE:
 *  Creates the process-wide pool. Call once at startup
 *  from the main thread, before any botWorkerPoolRun.
 *
 *  workerCount is clamped to [1, MAX_TANKS-1]. A value of
 *  0 leaves the pool uncreated and is a valid no-op state
 *  — callers fall through botWorkerPoolRun's serial path.
 *
 *  On SDL thread/semaphore creation failure the pool is
 *  torn down and left in an uncreated state; subsequent
 *  Run calls still work serially and Destroy is safe.
 *
 *ARGUMENTS:
 *  workerCount - Number of persistent worker threads.
 *
 *RETURNS:
 *  true on success (or when workerCount==0). false if any
 *  SDL primitive could not be created.
 *********************************************************/
bool botWorkerPoolCreate(int workerCount);

/*********************************************************
 *NAME:          botWorkerPoolDestroy
 *PURPOSE:
 *  Tears down all worker threads and frees pool resources.
 *  Call once at shutdown from the main thread, after the
 *  last botWorkerPoolRun has returned. Safe to call when
 *  the pool was never created or creation failed.
 *********************************************************/
void botWorkerPoolDestroy(void);

/*********************************************************
 *NAME:          botWorkerPoolRun
 *PURPOSE:
 *  Dispatches `count` jobs (botIndices[0..count-1]) and
 *  blocks until every one has finished. Must be called
 *  only from the main thread; the pool has a single
 *  producer.
 *
 *  Distribution rule: if the pool is created and count>1,
 *  the first count-1 jobs run on workers and the last job
 *  runs inline on the calling thread. This makes a pool of
 *  size N-1 deliver honest N-way parallelism rather than
 *  leaving the producer idle on the wait.
 *
 *  Fast paths: if the pool is uncreated or count<=1, jobs
 *  run serially on the calling thread without touching any
 *  pool state.
 *
 *ARGUMENTS:
 *  botIndices - Array of `count` job indices.
 *  count      - Number of jobs to run.
 *  jobFn      - Function invoked once per index.
 *  userData   - Opaque pointer forwarded to each job.
 *********************************************************/
void botWorkerPoolRun(const int *botIndices, int count,
                      BotJobFn jobFn, void *userData);

/*********************************************************
 *NAME:          botWorkerPoolGetSize
 *PURPOSE:
 *  Returns the active worker count. Returns 0 if the pool
 *  was never created or creation failed.
 *********************************************************/
int botWorkerPoolGetSize(void);

#ifdef __cplusplus
}
#endif

#endif /* BOT_WORKER_POOL_H */
