# Fix a compile error in pinned LuaJIT faaf6633 (v2.1 tip as of 2026-07):
# under LUAJIT_ENABLE_OSX_HRT, mcode_setprot() — a void function — ends in
# `return 0;` (src/lj_mcode.c), which modern clang rejects outright
# (-Wreturn-mismatch is an error). The path is dead code unless the HRT
# flag is set, which is why only macOS hardened-runtime builds trip it.
#
# Run as a FetchContent PATCH_COMMAND with -P from the LuaJIT source dir.
# Idempotent: string(REPLACE) is a no-op on an already-patched tree, and we
# only fail if the file matches NEITHER the broken nor the fixed form
# (i.e. the pin moved and this patch should be re-evaluated or deleted).
set(_f "src/lj_mcode.c")
file(READ "${_f}" _c)
set(_broken "  pthread_jit_write_protect_np((prot & PROT_EXEC));\n  return 0;")
set(_fixed  "  pthread_jit_write_protect_np((prot & PROT_EXEC));")
string(FIND "${_c}" "${_broken}" _at)
if(NOT _at EQUAL -1)
    string(REPLACE "${_broken}" "${_fixed}" _c "${_c}")
    file(WRITE "${_f}" "${_c}")
    message(STATUS "LuaJIT: patched void-return in mcode_setprot (lj_mcode.c)")
else()
    string(FIND "${_c}" "${_fixed}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "LuaJIT patch luajit_fix_osx_hrt_return no longer "
                            "matches lj_mcode.c — the pinned commit changed; "
                            "check whether upstream fixed mcode_setprot and "
                            "delete or update this patch.")
    endif()
endif()
