/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Slow tick record
 *Filename:      slow_tick_record.h
 *Purpose:
 *  The payload of the log_SlowTick record in a round log
 *  (.wbv): one server frame that took more than its 20 ms,
 *  or that started late, and how long each part of it took.
 *  The server writes one for each such frame and none for a
 *  frame on time, so a normal round has none.
 *
 *  The layout is in docs/replay-format.md. Every multi-byte
 *  field is big-endian. A time is in tenths of a millisecond:
 *  a part as a u16, so 65535 (6553.5 ms) is the most it holds,
 *  and the total, the gap and the pause as a u32.
 *
 *  Header only, with no includes but the C library's, so the
 *  server, the viewer (C and C++) and the unit tests use the
 *  same code.
 *********************************************************/

#ifndef SLOW_TICK_RECORD_H
#define SLOW_TICK_RECORD_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The layout this header writes. A reader takes the fields it knows from a
   record of this version or a later one, and a later one adds its fields
   after them. */
#define SLOW_TICK_RECORD_VERSION 1

/* The bytes of a version 1 record. */
#define SLOW_TICK_RECORD_BYTES 34

/* Why the frame was recorded; both can be set.
   LONG: the frame's own work took more than SLOW_TICK_LONG_MS.
   LATE: the frame started late. More than SLOW_TICK_LATE_GAP_MS passed from
   the start of the frame before, and more than SLOW_TICK_LATE_PAUSE_MS of
   that was outside both frames: the process was stopped, or slept too long,
   between them. */
#define SLOW_TICK_FLAG_LONG 0x01u
#define SLOW_TICK_FLAG_LATE 0x02u

/* The frame's budget: one 20 ms server frame (SERVER_TICK_LENGTH). */
#define SLOW_TICK_LONG_MS       20.0
/* Two frames: a frame that starts this long after the one before missed at
   least one slot. */
#define SLOW_TICK_LATE_GAP_MS   40.0
/* The time between the end of one frame and the start of the next is at
   most one frame on a server that keeps up. More than that is time the loop
   lost outside its frames. Without this test, the frame straight after a
   long one would be recorded too: it starts at once to catch up, so its gap
   is the long frame's own time, which that frame's record already holds. */
#define SLOW_TICK_LATE_PAUSE_MS 20.0

/* One frame. Times in tenths of a ms. */
typedef struct {
  uint8_t  version;
  uint8_t  flags;        /* SLOW_TICK_FLAG_* */
  uint32_t tick;         /* the server's game tick when the frame started */
  uint32_t total;        /* the frame's whole time, start to end */
  uint32_t gap;          /* from the start of the frame before; 0 for the first */
  uint32_t pause;        /* from the end of the frame before to this start */
  uint16_t netRecv;      /* reading the players' packets */
  uint16_t lockWait;     /* waiting for the tick mutex */
  uint16_t bots;         /* the bots' think */
  uint16_t sim;          /* the two simulation steps, less the two below */
  uint16_t scenario;     /* the scenario script (Lua mod) and its queued work */
  uint16_t recording;    /* the round log (.wbv) and the brain debug files */
  uint16_t netSend;      /* sending the snapshots and the events */
  uint16_t other;        /* the rest: total less every part above */
} SlowTickRecord;

/* The SLOW_TICK_FLAG_* bits for a frame of totalMs that started gapMs after
   the start of the frame before and pauseMs after its end. 0: the frame is
   not recorded. gapMs 0 is "no frame before", and is never late. */
static inline uint8_t slowTickRecordReason(double totalMs, double gapMs, double pauseMs) {
  uint8_t flags = 0;
  if (totalMs > SLOW_TICK_LONG_MS) {
    flags |= SLOW_TICK_FLAG_LONG;
  }
  if (gapMs > SLOW_TICK_LATE_GAP_MS && pauseMs > SLOW_TICK_LATE_PAUSE_MS) {
    flags |= SLOW_TICK_FLAG_LATE;
  }
  return flags;
}

/* A time in ms as tenths of a ms, rounded, and held to what a u16 holds. */
static inline uint16_t slowTickRecordTenths16(double ms) {
  double t = ms * 10.0 + 0.5;
  if (!(t > 0.0)) {
    return 0;
  }
  if (t >= 65535.0) {
    return 65535u;
  }
  return (uint16_t) t;
}

/* The same as a u32. */
static inline uint32_t slowTickRecordTenths32(double ms) {
  double t = ms * 10.0 + 0.5;
  if (!(t > 0.0)) {
    return 0;
  }
  if (t >= 4294967295.0) {
    return 0xFFFFFFFFu;
  }
  return (uint32_t) t;
}

/* The parts of one frame as the server times them, in ms. */
typedef struct {
  double netRecv;
  double lockWait;
  double bots;
  double sim;
  double scenario;
  double recording;
  double netSend;
} SlowTickParts;

/* Fill rec for a frame: its reason flags, the tick it started on, its times
   in ms and its parts. other is the total less the parts, and 0 when the
   parts add to more than the total (each part is rounded on its own). */
