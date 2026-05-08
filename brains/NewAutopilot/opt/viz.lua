-- =========================================================================
-- NewAutopilot/viz.lua
--
-- Centralized overlay gating. Every "draw something on the BrainTest
-- screen" call goes through one of these wrappers and MUST pass a
-- viz_id naming the V-dialog checkbox that gates it. If the checkbox
-- is unchecked the wrapper drops the underlying overlay_* call. If
-- viz_id is missing or unknown it ERRORS — that's intentional, the
-- whole point is "every damn thing drawn as an overlay has a checkbox"
-- and the brain is the wrong place to silently swallow that mistake.
--
-- viz_id naming: snake_case, matches the underscore-cased version of
-- the V dialog row. The C side (braintest_main.c VIZ_TOGGLES) carries
-- the human-readable Short / Long strings + the default state and
-- pushes the on/off bools into _BT_VIZ_<ID-UPPERCASE> globals every
-- toggle change.
--
-- Adding a new viz id:
--   1. Add an entry to M.IDS below (short = name in V dialog, long =
--      details column).
--   2. Add a matching row to braintest_main.c VIZ_TOGGLES with the
--      same id, short, long, and an offsetof a new bool field.
--   3. Use viz.text / rect / line / circle / hud_text with the new
--      id everywhere you'd previously have called the raw overlay_*.
--
-- Default state convention: when the C side hasn't pushed yet (e.g.
-- right at brain startup) the relevant _BT_VIZ_<ID> global is nil.
-- We treat nil as "on" so brains run outside BrainTest still see all
-- their overlays without any setup.
-- =========================================================================

local M = {}

