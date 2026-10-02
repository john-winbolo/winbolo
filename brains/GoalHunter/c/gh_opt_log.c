/*
 * gh_opt_log.c — threaded log writer for the GoalHunter brain
 *
 * Enqueues log strings from the Lua tick threads and drains them to disk on a
 * background thread, so file I/O never stalls a bot tick.
 *
 * Thread model: N producers, one consumer.
 *   Producers  — the brain threads. With `-threads N` the host runs N bot
 *                ticks concurrently and any of them can push, so this is not
 *                the single-producer queue the original comment described.
 *                It has always been safe: every queue mutation happens under
 *                s_mutex, and the one field producers read outside it
 *                (s_running) is written only under s_open_mutex during
 *                open/ensure/close, which the host serializes.
 *   Consumer   — one writer thread, started lazily by open() / ensure().
 *
 * Two things keep the queue honest:
 *   * Open handles are cached per path (GH_FILE_CACHE entries, LRU). print2
 *     writes ~45 lines per think per bot; with four bots, reopening the file
 *     for every append was ~9,000 fopen/fclose pairs a second.
 *   * The queue is bounded (GH_QUEUE_MAX_*). A producer that runs past the
 *     bound waits for the writer to catch up, so a hard process kill loses at
 *     most one bound's worth of lines rather than an unbounded backlog, and a
 *     runaway producer cannot eat memory.
 *
 * naOptLogFlushSync() blocks until everything enqueued so far is on disk; the
 * brain-crash path calls it through the hook braincore.c installs, so a crash
 * report is not written while the last seconds of print2 are still in the
 * queue.
 */

#include "gh_opt_log.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
   typedef CRITICAL_SECTION  gh_mutex_t;
   typedef CONDITION_VARIABLE gh_cond_t;
   typedef HANDLE             gh_thread_t;
   static void gh_mutex_init   (gh_mutex_t *m) { InitializeCriticalSection(m); }
   static void gh_mutex_lock   (gh_mutex_t *m) { EnterCriticalSection(m); }
   static void gh_mutex_unlock (gh_mutex_t *m) { LeaveCriticalSection(m); }
   static void gh_mutex_destroy(gh_mutex_t *m) { DeleteCriticalSection(m); }
   static void gh_cond_init    (gh_cond_t  *c) { InitializeConditionVariable(c); }
   static void gh_cond_signal  (gh_cond_t  *c) { WakeConditionVariable(c); }
   static void gh_cond_broadcast(gh_cond_t *c) { WakeAllConditionVariable(c); }
   static void gh_cond_destroy (gh_cond_t  *c) { (void)c; }
   static void gh_cond_wait    (gh_cond_t *c, gh_mutex_t *m) {
       SleepConditionVariableCS(c, m, INFINITE);
   }
   static DWORD WINAPI writer_thread_fn(LPVOID arg);
   static void gh_thread_start(gh_thread_t *t) {
       *t = CreateThread(NULL, 0, writer_thread_fn, NULL, 0, NULL);
   }
   static void gh_thread_join(gh_thread_t *t) {
       WaitForSingleObject(*t, INFINITE);
       CloseHandle(*t);
   }
#  define gh_strdup _strdup
#else
#  include <pthread.h>
   typedef pthread_mutex_t gh_mutex_t;
   typedef pthread_cond_t  gh_cond_t;
   typedef pthread_t       gh_thread_t;
   static void gh_mutex_init   (gh_mutex_t *m) { pthread_mutex_init(m, NULL); }
   static void gh_mutex_lock   (gh_mutex_t *m) { pthread_mutex_lock(m); }
   static void gh_mutex_unlock (gh_mutex_t *m) { pthread_mutex_unlock(m); }
   static void gh_mutex_destroy(gh_mutex_t *m) { pthread_mutex_destroy(m); }
   static void gh_cond_init    (gh_cond_t  *c) { pthread_cond_init(c, NULL); }
   static void gh_cond_signal  (gh_cond_t  *c) { pthread_cond_signal(c); }
   static void gh_cond_broadcast(gh_cond_t *c) { pthread_cond_broadcast(c); }
   static void gh_cond_destroy (gh_cond_t  *c) { pthread_cond_destroy(c); }
   static void gh_cond_wait    (gh_cond_t *c, gh_mutex_t *m) {
       pthread_cond_wait(c, m);
   }
   static void *writer_thread_fn(void *arg);
   static void gh_thread_start(gh_thread_t *t) {
       pthread_create(t, NULL, writer_thread_fn, NULL);
   }
   static void gh_thread_join(gh_thread_t *t) { pthread_join(*t, NULL); }
   static char *gh_strdup(const char *s) {
       size_t n = strlen(s) + 1;
       char *p = (char *)malloc(n);
       if (p) memcpy(p, s, n);
       return p;
   }
