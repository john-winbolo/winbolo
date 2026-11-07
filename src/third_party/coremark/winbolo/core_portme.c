/*
 * CoreMark port for WinBolo's CPU speed test. See core_portme.h.
 *
 * Written for WinBolo. Licensed under the Apache License, Version 2.0, as
 * the rest of this directory is; see ../LICENSE.md.
 */
#include <stdlib.h>
#include <SDL3/SDL.h>

#include "coremark.h"

/* The performance run's seeds (CoreMark's PERFORMANCE_RUN values). They are
   volatile so that the compiler reads them at run time. */
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
/* The iteration count and the algorithm mask; 0 means "all algorithms" to
   core_main.c's main. The speed test sets the counts in core_results. */
volatile ee_s32 seed4_volatile = 0;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

static CORE_TICKS startTicks;
static CORE_TICKS stopTicks;

void *portable_malloc(ee_size_t size) {
  return malloc(size);
}

void portable_free(void *p) {
  free(p);
}

void start_time(void) {
  startTicks = (CORE_TICKS)SDL_GetTicksNS();
}

void stop_time(void) {
  stopTicks = (CORE_TICKS)SDL_GetTicksNS();
}

CORE_TICKS get_time(void) {
  return stopTicks - startTicks;
}

secs_ret time_in_secs(CORE_TICKS ticks) {
  return (secs_ret)ticks / 1e9;
}

void portable_init(core_portable *p, int *argc, char *argv[]) {
  (void)argc;
  (void)argv;
  p->portable_id = 1;
}

void portable_fini(core_portable *p) {
  p->portable_id = 0;
}
