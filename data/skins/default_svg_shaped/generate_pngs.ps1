# Renders tiles.svg layers to PNGs (or SVGs if -Sizes svg).
#
# We script this instead of using Inkscape's UI export because the GUI
# doesn't expose --export-png-antialias=0, which gives crisp pixel-art
# edges. Vector geometry rasterized with AA off produces the sharp,
# blocky look we want; the GUI always anti-aliases.
#
# Usage:
#   .\build.ps1                        # default sizes (PNG)
#   .\build.ps1 -Sizes 16,160          # specific PNG sizes
#   .\build.ps1 -Sizes svg             # standalone SVGs (full Inkscape SVG)
#   .\build.ps1 -Sizes svg -PlainSvg   # SVGs stripped of Inkscape metadata

param(
  [string[]]$Sizes = @("160"),
  [switch]$PlainSvg
)

# Override Inkscape path here if auto-detect fails. Otherwise leave $null.
$InkscapeOverride = $null

# ------------------------------------------------------------

function Show-StoreVersionWarning {
  Write-Host ""
  Write-Host "============================================================" -ForegroundColor Yellow
  Write-Host " Windows Store Inkscape detected --- it can't be scripted" -ForegroundColor Yellow
  Write-Host "============================================================" -ForegroundColor Yellow
  Write-Host ""
  Write-Host "The Store version is sandboxed and blocks script execution."
  Write-Host "Install the MSI instead:" -ForegroundColor Cyan
  Write-Host ""
  Write-Host "  1. Uninstall: Settings -> Apps -> Inkscape -> Uninstall"
  Write-Host "  2. Download MSI: https://inkscape.org/release/" -ForegroundColor Cyan
  Write-Host "  3. Install (keep 'Add to PATH' checked)"
  Write-Host "  4. Re-run this script in a new PowerShell window"
  Write-Host ""
}

function Show-NotFoundWarning {
  Write-Host ""
  Write-Host "============================================================" -ForegroundColor Red
  Write-Host " Inkscape not found" -ForegroundColor Red
  Write-Host "============================================================" -ForegroundColor Red
  Write-Host ""
  Write-Host "Install the MSI version (NOT the Microsoft Store version ---" -ForegroundColor Cyan
  Write-Host "the Store version is sandboxed and can't be scripted)."
  Write-Host ""
  Write-Host "  1. Download MSI: https://inkscape.org/release/" -ForegroundColor Cyan
  Write-Host "  2. Install (keep 'Add to PATH' checked)"
  Write-Host "  3. Re-run this script in a new PowerShell window"
  Write-Host ""
  Write-Host "Or set `$InkscapeOverride at the top of this script to point"
  Write-Host "directly at your inkscape.exe."
  Write-Host ""
}

function Find-Inkscape {
  $cmd = Get-Command inkscape.exe -ErrorAction SilentlyContinue
  if ($cmd) { return @{ Path = $cmd.Source; IsStore = $false } }

  $candidates = @(
    "$env:ProgramFiles\Inkscape\bin\inkscape.exe",
    "$env:ProgramFiles\Inkscape\inkscape.exe",
    "${env:ProgramFiles(x86)}\Inkscape\bin\inkscape.exe",
    "${env:ProgramFiles(x86)}\Inkscape\inkscape.exe",
    "$env:LOCALAPPDATA\Programs\Inkscape\bin\inkscape.exe",
    "$env:USERPROFILE\scoop\apps\inkscape\current\bin\inkscape.exe"
  )
  foreach ($p in $candidates) {
    if (Test-Path $p) { return @{ Path = $p; IsStore = $false } }
  }

  if (Get-AppxPackage -Name "*Inkscape*" -ErrorAction SilentlyContinue) {
    return @{ Path = $null; IsStore = $true }
  }

  return @{ Path = $null; IsStore = $false }
}

function Get-SafeFilename($name) {
  $name = $name -replace '\s+', '_'
  $name = $name -replace '[\\/:*?"<>|]', '_'
  return $name
}

# --- Resolve Inkscape ---

if ($InkscapeOverride) {
  $Inkscape = $InkscapeOverride
} else {
  $result = Find-Inkscape
  if ($result.IsStore) {
    Show-StoreVersionWarning
    exit 1
  }
  $Inkscape = $result.Path
}

if (-not $Inkscape) {
  Show-NotFoundWarning
  exit 1
}

Write-Host "Using Inkscape: $Inkscape" -ForegroundColor Cyan
Write-Host "Sizes: $($Sizes -join ', ')" -ForegroundColor Cyan

# Run from the script's directory so tiles.svg and the output files
# resolve relative to the script, not wherever the shell was sitting.
Push-Location $PSScriptRoot
try {

# --- Export ---

$content = Get-Content tiles.svg -Raw
$pattern = '<g\b([^>]*\binkscape:groupmode="layer"[^>]*)>'

[regex]::Matches($content, $pattern) | ForEach-Object {
  $attrs = $_.Groups[1].Value

  $idMatch = [regex]::Match($attrs, 'id="([^"]+)"')
  $labelMatch = [regex]::Match($attrs, 'inkscape:label="([^"]+)"')

  if (-not $idMatch.Success) { return }

  $id = $idMatch.Groups[1].Value
  $label = if ($labelMatch.Success) { $labelMatch.Groups[1].Value } else { $id }

  if ($label -eq "Image") { return }

  $safe = Get-SafeFilename $label

  foreach ($size in $Sizes) {
    if ($size -eq "svg") {
      $out = "${safe}_inkscape.svg"
      Write-Host "  $label -> $out"
      $arguments = @(
        "tiles.svg",
        "--export-id=$id",
        "--export-id-only",
        "--export-type=svg",
        "--export-filename=$out"
      )
      if ($PlainSvg) { $arguments += "--export-plain-svg" }
    } else {
      $out = "${safe}_${size}.png"
      Write-Host "  $label -> $out"
      $arguments = @(
        "tiles.svg",
        "--export-id=$id",
        "--export-id-only",
        "--export-type=png",
        "--export-width=$size",
        "--export-png-antialias=0",   # 0 = sharp pixel edges, 1-3 (best) = smoother/blended
        "--export-filename=$out"
      )
    }
    & $Inkscape @arguments
  }
}

} finally {
  Pop-Location
}