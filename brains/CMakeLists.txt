# brains/CMakeLists.txt
#
# Compiles bot-specific Lua C bindings (brains/<bot>/c/*.c) once into
# bot_brains_static, instead of recompiling them per-binary. Every
# binary that links luabrainshandler.c links this library to pull in
# the per-tick registrations and bot-specific header search paths.
# ONE brain's c/ directory is compiled: WINBOLO_BRAIN_C_DIR (default the
# active GoalHunter). Only one brain version ships now, so the glob has a
# single candidate -- but the variable stays, and stays a single directory,
# because of what happened when several versions were in the tree: each
# frozen older brain carried its own c/ copy with the SAME symbol names
# (naShieldStampRegister, naThreatRegister, ...). Globbing every version
# compiled them all into this lib, the linker kept the FIRST copy (LNK4006
# "second definition ignored") and silently dropped the rest -- so every
# brain, including the active one, ran the oldest version's bindings, and an
# edit to the active version's c/ never reached the binary. Anyone who
# freezes a second brain tree again must keep this one-directory rule.
set(WINBOLO_BRAIN_C_DIR "GoalHunter" CACHE STRING
    "brains/<dir>/c is the one set of brain C bindings compiled and linked (all brain versions share it)")
# The brain directory was brains/GoalHunter_1.7 until it lost its version
# suffix. A build directory configured before that still caches the old
# name, so move it to the new one while no GoalHunter_1.7 tree exists.
if(WINBOLO_BRAIN_C_DIR STREQUAL "GoalHunter_1.7" AND
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/GoalHunter_1.7/c")
    message(STATUS "WINBOLO_BRAIN_C_DIR: GoalHunter_1.7 is now GoalHunter")
    set(WINBOLO_BRAIN_C_DIR "GoalHunter" CACHE STRING
        "brains/<dir>/c is the one set of brain C bindings compiled and linked (all brain versions share it)"
        FORCE)
endif()

file(GLOB BOT_C_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${WINBOLO_BRAIN_C_DIR}/c/*.c")
if(NOT BOT_C_SOURCES)
    message(FATAL_ERROR "brains/${WINBOLO_BRAIN_C_DIR}/c has no .c sources (WINBOLO_BRAIN_C_DIR)")
endif()

add_library(bot_brains_static STATIC ${BOT_C_SOURCES})

bolo_apply_include_rules(bot_brains_static sim_owner)
set_target_properties(bot_brains_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON)

# Per-bot include directories derived from the source list. Exported
# as PUBLIC so consumers (luabrainshandler.c, braintest_main.c) can
# include the bot-specific headers without long relative paths.
set(_bot_c_include_dirs "")
foreach(_bot_c_src ${BOT_C_SOURCES})
    get_filename_component(_bot_c_dir "${_bot_c_src}" DIRECTORY)
    list(APPEND _bot_c_include_dirs "${_bot_c_dir}")
endforeach()
list(REMOVE_DUPLICATES _bot_c_include_dirs)
target_include_directories(bot_brains_static PUBLIC ${_bot_c_include_dirs})

# Brain C bindings call into bolo's sim, lua's C API, and (for
# gh_shield_stamp.c) SDL3. PUBLIC so consumers inherit the link
# without repeating it.
target_link_libraries(bot_brains_static PUBLIC
    bolo_static
    lua_static
    SDL3::SDL3)

if(EMSCRIPTEN)
    target_compile_options(bot_brains_static PRIVATE -fno-common -include sys/types.h)
elseif(NOT MSVC)
    target_compile_options(bot_brains_static PRIVATE -fcommon)
endif()
