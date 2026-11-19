/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Bot host record
 *Filename:      bothost_record.h
 *Purpose:
 *  The payload of the log_BotHostData record in a round log
 *  (.wbv): what the server's bot placement measured and chose
 *  for each host (udp_server_bothost.c, botHostOverrunProbe in
 *  bot_host_link.c). Written only when the server runs with
 *  -record-bothost-data (a hosting client: the "Record Bot
 *  Host Data" setting); without it the file has none.
 *
 *  Every record opens with [version 1][kind 1][host 1]. host
 *  is 0 for the server's own process and h + 1 for Bot Host h
 *  (the index of the server's host table). Then, by kind:
 *
 *    HOST     [player 1][nameLen 1][name]
 *             player: the player whose own Bot Host it is, or
 *             0xFF (a listed host, or the server).
 *    PROBE    [bots 1][slots 1][limit 1][thinks 2][overruns 2]
 *             [flags 1][badProbes 1][probes 1][windowRate 2]
 *             [ceiling 1][start 1]
 *             one per host each second. limit 0xFF: none.
 *             flags bit 0: this second's rate is above the
 *             target rate. probes: the probes in the window.
 *             badProbes: version 1 only (the bad probes in the
 *             window); 0xFF from version 2. windowRate (version
 *             2): the host's rate over the window, overruns /
 *             thinks, in hundredths of a percent (0xFFFF: not
 *             known). ceiling and start (version 3): the
 *             host's dynamic slots, the most its live limit may
 *             reach and the limit a game starts at; 0xFF: the
 *             host has no dynamic slots (its limit is then 0xFF
 *             until a step down, as in version 2). slots is the
 *             ceiling too.
 *    READING  [free 1][botCpu 1][bots 1][flags 1]
 *             one per host each free CPU sample. free 0xFF:
 *             not known; botCpu is the CTRL_BOT_HOST_CPU byte.
 *             flags bit 0: the bots thought; bit 1: bad.
 *    DECISION [decision 1][limitBefore 1][limitAfter 1]
 *             [badProbes 1][probes 1][flags 1][predicted 2]
 *             [oneBotCost 2][freeAvg 2][lowestClean 1]
 *             [highestBad 1][seat 1][toHost 1][reasonLen 1]
 *             [reason]
 *             then from version 2: [srcRate 2][dstRate 2]
 *             [moves 1][rule 1][stay 2][needStay 2]
 *             predicted is a signed tenth of a percent; cost
 *             and freeAvg are tenths of a percent. 0xFF in a
 *             byte field: none. flags bit 0: the one-bot cost
 *             is known; bit 1: the free average is known;
 *             bit 2: the one-bot cost is the full-budget
 *             estimate (no measured cost yet).
 *             PLACE and SPREAD_MOVE carry the free CPU check
 *             of the host the bot goes to (toHost); the
 *             others carry that of host.
 *             srcRate and dstRate: the window rates of host and
 *             toHost in hundredths of a percent (0xFFFF: none;
 *             dstRate none on a trial: the target had no bots).
 *             moves: the bot's voluntary moves before this one
 *             (m). rule: 0 none, 1 normal, 2 emergency, 3 trial.
 *             stay: whole seconds since the bot's last voluntary
 *             move, needStay: the stay the rule asked for (0:
 *             none); 0xFFFF in both: none. MOVE, WOULD_MOVE and
 *             NO_BETTER carry them; the others have none.
 *             LIMIT_RAISE (version 3): the dynamic limit went up
 *             (by up to the raise step); LIMIT_START: a game
 *             started at the host's start; LIMIT_CARRY: a game
 *             started at the limit the last game ended with.
 *             limitBefore and limitAfter carry the change.
 *    CPU      [shown 4][source 1][budget 4][botCpu 1][bots 1]
 *             [overrunTenths 2][free 1][slots 1]
 *             one per host each free CPU sample (5 s), while
 *             the game runs. shown is the per-bot value the
 *             lobby and the game show, in thousandths of a
 *             percent of the machine's CPU (0xFFFFFFFF: none),
 *             and source where it comes from
 *             (BOT_HOST_CPU_SRC_* of bot_host_cpu.h: 0 none,
 *             1 measured, 2 last measured, 3 full budget).
 *             budget is the full-budget estimate in the same
 *             unit (0xFFFFFFFF: none). botCpu is the
 *             CTRL_BOT_HOST_CPU byte, bots the bots on the
 *             host, overrunTenths the overrun rate of the same
 *             seconds in tenths of a percent (0xFFFF: no
 *             thinks), free the machine's free CPU (0xFF: not
 *             known) and slots the host's effective slots.
 *
 *  Every multi-byte field is big-endian. A reader takes the
 *  fields it knows and skips the rest by the framed length, so
 *  a later version adds its fields at the end. A reader that
 *  does not know the record skips it by the framed length.
 *
 *  Header only, with no includes but the C library's, so the
 *  server, the viewer (C and C++) and the unit tests use the
 *  same code.
 *********************************************************/

#ifndef BOTHOST_RECORD_H
#define BOTHOST_RECORD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOTHOST_RECORD_VERSION 3

enum {
  BOTHOST_RECORD_HOST     = 1,
  BOTHOST_RECORD_PROBE    = 2,
  BOTHOST_RECORD_READING  = 3,
  BOTHOST_RECORD_DECISION = 4,
  BOTHOST_RECORD_CPU      = 5
};

/* DECISION kinds. */
enum {
  BOTHOST_DECISION_TRIGGER        = 1, /* version 1: the bad probe share
                                        * was met */
  BOTHOST_DECISION_MOVE           = 2, /* a bot moved off the host (the
                                        * overrun move) */
  BOTHOST_DECISION_CUT            = 3, /* the limit was lowered */
  BOTHOST_DECISION_RAISE          = 4, /* the limit went up by 1 */
  BOTHOST_DECISION_RAISE_REFUSED  = 5, /* a raise was looked at and refused */
  BOTHOST_DECISION_PLACE_REFUSED  = 6, /* the host may not take a new bot */
  BOTHOST_DECISION_WOULD_MOVE     = 7, /* a bot would move, but moves are
                                        * off (-bothost-nomove) */
  BOTHOST_DECISION_PLACE          = 8, /* the spread offered a server bot
                                        * to a Bot Host */
  BOTHOST_DECISION_SPREAD_MOVE    = 9, /* the spread moved a bot off a Bot
                                        * Host */
  BOTHOST_DECISION_NO_BETTER      = 10, /* the host is over the target
                                         * rate, but no bot may move to a
                                         * better host */
  BOTHOST_DECISION_HOLD           = 11, /* a bot was held: no place has a
                                         * slot for it
                                         * (-bothost-enforce-server-limit) */
  BOTHOST_DECISION_RESUME         = 12, /* a held bot runs again where a
                                         * place has a slot */
  BOTHOST_DECISION_LIMIT_RAISE    = 13, /* version 3: the dynamic limit
                                         * went up */
  BOTHOST_DECISION_LIMIT_START    = 14, /* a game starts at the start */
  BOTHOST_DECISION_LIMIT_CARRY    = 15  /* a game starts at the limit the
                                         * last game ended with */
};

/* The rule of a move (DECISION rule). */
enum {
  BOTHOST_RULE_NONE      = 0,
  BOTHOST_RULE_NORMAL    = 1,
  BOTHOST_RULE_EMERGENCY = 2,
  BOTHOST_RULE_TRIAL     = 3
};

#define BOTHOST_RECORD_NONE        0xFFu
#define BOTHOST_RECORD_NAME_LEN    32
#define BOTHOST_RECORD_REASON_LEN  64
#define BOTHOST_RECORD_PROBE_BAD   0x01u
#define BOTHOST_RECORD_READING_RAN 0x01u
#define BOTHOST_RECORD_READING_BAD 0x02u
#define BOTHOST_RECORD_DEC_COST    0x01u
#define BOTHOST_RECORD_DEC_FREE    0x02u
#define BOTHOST_RECORD_DEC_BUDGET  0x04u
#define BOTHOST_RECORD_MILLI_NONE  0xFFFFFFFFu
#define BOTHOST_RECORD_RATE_NONE   0xFFFFu
#define BOTHOST_RECORD_SECS_NONE   0xFFFFu
/* The longest payload: a DECISION with the longest reason. */
#define BOTHOST_RECORD_MAX_BYTES   (3 + 16 + BOTHOST_RECORD_REASON_LEN + 10)

typedef struct {
  uint8_t  version;
  uint8_t  kind;
  uint8_t  host;
  /* HOST */
  uint8_t  player;
  char     name[BOTHOST_RECORD_NAME_LEN];
  /* PROBE (bots also READING) */
  uint8_t  bots;
  uint8_t  slots;
  uint8_t  limit;
  uint16_t thinks;
  uint16_t overruns;
  uint8_t  flags;      /* PROBE, READING and DECISION flags */
  uint8_t  badProbes;  /* PROBE and DECISION (version 1) */
  uint8_t  probes;     /* PROBE and DECISION */
  uint16_t windowRate; /* PROBE: hundredths of a percent */
  uint8_t  ceiling;    /* PROBE (version 3): 0xFF none */
  uint8_t  start;      /* PROBE (version 3): 0xFF none */
  /* READING */
  uint8_t  freePct;
  uint8_t  botCpu;
  /* DECISION */
  uint8_t  decision;
  uint8_t  limitBefore;
  uint8_t  limitAfter;
  int16_t  predictedTenths;
  uint16_t costTenths;
  uint16_t freeAvgTenths;
  uint8_t  lowestClean;
  uint8_t  highestBad;
  uint8_t  seat;
  uint8_t  toHost;
  char     reason[BOTHOST_RECORD_REASON_LEN];
  uint16_t srcRate;    /* hundredths of a percent */
  uint16_t dstRate;
  uint8_t  moves;
  uint8_t  rule;       /* BOTHOST_RULE_* */
  uint16_t staySecs;
  uint16_t needStaySecs;
  /* CPU (bots, botCpu, freePct and slots above too) */
  uint32_t shownMilli;
  uint8_t  source;
  uint32_t budgetMilli;
  uint16_t overrunTenths;
} BotHostRecord;

static inline void botHostRecordPut16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t) ((v >> 8) & 0xFFu);
  p[1] = (uint8_t) (v & 0xFFu);
}