#endif

/* Queue bound. print2 batches a whole second of lines into ONE entry, so
 * entries are few and large — bound on both counts. 64 entries / 8 MB is
 * roughly a second of four-bot debug output, which is what a hard kill of the
 * process may lose. */
#define GH_QUEUE_MAX_ENTRIES 64
#define GH_QUEUE_MAX_BYTES   (8u * 1024u * 1024u)

/* Output files kept open between entries: one per bot for print2, plus
 * optimize.log and the occasional one-shot diagnostic file. */
#define GH_FILE_CACHE 8

/* ── Queue ────────────────────────────────────────────────────────────── */
typedef struct QEntry {
    char          *path;   /* destination file; always set (write() resolves
                              the main log's path on the producer side) */
    char          *text;
    size_t         len;
    int            newline; /* append a "\n" after text */
    struct QEntry *next;
} QEntry;

static gh_mutex_t  s_mutex;
static gh_cond_t   s_cond;       /* writer waits here for work */
static gh_cond_t   s_drained;    /* producers / flushers wait here for room */
static QEntry     *s_head     = NULL;
static QEntry     *s_tail     = NULL;
static size_t      s_count    = 0;
static size_t      s_bytes    = 0;
static int         s_shutdown = 0;
static int         s_writing  = 0;   /* writer holds a batch that is not on
                                        disk yet */
static gh_thread_t s_thread;
static int         s_running  = 0;
static char        s_main_path[2048] = "";

/* Serializes l_open / l_ensure / l_close so concurrent brains can't race the
   writer-thread lifecycle (s_running, s_thread, s_mutex, s_cond). */
static gh_mutex_t  s_open_mutex;
static int         s_open_mutex_inited = 0;

/* Push one entry onto the queue (called from a brain thread, no lock held).
 * Waits while the queue is over its bound, which is what keeps a crash's
 * losses bounded. */
static void queue_push(char *path, char *text, size_t len, int newline) {
    QEntry *e = (QEntry *)malloc(sizeof(QEntry));
    if (!e || !path) { free(e); free(path); free(text); return; }
    e->path    = path;
    e->text    = text;
    e->len     = len;
    e->newline = newline;
    e->next    = NULL;
    gh_mutex_lock(&s_mutex);
    while (!s_shutdown &&
           (s_count >= GH_QUEUE_MAX_ENTRIES || s_bytes >= GH_QUEUE_MAX_BYTES)) {
        gh_cond_signal(&s_cond);
        gh_cond_wait(&s_drained, &s_mutex);
    }
    if (s_tail) s_tail->next = e; else s_head = e;
    s_tail = e;
    s_count++;
    s_bytes += len;
    gh_cond_signal(&s_cond);
    gh_mutex_unlock(&s_mutex);
}

/* ── Writer thread ────────────────────────────────────────────────────── */

/* Small LRU of open output handles so an append does not fopen/fclose. Touched
 * by the writer thread only, so it needs no lock of its own. */
typedef struct {
    char         *path;
    FILE         *fp;
    unsigned long used;   /* monotonic stamp; largest = most recently used */
} GhOpenFile;   /* not "OpenFile": that is a windows.h function */

static GhOpenFile s_files[GH_FILE_CACHE];
static unsigned long s_use_clock = 0;

static FILE *file_for(const char *path) {
    int i, slot = -1;
    for (i = 0; i < GH_FILE_CACHE; i++) {
        if (s_files[i].path && strcmp(s_files[i].path, path) == 0) {
            s_files[i].used = ++s_use_clock;
            return s_files[i].fp;
        }
    }
    /* Miss: take a free slot, else evict the least recently used one. */
    for (i = 0; i < GH_FILE_CACHE; i++) {
        if (!s_files[i].path) { slot = i; break; }
        if (slot < 0 || s_files[i].used < s_files[slot].used) slot = i;
    }
    if (s_files[slot].fp) fclose(s_files[slot].fp);
    free(s_files[slot].path);
    s_files[slot].path = NULL;
    s_files[slot].fp   = fopen(path, "a");
    if (!s_files[slot].fp) return NULL;
    s_files[slot].path = gh_strdup(path);
    if (!s_files[slot].path) {
        fclose(s_files[slot].fp);
        s_files[slot].fp = NULL;
        return NULL;
    }
    s_files[slot].used = ++s_use_clock;
    return s_files[slot].fp;
}