-- Registry. Add new entries here as new overlays are migrated.
-- The "short" / "long" fields are documentation only on the Lua
-- side — the actual V-dialog text comes from the C side. We keep
-- a copy here so the brain can self-document and so a stray
-- typo'd id at a call site is caught with a clear list of valid
-- ids in the error message.
M.IDS = {
  -- Existing (Phase 1).
  shot_tile_grid = { short = "Shot tile grid",
                     long  = "Per-shell 1/16 sub-grid lines" },
  lgm_tile_grid  = { short = "LGM tile grid",
                     long  = "Per-LGM 1/16 sub-grid lines" },
  steering_text  = { short = "Steering text",
                     long  = "PLOW + pf.next debug labels near tank" },
  shell_hitbox   = { short = "Shell hitbox dbg",
                     long  = "Cyan tile outline + d=N tip text per shell" },
  shell_hit_dot  = { short = "Shell hit dot",
                     long  = "Orange/red dot at each shell's exact wu" },
  lgm_hitbox     = { short = "LGM hitbox dbg",
                     long  = "LGM orange dot + wu/tile coord text" },
  cliff_safety   = { short = "Cliff safety",
                     long  = "Yellow look-ahead tiles + cliff brake label" },
  lgm_stranded   = { short = "LGM stranded dbg",
                     long  = "Per-factor red/green labels by the LGM" },
  tank_position  = { short = "Tank position",
                     long  = "Tank wu/gu/tile readout + standoff wu" },
  tank_angle     = { short = "Tank angle",
                     long  = "Tank heading in brads (256 = full circle): integer info.direction + float info.tank_angle when present, plus turn-ramp counter (firstLeft/firstRight) value the engine reports." },

  -- HUD text (corners).
  hud_manual_control = { short = "HUD: manual control",
                         long  = "Top-left/right MANUAL CONTROL banner + key list" },
  hud_resources      = { short = "HUD: resources",
                         long  = "Shells/Mines/Armour/Trees/Speed/Boat counters" },
  hud_tick_info      = { short = "HUD: tick info (think_ms / phase / goal)",
                         long  = "Top-left line under the big TICK box. Brain-side timing via os.clock; color codes the think_ms cell green<5/yellow<10/orange<20/red>=20." },
  hud_replan         = { short = "HUD: replan + phase",
                         long  = "Replan countdown + phase + reason" },
  hud_goal           = { short = "HUD: current goal",
                         long  = "Active goal kind / target / substate" },
  hud_goal_candidates = { short = "HUD: goal candidates",
                          long  = "Pool candidates + scores list" },
  hud_attack_status  = { short = "HUD: attack status",
                         long  = "'Attack: kind sub: substate' line" },
  hud_lgm_status     = { short = "HUD: LGM status",
                         long  = "LGM ETA / nearby / stranded text" },
  hud_lgm_blocked    = { short = "HUD: LGM blocked",
                         long  = "'LGM blocked: water ahead' label" },
  hud_enemy_lgm_dead = { short = "HUD: enemy LGM dead",
                         long  = "Countdown for enemy LGM respawn" },
  hud_base_killer    = { short = "HUD: base killer",
                         long  = "'BASE KILLER' mode label" },
  hud_stuck_counter  = { short = "HUD: stuck counter",
                         long  = "Stuck-detection countdown" },
  -- (hud_click_cost removed — panel is rendered C-side again so it can
  --  use the live Dijkstra slate / show "N/A" in playback.)
  hud_compass        = { short = "HUD: compass dirs",
                         long  = "Direction-text indicators" },
  hud_emergency_drop = { short = "HUD: emergency drop",
                         long  = "'EMERGENCY PILL DROP!' banner" },
  hud_version        = { short = "HUD: version",
                         long  = "Bottom-right NewAutopilot vN.N label" },

  -- Tank / aim.
  tank_hitbox       = { short = "Tank hitbox",
                        long  = "Yellow ±128 wu hitbox outline around own tank" },
  tank_aim_marker   = { short = "Tank aim marker",
                        long  = "Yellow crosshair + corners at the aim wu point" },

  -- Pill interaction.
  pill_range_circle = { short = "Pill range circles",
                        long  = "Red max-range + green standoff circles around target pill" },
  pill_id_label     = { short = "Pill/base IDs",
                        long  = "Numeric ID labels overlaid on every pill/base tile" },

  -- Pathfinder / nav.
  pf_destination    = { short = "Pathfinder destination",
                        long  = "Marker at the bot's current nav destination" },
  pf_path_lines     = { short = "A* path lines",
                        long  = "Polyline tracing the bot's current A* path" },

  -- Approach / charge / engage / detree status.
  approach_dist     = { short = "Approach status",
                        long  = "dist=N/T spd=N/T HUD next to the tank during approach / in_range_position" },
  charge_status     = { short = "Charge status",
                        long  = "CHARGE: <phase> top-left HUD line" },
  detree_progress   = { short = "Detree progress",
                        long  = "DETREE N/M (left=K) overlay above tank" },

  -- Pill take / shield system.
  shield_scan_candidates = { short = "Shield: candidates",
                             long  = "Per-candidate score boxes for the 8 ring positions + standoff" },
  shield_scan_blockers   = { short = "Shield: blockers",
                             long  = "Winner's per-aim blocker borders + colored fills" },
  shield_scan_trajectory = { short = "Shield: trajectories",
                             long  = "Green circles on outgoing tiles + red squares on return-fire path + colored aim line" },
  shield_scan_legend     = { short = "Shield: legend",
                             long  = "Color-key 'name:actual+potential' legend next to viz target" },
  wall_build_queue       = { short = "Wall build queue",
                             long  = "Orange outlines + 1..N order numbers on the wall build queue" },
  build_status           = { short = "Build/approach timers",
                             long  = "BUILD pending/countdown + APPROACH timeout countdown labels" },
  build_decision_banner  = { short = "Build decision banner",
                             long  = "Yellow banner above tank: BUILD_WALLS or skip reason" },
  wall_skip_reason       = { short = "Wall skip reason",
                             long  = "Red 'WALL_SKIP: ...' line when builder gates fail" },
  pill_take_target       = { short = "Pill take target",
                             long  = "Magenta crosshair + TARGET label at chosen corner" },

  -- Wall / base shield (legacy ws_* + base_shield).
  base_shield_viz   = { short = "Base shield viz",
                        long  = "ws_*/base_shield wall + line markers (legacy single-wall path)" },

  -- Inspect overlay (right-click on pill etc).
  inspect_pill      = { short = "Inspect pill",
                        long  = "Full pill-attack inspector (range circles, scan spots, score legend)" },

  -- Coverage / threat layers.
  coverage_grid     = { short = "Coverage grid",
                        long  = "_SHOW_COVERAGE per-tile coverage values" },
  pill_threat_overlay = { short = "Pill threat overlay",
                          long  = "Threat circles around hostile pills" },

  -- LGM markers (other than own LGM hitbox).
  lgm_destination   = { short = "LGM destination",
                        long  = "Marker at the LGM's current dispatch destination" },
  ally_lgm_marker   = { short = "Ally LGM marker",
                        long  = "Markers + labels for allied LGMs in field" },
  friendly_pill_shield = { short = "Friendly pill shield",
                           long  = "SHIELD-tag friendly pills used for shielding" },

  -- Cover sample / swerve.
  swerve_dir_choice = { short = "Swerve dir choice",
                        long  = "Sample lines + L/R cover scores for swerve direction pick" },
  bpc_cover_samples = { short = "BPC cover samples",
                        long  = "Blue tile outlines from line_walk during BPC cover sweep" },

  -- Misc tile/state markers.
  blocked_tiles     = { short = "Blocked tiles",
                        long  = "Red outlines on tiles in state.blocked" },
  bait_pill_marker  = { short = "Bait pill marker",
                        long  = "BAIT? markers on deepsea pills suspected as bait" },
  pill_reposition_marker = { short = "Pill reposition marker",
                             long  = "Orange outline around pills marked for repositioning" },
  wounded_pill_marker    = { short = "Wounded pill marker",
                             long  = "Marker on the wounded-pill carryover target" },
  pool6_self_dr          = { short = "self_dr per pool-6 pill",
                             long  = "Pool 6 (attack_pill) candidates labeled with their self-danger reduction value — the discount subtracted from the spot-path cost equal to that pill's own danger contribution × (1 - hp/15). Larger values = more committed to closing in despite the pill's own anger." },

  -- Shot tracker.
  shot_tracker_viz  = { short = "Shot tracker",
                        long  = "Per-shell tracking overlays: spawn line, hit/miss markers, dead-shell circles" },

  -- Wsim trajectory rejects (orange/red death paths from goal scoring).
  wsim_paths        = { short = "Wsim death paths",
                        long  = "Orange/red lines + hit markers for rejected attack trajectories" },

  -- Tank combat detection.
  tank_combat_viz   = { short = "Tank combat detection",
                        long  = "Navy detection/engage range circles, gate HUD, per-enemy lines/labels" },

  -- Tank combat standoff scan (attack_tank substate).
  tank_combat_standoff_scan = { short = "Tank combat standoff scan",
                                long  = "Per-spot score boxes, maneuver tiles, chosen standoff circle/ellipse for attack_tank" },

  -- Steering lookahead marker (magenta circle + line to lookahead).
  nav_lookahead_marker = { short = "Nav lookahead marker",
                           long  = "Magenta lookahead target circle + line from tank to lookahead" },

  -- Wall-shoot precondition labels above the next pf step.
  wall_shoot_precond = { short = "Wall-shoot precond",
                         long  = "OK/X precondition labels on the next A* wall tile (dist, shells, allow, nav)" },

  -- Facing-away brake ring + correction label.
  facing_away_brake  = { short = "Facing-away brake",
                         long  = "Yellow ring around tank + correction angle label when facing-away brake fires" },

  -- Misc init.lua extras.
  adjacent_tiles        = { short = "Adjacent tile boxes",
                            long  = "Cyan filled boxes on the four tiles adjacent to the tank" },
  attack_bullet_counter = { short = "Attack bullet counter",
                            long  = "Fired/needed shot counter centered on the target pill" },
  attack_base_marker    = { short = "Attack base marker",
                            long  = "Red crosshair on target base + orange line from tank during attack_base" },

  -- Forest tiles drawn on the shot path during attack planning (debug).
  forest_path_tiles  = { short = "Forest path tiles",
                         long  = "White outlines on forest tiles a candidate shot path crosses (tree-cost viz)" },

  -- HUD: kill attempt indicator (KILL ATTEMPT / DAMAGE ONLY + counters).
  hud_kill_attempt   = { short = "HUD: kill attempt",
                         long  = "Top-left KILL ATTEMPT/DAMAGE ONLY label + bullets_needed/pill_hp/fired counters" },

  -- HUD: shoot_pill (PPT) live progress toward each of the three exit
  -- triggers (kill / swerve-after-N-hits / no-progress abort).
  hud_shoot_pill_progress = { short = "HUD: shoot_pill progress",
                              long  = "PPT shoot_pill exit-trigger bars: kill (pill_hp), swerve (hits taken), abort (ticks since last hp drop)" },

  -- Floating "<in-flight>/<pill HP>" label above the target pill.
  pill_shot_count    = { short = "Pill shot count",
                         long  = "Cyan '<in-flight shots>/<pill HP>' label floating above the target pill" },

  -- HUD: raw swerve debug counters during swerve substate.
  hud_swerve_debug   = { short = "HUD: swerve debug",
                         long  = "Top-left SWERVE label + raw _swerve_*ticks_left / _swerve_pill_dead values" },

  -- Attack scan spots: per-spot score boxes, maneuver tiles, ellipse,
  -- legend, and standoff/approach markers for pill take planning.
  attack_scan_spots  = { short = "Attack scan spots",
                         long  = "Pill-take scan spots: per-spot LOS+score boxes, maneuver tiles, ellipses, legend, standoff/approach markers" },

  -- When on, the pool-6 evaluator emits the full scan-spots overlay
  -- (LOS, score, ellipse, maneuver tiles) for EVERY hostile/neutral
  -- pill it considers — not just the one that won. Emission happens
  -- on the tick `evaluate_pill_difficulty` ran for that pill (~once
  -- per second per pill given the 50-tick diff cache), so the
  -- recording carries the data and you can scrub onto a replan tick
  -- in playback to see every pill's candidate spots.
  -- Default off — when on, pool-6 evaluation runs in detailed mode
  -- (~9× per-pill scan cost). Flip on when debugging a surprising
  -- target/spot pick.
  attack_scan_spots_all_pills = {
    short = "Attack scan spots: ALL pills (pool 6)",
    long  = "Per-pill candidate-spot overlay during pool 6 evaluation. Emits the scan-spot overlay (LOS, score, ellipse, maneuver tiles) for every hostile/neutral pill the goal selector considers, on the tick its eval ran. Diff cache TTL ~50 ticks so each pill emits once per ~1s. Heavier than the master attack_scan_spots toggle (forces detailed scan).",
    default_on = false,
  },

  -- Last attack-goal clear reason: HUD line that names the substate
  -- and the reason string passed to clear_attack_goal (or auto-derived
  -- caller file:line if nothing was passed). Stays on screen for ~300
  -- ticks after the clear, so you can scrub back from the moment goal
  -- went to "none" and see exactly what killed it.
  attack_clear_reason = {
    short = "Attack clear reason",
    long  = "Top-left HUD line showing the most recent clear_attack_goal call: tick, prior kind/substate, prior pill (mx,my), and the reason string. Visible for ~300 ticks after the clear." },

  -- Persistent solid-beige disc + pill target_id label centered on the
  -- chosen attack standoff. Drawn every tick the goal holds a standoff
  -- so it stays visible until the standoff changes or the goal ends —
  -- a low-noise marker for "this is the take we're committing to".
  attack_chosen_standoff_marker = {
    short = "Attack chosen standoff marker",
    long  = "Persistent beige disc with the target pill's id at the chosen attack standoff. Stays visible while the goal holds a standoff." },

  -- Pickup path: per-attack_pill candidate, the spot→pill A* path the
  -- planner uses to estimate the post-kill walk-on cost. Magenta polyline
  -- traced from the firing spot to the pill tile, computed by step_eval_queue.
  attack_pill_pickup_path = {
    short = "Attack pickup path",
    long  = "Magenta polyline tracing the active attack_pill goal's spot→pill A* path (the post-kill walk leg used to compute the pickup cost)." },

  -- Meta: when on, every overlay shape gets its viz_id labeled in tiny
  -- text at the bottom-right. Useful for "what overlay is THAT?" debugging.
  label_overlays     = { short = "Label overlays",
                         long  = "Print viz_id in tiny text at the bottom-right of every drawn shape" },
}

