/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "cmd_stdin.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMD_STDIN_LINE_MAX 1024

struct CmdStdin {
  FILE *fp;
  bool  owns_fp;
  int   nextLineNumber;        /* 1-based, line we are about to read */
  bool  havePending;
  CmdLine pending;
};

/* Each op's wire-spelling. Order matches the CmdOp enum. */
static const char *kOpNames[CMD_OP__COUNT] = {
  "add_bot",
  "set_team",
  "set_ready",
  "name_change",
  "alliance_request",
  "alliance_accept",
  "alliance_leave",
  "map_skip_vote",
  "chat",
  "start_game",
  "reapply_alliances",
  "shutdown",
  "exit",
  "ping",
};

const char *cmdOpName(CmdOp op) {
  if ((int)op < 0 || (int)op >= CMD_OP__COUNT) return "?";
  return kOpNames[op];
}

/* ------------------------------------------------------------------ */
/* Hand-rolled JSON-line parser                                        */
/* ------------------------------------------------------------------ */
/*
 * Flat-object subset: {"key": value, "key": value, ...}
 *   - Whitespace allowed between tokens.
 *   - Keys are double-quoted ASCII identifiers (no escapes needed
 *     for the op vocabulary, but the value scanner accepts \\ and \").
 *   - Values are: integer decimal, true/false, or double-quoted
 *     string. No nested objects, no arrays, no null, no floats.
 *
 * Malformed input is fatal — the harness depends on determinism.
 */

static void fatalParseError(int lineNumber, const char *line, const char *what) {
  fprintf(stderr, "cmd-stdin: line %d: %s\n", lineNumber, what);
  fprintf(stderr, "cmd-stdin: %s", line);
  if (line[0] == '\0' || line[strlen(line) - 1] != '\n') {
    fputc('\n', stderr);
  }
  exit(2);
}