static void files_flush(void) {
    int i;
    for (i = 0; i < GH_FILE_CACHE; i++)
        if (s_files[i].fp) fflush(s_files[i].fp);
}

static void files_close(void) {
    int i;
    for (i = 0; i < GH_FILE_CACHE; i++) {
        if (s_files[i].fp) fclose(s_files[i].fp);
        free(s_files[i].path);
        s_files[i].fp   = NULL;
        s_files[i].path = NULL;
    }
}

#ifdef _WIN32
static DWORD WINAPI writer_thread_fn(LPVOID arg) {
#else
static void *writer_thread_fn(void *arg) {
#endif
    (void)arg;

    for (;;) {
        QEntry *batch, *e;
        int stop;
        gh_mutex_lock(&s_mutex);
        while (!s_head && !s_shutdown)
            gh_cond_wait(&s_cond, &s_mutex);
        batch     = s_head;
        s_head    = s_tail = NULL;
        s_count   = 0;
        s_bytes   = 0;
        stop      = s_shutdown;
        s_writing = 1;
        gh_cond_broadcast(&s_drained);   /* room again for blocked producers */
        gh_mutex_unlock(&s_mutex);

        e = batch;
        while (e) {
            QEntry *next = e->next;
            FILE *f = file_for(e->path);
            if (f) {
                fputs(e->text, f);
                if (e->newline) fputc('\n', f);
            }
            free(e->path);
            free(e->text);
            free(e);
            e = next;
        }

        files_flush();

        gh_mutex_lock(&s_mutex);
        s_writing = 0;
        gh_cond_broadcast(&s_drained);   /* wake naOptLogFlushSync */
        gh_mutex_unlock(&s_mutex);

        if (stop) {
            int more;
            gh_mutex_lock(&s_mutex);
            more = (s_head != NULL);
            gh_mutex_unlock(&s_mutex);
            if (!more) break;
        }
    }

    files_close();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

void naOptLogFlushSync(void) {
    if (!s_running) return;
    gh_mutex_lock(&s_mutex);
    while (s_head || s_writing) {
        gh_cond_signal(&s_cond);
        gh_cond_wait(&s_drained, &s_mutex);
    }
    gh_mutex_unlock(&s_mutex);
}

/* ── Lua bindings ─────────────────────────────────────────────────────── */

/* Start the writer thread; main_path NULL/"" means "no main file". Caller
 * holds s_open_mutex. Returns 1 if this call started the thread. */
static int start_writer(const char *main_path) {
    if (s_running) return 0;
    if (main_path && main_path[0]) {
        strncpy(s_main_path, main_path, sizeof(s_main_path) - 1);
        s_main_path[sizeof(s_main_path) - 1] = '\0';
    } else {
        s_main_path[0] = '\0';
    }
    s_shutdown = 0;
    s_head = s_tail = NULL;
    s_count = s_bytes = 0;
    s_writing = 0;

    gh_mutex_init(&s_mutex);
    gh_cond_init(&s_cond);
    gh_cond_init(&s_drained);
    gh_thread_start(&s_thread);
    s_running = 1;
    return 1;
}

/* gh_opt_log.open(path) -- name the main log file (the destination for
 * write()) and start the writer thread if it isn't up yet.
 *
 * Returns true whenever the main path is now `path`, INCLUDING when someone
 * else had already started the thread. It used to return false in that case
 * and keep whatever path the first caller passed, which matters now that
 * print2 also starts the thread: optimize.lua reads false as "no log for me"
 * and would stop writing optimize.log altogether. The main path is only a
 * filename now (see l_write), so it can be set at any time. */
static int l_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    gh_mutex_lock(&s_open_mutex);
    if (!s_running) {
        start_writer(path);
    } else {
        strncpy(s_main_path, path, sizeof(s_main_path) - 1);
        s_main_path[sizeof(s_main_path) - 1] = '\0';
    }
    gh_mutex_unlock(&s_open_mutex);
    lua_pushboolean(L, 1);
    return 1;
}

/* gh_opt_log.ensure() -- start the writer thread WITHOUT claiming the main log
 * file. print2 only ever uses append(path, ...), so it must not be the caller
 * that decides where gh_opt_log.write() lands: whichever brain called open()
 * first set s_main_path, and if print2 won that race the profiler's
 * optimize.log went into a print2_bot<N>.log. */
static int l_ensure(lua_State *L) {
    int started;
    gh_mutex_lock(&s_open_mutex);
    started = start_writer(NULL);
    gh_mutex_unlock(&s_open_mutex);
    lua_pushboolean(L, (started || s_running) ? 1 : 0);
    return 1;
}

/* gh_opt_log.write(text) -- enqueue text for the main log (no newline added).
 * The main path is resolved here, on the producer side, so the writer thread
 * never reads s_main_path and needs no lock for it. */
static int l_write(lua_State *L) {
    size_t len;
    const char *str;
    char *copy, *pcopy;
    if (!s_running) return 0;
    str = luaL_checklstring(L, 1, &len);
    gh_mutex_lock(&s_open_mutex);
    pcopy = s_main_path[0] ? gh_strdup(s_main_path) : NULL;
    gh_mutex_unlock(&s_open_mutex);
    if (!pcopy) return 0;
    copy = (char *)malloc(len + 1);
    if (!copy) { free(pcopy); return 0; }
    memcpy(copy, str, len + 1);
    queue_push(pcopy, copy, len, 0);
    return 0;
}

/* gh_opt_log.append(path, text[, raw]) -- enqueue text to an arbitrary file,
 * adding a trailing newline unless `raw` is true.
 *
 * Returns true when the text was queued. print2 treats a falsy return as a
 * hard failure (its "crash loudly rather than log silently into nothing"
 * rule), which also catches the sandbox refusing a path outside the brain
 * directory -- the jail wrapper in luabrainshandler.c returns nil there. */
static int l_append(lua_State *L) {
    size_t plen, tlen;
    const char *path, *text;
    char *pcopy, *tcopy;
    int newline;
    if (!s_running) { lua_pushboolean(L, 0); return 1; }
    path = luaL_checklstring(L, 1, &plen);
    text = luaL_checklstring(L, 2, &tlen);
    /* raw = write text exactly as given. print2 hands over a block that
       already ends in a newline; without this the writer added another and
       left a blank line between every batch. */
    newline = lua_toboolean(L, 3) ? 0 : 1;
    pcopy = (char *)malloc(plen + 1);
    tcopy = (char *)malloc(tlen + 1);
    if (!pcopy || !tcopy) {
        free(pcopy); free(tcopy);
        lua_pushboolean(L, 0);
        return 1;
    }
    memcpy(pcopy, path, plen + 1);
    memcpy(tcopy, text, tlen + 1);
    queue_push(pcopy, tcopy, tlen, newline);
    lua_pushboolean(L, 1);
    return 1;
}

/* gh_opt_log.flush() -- block until the queue is on disk */
static int l_flush(lua_State *L) {
    (void)L;
    naOptLogFlushSync();
    return 0;
}

/* gh_opt_log.close() -- drain the queue to disk. The writer thread is NOT
 * stopped: it is one process-wide thread shared by every brain instance in
 * the process (each lua_State registers this module, but s_thread / the
 * queue are file-statics), and a brain has no way to know it is the last
 * user. GoalHunter 1.6's optimize.lua calls close() at its Brain.close;
 * when it used to stop the thread, every current GoalHunter brain closing after it found
 * append() returning false and died in print2's fail_hard -- two
 * brain_crash logs at the end of every 1.6-vs-current game (20260903_105448
 * block 2), and, now that the server rebuilds every bot's brain at each
 * round start, it would have been two per round. A synchronous drain gives
 * close() everything it was ever used for (the log is complete on disk
 * before the state goes away); the thread idles until the next append or
 * process exit. */
static int l_close(lua_State *L) {
    (void)L;
    naOptLogFlushSync();
    return 0;
}

/* ── Registration ─────────────────────────────────────────────────────── */
static const luaL_Reg gh_opt_log_lib[] = {
    { "open",   l_open   },
    { "ensure", l_ensure },
    { "write",  l_write  },
    { "append", l_append },
    { "flush",  l_flush  },
    { "close",  l_close  },
    { NULL, NULL }
};

void naOptLogRegister(lua_State *L) {
    /* Lazy-init s_open_mutex on first registration.
     *
     * CONTRACT: this function is called only from luaBrainInstanceCreate,
     * which runs single-threaded on the producer thread before any brain
     * ticks. The check-then-write below is unsynchronized by design and
     * relies on that serialization — without it, two concurrent registers
     * race (double gh_mutex_init on the same mutex, leaked native handle
     * and undefined behaviour). If a future caller invokes this from any
     * other context, replace the bool guard with a once-flag (pthread_once
     * or SDL_CompareAndSwapAtomicInt) before merging that change. */
    if (!s_open_mutex_inited) {
        gh_mutex_init(&s_open_mutex);
        s_open_mutex_inited = 1;
    }
    luaL_newlib(L, gh_opt_log_lib);
    lua_setglobal(L, "gh_opt_log");
}