local function known_ids_csv()
  local t = {}
  for k in pairs(M.IDS) do t[#t + 1] = k end
  table.sort(t)
  return table.concat(t, ", ")
end

local function assert_id(viz_id)
  if viz_id == nil then
    error("viz: viz_id is required — every overlay call must name its " ..
          "V-dialog checkbox. Known ids: " .. known_ids_csv(), 3)
  end
  if not M.IDS[viz_id] then
    error("viz: unknown viz_id '" .. tostring(viz_id) .. "'. Known ids: " ..
          known_ids_csv() ..
          ". Add the new id to viz.lua M.IDS and braintest_main.c VIZ_TOGGLES.",
          3)
  end
end

-- Returns true if the viz is enabled.
--
-- Resolution order:
--   1. _BT_VIZ_SUPPRESS_ALL (X-key bare-screen) → off, except hud_resources.
--   2. Per-id global _BT_VIZ_<ID> (BrainTest pushes this on toggle) wins
--      when set to true OR false.
--   3. Otherwise honor IDS[viz_id].default_on. Toggles documented as
--      default_on=false stay off in non-BrainTest runs (release client,
--      headless server) where the C side never pushes a value. This
--      matters for expensive gates like attack_scan_spots_all_pills
--      that force detailed pool-6 evaluation when on.
--   4. Fallback to ON for unspecified IDs (preserves the historical
--      "any viz that forgot to declare default_on shows by default"
--      contract for the BrainTest live-toggle workflow).
--
-- Used by brain code to gate EXPENSIVE precompute (e.g. trace data
-- assembly) — the draw wrappers below no longer short-circuit on this
-- value. They emit the overlay command unconditionally (with the
-- viz_id tagged in) so the BrainTest renderer can filter at draw time
-- AND so toggling V checkboxes during playback updates the recorded
-- frame's visible overlays.
-- Per-tick cache: refresh() snapshots the host's _BT_VIZ_* globals into a
-- flat table so call-site `if viz.is_on("foo") then ... end` guards reduce
-- to a single table lookup. Skips the per-call string concat + :upper() +
-- assert_id work the slow path does. think() must call viz.refresh() at
-- the top of each tick to keep this in sync with V-dialog toggles.
M._on = {}
local _on = M._on  -- closure-local alias for the fast path

function M.refresh()
  local suppress_all = _G._BT_VIZ_SUPPRESS_ALL
  for id in pairs(M.IDS) do
    local g = _G["_BT_VIZ_" .. id:upper()]
    if suppress_all and id ~= "hud_resources" then
      _on[id] = false
    else
      _on[id] = (g ~= false)
    end
  end
end

function M.is_on(viz_id)
  -- Fast path: if refresh() has populated the cache, answer in O(1).
  local cached = _on[viz_id]
  if cached ~= nil then return cached end
  -- Cold path: cache not populated yet (first tick / non-BrainTest run).
  assert_id(viz_id)
  if _G._BT_VIZ_SUPPRESS_ALL and viz_id ~= "hud_resources" then
    return false
  end
  local g = _G["_BT_VIZ_" .. viz_id:upper()]
  if g ~= nil then return g end
  local entry = M.IDS[viz_id]
  if entry and entry.default_on == false then return false end
  return true
end

-- Register every entry in M.IDS with the host's V dialog. Called from
-- Brain.open(). The braintest_viz_register binding only exists when
-- the brain runs under BrainTest; under WinBolo client it's nil and
-- this function is a no-op (the brain still draws overlays, they're
-- just never displayed).
function M.register_all()
  if not braintest_viz_register then return end
  for id, entry in pairs(M.IDS) do
    -- The 5th arg (default_on) is omitted so the C binding defaults
    -- to ON. Brains that want a viz off-by-default can pass the
    -- entry through with a `default_on = false` field; we honor it.
    local def_on = entry.default_on
    if def_on == nil then def_on = true end
    braintest_viz_register(id,
                           entry.short or id,
                           entry.short or "",
                           entry.long or "",
                           def_on)
  end
end

-- viz_idx lookup: BrainTest pushes a _BT_VIZ_IDS = { id = idx, ... }
-- table every tick. We cache the idx on M.IDS[id].idx so the
-- per-call overhead is a single table lookup. Returns 255
-- ("OVERLAY_VIZ_IDX_NONE") when running outside BrainTest, in which
-- case the renderer treats the cmd as unfiltered (always visible).
--
-- Important: only cache REAL indices. If _BT_VIZ_IDS hasn't been
-- pushed yet (first few ticks of a fresh bot, or running outside
-- BrainTest), don't pin the cache to 255 — re-check next call so
-- viz cmds become filterable as soon as the table arrives. Without
-- this, a brain that emits before _BT_VIZ_IDS lands stamps all its
-- cmds as "no viz_id" forever, and toggling V or X never filters
-- them.
local function vid(viz_id)
  local entry = M.IDS[viz_id]
  if entry.idx then return entry.idx end
  local tbl = _G._BT_VIZ_IDS
  if tbl and tbl[viz_id] then
    entry.idx = tbl[viz_id]
    return entry.idx
  end
  return 255
end

-- Tiny label drawn at the bottom-right of a shape, tagged with the
-- label_overlays viz_idx so the BrainTest renderer can hide it when
-- the "Label overlays" checkbox is off. Skips itself (a label_overlays
-- shape would otherwise label its own label, recursively).
-- Coordinates are tile units; SIZE matches the smallest readable
-- overlay text size used elsewhere in the brain.
local LABEL_SIZE = 0.25
local function label(viz_id, x, y)
  if viz_id == "label_overlays" then return end
  if not overlay_text then return end
  -- Don't emit the label when the parent viz is off — otherwise the
  -- shape itself hides (filtered at render by its own viz_idx) but
  -- its label keeps showing because the label is tagged with
  -- label_overlays' idx, not the parent's. Trade-off: in playback,
  -- frames captured while parent was off won't suddenly show labels
  -- if you flip parent on. Acceptable for the common case.
  if not M.is_on(viz_id) then return end
end

-- Drawing wrappers. Each ALWAYS emits the underlying overlay_* call
-- (when available) and tags it with the viz_id's stable index, so
-- BrainTest can filter at draw time — including for recorded frames
-- during playback when V checkboxes change. Brain code that wants to
-- skip expensive precompute should still call M.is_on(viz_id) first.
function M.text(viz_id, ...)
  assert_id(viz_id)
  if not overlay_text then return end
  local idx = vid(viz_id)
  -- overlay_text args: x, y, text, anchor, r, g, b, a, scale, viz_idx
  local x, y, text, anchor, r, g, b, a, scale = ...
  local result = overlay_text(x, y, text, anchor, r, g, b, a, scale, idx)
  if type(x) == "number" and type(y) == "number" then
    label(viz_id, x, y + LABEL_SIZE * 1.1)
  end
  return result
end

function M.rect(viz_id, ...)
  assert_id(viz_id)
  if not overlay_rect then return end
  local idx = vid(viz_id)
  -- overlay_rect args: x1, y1, x2, y2, r, g, b, a, filled, subpixel, viz_idx
  local x1, y1, x2, y2, r, g, b, a, filled, subpixel = ...
  local result = overlay_rect(x1, y1, x2, y2, r, g, b, a, filled, subpixel, idx)
  if type(x2) == "number" and type(y2) == "number" then
    label(viz_id, math.max(x1, x2), math.max(y1, y2))
  end
  return result
end

function M.line(viz_id, ...)
  assert_id(viz_id)
  if not overlay_line then return end
  local idx = vid(viz_id)
  -- overlay_line args: x1, y1, x2, y2, r, g, b, a, viz_idx
  local x1, y1, x2, y2, r, g, b, a = ...
  local result = overlay_line(x1, y1, x2, y2, r, g, b, a, idx)
  if type(x2) == "number" and type(y2) == "number" then
    label(viz_id, math.max(x1, x2), math.max(y1, y2))
  end
  return result
end

function M.circle(viz_id, ...)
  assert_id(viz_id)
  if not overlay_circle then return end
  local idx = vid(viz_id)
  -- overlay_circle args: cx, cy, radius, r, g, b, a, viz_idx, subpixel
  local cx, cy, radius, r, g, b, a, subpixel = ...
  local result = overlay_circle(cx, cy, radius, r, g, b, a, idx, subpixel)
  if type(cx) == "number" and type(cy) == "number" and type(radius) == "number" then
    label(viz_id, cx + radius * 0.7071, cy + radius * 0.7071)
  end
  return result
end

function M.hud_text(viz_id, ...)
  assert_id(viz_id)
  if not overlay_hud_text then return end
  if not M.is_on(viz_id) then return end
  local idx = vid(viz_id)
  -- overlay_hud_text args: x, y, text, anchor, r, g, b, a, viz_idx
  local x, y, text, anchor, r, g, b, a = ...
  return overlay_hud_text(x, y, text, anchor, r, g, b, a, idx)
end

-- =========================================================================
-- viz_detail registry — per-tick clickable map regions with rich body
-- text shown in BrainTest's "D" inspector dialog.
--
-- Lifecycle: brain calls M.detail_clear() at the top of think(), then
-- per-primitive calls M.detail(detail_id, kind, ...geometry..., label)
-- and zero-or-more M.detail_text(detail_id, line) to attach body lines.
-- The registry is shared across all bots; the dialog filters/sorts
-- by id. Each id is unique per primitive (brain decides naming).
--
-- All overlay_detail_* bindings are NULL-safe so brains running outside
-- BrainTest skip the work transparently.
-- =========================================================================

function M.detail_clear()
  -- DEPRECATED. The host (BrainTest) clears the viz_detail registry
  -- ONCE per tick before any brain.think runs (see appTickBrain in
  -- braintest_main.c). Brains that called this themselves were
  -- wiping each other's entries because the registry is global —
  -- last bot won, others lost. Kept as a no-op so existing brain
  -- code that calls viz.detail_clear() still works without error.
end

--- Register a clickable rect spanning (x1,y1)-(x2,y2) in tile coords.
--- detail_id must be unique per-primitive within a tick.
function M.detail_rect(detail_id, x1, y1, x2, y2, label)
  if not overlay_detail then return end
  return overlay_detail(detail_id, "rect", x1, y1, x2, y2, label or "")
end

--- Register a clickable circle centered at (cx, cy) with given radius.
function M.detail_circle(detail_id, cx, cy, radius, label)
  if not overlay_detail then return end
  return overlay_detail(detail_id, "circle", cx, cy, radius, 0, label or "")
end

--- Register a clickable text anchor at (x, y).
function M.detail_text_anchor(detail_id, x, y, label)
  if not overlay_detail then return end
  return overlay_detail(detail_id, "text", x, y, 0, 0, label or "")
end

--- Append a body line to the entry for detail_id. Body lines are
--- shown in a read-only multiline text field so the user can copy.
function M.detail_text(detail_id, line)
  if not overlay_detail_text then return end
  return overlay_detail_text(detail_id, line)
end

--- Convenience: register + multiple body lines in one call.
--- usage: M.detail("id", "rect", {x1,y1,x2,y2}, "label", {"line1","line2"})
function M.detail(detail_id, kind, geometry, label, body_lines)
  if not overlay_detail then return end
  local x1, y1, x2, y2 = 0, 0, 0, 0
  if geometry then
    x1 = geometry[1] or 0
    y1 = geometry[2] or 0
    x2 = geometry[3] or 0
    y2 = geometry[4] or 0
  end
  if body_lines and overlay_detail_text then
    for i = 1, #body_lines do
    end
  end
end

return M
