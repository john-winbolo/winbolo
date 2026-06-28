/*
 * Tests for the brain-crash logger in src/bolo/braincore.c. The logger
 * writes a unique brain_crash_<UTC>_pid<PID>_bot<N>.log file (or
 * _L<luaptr>.log when bot_index isn't readable) every time brain.think
 * raises a Lua error. The file is the ONLY persistent record of why a
 * bot disappeared mid-game on a live server, so the format is
 * load-bearing for crash-report intake.
 *
 * Two tests:
 *   - brain_crash_log_writes_file:
 *       calls brc_write_crash_log directly with state.bot_index=7,
 *       state.tick=12345, _G.DEBUG_SESSION_DIR=<tempdir>, and a
 *       known error string. Verifies a brain_crash_*_bot7.log file
 *       lands in the tempdir and contains the expected
 *       [BRAIN_CRASH] tagged fields + the error text.
 *   - brain_crash_log_falls_back_to_luaptr:
 *       no state.bot_index; verifies the fallback filename pattern
 *       brain_crash_*_L0x*.log is used instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <SDL3/SDL.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "test_harness.h"
#include "braincore.h"

/* Visible iff dropped to non-static in braincore.c. */
extern void brc_write_crash_log(lua_State *L,
                                const char *method,
                                const char *err_or_traceback);

/* SDL3 directory enumeration callback: collect any brain_crash_*.log
 * filenames into a caller-owned buffer. Returns SDL_ENUM_CONTINUE
 * regardless so the walk visits every entry. */
typedef struct {
    char  found[FILENAME_MAX];
    int   count;
} CrashFileCollector;

static SDL_EnumerationResult collect_crash_files(void *userdata,
                                                  const char *dirname,
                                                  const char *fname) {
    (void)dirname;
    CrashFileCollector *c = (CrashFileCollector *)userdata;
    if (SDL_strncmp(fname, "brain_crash_", 12) == 0) {
        size_t n = SDL_strlen(fname);
        if (n >= 4 && SDL_strcmp(fname + n - 4, ".log") == 0) {
            c->count++;
            if (c->found[0] == '\0') {
                SDL_strlcpy(c->found, fname, sizeof(c->found));
            }
        }
    }
    return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult delete_dir_entry(void *userdata,
                                               const char *dirname,
                                               const char *fname) {
    (void)userdata;
    char full[FILENAME_MAX];
    SDL_snprintf(full, sizeof(full), "%s/%s", dirname, fname);
    SDL_RemovePath(full);
    return SDL_ENUM_CONTINUE;
}

/* Tempdir helper. Path is relative to CWD so the test runner doesn't
 * need to know where the binary lives. Caller must call cleanup_tempdir
 * even on failure paths (the asserts below take care of this via gotos
 * where applicable, but the test bodies just leak the dir on FAIL
 * because UT_ASSERT returns from the function — acceptable for unit
 * tests, the dir is small and named distinctively). */
static const char *TEMPDIR = "test_brain_crash_tmp";

static void cleanup_tempdir(void) {
    SDL_EnumerateDirectory(TEMPDIR, delete_dir_entry, NULL);
    SDL_RemovePath(TEMPDIR);
}

/* Build a minimal lua_State with the globals brc_write_crash_log
 * reads (state.bot_index, state.tick, DEBUG_SESSION_DIR). bot_index < 0
 * means "leave it unset" so the fallback path is exercised. */
static lua_State *new_lua_state_with_globals(int bot_index, int tick,
                                              const char *session_dir) {
    lua_State *L = luaL_newstate();
    if (L == NULL) return NULL;
    luaL_openlibs(L);

    /* Per-crash file writes are gated on BRAIN_DEBUG_MODE (production hosts
     * write none — only the stderr surface fires). These tests exercise the
     * file writer, so turn it on; without it brc_write_crash_log writes no
     * file and the count assertions see 0. */
    lua_pushboolean(L, 1);
    lua_setglobal(L, "BRAIN_DEBUG_MODE");

    /* state = { bot_index = N, tick = T } */
    lua_newtable(L);
    if (bot_index >= 0) {
        lua_pushinteger(L, bot_index);
        lua_setfield(L, -2, "bot_index");
    }
    lua_pushinteger(L, tick);
    lua_setfield(L, -2, "tick");
    lua_setglobal(L, "state");

    if (session_dir != NULL) {
        lua_pushstring(L, session_dir);
        lua_setglobal(L, "DEBUG_SESSION_DIR");
    }
    return L;
}

/* Slurp small files (<=1MB) into a heap buffer for substring asserts.
 * Returns NULL if the file doesn't exist or is too big. Caller frees. */
static char *slurp_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1024 * 1024) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* Asserts the file body has every required [BRAIN_CRASH] field and the
 * caller-supplied marker. Returns 0 on success, 1 on failure (test
 * framework convention). */
