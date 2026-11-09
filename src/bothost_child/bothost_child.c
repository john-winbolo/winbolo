/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The built-in Bot Host as a child process: the parts with no process in
 * them (bothost_child.h). The child's arguments, the Windows command line
 * quoting, the status and hello lines, the ticket in hex, and the restart
 * counter. The unit tests run all of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bothost_child.h"

/* ---- Arguments ------------------------------------------------------- */

typedef struct {
    char        *buf;
    size_t       size;
    size_t       used;
    const char **argv;
    int          max;
    int          argc;
    bool         ok;
} ArgBuild;

static void argAdd(ArgBuild *b, const char *s) {
    size_t n;
    if (!b->ok) return;
    if (s == NULL) s = "";
    n = strlen(s) + 1;
    /* One place is kept for the NULL at the end. */
    if (b->argc + 1 >= b->max || b->used + n > b->size) {
        b->ok = false;
        return;
    }
    memcpy(b->buf + b->used, s, n);
    b->argv[b->argc++] = b->buf + b->used;
    b->used += n;
}

static void argAddInt(ArgBuild *b, const char *opt, long v) {
    char num[32];
    snprintf(num, sizeof(num), "%ld", v);
    argAdd(b, opt);
    argAdd(b, num);
}

int botHostChildBuildArgs(const BotHostChildParams *p, const char *exe,
                          char *buf, size_t bufSize,
                          const char **argv, int maxArgv) {
    ArgBuild b;
    if (p == NULL || exe == NULL || buf == NULL || argv == NULL ||
        maxArgv < 2) {
        return -1;
    }
    memset(&b, 0, sizeof(b));
    b.buf = buf;
    b.size = bufSize;
    b.argv = argv;
    b.max = maxArgv;
    b.ok = true;

    argAdd(&b, exe);
    argAdd(&b, "--embed-child");
    /* The child ends when the game's pipe closes, on every system. */
    argAdd(&b, "--exit-on-stdin-close");
    argAdd(&b, "--server");
    argAdd(&b, p->serverHost);
    argAddInt(&b, "--port", (long)p->serverPort);
    argAddInt(&b, "--player", (long)p->player);
    argAdd(&b, "--brain");
    argAdd(&b, p->brain);
    argAdd(&b, "--name");
    argAdd(&b, p->name);
    argAdd(&b, "--cachedir");
    argAdd(&b, p->cacheDir);
    argAddInt(&b, "--slots", (long)(p->slots > 0 ? p->slots : 0));
    if (p->officialOnly) argAdd(&b, "--official-only");
    if (p->record) argAdd(&b, "--record");
    if (p->recordMinFreeGb > 0.0) {
        char num[32];
        snprintf(num, sizeof(num), "%.3f", p->recordMinFreeGb);
        argAdd(&b, "--record-min-free-gb");
        argAdd(&b, num);
    }
    if (p->cpuSpeedFile != NULL && p->cpuSpeedFile[0] != '\0') {
        argAdd(&b, "--cpu-speed-file");
        argAdd(&b, p->cpuSpeedFile);
    }
    if (!b.ok) return -1;
    argv[b.argc] = NULL;
    return b.argc;
}

/* The rules of CommandLineToArgvW: backslashes are plain unless a quote
 * follows them; then each pair is one backslash, and an odd one makes the
 * quote plain. */
