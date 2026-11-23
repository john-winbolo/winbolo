# Make LuaJIT's msvcbuild.bat build without ffi.
#
# BOLO_LUAJIT_LOCKDOWN_XCFLAGS compiles ffi out with LUAJIT_DISABLE_FFI, which
# reaches every C compile through the CL environment variable. The interpreter
# is not C: DynASM generates it from vm_x64.dasc / vm_x86.dasc, and
# msvcbuild.bat passes DynASM a hard-coded `-D FFI`, so the interpreter would
# still be generated with its ffi parts while the C side has none. LuaJIT's
# Makefile works this out from the C defines; the bat does not.
#
# Run with -P from the LuaJIT source dir, as the first command of the MSVC
# build step, not as a FetchContent PATCH_COMMAND: a patch command runs only
# when the source is first fetched, and a tree that fetched LuaJIT before this
# file existed has to be mended too.
#
# Idempotent: an already-patched bat is left alone, and it fails only if the
# bat has neither form (the pin moved and this needs another look).
set(_f "src/msvcbuild.bat")
file(READ "${_f}" _c)
string(FIND "${_c}" "-D JIT -D FFI -D ENDIAN_LE" _at)
if(NOT _at EQUAL -1)
    string(REPLACE "-D JIT -D FFI -D ENDIAN_LE" "-D JIT -D ENDIAN_LE" _c "${_c}")
    # file(READ) drops the carriage returns, and cmd needs them back: it
    # mis-reads labels in a batch file with bare line feeds. The bat has no
    # @NAME@ text for @ONLY to replace.
    file(CONFIGURE OUTPUT "${_f}" CONTENT "${_c}" @ONLY NEWLINE_STYLE CRLF)
    message(STATUS "LuaJIT: took -D FFI out of msvcbuild.bat's DynASM flags")
else()
    string(FIND "${_c}" "-D JIT -D ENDIAN_LE" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "LuaJIT patch luajit_msvc_no_ffi no longer matches "
                            "msvcbuild.bat -- the pinned commit changed; check "
                            "how the bat sets DASMFLAGS and update this patch.")
    endif()
endif()
