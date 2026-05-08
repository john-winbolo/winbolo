/*
 * na_opt_log.c — threaded optimize.log writer
 *
 * Enqueues log strings from the Lua tick thread and drains them to disk
 * on a background thread, so file I/O never stalls the bot tick.
 *
 * Thread model: one producer (Lua main thread), one consumer (writer thread).
 * Queue is a mutex-protected linked list. Writer thread sleeps on a condition
 * variable and wakes whenever entries are pushed or shutdown is requested.
 */

#include "na_opt_log.h"
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
   typedef CRITICAL_SECTION  na_mutex_t;
   typedef CONDITION_VARIABLE na_cond_t;
   typedef HANDLE             na_thread_t;
   static void na_mutex_init   (na_mutex_t *m) { InitializeCriticalSection(m); }
   static void na_mutex_lock   (na_mutex_t *m) { EnterCriticalSection(m); }
   static void na_mutex_unlock (na_mutex_t *m) { LeaveCriticalSection(m); }
   static void na_mutex_destroy(na_mutex_t *m) { DeleteCriticalSection(m); }
   static void na_cond_init    (na_cond_t  *c) { InitializeConditionVariable(c); }
   static void na_cond_signal  (na_cond_t  *c) { WakeConditionVariable(c); }
   static void na_cond_destroy (na_cond_t  *c) { (void)c; }
   static void na_cond_wait    (na_cond_t *c, na_mutex_t *m) {
       SleepConditionVariableCS(c, m, INFINITE);
   }
   static DWORD WINAPI writer_thread_fn(LPVOID arg);
   static void na_thread_start(na_thread_t *t) {
       *t = CreateThread(NULL, 0, writer_thread_fn, NULL, 0, NULL);
   }
   static void na_thread_join(na_thread_t *t) {
       WaitForSingleObject(*t, INFINITE);
       CloseHandle(*t);
   }
#else
#  include <pthread.h>
   typedef pthread_mutex_t na_mutex_t;
   typedef pthread_cond_t  na_cond_t;
   typedef pthread_t       na_thread_t;
   static void na_mutex_init   (na_mutex_t *m) { pthread_mutex_init(m, NULL); }
   static void na_mutex_lock   (na_mutex_t *m) { pthread_mutex_lock(m); }
   static void na_mutex_unlock (na_mutex_t *m) { pthread_mutex_unlock(m); }
   static void na_mutex_destroy(na_mutex_t *m) { pthread_mutex_destroy(m); }
   static void na_cond_init    (na_cond_t  *c) { pthread_cond_init(c, NULL); }
   static void na_cond_signal  (na_cond_t  *c) { pthread_cond_signal(c); }
   static void na_cond_destroy (na_cond_t  *c) { pthread_cond_destroy(c); }
   static void na_cond_wait    (na_cond_t *c, na_mutex_t *m) {
       pthread_cond_wait(c, m);
   }
   static void *writer_thread_fn(void *arg);
   static void na_thread_start(na_thread_t *t) {
       pthread_create(t, NULL, writer_thread_fn, NULL);
   }
   static void na_thread_join(na_thread_t *t) { pthread_join(*t, NULL); }
#endif

/* ── Queue ────────────────────────────────────────────────────────────── */
typedef struct QEntry {
    char          *path;   /* NULL  → write to main log file as-is
                              non-NULL → open this path, append text+"\n", close */
    char          *text;
    struct QEntry *next;
} QEntry;

static na_mutex_t  s_mutex;
static na_cond_t   s_cond;
static QEntry     *s_head     = NULL;
static QEntry     *s_tail     = NULL;
static int         s_shutdown = 0;
static na_thread_t s_thread;
static int         s_running  = 0;
static char        s_main_path[2048] = "";

/* Push one entry onto the queue (called from main thread, mutex NOT held). */
static void queue_push(char *path, char *text) {
    QEntry *e = (QEntry *)malloc(sizeof(QEntry));
    if (!e) { free(path); free(text); return; }
    e->path = path;
    e->text = text;
    e->next = NULL;
    na_mutex_lock(&s_mutex);
    if (s_tail) s_tail->next = e; else s_head = e;
    s_tail = e;
    na_cond_signal(&s_cond);
    na_mutex_unlock(&s_mutex);
}

