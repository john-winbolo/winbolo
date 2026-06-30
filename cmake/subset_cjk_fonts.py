#!/usr/bin/env python3
# Copyright (c) 1998-2026 John Morrison.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Subset the bundled CJK fonts down to the glyph ranges WinBolo can actually
# display, shrinking the WASM preload from ~163 MB of fonts to ~19 MB.
#
# Two font systems, two range policies:
#
#   * Noto Sans CJK (ImGui dialog font) is *range-pinned*: imgui_fonts.h asks
#     ImGui for GetGlyphRangesJapanese / ...Korean / ...ChineseSimplifiedCommon
#     and the atlas only ever bakes those glyphs. Subsetting each Noto OTF to
#     its requested range is therefore LOSSLESS — ImGui could not draw the rest
#     anyway. zh-TW shares the Simplified-Common range (see imgui_fonts.h).
#
#   * Sarasa Mono Slab (in-game text via SDL_ttf/FreeType) is NOT range-pinned;
#     it renders arbitrary codepoints from player names and chat. We subset it
#     to the UNION of the three ImGui ranges so common CJK still renders. A rare
#     ideograph in a name will tofu in-game — but it already tofus in the lobby
#     dialogs today (same ranges via the always-on JP/KR fallback chain), so
#     this only makes in-game coverage consistent with the lobby's.
#
# The ranges are parsed straight out of the FetchContent copy of imgui_draw.cpp
# so they stay in sync if ImGui is bumped.

import argparse
import os
import re
import sys


def _imgui_func_body(src, name):
    """Return the body of ImFontAtlas::GetGlyphRanges<name>(). Anchored on the
    qualified definition so we skip the comment block that lists the same names."""
    i = src.index("ImFontAtlas::GetGlyphRanges" + name + "()")
    j = src.index("return", i)
    return src[i:j]


def _base_ranges(body):
    m = re.search(r"base_ranges\[\][^=]*=\s*(?://[^\n]*\n)?\s*\{(.*?)\};", body, re.S)
    cps = set()
    if m:
        nums = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1))]
        for i in range(0, len(nums) - 1, 2):
            cps.update(range(nums[i], nums[i + 1] + 1))
    return cps


def _accum_ranges(body):
    """Unpack the accumulative-offsets-from-0x4E00 ideograph table."""
    m = re.search(r"offsets_from_0x4E00\[\]\s*=\s*\{(.*?)\};", body, re.S)
    nums = [int(x) for x in re.findall(r"-?\d+", m.group(1))]
    cps = set()
    base = 0x4E00
    for n in nums:
        base += n
        cps.add(base)
    return cps


def _simple_ranges(body):
    """A plain {lo, hi, ...} terminated range list (used by Korean)."""
    m = re.search(r"ranges\[\]\s*=\s*\{(.*?)\};", body, re.S)
    nums = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1))]
    cps = set()
    for i in range(0, len(nums) - 1, 2):
        cps.update(range(nums[i], nums[i + 1] + 1))
    return cps


def parse_imgui_ranges(imgui_draw_path):
    src = open(imgui_draw_path, encoding="utf-8", errors="replace").read()
    jp = _base_ranges(_imgui_func_body(src, "Japanese")) | _accum_ranges(
        _imgui_func_body(src, "Japanese"))
    sc = _base_ranges(_imgui_func_body(src, "ChineseSimplifiedCommon")) | _accum_ranges(
        _imgui_func_body(src, "ChineseSimplifiedCommon"))
    kr = _simple_ranges(_imgui_func_body(src, "Korean"))
    # Sanity floor: if any parse silently regresses (e.g. an ImGui refactor
    # renames the arrays), fail loudly rather than ship a near-empty subset.
    if len(jp) < 3000 or len(sc) < 3000 or len(kr) < 10000:
        sys.exit("subset_cjk_fonts: parsed ImGui ranges look wrong "
                 f"(JP={len(jp)} SC={len(sc)} KR={len(kr)}); refusing to subset")
    return jp, sc, kr


# srcname -> which range set to keep. Filled in main() once ranges are known.
def _plan(jp, sc, kr):
    union = jp | sc | kr
    return {
        "NotoSansCJKjp-Regular.otf": jp,
        "NotoSansCJKkr-Regular.otf": kr,
        "NotoSansCJKsc-Regular.otf": sc,
        "NotoSansCJKtc-Regular.otf": sc,   # zh-TW renders via the SC range
        "SarasaMonoSlabJ-Regular.ttf": union,
        "SarasaMonoSlabK-Regular.ttf": union,
        "SarasaMonoSlabSC-Regular.ttf": union,
        "SarasaMonoSlabTC-Regular.ttf": union,
    }


def subset_one(src_path, dst_path, unicodes):
    from fontTools import subset
    opts = subset.Options()
    opts.hinting = False
    opts.desubroutinize = True
    opts.drop_tables += ["DSIG"]
    opts.recalc_bounds = True
    font = subset.load_font(src_path, opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)
    subset.save_font(font, dst_path, opts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--imgui-draw", required=True,
                    help="path to the FetchContent imgui_draw.cpp")
    ap.add_argument("--src-fonts", required=True,
                    help="directory holding the original data/fonts files")
    ap.add_argument("--out", required=True,
                    help="output directory for the subset CJK fonts")
    args = ap.parse_args()

    try:
        import fontTools  # noqa: F401
    except ImportError:
        sys.exit("subset_cjk_fonts: fonttools not importable — "
                 "`pip install fonttools brotli`")

    jp, sc, kr = parse_imgui_ranges(args.imgui_draw)
    plan = _plan(jp, sc, kr)

    os.makedirs(args.out, exist_ok=True)
    total_in = total_out = 0
    for name, unicodes in plan.items():
        src = os.path.join(args.src_fonts, name)
        dst = os.path.join(args.out, name)
        if not os.path.exists(src):
            sys.exit(f"subset_cjk_fonts: missing source font {src}")
        subset_one(src, dst, unicodes)
        si, so = os.path.getsize(src), os.path.getsize(dst)
        total_in += si
        total_out += so
        print(f"  {name:32s} {si // 1024:7d}K -> {so // 1024:6d}K  "
              f"({len(unicodes)} glyphs)")
    print(f"  {'TOTAL':32s} {total_in // (1024*1024):7d}M -> "
          f"{total_out // (1024*1024):6d}M")


if __name__ == "__main__":
    main()
