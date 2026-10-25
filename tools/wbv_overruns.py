#!/usr/bin/env python3
# Copyright (c) 1998-2026 John Morrison.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print the server timing records (log_OverrunStats) of a WinBolo round log.

A round log (.wbv) is a ZIP whose member log.dat holds the recording. The
server writes a log_OverrunStats record every 30 seconds of a round, and one
more when the round's log closes. Each holds the numbers the server's console
prints in its overrun lines (the console prints them only at 1:00 and then
every 5 minutes). This tool prints each record in
the shape of those lines, with no window. The layout is in
docs/replay-format.md ("log_OverrunStats payload").

    python tools/wbv_overruns.py ROUND.wbv [ROUND2.wbv ...]

A file recorded before the record existed has none, and the tool says so.
Only V2 and V3 files (plaintext, framed events) are read; V0 and V1 files are
from before the record and are reported as such.
"""

import struct
import sys
import zipfile
import zlib

# The most bytes of log.dat the viewer reads (LOG_MAX_SIZE in
# src/logviewer/blocks.c). A member over this is refused before it is
# unpacked, so a small zip cannot make the tool unpack gigabytes.
LOG_MAX_SIZE = 512 * 1024 * 1024

# The newest log version this tool reads. A later version may change the
# layout, so it is refused rather than read as V3.
LOG_VERSION_MAX = 3

LOG_QUIT = 0
LOG_NOEVENTS = 1
LOG_NOEVENTS_LONG = 2
LOG_EVENT = 3
LOG_EVENT_LONG = 4
LOG_EVENT_SNAPSHOT = 5

LOG_PLAYER_JOINED = 1
LOG_CHANGE_NAME = 21
LOG_OVERRUN_STATS = 72

MAX_TANKS = 16

HEAD_BYTES = 44
BOT_BYTES = 13
FLAG_ROUND_END = 0x01
FLAG_BOTS = 0x02


class LogError(Exception):
    pass


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise LogError("the stream ends inside a record at byte %d" % self.pos)
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self):
        return self.take(1)[0]

    def pascal(self):
        return self.take(self.u8())

    def done(self):
        return self.pos >= len(self.data)


def pascal_at(buf, at):
    """The Pascal string at buf[at], or None when the buffer is too short."""
    if at >= len(buf):
        return None
    n = buf[at]
    if at + 1 + n > len(buf):
        return None
    return buf[at + 1:at + 1 + n].decode("utf-8", "replace")


def read_header(r):
    if r.take(8) != b"WBOLOMOV":
        raise LogError("log.dat does not start with WBOLOMOV")
    version = r.u8()
    map_name = r.pascal().decode("utf-8", "replace")
    # Game type, hidden mines, AI, password, max players, 3 version bytes,
    # IP 4, port 2, create time 4, key 32.
    r.take(1 + 1 + 1 + 1 + 1 + 3 + 4 + 2 + 4 + 32)
    return version, map_name


def read_snapshot(r, names):
    """Skip a snapshot body, keeping each player block's name."""
    r.take(8)                      # start delay, game length
    for _ in range(3):             # pills, bases, starts
        r.pascal()
    while True:                    # map runs, up to the y == 255 run
        run_len = r.u8()
        if run_len < 4:
            raise LogError("a map run shorter than its header at byte %d" % r.pos)
        run = r.take(run_len - 1)
        if run[0] == 0xFF:
            break
    for _ in range(MAX_TANKS):
        block = r.pascal()
        if len(block) >= 2 and block[1]:
            name = pascal_at(block, 11)
            if name:
                names[block[0]] = name


