# LuaJIT port — feasibility spike (branch `lua-jit`)

Status of an exploratory attempt to run the bot brains on **LuaJIT** (a Lua
**5.1** VM with a tracing JIT) instead of the current **PUC-Lua 5.4.7**
interpreter, to see whether brain CPU time drops.

This is a SPIKE. Read this before assuming any of it is production-ready.

## TL;DR

- The brains are written for Lua 5.4 and use syntax LuaJIT (5.1) cannot parse:
  floor division `//` and the bitwise operators `<< >> & | ~` (incl. unary `~`).
- `tools/lua54to51.py` down-transpiles those to 5.1 (`bit.*` + a `__idiv`
  helper). **Done and validated**: 49/50 brain files rewrite and pass `luac`
  (802 operators across the tree); the one exception, `los_stamp_cache.lua`,
  fails `luac -p` in the *original* too (a giant generated data table — the
  transpiler makes 0 edits to it), so it's pre-existing, not a transpiler bug.
- A small C-API shim is still needed (`lua_getextraspace`, `lua_isinteger`).
- LuaJIT itself still has to be built (MSVC) and wired in behind a CMake option.
- **Hard blockers remain** (below). This is not a drop-in.

## What the brains actually use (measured, GoalHunter_1.5)

| 5.3/5.4 feature | LuaJIT 5.1 status | brains use it? | handling |
|---|---|---|---|
| `//` floor division | not parseable | yes (~24) | → `__idiv(a,b)=math.floor(a/b)` |
| `<< >> & \| ~` bitwise | not parseable | yes (~hundreds) | → LuaJIT `bit.*` |
| unary `~x` (bnot) | not parseable | yes (e.g. `keys & ~KEY_FASTER`) | → `bit.bnot(x)` |
| `<const>` / `<close>` | not parseable | **no** | tool errors if seen |
| `goto`/labels | OK (LuaJIT has it) | yes | unchanged |

## C-API gap (in the brain glue)

Used by `luabrainshandler.c` / `bot_manager.c`, absent in LuaJIT 5.1:

- `lua_getextraspace` (5.4) — stores the per-state `BotContext*` (4 call sites).
  LuaJIT has no extraspace; shim must provide a stable per-`lua_State` void*
  slot (e.g. a registry-pinned userdata box).
- `lua_isinteger` (5.3) — shim via "is a number with no fractional part".

## Hard blockers / risks (why this isn't a slam dunk)

1. **WASM target.** `src/wasm` builds and runs brains (`lua_static`,
   `bot_brains_static`, preloaded brain files). **LuaJIT has no WebAssembly
   backend.** The web build must keep PUC-Lua → you'd maintain two VMs and
   brains valid on both. LuaJIT can only ever be an *opt-in for native targets*.
2. **Determinism.** `brainrec.btr` replays and `tests/baseline/expected/*.json`
   assume exact numeric results. LuaJIT's number model differs from 5.4
   (no integer subtype; `bit.*` coerces through int32; different FP
   intermediates). The transpile reproduces floor-division *values*, but
   bit-exact equality with PUC-5.4 across the brain is **unverified** and may
   diverge replays/baselines. Must be re-validated before trusting it.
3. **iOS.** JIT is disabled (no W^X entitlement) → interpreter-only there, so
   the compatibility cost buys nothing on that platform.
4. **Sandbox / `ffi`.** LuaJIT ships `ffi` (arbitrary C / dlopen) — a full
   sandbox escape. `brain_apply_sandbox` must nil `ffi` (and `jit.*` as
   desired) for untrusted brains.
5. **Upside is partial.** The hot paths (pathfinder, world-sim) are already in
   C (`cpf_*`, `wsim_*`). LuaJIT mainly accelerates hot *Lua*; much of that is
   already out of Lua, so measure before committing (see Benchmark plan).

## Components

- `tools/lua54to51.py` — the down-transpiler. **Done, tested.**
  - `--report` (list rewrites), `--in-place`, `--out DIR`.
  - Real lexer (skips strings / `--` and `--[[ ]]` comments, so `http://` and
    string contents are never touched).
  - Precedence-correct: unary `~` → `//` → `>> <<` → `&` → binary `~` → `|`,
    each pass turning its operator into a call so the next pass sees a primary.
  - Fails loudly (`LexError`) on any operand shape it can't prove — never emits
    a silently-wrong rewrite.

## Remaining work (not done in this spike)

- [ ] `src/.../luajit_compat.h` — C-API shim (extraspace box, `lua_isinteger`).
- [ ] CMake `WINBOLO_LUAJIT` option: fetch+build LuaJIT (MSVC `msvcbuild.bat`),
      swap `lua_static`→LuaJIT, force-include the shim, run the transpile over
      the staged brains, disable `ffi` in the sandbox.
- [ ] Build a LuaJIT-backed `WinBoloDS`/`BrainTest`.
- [ ] Re-validate determinism against `brainrec.btr` + baselines.
- [ ] Benchmark vs `main` (below).

## Benchmark plan (vs `main`)

Same scenario on both VMs, compare brain think-time:

```
WinBoloDS -map data\maps\Crankcase.map -gametype tournament \
  -brain brains\GoalHunter_1.5\init.lua -ai yesfull -bots 6 -allybots \
  -threads 1 -quitonwin -brain-profile-log
```

- `-threads 1` to remove pool scheduling noise; compare per-tick brain time
  from `performance.ticks.log` (median + p95) and total wall time.
- main = PUC-5.4 build; branch = LuaJIT build (transpiled brains).
- Report think-time delta AND confirm identical game outcome (determinism gate).
