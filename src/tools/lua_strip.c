/*********************************************************
 * lua_strip.c — Lua brain source optimizer
 *
 * Strips named call-statement patterns (print2, viz.*, overlay_*, ...)
 * AND named block patterns (e.g. `if BRAIN_DEBUG_MODE then ... end`)
 * from Lua source files and writes cleaned copies to an output directory.
 * After stripping, each output file is syntax-checked via luaL_loadfile
 * so that a malformed strip (e.g. mismatched parens) fails the build.
 *
 * Usage:
 *   lua_strip [--strip <prefix>] ... [--strip-block <prefix>] ...
 *             <outdir> <file.lua> [<file.lua> ...]
 *
 * --strip mode (call-statement stripping):
 *   A line is stripped when its first non-whitespace token starts with
 *   one of the --strip prefixes. Multi-line calls are handled by
 *   tracking unmatched '(' depth; all lines up to and including the
 *   closing ')' are suppressed.
 *
 * --strip-block mode (Lua block stripping):
 *   A line is stripped when its first non-whitespace token starts with
 *   one of the --strip-block prefixes. Lua block-depth tracking
 *   (then/do/function/repeat opens, end/until closes, elseif compensates)
 *   removes everything up to and including the matching `end` or `until`.
 *   Use this for `if BRAIN_DEBUG_MODE then ... end` blocks so the runtime
 *   check disappears in opt/ entirely.
 *
 * Single- and double-quoted string contents and -- comments are excluded
 * from both paren and block counting.
 *
 * Exit code: 0 = all files processed OK, 1 = one or more errors.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#  include <direct.h>
#  define lua_strip_mkdir(d) _mkdir(d)
#  define PATH_SEP '\\'
#else
#  include <sys/stat.h>
#  define lua_strip_mkdir(d) mkdir((d), 0755)
#  define PATH_SEP '/'
#endif

/* Lua headers for syntax checking */
#include "lua.h"
#include "lauxlib.h"

/* ── Configuration limits ─────────────────────────────────────────── */
#define MAX_PATTERNS  64
#define MAX_LINE      8192
#define MAX_PATH      2048

/* ── Strip pattern table ──────────────────────────────────────────── */
static const char *g_patterns[MAX_PATTERNS];
static int         g_npatterns = 0;

static const char *g_block_patterns[MAX_PATTERNS];

static const char *g_excludes[MAX_PATTERNS];
static int         g_nexcludes = 0;
static int         g_nblock_patterns = 0;

/* Returns 1 if line (after leading whitespace) starts with any pattern. */
static int line_matches(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    for (int i = 0; i < g_npatterns; i++) {
        size_t plen = strlen(g_patterns[i]);
        if (strncmp(line, g_patterns[i], plen) == 0)
            return 1;
    }
    return 0;
}

/* Returns 1 if line (after leading whitespace) starts with any block-strip
 * prefix — used to recognise the OPENING line of a Lua block to remove. */
static int line_matches_block(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    for (int i = 0; i < g_nblock_patterns; i++) {
        size_t plen = strlen(g_block_patterns[i]);
        if (strncmp(line, g_block_patterns[i], plen) == 0)
            return 1;
    }
    return 0;
}

/*
 * Count net Lua block-depth change (openers minus closers) on a single line.
 *   Openers (+1):  then  do  function  repeat
 *   Closers (-1):  end  until
 *   Compensator (-1): elseif
 *       (`elseif` continues an `if` block — the next `then` shouldn't
 *        re-open it. Subtracting 1 here cancels that next `then`.)
 *
 * Strings ("..." and '...') and -- line comments are skipped. Long-bracket
 * strings ([[...]]) are not handled — they don't appear in the if-blocks
 * we strip in practice. Word boundaries are enforced (prev char must be
 * non-word; following char must be non-word) so identifiers like
 * "do_something" or "endless" don't false-match.
 */
static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}

static int match_kw(const char *p, const char *kw, int *advance) {
    size_t klen = strlen(kw);
    if (strncmp(p, kw, klen) != 0) return 0;
    /* Trailing-edge boundary: char after must be non-word (or end). */
    if (is_word_char(p[klen])) return 0;
    *advance = (int)klen;
    return 1;
}

static int block_delta(const char *line) {
    int delta = 0;
    const char *p = line;
    int prev_word = 0;
    while (*p) {
        /* Line comment ends scanning. */
        if (p[0] == '-' && p[1] == '-') break;
        /* Skip string literals. */
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && *(p+1)) p++;
                p++;
            }
            if (*p) p++;
            prev_word = 0;
            continue;
        }
        int wc = is_word_char(*p);
        /* Only check at word starts (leading boundary). */
        if (wc && !prev_word) {
            int adv = 0;
            if      (match_kw(p, "then",     &adv)) delta++;
            else if (match_kw(p, "do",       &adv)) delta++;
            else if (match_kw(p, "function", &adv)) delta++;
            else if (match_kw(p, "repeat",   &adv)) delta++;
            else if (match_kw(p, "end",      &adv)) delta--;
            else if (match_kw(p, "until",    &adv)) delta--;
            else if (match_kw(p, "elseif",   &adv)) delta--;
            if (adv) {
                p += adv;
                prev_word = 1;
                continue;
            }
        }
        prev_word = wc;
        p++;
    }
    return delta;
}

