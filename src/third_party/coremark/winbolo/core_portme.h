/*
 * CoreMark port for WinBolo's CPU speed test (src/cpu_speed/cpu_speed_test.c).
 *
 * Written for WinBolo, in place of the port directories CoreMark ships
 * (posix/, linux/, macos/ and the others), none of which are taken. It is
 * licensed under the Apache License, Version 2.0, as the rest of this
 * directory is; see ../LICENSE.md.
 *
 * The port runs one context per call. The speed test makes its own threads
 * (SDL threads) and gives each one its own core_results and its own memory,
 * so CoreMark's own parallel layer (fork, pthreads, sockets) is off.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>
#include <stdint.h>

#define HAS_FLOAT   1
#define HAS_TIME_H  1
#define USE_CLOCK   0
#define HAS_STDIO   1
#define HAS_PRINTF  1

/* Nanoseconds, from SDL's clock. Only core_main.c's own main reads it; the
   speed test times its phases itself. */
typedef uint64_t CORE_TICKS;

#ifndef COMPILER_VERSION
#if defined(_MSC_VER)
#define COMPILER_VERSION "MSVC"
#elif defined(__clang__)
#define COMPILER_VERSION "Clang " __clang_version__
#elif defined(__GNUC__)
#define COMPILER_VERSION "GCC " __VERSION__
#else
#define COMPILER_VERSION "unknown"
#endif
#endif
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS "WinBolo Release flags"
#endif
#define MEM_LOCATION "MALLOC"

/* The same types as CoreMark's posix port, which the bench numbers came
   from. */
typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef uintptr_t      ee_ptr_int;
typedef size_t         ee_size_t;
#define NULL_PTR ((void *)0)

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

/* The seeds are read from volatile variables at run time, so the compiler
   cannot fold the work away. The speed test sets them to the performance
   run's values (0, 0, 0x66). */
#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD  MEM_MALLOC

#define MULTITHREAD 1
#define USE_PTHREAD 0
#define USE_FORK    0
#define USE_SOCKET  0

#define MAIN_HAS_NOARGC   0
#define MAIN_HAS_NORETURN 0

typedef struct CORE_PORTABLE_S {
  ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

extern ee_u32 default_num_contexts;

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#define PERFORMANCE_RUN 1
#endif

#endif /* CORE_PORTME_H */
