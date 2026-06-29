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
- LuaJIT 2.1 **builds cleanly** (MSVC `msvcbuild.bat`); the transpiled brains
  parse under the real LuaJIT (49/50, same pre-existing exception). Proven, not
  theoretical.
- A representative pure-Lua kernel runs **~5.4× faster** on LuaJIT (identical
  checksum) — but that's an UPPER BOUND; the in-game gain is lower because the
  hot paths are already in C (see Benchmark results).
- Still NOT done: the C-API shim, wiring LuaJIT into the CMake build, and
  re-validating determinism. **Hard blockers remain** (below) — not a drop-in.

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

## Benchmark results (pure-Lua kernel)

A representative self-contained kernel (`tools/luajit_bench.lua`: binary-heap
Dijkstra over a packed grid, exercising the exact `// >> << & ~` ops the
transpiler handles) run under both VMs, both built `/O2`:

| VM | 60k iters (best of 3) | checksum |
|---|---|---|
| PUC-Lua 5.4.7 (`lua54.exe`) | **55.22 s** | 36000000 |
| LuaJIT 2.1 (`luajit.exe`, transpiled) | **10.27 s** | 36000000 |

**≈ 5.4× faster**, identical checksums (correctness gate passed — the
transpiled bit/floor-div code computes the same result on LuaJIT).

How it was produced (both interpreters built from the repo's pinned sources):
- LuaJIT: `git clone --depth 1 LuaJIT/LuaJIT`, then `msvcbuild.bat` in a VS x64
  dev shell → `luajit.exe` + `lua51.lib` + `lua51.dll`.
- PUC 5.4: `cl /O2` over `build/_deps/lua-src/lua-5.4.7/src/*.c`
  (`-I include -I src`, drop `luac.obj`) → `lua54.exe`.
- `python tools/lua54to51.py --out <dir> tools/luajit_bench.lua` for the LuaJIT
  side; run `lua54 luajit_bench.lua 60000` vs `luajit bench51.lua 60000`.

### Big caveat — this is an UPPER BOUND, not the in-game number

This kernel is 100% Lua. The real brain's heaviest work (pathfinder, world-sim)
is already in **C** (`cpf_*` / `wsim_*`), so only a fraction of a real frame is
spent in Lua. The end-to-end in-game speedup will be **materially lower** than
5.4× — by how much depends on the Lua/C time split, which needs the full VM
integration + `-brain-profile-log` to measure. Treat 5.4× as "what LuaJIT does
to the Lua portion," not "what the game gets."

## Benchmark plan for the real workload (vs `main`)

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