static int assert_crash_file_content(const char *body,
                                      const char *expect_method,
                                      const char *expect_error_marker,
                                      int expect_bot_index,
                                      int expect_tick) {
    UT_ASSERT_MSG(strstr(body, "===== BRAIN CRASH =====") != NULL,
                  "missing opening banner");
    UT_ASSERT_MSG(strstr(body, "===== END BRAIN CRASH =====") != NULL,
                  "missing closing banner");
    UT_ASSERT_MSG(strstr(body, "[BRAIN_CRASH] timestamp_utc=") != NULL,
                  "missing timestamp_utc field");
    UT_ASSERT_MSG(strstr(body, "[BRAIN_CRASH] timestamp_local=") != NULL,
                  "missing timestamp_local field");
    UT_ASSERT_MSG(strstr(body, "[BRAIN_CRASH] pid=") != NULL,
                  "missing pid field");
    UT_ASSERT_MSG(strstr(body, "[BRAIN_CRASH] lua_state=") != NULL,
                  "missing lua_state field");

    char want_method[64];
    SDL_snprintf(want_method, sizeof(want_method),
                 "[BRAIN_CRASH] method=brain.%s", expect_method);
    UT_ASSERT_MSG(strstr(body, want_method) != NULL,
                  "expected method line %s", want_method);

    if (expect_bot_index >= 0) {
        char want_bot[64];
        SDL_snprintf(want_bot, sizeof(want_bot),
                     "[BRAIN_CRASH] bot_index=%d", expect_bot_index);
        UT_ASSERT_MSG(strstr(body, want_bot) != NULL,
                      "expected bot_index line %s", want_bot);
    }

    char want_tick[64];
    SDL_snprintf(want_tick, sizeof(want_tick),
                 "[BRAIN_CRASH] state.tick=%d", expect_tick);
    UT_ASSERT_MSG(strstr(body, want_tick) != NULL,
                  "expected tick line %s", want_tick);

    UT_ASSERT_MSG(strstr(body, expect_error_marker) != NULL,
                  "expected error marker %s in body",
                  expect_error_marker);
    return 0;
}

int run_brain_crash_log_writes_file(void) {
    cleanup_tempdir();
    UT_ASSERT_MSG(SDL_CreateDirectory(TEMPDIR),
                  "failed to create tempdir: %s", SDL_GetError());

    lua_State *L = new_lua_state_with_globals(7, 12345, TEMPDIR);
    UT_ASSERT(L != NULL);

    const char *kErr = "TEST_BRAIN_CRASH_MARKER: divide by zero\n"
                       "stack traceback:\n"
                       "\t[C]: in ?\n"
                       "\t[string \"brain\"]:42: in function 'think'";
    brc_write_crash_log(L, "think", kErr);

    CrashFileCollector c;
    memset(&c, 0, sizeof(c));
    SDL_EnumerateDirectory(TEMPDIR, collect_crash_files, &c);

    UT_ASSERT_MSG(c.count == 1,
                  "expected exactly 1 brain_crash_*.log file in tempdir, got %d",
                  c.count);
    UT_ASSERT_MSG(strstr(c.found, "_bot7.log") != NULL,
                  "filename %s should encode bot_index=7", c.found);

    char path[FILENAME_MAX];
    SDL_snprintf(path, sizeof(path), "%s/%s", TEMPDIR, c.found);
    size_t body_len = 0;
    char *body = slurp_file(path, &body_len);
    UT_ASSERT_MSG(body != NULL, "failed to read crash file %s", path);

    int rc = assert_crash_file_content(body, "think",
                                       "TEST_BRAIN_CRASH_MARKER",
                                       7, 12345);
    free(body);
    lua_close(L);
    cleanup_tempdir();
    return rc;
}

int run_brain_crash_log_falls_back_to_luaptr(void) {
    cleanup_tempdir();
    UT_ASSERT_MSG(SDL_CreateDirectory(TEMPDIR),
                  "failed to create tempdir: %s", SDL_GetError());

    /* bot_index=-1 means "don't set it" so the C-side read returns
     * its sentinel and the filename should fall back to _L<ptr>.log. */
    lua_State *L = new_lua_state_with_globals(-1, 99, TEMPDIR);
    UT_ASSERT(L != NULL);

    brc_write_crash_log(L, "init",
                         "TEST_FALLBACK_MARKER: bad init\n"
                         "stack traceback:\n\t[C]: in ?");

    CrashFileCollector c;
    memset(&c, 0, sizeof(c));
    SDL_EnumerateDirectory(TEMPDIR, collect_crash_files, &c);
    UT_ASSERT_MSG(c.count == 1,
                  "expected 1 crash file, got %d", c.count);
    UT_ASSERT_MSG(strstr(c.found, "_L0") != NULL ||
                  strstr(c.found, "_L00") != NULL,
                  "filename %s should embed lua_State pointer (_L...)",
                  c.found);

    char path[FILENAME_MAX];
    SDL_snprintf(path, sizeof(path), "%s/%s", TEMPDIR, c.found);
    size_t body_len = 0;
    char *body = slurp_file(path, &body_len);
    UT_ASSERT_MSG(body != NULL, "failed to read crash file %s", path);

    /* bot_index should be reported as -1 (the unset sentinel). */
    int rc = assert_crash_file_content(body, "init",
                                       "TEST_FALLBACK_MARKER",
                                       -1, 99);
    free(body);
    lua_close(L);
    cleanup_tempdir();
    return rc;
}