def decode_overrun(p):
    """The record as a dict, or None for one shorter than its fixed part or
    of version 0 (a reader ignores those)."""
    if len(p) < HEAD_BYTES or p[0] == 0:
        return None
    (version, flags, frames, tick_last, tick_ewma, tick_peak, over_budget,
     budget, sim_last, sim_ewma, workers, active, target, brain_last,
     brain_ewma, brain_peak, prep_last, prep_ewma, overruns, thinks,
     count) = struct.unpack(">BBIHHHIBHHBBHHHHHHIIB", p[:HEAD_BYTES])
    count = min(count, (len(p) - HEAD_BYTES) // BOT_BYTES, MAX_TANKS)
    bots = []
    for i in range(count):
        at = HEAD_BYTES + i * BOT_BYTES
        player, last, peak, b_over, b_thinks = struct.unpack(
            ">BHHII", p[at:at + BOT_BYTES])
        bots.append((player, last, peak, b_over, b_thinks))
    return dict(version=version, flags=flags, frames=frames,
                tick_last=tick_last, tick_ewma=tick_ewma, tick_peak=tick_peak,
                over_budget=over_budget, budget=budget, sim_last=sim_last,
                sim_ewma=sim_ewma, workers=workers, active=active,
                target=target, brain_last=brain_last, brain_ewma=brain_ewma,
                brain_peak=brain_peak, prep_last=prep_last,
                prep_ewma=prep_ewma, overruns=overruns, thinks=thinks,
                bots=bots)


def ms(tenths):
    return "%d.%d" % (tenths // 10, tenths % 10)


def rate(part, total):
    return "%.2f" % (part * 100.0 / total if total else 0.0)


def print_record(rec, names, out):
    secs = rec["frames"] // 50
    when = "%d:%02d" % (secs // 60, secs % 60)
    if rec["flags"] & FLAG_ROUND_END:
        out.write("Overrun stats at the round end, %s of the round:\n" % when)
    else:
        out.write("Overrun stats at %s of the round:\n" % when)
    out.write("Server timing:\n")
    out.write("  %-11s last=%sms  EWMA=%sms  peak=%sms  over budget=%d (budget=%dms)\n"
              % ("Tick:", ms(rec["tick_last"]), ms(rec["tick_ewma"]),
                 ms(rec["tick_peak"]), rec["over_budget"], rec["budget"]))
    out.write("  %-11s last=%sms  EWMA=%sms\n"
              % ("Simulation:", ms(rec["sim_last"]), ms(rec["sim_ewma"])))
    if rec["flags"] & FLAG_BOTS:
        if rec["workers"] == 0:
            out.write("Bot pool: single-thread (%d active bots), target=%sms/bot\n"
                      % (rec["active"], ms(rec["target"])))
        else:
            out.write("Bot pool: %d workers (%d active bots), target=%sms/bot\n"
                      % (rec["workers"], rec["active"], ms(rec["target"])))
        out.write("  %-11s last=%sms  EWMA=%sms  peak=%sms\n"
                  % ("Brain:", ms(rec["brain_last"]), ms(rec["brain_ewma"]),
                     ms(rec["brain_peak"])))
        out.write("  %-11s last=%sms  EWMA=%sms  total overruns=%d/%d (%s%%)\n"
                  % ("Bot prep:", ms(rec["prep_last"]), ms(rec["prep_ewma"]),
                     rec["overruns"], rec["thinks"],
                     rate(rec["overruns"], rec["thinks"])))
    if rec["bots"]:
        out.write("Bots:\n")
        for player, last, peak, b_over, b_thinks in rec["bots"]:
            name = names.get(player) or ("player %d" % player)
            out.write("  %s (player %d)\n" % (name, player))
            if b_thinks == 0:
                out.write("    [BOT] no thinks\n")
            else:
                out.write("    [BOT] last=%sms peak=%sms overruns=%d/%d (%s%%)\n"
                          % (ms(last), ms(peak), b_over, b_thinks,
                             rate(b_over, b_thinks)))
    out.write("\n")


def read_events(r, count, names, found, out):
    for _ in range(count):
        kind = r.u8()
        n = struct.unpack(">H", r.take(2))[0]
        p = r.take(n)
        if kind == LOG_PLAYER_JOINED and len(p) >= 5:
            name = pascal_at(p, 5)
            if name:
                names[p[0]] = name
        elif kind == LOG_CHANGE_NAME and len(p) >= 1:
            name = pascal_at(p, 1)
            if name:
                names[p[0]] = name
        elif kind == LOG_OVERRUN_STATS:
            rec = decode_overrun(p)
            if rec is not None:
                found.append(rec)
                print_record(rec, names, out)


def read_file(path, out):
    with zipfile.ZipFile(path) as z:
        size = z.getinfo("log.dat").file_size
        if size > LOG_MAX_SIZE:
            raise LogError("log.dat is %d bytes, over the %d byte limit the viewer reads"
                           % (size, LOG_MAX_SIZE))
        data = z.read("log.dat")
    r = Reader(data)
    version, map_name = read_header(r)
    out.write("%s\n" % path)
    out.write("Log version %d, map %s\n\n" % (version, map_name))
    if version < 2:
        out.write("No overrun records: a V%d file is from before the record existed.\n"
                  % version)
        return 0
    if version > LOG_VERSION_MAX:
        raise LogError("log version %d is newer than this tool reads (V%d)"
                       % (version, LOG_VERSION_MAX))
    names = {}
    found = []
    entries = 0
    ended = False
    while not r.done():
        op = r.u8()
        entries += 1
        if op == LOG_QUIT:
            ended = True
            break
        if op == LOG_NOEVENTS:
            r.u8()
        elif op == LOG_NOEVENTS_LONG:
            r.take(2)
        elif op == LOG_EVENT:
            read_events(r, r.u8(), names, found, out)
        elif op == LOG_EVENT_LONG:
            read_events(r, struct.unpack("<H", r.take(2))[0], names, found, out)
        elif op == LOG_EVENT_SNAPSHOT:
            read_snapshot(r, names)
        else:
            raise LogError("unknown record opcode %d at byte %d" % (op, r.pos - 1))
    if ended:
        out.write("Read %d log entries to the end of the stream.\n" % entries)
    else:
        out.write("Read %d log entries. The stream has no end marker (LOG_QUIT), so"
                  " the file was cut short; the records above are the ones before"
                  " the cut.\n" % entries)
    if not found:
        out.write("No overrun records. A server made before the record existed"
                  " recorded this file.\n")
    else:
        out.write("%d overrun record(s).\n" % len(found))
    return len(found)


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    rc = 0
    for i, path in enumerate(argv[1:]):
        if i:
            sys.stdout.write("\n")
        try:
            read_file(path, sys.stdout)
        except (OSError, KeyError, zipfile.BadZipFile, zlib.error, EOFError,
                RuntimeError, NotImplementedError, MemoryError, LogError) as e:
            # zlib.error and EOFError: a corrupt or cut deflate stream.
            # RuntimeError: an encrypted member. NotImplementedError: a
            # compression method zipfile does not have. MemoryError: a member
            # that passed the size check but still could not be held.
            sys.stderr.write("%s: %s\n" % (path, e))
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