/* Skip ASCII whitespace; returns the next non-space position. */
static const char *skipWs(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

/* Match a literal at *pp, advancing *pp past it; return false if no match. */
static bool matchLit(const char **pp, const char *lit) {
  const char *p = *pp;
  size_t n = strlen(lit);
  if (strncmp(p, lit, n) != 0) return false;
  *pp = p + n;
  return true;
}

/* Parse a JSON string into out (capped at outSize-1). The leading "
 * must already be consumed. Advances *pp past the closing ".
 * Returns false on malformed escape or unterminated string. */
static bool parseStringBody(const char **pp, char *out, size_t outSize) {
  const char *p = *pp;
  size_t i = 0;
  while (*p != '\0' && *p != '"') {
    char ch;
    if (*p == '\\') {
      p++;
      switch (*p) {
        case '"':  ch = '"'; break;
        case '\\': ch = '\\'; break;
        case 'n':  ch = '\n'; break;
        case 't':  ch = '\t'; break;
        case 'r':  ch = '\r'; break;
        default:   return false;  /* unsupported escape — caller fatals */
      }
      p++;
    } else {
      ch = *p++;
    }
    if (i + 1 < outSize) out[i++] = ch;
  }
  if (*p != '"') return false;
  out[i] = '\0';
  *pp = p + 1;
  return true;
}

/* Parse a JSON key (double-quoted ident) into out. Capped at outSize-1.
 * Advances *pp past the closing ". Returns false on malformed. */
static bool parseKey(const char **pp, char *out, size_t outSize) {
  const char *p = skipWs(*pp);
  if (*p != '"') return false;
  p++;
  *pp = p;
  return parseStringBody(pp, out, outSize);
}

/* Parse a JSON unsigned decimal integer (no sign, no float).
 * On success, advances *pp and writes *out. Returns false on no digits. */
static bool parseUint(const char **pp, uint64_t *out) {
  const char *p = skipWs(*pp);
  if (!isdigit((unsigned char)*p)) return false;
  uint64_t v = 0;
  while (isdigit((unsigned char)*p)) {
    v = v * 10 + (uint64_t)(*p - '0');
    p++;
  }
  *pp = p;
  *out = v;
  return true;
}

/* Parse one CmdLine from `line` (no trailing newline assumption — both
 * accepted). Lifts the per-line fatal-on-bad-input policy via lineNumber. */
static void parseOneLine(const char *origLine, int lineNumber, CmdLine *out) {
  char buf[CMD_STDIN_LINE_MAX];
  /* Copy to mutable buffer in case we want to mutate; we don't, but
   * keeping a separate buffer simplifies the fatalParseError prints. */
  strncpy(buf, origLine, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  memset(out, 0, sizeof(*out));
  out->lineNumber = lineNumber;
  out->op = CMD_OP__COUNT;  /* sentinel: not yet seen */

  /* Track which fields the caller has supplied so we can validate
   * the op-specific requirement set after the parse pass. */
  bool haveTick = false;
  bool haveOp   = false;
  bool haveSlot = false, haveTeam = false, haveReady = false;
  bool haveFrom = false, haveTo = false, haveName = false;
  bool haveDest = false, haveBody = false;
  bool haveKind = false, haveMx = false, haveMy = false;

  const char *p = skipWs(buf);
  if (*p != '{') fatalParseError(lineNumber, origLine, "expected '{'");
  p++;
  p = skipWs(p);

  /* Empty object? Treat as malformed — we need at minimum tick + op. */
  if (*p == '}') fatalParseError(lineNumber, origLine, "empty object — need tick + op");

  while (1) {
    char key[64];
    p = skipWs(p);
    if (!parseKey(&p, key, sizeof(key))) {
      fatalParseError(lineNumber, origLine, "expected key string");
    }
    p = skipWs(p);
    if (*p != ':') fatalParseError(lineNumber, origLine, "expected ':' after key");
    p++;
    p = skipWs(p);

    if (strcmp(key, "tick") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v)) fatalParseError(lineNumber, origLine, "tick must be unsigned int");
      out->tick = (uint32_t)v;
      haveTick = true;
    } else if (strcmp(key, "op") == 0) {
      if (*p != '"') fatalParseError(lineNumber, origLine, "op value must be string");
      p++;
      char opStr[32];
      if (!parseStringBody(&p, opStr, sizeof(opStr))) {
        fatalParseError(lineNumber, origLine, "malformed op string");
      }
      int i;
      for (i = 0; i < CMD_OP__COUNT; i++) {
        if (strcmp(opStr, kOpNames[i]) == 0) {
          out->op = (CmdOp)i;
          break;
        }
      }
      if (i == CMD_OP__COUNT) {
        char msg[96];
        snprintf(msg, sizeof(msg), "unknown op '%s'", opStr);
        fatalParseError(lineNumber, origLine, msg);
      }
      haveOp = true;
    } else if (strcmp(key, "slot") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "slot must be 0..255");
      }
      out->slot = (BYTE)v;
      haveSlot = true;
    } else if (strcmp(key, "team") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "team must be 0..255");
      }
      out->team = (BYTE)v;
      haveTeam = true;
    } else if (strcmp(key, "ready") == 0) {
      if (matchLit(&p, "true")) out->ready = true;
      else if (matchLit(&p, "false")) out->ready = false;
      else fatalParseError(lineNumber, origLine, "ready must be true|false");
      haveReady = true;
    } else if (strcmp(key, "from") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "from must be 0..255");
      }
      out->from = (BYTE)v;
      haveFrom = true;
    } else if (strcmp(key, "to") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "to must be 0..255");
      }
      out->to = (BYTE)v;
      haveTo = true;
    } else if (strcmp(key, "name") == 0) {
      if (*p != '"') fatalParseError(lineNumber, origLine, "name must be string");
      p++;
      if (!parseStringBody(&p, out->name, sizeof(out->name))) {
        fatalParseError(lineNumber, origLine, "malformed name string");
      }
      haveName = true;
    } else if (strcmp(key, "dest") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "dest must be 0..255");
      }
      out->dest = (BYTE)v;
      haveDest = true;
    } else if (strcmp(key, "body") == 0) {
      if (*p != '"') fatalParseError(lineNumber, origLine, "body must be string");
      p++;
      if (!parseStringBody(&p, out->body, sizeof(out->body))) {
        fatalParseError(lineNumber, origLine, "malformed body string");
      }
      haveBody = true;
    } else if (strcmp(key, "kind") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "kind must be 0..255");
      }
      out->kind = (BYTE)v;
      haveKind = true;
    } else if (strcmp(key, "mx") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "mx must be 0..255");
      }
      out->mx = (BYTE)v;
      haveMx = true;
    } else if (strcmp(key, "my") == 0) {
      uint64_t v;
      if (!parseUint(&p, &v) || v > 255) {
        fatalParseError(lineNumber, origLine, "my must be 0..255");
      }
      out->my = (BYTE)v;
      haveMy = true;
    } else {
      char msg[96];
      snprintf(msg, sizeof(msg), "unknown key '%s'", key);
      fatalParseError(lineNumber, origLine, msg);
    }

    p = skipWs(p);
    if (*p == ',') { p++; continue; }
    if (*p == '}') { p++; break; }
    fatalParseError(lineNumber, origLine, "expected ',' or '}'");
  }

  /* Trailing junk after } is fatal — keeps the determinism guarantee. */
  p = skipWs(p);
  if (*p != '\0' && *p != '\n' && *p != '\r') {
    fatalParseError(lineNumber, origLine, "trailing junk after '}'");
  }

  if (!haveTick) fatalParseError(lineNumber, origLine, "missing tick");
  if (!haveOp)   fatalParseError(lineNumber, origLine, "missing op");

  /* Op-specific required fields. */
  switch (out->op) {
    case CMD_OP_SET_TEAM:
      if (!haveSlot || !haveTeam) {
        fatalParseError(lineNumber, origLine, "set_team needs slot + team");
      }
      break;
    case CMD_OP_SET_READY:
      if (!haveSlot || !haveReady) {
        fatalParseError(lineNumber, origLine, "set_ready needs slot + ready");
      }
      break;
    case CMD_OP_NAME_CHANGE:
      if (!haveSlot || !haveName) {
        fatalParseError(lineNumber, origLine, "name_change needs slot + name");
      }
      break;
    case CMD_OP_ALLIANCE_REQUEST:
    case CMD_OP_ALLIANCE_ACCEPT:
      if (!haveFrom || !haveTo) {
        fatalParseError(lineNumber, origLine, "alliance request/accept needs from + to");
      }
      break;
    case CMD_OP_ALLIANCE_LEAVE:
    case CMD_OP_MAP_SKIP_VOTE:
      if (!haveSlot) {
        fatalParseError(lineNumber, origLine, "alliance_leave/map_skip_vote needs slot");
      }
      break;
    case CMD_OP_CHAT:
      if (!haveDest || !haveBody) {
        fatalParseError(lineNumber, origLine, "chat needs dest + body");
      }
      break;
    case CMD_OP_PING:
      if (!haveKind || !haveMx || !haveMy) {
        fatalParseError(lineNumber, origLine, "ping needs kind + mx + my");
      }
      break;
    default:
      break;
  }

  /* Silence "unused" warnings for the field-presence flags we do not
   * cross-check (they exist to keep the parser symmetric). */
  (void)haveFrom; (void)haveTo;
}

