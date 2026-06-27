#ifndef GH_OPT_LOG_H
#define GH_OPT_LOG_H

#include <lua.h>

/*
 * gh_opt_log — threaded optimize.log writer.
 *
 * Moves file I/O off the Lua tick thread so log writes don't stall the bot.
 * The main thread enqueues strings; a background thread drains them to disk.
 *
 * Lua API (registered as global table "gh_opt_log"):
 *   gh_opt_log.open(path)          -- open main log file, start writer thread
 *   gh_opt_log.write(text)         -- enqueue text for main log (no added newline)
 *   gh_opt_log.append(path, text)  -- enqueue text + "\n" to an arbitrary file
 *   gh_opt_log.close()             -- flush remaining queue, stop thread, close file
 */

void naOptLogRegister(lua_State *L);

#endif /* GH_OPT_LOG_H */
