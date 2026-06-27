# Blitz state machine (squad-coordinated pill take)

A **blitz** is a synchronized squad pill take: a commander leading an
`attack_pill` take on a hostile pill opens a *call*; nearby soldiers negotiate an
engage spot, commit, set up together, and on a single shared **GO** the squad
fires/charges at once so the pill's one-target-per-reload fire is swamped.

This doc reflects the CURRENT implementation (negotiation + open-once call). The
old `blitz_setup` / `blitz_ready` substates are gone — see history below.

## Roles

Roles are **emergent** (R0 dynamic commanders): a bot is a **commander** only
while it leads a HARD pill take (`pill.health >= HARD_TAKE_MIN_HP`, 12); on a
soft pill or while following someone it's a **soldier**. So a soft pill is never
blitzed — it's a normal solo take.

## The open-once call protocol (NOT broadcast every tick)

The call is broadcast **once on open and once on close** over the tiny 128-byte
`/info` bus — it is NOT re-sent every tick (that was noise). Receivers hold it in
a registry (`state.blitz_calls`, keyed by commander) until a close arrives or it
goes stale.

- **`bco <pill>`** — *call open*, sent the tick the commander starts leading a
  blitz (`cur_call` becomes set: `attack_pill` + `goal._blitz` + a pre-commit
  substate). Sent once; re-announced only on a discovery query (`bcq`) from a
  freshly spawned/joining bot.
- The call **stays open** across the pre-commit substates
  (`BLITZ_CALL_OPEN_SUB` = `plan_position`, `approach`, `gather_trees`,
  `detree`, `build_walls`, `blitz_wait`) on the SAME pill — it does NOT close
  just because `goal._blitz` momentarily flickers (e.g. DYNAMIC_COMMANDERS
  demoting the commander as the pill HP wobbles near the threshold). The
  `_my_blitz_call` latch keeps it open through a flicker.
- **`bcc`** — *call closed*, sent once when the substate leaves the pre-commit
  set (committed to firing: `aim`/`charge`/`in_range_*`/`engage`/`shoot_pill`/
  `swerve`), the target changes, the bot becomes a soldier, or the goal ends.
- Receivers also **prune** a stale call (commander inactive at ally-expiry, or
  its latest broadcast no longer shows that `attack_pill`) — covers a silent
  disconnect that never sent `bcc`.

### Negotiation wire keys (on the `/info state` slate)

- **`bes`** — offered/claimed standoff, **`"fx,fy"` 4-decimal float tile coords**
  (sub-tile precision for de-confliction). Broadcast both while negotiating
  (`squad_negotiate_cmdr` set) and once committed (`goal._blitz`).
- **`bd`** — reported walk distance to the offered standoff (commander tie-breaks
  conflicts by it: the FURTHER tank keeps a contested spot).
- **`brj`** — commander's reject list, spot-tagged **`"pn:[fx,fy]"`** (`;`-sep).
  A soldier repicks only when the reject names it AND matches the spot it's
  CURRENTLY offering — stale rejects are ignored, so `SQUAD_BLITZ_REPICK_GAP`
  can be 1 (repick every tick) with no candidate-list burn.
- **`bac`** — commander's accept list (player numbers): answered + conflict-free
  + shot-to-pill-center not wall-blocked.
- **`rdy`** — `"1"` once a committed soldier is IN POSITION at its setup point.
- **`bgo`** — the commander's **GO**, latched while it's committed/firing so a
  soldier that checks a tick late still sees it.

## State flow

### Soldier
1. **negotiate** (stays on its OWN goal): picks the nearest in-range open call,
   offers a standoff via `pick_standoff` (`bes`+`bd`), repicks on `brj`.
2. **commit** when the commander accepts (`bac` names it): adopts `attack_pill`
   on the blitz pill (`squad_blitz_accepted` latches; `goal._blitz` set).
3. **approach** — drives to its **SETUP point** (standoff radius +
   `ATTACK_APPROACH_OFFSET`, i.e. OUT of pill range), not the standoff itself.
4. **blitz_wait** — IN POSITION (`rdy=1`), holds for `bgo`.
5. **GO** → `commit_fire`: a soldier is non-PPT → **charge** → `engage`/
   `shoot_pill` → `swerve` (only on damage).

