# brains/CMakeLists.txt
#
# Compiles bot-specific Lua C bindings (brains/<bot>/c/*.c) once into
# bot_brains_static, instead of recompiling them per-binary. Every
# binary that links luabrainshandler.c links this library to pull in
# the per-tick registrations and bot-specific header search paths.
# Adding a new bot's c/ subdirectory is picked up automatically at
# configure time via CONFIGURE_DEPENDS.

file(GLOB BOT_C_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/c/*.c")

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
# na_shield_stamp.c) SDL3. PUBLIC so consumers inherit the link
# without repeating it.
target_link_libraries(bot_brains_static PUBLIC
    bolo_static
    lua_static
    SDL3::SDL3)

if(NOT MSVC)
    target_compile_options(bot_brains_static PRIVATE -fcommon)
endif()
