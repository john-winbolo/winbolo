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
 *             [flags 1][badProbes 1][probes 1]
 *             one per host each second. limit 0xFF: none.
 *             flags bit 0: this probe is bad. badProbes of the
 *             probes in the window are bad.
 *    READING  [free 1][botCpu 1][bots 1][flags 1]
 *             one per host each free CPU sample. free 0xFF:
 *             not known; botCpu is the CTRL_BOT_HOST_CPU byte.
 *             flags bit 0: the bots thought; bit 1: bad.
 *    DECISION [decision 1][limitBefore 1][limitAfter 1]
 *             [badProbes 1][probes 1][flags 1][predicted 2]
 *             [oneBotCost 2][freeAvg 2][lowestClean 1]
 *             [highestBad 1][seat 1][toHost 1][reasonLen 1]
 *             [reason]
 *             predicted is a signed tenth of a percent; cost
 *             and freeAvg are tenths of a percent. 0xFF in a
 *             byte field: none. flags bit 0: the one-bot cost
 *             is known; bit 1: the free average is known.
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

#define BOTHOST_RECORD_VERSION 1

enum {
  BOTHOST_RECORD_HOST     = 1,
  BOTHOST_RECORD_PROBE    = 2,
  BOTHOST_RECORD_READING  = 3,
  BOTHOST_RECORD_DECISION = 4
};

/* DECISION kinds. */
enum {
  BOTHOST_DECISION_TRIGGER        = 1, /* the bad probe share was met */
  BOTHOST_DECISION_MOVE           = 2, /* a bot moved off the host */
  BOTHOST_DECISION_CUT            = 3, /* the limit was lowered */
  BOTHOST_DECISION_RAISE          = 4, /* the limit went up by 1 */
  BOTHOST_DECISION_RAISE_REFUSED  = 5, /* a raise was looked at and refused */
  BOTHOST_DECISION_PLACE_REFUSED  = 6  /* the host may not take a new bot */
};

#define BOTHOST_RECORD_NONE        0xFFu
#define BOTHOST_RECORD_NAME_LEN    32
#define BOTHOST_RECORD_REASON_LEN  64
#define BOTHOST_RECORD_PROBE_BAD   0x01u
#define BOTHOST_RECORD_READING_RAN 0x01u
#define BOTHOST_RECORD_READING_BAD 0x02u
#define BOTHOST_RECORD_DEC_COST    0x01u
#define BOTHOST_RECORD_DEC_FREE    0x02u
/* The longest payload: a DECISION with the longest reason. */
#define BOTHOST_RECORD_MAX_BYTES   (3 + 16 + BOTHOST_RECORD_REASON_LEN)

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
  uint8_t  badProbes;  /* PROBE and DECISION */
  uint8_t  probes;     /* PROBE and DECISION */
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
} BotHostRecord;

static inline void botHostRecordPut16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t) ((v >> 8) & 0xFFu);
  p[1] = (uint8_t) (v & 0xFFu);
}

static inline uint16_t botHostRecordGet16(const uint8_t *p) {
  return (uint16_t) (((unsigned) p[0] << 8) | (unsigned) p[1]);
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
    if (pos < len) {
      (void) botHostRecordGetStr(in, len, &pos, rec->reason, BOTHOST_RECORD_REASON_LEN);
    }
    return true;
  default:
    return false;
  }
}

#endif /* BOTHOST_RECORD_H */