static inline uint16_t botHostRecordGet16(const uint8_t *p) {
  return (uint16_t) (((unsigned) p[0] << 8) | (unsigned) p[1]);
}

static inline void botHostRecordPut32(uint8_t *p, uint32_t v) {
  botHostRecordPut16(p, (uint16_t) (v >> 16));
  botHostRecordPut16(p + 2, (uint16_t) (v & 0xFFFFu));
}

static inline uint32_t botHostRecordGet32(const uint8_t *p) {
  return ((uint32_t) botHostRecordGet16(p) << 16) |
         (uint32_t) botHostRecordGet16(p + 2);
}

/* A count held to what a u16 holds. */
static inline uint16_t botHostRecordU16(uint32_t v) {
  return v > 65535u ? (uint16_t) 65535u : (uint16_t) v;
}

/* A byte field from an int: below 0 or above 254 is BOTHOST_RECORD_NONE. */
static inline uint8_t botHostRecordByte(int v) {
  return (v < 0 || v > 254) ? (uint8_t) BOTHOST_RECORD_NONE : (uint8_t) v;
}

/* A percent as tenths, held to the field's range. */
static inline int16_t botHostRecordTenthsS(double pct) {
  double t = pct * 10.0;
  t += t >= 0.0 ? 0.5 : -0.5;
  if (!(t > -32768.0)) return (int16_t) -32768;
  if (t > 32767.0) return (int16_t) 32767;
  return (int16_t) t;
}