static inline void slowTickRecordFill(SlowTickRecord *rec, uint8_t flags, uint32_t tick,
                                      double totalMs, double gapMs, double pauseMs,
                                      const SlowTickParts *parts) {
  double rest = totalMs;
  memset(rec, 0, sizeof(*rec));
  rec->version = SLOW_TICK_RECORD_VERSION;
  rec->flags   = flags;
  rec->tick    = tick;
  rec->total   = slowTickRecordTenths32(totalMs);
  rec->gap     = slowTickRecordTenths32(gapMs);
  rec->pause   = slowTickRecordTenths32(pauseMs);
  if (parts != NULL) {
    rec->netRecv   = slowTickRecordTenths16(parts->netRecv);
    rec->lockWait  = slowTickRecordTenths16(parts->lockWait);
    rec->bots      = slowTickRecordTenths16(parts->bots);
    rec->sim       = slowTickRecordTenths16(parts->sim);
    rec->scenario  = slowTickRecordTenths16(parts->scenario);
    rec->recording = slowTickRecordTenths16(parts->recording);
    rec->netSend   = slowTickRecordTenths16(parts->netSend);
    rest -= parts->netRecv + parts->lockWait + parts->bots + parts->sim +
            parts->scenario + parts->recording + parts->netSend;
  }
  rec->other = slowTickRecordTenths16(rest);
}

/* How far past its slot the frame ran, in whole ms, rounded: its own time,
   or for a late frame the time it started after its slot, whichever is more.
   A viewer that hides small overruns compares this. */
static inline uint32_t slowTickRecordSizeMs(const SlowTickRecord *rec) {
  uint32_t size = rec->total;
  if ((rec->flags & SLOW_TICK_FLAG_LATE) && rec->gap > 200u &&
      rec->gap - 200u > size) {
    size = rec->gap - 200u;   /* 200 tenths: the frame's own 20 ms slot */
  }
  return (size + 5u) / 10u;
}

static inline void slowTickRecordPut16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t) ((v >> 8) & 0xFFu);
  p[1] = (uint8_t) (v & 0xFFu);
}

static inline void slowTickRecordPut32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) ((v >> 24) & 0xFFu);
  p[1] = (uint8_t) ((v >> 16) & 0xFFu);
  p[2] = (uint8_t) ((v >> 8) & 0xFFu);
  p[3] = (uint8_t) (v & 0xFFu);
}

static inline uint16_t slowTickRecordGet16(const uint8_t *p) {
  return (uint16_t) (((unsigned) p[0] << 8) | (unsigned) p[1]);
}

static inline uint32_t slowTickRecordGet32(const uint8_t *p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
         ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

/* Write rec as the payload into out, which holds cap bytes. The version byte
   written is SLOW_TICK_RECORD_VERSION, whatever rec says. Returns the bytes
   written, or 0 when cap is too small. */
static inline int slowTickRecordEncode(const SlowTickRecord *rec, uint8_t *out, int cap) {
  uint8_t *p = out;

  if (rec == NULL || out == NULL || cap < SLOW_TICK_RECORD_BYTES) {
    return 0;
  }
  *p++ = SLOW_TICK_RECORD_VERSION;
  *p++ = rec->flags;
  slowTickRecordPut32(p, rec->tick);      p += 4;
  slowTickRecordPut32(p, rec->total);     p += 4;
  slowTickRecordPut32(p, rec->gap);       p += 4;
  slowTickRecordPut32(p, rec->pause);     p += 4;
  slowTickRecordPut16(p, rec->netRecv);   p += 2;
  slowTickRecordPut16(p, rec->lockWait);  p += 2;
  slowTickRecordPut16(p, rec->bots);      p += 2;
  slowTickRecordPut16(p, rec->sim);       p += 2;
  slowTickRecordPut16(p, rec->scenario);  p += 2;
  slowTickRecordPut16(p, rec->recording); p += 2;
  slowTickRecordPut16(p, rec->netSend);   p += 2;
  slowTickRecordPut16(p, rec->other);     p += 2;
  return (int) (p - out);
}

/* Read a payload of len bytes into rec. FALSE when it is shorter than a
   version 1 record or its version is 0. Bytes after the version 1 fields (a
   later version's) are left alone. */
static inline bool slowTickRecordDecode(const uint8_t *in, int len, SlowTickRecord *rec) {
  const uint8_t *p = in;

  if (rec == NULL) {
    return false;
  }
  memset(rec, 0, sizeof(*rec));
  if (in == NULL || len < SLOW_TICK_RECORD_BYTES || in[0] == 0) {
    return false;
  }
  rec->version   = *p++;
  rec->flags     = *p++;
  rec->tick      = slowTickRecordGet32(p); p += 4;
  rec->total     = slowTickRecordGet32(p); p += 4;
  rec->gap       = slowTickRecordGet32(p); p += 4;
  rec->pause     = slowTickRecordGet32(p); p += 4;
  rec->netRecv   = slowTickRecordGet16(p); p += 2;
  rec->lockWait  = slowTickRecordGet16(p); p += 2;
  rec->bots      = slowTickRecordGet16(p); p += 2;
  rec->sim       = slowTickRecordGet16(p); p += 2;
  rec->scenario  = slowTickRecordGet16(p); p += 2;
  rec->recording = slowTickRecordGet16(p); p += 2;
  rec->netSend   = slowTickRecordGet16(p); p += 2;
  rec->other     = slowTickRecordGet16(p);
  return true;
}

#endif /* SLOW_TICK_RECORD_H */
