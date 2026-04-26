# Outputs one standalone SVG per layer in tiles.svg.
& "$PSScriptRoot\generate_pngs.ps1" -Sizes svg
& "$PSScriptRoot\fix_svg_patterns.ps1"