static inline uint16_t botHostRecordTenthsU(double pct) {
  double t = pct * 10.0 + 0.5;
  if (!(t > 0.0)) return 0;
  if (t > 65535.0) return 65535u;
  return (uint16_t) t;
}

/* A rate as hundredths of a percent, held below BOTHOST_RECORD_RATE_NONE. */
static inline uint16_t botHostRecordHundredthsU(double pct) {
  double t = pct * 100.0 + 0.5;
  if (!(t > 0.0)) return 0;
  if (t > 65534.0) return 65534u;
  return (uint16_t) t;
}

/* Whole seconds, held below BOTHOST_RECORD_SECS_NONE. */
static inline uint16_t botHostRecordSecs(double secs) {
  double t = secs + 0.5;
  if (!(t > 0.0)) return 0;
  if (t > 65534.0) return 65534u;
  return (uint16_t) t;
}

/* A string field: up to cap - 1 bytes, as a length byte and the bytes. */
static inline int botHostRecordPutStr(uint8_t *p, const char *s, int cap) {
  int n = (int) strnlen(s, (size_t) cap);
  if (n > cap - 1) n = cap - 1;
  p[0] = (uint8_t) n;
  memcpy(p + 1, s, (size_t) n);
  return 1 + n;
}

/* Write rec as the payload into out, which holds cap bytes. The version byte
   is BOTHOST_RECORD_VERSION whatever rec says. Returns the bytes written, or
   0 for an unknown kind or a cap that is too small. */