/* ── Writer thread ────────────────────────────────────────────────────── */
#ifdef _WIN32
static DWORD WINAPI writer_thread_fn(LPVOID arg) {
#else
static void *writer_thread_fn(void *arg) {
#endif
    (void)arg;
    FILE *main_file = (s_main_path[0]) ? fopen(s_main_path, "a") : NULL;

    for (;;) {
        na_mutex_lock(&s_mutex);
        while (!s_head && !s_shutdown)
            na_cond_wait(&s_cond, &s_mutex);
        QEntry *batch    = s_head;
        s_head = s_tail  = NULL;
        int stop         = s_shutdown;
        na_mutex_unlock(&s_mutex);

        /* Drain batch */
        QEntry *e = batch;
        while (e) {
            QEntry *next = e->next;
            if (!e->path) {
                /* Main log: write text verbatim */
                if (main_file) fputs(e->text, main_file);
            } else {
                /* Arbitrary file: open, append text + newline, close */
                FILE *f = fopen(e->path, "a");
                if (f) { fputs(e->text, f); fputc('\n', f); fclose(f); }
                free(e->path);
            }
            free(e->text);
            free(e);
            e = next;
        }

        if (main_file) fflush(main_file);

        if (stop) {
            na_mutex_lock(&s_mutex);
            int more = (s_head != NULL);
            na_mutex_unlock(&s_mutex);
            if (!more) break;
        }
    }

    if (main_file) fclose(main_file);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ── Lua bindings ─────────────────────────────────────────────────────── */

/* na_opt_log.open(path) -- open main log, start writer thread */
static int l_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (s_running) { lua_pushboolean(L, 0); return 1; }

    strncpy(s_main_path, path, sizeof(s_main_path) - 1);
    s_main_path[sizeof(s_main_path) - 1] = '\0';
    s_shutdown = 0;
    s_head = s_tail = NULL;

    na_mutex_init(&s_mutex);
    na_cond_init(&s_cond);
    na_thread_start(&s_thread);
    s_running = 1;

    lua_pushboolean(L, 1);
    return 1;
}

/* na_opt_log.write(text) -- enqueue text for main log (no newline added) */
static int l_write(lua_State *L) {
    if (!s_running) return 0;
    size_t len;
    const char *str = luaL_checklstring(L, 1, &len);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, str, len + 1);
    queue_push(NULL, copy);
    return 0;
}

/* na_opt_log.append(path, text) -- enqueue text+"\n" to an arbitrary file */
static int l_append(lua_State *L) {
    if (!s_running) return 0;
    size_t plen, tlen;
    const char *path = luaL_checklstring(L, 1, &plen);
    const char *text = luaL_checklstring(L, 2, &tlen);
    char *pcopy = (char *)malloc(plen + 1);
    char *tcopy = (char *)malloc(tlen + 1);
    if (!pcopy || !tcopy) { free(pcopy); free(tcopy); return 0; }
    memcpy(pcopy, path, plen + 1);
    memcpy(tcopy, text, tlen + 1);
    queue_push(pcopy, tcopy);
    return 0;
}

/* na_opt_log.close() -- drain remaining queue, stop thread */
static int l_close(lua_State *L) {
    (void)L;
    if (!s_running) return 0;
    na_mutex_lock(&s_mutex);
    s_shutdown = 1;
    na_cond_signal(&s_cond);
    na_mutex_unlock(&s_mutex);
    na_thread_join(&s_thread);
    na_mutex_destroy(&s_mutex);
    na_cond_destroy(&s_cond);
    s_running  = 0;
    s_shutdown = 0;
    s_head = s_tail = NULL;
    return 0;
}

/* ── Registration ─────────────────────────────────────────────────────── */
static const luaL_Reg na_opt_log_lib[] = {
    { "open",   l_open   },
    { "write",  l_write  },
    { "append", l_append },
    { "close",  l_close  },
    { NULL, NULL }
};

void naOptLogRegister(lua_State *L) {
    luaL_newlib(L, na_opt_log_lib);
    lua_setglobal(L, "na_opt_log");
}
