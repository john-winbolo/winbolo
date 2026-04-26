# Detects Python and runs fix_svg_patterns.py.
# The actual fix logic lives in the .py file --- portable across OSes.

param(
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$Rest
)

function Show-PythonNotFoundWarning {
  Write-Host ""
  Write-Host "============================================================" -ForegroundColor Red
  Write-Host " Python not found" -ForegroundColor Red
  Write-Host "============================================================" -ForegroundColor Red
  Write-Host ""
  Write-Host "This script needs Python 3 to run. Install it:" -ForegroundColor Cyan
  Write-Host ""
  Write-Host "  Easy: type 'python' in any terminal --- Windows will offer"
  Write-Host "        to install it from the Microsoft Store (works fine"
  Write-Host "        for scripts, unlike the Inkscape Store version)."
  Write-Host ""
  Write-Host "  Or:   download from https://www.python.org/downloads/" -ForegroundColor Cyan
  Write-Host "        (keep 'Add Python to PATH' checked during install)"
  Write-Host ""
  Write-Host "After installing, open a new PowerShell window and re-run."
  Write-Host ""
}

function Find-Python {
  foreach ($candidate in @("python3", "python", "py")) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if (-not $cmd) { continue }

    try {
      $version = & $candidate --version 2>&1
      if ($version -match "Python 3") { return $candidate }
    } catch { }
  }
  return $null
}

$python = Find-Python
if (-not $python) {
  Show-PythonNotFoundWarning
  exit 1
}

$script = Join-Path $PSScriptRoot "fix_svg_patterns.py"
if (-not (Test-Path $script)) {
  Write-Error "fix_svg_patterns.py not found next to this script."
  exit 1
}

# Run Python with cwd set to the script's directory so file globs resolve
# next to the script regardless of where this was invoked from.
Push-Location $PSScriptRoot
try {
  & $python $script @Rest
  exit $LASTEXITCODE
} finally {
  Pop-Location
}