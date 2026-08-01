# Fix a per-thread W^X leak in pinned LuaJIT faaf6633's macOS hardened-
# runtime (LUAJIT_ENABLE_OSX_HRT) path — the cause of a shipped 2.02
# SIGBUS (exec fault in trace mcode on a bot worker thread; Sentry event
# 24dae975, macOS 26.5 arm64).
#
# Under the HRT, mcode_setprot() maps to pthread_jit_write_protect_np(),
# which flips a per-THREAD CPU register — unlike mprotect(), protection
# is not a property of the pages. Two places in stock lj_mcode.c still
# assume the mprotect model:
#
#   1. mcode_protect() skips the toggle when the J->mcprot cache already
#      matches. The cache can say "executable" while the CURRENT thread
#      is still in write mode (the lua_State migrated threads, or an
#      error path skipped a restore) — the skip then leaves the thread
#      unable to execute any mcode. Fix: under MCMAP_CREATE, always
#      toggle (it is a cheap register write, not a syscall).
#
#   2. mcode_alloc()'s placement-probe loop enables write mode on every
#      successful mmap (mcode_alloc_at), frees badly placed areas, and
#      on exhaustion longjmps out via lj_trace_err() — with the thread
#      left in write mode and no J->mcprot record, so the cache-guarded
#      restore in lj_mcode_abort() is skipped. The thread SIGBUSes on
#      its next trace entry. Fix: re-arm execute mode on the failure
#      path before the longjmp.
#
# Run as a FetchContent PATCH_COMMAND with -P from the LuaJIT source dir,
# after luajit_fix_osx_hrt_return.cmake (the two touch disjoint regions).
# Idempotent: each replacement is a no-op on an already-patched tree; we
# fail only if the file matches NEITHER the broken nor the fixed form of
# a fix (i.e. the pin moved — re-evaluate or delete this patch).
set(_f "src/lj_mcode.c")
file(READ "${_f}" _c)

# -- Fix 1: mcode_protect must not trust the J->mcprot cache under HRT --
set(_broken1 [=[/* Change protection of MCode area. */
static void mcode_protect(jit_State *J, int prot)
{
  if (J->mcprot != prot) {
    mcode_setprot(J, J->mcarea, J->szmcarea, prot);
    J->mcprot = prot;
  }
}]=])
set(_fixed1 [=[/* Change protection of MCode area. */
static void mcode_protect(jit_State *J, int prot)
{
#if MCMAP_CREATE
  /* pthread_jit_write_protect_np() is per-THREAD state, not a property
  ** of the pages, so the J->mcprot cache goes stale whenever this
  ** lua_State runs on another thread or an error path skips a restore.
  ** Always toggle: it is a cheap register write, not a syscall.
  */
  mcode_setprot(J, J->mcarea, J->szmcarea, prot);
  J->mcprot = prot;
#else
  if (J->mcprot != prot) {
    mcode_setprot(J, J->mcarea, J->szmcarea, prot);
    J->mcprot = prot;
  }
#endif
}]=])

string(FIND "${_c}" "${_broken1}" _at)
if(NOT _at EQUAL -1)
    string(REPLACE "${_broken1}" "${_fixed1}" _c "${_c}")
    message(STATUS "LuaJIT: patched mcode_protect cache bypass (lj_mcode.c)")
else()
    string(FIND "${_c}" "${_fixed1}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "LuaJIT patch luajit_fix_osx_hrt_thread_leak "
                            "(fix 1, mcode_protect) no longer matches "
                            "lj_mcode.c — the pinned commit changed; check "
                            "whether upstream fixed the per-thread W^X "
                            "handling and delete or update this patch.")
    endif()
endif()

# -- Fix 2: mcode_alloc failure path must restore execute mode ----------
# Anchor on the unique trailing comment; the other LJ_TRERR_MCODEAL
# throw (lj_mcode_limiterr) runs with no probe allocations outstanding.
set(_broken2 [=[  lj_trace_err(J, LJ_TRERR_MCODEAL);  /* Give up. OS probably ignores hints? */]=])
set(_fixed2 [=[#if MCMAP_CREATE
  /* The freed probe allocations above each enabled JIT write mode on
  ** this thread (mcode_alloc_at), and lj_trace_err() longjmps past
  ** every restore with no J->mcprot record. Re-arm execute mode or
  ** this thread's next trace entry faults with SIGBUS.
  */
  pthread_jit_write_protect_np(1);
#endif
  lj_trace_err(J, LJ_TRERR_MCODEAL);  /* Give up. OS probably ignores hints? */]=])

string(FIND "${_c}" "${_fixed2}" _at)
if(_at EQUAL -1)
    string(FIND "${_c}" "${_broken2}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "LuaJIT patch luajit_fix_osx_hrt_thread_leak "
                            "(fix 2, mcode_alloc fail path) no longer "
                            "matches lj_mcode.c — the pinned commit "
                            "changed; check whether upstream fixed the "
                            "per-thread W^X handling and delete or update "
                            "this patch.")
    endif()
    string(REPLACE "${_broken2}" "${_fixed2}" _c "${_c}")
    message(STATUS "LuaJIT: patched mcode_alloc write-mode leak (lj_mcode.c)")
endif()

file(WRITE "${_f}" "${_c}")
