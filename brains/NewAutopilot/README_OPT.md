# Building the stripped `opt/` brain

`opt/` holds the production version of the brain — the same Lua sources
with debug-only calls (`viz.*`, `overlay_*`, `print2`, and
`if BRAIN_DEBUG_MODE then ... end` blocks) removed. WinBolo loads `opt/`
by default; BrainTest loads either the unstripped originals (default)
or `opt/` when launched with `--opt`.

You must regenerate `opt/` whenever you edit a `.lua` file in this
directory — the stripper is **not** wired into the C build.

## One-shot

From `brains/NewAutopilot/`:

```
strip.bat
```

This runs `lua_strip.exe` over every `.lua` in this directory and writes
cleaned copies into `opt/`. Each output is syntax-checked via
`luaL_loadfile`; a malformed strip fails the script with a non-zero exit
code, so a clean run is also a syntax-clean run.

`strip.bat` defaults the stripper path to
`..\..\build\Release\lua_strip.exe`. Pass an explicit path if your build
layout differs:

```
strip.bat D:\path\to\lua_strip.exe
```

## Building lua_strip itself

`lua_strip` is a CMake target. Build it once (Release):

```
"D:\Development\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  D:\Development\winbolo\build\lua_strip.vcxproj `
  /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
```

Source: `src/tools/lua_strip.c`. It accepts `--strip <prefix>` for
call-statement stripping (line whose first token starts with the prefix
is dropped, paren-balanced across multi-line calls) and
`--strip-block <prefix>` for Lua block stripping (matches a block-opening
line and removes through the matching `end`/`until`, tracking nested
`then/do/function/repeat`).

## What `strip.bat` strips

```
--strip print2
--strip "viz."
--strip overlay_
--strip-block "if BRAIN_DEBUG_MODE then"
```

Adding new patterns: edit `strip.bat`. Match the spirit of the existing
ones — debug/log/visualization calls that have no behavioral effect.

## Authoring rules for `if BRAIN_DEBUG_MODE then` blocks

Wrap blocks in `if BRAIN_DEBUG_MODE then ... end` ONLY when the block is
panel/log/diagnostic data and has no effect on bot behavior. Anything
the bot reads to decide what to do must NOT be inside a stripped block.

Authoring checklist:

1. The block opens with exactly `if BRAIN_DEBUG_MODE then` (the
   stripper matches that prefix). Don't fold an existing condition in
   front of it.
2. Initialize any field the non-debug path reads OUTSIDE the wrap. For
   example, `local cands = {}` then `if BRAIN_DEBUG_MODE then ...
   populate cands ... end` — readers see an empty table in `opt/`, not
   `nil`.
3. Don't move side-effecting code (cost adjustments, goal commits,
   pathfinder calls) inside. The stripper deletes the whole block; in
   `opt/` it's gone.
4. After editing, run `strip.bat` and confirm it reports a non-zero
   stripped-line count for the file you touched and exits 0.

## After editing brain Lua

1. Edit the `.lua` source under `brains/NewAutopilot/`.
2. Run `strip.bat`.
3. Copy the changed file(s) to `build/Brains/NewAutopilot/` AND the
   matching `build/Brains/NewAutopilot/opt/` — the C build does not
   auto-copy.
4. Re-run BrainTest / WinBolo.

## Verifying `opt/` runs without your debug data

Quickest check: launch BrainTest with `--opt`. That loads `opt/` and
sets `BRAIN_DEBUG_MODE = false` for the run, so any panel-only data your
wrap was supposed to remove should now be missing (panels show empty,
HUD strings blank). If the bot misbehaves under `--opt` but works
without it, you wrapped something behavior-relevant.

The optimize.log (`build/optimize.log` or
`<DEBUG_SESSION_DIR>/optimize.log`) is only written under `--opt` —
`optimize.lua` no-ops itself when `BRAIN_DEBUG_MODE` is true.
