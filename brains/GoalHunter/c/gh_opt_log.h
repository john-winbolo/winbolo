#ifndef GH_OPT_LOG_H
#define GH_OPT_LOG_H

#include <lua.h>

/*
 * gh_opt_log — threaded log writer for the brain's optimize.log and print2 logs.
 *
 * Moves file I/O off the Lua tick threads so log writes don't stall a bot.
 * Brain threads enqueue strings; one background thread drains them to disk,
 * keeping each output file open between batches.
 *
 * Lua API (registered as global table "gh_opt_log"):
 *   gh_opt_log.open(path)          -- open main log file, start writer thread
 *   gh_opt_log.ensure()            -- start the writer thread WITHOUT claiming
 *                                     the main log file, for append-only users
 *                                     such as print2
 *   gh_opt_log.write(text)         -- enqueue text for main log (no added newline)
 *   gh_opt_log.append(path, text[, raw])
 *                                  -- enqueue text to an arbitrary file, adding
 *                                     a trailing newline unless raw is true;
 *                                     returns true when it was queued
 *   gh_opt_log.flush()             -- block until the queue is on disk
 *   gh_opt_log.close()             -- block until the queue is on disk. Does NOT
 *                                     stop the thread: it is shared by every
 *                                     brain instance in the process (1.6's
 *                                     optimize.lua closes at Brain.close while
 *                                     current brains are still appending)
 */

void naOptLogRegister(lua_State *L);

/*
 * Block until everything enqueued so far has been written and fflush'd. Safe
 * from any thread and a no-op when the writer isn't running. Installed as
 * braincore's crash-log flush hook (brainCoreSetLogFlushHook) so a brain crash
 * report is not written while the last second of print2 is still queued.
 */
void naOptLogFlushSync(void);

#endif /* GH_OPT_LOG_H */