### Commander
1. **plan_position → approach** — normal PPT planning (shield scan), drives to
   its setup point.
2. **in-position decision:**
   - a committed joiner already present (`blitz_ready_status > 0`) OR a non-PPT
     (soft/wounded) take → **skip walls** → `blitz_wait`. (`_blitz_shielded`
     stays nil → on GO everyone, commander included, **charges** the overwhelm.)
   - full-pill PPT, no joiner yet → **build_walls** first (call stays JOINABLE
     through the build); completion → `blitz_wait` with `_blitz_shielded=true`.
3. **blitz_wait** — quorum: GO when all committed soldiers `rdy`, OR
   `SQUAD_BLITZ_READY_TIMEOUT` (~3 s), OR `total==0` (solo → GO instantly).
4. **GO** → `commit_fire`: `_blitz_shielded` → **aim → in_range** (fire from
   BEHIND the shield, never charge into it); else → **charge**.

`blitz_ready_status` counts only soldiers **committed to our pill** (broadcasting
`attack_pill` on it = roster Status "y") — a merely-negotiating ("m") soldier
does NOT trigger `blitz_wait` or count toward the GO quorum.

## Degrade / robustness (everything times out or aborts on its own signal)

- **No joiner by GO** — commander GOes solo: shielded → PPT fire, else charge.
  If it had SKIPPED walls for a joiner that then vanished (full pill, not
  shielded), it degrades to a solo PROTECTED take (re-approach → build walls).
- **Commander dies / retargets** — soldier's commitment clears (squad layer +
  the top-of-`update_attack_substate` abort, gated to pre-commit substates) →
  `clear_attack_goal`. `blitz_wait` also self-aborts on commander dead / off our
  pill, with a `SQUAD_BLITZ_WAIT_TIMEOUT` (~30 s) backstop for silent disconnect.
- **Soldier dies / disconnects** — excluded from the quorum (`tank_dead_at` →
  fast; ally-expiry → ≤35 s); the commander's ~3 s ready-timeout GOes anyway.
- **Pill dies** (anyone, mid-`blitz_wait`) — `clear_attack_goal`.
- **Committed (firing) takes are NOT aborted** by the blitz ending — once past a
  pre-commit substate the bot finishes the kill.

## ally_claimed interaction — "join it or leave it"

An ally's `attack_pill` has exactly two outcomes for another bot, never a third
solo-alongside:
- **A blitz you're part of** (`squad_blitz_target == pill` — committed soldier or
  commander) → exempt from `ally_claimed`, take it AS the blitz.
- **Otherwise** → normal `ally_claimed`: you won't SOLO it, and you can't
  cost-STEAL a joinable blitz either (forced yield) — you join via the squad
  layer, or you're locked out if you can't join.

## Tuning constants

`HARD_TAKE_MIN_HP` 12 · `SQUAD_BLITZ_READY_TIMEOUT` 150 (~3 s) ·
`SQUAD_BLITZ_WAIT_TIMEOUT` 1500 (~30 s) · `SQUAD_BLITZ_REPICK_GAP` 1 ·
`SQUAD_BLITZ_CLASH_TILES` 1 (euclidean float) · `SQUAD_HELP_RANGE` 30.

## Visualizers

- `squad_blitz` (followed bot): own engage spot (magenta → green IN POSITION →
  cyan committed) + setup point + line to pill; state label; squadmates' `bes`.
- `blitz_roster` (commander): per answering soldier — `tank | Status (y/m) |
  Standoff (fx,fy) | Dist | Ans (ACC/REJ)`; "Waiting…" / "Not blitz commanding".
- `blitz_call` (self/soldier): our answer, standoff, walk dist, repos.
- `blitz_joinable` (`JOIN BLITZ` borders): pills with an open in-range call.

## History

- **Removed `blitz_setup`** — a soldier now commits the negotiated standoff
  straight to `approach` (no separate claim substate).
- **`blitz_ready` → `blitz_wait`** (rename).
- **Open-once call** replaced the earlier per-tick `goal._blitz`-keyed broadcast
  that closed prematurely on role flicker.