static inline int botHostRecordEncode(const BotHostRecord *rec, uint8_t *out, int cap) {
  uint8_t tmp[BOTHOST_RECORD_MAX_BYTES];
  uint8_t *p = tmp;
  int n;

  if (rec == NULL || out == NULL) return 0;
  *p++ = BOTHOST_RECORD_VERSION;
  *p++ = rec->kind;
  *p++ = rec->host;
  switch (rec->kind) {
  case BOTHOST_RECORD_HOST:
    *p++ = rec->player;
    p += botHostRecordPutStr(p, rec->name, BOTHOST_RECORD_NAME_LEN);
    break;
  case BOTHOST_RECORD_PROBE:
    *p++ = rec->bots;
    *p++ = rec->slots;
    *p++ = rec->limit;
    botHostRecordPut16(p, rec->thinks);   p += 2;
    botHostRecordPut16(p, rec->overruns); p += 2;
    *p++ = rec->flags;
    *p++ = rec->badProbes;
    *p++ = rec->probes;
    botHostRecordPut16(p, rec->windowRate); p += 2;
    *p++ = rec->ceiling;
    *p++ = rec->start;
    break;
  case BOTHOST_RECORD_READING:
    *p++ = rec->freePct;
    *p++ = rec->botCpu;
    *p++ = rec->bots;
    *p++ = rec->flags;
    break;
  case BOTHOST_RECORD_DECISION:
    *p++ = rec->decision;
    *p++ = rec->limitBefore;
    *p++ = rec->limitAfter;
    *p++ = rec->badProbes;
    *p++ = rec->probes;
    *p++ = rec->flags;
    botHostRecordPut16(p, (uint16_t) rec->predictedTenths); p += 2;
    botHostRecordPut16(p, rec->costTenths);                 p += 2;
    botHostRecordPut16(p, rec->freeAvgTenths);              p += 2;
    *p++ = rec->lowestClean;
    *p++ = rec->highestBad;
    *p++ = rec->seat;
    *p++ = rec->toHost;
    p += botHostRecordPutStr(p, rec->reason, BOTHOST_RECORD_REASON_LEN);
    botHostRecordPut16(p, rec->srcRate);      p += 2;
    botHostRecordPut16(p, rec->dstRate);      p += 2;
    *p++ = rec->moves;
    *p++ = rec->rule;
    botHostRecordPut16(p, rec->staySecs);     p += 2;
    botHostRecordPut16(p, rec->needStaySecs); p += 2;
    break;
  case BOTHOST_RECORD_CPU:
    botHostRecordPut32(p, rec->shownMilli);      p += 4;
    *p++ = rec->source;
    botHostRecordPut32(p, rec->budgetMilli);     p += 4;
    *p++ = rec->botCpu;
    *p++ = rec->bots;
    botHostRecordPut16(p, rec->overrunTenths);   p += 2;
    *p++ = rec->freePct;
    *p++ = rec->slots;
    break;
  default:
    return 0;
  }
  n = (int) (p - tmp);
  if (cap < n) return 0;
  memcpy(out, tmp, (size_t) n);
  return n;
}

/* A string field at in[*pos]: FALSE when its bytes are not all there. */
static inline bool botHostRecordGetStr(const uint8_t *in, int len, int *pos,
                                       char *out, int cap) {
  int n;
  if (*pos >= len) return false;
  n = in[*pos];
  if (*pos + 1 + n > len) return false;
  if (n > cap - 1) n = cap - 1;
  memcpy(out, in + *pos + 1, (size_t) n);
  out[n] = '\0';
  *pos += 1 + in[*pos];
  return true;
}

/* Read a payload of len bytes into rec. FALSE when it is shorter than its
   kind's fields, its version is 0, or its kind is not known. Bytes after the
   fields (a later version's) are left alone. */
