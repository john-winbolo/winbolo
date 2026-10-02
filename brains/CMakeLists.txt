# brains/CMakeLists.txt
#
# Compiles bot-specific Lua C bindings (brains/<bot>/c/*.c) once into
# bot_brains_static, instead of recompiling them per-binary. Every
# binary that links luabrainshandler.c links this library to pull in
# the per-tick registrations and bot-specific header search paths.
# Only the GoalHunter brain's c/ directory is compiled, from a fixed path.
# Keep it to ONE directory. When several brain versions were in the tree,
# each frozen older brain carried its own c/ copy with the SAME symbol names
# (naShieldStampRegister, naThreatRegister, ...). Globbing every version
# compiled them all into this lib, the linker kept the FIRST copy (LNK4006
# "second definition ignored") and silently dropped the rest -- so every
# brain, including the active one, ran the oldest version's bindings, and an
# edit to the active version's c/ never reached the binary. Anyone who adds
# a second brain tree with a c/ directory must keep this one-directory rule.
# The path is not a cache option: a cached value outlives a rename of the
# brain directory in every old build directory.
set(_bot_c_dir_rel "GoalHunter/c")
file(GLOB BOT_C_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/${_bot_c_dir_rel}/*.c")
if(NOT BOT_C_SOURCES)
    message(FATAL_ERROR "brains/${_bot_c_dir_rel} has no .c sources")
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
