-- =========================================================================
-- GoalHunter/viz.lua
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
  -- Ammo-deprivation indicator: marker over the tank when ammo_deprived (or a
  -- countdown while the low-ammo clock runs toward it).
  ammo_deprived  = { short = "Ammo deprived",
                     long  = "Red marker + HUD when ammo_deprived; low-ammo countdown otherwise",
                     default_on = false },
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
  -- Heavy per-tick disk I/O for shell hitbox + intersect diagnostics.
  -- Default off: the file writes are expensive (one io.open/close per
  -- visible shell per tick).  Enable when actively debugging hitbox
  -- collisions.  Gates writes to hitboxes.log and intersect.log; the
  -- visual overlays (shell_hitbox / shell_hit_dot) stay separately
  -- togglable.
  hitbox_logs    = { short = "Hitbox/intersect log files",
                     long  = "Write hitboxes.log + intersect.log every tick (heavy disk I/O). Off by default; flip on only when debugging shell-hit-detection issues.",
                     default_on = false },

  blitz_comm_lines = { short = "Blitz comm lines",
                       long  = "Negotiation comm lines between a blitzing commander and tanks. Commander view: YELLOW out to in-range tanks while the call is live (no answer yet); ORANGE to a tank that answered but isn't locked; GREEN to accepted soldiers (going to position); RED to a tank that declined (No). Soldier view: a line to our commander — yellow answering, orange once we have a standoff offer, green once accepted, and RED for a decline (busy / no standoff spot), latched ~BLITZ_COMM_LATCH_TICKS so even a one-tick reject is visible." },

  blitz_joinable = { short = "HUD: joinable blitzes",
                     long  = "Right-side HUD list of every ongoing blitz call this bot knows about (the blitz-call registry) with per-call join status: JOINABLE (in help range, free slot, current goal interruptible), far, FULL, busy(reason), or pill gone. Columns: cmdr/pill/dist/slots; '>'=committed, '~'=negotiating. Also rings each known pill on the map (green=joinable, grey=known)." },

  squad_roster = { short = "HUD: squad roster",
                   long  = "Right-side per-bot squad roster (role + status, \"-- SQUADS --\"). Drawn via hud_text so it's draggable/labelable like other HUD overlays.",
                   default_on = true },

  label_hud_overlays = { short = "Label HUD overlays",
                         long  = "Prefix every HUD text overlay with its viz id (e.g. \"[circles_hud] ...\") so you can tell which toggle drives each on-screen text block. Affects all viz.hud_text overlays; off by default.",
                         default_on = false },

  hud_budget = { short = "HUD: budget + kill",
                 long  = "Prominent top-of-screen banner showing last_ms / target_ms ratio (green/yellow/orange/red) plus a flashing red KILLED banner when brain.wasKilled was set last tick.  Reads _G.brain.{lastThinkMs,targetMs,wasKilled}." },
  cliff_safety   = { short = "Cliff safety",
                     long  = "Yellow look-ahead tiles + cliff brake label" },
  lgm_stranded   = { short = "LGM stranded dbg",
                     long  = "Per-factor red/green labels by the LGM" },
  tank_position  = { short = "Tank position",
                     long  = "Tank wu/gu/tile readout + standoff wu" },
  tank_angle     = { short = "Tank angle",
                     long  = "Tank heading in brads (256 = full circle): integer info.direction + float info.tank_angle when present, plus turn-ramp counter (firstLeft/firstRight) value the engine reports." },

  -- HUD text (corners).
  hud_manual         = { short = "HUD: manual (Bolo HUD)",
                         long  = "Single manual-mode Bolo HUD: bars, build menu, gun light, pill/base grids, key list. Only renders in manual mode." },
  hud_resources      = { short = "HUD: resources",
                         long  = "Shells/Mines/Armour/Trees/Speed/Boat counters" },
  hud_tick_info      = { short = "HUD: tick info (think_ms / phase / goal / capacity tier)",
                         long  = "Top-left lines under the big TICK box. Brain-side timing via os.clock; color codes the think_ms cell green<5/yellow<10/orange<20/red>=20. Also shows the execution capacity tier (\"capacity: tier N/10\"), color-shifted toward red as the tier drops (red<=3, orange<=5, yellow<=7) so you can filter/search for 'tier' to find it." },
  hud_replan         = { short = "HUD: replan + phase",
                         long  = "Replan countdown + phase + reason" },
  hud_goal           = { short = "HUD: current goal",
                         long  = "Active goal kind / target / substate" },
  hud_next_goal      = { short = "HUD: next goal",
                         long  = "Lookahead next goal (state.next_goal) or the runner-up candidate" },
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
                         long  = "Bottom-right GoalHunter vN.N label" },


  -- Tank / aim.
  tank_hitbox       = { short = "Tank hitbox",
                        long  = "Yellow ±128 wu hitbox outline around own tank" },
  tank_aim_marker   = { short = "Tank aim marker",
                        long  = "Yellow crosshair + corners at the aim wu point" },

  blitz_call = { short = "Blitz call (self)",
                 long  = "Soldier/self view of the blitz call: our answer (Y->commander / N), our computed standoff coords, the walk distance we reported (bd), repos count (commander 'pick another' rejections), and a map dot on our standoff." },

  blitz_roster = { short = "Blitz roster (commander)",
                   long  = "Commander/self negotiation table: one row per answering soldier — id, Status (y=committed / m=negotiating), Engage spot (bes), Dist (reported walk dist bd), EngageAnswer (our verdict: ACC / REJ=clash-or-wall-blocked / -)." },

  blitz_wait_timeout = { short = "HUD: blitz_wait GO timeout",
                         long  = "Commander-only HUD panel shown while waiting in blitz_wait: ready/total soldiers, the progress-based GO timeout (base READY_TIMEOUT + accumulated patience extension), elapsed/remaining, and the closest pending soldier's walk dist (min_bd) with a CLOSING/STALLED indicator. A bar shows elapsed vs the effective timeout (green = a soldier is still making progress so the timeout keeps extending, red = stalled/about to fire GO)." },

  blitz_join_hl = { short = "Joinable-blitz pills",
                    long  = "Cyan border + 'JOIN BLITZ' tag around each pill an ally is running an OPEN, in-range blitz call on. These pills are exempt from ally_claimed de-confliction — taking one JOINS the blitz rather than yielding to the ally." },

  help_range = { short = "Commander help range",
                 long  = "Manhattan diamond of radius SQUAD_HELP_RANGE around each blitzing commander's pill (self + allied commanders) — the region in which a soldier will answer the take. Matches the actual join gate (Manhattan dist tank->pill)." },

  -- Squad coordination: a soldier's claimed blitz engage standoff + setup point.
  squad_blitz       = { short = "Squad blitz engage",
                        long  = "The followed bot's claimed blitz engage standoff (magenta, green when IN POSITION), the setup/approach point (orange), and a line to the pill." },

  blitz_negotiate_scan = { short = "Blitz negotiate: standoff scan",
                           long  = "While a NON-commander is negotiating a blitz, the full ellipse standoff scan it runs for the target pill: each candidate spot's LOS box, maneuver ellipse + tiles, and the A+B+D+E=total score label (bucket members overdrawn dark purple). Shows the scores the soldier is choosing its offered standoff from. Only drawn while actively offering (one frame if the offer is accepted immediately)." },

  -- Squad coordination: role label above each tank (C#/S# [C#]/H#).
  squad_labels      = { short = "Squad tank labels",
                        long  = "Role label above each protocol tank: 'C<pn>' (commander), 'S<pn> [C<cmdr>]' (soldier + its commander), 'H<pn>' (harasser). Colored by role." },

  -- Pill interaction.
  pill_range_circle = { short = "Pill range circles",
                        long  = "Red max-range + green standoff circles around target pill" },
  pill_id_label     = { short = "Pill/base IDs",
                        long  = "Numeric ID labels overlaid on every pill/base tile" },
  defend_pill_viz   = { short = "Defend pill tiers",
                        long  = "Per team pill: ring + label colored by the defend threat tier that priced it (red=siege, orange=setup tell, yellow=sighting, blue=worn/damaged-quiet, grey=quiet undamaged, green=HEAT bid, dark=arrived no-bid) with the pool cost. Quiet/worn pills are priced by the DEFEND_QUIET_DMG_COST curve on hits taken (0 hits ~1500, easing to the 250 floor as the pill gets chewed up), not by base+travel. On the ACTIVE defend goal's pill: the Euclidean DEFEND_ARRIVE_RADIUS circle where the travel phase hands off to the heat gate. Mirrors eval_defend_pill's actual tiers/radius." },
  heat_pill_viz     = { short = "Heat pill action",
                        long  = "While executing a heat win: line tank->pill, circle on the pill, and fired-count label (heat_pill_position/aim/shoot -> heat_done). Matches defend_pill_steer's sequence." },

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
  charge_stop_pred  = { short = "Charge stop predict",
                        long  = "Predicted brake-now stop point (cpf.predict_stop): marker + line from tank, GREEN if a shot from there hits the pill, RED if it falls short. Lets you eyeball whether the stop predictor is accurate." },
  stop_predict_live = { short = "Stop predict (live)",
                        long  = "Always-available stop-distance prediction: one orange square per simulated brake step from the tank to the engine-exact brake-now stop point, drawn EVERY tick regardless of substate (charge_stop_pred / approach_stop_pred only draw during those phases). Plus a STOP <dist> spd=<n> label at the stop point." },
  approach_stop_pred = { short = "Approach stop predict",
                        long  = "attack_pill approach 'lined-up fast-path': when heading error is small the tank cruises at full speed and brakes off cpf.predict_stop instead of the slow proportional creep. Only drawn while aligned. Line+box = predicted brake-now stop point; small blue box = the approach point we're landing on. GREEN when the predicted stop lands on it, ORANGE while still closing." },
  fast_approach     = { short = "Fast approach branch",
                        long  = "Names the throttle branch actually driving the tank during the attack_pill 'approach' substate, drawn above the tank EVERY tick the dispatcher runs (i.e. before the stage-2 creep handoff). GREEN = the predict_stop fast-path (ap_linedup_fast) owns throttle as intended; RED = some OTHER branch pre-empted it (lgm_pace_brake / cliff_brake / facing_away / ap_brake_zone) — that's why a slow crawl reappears. Shows FAST_APPROACH flag, branch, spd, corr, sdist." },
  detree_progress   = { short = "Detree progress",
                        long  = "DETREE N/M (left=K) overlay above tank" },

  -- Pill take / shield system.
  plan_trace             = { short = "Plan-pos trace",
                             long  = "Always-on multiline label above tank showing each plan_position gate state: chunk status, spots/los/greens counts, best pick, shield scan result. Lets you see at a glance where the chain breaks." },
  shield_scan_candidates = { short = "Shield: candidates (lua version)",
                             long  = "Per-candidate score boxes for the 8 ring positions + standoff. ON forces the slow lua version of shield.scan (it keeps the per-candidate data this draws); OFF uses the fast c version, which discards it. Default OFF so unattended -braindebug hosts (winbolods recording) keep the c version.",
                             default_on = false },
  shield_scan_blockers   = { short = "Shield: blockers",
                             long  = "Winner's per-aim blocker borders + colored fills" },
  shield_blocker_union   = { short = "Shield: blockers union (lua version)",
                             long  = "Union across all candidate standoff angles. BLACK = every tile a pillbox bullet crosses (raw, NO gating — was even considered, incl. tiles our own shot crosses). On top: green=actual wall/pill, yellow=potential buildable, grey=LGM-unreachable (these passed every gate). So black-only = on a shot path but REJECTED (too close/behind/unbuildable); no box = never considered. ON forces the slow lua version of shield.scan; OFF uses the fast c version. Default OFF so unattended -braindebug hosts keep the c version.",
                             default_on = false },
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

  repair_pill_viz   = { short = "Repair pill viz",
                        long  = "Yellow target ring + status line for repair_pill dispatch (green=ready, red=blocked)" },

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
  swerve_shell_scan = { short = "Swerve shell scan",
                        long  = "Incoming-shell scan during swerve, computed in the tank's MOVING frame (relative velocity). Per shell: a line to its closest-approach point — RED = it'll hit (marker lands on the tank, within the orange hit ring), GREEN = misses. Yellow ring = scan radius. Cyan ghost = predicted tank position (our velocity projected forward — the path the dodge math uses). HUD shows armed/unloaded + arm-timeout." },
  bpc_cover_samples = { short = "BPC cover samples",
                        long  = "Blue tile outlines from line_walk during BPC (Basic Pill Capture) cover sweep" },

  -- Misc tile/state markers.
  blocked_tiles     = { short = "Blocked tiles",
                        long  = "Red outlines on tiles in state.blocked" },
  bait_pill_marker  = { short = "Bait pill marker",
                        long  = "BAIT? markers on deepsea pills suspected as bait" },
  pill_reposition_marker = { short = "Pill reposition marker",
                             long  = "Orange outline around pills marked for repositioning" },
  reposition_scores      = { short = "Reposition scores",
                             long  = "This tank's top-N scored reposition candidate pills: a line from the tank to each, a ring, and the score (lower = more worth moving). '(x)' means the tank can't currently carry the move out. Plus a HUD line showing the last 'move-pill' position-scan: tick #, ticks ago, seconds ago (flashes '<- COMPUTED' on the scan tick). Per-tank — switch focus to see another bot's picks." },
  reposition_vote        = { short = "Reposition vote",
                             long  = "Live reposition VOTE panel (top-left HUD): an open vote we started (ticks left, NO/YES tally), a vote we're balloting on, and the latched PASS/FAIL result (kept on screen ~120 ticks) with the yes/no voter sets." },
  reposition_scores_hud  = { short = "Reposition scores (table)",
                             long  = "Top-5 reposition candidates as a HUD table: rank, pill #, score (lower = more worth moving), tile, and whether this bot can carry the move out. Table form of 'Reposition scores'." },
  reposition_votes       = { short = "Reposition votes (table)",
                             long  = "Per-ally reposition-vote table: one colored square per ally (yellow = vote ongoing, green = passed, red = failed) plus a notes column — who is INITIATING the vote, who voted YES/NO, and (for this bot) the REASON it voted that way." },
  wounded_pill_marker    = { short = "Wounded pill marker",
                             long  = "Marker on the wounded-pill carryover target" },
  kill_pickup            = { short = "Fresh-kill pickup",
                             long  = "Ring + tank line on the pill we just killed and are committing HARD to grab (yellow=ours, magenta=yielded to a higher-armour blitz ally)" },
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

  ghost_tank        = { short = "Ghost tank tracking",
                        long  = "Extrapolated position + TTL of an enemy tank that went out of sight (forest/fog)" },

  -- Ally-state overlay (right middle): per-player table populated from
  -- the chat-based shared-state messages. One row per active slot
  -- (bot #, goal, substate, target, k=v data).
  ally_state_overlay = { short = "Ally state table",
                         long  = "Right-middle HUD showing each ally's goal / sub / target / k=v data, fed by the chat-based ally_state slate" },
  pill_portfolio     = { short = "Pill portfolio table",
                         long  = "Far-left HUD: one row per friendly pillbox, colored by reposition category (back/front/aggressive/in-use), with a legend below. Categories use the influence/front-line ('3') definition." },
  pill_best_spots_back  = { short = "Best BACK pill spots",
                            long  = "Map overlay: top evaluated placement spots for a BACK pill, as bold filled orange squares (most opaque = best). Pairs with the '3' influence view. Fed by the place_pill_strategic candidate scan." },
  pill_best_spots_aggro = { short = "Best AGGRO pill spots",
                            long  = "Map overlay: top evaluated placement spots for an AGGRESSIVE pill, as bold filled red squares (most opaque = best). Pairs with the '3' influence view." },
  demine_scan = { short = "De-mine scan",
                  long  = "Auto mine-clear (demine.lua): the crosshair-reach ring the scan searches, a red inner 'no-blast' ring (min distance), and every considered mine tile — GREEN chosen, YELLOW candidate with its cost number, RED rejected with the reason (not our ground / cooldown / too close / out of reach / our LGM near / shot blocked). Cost = dist x (1 + BEHIND_MULT x angleoff/128), so mines near the heading win. While a kill_mine goal is active: a red target ring, a line from the tank, and live 'KILL MINE d=Nt sl=N age=N' (gunsight length + ticks since push)." },
  trepair_scan = { short = "Terrain-repair scan",
                   long  = "Auto battle-damage repair (demine.lua): the LGM-radius ring, and every crater/rubble/crater-flood-water tile in OUR territory — GREEN chosen, YELLOW valid ('road c=N' tree cost), RED rejected (not our ground / cooldown / open water / under tank / low trees / LGM unreachable / unsafe walk). While a repair job is active (parallel — the tank carries on with its goal): the target ring, a tank->tile line and (if the LGM is out) an LGM->tile line, and 'REPAIR tt=N cost=N age=N'." },
  test_no_refuel = { short = "HUD: TEST never-refuel bot",
                     long  = "TEST AID: red banner when the followed bot rolled the never-refuel flag (TEST_NEVER_REFUEL_CHANCE) — all its refuel candidates are rejected so it hits ammo-deprivation naturally. Roll results also print to the console at startup.",
                     default_on = true },
  ammo_deprive_countdown = { short = "HUD: Ammoless-helper countdown",
                     long  = "Followed bot's countdown to state.ammo_deprived — the 'ammoless helper' mode where a shell-starved tank stops trying to fight solo and instead joins any blitz / suicide-charges (AMMO_DEPRIVED_BLITZ_MULT). Mirrors strategy.lua exactly: the clock starts the tick shells fall below AMMO_DEPRIVED_SHELLS outside the opening phase (state.ammo_low_since), clears the instant shells recover to that line, and fires after AMMO_DEPRIVED_TICKS (~60s). States: grey=idle (ammo ok), BLUE=GATED (dry but suppressed by the opening land-grab phase — clock can't start yet), amber=counting down, red=ACTIVE. Shows seconds+ticks remaining and a fill bar." },
  wait_spot = { short = "Wait-for-LGM safe spot",
                long  = "Danger-aware wait_for_lgm: when the parked tile is under fire, shows the scored candidate ring (yellow=safe candidate, red=dangerous, grey=unreachable, green=chosen), the chosen wait tile (green ring), a line to the returning LGM, and the danger value that triggered the move. Data from pick_wait_spot — the same scores the goal used." },
  spike_pills = { short = "Spiking pills (base denial)",
                  long  = "Map overlay: hostile/neutral pills within PILL_FIRE_RANGE of a friendly base ('spiking' — they shoot us while we refuel, denying the base). Magenta square on the pill, line + tint to each denied base, 'SPIKE n=N' label. These pills get SPIKE_PILL_DISCOUNT on attack_pill combat cost; while any exists, every other pill pays SPIKE_OTHER_PENALTY. Same table the pool-6 cost reads." },
  panic_build = { short = "Panic build (emergency)",
                  long  = "Shown whenever a non-rejected enemy tank is present (attack_tank viable): the emergency def_build candidate spots (green=chosen, yellow=valid, red=rejected with reason), the threat tank (red), a line from us to it, and a 'PANIC BUILD' label. If a tank is present but we have no pill to drop, just a 'PANIC (no pill)' marker on the threat." },
  ally_avoid_overlay = { short = "Ally avoid zones",
                         long  = "Orange tiles around an ally tank doing a pill take (5x5 when within STANDOFF+2 of pill), plus the firing lane to the pill. Also prints `BLOCK: ON/OFF sub=… d=N/T` next to each attack_pill ally so you can see live whether the 5x5 stamp is active and how close they are to the activation threshold." },
  blocker_pills = { short = "Blocker pills (in-use)",
                    long  = "Friendly pills flagged _in_use (sitting on a take's blocker TILE → role utility, protected from reposition + repair). Filled orange tile + 'BLOCKER:src' label where src is 'me' (our own current_blocker_tiles) or 'pN' (ally N's pblk tile broadcast). The BLOCKER_VIZ print2 logs our own blocker tiles (packed my*256+mx) + team in-use count. If a pill you expect protected isn't orange, no bot is declaring its tile as a blocker." },
  cautious_nav_around_ally_take = { short = "Cautious-approach ring",
                     long  = "Yellow 3x3 ring(s) around every ally currently ON a pill take (state._ally_take_tiles). When OUR tank sits on or steps onto a ring tile it enters boat-style per-tile crawl (path_lookahead suppresses skip-ahead) + forced SLOWER: that tile flashes bright orange with a 'CRAWL:on/next' label, and a TAKE_CRAWL print2 fires. If the bot bowls through an ally take, check this overlay — no yellow ring means no ally is declaring the take; ring but no CRAWL flash means we never entered it." },
  nav_veer = { short = "Nav veer (ally dodge)",
               long  = "The live trace-time obstacle set fed to the Dijkstra route tracer: each converging blitz ally's exact tile as a solid RED square (follows the tank instantly, no slate lag), plus the resulting VEERED route from our tank to the current goal as a cyan polyline — so you can watch the path bend around allies in real time. Compare with the green optimal-path overlay to see the dodge." },

  -- plan_position chunked-sweep progress (low-tier multi-tick sweep).
  plan_position_progress = { short = "Plan-pos progress",
                             long  = "When the plan_position 72-angle sweep is chunked across N ticks at lower tiers, shows angle progress + tick counter near the tank" },

  -- Chunked plan_position pill-eval sweep (one pill at a time, only the committed attack target).
  pill_eval_progress     = { short = "Pill eval progress",
                             long  = "Progress bar above the pill being evaluated by the chunked plan_position 72-angle sweep" },

  -- Chat log overlay (right side, below ally_state_overlay).
  chat_log_overlay   = { short = "Chat log",
                         long  = "Ring of recent incoming + outgoing chat messages (in particular /info traffic)" },

  -- HUD line shown whenever refuel_at_base is the active goal: reports
  -- the chebyshev distance to the base, whether an ally tank is on the
  -- base tile, and (when in wait_for_ally) the chosen park tile.
  hud_refuel_ally_check = { short = "HUD: refuel ally check",
                            long  = "When refuel_at_base is the active goal, shows ally-on-base check distance, occupancy, and the wait_for_ally park tile." },

  ally_claimed_marker = { short = "Ally claimed marker",
                          long  = "Semi-transparent gray rectangle over each pill/base another bot is currently broadcasting as their goal (sourced from ally_state slate)." },

  circles = { short = "Front-line circles",
              long  = "Radius-12 front-line regions (R2): ring colored red=LOSING (lost ground over both 1m & 5m), orange=WARN (instant enemy-count advisory), else green/yellow by influence share; center dot, safe-tile dot, and a label with id/POI/ratio. Sourced from circles.lua." },

  circles_hud = { short = "HUD: circles table",
                  long  = "Right-side HUD: one row per circle — id, ratio now/prev/origin, fp/ep fb/eb tanks, losing?, reinforcement need. For tuning LOSE_DELTA." },

  circle_trend = { short = "Circle trend arrows",
                   long  = "Per-circle influence-share deltas vs the 1 min and 5 min lookbacks with ^/v/= arrows ('?' = that window not full yet); BOTH must be down >= LOSE_DELTA to count as losing." },

  reinforce_link = { short = "Reinforce link",
                     long  = "Line from this tank to its target losing-circle safe tile when reinforcing (R3-exec), plus a REINFORCING tag. Off unless CIRCLE_REINFORCE_ENABLED and a circle is losing." },

  circle_poi = { short = "Circle POI coverage",
                 long  = "Dots on every pill/base a circle covers (its greedy-selected POIs); faint marks on POIs no circle covers. Debugs circle placement." },

  circle_history = { short = "Circle history graph",
                     long  = "Full lineage graph of circle records (alive + destroyed within the last 5 min): each at its center with index + created/(destroyed) ticks + current ratio, lines connecting each record to its predecessor. Green = alive, gray = destroyed." },

  circle_warning = { short = "Circle warnings",
                     long  = "Advisory-only (display, no dispatch): orange ring + instant enemy-vs-friendly counts on circles whose instant pill/base/tank counts flag a warning. For hooking behavior later." },

  role_live = { short = "Live squad roles",
                long  = "C/S/H tag over each tank; for a dynamic commander shows the trigger (take #pid HP). Self shows live role + reason. R0 debugging." },

  hard_take_pills = { short = "Hard-take pills",
                      long  = "Ring + HP label on every pill at/above HARD_TAKE_MIN_HP — the targets that spawn a commander+squad under DYNAMIC_COMMANDERS." },

  pill_roles = { short = "Pill portfolio roles",
                 long  = "Every friendly pill tinted by cached role: back/front/aggro/util. Shows the 20/45/20/15 balance and the shoot-range front classification." },

  front_band = { short = "Front line + shoot band",
                 long  = "Front '3' tiles plus the ~9-tile euclidean band that defines the 'front' pill category — why a pill is/isn't front." },


  repos_claims = { short = "Reposition claims",
                   long  = "Marks each pill an ally is repositioning (repos=1 claim this bot honors) vs the pill this bot would pick. Confirms R3a de-confliction." },

  enemy_lgm_marker = { short = "Enemy LGM marker",
                       long  = "Yellow X over every visible hostile LGM with a small velocity arrow when it's moving. Sourced from perc.enemy_lgms." },

  kill_lgm_status = { short = "Kill-LGM status",
                      long  = "Per-LGM kill-evaluation labels (dist / aim corr / LOS clear-blocked / would-fire) plus a HUD summary line for the chosen target. Always shows when at least one hostile LGM is in view." },


  lgm_registry_hud = { short = "HUD: LGM registry",
                       long  = "Right-side HUD table with one row per known player_num's LGM state (status / tile / source / respawn countdown). Sourced from lgm_registry." },

  lgm_registry_map = { short = "LGM registry map markers",
                       long  = "Per-player tile rings on the map for every LGM the registry knows about: green=alive ally, red=alive enemy, gray=dead with respawn countdown. Skips self (covered by ally_lgm_marker / own-LGM overlay)." },

  kill_lgm_engage  = { short = "Kill-LGM engage spot",
                       long  = "Magenta ring on the chosen engage tile (the closest-reachable in-range boundary tile around the LGM, picked by refresh_kill_lgm) + a line from the tank to that tile. Shows where the bot is driving while out of range." },

  kill_lgm_predict = { short = "Kill-LGM lead prediction",
                       long  = "Ring + line from each visible LGM to the predicted shell-impact tile. Colored by predictor tier: yellow=linear (no straight-line lock yet), cyan=destination-locked engine sim (3-window match active). PRED label shows flight_ticks + tier." },

  kill_lgm_sim_path  = { short = "Kill-LGM forward sim path",
                         long  = "Dotted trail of the LGM's projected positions out to shell-impact-ticks, using the engine-faithful sim (terrain v_max, wall sliding, corner blocks). Only drawn when predict_aim picked the dest_lock tier (3 matching 10-tick velocity windows → straight-line walk detected); nothing shown for the linear fallback." },

  test_lgm_target    = { short = "Test: LGM victim target",
                         long  = "Cyan rect on the tile the victim bot's LGM is currently being dispatched to build. Test-harness only; only renders when the bot was flagged via BrainTest's -victim_ids <ids> CLI option (sets _BT_VICTIM=true)." },

  test_victim_marker = { short = "Test: VICTIM marker",
                         long  = "Big red VICTIM label + outline above the victim bot's own tank so it's instantly visible which bot is in test-victim mode (vs. the real attackers being tested). Renders whenever _BT_VICTIM is true (set by BrainTest's -victim_ids CLI flag)." },

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

  hud_throttle       = { short = "Throttle decision HUD",
                         long  = "Top-left text showing which throttle elseif-branch fired this tick (cruise / ap_brake_zone / facing_away / kill_lgm_halt / boat_exit / etc.) plus key inputs (speed, abs_corr, eff_dist, brake_dist) and flags (boat_exit, inboat, cliff, facing_away, orbit, ap_brake) plus the keys actually pressed. Great for the 'tank stuck at speed 0' class of bug." },

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
  pill_shot_count    = { short = "Pill HP",
                         long  = "Cyan number above the target pill = its remaining HP. Pair with 'Tank shots that hit pill' (orange in-air count): orange >= cyan means the kill is locked." },
  tank_shots_hit_pill = { short = "Tank shots that hit pill",
                         long  = "Orange 'air N' above the target pill = OUR tank shells currently IN FLIGHT that will HIT this pill (C-sim trajectory reaches the pill tile, no forest/wall/other-pill blocking first) = goal._on_target_in_flight. When N >= pill HP we hold fire." },

  -- Magenta path the kill_hardline take is driving to the tile beside the pill.
  hardline_path       = { short = "Hardline path",
                          long  = "Magenta line of the kill_hardline approach path (target pill's danger subtracted) + box on the chosen tile beside the pill" },

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

-- Layers EXEMPT from the "all" collect override. Forcing these on has side
-- effects beyond drawing, so even when the V-window radio asks to collect ALL
-- layers we leave these on their real toggle:
--   label_hud_overlays    — prefixes every HUD string with its [viz_id]
--   pill_best_spots_back/aggro — makes place_pill_strategic run its full
--                           candidate scan EVERY tick (goals.lua viz_only gate)
M.COLLECT_EXEMPT = {
  label_hud_overlays    = true,
  pill_best_spots_back  = true,
  pill_best_spots_aggro = true,
}
local _collect_exempt = M.COLLECT_EXEMPT

function M.refresh()
  -- Collection-mode override (set per-bot by BrainTest's V-window radio):
  --   "off" → this bot emits NOTHING (so a non-viewed tank isn't collected)
  --   "all" → this bot emits EVERY layer regardless of its toggle (so a replay
  --           has all visualizers for it even ones that were off)
  --   nil/other → normal per-layer toggle behavior.
  local collect = _G._BT_VIZ_COLLECT
  local suppress_all = _G._BT_VIZ_SUPPRESS_ALL
  for id in pairs(M.IDS) do
    if collect == "off" then
      _on[id] = false
    elseif collect == "all" and not _collect_exempt[id] then
      _on[id] = true
    elseif suppress_all and id ~= "hud_resources" then
      _on[id] = false
    else
      local g = _G["_BT_VIZ_" .. id:upper()]
      if g ~= nil then
        _on[id] = g
      else
        -- No host toggle pushed (winbolods / release client): honor the
        -- declared default. default_on=false viz (e.g. label_hud_overlays)
        -- stay OFF instead of defaulting on — matches is_on()'s cold path and
        -- the documented intent above.
        local entry = M.IDS[id]
        _on[id] = not (entry and entry.default_on == false)
      end
    end
  end
end

function M.is_on(viz_id)
  -- Master gate: visualizers exist only in debug mode. In production — or a
  -- base brain run without -braindebug — every viz query no-ops here, so the
  -- brain also skips any viz-only precompute it guards with is_on. (Phrased
  -- "if not" so lua_strip's "if BRAIN_DEBUG_MODE" block matcher leaves it.)
  if not BRAIN_DEBUG_MODE then return false end
  -- Fast path: if refresh() has populated the cache, answer in O(1).
  local cached = _on[viz_id]
  if cached ~= nil then return cached end
  -- Cold path: cache not populated yet (first tick / non-BrainTest run).
  assert_id(viz_id)
  local collect = _G._BT_VIZ_COLLECT
  if collect == "off" then return false end
  if collect == "all" and not _collect_exempt[viz_id] then return true end
  if _G._BT_VIZ_SUPPRESS_ALL and viz_id ~= "hud_resources" then
    return false
  end
  local g = _G["_BT_VIZ_" .. viz_id:upper()]
  if g ~= nil then return g end
  local entry = M.IDS[viz_id]
  if entry and entry.default_on == false then return false end
  return true
end

-- Self-assign stable viz indices (sorted by id) and publish _BT_VIZ_IDS.
-- Used by headless hosts (winbolods recorder) so vid() stamps a real
-- viz_idx on every emitted overlay — otherwise everything records as
-- 255/NONE and can't be labelled, filtered in playback, or diffed/excluded
-- by category. M.legend_json() exports the idx->id map for the loader.
local function self_assign_ids()
  if _G._BT_VIZ_IDS then return end
  local ids = {}
  for id in pairs(M.IDS) do ids[#ids + 1] = id end
  table.sort(ids)
  local map = {}
  for i = 1, #ids do map[ids[i]] = i - 1 end
  _G._BT_VIZ_IDS = map
end

-- Register every entry in M.IDS with the host's V dialog. Called from
-- Brain.open(). NOTE: braintest_viz_register is ALWAYS bound (it lives in
-- shared braincore.c); it returns the assigned index, or -1 when no host
-- registry is wired (WinBolo client / headless winbolods). So we detect the
-- headless case by the return value, not by the binding's presence, and
-- self-assign indices then.
function M.register_all()
  if not braintest_viz_register then
    self_assign_ids()   -- binding truly absent
    return
  end
  local host_assigned = false
  for id, entry in pairs(M.IDS) do
    -- The 5th arg (default_on) is omitted so the C binding defaults
    -- to ON. Brains that want a viz off-by-default can pass the
    -- entry through with a `default_on = false` field; we honor it.
    local def_on = entry.default_on
    if def_on == nil then def_on = true end
    local idx = braintest_viz_register(id,
                           entry.short or id,
                           entry.short or "",
                           entry.long or "",
                           def_on)
    if type(idx) == "number" and idx >= 0 then host_assigned = true end
  end
  -- Binding present but no host registry (returned -1 for everything):
  -- headless winbolods → self-assign so overlays carry a real viz_idx.
  if not host_assigned then self_assign_ids() end
end

-- idx -> viz_id legend (JSON object {"0":"id0",...}) for the brain recorder.
-- Written once into the .btr so the BrainTest loader can map each recorded
-- viz_idx back to a category name (then to its own registry index) for
-- playback filtering. Index VALUES needn't match BrainTest's — the names do.
function M.legend_json()
  local t = _G._BT_VIZ_IDS or {}
  local parts = {}
  for id, idx in pairs(t) do
    parts[#parts + 1] = string.format('"%d":"%s"', idx, id)
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

-- Categories the brain RECORDER skips (winbolods .btr only). They still draw
-- live in BrainTest; this just keeps them off disk because they dominate file
-- size and are cosmetic/derivable:
--   label_overlays               — per-shape caption helper, ~48% of bytes
--   attack_scan_spots_all_pills  — all-pills spot scan, ~38% of bytes
-- Add more ids here to drop them from recordings.
M.RECORD_SKIP = {
  label_overlays = true,
  attack_scan_spots_all_pills = true,
}

-- CSV of viz indices for the RECORD_SKIP categories (resolved via the live
-- _BT_VIZ_IDS map). The recorder marks these viz_idx values and drops matching
-- overlay commands when serializing each frame.
function M.record_skip_idx_csv()
  local t = _G._BT_VIZ_IDS or {}
  local parts = {}
  for id in pairs(M.RECORD_SKIP) do
    local idx = t[id]
    if idx then parts[#parts + 1] = tostring(idx) end
  end
  return table.concat(parts, ",")
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
  overlay_text(x, y, viz_id, "topleft", 220, 220, 220, 200, LABEL_SIZE,
               vid("label_overlays"))
end

-- Drawing wrappers. Each ALWAYS emits the underlying overlay_* call
-- (when available) and tags it with the viz_id's stable index, so
-- BrainTest can filter at draw time — including for recorded frames
-- during playback when V checkboxes change. Brain code that wants to
-- skip expensive precompute should still call M.is_on(viz_id) first.
-- Exception: _BT_VIZ_COLLECT == "off" (BrainTest non-followed bots,
-- winbolods -bd-noviz) means THIS BOT EMITS NOTHING — that's the
-- documented collect-off contract, and the Lua→C overlay calls are
-- the cost being avoided, so the wrappers short-circuit here.
function M.text(viz_id, ...)
  if not BRAIN_DEBUG_MODE or _G._BT_VIZ_COLLECT == "off" then return end
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
  if not BRAIN_DEBUG_MODE or _G._BT_VIZ_COLLECT == "off" then return end
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
  if not BRAIN_DEBUG_MODE or _G._BT_VIZ_COLLECT == "off" then return end
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
  if not BRAIN_DEBUG_MODE or _G._BT_VIZ_COLLECT == "off" then return end
  assert_id(viz_id)
  if not overlay_circle then return end
  local idx = vid(viz_id)
  -- overlay_circle args: cx, cy, radius, r, g, b, a, viz_idx, subpixel, filled
  local cx, cy, radius, r, g, b, a, subpixel, filled = ...
  local result = overlay_circle(cx, cy, radius, r, g, b, a, idx, subpixel, filled)
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
  -- Optional: tag each overlay with its viz id so you can identify which
  -- toggle owns each on-screen text block. (label_hud_overlays itself never
  -- routes through hud_text, so no recursion.)
  if M.is_on("label_hud_overlays") and type(text) == "string" then
    text = "[" .. viz_id .. "] " .. text
  end
  return overlay_hud_text(x, y, text, anchor, r, g, b, a, idx)
end

-- HUD-space rectangle (solid fill or 1px outline), same pixel-offset/anchor
-- scheme as hud_text. Args: x, y, w, h, anchor, r, g, b [, a [, filled]].
function M.hud_rect(viz_id, ...)
  assert_id(viz_id)
  if not overlay_hud_rect then return end
  if not M.is_on(viz_id) then return end
  local idx = vid(viz_id)
  local x, y, w, h, anchor, r, g, b, a, filled = ...
  return overlay_hud_rect(x, y, w, h, anchor, r, g, b, a or 255, filled and 1 or 0, idx)
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
  overlay_detail(detail_id, kind, x1, y1, x2, y2, label or "")
  if body_lines and overlay_detail_text then
    for i = 1, #body_lines do
      overlay_detail_text(detail_id, body_lines[i])
    end
  end
end

return M