static inline bool botHostRecordDecode(const uint8_t *in, int len, BotHostRecord *rec) {
  int pos = 3;

  if (rec == NULL) return false;
  memset(rec, 0, sizeof(*rec));
  if (in == NULL || len < 3 || in[0] == 0) return false;
  rec->version = in[0];
  rec->kind = in[1];
  rec->host = in[2];
  switch (rec->kind) {
  case BOTHOST_RECORD_HOST:
    if (len < pos + 1) return false;
    rec->player = in[pos++];
    return botHostRecordGetStr(in, len, &pos, rec->name, BOTHOST_RECORD_NAME_LEN);
  case BOTHOST_RECORD_PROBE:
    if (len < pos + 10) return false;
    rec->bots      = in[pos++];
    rec->slots     = in[pos++];
    rec->limit     = in[pos++];
    rec->thinks    = botHostRecordGet16(in + pos); pos += 2;
    rec->overruns  = botHostRecordGet16(in + pos); pos += 2;
    rec->flags     = in[pos++];
    rec->badProbes = in[pos++];
    rec->probes    = in[pos++];
    rec->windowRate = (uint16_t) BOTHOST_RECORD_RATE_NONE;
    rec->ceiling = (uint8_t) BOTHOST_RECORD_NONE;
    rec->start = (uint8_t) BOTHOST_RECORD_NONE;
    if (len >= pos + 2) {
      rec->windowRate = botHostRecordGet16(in + pos);
      pos += 2;
    }
    if (len >= pos + 2) {
      rec->ceiling = in[pos++];
      rec->start   = in[pos++];
    }
    return true;
  case BOTHOST_RECORD_READING:
    if (len < pos + 4) return false;
    rec->freePct = in[pos++];
    rec->botCpu  = in[pos++];
    rec->bots    = in[pos++];
    rec->flags   = in[pos++];
    return true;
  case BOTHOST_RECORD_DECISION:
    if (len < pos + 16) return false;
    rec->decision        = in[pos++];
    rec->limitBefore     = in[pos++];
    rec->limitAfter      = in[pos++];
    rec->badProbes       = in[pos++];
    rec->probes          = in[pos++];
    rec->flags           = in[pos++];
    rec->predictedTenths = (int16_t) botHostRecordGet16(in + pos); pos += 2;
    rec->costTenths      = botHostRecordGet16(in + pos);           pos += 2;
    rec->freeAvgTenths   = botHostRecordGet16(in + pos);           pos += 2;
    rec->lowestClean     = in[pos++];
    rec->highestBad      = in[pos++];
    rec->seat            = in[pos++];
    rec->toHost          = in[pos++];
    rec->srcRate = (uint16_t) BOTHOST_RECORD_RATE_NONE;
    rec->dstRate = (uint16_t) BOTHOST_RECORD_RATE_NONE;
    rec->moves = (uint8_t) BOTHOST_RECORD_NONE;
    rec->rule = (uint8_t) BOTHOST_RULE_NONE;
    rec->staySecs = (uint16_t) BOTHOST_RECORD_SECS_NONE;
    rec->needStaySecs = (uint16_t) BOTHOST_RECORD_SECS_NONE;
    if (pos < len &&
        botHostRecordGetStr(in, len, &pos, rec->reason, BOTHOST_RECORD_REASON_LEN) &&
        len >= pos + 10) {
      rec->srcRate      = botHostRecordGet16(in + pos); pos += 2;
      rec->dstRate      = botHostRecordGet16(in + pos); pos += 2;
      rec->moves        = in[pos++];
      rec->rule         = in[pos++];
      rec->staySecs     = botHostRecordGet16(in + pos); pos += 2;
      rec->needStaySecs = botHostRecordGet16(in + pos); pos += 2;
    }
    return true;
  case BOTHOST_RECORD_CPU:
    if (len < pos + 15) return false;
    rec->shownMilli    = botHostRecordGet32(in + pos); pos += 4;
    rec->source        = in[pos++];
    rec->budgetMilli   = botHostRecordGet32(in + pos); pos += 4;
    rec->botCpu        = in[pos++];
    rec->bots          = in[pos++];
    rec->overrunTenths = botHostRecordGet16(in + pos); pos += 2;
    rec->freePct       = in[pos++];
    rec->slots         = in[pos++];
    return true;
  default:
    return false;
  }
}

#endif /* BOTHOST_RECORD_H */
