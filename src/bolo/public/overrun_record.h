/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Overrun record
 *Filename:      overrun_record.h
 *Purpose:
 *  The payload of the log_OverrunStats record in a round log
 *  (.wbv): the server's timing and the bots' overruns, the
 *  same numbers the server's overrun lines print to the
 *  console. The server writes one every 30 seconds of a round,
 *  and one when the round's log closes.
 *
 *  The layout is in docs/replay-format.md. Every multi-byte
 *  field is big-endian. A time is in tenths of a millisecond
 *  as a u16, so 65535 (6553.5 ms) is the most it holds.
 *
 *  Version 2 adds where each bot's brain ran: a host byte per
 *  bot after the bots, then a table of the Bot Host names those
 *  bytes point to. A bot run in the server's own process has
 *  host 0 (the server); a bot on a Bot Host has the 1-based
 *  place of its host's name in the table. A version 1 record
 *  reads with every host not known.
 *
 *  Header only, with no includes but the C library's, so the
 *  server, the viewer (C and C++) and the unit tests use the
 *  same code.
 *********************************************************/

#ifndef OVERRUN_RECORD_H
#define OVERRUN_RECORD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The layout this header writes. A reader takes the fields it knows from a
   record of this version or a later one, and a later one adds its fields
   after them. */
#define OVERRUN_RECORD_VERSION 2

/* Flag bits. ROUND_END: written when the round's log closed, not at one of
   the timed points. BOTS: the round had bots, so the bot pool fields hold
   values. */
#define OVERRUN_RECORD_FLAG_ROUND_END 0x01u
#define OVERRUN_RECORD_FLAG_BOTS      0x02u

/* The fixed part, and each bot's part after it. */
#define OVERRUN_RECORD_HEAD_BYTES 44
#define OVERRUN_RECORD_BOT_BYTES  13
/* One bot part for each player the game can hold (MAX_TANKS). */
#define OVERRUN_RECORD_MAX_BOTS   16
/* Version 2's host table: at most one host for each bot, each name at most
   OVERRUN_RECORD_HOST_NAME_LEN - 1 bytes on the wire (a length byte, then
   the bytes, no terminator). */
#define OVERRUN_RECORD_MAX_HOSTS     OVERRUN_RECORD_MAX_BOTS
#define OVERRUN_RECORD_HOST_NAME_LEN 32
#define OVERRUN_RECORD_MAX_BYTES \
  (OVERRUN_RECORD_HEAD_BYTES + OVERRUN_RECORD_MAX_BOTS * OVERRUN_RECORD_BOT_BYTES + \
   OVERRUN_RECORD_MAX_BOTS + 1 + \
   OVERRUN_RECORD_MAX_HOSTS * OVERRUN_RECORD_HOST_NAME_LEN)

/* A bot's host byte: the server's own process, a place in the host table
   (1 to hostCount), or not known (a version 1 record, or a Bot Host whose
   name the server did not have). */
#define OVERRUN_RECORD_HOST_SERVER 0u
#define OVERRUN_RECORD_HOST_NONE   0xFFu

/* One bot. Times in tenths of a ms. */
typedef struct {
  uint8_t  player;     /* player number (slot) */
  uint16_t last;       /* the bot's last think */
  uint16_t peak;       /* its worst think this round */
  uint32_t overruns;   /* thinks over the target this round */
  uint32_t thinks;     /* thinks this round */
  uint8_t  host;       /* version 2: OVERRUN_RECORD_HOST_SERVER, a place in
                          hostNames (1-based), or OVERRUN_RECORD_HOST_NONE */
} OverrunRecordBot;

/* The whole record. Times in tenths of a ms. */
typedef struct {
  uint8_t  version;
  uint8_t  flags;            /* OVERRUN_RECORD_FLAG_* */
  uint32_t roundFrames;      /* running 20 ms server frames of the round so far */
  uint16_t tickLast;         /* server tick */
  uint16_t tickEwma;
  uint16_t tickPeak;
  uint32_t ticksOverBudget;  /* ticks that took the budget or more */
  uint8_t  budgetMs;         /* the tick budget, in whole ms */
  uint16_t simLast;          /* the two simulation steps of a tick */
  uint16_t simEwma;
  uint8_t  workers;          /* bot pool worker threads; 0 is single thread */
  uint8_t  activeBots;
  uint16_t targetPerBot;     /* think target for one bot */
  uint16_t brainLast;        /* the bot pool's brain phase */
  uint16_t brainEwma;
  uint16_t brainPeak;        /* the worst single think in the pool */
  uint16_t prepLast;         /* the bot pool's work outside the brains */
  uint16_t prepEwma;
  uint32_t totalOverruns;    /* the pool's overruns this round */
  uint32_t totalThinks;      /* the pool's thinks this round */
  uint8_t  botCount;         /* entries used in bots[] */
  OverrunRecordBot bots[OVERRUN_RECORD_MAX_BOTS];
  uint8_t  hostCount;        /* version 2: names used in hostNames[] */
  char     hostNames[OVERRUN_RECORD_MAX_HOSTS][OVERRUN_RECORD_HOST_NAME_LEN];
} OverrunRecord;

