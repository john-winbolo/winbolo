#!/usr/bin/env python3
# Copyright (c) 1998-2026 John Morrison.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw round logs (.wbv) side by side in one MP4, with WbvRender and ffmpeg.

Each log is drawn by WbvRender with the same fixed view, from the same game
time, so frame n of every panel is the same tick of its own round. Each panel
gets its caption and the game time at the top left. The panels are put next
to each other, left to right in the order given. One log makes a plain clip.

    python -I tools/arena_sbs.py --wbvrender <build>/WbvRender.exe \\
        --view 114 104 32 24 --scale 2 --out both.mp4 \\
        v1.wbv "v1 double take" v2.wbv "v2 double take"

--view is the top-left map square and the size in squares, as WbvRender's
-view takes it. --from / --to cut the game, in seconds. A panel whose round
is shorter holds its last frame until the longest one ends.

WbvRender and ffmpeg run in the background of this script; it prints each
panel's frame count as WbvRender reports it, and writes "N/M" panels done to
--progress when that is given.
"""

import argparse
import os
import subprocess
import sys
import tempfile

FONT = "C\\:/Windows/Fonts/consola.ttf" if os.name == "nt" else \
       "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"


def label_filter(text):
    t = text.replace("\\", "\\\\").replace("'", "’").replace(":", "\\:")
    common = "fontfile='%s':fontcolor=white:box=1:boxcolor=black@0.6:" \
             "boxborderw=6:fontsize=28" % FONT
    return ("drawtext=%s:x=10:y=10:text='%s',"
            "drawtext=%s:x=10:y=50:text='%%{pts\\:hms}'" % (common, t, common))


def render_panel(exe, wbv, caption, args, out_mp4):
    cmd = [exe, wbv, "-out", "-", "-view"] + [str(v) for v in args.view] + \
          ["-scale", str(args.scale), "-fps", str(args.fps), "-quiet"]
    if args.frm is not None:
        cmd += ["-from", str(args.frm)]
    if args.to is not None:
        cmd += ["-to", str(args.to)]
    w = args.view[2] * 16 * args.scale
    h = args.view[3] * 16 * args.scale
    ff = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo",
          "-pixel_format", "rgb24", "-video_size", "%dx%d" % (w, h),
          "-framerate", str(args.fps), "-i", "-", "-vf",
          label_filter(caption), "-c:v", "libx264", "-pix_fmt", "yuv420p",
          "-crf", "20", out_mp4]
    # WbvRender runs from its own folder: the data folder is beside it.
    src = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                           cwd=os.path.dirname(os.path.abspath(exe)))
    enc = subprocess.Popen(ff, stdin=src.stdout)
    src.stdout.close()
    rc_enc = enc.wait()
    rc_src = src.wait()
    if rc_src != 0 or rc_enc != 0:
        sys.exit("panel %s failed: WbvRender %d, ffmpeg %d"
                 % (wbv, rc_src, rc_enc))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wbvrender", required=True)
    ap.add_argument("--view", type=int, nargs=4, required=True,
                    metavar=("X", "Y", "W", "H"))
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--fps", type=int, default=50)
    ap.add_argument("--from", dest="frm", default=None)
    ap.add_argument("--to", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--progress", default=None)
    ap.add_argument("panels", nargs="+",
                    help="pairs: <file.wbv> <caption>")
    args = ap.parse_args()
    if len(args.panels) % 2:
        sys.exit("panels come in pairs: <file.wbv> <caption>")
    pairs = list(zip(args.panels[0::2], args.panels[1::2]))

    tmp = tempfile.mkdtemp(prefix="arena_sbs_")
    clips = []
    for i, (wbv, caption) in enumerate(pairs):
        clip = os.path.join(tmp, "panel_%d.mp4" % i)
        render_panel(args.wbvrender, os.path.abspath(wbv), caption, args,
                     clip)
        clips.append(clip)
        if args.progress:
            with open(args.progress, "a") as f:
                f.write("arena_sbs %s: %d/%d panels\n"
                        % (os.path.basename(args.out), i + 1, len(pairs)))

    if len(clips) == 1:
        os.replace(clips[0], args.out)
    else:
        ins = []
        for c in clips:
            ins += ["-i", c]
        # tpad holds each panel's last frame for ever, so a short round does
        # not end the clip; -t then cuts at the longest panel's end.
        n = len(clips)
        pads = ";".join("[%d:v]tpad=stop=-1:stop_mode=clone[p%d]" % (i, i)
                        for i in range(n))
        stack = "".join("[p%d]" % i for i in range(n)) + \
                "hstack=inputs=%d:shortest=1[v]" % n
        dur = max(float(subprocess.check_output(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", c]).decode().strip()) for c in clips)
        subprocess.check_call(["ffmpeg", "-y", "-loglevel", "error"] + ins +
                              ["-filter_complex", pads + ";" + stack,
                               "-map", "[v]", "-t", "%.3f" % dur,
                               "-c:v", "libx264", "-pix_fmt", "yuv420p",
                               "-crf", "20", args.out])
    for c in clips:
        if os.path.exists(c):
            os.remove(c)
    os.rmdir(tmp)
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
