# Outputs one standalone SVG per layer in tiles.svg.
#
# Kept for fun / archival, but in practice WinBolo's SVG path uses
# nanosvg, which only handles solid fills, linear gradients, and
# radial gradients. It silently drops <pattern>, <filter>, <mask>,
# <clipPath>, <use href> across files, CSS, and most SVG2 features.
# Anything Inkscape exports that uses those will render as
# transparent / wrong in-game.
#
# Use generate_pngs.ps1 instead — Inkscape rasterizes the layers at
# the requested sizes (e.g. 16, 32, 48, 64, ..., 160 px) with full
# fidelity, and WinBolo's atlas builder picks the right size by the
# <name>_<size>.png suffix convention.
#
# Note: even when SVGs are shipped, WinBolo just rasterizes them to
# RGBA bitmaps at load time — the same thing Inkscape does to make
# the PNGs — so PNG sources end up functionally equivalent (and look
# better since Inkscape's renderer is far more capable than nanosvg).
& "$PSScriptRoot\generate_pngs.ps1" -Sizes svg