/*
 * Count net '(' minus ')' in a line, skipping string literals and
 * single-line -- comments.  Long strings ([[...]]) are not handled;
 * they don't appear in the patterns we strip in practice.
 */
static int paren_delta(const char *p) {
    int depth = 0;
    while (*p) {
        if (p[0] == '-' && p[1] == '-') break;          /* comment */
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (*p == '\\' && *(p+1)) p++;           /* escape */
                p++;
            }
            if (*p) p++;
            continue;
        }
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        p++;
    }
    return depth;
}

/* ── Per-file processing ──────────────────────────────────────────── */
static int process_file(const char *inpath, const char *outpath) {
    FILE *fin = fopen(inpath, "r");
    if (!fin) {
        fprintf(stderr, "lua_strip: cannot open '%s': ", inpath);
        perror("");
        return 1;
    }
    FILE *fout = fopen(outpath, "w");
    if (!fout) {
        fclose(fin);
        fprintf(stderr, "lua_strip: cannot write '%s': ", outpath);
        perror("");
        return 1;
    }

    char line[MAX_LINE];
    int  depth          = 0;   /* unmatched '(' depth while skipping */
    int  skipping       = 0;   /* are we inside a stripped multi-line call? */
    int  block_depth    = 0;   /* Lua block depth while skipping */
    int  block_skipping = 0;   /* are we inside a stripped block? */
    int  stripped       = 0;   /* number of lines suppressed */

    while (fgets(line, sizeof(line), fin)) {
        if (block_skipping) {
            block_depth += block_delta(line);
            stripped++;
            if (block_depth <= 0) { block_skipping = 0; block_depth = 0; }
        } else if (skipping) {
            depth += paren_delta(line);
            if (depth <= 0) { skipping = 0; depth = 0; }
            stripped++;
        } else if (line_matches(line)) {
            depth = paren_delta(line);
            if (depth > 0) skipping = 1;
            stripped++;
        } else if (line_matches_block(line)) {
            block_depth = block_delta(line);
            stripped++;
            if (block_depth > 0) block_skipping = 1;
            /* depth <= 0 means the entire block fits on this one line
             * (e.g. `if BRAIN_DEBUG_MODE then x = 1 end`); we just drop
             * the line and don't enter multi-line skip mode. */
        } else {
            fputs(line, fout);
        }
    }

    fclose(fin);
    fclose(fout);

    /* Syntax-check the stripped output via Lua C API (no execution). */
    lua_State *L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "lua_strip: out of memory creating Lua state\n");
        return 1;
    }
    int rc = luaL_loadfile(L, outpath);
    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "lua_strip: syntax error in stripped '%s':\n  %s\n",
                outpath, err ? err : "(unknown error)");
        lua_close(L);
        return 1;
    }
    lua_close(L);

    printf("lua_strip: %-50s -> %s  (%d lines stripped)\n",
           inpath, outpath, stripped);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    /* Parse --strip / --strip-block flags (interleaving allowed). */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--strip") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "lua_strip: --strip requires an argument\n");
                return 1;
            }
            if (g_npatterns < MAX_PATTERNS)
                g_patterns[g_npatterns++] = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--strip-block") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "lua_strip: --strip-block requires an argument\n");
                return 1;
            }
            if (g_nblock_patterns < MAX_PATTERNS)
                g_block_patterns[g_nblock_patterns++] = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--exclude") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "lua_strip: --exclude requires an argument\n");
                return 1;
            }
            if (g_nexcludes < MAX_PATTERNS)
                g_excludes[g_nexcludes++] = argv[i + 1];
            i += 2;
        } else {
            break;
        }
    }

    if (i >= argc) {
        fprintf(stderr,
            "Usage: lua_strip [--strip <prefix>] ... [--strip-block <prefix>] ... <outdir> <file.lua> ...\n");
        return 1;
    }

    const char *outdir = argv[i++];

    /* Create output directory if it doesn't exist (best-effort). */
    lua_strip_mkdir(outdir);

    if (i >= argc) {
        fprintf(stderr, "lua_strip: no input files specified\n");
        return 1;
    }

    int errors = 0;
    while (i < argc) {
        const char *inpath = argv[i++];

        /* Extract bare filename from path. */
        const char *fname = strrchr(inpath, '/');
        const char *fname2 = strrchr(inpath, '\\');
        if (fname2 > fname) fname = fname2;
        fname = fname ? fname + 1 : inpath;

        /* Skip excluded filenames. */
        int excluded = 0;
        for (int j = 0; j < g_nexcludes; j++) {
            if (strcmp(fname, g_excludes[j]) == 0) { excluded = 1; break; }
        }
        if (excluded) continue;

        char outpath[MAX_PATH];
        snprintf(outpath, sizeof(outpath), "%s%c%s", outdir, PATH_SEP, fname);

        if (process_file(inpath, outpath))
            errors++;
    }

    if (errors)
        fprintf(stderr, "lua_strip: %d file(s) failed\n", errors);

    return errors > 0 ? 1 : 0;
}