bool botHostChildQuoteWinArg(const char *arg, char *out, size_t outSize,
                             size_t *len) {
    size_t at = *len;
    const char *c;
    bool quote = arg[0] == '\0' || strpbrk(arg, " \t\n\v\"") != NULL;

#define PUT(ch)                                                          \
    do {                                                                 \
        if (at + 1 >= outSize) return false;                             \
        out[at++] = (ch);                                                \
    } while (0)

    if (!quote) {
        for (c = arg; *c != '\0'; c++) PUT(*c);
    } else {
        PUT('"');
        for (c = arg;; c++) {
            size_t slashes = 0, i;
            while (*c == '\\') {
                slashes++;
                c++;
            }
            if (*c == '\0') {
                /* Before the closing quote: every backslash doubled. */
                for (i = 0; i < slashes * 2; i++) PUT('\\');
                break;
            }
            if (*c == '"') {
                for (i = 0; i < slashes * 2 + 1; i++) PUT('\\');
                PUT('"');
            } else {
                for (i = 0; i < slashes; i++) PUT('\\');
                PUT(*c);
            }
        }
        PUT('"');
    }
#undef PUT
    out[at] = '\0';
    *len = at;
    return true;
}

/* ---- The line codec -------------------------------------------------- */

void botHostChildFormatStatus(const BotHostChildStatus *st, char *out,
                              size_t outSize) {
    snprintf(out, outSize, "status %d %d %d %d %d %d %d",
             st->running ? 1 : 0, st->connected ? 1 : 0,
             st->refused ? 1 : 0, st->slots, st->bots,
             st->sameServer ? 1 : 0, st->consentAsking ? 1 : 0);
}

bool botHostChildParseStatus(const char *line, BotHostChildStatus *out) {
    int v[7];
    if (line == NULL || out == NULL) return false;
    if (sscanf(line, "status %d %d %d %d %d %d %d", &v[0], &v[1], &v[2],
               &v[3], &v[4], &v[5], &v[6]) != 7) {
        return false;
    }
    if (v[3] < 0 || v[4] < 0) return false;
    out->running = v[0] != 0;
    out->connected = v[1] != 0;
    out->refused = v[2] != 0;
    out->slots = v[3];
    out->bots = v[4];
    out->sameServer = v[5] != 0;
    out->consentAsking = v[6] != 0;
    return true;
}

bool botHostChildParseHello(const char *line, int *protocol,
                            bool *officialOnly) {
    int proto, off;
    if (line == NULL) return false;
    if (sscanf(line, "hello %d official=%d", &proto, &off) != 2) return false;
    if (protocol != NULL) *protocol = proto;
    if (officialOnly != NULL) *officialOnly = off != 0;
    return true;
}

void botHostChildTicketHex(const uint8_t *ticket, size_t len, char *out,
                           size_t outSize) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    if (outSize == 0) return;
    for (i = 0; i < len && 2 * i + 2 < outSize; i++) {
        out[2 * i] = digits[ticket[i] >> 4];
        out[2 * i + 1] = digits[ticket[i] & 15];
    }
    out[2 * i] = '\0';
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool botHostChildTicketParse(const char *hex, uint8_t *ticket, size_t len) {
    size_t i;
    if (hex == NULL || strlen(hex) < 2 * len) return false;
    for (i = 0; i < len; i++) {
        int hi = hexValue(hex[2 * i]);
        int lo = hexValue(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        ticket[i] = (uint8_t)((hi << 4) | lo);
    }
    /* Nothing but line ends after the digits. */
    for (i = 2 * len; hex[i] != '\0'; i++) {
        if (hex[i] != '\r' && hex[i] != '\n') return false;
    }
    return true;
}

/* ---- The restart counter --------------------------------------------- */

void botHostChildRestartInit(BotHostChildRestart *r) {
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
}

bool botHostChildRestartAfterExit(BotHostChildRestart *r, uint32_t nowMs) {
    if (r == NULL) return false;
    if (r->restarts >= BOT_HOST_CHILD_MAX_RESTARTS) {
        r->waiting = false;
        return false;
    }
    r->restarts++;
    r->waiting = true;
    r->waitUntilMs = nowMs + BOT_HOST_CHILD_RESTART_WAIT_MS;
    return true;
}

bool botHostChildRestartDue(BotHostChildRestart *r, uint32_t nowMs) {
    if (r == NULL || !r->waiting) return false;
    if ((int32_t)(nowMs - r->waitUntilMs) < 0) return false;
    r->waiting = false;
    return true;
}