/* A time in ms as tenths of a ms, rounded, and held to what a u16 holds. */
static inline uint16_t overrunRecordTenths(double ms) {
  double t = ms * 10.0 + 0.5;
  if (!(t > 0.0)) {
    return 0;
  }
  if (t >= 65535.0) {
    return 65535u;
  }
  return (uint16_t) t;
}

static inline void overrunRecordPut16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t) ((v >> 8) & 0xFFu);
  p[1] = (uint8_t) (v & 0xFFu);
}

static inline void overrunRecordPut32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) ((v >> 24) & 0xFFu);
  p[1] = (uint8_t) ((v >> 16) & 0xFFu);
  p[2] = (uint8_t) ((v >> 8) & 0xFFu);
  p[3] = (uint8_t) (v & 0xFFu);
}

static inline uint16_t overrunRecordGet16(const uint8_t *p) {
  return (uint16_t) (((unsigned) p[0] << 8) | (unsigned) p[1]);
}

static inline uint32_t overrunRecordGet32(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
         ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

/* The length of a host name as the record holds it: at most
   OVERRUN_RECORD_HOST_NAME_LEN - 1 bytes. */
static inline int overrunRecordHostNameLen(const char *name) {
  int n = 0;
  while (n < OVERRUN_RECORD_HOST_NAME_LEN - 1 && name[n] != '\0') {
    n++;
  }
  return n;
}

/* The host byte for a Bot Host named name: its place in rec's host table,
   added at the end when it is not there yet. An empty name, or a full
   table, gives OVERRUN_RECORD_HOST_NONE. A name longer than the table holds
   is cut, and two names the same after the cut share a place. */
static inline uint8_t overrunRecordHostPlace(OverrunRecord *rec, const char *name) {
  char cut[OVERRUN_RECORD_HOST_NAME_LEN];
  int n;
  int i;

  if (rec == NULL || name == NULL || name[0] == '\0') {
    return OVERRUN_RECORD_HOST_NONE;
  }
  n = overrunRecordHostNameLen(name);
  memcpy(cut, name, (size_t) n);
  cut[n] = '\0';
  for (i = 0; i < rec->hostCount && i < OVERRUN_RECORD_MAX_HOSTS; i++) {
    if (strcmp(rec->hostNames[i], cut) == 0) {
      return (uint8_t) (i + 1);
    }
  }
  if (rec->hostCount >= OVERRUN_RECORD_MAX_HOSTS) {
    return OVERRUN_RECORD_HOST_NONE;
  }
  memcpy(rec->hostNames[rec->hostCount], cut, (size_t) n + 1);
  rec->hostCount++;
  return rec->hostCount;
}

/* The Bot Host name of bot b of rec, or NULL when the bot ran in the
   server's process or its host is not known (overrunRecordBotOnServer tells
   the two apart). */
static inline const char *overrunRecordBotHostName(const OverrunRecord *rec,
                                                   const OverrunRecordBot *b) {
  if (rec == NULL || b == NULL || b->host == OVERRUN_RECORD_HOST_SERVER ||
      b->host == OVERRUN_RECORD_HOST_NONE || b->host > rec->hostCount) {
    return NULL;
  }
  return rec->hostNames[b->host - 1];
}

/* TRUE when bot b ran in the server's own process. */
static inline bool overrunRecordBotOnServer(const OverrunRecordBot *b) {
  return b != NULL && b->host == OVERRUN_RECORD_HOST_SERVER;
}

/* Write rec as the payload into out, which holds cap bytes. The version byte
   written is OVERRUN_RECORD_VERSION, whatever rec says. Returns the bytes
   written, or 0 when cap is too small. */
static inline int overrunRecordEncode(const OverrunRecord *rec, uint8_t *out, int cap) {
  int count = rec->botCount;
  int hosts = rec->hostCount;
  int need;
  int i;
  uint8_t *p = out;

  if (count > OVERRUN_RECORD_MAX_BOTS) {
    count = OVERRUN_RECORD_MAX_BOTS;
  }
  if (hosts > OVERRUN_RECORD_MAX_HOSTS) {
    hosts = OVERRUN_RECORD_MAX_HOSTS;
  }
  need = OVERRUN_RECORD_HEAD_BYTES + count * OVERRUN_RECORD_BOT_BYTES + count + 1;
  for (i = 0; i < hosts; i++) {
    need += 1 + overrunRecordHostNameLen(rec->hostNames[i]);
  }
  if (out == NULL || cap < need) {
    return 0;
  }
  *p++ = OVERRUN_RECORD_VERSION;
  *p++ = rec->flags;
  overrunRecordPut32(p, rec->roundFrames);     p += 4;
  overrunRecordPut16(p, rec->tickLast);        p += 2;
  overrunRecordPut16(p, rec->tickEwma);        p += 2;
  overrunRecordPut16(p, rec->tickPeak);        p += 2;
  overrunRecordPut32(p, rec->ticksOverBudget); p += 4;
  *p++ = rec->budgetMs;
  overrunRecordPut16(p, rec->simLast);         p += 2;
  overrunRecordPut16(p, rec->simEwma);         p += 2;
  *p++ = rec->workers;
  *p++ = rec->activeBots;
  overrunRecordPut16(p, rec->targetPerBot);    p += 2;
  overrunRecordPut16(p, rec->brainLast);       p += 2;
  overrunRecordPut16(p, rec->brainEwma);       p += 2;
  overrunRecordPut16(p, rec->brainPeak);       p += 2;
  overrunRecordPut16(p, rec->prepLast);        p += 2;
  overrunRecordPut16(p, rec->prepEwma);        p += 2;
  overrunRecordPut32(p, rec->totalOverruns);   p += 4;
  overrunRecordPut32(p, rec->totalThinks);     p += 4;
  *p++ = (uint8_t) count;
  for (i = 0; i < count; i++) {
    const OverrunRecordBot *b = &rec->bots[i];
    *p++ = b->player;
    overrunRecordPut16(p, b->last);      p += 2;
    overrunRecordPut16(p, b->peak);      p += 2;
    overrunRecordPut32(p, b->overruns);  p += 4;
    overrunRecordPut32(p, b->thinks);    p += 4;
  }
  /* Version 2: each bot's host, then the host table. */
  for (i = 0; i < count; i++) {
    uint8_t h = rec->bots[i].host;
    if (h != OVERRUN_RECORD_HOST_SERVER && h > hosts) {
      h = OVERRUN_RECORD_HOST_NONE;
    }
    *p++ = h;
  }
  *p++ = (uint8_t) hosts;
  for (i = 0; i < hosts; i++) {
    int n = overrunRecordHostNameLen(rec->hostNames[i]);
    *p++ = (uint8_t) n;
    memcpy(p, rec->hostNames[i], (size_t) n);
    p += n;
  }
  return (int) (p - out);
}

/* Read a payload of len bytes into rec. FALSE when it is shorter than the
   fixed part or its version is 0. A record that says it has more bots than
   its bytes hold keeps the ones that are there, and bytes after the last bot
   (a later version's) are left alone. Each bot's host is
   OVERRUN_RECORD_HOST_NONE unless a record of version 2 or later holds every
   bot it says it has, the host bytes and the whole host table; a host byte
   past the table reads as not known. */
static inline bool overrunRecordDecode(const uint8_t *in, int len, OverrunRecord *rec) {
  const uint8_t *p = in;
  int count;
  int room;
  int i;

  if (rec == NULL) {
    return false;
  }
  memset(rec, 0, sizeof(*rec));
  if (in == NULL || len < OVERRUN_RECORD_HEAD_BYTES || in[0] == 0) {
    return false;
  }
  rec->version         = *p++;
  rec->flags           = *p++;
  rec->roundFrames     = overrunRecordGet32(p); p += 4;
  rec->tickLast        = overrunRecordGet16(p); p += 2;
  rec->tickEwma        = overrunRecordGet16(p); p += 2;
  rec->tickPeak        = overrunRecordGet16(p); p += 2;
  rec->ticksOverBudget = overrunRecordGet32(p); p += 4;
  rec->budgetMs        = *p++;
  rec->simLast         = overrunRecordGet16(p); p += 2;
  rec->simEwma         = overrunRecordGet16(p); p += 2;
  rec->workers         = *p++;
  rec->activeBots      = *p++;
  rec->targetPerBot    = overrunRecordGet16(p); p += 2;
  rec->brainLast       = overrunRecordGet16(p); p += 2;
  rec->brainEwma       = overrunRecordGet16(p); p += 2;
  rec->brainPeak       = overrunRecordGet16(p); p += 2;
  rec->prepLast        = overrunRecordGet16(p); p += 2;
  rec->prepEwma        = overrunRecordGet16(p); p += 2;
  rec->totalOverruns   = overrunRecordGet32(p); p += 4;
  rec->totalThinks     = overrunRecordGet32(p); p += 4;
  count                = *p++;
  room = (len - OVERRUN_RECORD_HEAD_BYTES) / OVERRUN_RECORD_BOT_BYTES;
  if (count > room) {
    count = room;
  }
  if (count > OVERRUN_RECORD_MAX_BOTS) {
    count = OVERRUN_RECORD_MAX_BOTS;
  }
  rec->botCount = (uint8_t) count;
  for (i = 0; i < count; i++) {
    OverrunRecordBot *b = &rec->bots[i];
    b->player   = *p++;
    b->last     = overrunRecordGet16(p); p += 2;
    b->peak     = overrunRecordGet16(p); p += 2;
    b->overruns = overrunRecordGet32(p); p += 4;
    b->thinks   = overrunRecordGet32(p); p += 4;
    b->host     = OVERRUN_RECORD_HOST_NONE;
    /* The player number indexes per-player tables (MAX_TANKS). */
    if (b->player >= OVERRUN_RECORD_MAX_BOTS) {
      memset(rec, 0, sizeof(*rec));
      return false;
    }
  }
  /* Version 2's hosts. */
  if (rec->version >= 2 && count == in[OVERRUN_RECORD_HEAD_BYTES - 1]) {
    const uint8_t *end = in + len;
    const uint8_t *hostBytes = p;
    int hosts;
    if (end - p < count + 1) {
      return true;
    }
    p += count;
    hosts = *p++;
    if (hosts > OVERRUN_RECORD_MAX_HOSTS) {
      return true;
    }
    for (i = 0; i < hosts; i++) {
      int n;
      if (p >= end) {
        break;
      }
      n = *p++;
      if (n > OVERRUN_RECORD_HOST_NAME_LEN - 1 || end - p < n) {
        break;
      }
      memcpy(rec->hostNames[i], p, (size_t) n);
      rec->hostNames[i][n] = '\0';
      p += n;
    }
    if (i < hosts) {
      memset(rec->hostNames, 0, sizeof(rec->hostNames));
      return true;
    }
    rec->hostCount = (uint8_t) hosts;
    for (i = 0; i < count; i++) {
      uint8_t h = hostBytes[i];
      if (h != OVERRUN_RECORD_HOST_SERVER && h > hosts) {
        h = OVERRUN_RECORD_HOST_NONE;
      }
      rec->bots[i].host = h;
    }
  }
  return true;
}

/* The most sections overrunRecordSections gives: the server, each host in
   the table, and the bots whose host is not known. */
#define OVERRUN_RECORD_MAX_SECTIONS (OVERRUN_RECORD_MAX_HOSTS + 2)

/* The sections a display groups rec's bots into, one header for each host
   that has a bot: the server's own process first, then each Bot Host in the
   host table's order, then the bots whose host is not known. keys gets each
   section's host byte (OVERRUN_RECORD_HOST_SERVER, a place in the table, or
   OVERRUN_RECORD_HOST_NONE), at most cap of them. A bot is in the section
   whose key equals its host byte, or in the not known section when its byte
   is past the table. Returns the number of sections, or 0 when no bot has a
   known host (a version 1 record): the display then shows one list with no
   headers. */
static inline int overrunRecordSections(const OverrunRecord *rec, uint8_t *keys, int cap) {
  bool used[OVERRUN_RECORD_MAX_HOSTS + 1];
  bool none = false;
  bool known = false;
  int count;
  int hosts;
  int n = 0;
  int i;

  if (rec == NULL || keys == NULL || cap <= 0) {
    return 0;
  }
  memset(used, 0, sizeof(used));
  count = rec->botCount > OVERRUN_RECORD_MAX_BOTS ? OVERRUN_RECORD_MAX_BOTS : rec->botCount;
  hosts = rec->hostCount > OVERRUN_RECORD_MAX_HOSTS ? OVERRUN_RECORD_MAX_HOSTS : rec->hostCount;
  for (i = 0; i < count; i++) {
    uint8_t h = rec->bots[i].host;
    if (h == OVERRUN_RECORD_HOST_SERVER || (h != OVERRUN_RECORD_HOST_NONE && h <= hosts)) {
      used[h] = true;
      known = true;
    } else {
      none = true;
    }
  }
  if (!known) {
    return 0;
  }
  for (i = 0; i <= hosts && n < cap; i++) {
    if (used[i]) {
      keys[n++] = (uint8_t) i;
    }
  }
  if (none && n < cap) {
    keys[n++] = OVERRUN_RECORD_HOST_NONE;
  }
  return n;
}

/* TRUE when bot b goes under the section with host byte key
   (overrunRecordSections). */
static inline bool overrunRecordBotInSection(const OverrunRecord *rec,
                                             const OverrunRecordBot *b, uint8_t key) {
  uint8_t h;
  if (rec == NULL || b == NULL) {
    return false;
  }
  h = b->host;
  if (h != OVERRUN_RECORD_HOST_SERVER && h > rec->hostCount) {
    h = OVERRUN_RECORD_HOST_NONE;
  }
  return h == key;
}

#endif /* OVERRUN_RECORD_H */
