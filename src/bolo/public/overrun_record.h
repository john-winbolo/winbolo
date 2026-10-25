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
#define OVERRUN_RECORD_VERSION 1

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
#define OVERRUN_RECORD_MAX_BYTES \
  (OVERRUN_RECORD_HEAD_BYTES + OVERRUN_RECORD_MAX_BOTS * OVERRUN_RECORD_BOT_BYTES)

/* One bot. Times in tenths of a ms. */
typedef struct {
  uint8_t  player;     /* player number (slot) */
  uint16_t last;       /* the bot's last think */
  uint16_t peak;       /* its worst think this round */
  uint32_t overruns;   /* thinks over the target this round */
  uint32_t thinks;     /* thinks this round */
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

/* Write rec as the payload into out, which holds cap bytes. The version byte
   written is OVERRUN_RECORD_VERSION, whatever rec says. Returns the bytes
   written, or 0 when cap is too small. */
static inline int overrunRecordEncode(const OverrunRecord *rec, uint8_t *out, int cap) {
  int count = rec->botCount;
  int need;
  int i;
  uint8_t *p = out;

  if (count > OVERRUN_RECORD_MAX_BOTS) {
    count = OVERRUN_RECORD_MAX_BOTS;
  }
  need = OVERRUN_RECORD_HEAD_BYTES + count * OVERRUN_RECORD_BOT_BYTES;
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
  return (int) (p - out);
}

/* Read a payload of len bytes into rec. FALSE when it is shorter than the
   fixed part or its version is 0. A record that says it has more bots than
   its bytes hold keeps the ones that are there, and bytes after the last bot
   (a later version's) are left alone. */
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
  }
  return true;
}

#endif /* OVERRUN_RECORD_H */