/* ------------------------------------------------------------------ */
/* Stream API                                                          */
/* ------------------------------------------------------------------ */

CmdStdin *cmdStdinOpen(const char *path) {
  if (path == NULL || path[0] == '\0') return NULL;
  CmdStdin *cs = (CmdStdin *)calloc(1, sizeof(*cs));
  if (cs == NULL) return NULL;
  if (strcmp(path, "-") == 0) {
    cs->fp = stdin;
    cs->owns_fp = false;
  } else {
    cs->fp = fopen(path, "r");
    if (cs->fp == NULL) {
      fprintf(stderr, "cmd-stdin: cannot open '%s'\n", path);
      free(cs);
      return NULL;
    }
    cs->owns_fp = true;
  }
  cs->nextLineNumber = 1;
  cs->havePending = false;
  return cs;
}

void cmdStdinClose(CmdStdin *cs) {
  if (cs == NULL) return;
  if (cs->fp != NULL && cs->owns_fp) fclose(cs->fp);
  free(cs);
}

/* Pull the next non-blank, non-comment line into the pending slot.
 * Returns false on EOF. Fatals on malformed input. */
static bool refillPending(CmdStdin *cs) {
  char line[CMD_STDIN_LINE_MAX];
  while (fgets(line, sizeof(line), cs->fp) != NULL) {
    int lineNo = cs->nextLineNumber++;
    /* Skip blank lines and # comments (with optional leading whitespace). */
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#') continue;
    parseOneLine(line, lineNo, &cs->pending);
    cs->havePending = true;
    return true;
  }
  return false;
}

bool cmdStdinPeek(CmdStdin *cs, CmdLine *out) {
  if (cs == NULL) return false;
  if (!cs->havePending) {
    if (!refillPending(cs)) return false;
  }
  *out = cs->pending;
  return true;
}

void cmdStdinConsume(CmdStdin *cs) {
  if (cs == NULL) return;
  cs->havePending = false;
}
