/*
 * Tiny in-repo test harness. Each test source declares an entry
 * point of the form `int run_<name>(void)` returning 0 on success
 * and non-zero on failure (printing diagnostics to stderr).
 *
 * test_main.c dispatches between them by name so each CTest entry
 * can run a single test, and so a developer can run one test at a
 * time from a single binary.
 */
#ifndef WINBOLO_UNITTEST_HARNESS_H
#define WINBOLO_UNITTEST_HARNESS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "scenario_table.h"  /* ScnTable — the fixture brain's record below */

#define UT_FAIL(fmt, ...)                                                   \
    do {                                                                    \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n",                            \
                __FILE__, __LINE__, ##__VA_ARGS__);                         \
        return 1;                                                           \
    } while (0)

#define UT_ASSERT(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: assertion failed: %s\n",           \
                    __FILE__, __LINE__, #cond);                             \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define UT_ASSERT_MSG(cond, fmt, ...)                                       \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s — " fmt "\n",                   \
                    __FILE__, __LINE__, #cond, ##__VA_ARGS__);              \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#ifdef __cplusplus
extern "C" {
#endif

/* The name of the test currently running, as test_main.c's table spells it
 * ("none" outside a test). For a test that needs a FIXTURE ON DISK.
 *
 * CTest gives each entry its own process (`WinBoloUnitTests --test <name>`)
 * and `ctest -j` runs several of those at once, all sharing one working
 * directory. A fixture at a path baked in at compile time is therefore
 * shared by every test in its file: they create it, read it and delete it
 * under each other, and the failures land on whichever test lost the race.
 * A mutex fixes nothing — the contention is between processes, not threads.
 *
 * So build the path from this, and two tests can never collide. It is the
 * test name rather than the process id on purpose: the path is then the
 * SAME on every run, so a test that removes its own fixture on the way in
 * also clears the wreckage a previously crashed run left behind. A pid
 * would make every crash leak a directory nothing ever cleans up.
 *
 * The one case this does not cover is two concurrent runs of the same test.
 * CTest never does that within one invocation, but two invocations do it all
 * the time — a developer alongside an agent, or two worktrees on one
 * machine — and a name that is the same every run is exactly what makes
 * them collide. utScratchPath below is the answer to that; prefer it for
 * anything that lands on disk, and reach for this only to label something.
 *
 * Do not use it to vary what a test ASSERTS — only where it keeps its
 * scratch files. */
const char *utCurrentTestName(void);

/* Build a path to `leaf` inside a directory private to this process and
 * this test, creating the directory on first use. Pass NULL or "" for the
 * directory itself. False means the directory could not be created and
 * nothing was written to `out`.
 *
 * The layout is <build dir>/test-scratch/<pid>/<test name>/<leaf>, which
 * keeps two worktrees, two runs and two tests apart at once; scratch_dir.c
 * carries the reasoning and the measurement behind it. The process removes
 * its own subtree at exit.
 *
 * Use it for every file a test writes. An absolute path under /tmp is the
 * worst case — it puts every checkout on the machine on one file — but a
 * name baked in at compile time is only better by degree. */
bool utScratchPath(char *out, size_t outSz, const char *leaf);

int run_transport_local_passive_threads(void);
int run_sp_subscriber_delivery(void);
int run_active_sim_armed_on_lobby_tick(void);
int run_active_sim_cleared_on_cross_thread_destroy(void);
int run_active_local_input_to_shot(void);
int run_sp_shoot_through_timer(void);
int run_prefs_document_roundtrip(void);
int run_prefs_keys_roundtrip(void);
int run_prefs_doc_roundtrip(void);
int run_prefs_doc_defaults(void);
int run_prefs_doc_special_chars(void);
int run_prefs_doc_unknown_preserved(void);
int run_prefs_doc_save_atomic(void);
int run_prefs_api_roundtrip(void);
int run_prefs_api_defaults(void);
int run_prefs_api_corrupt_backup(void);
int run_prefs_api_debounce(void);
int run_prefs_api_shutdown_flush(void);
int run_prefs_api_upload_excludes_local(void);
int run_prefs_api_sync_dirty(void);
int run_prefs_api_device_identity(void);
int run_prefs_api_adopt_server(void);
int run_prefs_api_mark_synced(void);
int run_join_running_phase_not_lobby(void);
int run_lobby_settings_codec_and_apply(void);
/* The scenario the settings event names, onto the ClientSim and back out
 * through the accessors — with a scenario and then without one. */
int run_lobby_settings_scenario_apply(void);
/* Whether the server runs its scripts without the sandbox, onto the
 * ClientSim and back out through clientSimGetLobbyScenarioUnsafe. */
int run_lobby_settings_scenario_unsafe_apply(void);
int run_lobby_team_meta_codec_and_apply(void);
int run_lobby_bot_config_codec_and_apply(void);
int run_lobby_bot_brain_codec_and_apply(void);
int run_lobby_brain_list_codec_and_apply(void);
int run_lobby_brain_docs_chunk_codec_roundtrip(void);
int run_lobby_brain_docs_chunk_len_is_exact(void);

/* The brains' lobby texts: cached once, sent to a joiner, kept out of the
 * spectator ring's per-keyframe control snapshot, and dropped on the client
 * when a new catalogue arrives. */
int run_lobby_brain_list_clears_stale_texts(void);
int run_lobby_brain_announce_codec_and_apply(void);
int run_lobby_brain_docs_put_follows_generation(void);

/* The "name: text" lobby chat line has ONE spelling, because the lobby's
 * bot-announce poll searches the history for the line it just appended. */
int run_lobby_chat_line_format_is_what_is_appended(void);
int run_lobby_brain_docs_stay_out_of_the_control_snapshot(void);
int run_lobby_brain_docs_reach_a_joining_subscriber(void);
int run_lobby_brain_docs_refresh_moves_the_generation(void);
int run_lobby_sync_complete_codec_roundtrip(void);
int run_lobby_rating_posted_codec_roundtrip(void);
int run_command_codec_roundtrip_variants(void);
int run_command_codec_lobby_claim_start(void);
int run_command_codec_rating_posted(void);
int run_command_codec_view_state(void);
/* CMD_VIEW_CYCLE codec round-trip: kind + direction + from survive the wire,
 * including from == 0xFF, and the fixed body length is enforced. */
int run_command_codec_view_cycle(void);
/* CMD_PING's fixed five-byte body: kind plus two big-endian u16 WORLD
 * coordinates, both extremes of the range, and the length refusals. */
int run_command_codec_ping(void);
int run_lobby_claim_start_host_swaps_occupied(void);
int run_lobby_claim_start_host_assign_onto_own(void);
int run_lobby_claim_start_host_swap_into_none(void);
int run_lobby_claim_start_non_host_occupied_rejected(void);
int run_lobby_claim_start_non_host_other_slot_rejected(void);
int run_lobby_claim_start_self_free_and_release(void);
int run_lobby_claim_start_validation(void);
int run_player_mute_mask_roundtrip(void);
int run_command_codec_cmdseq_slot(void);
int run_command_codec_bounds_checks(void);
int run_command_rejected_parks_name_codes(void);
int run_command_rejected_ignores_other_slot(void);
int run_command_rejected_clear_resets_both_fields(void);
int run_live_stats_kill_credits_killer_and_victim(void);
int run_live_stats_suicide_counts_death_only(void);
int run_live_stats_lgm_loss_splits_victim_and_killer(void);
int run_live_stats_captures_credit_new_owner(void);
int run_live_stats_out_of_range_slot_ignored(void);
int run_live_stats_cleared_on_running_phase(void);
int run_shell_death_codec_roundtrip(void);
int run_shell_death_culls_matching_predicted_shell(void);
int run_shell_death_rejected_culls_without_impact(void);
int run_lobby_bot_config_memory_applies(void);
int run_lobby_bot_config_memory_empty_is_noop(void);
int run_lobby_bot_config_memory_unknown_key_ignored(void);
int run_lobby_bot_config_memory_not_honoured(void);
int run_lobby_bot_config_memory_manual_only(void);
int run_lobby_bot_config_memory_cleared_on_return_to_lobby(void);
int run_lobby_add_bot_rejects_empty_brain_path(void);
int run_lobby_add_bot_rejects_ai_none(void);
int run_lobby_add_bot_rejects_not_in_lobby(void);
int run_lobby_add_bot_rejects_non_host_sender(void);
int run_lobby_bot_count_counts_only_connected_bots(void);
int run_lobby_maxbots_cap_boundary(void);
int run_lobby_add_bot_rejects_when_at_maxbots(void);
int run_transfer_host_promotes_target(void);
int run_transfer_host_rejects_non_host_sender(void);
int run_transfer_host_openhost_does_not_grant(void);
int run_transfer_host_rejects_self(void);
int run_transfer_host_rejects_unconnected(void);
int run_transfer_host_rejects_bot_target(void);
int run_firstjoinhost_promotes_over_bot(void);
int run_firstjoinhost_off_leaves_host_alone(void);
int run_firstjoinhost_second_joiner_does_not_displace(void);
int run_command_queue_first_submit_drains(void);
int run_command_queue_second_submit_does_not_drain(void);
int run_command_queue_ack_drains_pending_tail(void);
int run_command_queue_ack_clearing_queue_skips_drain(void);
int run_game_vote_state_codec_back_to_lobby(void);
int run_game_vote_state_codec_surrender_passed(void);
int run_game_vote_state_decoder_rejects_short(void);
int run_vote_surrender_rejects_unassigned_caller(void);
int run_vote_surrender_starts_for_real_team_caller(void);
int run_vote_surrender_rejects_three_active_teams(void);
int run_vote_surrender_state_withheld_from_other_team(void);
int run_vote_surrender_newswire_hidden_from_other_team(void);
int run_vote_surrender_newswire_shown_to_own_team(void);
int run_vote_back_to_lobby_newswire_stays_public(void);
int run_vote_surrender_may_answer_rule(void);
/* All-bases win (test_base_win_immediate.c). One alliance holding every base
 * with none of them dead ends the round on the spot, announced and without
 * faking a vote; one dead base (armour <= MIN_ARMOUR_CAPTURE) holds the round
 * open until it regenerates, which is the whole comeback window. */
int run_base_win_ends_round_immediately(void);
int run_base_win_dead_base_blocks_the_win(void);
int run_base_win_fires_when_dead_base_regenerates(void);
int run_base_win_quitonwin_with_lobby_returns_to_lobby(void);
int run_base_win_nolobby_quitonwin_is_instant(void);
/* Game-over resolution (test_base_win_resolve.c). serverSimResolveGameOver
 * turns returnToLobbyReason into the returning lobby's message and the
 * WinBolo.net win crediting: a swept round reports its winner, a passed
 * vote is irrevocable and credits nobody, a surrender credits the opposing
 * team, and an abandoned round reports nothing. */
int run_base_win_reports_the_winner(void);
int run_manual_vote_countdown_survives_lost_base(void);
int run_surrender_credits_the_opposing_team(void);
int run_win_during_manual_countdown_resolves_as_vote(void);
int run_abandoned_round_reports_nothing(void);
int run_round_stats_zeroed_on_fresh_sim(void);
int run_round_stats_kill_basic(void);
int run_round_stats_drown_not_suicide(void);
int run_round_stats_suicide_shell(void);
int run_round_stats_mine_death(void);
int run_round_stats_trees_wasted_and_pills_dropped(void);
int run_round_stats_capture_steal(void);
int run_round_stats_base_capture_steal(void);
int run_round_stats_lgm(void);
int run_round_stats_lifecycle_reset(void);
int run_round_stats_reset_clears_notables(void);
int run_round_stats_notables_ordered(void);
int run_round_stats_leaver_dropped(void);
int run_round_stats_attribution_callbacks(void);
int run_round_stats_direct_damage(void);
int run_round_stats_mine_owner_api(void);
int run_round_stats_mine_damage(void);
int run_round_stats_leaver_clears_mines(void);

/* A mine kill reaches the event buffer (test_mine_kill_event.c): a tank
 * destroyed by a mine publishes EVENT_TANK_KILLED naming the mine's layer as
 * the killer and LAST_DEATH_BY_MINES as the cause, on land and in a boat, and
 * a tank on its own mine names itself without being credited a kill. Each
 * detonation goes through minesExpCheckFill, which is what reads the layer
 * out of the mine grid. */
int run_mine_kill_publishes_event(void);
int run_mine_kill_on_boat_publishes_event(void);
int run_mine_kill_own_mine_names_self(void);
int run_mine_blast_range(void);

int run_awards_basic_winners(void);
int run_awards_tiebreak(void);
int run_awards_omission(void);
int run_awards_floors(void);
int run_awards_include_bots(void);
int run_awards_subset_basic(void);
int run_awards_subset_deterministic(void);
int run_round_stats_codec_roundtrip(void);
int run_round_stats_codec_rejects_bad_key(void);
int run_round_stats_codec_worstcase(void);
int run_round_stats_scenario_score_codec(void);
int run_round_stats_build_summary(void);
int run_round_stats_scenario_score_filled(void);
int run_round_stats_summary_highlights(void);
int run_round_stats_client_ingest(void);
int run_round_stats_track_records(void);
int run_round_stats_track_cap(void);
int run_round_stats_derive_equivalence(void);
int run_highlights_cluster_wipe(void);
int run_highlights_wipe_team_bonus(void);
int run_highlights_objective_steal(void);
int run_highlights_award_anchor(void);
int run_highlights_selection(void);
int run_highlights_lead_in(void);
int run_highlights_empty(void);
int run_territory_shift_basic(void);
int run_highlights_turning_point(void);
int run_territory_shift_bounded(void);
int run_territory_recent_damage(void);
int run_highlights_front_collapse(void);
int run_highlights_award_anchor_density(void);
int run_highlights_time_spread(void);
int run_highlights_multi_lgm(void);
int run_highlights_fumble(void);
int run_highlights_rare_death(void);
int run_highlights_award_dedup(void);
int run_highlights_pickup_spree(void);
int run_highlights_action_density(void);
int run_lv_stats_clip_time_format(void);
int run_attribution_track_schema(void);
int run_vote_toggle_standalone_no_does_nothing(void);
int run_vote_toggle_invalid_mode_dropped(void);
int run_vote_toggle_yes_opens_vote(void);
int run_vote_toggle_no_during_running_records_answered(void);
int run_vote_back_to_lobby_blocked_without_lobby(void);
int run_vote_surrender_blocked_without_lobby(void);
int run_lobby_set_team_clamps_max_tanks(void);
int run_lobby_set_team_accepts_max_legal(void);
int run_lobby_set_team_accepts_unassigned(void);
int run_lobby_set_team_clamps_above_max(void);
int run_server_text_codec_via_chat_wire(void);
int run_md5_rfc1321_vectors(void);
int run_md5_streaming_matches_oneshot(void);
int run_md5_block_boundaries(void);
int run_md5_to_hex(void);

/* bolo_rand save/restore (test_rand_save_restore.c): a restored snapshot
 * rewinds the generator exactly, so draws made between save and restore do not
 * shift the post-restore sequence — the contract that keeps cosmetic bot
 * naming from perturbing the deterministic game stream. */
int run_rand_save_restore(void);

/* Network-optimization wire changes (test_net_opt_wire.c): the pill
 * armour/inTank byte-packing helpers round-trip across the full range, and
 * the split base / packed pill game events have the expected wire sizes and
 * reliability classes. */
int run_pill_armour_intank_roundtrip(void);
int run_base_event_classification(void);

int run_ranked_flag_persists_with_one_player(void);
int run_ranked_shape_gate(void);
int run_lobby_lock_bit_lookup(void);
int run_lobby_lock_rejects_settings(void);
int run_lobby_lock_mask_roundtrip(void);
int run_view_policy_defaults(void);
int run_view_policy_apply_validates(void);
int run_view_policy_lock_bits(void);
int run_view_policy_lobby_reset(void);
int run_classic_mode_defaults(void);
int run_classic_mode_forces_views(void);
int run_classic_mode_lock_bit(void);
int run_classic_mode_lock_implied(void);
int run_classic_mode_lock_blocks_dispatch(void);
int run_classic_mode_lobby_reset(void);
int run_allies_in_trees_defaults(void);
int run_allies_in_trees_classic_mode(void);
int run_lobby_mods_enabled_defaults(void);
int run_lobby_mods_enabled_dispatch(void);
int run_lobby_mods_enabled_codec(void);
int run_fog_settings_defaults(void);
int run_fog_settings_classic_mode_preset(void);
int run_fog_settings_lock_implied(void);
int run_fog_settings_lobby_edits(void);
int run_fog_settings_lobby_reset(void);
int run_visibility_preset_round_trip(void);
int run_visibility_preset_custom(void);
int run_visibility_preset_ignores_decay(void);
int run_visibility_preset_pref_words(void);
int run_info_packet_view_policy_layout(void);
int run_info_packet_preset_round_trip(void);
int run_info_packet_absent_views_read_classic(void);
int run_info_packet_view_policy_length_tier(void);
int run_upload_cap_enforced(void);
int run_map_field_clamps_evil(void);
int run_map_field_clamps_passthrough(void);
int run_map_field_clamps_angry_start(void);
int run_map_field_clamps_no_sim(void);
int run_map_reload_rollback(void);

/* In-memory BMAP parser (test_map_read_memory.c). mapReadFromMemory
 * must agree byte-for-byte with mapRead on valid maps, the WBN
 * preview conversion (file bytes -> compressed -> preview) must
 * round-trip, and malformed buffers must be rejected. */
int run_map_read_memory_matches_file(void);
int run_map_read_memory_handbuilt(void);
int run_map_convert_file_to_compressed(void);
int run_map_read_memory_rejects_garbage(void);
int run_map_read_clears_mine_under_pill(void);
int run_upload_busy_predicate(void);
int run_upload_lost_ack_retry(void);
int run_upload_timeout_releases_other_player(void);
int run_upload_lost_done_retry(void);
int run_upload_partial_timeout_retry(void);
int run_upload_filename_safe(void);
int run_upload_filename_safe_script(void);
int run_lobby_time_minutes_valid(void);
int run_lobby_bot_name_rejects_reserved_prefix(void);
int run_lobby_bot_name_rejects_disallowed_char(void);
int run_lobby_bot_name_rejects_collision(void);
int run_lobby_bot_name_skip_does_not_bypass_validator_prefix(void);
int run_lobby_bot_name_skip_does_not_bypass_validator_control(void);
int run_lobby_bot_name_skip_excludes_self_from_uniqueness(void);
int run_lobby_bot_name_accepts_clean_unique(void);
int run_bot_pool_builtin_defaults(void);
int run_bot_pool_install_and_clamp(void);
int run_bot_pool_install_dedup_and_drop(void);
int run_bot_pool_reset_restores_builtin(void);
int run_bot_pool_pick_unique_and_overflow(void);
int run_bot_pool_json_load_roundtrip(void);
int run_bot_pool_json_missing_file_keeps_active(void);
int run_bot_pool_wire_roundtrip(void);
int run_bot_pool_wire_builtin_roundtrip(void);
int run_bot_pool_wire_rejects_garbage(void);
int run_proxy_meta_parse_roundtrip(void);
int run_proxy_meta_parse_truncation_safe(void);
int run_proxy_meta_parse_oversized_name(void);
int run_proxy_meta_parse_prefs_clamps(void);
int run_tick_core_active_local_no_double_step(void);
int run_tick_core_passive_local_pumps_keys_half(void);
int run_tick_core_lobby_preserves_parity(void);
int run_transport_ticks_server_lifecycle(void);
int run_await_join_connected_immediate(void);
int run_await_join_lobby_latch(void);
int run_await_join_timeout_and_error(void);
int run_bot_pool_wire_chunk_transport(void);
int run_lobby_map_list_chunked(void);
/* The scripted byte on a map-list entry: the layout by hand-written bytes,
 * and a list carrying a mix of scripted and plain maps. */
int run_lobby_map_list_scripted_golden(void);
int run_lobby_map_list_scripted_mixed(void);

/* The lobby's reload request (test_lobby_reload_scenario.c): who may ask,
 * when, how often, and what a lobby with no scenario answers. */
int run_lobby_reload_scenario_needs_host(void);
int run_lobby_reload_scenario_needs_lobby(void);
int run_lobby_reload_scenario_no_scenario(void);
int run_lobby_reload_scenario_calls_back(void);
int run_lobby_reload_scenario_cooldown(void);

/* The lobby's scenario pick (test_lobby_set_scenario.c): who may pick, when,
 * which names are accepted, that a refusal leaves the previous pick alone,
 * and the tick gap between one pick and the next. */
int run_lobby_set_scenario_selects(void);
int run_lobby_set_scenario_none(void);
int run_lobby_set_scenario_refuses_unknown(void);
int run_lobby_set_scenario_refuses_shape(void);
int run_lobby_set_scenario_refuses_bound(void);
int run_lobby_set_scenario_cooldown(void);
int run_lobby_set_scenario_unreadies(void);

/* The lobby's ordered script list (test_lobby_script_list.c): one scenario
 * deciding the round and mods behind it. The chunked control event and the
 * whole-list command against hand-written bytes, the dispatcher's gates, and
 * the chunks reassembled into the accessors a chooser reads. */
int run_script_list_control_codec(void);
int run_script_list_command_codec(void);
int run_script_list_dispatch(void);
int run_script_list_lists_once(void);

/* test_scenario_settings.c: a script's own lobby settings, from the
 * declaration through the wire to game.setting and Survival. */
int run_scenario_settings_blob(void);
int run_scenario_settings_manifest_lua(void);
int run_scenario_settings_manifest_json(void);
int run_scenario_settings_server_clamp(void);
int run_scenario_settings_codec(void);
int run_scenario_settings_client_apply(void);
int run_scenario_settings_game_setting(void);
int run_scenario_settings_wire(void);
int run_scenario_settings_survival_decl(void);
int run_scenario_settings_survival_short(void);
int run_script_list_client_apply(void);
int run_lobby_map_search_chunked(void);
int run_wbn_bearer_state(void);
int run_wbn_rekey_codec(void);
int run_wbn_news_parse(void);

/* News fetch lifetime regression (test_wbn_news_fetch.cpp): freeing the
 * fetch handle while the worker thread is still parked in the network GET
 * must be safe. Reproduces the field crash where newsPopupShutdown deleted
 * the fetch out from under the worker's terminal lock (a use-after-free that
 * crashed in mtx_do_lock). Deterministically flagged under -DENABLE_ASAN=ON
 * on the pre-fix code; clean once the state is shared-owned. */
int run_wbn_news_free_during_fetch(void);

int run_wbn_country_cache(void);
int run_wbn_prefs_parse_get(void);
int run_wbn_prefs_parse_updatedat(void);
int run_wbn_prefs_decide(void);
int run_wbn_prefs_build_put_body(void);
int run_wbn_serverlist_parse(void);
int run_wbn_serverlist_players(void);
int run_wbn_serverlist_motd(void);
int run_wbn_serverlist_malformed(void);
int run_wbn_map_parse(void);
int run_brain_crash_log_writes_file(void);
int run_brain_crash_log_falls_back_to_luaptr(void);
int run_brain_inbox_push_peek_fifo(void);
int run_brain_inbox_overflow_drops_oldest(void);
int run_brain_inbox_legacy_drain_fifo(void);
int run_brain_inbox_clear_resets(void);
int run_brain_list_scan_path_resolves(void);
int run_brain_list_texts_read(void);
int run_brain_docs_compress_roundtrip(void);

/* The two test rosters — test_main.c's dispatch table and CMakeLists.txt's
 * _unit_test_names — say the same thing, so a case added to one and not the
 * other is found rather than silently never run. */
int run_unit_test_names_match_cmake(void);

/* A brain's mode manifest (test_brain_modes.c): brains/<brain>/modes.txt,
 * the API by which a brain tells the lobby which modes it has and which
 * difficulty levels each of them offers. Happy path, the synthesized
 * fallback when there is no manifest, malformed lines being skipped rather
 * than fatal, and the two fixed-size caps. */
int run_brain_modes_manifest_parses(void);
int run_brain_modes_missing_falls_back(void);
int run_brain_modes_malformed_lines_skipped(void);
int run_brain_modes_counts_clamped(void);

/* The brain's terrain window at the map edge (test_brain_view_data.c): the
 * rect brainDataMakeInfo builds is inclusive, so a tank on row or column 240
 * and beyond is described out to 255. BYTE loop counters never finished such a
 * fill, which left a joining client spinning inside its first brain pass. */
int run_brain_view_data_edge_rect(void);
int run_bolo_rand_golden_sequence(void);

/* Pathfinder diagonal corner-cut rule (test_pf_corner_cut.c): the nav
 * Dijkstra edge cache, the A*, and cost_to must all refuse an on-foot
 * diagonal step past a solid (building/halfbuilding) cardinal corner —
 * a full-tile tank cannot make that move. Regression for the field
 * incident where the Dijkstra's stale BOTH-corners rule planned a
 * diagonal past a halfbuilding and the tank ground the corner for
 * ~110 ticks (20260713_233008_1 bot1 t=11467). */
int run_pf_dijkstra_no_solid_corner_cut(void);
int run_pf_astar_no_solid_corner_cut(void);
int run_pf_costto_no_solid_corner_cut(void);
int run_pf_tail_reaches_radius_and_stops(void);
int run_pf_tail_blocked_by_wall_and_sea(void);
int run_pf_tail_neutral_zone_slows(void);
int run_pf_tail_meeting_cancels_tie_to_hostile(void);
int run_pf_tail_never_overwrites_a_stamp(void);
int run_pf_tail_deep_margin_keeps_off_the_shore(void);
int run_pf_tail_contact_makes_a_front_line(void);

int run_loadbrowser_segment_walks_parts(void);
int run_loadbrowser_segment_crosses_blocks(void);
int run_loadbrowser_segment_no_neighbour(void);
int run_loadbrowser_segment_path_forms(void);
int run_loadbrowser_segment_labels(void);

int run_loadbrowser_rename_splits_names(void);
int run_loadbrowser_rename_family_plan(void);
int run_loadbrowser_rename_rejections(void);
int run_loadbrowser_rename_applies(void);

int run_net_impair(void);

/* ChannelMux reliability primitive (test_channel_mux.c): the full
 * off-socket loss/reorder/dup/window/boundary matrix over two ChannelMux
 * instances and an in-test frame shuttle. No sockets, no threads. */
int run_channel_mux(void);

/* The control channel's queue behind its window (test_channel_mux.c): sends
 * past the window are taken and delivered in order, the queue is bounded,
 * the game channel still refuses at its window, and a send reset drops the
 * queue. */
int run_channel_mux_control_queues_past_window(void);
int run_channel_mux_control_backlog_delivers_in_order(void);
int run_channel_mux_control_backlog_full_refuses(void);
int run_channel_mux_game_channel_still_refuses_at_window(void);
int run_channel_mux_control_reset_clears_backlog(void);

/* Spectator delayed-stream ring (test_spectator_ring.c): segmentation,
 * keyframe-at-segment-start, mid-interval seek + replay, segment isolation,
 * previous-generation read, cold start and the retention window boundary,
 * proven with synthetic byte payloads. */
int run_spectator_ring(void);

/* Spectator ring records without a .wbv log (test_spectator_ring_nolog.c):
 * a registered ring driven by serverSimTick (logWriteTick) populates and a
 * delay-0 seek returns a seed, with no logStart and no .wbv file. */
int run_spectator_ring_nolog(void);
/* And a log_ServerTick every FULL_SYNC_INTERVAL ticks in that ring's events,
 * with no snapshot ever written to a .wbv. */
int run_spectator_ring_nolog_tick_anchors(void);

/* Spectator replay translator (test_spectator_replay.c): specReplayWriteHeader
 * lays out the v2 header field-for-field, specReplayTranslateEvents /
 * specReplayTranslateKeyframe frame ring records into the LOG_* byte stream,
 * and a real translated keyframe decodes through lv_screenLoadFromStream. */
int run_spectator_replay(void);

/* Spectator config plumbing (test_spectator_config.c): -maxspectators /
 * -specdelay flow through ServerInstanceConfig into the sim and back via the
 * getters, with seconds->ticks conversion and the no-fallback zero semantics. */
int run_spectator_config(void);

/* Spectator JOIN handshake (test_spectator_join.c): a JOIN_FLAG_SPECTATOR
 * join registers a tankless viewer (slot 0xFF, no tank slot), honours the
 * maxSpectators cap / disabled case, and resolves over the cookie handshake. */
int run_spectator_join(void);

/* Spectator roster publish (test_spectator_roster_publish.c): the server
 * transport broadcasts CTRL_SPECTATOR_SLOT to player clients on spectator
 * join/leave and sends the full roster to a newly-joined player (catch-up),
 * verified through the real client apply path via clientSimGetSpectatorSlot. */
int run_spectator_roster_publish(void);

/* CTRL_SPECTATOR_SLOT body-codec round-trip (test_spectator_slot_codec.c):
 * the per-spectator roster event encodes/decodes through the body tables,
 * with the disconnected minimum and the out-of-range specIdx rejection. */
int run_spectator_slot_codec(void);

/* CTRL_SPECTATOR_CHAT body-codec round-trip (test_spectator_chat_codec.c):
 * the spectator lobby-chat event encodes/decodes through the body tables, with
 * the empty/max-length messages and the out-of-range specIdx / overrun
 * rejections. */
int run_spectator_chat_codec(void);

/* CTRL_STATS_SEED body-codec round-trip (test_stats_seed_codec.c): the
 * mid-round live-scoreboard seed encodes/decodes through the body tables, with
 * a full MAX_TANKS roster, the zero-row minimum, and the short / truncated /
 * out-of-range-slot rejections plus the over-count clamp. */
int run_stats_seed_codec(void);

/* Spectator-chat routing (test_spectator_chat_routing.c): a published
 * CTRL_SPECTATOR_CHAT reaches a player bus subscriber and a live spectator
 * (allowlist); broadcast CTRL_CHAT reaches the spectator; team/unicast chat
 * does not. */
int run_spectator_chat_routing(void);

/* Spectator-chat replay emission (test_spectator_chat_log.c):
 * serverSimReceiveSpectatorChat writes a log_SpectatorChat (specIdx + message)
 * into the recording .wbv, read back from the framed stream. */
int run_spectator_chat_log(void);

/* Lobby-chat catch-up buffer (test_spectator_chat_catchup.c): broadcast +
 * spectator chat captured in lobby/countdown, cleared at game start, capped,
 * and re-delivered by serverSimReplayLobbyChat at the drain-flip; a fresh
 * subscriber's sync replay carries no backlog. */
int run_spectator_chat_catchup(void);

/* Spectator connect — client side (test_spectator_connect.c): the real client
 * transport connecting with the spectator flag runs the join handshake and
 * lands in CLIENT_CONNECT_SPECTATING (tankless, awaiting seed) with no tank
 * slot consumed and no map download started. */
int run_spectator_connect(void);

/* Spectator live-lobby control intake — client side
 * (test_spectator_client_lobby_intake.c): a tankless spectator (myPlayerNum 0,
 * an alias of real slot 0) is fed the allowlisted lobby control bus and must
 * populate its mirror without mis-treating slot 0 as the viewer's own self —
 * the slot-0 name resolves, slot-0 broadcast chat is delivered, the viewer
 * holds no map-skip vote, and no team-scoped chat is shown to the teamless
 * viewer. */
int run_spectator_client_lobby_intake(void);

/* Spectator command rejection (test_spectator_command_reject.c): a tankless
 * viewer attempts lobby/gameplay commands the production way and the server
 * shows no effect (no slot, still a spectator); plus the dispatcher guard
 * rejects an out-of-range sender slot with CMD_REJECT_INVALID. */
int run_spectator_command_reject(void);

/* Spectator lobby read-only invariant (test_spectator_lobby_readonly.c): a
 * tankless viewer fires every guarded clientSimNetSend* mutation path and the
 * seated lobby is wholly untouched (no player added/removed, slot-0 team/ready/
 * name unchanged, viewer never promoted) — the client-side send-suppression
 * backstop. A positive control confirms the mirror still follows real changes.
 * The dialog's read-only rendering is human-validated; this covers the wire. */
int run_spectator_lobby_readonly(void);

/* Spectator feed capture — client side (test_spectator_capture.c): a real
 * spectator client over loopback captures the CHANNEL_BULK seed and the ordered
 * forward records into the ClientSim feed (record header stripped, order kept),
 * also exercising the PACKET_CHANNEL ack-emitter gate fix end-to-end. */
int run_spectator_capture(void);

/* Spectator replay-log events (test_spectator_log.c): serialize round-trip of
 * log_SpectatorJoined / log_SpectatorLeft / log_SpectatorChat over the v2
 * framed .wbv stream, plus loopback emission proving a spectator join/timeout
 * writes the Joined/Left kinds into a recording log. */
int run_spectator_log(void);

/* Spectator seed transfer (test_spectator_seed.c): a connected spectator on a
 * server whose ring is recording gets the delayed keyframe at head - delay
 * armed as a BULK_KIND_SPEC_SEED transfer; the armed seed blob is byte-equal to
 * the ring's keyframe at that delay. */
int run_spectator_seed(void);

/* Live-lobby spectator control bus (test_spectator_live_bus.c): a lobby
 * spectator is a real-time control-bus subscriber. The sync replay reaches a
 * lobby subscriber (and a seated spectator); serverSpectatorDeliverControl is a
 * drop-by-default allowlist; MAX_TANKS+MAX_SPECTATORS subscribers all register
 * (overflow returns INVALID); the enumerator seeds the spectator roster and a
 * roster broadcast fans CTRL_SPECTATOR_SLOT to live spectators. */
int run_spectator_lobby_subscribe(void);
int run_spectator_control_filter(void);
int run_spectator_subscriber_capacity(void);
int run_spectator_roster_to_spectators(void);

/* Live↔delayed cutover (test_spectator_cutover.c): a live-lobby spectator is
 * unsubscribed from the control bus at game start before any running-state
 * publish (so it receives zero running control — the anti-cheat boundary), and
 * is flipped back to the live lobby only after its delayed read head drains the
 * game→lobby segment boundary. */
int run_spectator_lobby_cutover(void);

/* Dual-mode spectator session (test_loopback_spectator_lobby.c): the client's
 * live-lobby bit is seeded from the accept mode byte (live when the server is in
 * the lobby, delayed when a game runs), leaves live-lobby mode when the delayed
 * feed begins at game start, and returns to it when live lobby control resumes
 * after the delayed game drains back to the lobby. */
int run_loopback_spectator_lobby(void);

/* Bulk-transfer framing (test_bulk_transfer.c): the off-socket stream-header
 * round-trip, byte-identical blob reassembly under loss + reorder, header
 * robustness, pipelining and the send-side serializer guard. */
int run_bulk_transfer(void);

/* Round-log transfer (test_round_log.c): the BULK_KIND_ROUND_LOG stream
 * header round-trips at the extremes the transfer uses (kind 8, a reqSeq echo
 * in gen, a totalSize at ROUND_LOG_MAX_BYTES, a path at BULK_PATH_MAX) and
 * refuses a truncated one without over-reading; a rejected round-log stream
 * leaves the byte run aligned, proved on the stream that follows it; and a
 * client asking a loopback server with no RoundLogSource registered lands on
 * CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED over the real REQ/ERR packets. The
 * success path needs a recorded round and a recorder in the server, so it is
 * human-gated on two machines rather than covered here. */
int run_round_log_header_roundtrip(void);
int run_round_log_sink_rejection_realigns(void);
int run_round_log_refused_when_unavailable(void);

/* Send-side overflow guards (test_overflow_guards.c): channelStreamSend and
 * bulkSenderBegin reject a wrap-prone length via the existing false path with
 * no state change. */
int run_overflow_guards(void);

/* CTRL_CHANNEL_RESET — the game-start baseline reset (test_channel_reset.c):
 * the body-codec round-trip and the per-client baseline values
 * transportUdpServerOnGameStart sends across two fabricated slots. */
int run_channel_reset_codec_roundtrip(void);
int run_channel_reset_two_slot_baselines(void);

/* Lobby runtime fixes (test_lobby_runtime_fixes.c). */
int run_countdown_abort_publishes_phase(void);
int run_lobby_auto_unready_clears_humans_keeps_bots(void);
int run_mapdir_root_fallback(void);

/* Bot chat routing (test_bot_chat_routing.c) — exercises serverSimApplyCommand
 * with a bot's slot as senderSlot and inspects the recipient ClientSim's
 * brain inbox. Confirms bots send and receive through the same CMD_CHAT
 * dispatcher arm + CTRL_CHAT subscriber funnel as humans. */
int run_bot_chat_send_to_human_lands_in_human_inbox(void);
int run_bot_chat_send_to_other_bot_lands_in_recipient_inbox(void);
int run_bot_chat_receive_from_human_lands_in_bot_inbox(void);
int run_bot_chat_receive_from_other_bot_via_broadcast(void);

/* The delivery rule in front of a hosted bot's inbox: an enemy's broadcast is
 * chatter, not an order, and a line aimed at the seat lands whoever sent it. */
int run_bot_chat_enemy_broadcast_is_not_an_order(void);
int run_bot_chat_enemy_unicast_still_lands(void);
/* A bot seated mid-round has an empty own row in its client-side player
 * table, so the rule reads the bound server's alliance matrix instead. */
int run_bot_chat_mid_round_bot_reads_server_alliances(void);

/* A bot placing its own smart ping (test_bot_ping.c) — a brain's ping
 * request becomes a CMD_PING from the bot's own player slot, and the bot's
 * own rate limit drops the extras. */
int run_bot_ping_queue_becomes_cmd_ping_from_bot_slot(void);
int run_bot_ping_rate_limit_drops_extras(void);
int run_bot_ping_ignores_bad_slot(void);
int run_bot_ping_ignores_a_slot_that_is_not_a_bot(void);

/* Internal brain-message routing (test_brain_internal_msg_routing.c) —
 * pins botManagerDeliverInternalMessage: bot brains sending with
 * messagedest=0 fan out into allied bot inboxes only, never the chat
 * wire, never a human's newswire. */
int run_internal_msg_reaches_allied_bot(void);
int run_internal_msg_skips_sender_self(void);
int run_internal_msg_skips_non_allied_bot(void);
int run_internal_msg_skips_inactive_bot_slot(void);
int run_internal_msg_handles_oversized_body(void);
int run_internal_msg_null_inputs_are_noop(void);

/* Subscriber-arm orphan regressions (test_subscriber_arm_orphans.c).
 * Each captures a side effect that was orphaned when its standalone
 * PACKET_* handler was superseded by the unified control bus. */
int run_alliance_request_flags_addressed(void);
int run_alliance_request_ignores_other_slot(void);
int run_phase_lobby_sets_inlobby(void);
int run_phase_gameover_resets_in_game(void);
int run_player_join_appends_lobby_chat(void);
int run_player_join_self_does_not_announce(void);
int run_player_leave_appends_lobby_chat(void);
int run_lobby_settings_clears_balance_proposal(void);

/* Identity-survives-reset regressions (test_identity_survives_reset.c).
 * Connection identity (name, country, clientType, clientFlags incl.
 * PLAYER_FLAG_BOT) must survive the per-round world reset; it was twice
 * dropped on the networked game-start path. */
int run_reset_round_state_preserves_identity(void);
int run_start_game_preserves_client_flags(void);
int run_return_to_lobby_drops_wbn_keeps_identity(void);

/* WBN session-rotation guard (test_wbn_session_rotation.c). The next
 * round's map must never be reported to WBN while the just-finished
 * round's server_key is still live. */
int run_wbn_lobby_update_deferred_during_rotation(void);
int run_wbn_lobby_update_sends_when_not_rotating(void);

/* No-lobby map-rotation round restart (test_maprotate_rotation.c). The
 * restart re-arms the empty-server check and opens the WBN session-rotation
 * window so the next round's map is never reported on the old server_key. */
int run_maprotate_restarts_round_and_rearms(void);
int run_maprotate_defers_wbn_update_until_key_rotated(void);
int run_maprotate_gameover_is_not_terminal(void);
int run_maprotate_boot_does_not_rotate_while_empty(void);
int run_maprotate_vote_return_is_not_terminal(void);

/* Auto-close on an empty server (test_autoclose_empty_server.c). -autoclose
 * closes the server once everyone has left, so the latch behind it must read
 * only real arrivals — neither the no-lobby boot's own round start nor a bot
 * whose runner fails to build may leave it set on a server nobody joined. */
int run_autoclose_boot_does_not_close_while_empty(void);
int run_autoclose_closes_after_last_player_leaves(void);
int run_autoclose_lobby_boot_waits_for_a_joiner(void);
int run_autoclose_failed_bot_add_leaves_check_armed(void);

/* Round transition off the tick thread (test_round_transition_tick.c). The
 * boundary queues server/quit, the round-log upload and server/register for
 * the WinBolo.net worker and returns; the rekey and the rotation window wait
 * for the register's reply. */
int run_round_transition_tick_does_not_block(void);
int run_round_transition_during_rotation_deferred(void);
int run_round_transition_worker_down_sends_inline(void);

/* Re-authentication off the tick thread (test_reauth_result.c). The reauth
 * captures the slot and queues client/verify; the reply stamps the slot on a
 * later tick, and one whose slot was reused meanwhile is dropped. */
int run_reauth_result_after_slot_reuse_discarded(void);
int run_reauth_result_stamps_slot(void);
int run_reauth_repeat_while_verify_out_refused(void);
/* And the anonymous PLAYER_JOIN fallback's side of it: the sweep defers to an
 * outstanding verify, because the two announcements name different things,
 * and the hold lapses so a lost reply cannot suppress the join for good. */
int run_reauth_result_outruns_grace_stamps_once(void);
int run_reauth_result_failure_releases_anonymous_join(void);
int run_reauth_result_lost_still_announces(void);
/* And the web slot's join code, the last WinBolo.net call that ran on the
 * tick: resolved off it, cached and stamped when the reply lands, dropped
 * when the slot was reused meanwhile. */
int run_reauth_web_code_verified_off_tick(void);
int run_reauth_web_code_after_slot_reuse_discarded(void);
int run_reauth_web_code_guest_stays_anonymous(void);

/* Deferred WBN PLAYER_JOIN core (test_wbn_deferred_join.c). The join
 * event is held until the slot's identity is known for the session —
 * keyed on reauth, anonymous on grace expiry — and re-fires per round.
 * Also pins the rotation rekey gate (verified-flag, not the wiped key). */
int run_wbn_join_keyed_on_reauth(void);
int run_wbn_join_anonymous_on_grace(void);
int run_wbn_join_idempotent_reauth_no_double(void);
int run_wbn_join_disconnect_drops(void);
int run_wbn_join_rearm_per_session(void);
int run_wbn_join_rekey_target_gate(void);

/* JOIN name-collision verdict core (test_join_collision.c). When an
 * incoming joiner's name matches a connected slot, a verified slot always
 * wins; an unverified slot is rejected as in-use unless the joiner claims
 * it will authenticate, which downgrades the reject to a provisional
 * admit. The client-asserted will-auth flag never displaces a verified
 * slot. */
int run_join_collision_unverified_no_auth_rejects(void);
int run_join_collision_unverified_will_auth_admits(void);
int run_join_collision_verified_rejects_will_auth(void);
int run_join_collision_verified_flag_irrelevant(void);

/* Reauth-time claim-resolve core (test_join_collision.c). Given whether the
 * desired bare name is held and the holder's verified flag, pick promote-
 * free / preempt-squatter / keep-temp; an absent holder collapses to
 * promote-free regardless of the flag. */
int run_claim_resolve_free_promotes(void);
int run_claim_resolve_unverified_preempts(void);
int run_claim_resolve_verified_keeps_temp(void);
int run_claim_resolve_free_ignores_holder_flag(void);

/* Lobby/leave cleanup (test_lobby_reset_cleanup.c). Removed slots clear
 * (no phantom re-announce), and the last human leaving a running game
 * returns the server to the lobby. */
int run_base_timer_cleared_on_leave(void);
int run_base_timer_not_inherited_next_round(void);
int run_remove_player_clears_slot(void);
int run_return_to_lobby_clears_phantom_slot(void);
int run_last_human_leave_returns_to_lobby(void);
int run_empty_return_to_lobby_removes_bots_and_unlocks(void);
int run_humanless_round_does_not_autoend(void);
int run_host_departs_promotes_lowest_human(void);
int run_nonhost_departs_keeps_host(void);
int run_host_reassign_skips_bots(void);
int run_lobby_reset_clears_host_slot(void);

/* Auto-lock on game start (test_autolock_on_game_start.c). The lobby stays
 * open to joiners, but the round start closes the join gate and the
 * transport lock and reports the lock to WinBolo.net. */
int run_autolock_lobby_stays_open(void);
int run_autolock_locks_on_start_wire(void);
int run_autolock_locks_on_start_inplace(void);
int run_autolock_join_blocked_after_start(void);
int run_no_autolock_stays_open_on_start(void);
int run_autolock_released_on_return_to_lobby(void);

/* Tree-growth water regressions (test_treegrow_no_sea.c). A converged
 * grow target survives serverSimResetGameWorld and, on the next map,
 * points at open sea; the grow gate also failed to reject DEEP_SEA. */
int run_treegrow_never_plants_on_deep_sea(void);
int run_treegrow_reset_clears_stale_target(void);

/* Pillbox repair load (test_pill_repair_load.c). The man carries a full
 * load and spends it against the armour the pill has when he arrives, so a
 * pill that took more fire on the way still ends up topped up, and the
 * trees he didn't need come back to the tank. */
int run_pill_repair_tops_up_from_arrival_armour(void);
int run_pill_repair_short_load_spends_what_it_has(void);
int run_pill_repair_full_load_covers_a_dead_pill(void);

/* Entity lifecycle (test_entity_lifecycle.c). Pillboxes, bases and starts
 * carry an active flag: removal is a tombstone that keeps the slot, the
 * count and every index above it, and addition takes the lowest removed
 * slot before it extends the list. */
int run_entity_removed_pill_is_gone_from_gameplay(void);
int run_entity_removed_base_is_gone_from_gameplay(void);
int run_entity_removed_start_is_never_chosen(void);
int run_entity_tournament_removed_start_is_never_chosen(void);
int run_entity_tournament_neutral_share_counts_live_bases(void);
int run_entity_add_reuses_lowest_removed_slot(void);
int run_entity_add_refused_when_full(void);
int run_entity_remove_refuses_already_removed(void);
int run_entity_remove_keeps_indices_above(void);
int run_entity_blob_load_marks_every_item_live(void);

/* The entity-change control event (test_entity_event.c). CTRL_ENTITY_CHANGE
 * carries the kind, the item's 0-based index, whether it is now on the map,
 * and the item's map record — never the server's per-tick working state. */
int run_entity_event_codec_roundtrip(void);
int run_entity_event_client_adds_at_fresh_index(void);
int run_entity_event_client_add_reuses_removed_slot(void);
int run_entity_event_client_remove_keeps_the_slot(void);
int run_entity_event_client_add_lands_on_the_server_index(void);
int run_entity_event_removed_index_sends_no_delta(void);
int run_entity_event_wire_corpus_fixture(void);
int run_entity_event_client_refuses_out_of_range_index(void);

/* What an entity change reaches (test_entity_record.c). The six arms write a
 * log_EntityChange beside the CTRL_ENTITY_CHANGE they publish, carrying the
 * same kind, index and map record, because a control event never reaches the
 * .wbv. The viewer reads it back, a brain's view rebuilds without the removed
 * item, and the lobby's counts go out again. */
int run_entity_record_round_trip_per_kind(void);
int run_entity_record_viewer_across_a_snapshot(void);
int run_entity_record_removed_and_restored_pill_replays(void);
int run_entity_record_brain_rect_skips_a_removed_pill(void);
int run_entity_record_lobby_add_republishes_counts(void);
/* And what only log_EntityMasks can say: a removal that happened before the
 * recording's first snapshot, and a decode that starts at a mid-round
 * snapshot with every record before it unread. */
int run_entity_record_lobby_removal_replays(void);
int run_entity_record_seek_lands_on_the_right_liveness(void);

/* The entity-sync control event (test_entity_sync.c). CTRL_ENTITY_SYNC
 * carries three 16-bit masks, one per item list, saying which indices hold
 * an item that is on the map — the part the compressed map blob leaves out,
 * sent to a client once it holds the blob. */
int run_entity_sync_codec_roundtrip(void);
int run_entity_sync_client_clears_the_holes(void);
int run_entity_sync_client_restores_the_live(void);
int run_entity_sync_mask_above_the_count_ignored(void);
int run_entity_sync_loopback_join(void);
int run_entity_sync_wire_corpus_fixture(void);

/* Incremental start-picker (test_starts_pick_incremental.c). The one-slot
 * cluster / farthest-first selection that auto-assigns a lobby start on
 * join, shared with startsAssignBatch's distance + validity logic. */
int run_starts_pick_cluster_nearest_teammate(void);
int run_starts_pick_farthest_when_solo(void);
int run_starts_pick_none_when_all_taken(void);
int run_starts_pick_spreads_on_side(void);
int run_starts_pick_unsided_takes_far_side(void);

/* Batch start-assignment reservations (test_starts_assign_batch.c). The
 * reservedStartIdx0 lock that honors lobby start picks at game start:
 * reserved slots land exactly, unreserved slots fill the rest, stale and
 * duplicate reservations degrade to ordinary placement, NULL is a no-op. */
int run_starts_batch_reserved_lands_exact(void);
int run_starts_batch_unreserved_avoids_reserved(void);
int run_starts_batch_stale_reservation_falls_through(void);
int run_starts_batch_duplicate_honors_first(void);
int run_starts_batch_null_reservations_place_normally(void);
/* Randomized solo placement, team clustering, and unanchored-team anchor
 * jitter in startsAssignBatch; friendly-pill "ideal" rule in the open path. */
int run_starts_batch_solo_random_seed(void);
int run_starts_batch_teams_cluster_and_separate(void);
int run_starts_batch_team_anchor_jitter_varies(void);
int run_starts_open_ideal_friendly_pill_eligible(void);
int run_starts_open_ideal_removed_pill_ignored(void);
int run_starts_open_removed_start_never_chosen(void);
/* Starts in the mined border come off the map at load (test_starts_border.c). */
int run_starts_border_start_dropped(void);
int run_starts_border_all_border_kept(void);
int run_starts_border_compressed_load_agrees(void);
int run_starts_border_named_inactive_safe(void);
int run_starts_border_harvard_yard(void);
/* Pillboxes and bases in the mined border, the same rule per kind. */
int run_pills_border_pill_dropped(void);
int run_bases_border_base_dropped(void);
int run_items_border_all_border_kept(void);
int run_items_border_compressed_load_agrees(void);
int run_items_border_game_ignores(void);
int run_pills_border_old_tutorial(void);

/* Start side classification (test_start_sides.c). The integer sector test
 * in start_sides.h that puts a start on N/E/S/W (two bits for a diagonal,
 * none for the centre band), and the eligibility rule that keeps a team
 * with no side off the sides other teams chose. */
int run_start_side_mask_sectors(void);
int run_start_side_mask_degenerate_bbox(void);
int run_start_side_eligible_closed_mask(void);

/* Lobby map-preview compass axis (test_lobby_side_axis.c). The pure
 * helpers in lobby_side_axis.h behind the preview's N/S · E/W rose:
 * exactly two populated teams, which axis a pair of team sides forms,
 * and what a click on an axis has to send. */
int run_lobby_side_axis_two_team_pair(void);
int run_lobby_side_axis_pair_classification(void);
int run_lobby_side_axis_click_targets(void);

/* Shared lobby starts (test_lobby_start_shared.c). The pure helpers in
 * lobby_start_shared.h behind a start several players reserve: the
 * minimal unique name prefixes, the comma-joined holder labels the mini
 * map and the tooltips show, and the one ownership class a start with
 * several holders reads as. */
int run_lobby_start_shared_prefix_len(void);
int run_lobby_start_shared_prefix_label(void);
int run_lobby_start_shared_name_label(void);
int run_lobby_start_shared_owner_fold(void);

/* Spawn scatter separation (test_starts_scatter_separation.c). The
 * spiral in startsScatterFind keeps a new tank START_SPAWN_SEPARATION
 * squares from every other live tank, and drops that rule on a second
 * pass when no square within reach can satisfy it. */
int run_starts_scatter_avoids_existing_tanks(void);
int run_starts_scatter_falls_back_when_crowded(void);

/* Team start sides in startsAssignBatch (test_starts_team_side.c). A team
 * with a side stays on it, a team with none is kept off the chosen sides,
 * a quota is capped at what the side can hold, and the slots left over
 * ride a start beside their own side instead of going unplaced. */
int run_starts_side_team_stays_on_its_side(void);
int run_starts_side_overflow_rides_own_side(void);
int run_starts_side_never_crosses_when_opposite_free(void);
int run_starts_side_any_matches_legacy(void);
int run_starts_side_reservation_beats_side(void);
int run_starts_side_empty_side_falls_back(void);
int run_starts_side_quota_capped_by_eligible(void);
int run_starts_side_any_team_kept_off_chosen_side(void);
/* ...and it spreads across the side it chose rather than filling one
 * corner of it: one player per corner, an even split when the side has
 * more, inside the side's own starts, and shared properly when two teams
 * pick the same side. */
int run_starts_side_spreads_across_corners(void);
int run_starts_side_spread_balances_corners(void);
int run_starts_side_spread_keeps_its_tier(void);
int run_starts_side_spread_two_teams_one_side(void);
int run_starts_side_spread_unsided_team_confined(void);
int run_starts_side_unsided_team_takes_far_side(void);
int run_starts_side_shared_side_anchor_stays_on_side(void);

/* Team start sides on the lobby server (test_lobby_team_side_dispatch.c).
 * The team-meta side clamp and its client mirror, the re-pick of every
 * reservation on a side change, the Unassign and Team side claims, the
 * departure and map-change backfills, and the side check on a non-host's
 * own claim. */
int run_lobby_team_side_clamps_and_rejects_non_host(void);
int run_lobby_side_change_repicks_everyone(void);
int run_lobby_unassign_and_team_side_claims(void);
int run_lobby_map_change_releases_off_side(void);
int run_lobby_non_host_off_side_claim_rejected(void);
int run_lobby_any_team_claim_kept_off_chosen_side(void);

/* Team start sides end to end (test_starts_side_integration.c): lobby
 * reservations, the batch's side table and the spawn scatter put a north
 * team's tanks north and a larger south team's tanks south, on distinct
 * squares. */
int run_starts_side_end_to_end_four_v_twelve(void);
int run_starts_side_region_sweep(void);
int run_starts_side_unsided_team_kept_off_chosen_side(void);
int run_starts_side_two_team_lobby_mirrors(void);
int run_starts_side_team_change_repicks_stale(void);

/* CTRL_ALLIANCE_RESET batched alliance event (test_alliance_reset.c).
 * Replaces the O(N²) per-pair CTRL_ALLIANCE_ACCEPT burst that overflowed
 * the host's reliable control queue at game start with 16 players. */
int run_alliance_reset_codec_roundtrip(void);
int run_alliance_reset_decoder_rejects_short(void);
int run_alliance_reset_reapply_publishes_one_event(void);
int run_alliance_reset_apply_rebuilds_alliances(void);
/* Bot ClientSim alliance matrices agree with the server's after a game
 * start, on both start paths (bot re-arm before vs after the reapply). */
int run_alliance_reset_bots_synced_after_inplace_start(void);
int run_alliance_reset_bots_synced_after_countdown_start(void);
/* Applying a reset moves no pillbox and no base owner — ownership changes
 * hands on a departure, which arrives as CTRL_PLAYER_LEAVE. */
int run_alliance_reset_apply_keeps_owners(void);
int run_alliance_reset_changed_matrix_keeps_owners(void);
int run_alliance_reset_set_team_leaves_old_team(void);

/* Log replay round-trip (test_log_roundtrip.c). */
int run_log_roundtrip_basic(void);
int run_log_roundtrip_snapshot_keeps_chain_synced(void);
int run_log_roundtrip_lobby_snapshot_is_empty_world(void);
int run_log_roundtrip_lobby_mode_drops_world_events(void);
/* log_Ping's six payload bytes: sender, kind, and two big-endian u16 WORLD
 * coordinates, byte for byte through the writer and back. */
int run_log_roundtrip_ping(void);
/* A snapshot's player block ends with the tank's shells, mines, armour and
 * trees for a slot in use, and a slot that is not in use stays the 2-byte
 * stub. */
int run_log_roundtrip_snapshot_tank_stocks(void);
/* log_TankSetStock carries the player and the four values, and is written only
 * when one of them changed since the last record for that tank. */
int run_log_roundtrip_tank_stock_record(void);

/* Record-and-decode round trip (test_replay_roundtrip.c, on the
 * replay_harness fixture): records a round in which a terrain cell, a base's
 * owner and stock, a pillbox's owner and a tank's stocks all change, replays
 * the .wbv through the production viewer and requires the replayed world to
 * be the recorded one. Every change lands after the opening snapshot and the
 * round is far shorter than the interval between snapshots, so only the
 * per-change events can carry them. */
int run_replay_roundtrip_world(void);
/* Same fixture: a base moved by basesMigrate (its owner left the game) is on
 * the same base, with the same new owner, after replay. */
int run_replay_roundtrip_base_migrate(void);
int run_replay_roundtrip_base_damage(void);
int run_replay_roundtrip_base_capture_stock(void);
/* Same fixture: a pillbox's armour and square change together, so the health
 * record has two more records behind it in the same stream. A viewer that
 * takes the wrong number of bytes for the health record reads the ones after
 * it off their boundaries, which shows up as a pill on the wrong square rather
 * than as an armour that happens to match. */
int run_replay_roundtrip_pill_health(void);

/* Older recordings read by today's viewer (test_replay_version_compat.c):
 * hand-built v2 and v1 logs carrying log_PillSetHealth in its one-byte nibble
 * form, the shape every file before LOG_VERSION 3 holds. Each puts a
 * placement record for the same pillbox straight behind the health one, so a
 * reader that sized the health record by today's shape is caught by the
 * square the pillbox ends up on rather than by its armour. The v1 case also
 * covers the load's sizing walk, which has no framed length to fall back on. */
int run_replay_v2_pill_health_nibble(void);
int run_replay_v1_pill_health_nibble(void);

/* The brain recorder's own version (test_brainrec_version.c): a session file
 * written by an older build states a version this build cannot read, because
 * the frames are raw snapshot structs and the structs changed size. The check
 * every reader shares has to refuse it rather than walk it. */
int run_brainrec_version_rejects_old(void);

/* Viewer-side decode of per-tank stocks (test_lv_tank_stocks.c): hand-built
 * snapshot bodies and forward records through lv_specSeedLoad /
 * lv_specRecordPump. Covers the four bytes on the end of a player block, a
 * block written before they existed (no stocks, and the fields in front of them
 * still decode), a not-in-use slot, the log_TankSetStock record, and an unknown
 * record type skipped by its framed length. */
int run_lv_tank_stocks_from_snapshot(void);
int run_lv_tank_stocks_snapshot_without_tail(void);
int run_lv_tank_stocks_from_record(void);

/* Viewer palette slots follow alliance groups (same file). from_snapshot:
 * mutual allies listed as forward references in a snapshot share a slot, and
 * keyframes that split and restore the groups without alliance events keep the
 * slots stable. events: players who joined solo hold slot-order colours, the
 * round's first world snapshot deals the merged groups compact colours (two
 * teams are Team 1 and Team 2 wherever they sat), and later merges and splits
 * leave everyone who didn't move alone. */
int run_lv_team_colours_from_snapshot(void);
int run_lv_team_colours_events(void);

/* The viewer reading a recording it cannot trust (test_lv_hostile_records.c):
 * hand-built bytes through lv_specSeedLoad / lv_specRecordPump. A count byte
 * past the item arrays or past the block's length is clamped before it is
 * stored or walked; a log_EntityChange removal naming an index past the array
 * is refused for every kind; a server line to a slot past the roster reads as
 * an empty seat; a log_TankSetModifiers blob of the wrong length is consumed
 * by its length so the record behind it still decodes; a map run of
 * identical squares that would pass column 255 stops at it; a record whose
 * frame length disagrees with its fields is left at the frame's end. */
int run_lv_hostile_item_counts(void);
int run_lv_hostile_entity_remove_index(void);
int run_lv_hostile_server_text_slot(void);
int run_lv_hostile_modifiers_length(void);
int run_lv_hostile_map_run_edge(void);
int run_lv_hostile_frame_length(void);

/* .wbv reader gate (test_wbv_reader.c): loads the committed fixtures
 * through the production log-viewer reader (lv_screenLoadMapFromMemory)
 * and asserts the decode succeeds with the expected header/snapshot
 * content. v1 locks XOR'd back-compat; v2 covers the current plaintext,
 * length-framed format. The on-demand v2 capture (test_wbv_v2_capture.c)
 * drives the dedicated-server writer to (re)generate spectator_v2.wbv; it
 * is dispatch-only, never run under CTest. */
int run_wbv_reader_v1(void);
int run_wbv_reader_v2(void);
int run_attribution_reader_roundtrip(void);
int run_attribution_reader_rejects_bad(void);
int run_attribution_reader_clamps_slotcount(void);
int run_attribution_reader_old_wbv(void);
int run_wbv_v2_capture(void);

/* Recorded headless runs (test_wbv_fixture_summaries.c): each committed
 * tests/fixtures/wbv/<name>.wbv from a --record scenario is decoded through
 * the production viewer and its plain-text summary compared byte for byte
 * with <name>.summary. wbv_summary_capture rewrites those summaries and is
 * dispatch-only, never run under CTest. */
int run_wbv_fixture_summaries(void);
int run_wbv_summary_capture(void);

/* Highlight clip-time calibration anchors (test_lv_calibration.c): builds
 * synthetic v2 .wbv logs and asserts lv_walkFindBaseOwnerTimes resolves
 * base cells from the log's own snapshots — a lobby-started log's opening
 * snapshot carries no bases, so the table must come from the first
 * in-game snapshot (and from the loaded table on a no-lobby log). */
int run_lv_walk_base_anchor_lobby_log(void);
int run_lv_walk_base_anchor_opening_snapshot(void);
int run_lv_walk_base_anchor_ordinal(void);

/* Hide Lobby presentation window (test_lv_calibration.c): with the lobby
 * hidden the viewer's progress, seeks and clock run against
 * [gameStart, totalTime) while the decoder stays on absolute log ms. Game
 * start is the world rewrite that ends the lobby, so a log whose lobby left no
 * log_LobbyExit still opens on the round, and a log that never had a lobby —
 * periodic in-game snapshots and all — falls back to the whole file. */
int run_lv_hide_lobby_window_mapping(void);
int run_lv_hide_lobby_no_lobby_fallback(void);
int run_lv_hide_lobby_no_marker_anchor(void);
int run_lv_hide_lobby_periodic_snapshot(void);

/* Load-time slot-name walk (test_lv_calibration.c): a player who joins after
 * the lobby marker is absent from the live roster at game start, where the
 * round summary is built, so lv_screenGetLoggedPlayerName reads the name from
 * the log's own join event instead. */
int run_lv_logged_name_from_join_event(void);

/* Load-time settings walk (test_lv_calibration.c): the settings a round was
 * played under are the last log_GameSettings the file holds, so the walk
 * collects them and a replay of the lobby's earlier event does not put the
 * opening settings back. A log with no settings event reports none, including
 * one opened straight after a log that had them. */
int run_lv_game_settings_from_walk(void);
int run_lv_game_settings_absent(void);

/* Spectator ring-seed fixture generator (test_spectator_seed_capture.c):
 * dispatch-only. Captures a real ServerSim ring keyframe (no trailing data) and
 * writes it to <WB_WBV_FIXTURE_DIR>/spectator_seed.bin when the env var is set. */
int run_spectator_seed_capture(void);

/* Append-fed blocks stream source (test_blocks_stream.c): the no-zip path
 * where lv_blocksAppendBytes feeds plaintext bytes that the existing read/seek
 * machinery serves back, covering chunked append, read-back, seek and
 * end-of-stream. */
int run_blocks_stream(void);

/* Stream-load decode path (test_stream_load.c): loads the spectator_v2 fixture's
 * plaintext log.dat through lv_screenLoadFromStream and asserts it decodes the
 * same content the zip reader does. */
int run_stream_load(void);

/* Stream-pump path (test_stream_pump.c): feeds the spectator_v2 fixture's
 * plaintext log.dat through lv_screenStreamPump one record at a time, with a
 * caught-up pump (no new bytes) mid-stream, and asserts the chunked feed
 * reaches the same end-of-log state as a one-shot load and that a caught-up
 * pump neither advances the cursor nor finishes playback. */
int run_stream_pump(void);

/* Spectator seed load (test_spec_seed_load.c): fabricates a ring-shaped seed
 * blob from the spectator_v2 fixture's snapshot body, runs it through
 * lv_specSeedLoad (synthesize header -> translate keyframe -> load) and asserts
 * the decoder rebuilds the world and the control slice is stashed. */
int run_spec_seed_load(void);

/* Real spectator seed load (test_spec_seed_load_real.c): loads the committed
 * spectator_seed.bin (a real ring seed, no trailing data) through lv_specSeedLoad
 * and asserts TRUE. Currently FAILS — reproduces the lv_processSnapshot over-read. */
int run_spec_seed_load_real(void);

/* Spectator forward-record pump (test_spec_record_pump.c): seeds the decoder
 * from the spectator_v2 fixture's snapshot, then derives ring-shaped forward
 * records from the fixture's event stream and pumps them through
 * lv_specRecordPump, asserting the decoder advances to the same end state, an
 * empty tick advances cleanly, and a mid-stream keyframe re-syncs the decoder
 * and refreshes the stashed control slice. */
int run_spec_record_pump(void);

/* Headless spectator seed integration (test_spectator_seed_integration.c):
 * stands up a running ServerSim with a registered spectator ring (no logStart,
 * no .wbv) over the loopback transport, ticks until the connected spectator
 * client captures a seed, and loads it through the production decode path
 * (specDrainTakeSeed -> specSeedDecodeInfo -> lv_specSeedLoad), asserting TRUE. */
int run_spectator_seed_integration(void);

/* specSeedDecodeInfo decode (test_spec_seed_decode_info.c): captures a real ring
 * keyframe seed from a running sim (map "Everard Island", no logStart) and
 * asserts specSeedDecodeInfo recovers the map name / game type / hidden-mines /
 * ai from the control snapshot's lobby-settings event, plus the negative path
 * (empty control snapshot -> returns false, output zeroed). */
int run_spec_seed_decode_info(void);

/* Extracts the inner "log.dat" from a .wbv zip into a heap buffer (caller
 * frees). Defined in test_log_roundtrip.c; shared with test_stream_load.c. */
bool extractLogDat(const char *wbvPath, uint8_t **outBuf, size_t *outLen);

/* -log path composition (test_log_dir_path.c). Pins -log <dir> auto-naming
 * the replay inside the directory, vs -log <file> / bare -log. */
int run_log_path_empty_uses_autobase(void);
int run_log_path_explicit_file_verbatim(void);
int run_log_path_explicit_file_keeps_single_wbv(void);
int run_log_path_directory_autonames_inside(void);
int run_log_path_directory_trailing_slash_no_double(void);
int run_log_path_directory_arg_appends_wbv(void);

/* Input-redundancy invariants (test_input_redundancy.c): the constants
 * sizing the redundancy window, the InputPacket wire roundtrip + packet
 * size bound, and sim-level dedup of redundant duplicate ticks. */
int run_input_redundancy(void);

/* Edge-send predicate (test_edge_send.c): udpInputEdgeChanged flags a
 * button/action change as an edge that promotes a recorded input to an
 * immediate send, while ignoring the per-send tick/ACK/ping stamps. */
int run_edge_send_predicate(void);

/* Stall-advance (test_stall_advance.c): a stall-substituted tick is a
 * processed tick (lastProcessedInput advances), late inputs for it drop
 * as stale, and one-shot actions are harvested/laid exactly once under the
 * lastActionAppliedTick invariant. Always built (no WB_NETDEBUG gate). */
/* Base-visibility integration (test_base_stock_visibility.c): per-player
 * closest-base selection (enemy exclusion, in-range, post-flip inclusion) and
 * the per-recipient full-sync stock cull in serverSimBuildSnapshot (real stock
 * only for the recipient's own closest base; other bases zeroed, owner kept). */
/* Mic-status visibility (test_voice_flags.c): serverSimBuildSnapshot shows
 * PLAYER_FLAG_HAS_MIC / PLAYER_FLAG_VOICE_MUTED only to the recipients that
 * player's voice could reach — everyone outside a running game, allies
 * inside one — and masks nothing but PLAYER_VOICE_FLAG_MASK. */
int run_voice_flags_snapshot_masking(void);

/* CTRL_VOICE_TALKING body-codec round-trip (test_voice_talking_codec.c):
 * the talking bitmap encodes/decodes through the body tables — empty, one
 * bit, several bits and MAX_TANKS - 1 — and a short body is rejected. */
int run_voice_talking_codec(void);

/* CTRL_SIM_RULES body codec (test_sim_rules_codec.c): every carried rule
 * round-trips through the body tables, compared field by field and
 * including rates a fixed-point scale could not carry; and the body's bytes
 * against a layout written out by hand. */
int run_sim_rules_codec_roundtrip(void);
int run_sim_rules_codec_golden(void);

/* The lobby's talking set over the loopback transport (test_voice_talking_set.c),
 * read off the watching client's mirror of it: a talker who goes quiet after the
 * countdown has begun still ages out of the set (the silence is measured on a
 * clock that keeps running while the sim's tick does not), and the only talker
 * leaving empties the set on every other client rather than leaving them lit. */
int run_voice_talking_stops_in_countdown(void);
int run_voice_talking_clears_on_leave(void);

int run_bases_closest_for_player(void);
int run_base_stock_visibility(void);

/* The half-tick carry basesHalfTickCalulator keeps between calls
   (test_base_half_tick.c): one pair of fields per sim, not per process. */
int run_base_half_tick_per_sim_sequence(void);
int run_base_half_tick_other_sim_does_not_disturb(void);
int run_base_armour_fog_of_war(void);
int run_base_armour_reveal_in_range(void);
int run_two_clients_full_sync_independent(void);

/* Base-death prediction (test_base_death_prediction.c): the collision path
 * resolves a base our own predicted shell is about to kill against the tick
 * being replayed, so a reconciliation replay spanning the hit sees a wall
 * before it and open ground after. */
int run_base_death_prediction_replay_tick(void);
/* Authoritative armour settles the stamp by the server's processed input
 * tick: kept while still ahead and one hit from dead, dropped once disproved,
 * and a landing that was waiting on an earlier hit's armour is armed. */
int run_base_death_prediction_authority(void);
/* Gunsight reconciliation (test_gunsight_reconciliation.c): unacknowledged
 * range inputs replay on top of the server's snapshot value so the range does
 * not snap back. Covers acknowledgement, both tick parities, clamping to the
 * configured min and max, local-only visibility and a reused ring-buffer slot. */
int run_gunsight_reconciliation(void);

/* Tank destroyed state (test_tank_death_state.c): armour is a plain
 * 0..TANK_FULL_ARMOUR value that clamps at zero and the destroyed state is
 * stored on the tank, so a hit greater than the armour remaining destroys it
 * while a hit that exactly empties the armour leaves it alive at zero. A
 * destroyed tank still reads as destroyed after a snapshot round trip. */
/* A seat the roster holds with nobody on the field reports tankNone, so the
 * status strip draws no tank for it, and reports what it always did once a
 * wave fields it again. */
int run_tank_alliance_unfielded_shows_no_tank(void);
/* And playersSetPlayer, which works the status tile out for itself on a join
 * or a rename, blanks a held seat's tile the same way. */
int run_tank_alliance_unfielded_status_tile(void);

int run_tank_damage_exact_armour_survives(void);
int run_tank_damage_overkill_destroys(void);
int run_tank_damage_partial_survives(void);
int run_tank_destroyed_snapshot_round_trip(void);

/* The destroyed state on the wire (test_tank_status_wire.c): tankStatus
 * carries TANK_STATUS_DEAD (in the respawn wait) and TANK_STATUS_DESTROYED
 * (destroyed, not yet respawned) to every recipient, and armour is a plain
 * 0..TANK_FULL_ARMOUR value with no death sentinel. The round trip holds a
 * tank destroyed with its wait over — the state only the destroyed bit can
 * carry — and checks the client reads it as dead, then alive again. */
int run_tank_status_wire_bits(void);
int run_tank_status_wire_start_find_round_trip(void);

/* FX viewport cull (test_fx_viewport_cull.c): serverSimBuildViewports +
 * inAnyViewport cover the recipient's tank screen and each owned/allied
 * pillbox screen, so an fx near an owned pillbox but off the tank screen is
 * still visible (the snapshot and best-effort fx cull share this set). */
int run_fx_viewport_cull(void);

/* Viewport extent (test_fx_viewport_cull.c): a recipient's own tank rect
 * reaches SNAPSHOT_SCREEN_SIZE / 2 + SNAPSHOT_VIEWPORT_MARGIN squares on every
 * side and corner, covers the block the overview reveals round a tank, and
 * stops one square past the extent. */
int run_viewport_floor(void);

/* Sound events (test_sound_delivery.c): the three sound events round-trip
 * through packGameEvent / unpackGameEvent with their four-byte payload intact
 * and nothing past it; serverSimBuildSnapshot's sound block culls by distance
 * at SDIST_NONE, skips a recipient's own shot, sends bubbles only to the player
 * losing the ammo, sends a tank hit to the player hit at any range, and drops
 * manLayingMineNear once its tier is far; and the delivered payload carries a
 * tier and a compass direction for a human recipient — never the sound's map
 * square, in or out of its viewport rects — while a bot keeps the square, and
 * so does a local slot flagged through serverSimSetSoundSquares until the flag
 * is cleared. */
int run_sound_event_codec(void);
int run_sound_delivery_builder(void);
int run_sound_payload_shape(void);

/* Policy-driven viewport rects (test_view_policy_rects.c):
 * serverSimBuildViewports honours the per-category ViewPolicy — allied pills,
 * bases and tanks each grant a screen under always, nothing under off (and
 * nothing under key without a claim), and under decay only while the
 * recipient's proximity clock is unexpired — a dead allied pill grants
 * nothing, and a player with no tank keeps a rect at its last known position
 * instead of seeing the whole map. */
int run_view_rects_default_baseline(void);
int run_view_rects_always_base_ally(void);
int run_view_rects_off(void);
int run_view_rects_decay(void);
int run_view_rects_dead_player(void);

/* Per-recipient pill squares over the same rects (test_view_policy_rects.c): a
 * pill inside them reports its real square with the position-current bit set,
 * one outside every rect reports the square that recipient was last given with
 * the bit clear while its owner, armour and in-tank flag keep arriving, the
 * square corrects itself once the recipient's screen reaches it, the
 * EVENT_PILL_UPDATE the builder emits is rewritten the same way rather than
 * dropped, and an advantage brain's snapshot is exempt while a plain computer
 * player's is not. */
int run_view_pill_pos_current(void);
int run_view_pill_pos_reveal(void);
int run_view_pill_update_event_fogged(void);
int run_view_pill_pos_bot_advantage(void);

/* Server-side tree hide (test_tree_hide.c): serverSimBuildSnapshot ships a
 * tank standing in trees more than MIN_TREEHIDE_DIST from the recipient on
 * either axis as a TANK_SNAPSHOT_HIDDEN_FLAG stub — in full inside that
 * distance, in full just after it has fired, and in full for an ally while the
 * server's allies-in-trees option is on, never for an enemy. */
int run_tree_hide_enemy(void);
int run_tree_hide_ally_option(void);

/* Server-side LGM visibility (test_lgm_visibility.c): a man on the parachute,
 * and a man standing on open ground, are sent whatever the owning tank is
 * doing — so hiding a tank in trees must not take its man with it, and the
 * entry that carries the man must still not carry the tank's position. */
int run_lgm_visibility_tank_in_trees(void);
int run_lgm_visibility_tank_visible(void);

/* Client-reported view state (test_view_state.c): CMD_VIEW_STATE stores which
 * view a client is in, a viewPolicyKey category grants the claimed item a rect
 * beside the recipient's own tank screen while it still qualifies rather than
 * in place of it, and every claim the server cannot
 * honour is accepted and degraded to the tank view — on arrival, per tick as
 * the target stops qualifying, on the round reset, and when the player being
 * viewed through leaves. */
int run_view_state_key_grants_rect(void);
int run_view_state_bad_claims_degrade(void);
int run_view_state_invalidation(void);
int run_view_state_lifecycle(void);

/* Server-side ally picking (test_view_state.c): CMD_VIEW_CYCLE asks the server
 * which ally to watch, serverSimPickAlly answers from live state — next and
 * previous step in slot order and wrap, a dead ally, an un-allied player and
 * the sender itself are never offered, and viewPolicyOff / an expired
 * viewPolicyDecay clock leave nothing to watch. Every ally request is answered
 * with a CTRL_VIEW_TARGET carrying the request's `from` back; a non-ally kind
 * is answered with nothing. The four scroll directions each compare one
 * coordinate, and compare it strictly, with the nearest match winning and a
 * press with no origin stepping from the start instead. */
int run_view_cycle_pick_order(void);
int run_view_cycle_pick_policy(void);
int run_view_cycle_pick_direction(void);

/* CTRL_VIEW_TARGET body-codec round-trip (test_view_target_codec.c): the
 * server's answer to a view-cycle request encodes/decodes through the body
 * tables, with the nothing-to-watch case and the fixed body length. */
int run_view_target_codec(void);

/* Applying the server's answer (test_view_target_apply.c): a CTRL_VIEW_TARGET
 * for our own slot whose fromEcho matches the ally we are stepping from parks
 * the camera on the ally it names; an answer for another slot or echoing an
 * earlier press is dropped, and a not-found answer leaves the tank view in
 * place. Also the shape checks the arm makes before reading the answer — a
 * kind other than ALLY and a target off the end of the player table are both
 * dropped, while a not-found answer stands whatever its target byte holds. */
int run_view_target_apply(void);

/* Item-view cycling helpers (test_view_cycling.c): basesGetNextView and
 * playersGetNextAllyView walk the allied bases and the allied live tanks in
 * order and wrap, skipping enemy, neutral, dead and un-allied items and
 * reporting FALSE when nothing qualifies; basesMoveView and
 * playersMoveAllyView take the nearest item in the pressed direction on both
 * axes and never one that is only nearer the other way; and
 * viewportUpdateItemView drops the view once a base is captured or an ally
 * dies or leaves, while following an ally that is still driving. Under a
 * decay policy the cycling also steps over the items whose clocks have run
 * out, clientSimViewEligibleMask being where that question is asked and
 * answering "every item" for the other three policies, and an ally the server
 * has stopped sending centres on the square it was last seen at until the
 * stub run outlasts the grace. */
int run_view_cycle_bases(void);
int run_view_cycle_base_direction(void);
int run_view_cycle_allies(void);
int run_view_cycle_ally_direction(void);
int run_view_cycle_exits(void);
int run_view_cycle_decay_skip(void);
int run_view_cycle_eligible_mask(void);
int run_view_cycle_ally_stub(void);
int run_view_cycle_stale_pill(void);

/* The per-pill position-current flag (test_pill_pos_current.c): the lookups
 * movement, turn rate and shell collision ask pass over a pill whose square
 * this client has not been told is current, while the view and camera siblings
 * still answer for it at the square it was last seen on; two pills on one
 * square resolve to the one that is really there in either order; the client's
 * apply path sets the flag from the snapshot bit on both the pill block and
 * EVENT_PILL_UPDATE, writing the square as sent either way; a pill watched
 * going into a tank and coming out again with no square sent stops being drawn
 * anywhere until its real square arrives, while a pill nobody has touched
 * keeps drawing; and a server's own list, which never carries a flag, answers
 * exactly as it did. */
int run_pill_pos_current_lookups(void);
int run_pill_pos_current_num_split(void);
int run_pill_pos_current_duplicate_square(void);
int run_pill_pos_current_snapshot_apply(void);
int run_pill_pos_current_server_unchanged(void);
int run_pill_pos_current_moved_state(void);
int run_pill_pos_current_carry_cycle(void);

/* Pure viewport square calculator (test_viewport_calc.c):
 * viewportCalcSquarePure agrees with viewportCalcSquare on every map square,
 * never yields TANK_TRANSPARENT, and resolves pill and base squares by the
 * alliance of the player being asked about. It draws a pill at a square this
 * client only remembers, and the terrain underneath at one the pill was
 * carried away from. */
int run_viewport_calc_square_pure(void);
int run_viewport_calc_pill_square_moved(void);

/* Overview region geometry (test_overview_map.c): overviewMapBuildRegions
 * writes the tank's block, which it is handed, first, gives every viewable
 * pillbox a 15x15 one trimmed at the map edges, keeps the pills in index
 * order and never writes more rects than the caller allowed for; where that
 * block goes is the overview window's to decide, and is covered through
 * overviewMapUpdate — the 29x29 on the tank under the Expanded window, the
 * 15x15 at the classic view under the Classic one, the same 15x15 round the
 * tank with no view to read, and the view a dying tank last had;
 * line of sight hides a square behind
 * a building without touching the tile it last showed, leaves the building and
 * the tank's own square in the block, reaches no watched item's block, holds
 * that tile through the update the block stops being live, and sets nothing at
 * all with the toggle off; and
 * overviewMapDeathBlackout puts the overview's blackout in the stretch of a
 * death wait running from the tick the classic view cuts to static through to
 * the respawn. */
int run_overview_regions(void);

/* Overview reveal (test_overview_map.c): one display tick on a freshly joined
 * client marks exactly the squares in the union of its regions live, fills
 * them with what the per-square calculator produces, and leaves every other
 * square of the map unseen. */
int run_overview_reveal(void);

/* Overview freeze (test_overview_map.c): a square the tank drives away from
 * keeps the tile it carried and never picks up a later terrain change the
 * client has already applied, while the same change inside the block the tank
 * drove into does reach the memory. */
int run_overview_freeze_no_leak(void);

/* Overview farewell stamp (test_overview_map.c): a pill that dies, is
 * captured or is picked up takes its block out of the live set, and the
 * block's last stamp shows the pill as it ended rather than as it was a tick
 * earlier. Removing the tank freezes its block the same way. */
int run_overview_pill_capture(void);

/* Overview lifetime (test_overview_map.c): clientSimResetWorld empties the
 * memory back to unseen, and a mid-game map resync leaves what has been seen
 * exactly where it was. */
int run_overview_reset(void);

/* Overview decay clocks (test_overview_map.c): a display tick stamps the items
 * the local tank is beside, for the categories on viewPolicyDecay and no
 * others; an item's block appears while its clock is inside the window, fades
 * over the end of it and freezes what it was showing when it runs out; and the
 * clocks start over on a round reset and a fresh map install while surviving a
 * mid-game resync. The view exit reads the same window: an item view whose
 * clock has run out drops back to the tank view, while one inside its window
 * and one on any other policy are left alone. */
int run_overview_decay_mirror(void);
int run_overview_decay_view_exit(void);

/* Overview regions under the view policies (test_overview_view_policy.c): the
 * rules a server ships with produce the player's own screen and nothing else,
 * where pills and allied tanks on always produce the region list the overview
 * drew before any of this was settable; always sweeps a category, key grants
 * only what the player is watching and off grants nothing, each kind in its
 * own block size and in the order the farewell stamp replays; and a decay
 * window runs from full brightness through the fade to nothing, with an item
 * the player could never watch earning nothing from having been driven
 * past. */
int run_overview_policy_baseline(void);
int run_overview_policy_pills_and_allies(void);
int run_overview_policy_categories(void);
int run_overview_policy_decay(void);

/* Overview over the wire (test_overview_map.c): the same reveal checks against
 * a world delivered by a real UDP join and map download, plus a staged map
 * event that reaches a live square's tile and leaves an unseen one alone. */
int run_overview_loopback(void);

/* Overview camera maths (test_overview_camera.cpp): a square's centre
 * round-trips through view pixels at every step of the 0.5x-4x zoom ladder,
 * a zoom step holds the world point under the cursor still unless follow
 * owns the centre, centre-on-tank puts the tank at the view centre, zoom and
 * pan stop at the ladder's and the map's bounds, and the visible-square
 * range matches hand-worked spans at the map edges. */
int run_overview_camera(void);

/* Overview camera scroll (test_overview_camera.cpp): the animated form of the
 * keep-on-screen nudge. A point already inside the view starts nothing, an
 * off-screen one starts a scroll that moves nothing until the first tick, and
 * ticks summing to OVERVIEW_SCROLL_MS land the centre exactly where the
 * instant nudge would have put it — the least move, not the point. In between
 * the eased centre only ever goes towards the target and never past it, a tick
 * longer than the whole duration stops on it, a re-aim part way runs from the
 * centre it had reached with the clock back at zero, and a pan, a zoom step or
 * a centre-on-tank ends the scroll where the follow tick does not. The follow
 * flag comes through all of it untouched. */
int run_overview_scroll(void);

/* Overview fog mask (test_overview_fog.cpp): a live square comes out clear and
 * ground outside every region fully fogged, the fog steps to full in the one
 * square outside an edge and is as hard off a corner as along a side,
 * overlapping regions take the brightest answer, a region against the map
 * border keeps its brightness to the border without writing past the end of
 * the mask, and no regions at all fogs the whole map. */
int run_overview_fog(void);

/* Fog of war looks (test_fog_roads.c): each of the four styles washes towards
 * the colour it says it does, None washes at all, only Darker with fog edge
 * draws the fog line band, the band fade falls to nothing, and the setting
 * store clamps a value from outside the enum back to Grey. */
int run_fog_style_looks(void);
int run_fog_style_setting(void);

/* The fog line's edge mask (test_fog_roads.c): only a fogged square is banded,
 * only on the sides facing a square in plain sight, so the band marks the fog
 * line on its fogged side; a square at the edge of the grid has fog beyond it
 * and is not banded there. Which terrain is banded is the build switch
 * FOG_EDGE_ALL_TERRAIN, 1 in this build. */
int run_fog_road_edges(void);
int run_fog_road_edge_masks(void);

/* Line of sight (test_sight.c): the square the player stands on is seen even
 * when it is itself a building, a building across the line hides everything
 * behind it while the building itself is seen, the same building beside the
 * line hides nothing, the diagonal between two buildings that touch is closed,
 * squares off the map are never seen, only a building and a half building stop
 * a line, and a block the origin is nowhere near is written at its own width. */
int run_sight(void);

/* The shadow rule (test_sight_shadow.c): with no wall on the map the mask is
 * the one the old centre-line rule built, square for square, on Everard
 * Island's own ground; a pillbox with a blocker up beside it is seen where the
 * centre line called it hidden; a sliver of a square about a quarter wide is
 * enough to see it and one more wall closes the sliver; two walls meeting at a
 * corner leave no crack between their shadows and a square squarely behind a
 * wall is hidden, from anywhere inside the square the player is standing on;
 * the trees answer what they always answered; and off the map is never seen
 * while the square the player is on always is. */
int run_sight_shadow(void);

/* What the shadow pass costs (test_sight_shadow.c): the widest block there is,
 * built from real ground with real walls on it, timed over five hundred runs
 * and printed. */
int run_sight_shadow_cost(void);

/* The full-shadow-pile branch (test_sight_shadow_overflow.c): a second copy of
 * the module built with the pile capped at two, so a row of four walls
 * overruns it and the fold a full pile answers with is taken. What it pins is
 * the direction - the capped mask hides every square the mask the module
 * really builds hides, and some the other way round, never the reverse. */
int run_sight_shadow_overflow(void);

/* In-window overview HUD geometry (test_overview_hud_layout.cpp): the column
 * fits the height at 1080p and on the Steam Deck's 800 lines, its pieces stack
 * in the classic order without overlapping and stay inside their backing
 * strip, the build select stands clear on the left edge with five evenly
 * stacked clickable items, the newswire strip spans the bottom edge, every
 * slice is cut from inside the 515x325 chrome, and a window too small for a
 * legible column is refused without writing to the caller's layout. */
int run_overview_hud_layout(void);

/* Overview dead tank (test_overview_map.c): a tank that is dead and waiting to
 * respawn holds its block on the square it died on — the corner it reads from
 * reveals nothing, the block keeps its tiles and goes on taking terrain
 * changes, and it follows the tank to wherever it respawns. */
int run_overview_dead_tank(void);

/* Overview entity filter (test_overview_map.c): the per-frame lists
 * clientSimPrepareOverviewEntities builds cover the whole map, and
 * overviewEntityIsVisible is what keeps an enemy tank the client still knows
 * about off the picture once it leaves the block the player can see. The
 * local player's own tank obeys the same test: watching a pill under
 * viewPolicyKey closes the block round the tank, and the tank goes off the
 * picture with it until the view is left. */
int run_overview_entities(void);

/* A pillbox or base a removal has taken off the map earns no region. */
int run_overview_removed_item_has_no_region(void);

/* Overview gunsight accessor (test_overview_map.c): clientSimGetGunsightPos
 * reports the crosshair's square and pixel offset while the tank is alive and
 * the sight is shown, and declines — writing nothing — for a hidden sight and
 * for a tank that is dead and waiting to respawn, where clientSimGetGunsightTile
 * still answers with the map origin. */
int run_overview_gunsight(void);

/* My-tank position accessor (test_overview_map.c): clientSimGetMyTankMapPos
 * reports the tank's square while it is alive, fails while it is dead and
 * waiting to respawn rather than handing back the map origin it reads as, and
 * leaves the caller's out-params alone when it fails. */
int run_tank_pos_dead(void);

/* Overview render snapshot (test_overview_snapshot.c): what
 * clientSimFillOverviewSnapshot copies out of the sim under the client mutex
 * for the render to draw from after it. The snapshot's map is byte for byte
 * the live memory and every scalar matches the accessor it came from, across
 * a living tank, a shown gunsight, an item view, a dead tank and a fill from
 * no sim; a write to the live memory after a fill, direct or through a
 * display tick, leaves the snapshot alone until the next fill; the entity
 * lists match what the renderer's own filter produced from the whole-map
 * lists, entry for entry; and a fill on an unchanged generation still hands
 * out the live map without copying it again. */
int run_overview_snapshot_mirror(void);
int run_overview_snapshot_isolation(void);
int run_overview_snapshot_filter(void);
int run_overview_snapshot_generation(void);
/* A pillbox or base a removal has taken off the map has no label in the
 * snapshot's list. */
int run_overview_snapshot_removed_item_has_no_label(void);

/* Sprite placement at a float scale (test_mapview_sprite_scale.c): the
 * arithmetic behind mapViewDrawShells / Tanks / LGMs, shared by the classic
 * view at a whole-number zoom and the overview at its 0.5x-4x ladder. At an
 * integer scale it gives the classic formula's positions, spelled out; at
 * every rung it matches what the overview's former sixteen-steps-per-pixel
 * path drew, within a sixteenth of a pixel; a shell's direction frame comes
 * back by its tip pixel times the scale and an explosion frame does not; and
 * the LGM centring snaps to a whole game pixel from the base under Classic
 * and Match pixelation while Smooth leaves it where the centring put it. */
int run_mapview_sprite_classic(void);
int run_mapview_sprite_ladder(void);
int run_mapview_sprite_shell_tip(void);
int run_mapview_sprite_lgm_snap(void);

/* The entity overlay's placements (test_mapview_overlay.c): the gunsight's
 * top-left is the classic view's formula at an integer scale and the
 * overview's at every rung; a tank's name sits one square to the right of
 * its sprite with no extra offset and is held at the clip's left edge; the
 * build cursor lands on its square at every rung; and the pill and base
 * numbers sit on their square and are withheld below the minimum scale. */
int run_mapview_overlay_gunsight(void);
int run_mapview_overlay_tank_label(void);
int run_mapview_overlay_cursor(void);
int run_mapview_overlay_item_labels(void);

/* The shared map colours: what a tile number says is standing on a square,
 * the flat colour its ground gets when the square is drawn too small for its
 * sprite, and the three layers a marker's stroke is built from. */
int run_map_colours_item_kind(void);
int run_map_colours_terrain(void);
int run_map_colours_markers(void);
int run_map_colours_palette_key(void);
int run_map_colours_team(void);

int run_stall_advances_processed_tick(void);
int run_turn_release_no_overshoot(void);
int run_turn_gap_preserves_ramp(void);
int run_turn_long_gap_no_duplicate(void);
int run_stall_mine_late_lays_once(void);
int run_stall_mine_duplicate_not_relaid(void);
int run_stall_fire_not_harvested(void);
int run_stall_never_fires(void);
int run_stall_brief_trough_no_advance(void);
int run_stall_long_dry_advances(void);

/* Stall-advance lockout (test_stall_lockout_rebase.c): a slot stall-advanced
 * past everything the client has produced takes the newest stale input it has
 * ever seen, moving lastProcessedInput back under it so the stream recovers;
 * the redundant copies of that tick rebase nothing further. */
int run_stall_lockout_rebase(void);
int run_stall_lockout_rebase_once_per_tick(void);
int run_stall_recovery_no_gap_fill(void);

/* Hitch diagnostics (test_hitch_logged.c): the catch-up loop lifted out of
 * serverGameTimer runs every tick the wall clock owes, logs one line when a
 * burst is long enough to lock a slot out, logs nothing at the normal
 * cadence, and stops where a step refuses — the shutdown handshake's shape. */
int run_hitch_logged(void);

/* Backlog catch-up (test_input_catchup.c): a standing input queue above the
 * jitter target bleeds at +1 input per sub-tick (cap 2 applies/sub-tick) so a
 * jitter-spike backlog drains in ~1s instead of ratcheting input latency;
 * steady state never triggers it. Always built (no WB_NETDEBUG gate). */
int run_input_catchup(void);
int run_catchup_ignores_redundant_duplicates(void);

/* Forward input-tick offset (test_input_tick_offset.c): a producer whose
 * counter has fallen behind the server's consumption jumps past
 * lastProcessedInput by the round trip plus a margin, keeps that jump as a
 * per-ClientSim offset every later packet carries, leaves a caught-up
 * producer alone, and caps the round-trip contribution. */
int run_input_tick_offset_adopts_jump(void);

/* Stale build-order harvest (test_build_harvest_stale.c): a build commanded on
 * a stall-substituted tick is stashed with its target tile frozen, so the
 * replay must re-check that tile against the current map — a now-invalid one is
 * dropped instead of nagging the player, a still-valid one still dispatches,
 * and one that folds while the man is out is queued unchecked as his next
 * order. */
int run_build_harvest_stale(void);
int run_build_harvest_valid(void);
int run_build_harvest_busy_queues(void);

/* Build-request validity (test_lgm_request_valid.c): lgmCheckNewRequest's
 * verdicts over terrain and tank stores, and lgmRequestIsValid asking for one
 * without dispatching, spending or messaging the player. */
int run_lgm_request_valid(void);
int run_lgm_request_quiet(void);

/* A quit with a pillbox in the man's hands (test_lgm_quit_pill.c): the pill is
 * on no tank's carry list while he holds it, so the leave path has to put it
 * down itself or it is lost — still flagged as carried, by a man who no longer
 * exists. Issue #340. */
int run_lgm_quit_drops_carried_pill(void);
/* The same pill, the other teardown that deletes a man mid-round:
 * serverSimUnfieldBot, which takes a held seat off the field between waves.
 * No ownership migration runs there, so a stranded pill stayed under the name
 * of a seat that was not on the field. */
int run_lgm_unfield_drops_carried_pill(void);

/* Adaptive jitter buffer (test_jitter_buffer_grow.c): queue drains under
 * jitter deepen jitterTarget toward MAX, a steadily full queue shrinks it
 * back to MIN, and it never exceeds MAX. Always built (no WB_NETDEBUG gate). */
int run_jitter_buffer_grow(void);
int run_shell_projection(void);

/* Shared shell list builder (test_screen_bullets_build.c): the classic
 * viewport list with the scroll origin and the overview's whole-map list with
 * origin 0 come out of clientSimBuildShellList entry for entry as each view
 * used to build them; a shell outside the viewport is only on the whole-map
 * list, an expired prediction on neither, and a bot's list reads the server
 * snapshots instead of the projected layer. */
int run_screen_bullets_build(void);

/* Ping RTT smoothers (test_ping_smoother.c): the min-over-window and EWMA
 * primitives — window-minimum tracking as samples slide out, EWMA constant
 * convergence, and a monotonic overshoot-free step response. */
int run_ping_smoother(void);

/* Client timing estimator (test_client_timing.c): min-over-window clock
 * offset / pipeline depth / RTT and inter-arrival jitter over synthetic clean
 * and jittered snapshot sequences — floor convergence and spike rejection. */
int run_client_timing(void);

/* Render-time interpolation (test_interp_render.c): interpUpdate serverTick
 * idempotency seq-guard, the render-clock fractional-t mapping, and the
 * adaptive display-delay controller (bounded <=1 extra snapshot, hysteresis,
 * asymmetric slew, frame-spike safe-degrade). */
int run_interp_render(void);

/* Respawn no-death-flash (test_interp_respawn_no_death_flash.c): a remote
 * tank whose prev sample is the dead/death-position state and whose curr is
 * the alive respawn (teleport) snaps to curr instead of tweening from the
 * stale death spot; a normal alive->alive pair still interpolates. */
int run_interp_respawn_no_death_flash(void);

/* Field-presence snapshot compaction (test_snapshot_compaction.c): pure
 * pack -> unpack roundtrip over representative tank entries — field fidelity,
 * wire-size bounds, the unchanged 1-byte stub, and truncation safety. */
/* The per-simulation rules table (test_sim_rules.c). Defaults walked field
 * by field against the constants they replaced; the range checks for every
 * field this change converted, at both ends and inside; the display and
 * brain copies reporting a rule that has been moved off its classic value;
 * the terrain speed and turn rules at the two map readers; and the river cap
 * carrying the wading test with it. */
int run_sim_rules_classic_defaults(void);
int run_sim_rules_validate_ranges(void);
int run_sim_rules_copies_follow(void);
int run_sim_rules_terrain_caps_follow(void);
int run_sim_rules_river_cap_moves_drowning(void);
int run_sim_rules_base_regen_seed_follows(void);
int run_sim_rules_terrain_life_follows(void);
int run_sim_rules_base_empties_without_wrapping(void);
int run_sim_rules_pill_empties_without_wrapping(void);
int run_sim_rules_pill_shell_damage_follows(void);
int run_sim_rules_pill_angry_divisor_follows(void);
int run_sim_rules_pill_massage_follows(void);
int run_sim_rules_tank_explosion_follows(void);
int run_sim_rules_water_loss_follows(void);
int run_sim_rules_pairs(void);
int run_sim_rules_capture_threshold_moves(void);
int run_sim_rules_builder_cost_follows(void);

/* A table against the classic one, and the observation builder's refusal to
 * build on a sim that is not running it (test_sim_rules.c). */
int run_sim_rules_are_classic(void);
int run_sim_rules_obs_refuses_non_classic(void);
int run_sim_rules_shell_flight_follows(void);
int run_sim_rules_brain_shot_follows(void);
int run_sim_rules_worldsim_pill_follows(void);
int run_sim_rules_boat_speed_follows(void);
int run_sim_rules_obs_reload_follows(void);

/* The rule list as a frontend reads it (test_sim_rules_describe.c): the
 * names and the classic values behind them, the reload rule a mod moves
 * first, the ratios and differences a change is described by, and the arm
 * each unit tag chooses. */
int run_sim_rules_describe_names(void);
int run_sim_rules_describe_reload(void);
int run_sim_rules_describe_ratios(void);
int run_sim_rules_describe_units(void);
/* The words a change is drawn as (src/gui/sdl3/sim_rules_phrase.c): the
 * number's sign, places and trimming, and nothing for an index that names
 * no rule. The lang arms come back as test_stubs.c's placeholder. */
int run_sim_rules_phrase(void);

/* The range behind each rule (test_sim_rules_range.c): every rule answers
 * one, the ends it states are the ends the validator refuses on, and the
 * rows a second rule caps name that rule. */
int run_sim_rules_range_every_rule(void);
int run_sim_rules_range_matches_check(void);
int run_sim_rules_range_paired(void);

/* What a rule is in words (test_sim_rules_desc.c): a description id per rule,
 * inside the block they were given and none of them shared, and the range
 * phrase answering safely for any index and any buffer. The English itself is
 * not visible here — test_stubs.c answers "?" for every id. */
int run_sim_rules_desc_table(void);
int run_sim_rules_desc_range_phrase(void);

int run_snapshot_compaction(void);

/* Render-only error smoothing (test_error_smoothing.c): the offset
 * accumulate/decay/clamp/wrap math as pure functions, plus the
 * clientSimResetWorld zeroing. Always built. */
int run_error_smoothing(void);

/* In-process loopback transport tests (loopback_harness.c): real UDP
 * client + server over localhost sockets, with seeded impairment on the
 * client endpoint. Convergence-bounded, never exact-trace. */
int run_loopback_join(void);
int run_loopback_join_loss(void);
int run_loopback_password(void);
int run_loopback_lobby_running_loss(void);
/* Quiet-lobby reliable control delivery under loss with no input flowing:
 * proves control acks ride the standalone PACKET_CHANNEL trailer. */
int run_loopback_quiet_lobby_control_loss(void);
/* A command packet refreshes the sending client's server-side liveness clock:
 * watched directly across a window in which nothing else is refreshing it. */
int run_loopback_command_liveness(void);
/* Parallel channel layer over the loopback transport: empty-flow inertness
 * plus a synthetic message round-trip under loss + jitter + dup. */
int run_loopback_channel(void);
/* A slot locked out by a server hitch recovers over the real transport under
 * an 80ms one-way delay (test_loopback_hitch.c): the client keeps producing
 * while the server stops, the server pays the debt in one burst, and a button
 * value no substitute can invent has to reach the server again and keep
 * reaching it as the client changes it. */
int run_loopback_hitch_recovers(void);
/* One client quits while another keeps playing (test_loopback_quit_keeps_peer.c):
 * the peer sees the slot leave its roster, stays connected for 600 pumps, and
 * its own inputs keep being applied on tick numbers it actually sent. */
int run_loopback_quit_keeps_peer(void);
/* One running tick carries more than one channel frame
 * (test_send_multi_frame.c): a raw-socket client joined past map download is
 * given more reliable game events than one frame holds, and a single
 * transportUdpServerSend has to put every one of them on the wire — the
 * snapshot plus at most SNAPSHOT_EXTRA_CHANNEL_FRAMES standalone frames. */
int run_send_drains_channels_multi_frame(void);
/* A burst of best-effort traffic survives the tick it was raised on
 * (test_best_effort_burst.c): effect events and a voice frame queued on one
 * tick, with the snapshot trailer already filled by reliable traffic, all reach
 * a real client within two pumps of that tick, and the server's mux reports no
 * ring drop and nothing left behind by the budget. */
int run_best_effort_not_dropped(void);
int run_effect_burst_not_starved_by_reliable(void);
int run_post_game_segment_applied(void);
/* A quitting player's ownership burst reaches the peer (test_best_effort_burst.c):
 * client 2 holds every pillbox and base and quits, and client 1 is told the new
 * owner of every one of them, plus the leave, within two pumps of the tick that
 * published them. */
int run_quit_burst_all_delivered(void);
/* Server lock/unlock notice over CHANNEL_GAME (test_lock_channel.c): the
 * "locked to new players" message now rides the reliable game channel, not the
 * snapshot reliable tail. */
int run_lock_channel(void);
/* Server-map preview over CHANNEL_BULK (test_loopback_preview.c): a real .map
 * file streamed back under loss and reassembled byte-identical on the client. */
int run_loopback_map_preview(void);
int run_loopback_brain_docs_fetch(void);
int run_loopback_brain_docs_spectator(void);
/* Client->server map upload over CHANNEL_BULK (test_loopback_upload.c): a map
 * uploaded under loss completes and the server decodes the reassembled bytes. */
int run_loopback_map_upload(void);
/* Script upload (test_script_upload.c): the BEGIN body for both kinds against
 * hand-written bytes; the server's BEGIN refusals for a script, and the map
 * ones the kind byte must leave alone; a 4 MiB package arriving whole at the
 * accept callback; a refused script's reason reaching the client; and the
 * client refusing a file before sending anything. */
int run_upload_begin_golden(void);
int run_script_upload_begin_refusals(void);
int run_loopback_script_upload_at_cap(void);
int run_loopback_script_upload_refused(void);
int run_script_upload_client_refusals(void);
/* Script upload landing (test_script_upload.c), with the host's own accept
 * callback: a mod and a package land and are listed; a bound package and a
 * .lua that will not load are refused, the second with its line; a name a
 * higher directory holds is refused at BEGIN; the persist caps; the accept
 * callback's own refusals; the session directory emptied and the persist one
 * kept; the lobby reset dropping the session's picks; and an uploaded mod
 * composing in the next decision. */
int run_loopback_script_upload_lands_listed(void);
int run_loopback_script_upload_package_listed(void);
int run_loopback_script_upload_bound_refused(void);
int run_loopback_script_upload_syntax_line(void);
int run_script_upload_name_taken(void);
int run_script_upload_persist_caps(void);
int run_script_upload_accept_refusals(void);
int run_script_upload_session_emptied(void);
/* A file the server removed is not listed even when the directory's stamp
 * is put back to the one the listing was kept at (POSIX only). */
int run_script_upload_listing_sees_own_removal(void);
int run_script_upload_reset_drops_session_picks(void);
int run_loopback_script_upload_plays_next_round(void);
int run_script_upload_list_source(void);
int run_loopback_script_upload_source_on_wire(void);
/* A list request sent as DONE lands is answered, not dropped by the request
 * cooldown the upload's BEGIN started. */
int run_loopback_script_upload_list_after_done(void);
/* The Mods chooser's rows from the server and from this computer
 * (test_lobby_script_rows.c): which of the two holds each file, by Workshop
 * id or by name ignoring case, in the server's order then the local order,
 * none of this computer's alone in process, and the cap; and the listing of
 * the player's own Mods directory with no sim. */
int run_lobby_script_rows_states_by_name(void);
int run_lobby_script_rows_case_only_match(void);
int run_lobby_script_rows_workshop_id_matches(void);
int run_lobby_script_rows_workshop_id_differs(void);
int run_lobby_script_rows_in_process(void);
int run_lobby_script_rows_order(void);
int run_lobby_script_rows_truncated(void);
int run_scenario_local_scripts_listed(void);
/* Map join-download + live resync over CHANNEL_BULK (test_loopback_download.c):
 * a lobby join download completes under loss; a mid-game joiner downloads while
 * the server is Running (the bulk-carrier deadlock case); and a reported
 * checksum mismatch drives a resync that installs and bumps the resync count. */
int run_loopback_download_join(void);
int run_loopback_download_midgame(void);
int run_loopback_resync(void);

/* Mid-lobby map change + join-download recovery (test_loopback_map_change.c):
 * a clean map change re-downloads and reconverges; map changes and initial
 * joins under loss+duplication (seed spreads) must always converge — the
 * shapes that used to wedge the lobby in DOWNLOADING_MAP when the bulk map
 * stream raced the JOIN_ACCEPT or a duplicate accept wiped the receiver. */
int run_loopback_map_change_clean(void);
int run_loopback_map_change_loss(void);
int run_loopback_join_accept_loss(void);
int run_loopback_join_accept_loss_midgame(void);

/* Gate-#1 render-path integration (test_gate1_integration.c): the viewTick
 * ±1-snapshot invariant over the real loopback transport, and the
 * listen-server host render-prepare no-op (own tank + recon unchanged). */
int run_gate1_viewtick_loopback(void);
int run_gate1_host_noop(void);

/* Estimator-jitter -> render-interpolation wiring (test_interp_jitter_e2e.c):
 * loss-induced jitter over the loopback transport grows the adaptive display
 * delay end-to-end via clientSimRenderPrepare; a clean link leaves it at 0. */
int run_interp_jitter_e2e(void);

int run_join_version_gate(void);

/* Per-source-IP JOIN rate limit (test_join_rate_limit.c): a burst of valid
 * JOINs from distinct loopback ephemeral ports (one source IP) draws at most
 * JOIN_RL_BURST accepts, while a JOIN from a distinct source IP is unaffected. */
int run_join_rate_limit(void);

/* JOIN address-proof cookie handshake (test_cookie_handshake.c): a cookie-less
 * JOIN draws a PACKET_JOIN_CHALLENGE and no slot; echoing the challenge cookie
 * completes the join; a garbage cookie never completes; a cookie expires once
 * the server's time-window advances (via the WB_COOKIE_WINDOW_OFFSET seam). */
int run_cookie_handshake(void);

/* Map-send amplification gate (test_map_amp_gate.c): a crafted JOIN over a raw
 * loopback socket draws only a small JOIN_CHALLENGE (no accept, no CHANNEL_BULK
 * carrier) until the address-proof cookie is echoed, after which the accept and
 * the map stream flow — the cookie is the sole amplification gate. */
int run_map_amp_gate(void);

/* The map-download re-ask interval (test_map_reask_throttle.c): a re-ask
 * recompresses the slot's terrain, re-sends JOIN_ACCEPT and restarts the
 * stream, so a joined client must not be able to drive that in a loop. The
 * first READY of a download is the arming ask and is never held off. */
int run_map_reask_throttle(void);

/* Map-desync resync queue logic (test_map_resync.c): the resync cut empties
 * the slot's map-event queue (ackedSeq == nextSeq); baked-in changes are not
 * re-sent and a post-cut change delivers exactly once; a duplicate request
 * while a resync is in flight does not re-cut (no event loss); the send gate
 * yields zero map events while in progress and resumes after. */
int run_map_resync_cut_and_deliver_once(void);
int run_map_resync_duplicate_request_no_recut(void);
int run_map_resync_send_gate_holds(void);
int run_map_resync_stale_gen_rejected(void);

/* Map compressed-codec round-trip (test_map_compress_roundtrip.c): a real map
 * (and a mutated one carrying mine-range terrain near pills/bases) must survive
 * mapSaveCompressedMap -> mapLoadCompressedMap tile-for-tile, and the
 * compressor must refuse a map that does not fit the output capacity it was
 * given rather than write past it. */
int run_map_compress_roundtrip_stock(void);
int run_map_compress_capacity_refuses(void);
int run_map_compress_incompressible(void);
int run_map_compress_roundtrip_mutated(void);
int run_map_compress_rejects_null_handles(void);
int run_map_checksum_ignores_mines(void);
int run_map_resync_base_crater_converges(void);
int run_map_pill_mine_cleared_on_load(void);
int run_map_carried_pill_keeps_terrain(void);

/* Per-client copies of the terrain (test_map_shadow.c): each slot's
 * clientKnownMap follows the live map across frames of scattered terrain
 * changes; its checksum equals the live map's and equals what
 * serverSimBuildSnapshot stamps into that slot's header;
 * serverSimGetCompressedMapFor produces the same bytes as
 * serverSimGetCompressedMap; and a join, a round reset and a lobby map change
 * each re-seed the copies. */
int run_map_shadow_tracks_real(void);
int run_map_shadow_crc_matches(void);
int run_map_shadow_blob_identical(void);
int run_map_shadow_seed_lifecycle(void);

/* Culled copies (test_map_shadow.c): a slot the UDP transport has marked is
 * skipped by the tick — the transport writes it one square at a time as it
 * queues events — and the catch-up sweep pays back what such a slot is owed
 * inside the rects it is given, at the cap it is given, converging to silence
 * and clamping rects that run off the map. */
int run_map_shadow_cull_withholds(void);
int run_map_shadow_sweep_converges(void);
int run_map_shadow_sweep_bounds(void);

/* The round-start copy (test_map_shadow.c): it is taken wherever a map is
 * installed and stands still between those points; a slot seeded from it — what
 * a player joining a running game gets — holds and compresses to the terrain
 * the round started on rather than the live map, and a sweep over the changed
 * ground converges it; with nothing captured the seed falls back to the live
 * map. */
int run_map_shadow_round_start_capture(void);
int run_map_shadow_join_seeds_round_start(void);
int run_map_shadow_round_start_fallback(void);

/* Per-client records of the pill squares (test_map_shadow.c): each slot's
 * clientKnownPillX/Y follows the live pill list across frames in which pills
 * move; the checksum over a slot's terrain copy and its pill squares equals the
 * live one and equals what serverSimBuildSnapshot stamps;
 * serverSimGetCompressedMapFor produces the same bytes as
 * serverSimGetCompressedMap while the two agree and the round-start squares
 * once they do not; and a full-sync snapshot's pill entries and pill events
 * rebuild, in the client's order, the list the header's checksum was taken
 * over — for a pill the recipient can see, and for one whose square it is not
 * being told. */
int run_pill_shadow_tracks_real(void);
int run_pill_shadow_crc_matches(void);
int run_pill_shadow_blob_identical(void);
int run_pill_shadow_fullsync_move_matches(void);
int run_pill_shadow_withheld_crc_matches(void);

/* Map-event culling over the loopback transport (test_loopback_map_cull.c): a
 * change a wire client cannot see is neither queued to it nor written into its
 * copy, its checksum still describes the map it holds so it never resyncs, an
 * in-process slot takes the same change, a visible change arrives as before,
 * and driving over to the stale ground catches it up. The second case forces a
 * resync while the client is behind: the blob is its copy, not the live map. */
int run_loopback_map_cull(void);
int run_loopback_map_cull_resync(void);

/* Sound culling over the loopback transport (test_sound_delivery_wire.c): for a
 * recipient whose only viewport rect is its own tank screen, both delivery
 * paths carry a sound 30 squares away and neither carries one at 45, a
 * delivered sound arrives with a tier and a bearing rather than its map
 * square, a far manLayingMineNear is dropped, and a tank hit on the recipient
 * arrives from 60 squares out. */
int run_sound_delivery_wire_cull(void);

/* What the client plays for a sound the wire delivered
 * (test_sound_delivery_wire.c): the near variant at 10 squares, the far
 * variant at 30, and manLayingMineNear at 10. */
int run_sound_tier_playback(void);

/* Ally view over the loopback transport (test_view_ally_loopback.c): under
 * viewPolicyKey, with an allied in-process player parked outside every rect
 * the wire client has and absent from the client's interpolation mask, the
 * ally key still reaches them and the server then starts sending them; the
 * same press with that ally in its death wait reaches nobody and leaves the
 * camera on the tank. */
int run_view_ally_loopback(void);

/* Client resync finalize (test_resync_finalize.c): a corrupt/truncated blob
 * must not advance installedMapGen/mapResyncCount (and re-arms the resync); a
 * valid blob advances them; installCompressedMap reports failure on garbage. */
int run_resync_finalize_corrupt_keeps_gen(void);
int run_resync_finalize_valid_advances_gen(void);
int run_install_compressed_map_rejects_garbage(void);
int run_resync_debounce_threshold(void);

/* Connection-id NAT-rebind migration (test_conn_migration.c): the pure
 * connId match-and-rehome decision, plus an end-to-end loopback join that
 * confirms the server stores a connId and inputs ride the new framing. */
int run_conn_migration_rehome(void);
int run_conn_migration_e2e(void);

/* Exact lag-compensation viewTick (test_viewtick_rewind.c): the pure
 * serverSimComputeLagCompTicks rewind math (real view age, ping fallback,
 * clamp) and the client's displayed-tick stamp (second-newest applied
 * snapshot serverTick, 0 until two are applied). */
int run_viewtick_rewind(void);
int run_viewtick_displayed_tick(void);

/* Differential check of the generated flat-leaf-snapshot codecs against the
 * hand-rolled packers (test_wire_corpus.c). The capture entry regenerates the
 * golden fixtures from a loopback session and is run on demand only. */
int run_wire_corpus(void);
int run_wire_corpus_capture(void);

/* packetTypeName mapping pin (test_packet_type_names.c): every PACKET_* maps to
 * its exact debug string and an undefined type id resolves to "UNKNOWN". */
int run_packet_type_names(void);

/* River tile lookup (test_screencalc_river.c): screenCalcRiver reads its
 * four orthogonal neighbours only as "wet or bridged", so RIVER / ROAD /
 * DEEP_SEA / BOAT are interchangeable there — the contract the four
 * RIVER_END1..4 arms broke by asking for == RIVER, which drew corner
 * pieces instead of end pieces around a road laid on a river cross. Also
 * pins the log viewer's hand-kept copy against the game's. */
int run_screencalc_river_road_counts_as_water(void);
int run_screencalc_river_arms_of_road_centred_cross(void);
int run_screencalc_river_copies_agree(void);

/* Smart-ping pure headers and wire shape (test_ping.c): the chord packing
 * and its exact-modifier match rule, the pie's slice selection including the
 * wrap at the top and the dead zone, the off-screen edge marker on all four
 * borders and at a corner, the rectangle inset that keeps a bar off the map
 * overview's HUD panels, and EVENT_PING's data size / reliability. */
int run_ping_binding_encode_decode(void);
int run_ping_binding_match(void);
int run_ping_binding_direct(void);
int run_ping_binding_format(void);
int run_ping_pie_slices(void);
int run_ping_edge_sides(void);
int run_ping_edge_corner(void);
int run_ping_edge_size_from_distance(void);
int run_ping_edge_name_anchor(void);
int run_ping_rect_inset(void);
int run_ping_event_wire(void);
int run_ping_sound_fallback(void);
int run_ping_name_truncate(void);

/* Server side of the smart ping (test_ping_dispatch.c): the CMD_PING arm's
 * running-game / occupied-slot / known-kind gates, the per-player rate limit
 * and its reset at a round change, the GameEvent an accepted ping turns into
 * (including the retry when the frame's event buffer is full), and the
 * team-only delivery predicate both copies of the snapshot filter call. */
int run_ping_dispatch_accepts_and_builds_event(void);
int run_ping_dispatch_rejects_lobby(void);
int run_ping_dispatch_rejects_empty_slot_and_out_of_range(void);
int run_ping_dispatch_map_range_bound(void);
int run_ping_dispatch_rejects_bad_kind(void);
int run_ping_dispatch_rate_limit(void);
int run_ping_dispatch_spam_30s_window(void);
int run_ping_dispatch_new_round_clears_rate_limit(void);
int run_ping_dispatch_smart_pings_off(void);
int run_ping_reaches_team_only(void);
int run_ping_mute_relay_skip(void);
int run_ping_mute_client_mirror_cleared_on_leave(void);
int run_ping_sender_name_empty_for_unused_slot(void);

/* The two curves behind the smart ping's world marker
 * (test_ping_marker_anim.c): the three-flash blink pingWorldMarkerAlpha
 * draws it with, and the arrival ring's close and pulse from ringAnimAt.
 * Both are pure functions of the ping's age, so a recording replays what
 * was shown live — which is the property these pin. */
int run_ping_world_marker_alpha(void);
int run_ring_anim_at(void);

/* The log_GameSettings blob (test_game_settings_blob.c): the real writer
 * and the log viewer's real decode over the same bytes, including the
 * offsets an older reader still indexes by and a short payload from a
 * writer that predates the last two fields. */
int run_game_settings_blob(void);

/* Smart ping across the wire (test_ping_network.c): the full client -> server
 * -> client path over the real loopback UDP transport, driven through
 * serverInstanceTick so the per-frame event-buffer clear is exercised. A lone
 * player's ping must echo back to itself, and a teammate's ping must reach the
 * other client. Red while the EVENT_PING is cleared before the wire drain. */
int run_ping_network(void);

/* Generated lang-name lookup table pin (test_lang_name_table.c): the
 * K_LANG_NAME_TABLE_SIZE macro matches the real kLangNameTable[] length,
 * the table is strictly sorted for bsearch, and every name round-trips —
 * guards the off-by-one that walked resolveName()'s bsearch off the end. */
int run_lang_name_table(void);

/* What a quit inside a dialog means (test_dialog_quit.c): Cmd+Q, Alt+F4 and
 * the close box end the application; the close request the gamepad's B
 * button forges only closes the dialog; another window's close request and a
 * windowless dialog claim neither. Plus where the quit goes — the host
 * registers a handler, and with none registered a quit must not crash the
 * standalone Log Viewer and Map Editor, which link these dialogs but have no
 * application loop to end. */
int run_dialog_quit(void);

/* Bot difficulty plumbing (test_bot_init_arg.c): the "difficulty=<word>"
 * BRAIN_INIT_ARG token is appended after any existing tokens with a ';',
 * takes an exact fit, and is dropped WHOLE (buffer untouched) when it would
 * not fit — a truncated token would silently run the bot at a difficulty
 * nobody asked for. Plus the difficulty <-> word round trip the CLI's
 * -difficulty flag and the "Chosen Difficulty" preference share. */
int run_bot_init_arg_difficulty_token(void);
int run_bot_init_arg_mode_tokens(void);
int run_bot_difficulty_names(void);

/* mDNS LAN discovery (test_mdns_discovery.c): unicast-loopback round-trip of
 * the advertiser builder + browser parse path, asserting the SRV port, the
 * inLobby/locked flags, every TXT field, and two-instance resolution. */
int run_mdns_discovery(void);

/* The mDNS view TXT key (test_mdns_view_txt.c): the server's visibility
 * rules through the browser's parse seam, and what an absent or
 * malformed value reports. */
int run_mdns_view_txt_roundtrip(void);

/* Self-reported client platform pin (test_client_type.c): the JOIN-time
 * bolo_detect_client_type() resolves to the build host's CLIENT_TYPE_*
 * (the baseline harness normalizes this field away, so it's pinned here),
 * and bolo_client_type_name() maps every enumerator to its exact name. */
int run_client_type_matches_platform(void);
int run_client_type_name_round_trips(void);

/* Client slot bookkeeping (test_client_slot_reassign.c): the client's single
 * tank and lgm live at its slot index, and clientSimSetPlayerNum has to carry
 * them across when a second JOIN_ACCEPT assigns a different slot. The move
 * used to read slot 0 unconditionally, which is only where they sit on the
 * first assignment — later ones wrote NULL over the live pointers and orphaned
 * both objects. Asserts one live tank and one live lgm, at the current slot. */
int run_client_slot_reassign_carries_tank_and_lgm(void);
int run_client_slot_reassign_same_slot_is_stable(void);
int run_client_slot_reassign_back_to_zero(void);

/* Downloaded-map clamps (test_map_load_validate.c): mapLoadCompressedMap
 * memcpys the wire bases/pillboxes/starts wholesale, so none of the per-field
 * clamps the file-load setters apply have run. Asserts a hostile blob cannot
 * leave an out-of-range owner, base stock, pill armour or speed, or start dir
 * in live game state. */
int run_map_load_clamps_base_fields(void);
int run_map_load_clamps_pill_fields(void);
int run_map_load_clamps_start_dir(void);
int run_players_oob_index_safe(void);
int run_control_oob_player_dropped(void);

/* A control-queue overflow detected inside a publish must defer the disconnect
 * (serverDisconnectClient + serverSimRemovePlayer both publish) rather than run
 * it synchronously and re-enter serverSimPublishControl
 * (test_control_overflow_disconnect.c). */
int run_control_overflow_defers_disconnect(void);

/* A player removed mid-sim-frame must not invalidate the arrays simRunHalfStep
 * snapshotted for its world update (test_ping_kick_teardown.c): shellsUpdate
 * survives a slot whose lgm was cleared behind the snapshot, and the high-ping
 * kick defers its teardown to transportUdpServerDrainPendingRemovals instead
 * of freeing the tank/lgm inline. */
int run_shells_survive_cleared_lgm_slot(void);
int run_ping_kick_defers_teardown(void);
int run_ping_kick_clears_strikes_on_disconnect(void);

/* tkExplosionUpdate pairs lgms[i] with tanks[i] over the COMPACTED per-player
 * arrays, and its small-explosion sweep must cover every index
 * (test_tkexp_lgm_pairing.c). */
int run_tkexp_lgm_pairing(void);

/* In-game input gate taxonomy (test_input_gate.c). gameInputSuspended()
 * suspends the polled in-game readers only for blocking surfaces (text
 * input, a focus-stealing modal, a popup/menu on the stack, a defocused
 * window) and never for the transient alliance/vote notifications. */
int run_input_gate_taxonomy(void);

/* Game-binding claims (test_key_claims.c). keyIsClaimedByGame() must report
 * every keyItems field as owned by the game, so a second window that drives
 * the game — the Map Overview pop-out — never shadows a bound key, and must
 * leave scancode 0 and unbound keys free. */
int run_key_claims(void);

/* Pasted server-address splitting for the manual join dialog
 * (test_server_address_parse.c). */
int run_addrparse_host_only(void);
int run_addrparse_host_port(void);
int run_addrparse_scheme(void);
int run_addrparse_trailing_path(void);
int run_addrparse_whitespace(void);
int run_addrparse_port_bounds(void);
int run_addrparse_bad_port(void);
int run_addrparse_empty(void);

/* Voice codec round-trip (test_voice_core.c): a continuous tone encoded and
 * decoded frame by frame stays inside the per-frame byte budget (the
 * constrained-VBR guarantee), decodes a full 20 ms frame every time, keeps
 * its signal level, and conceals a dropped packet. */
int run_voice_core_roundtrip(void);

/* Saved audio device name matching (test_voice_device.c): a name that is
 * present resolves to its index wherever it sits in the list, and every way
 * of being absent - not there, empty, NULL, an empty list, a prefix rather
 * than the whole name - answers -1, which the caller reads as the system
 * default. A hole in the list is stepped over. */
int run_voice_device_resolve(void);

/* Level meter peak (test_voice_peak.c): a spike sets the peak, it stands for
 * the hold with the clock driven straight through the argument, then falls
 * until it reaches the live level and follows it rather than dropping through
 * it. A louder reading mid-fall replaces the peak and re-arms the hold, and a
 * level from outside 0..1 is clamped before it reaches the state. */
int run_voice_peak(void);

/* Voice segment framing (test_voice_segment.c): fields survive both
 * directions with the payload left pointing into the caller's buffer, and
 * every short, oversized, or out-of-range segment is refused. */
int run_voice_segment_roundtrip(void);
int run_voice_segment_rejects_malformed(void);

/* Per-speaker jitter buffer (test_voice_jitter.c): frames play in sequence
 * order however they arrive, gaps are concealed, sequence numbers compare
 * correctly across the 256 wrap, a talker who stops stops playback, and a
 * later utterance at an unrelated sequence number resumes it. */
int run_voice_jitter_ordering_and_plc(void);

/* Frames that arrive but will not decode (test_voice_jitter.c): each is
 * concealed and consumed like a missing one, a run of them ends playback
 * instead of concealing indefinitely, and the end-of-utterance flag is
 * honoured on a frame that failed to decode. */
int run_voice_jitter_undecodable_run(void);

/* Jitter buffer under adverse arrival (test_voice_jitter_loss.c): a generated
 * loss / reorder / burst-outage pattern is pushed a tick at a time, and the
 * per-speaker counters are used to check that concealment matches what was
 * lost, that no frame is played twice or vanishes, that added latency stays
 * bounded, and that playback recovers from an outage. Also prints the
 * concealment and latency table the jitter constants are tuned from. */
int run_voice_jitter_under_loss(void);

/* Per-client voice flood cap (test_voice_flood_cap.c): over the loopback
 * transport, a burst queued inside one client tick is forwarded only up to
 * VOICE_SEGMENTS_PER_TICK with the remainder drained and counted as dropped,
 * while a steady one-frame-per-tick talker loses nothing and the separate
 * concurrent-talker cap never fires on them. */
int run_voice_flood_cap_enforced(void);

/* Concurrent-talker selection (test_voice_talker_select.c): the per-recipient
 * cap keeps the most recently started talkers rather than the lowest-numbered
 * slots, breaks an onset tie on the newer sequence number across the 256 wrap,
 * is stable for inputs tied on both, and holds its bounds at and below the
 * cap and on degenerate input. */
int run_voice_talker_select_ranks_recent(void);
int run_voice_talker_select_bounds(void);

/* Skin asset reads out of a directory vs a zip (test_skin_source.c). */
int run_skin_source_dir_and_zip(void);

/* The loaded skin vs the player's chosen one (test_skin_source.c). */
int run_skin_active_vs_requested(void);

/* Writing a Workshop id into a directory skin and into an archive
 * (test_skin_source.c). */
int run_skin_workshop_id_roundtrip(void);

/* The [MapPalette] section of skin.ini: the three colour spellings, black
 * surviving as a value rather than reading as absent, a bad value leaving its
 * own entry alone, and a section the parser does not know being skipped. */
int run_skin_map_palette(void);

/* Which densities a skin serves, and what each Tile Detail mode picks
 * out of that (test_skin_density.c). */
int run_skin_density_scan(void);
int run_sheet_bleed_edges(void);
int run_sheet_no_key_under_alpha(void);
int run_bmp_sheet_no_key_under_alpha(void);

/* The ring of texels around a sprite slot, on the padded atlas the tank,
 * shell and LGM drawers sample and on the packed sheet it is copied out
 * of (test_sprite_atlas.c). */
int run_sprite_atlas_isolated(void);
int run_sprite_atlas_lookup(void);
int run_sprite_atlas_packed_sheet_unsafe(void);

/* Which <name>_N.wav members a source holds, and the compaction that
   keeps a decoded pool contiguous (test_sound_variants.c). */
int run_sound_variant_pool_names(void);
int run_sound_variant_load_compaction(void);

/* Bolo pascal-string reader, both copies of it (test_pascal_string.c). */
int run_pascal_string_lengths(void);
int run_pascal_string_viewer_copy_agrees(void);
int run_pascal_string_roundtrip(void);

/* Dedicated-server operator console command parsing
 * (test_server_console.c). */
int run_console_lock_unlock(void);
int run_console_info_and_status(void);
int run_console_savemap_path(void);
int run_console_unknown_command_is_inert(void);
int run_console_say_keeps_case(void);

/* The scenario write door (test_scenario_funnel.c): the op funnel's
 * prelude and its refusals, the tick's op, message and tile allowances
 * and the host's ops that spend none of them, the policy and per-tick
 * registrations
 * beside it, and the start-in-progress flag that keeps the all-ready
 * detector out of a start already under way. */
/* The per-tank modifier set (test_tank_modifiers.c): the op that writes it,
 * the states it refuses, the snapshot group under the ninth presence bit, and
 * the create-clears / respawn-keeps rule for the values on the tank. */
/* The sites that read a modifier (test_tank_modifier_sites.c): one case per
 * site, each pairing the modified run with a classic one on the same
 * square so only the modifier is under test. */
int run_tank_mod_speed_caps_on_road(void);
int run_tank_mod_speed_river_still_moves(void);
int run_tank_mod_accel_doubles_ticks_to_cap(void);
int run_tank_mod_accel_doubles_ticks_to_brake(void);
int run_tank_mod_accel_halves_autoslow(void);
int run_tank_mod_turn_halves_circle_ticks(void);
int run_visible_turn_tap(void);
int run_visible_turn_held_and_reset(void);
int run_turn_tap_input_edges(void);
int run_tank_mod_reload_fires_twice_as_often(void);
int run_tank_mod_dealt_kills_in_half_the_hits(void);
int run_tank_mod_taken_takes_more_hits(void);
int run_tank_mod_mine_damage_scales_with_layer(void);
int run_mine_damage_fatal_reduction(void);
int run_tank_mod_neutral_owner_deals_classic(void);
int run_tank_mod_boat_exit_at_half_speed(void);
int run_tank_mod_pill_leads_half_speed_boat(void);
int run_tank_mod_predicted_stop_matches_engine(void);

int run_tank_modifiers_op_writes_set(void);
int run_tank_modifiers_op_refusals(void);
int run_tank_modifiers_wire_roundtrip(void);
int run_tank_modifiers_survive_death(void);
int run_tank_modifiers_cleared_at_create(void);

int run_scenario_op_unknown_type_unsupported(void);
int run_scenario_op_every_type_unsupported(void);
int run_scenario_policy_register_replace_clear(void);
int run_scenario_op_refused_in_policy(void);
int run_scenario_op_refused_in_nested_policy(void);
int run_scenario_op_refused_during_start(void);
int run_scenario_tick_called_both_branches(void);
int run_scenario_start_flag_set_during_start(void);
int run_scenario_start_guard_blocks_reentry(void);
int run_scenario_start_flag_cleared_after_start(void);
int run_scenario_round_start_called_both_starts(void);
int run_scenario_setup_window_admits_ops(void);
int run_scenario_setup_window_roster_refused(void);
int run_scenario_setup_window_shut_refuses_all(void);
int run_scenario_setup_window_no_callback(void);
int run_scenario_setup_window_holds_publish(void);
int run_scenario_round_boot_before_tanks(void);
int run_scenario_round_boot_publishes_rules_once(void);
int run_scenario_setup_events_off_the_wire(void);
int run_scenario_setup_terrain_raises_map_events(void);
int run_scenario_setup_terrain_reaches_shadow(void);
int run_scenario_round_start_clears_seat_holders(void);
int run_scenario_funnel_ops_per_tick(void);
int run_scenario_funnel_msgs_per_tick(void);
int run_scenario_funnel_host_ops_uncounted(void);
int run_scenario_funnel_prelude_refusal_uncounted(void);
int run_scenario_funnel_set_tile_spends_tile_budget(void);

int run_scenario_read_roster_slot(void);
int run_scenario_read_pill_info(void);
int run_scenario_read_base_info(void);
int run_scenario_read_start_info(void);
int run_scenario_read_tank_info(void);
int run_scenario_read_builder_info(void);
int run_scenario_read_terrain_buffer(void);
int run_scenario_read_num_fielded(void);

int run_scenario_tank_set_stocks(void);
int run_scenario_tank_kill(void);
int run_scenario_tank_teleport(void);
int run_scenario_tank_set_boat(void);
int run_scenario_tank_give_pill(void);
int run_scenario_tank_drop_pill(void);
int run_scenario_tank_arm_records(void);
int run_scenario_tank_death_ticks_respawn(void);

int run_scenario_lgm_dispatch(void);
int run_scenario_lgm_recall(void);
int run_scenario_lgm_kill(void);
int run_scenario_lgm_parachute(void);
int run_scenario_lgm_set_carried(void);
int run_scenario_lgm_kill_record(void);

int run_scenario_pill_set_owner(void);
int run_scenario_pill_set_armour(void);
int run_scenario_pill_set_speed(void);
int run_scenario_pill_move(void);
int run_scenario_base_set_owner(void);
int run_scenario_base_owner_keep_stock(void);
int run_scenario_base_set_stock(void);
int run_scenario_pill_base_arm_records(void);

/* The six entity arms (test_scenario_entity_arms.c). Adding and removing a
 * pillbox, a base or a start: the list chooses the slot and reports it, a
 * removal is a tombstone that keeps the slot and the count, and every change
 * goes out as a CTRL_ENTITY_CHANGE. */
int run_scenario_entity_add_pill(void);
int run_scenario_entity_remove_pill(void);
int run_scenario_removed_item_is_no_item(void);
int run_scenario_entity_add_base(void);
int run_scenario_entity_remove_base(void);
int run_scenario_entity_add_start(void);
int run_scenario_entity_remove_start(void);
int run_scenario_entity_publish(void);
int run_scenario_entity_add_out_null(void);

/* Pillbox armour above 15 (test_pill_armour_scale.c): the sixteen pictures
 * scaled across pill_max_armour, and the client capping the armour a server
 * states about a pill. */
int run_pill_armour_scale_classic_cap(void);
int run_pill_base_anger_radius(void);
int run_pill_armour_scale_raised_cap(void);
int run_pill_armour_scale_client_caps(void);

/* One pillbox removed and put back over the real loopback transport
 * (test_loopback_entity_change.c): the client's list follows the server's. */
int run_loopback_entity_change(void);

/* A scenario's changeover over the real loopback transport
 * (test_loopback_scenario_seat.c): the pillbox a human holds keeps its owner
 * while a held seat is fielded and taken off again, with no full sync in the
 * middle to cover a migration. */
int run_loopback_scenario_seat_keeps_pills(void);

/* A second player joining a lobby full of bots over the real loopback
 * transport (test_loopback_join_burst.c): the join replay's control burst
 * against the control channel's window, with every bot seat filled and with
 * two bots. */
int run_loopback_join_lobby_burst(void);
int run_loopback_join_small_lobby(void);

/* The rules table over the real loopback transport
 * (test_loopback_sim_rules.c): a mid-round change reaching a connected
 * client, a joiner arriving on the changed table, a server-only rule
 * publishing nothing, a new table clamping what the client holds, and the two
 * sides reading the same records after a cap drops. */
/* What a client does with a CTRL_SIM_RULES event it should not trust
 * (test_sim_rules_client_check.c): every carried rule driven outside its own
 * row, NaN and infinity rates, a carried pair, a pair the event only carries
 * half of, and the ordinary table still landing. */
int run_sim_rules_client_check_every_carried_field(void);
int run_sim_rules_client_check_nan_and_inf(void);
int run_sim_rules_client_check_attack_pair(void);
int run_sim_rules_client_check_server_only_pair(void);
int run_sim_rules_client_check_valid_applies(void);

int run_loopback_sim_rules_change(void);
int run_loopback_sim_rules_join(void);
int run_loopback_sim_rules_reclamp(void);
int run_loopback_sim_rules_agree(void);

int run_scenario_map_set_tile(void);
int run_scenario_map_fill_rect(void);
int run_scenario_map_fill_paced(void);
int run_scenario_map_fill_no_budget_refused(void);
int run_scenario_map_fill_dropped_at_round_start(void);
int run_scenario_map_fill_dropped_at_map_swap(void);
int run_scenario_map_fill_respects_event_buffer(void);
int run_scenario_map_place_mine(void);
int run_scenario_map_remove_mine(void);
int run_scenario_map_arm_records(void);

/* The six roster ops (test_scenario_roster_arms.c). Spawning and removing
 * queue and the sim makes one change a tick; the lobby three apply where
 * they stand and refuse for the reasons their command arms refuse. */
int run_scenario_roster_spawn_refusals(void);
int run_scenario_roster_spawn_lands(void);
int run_scenario_roster_spawn_paced(void);
int run_scenario_roster_team_during_add(void);
/* A spawn allies the new bot with its team on one CTRL_ALLIANCE_ACCEPT,
 * never the batched matrix, and with no newswire line; a spawn onto no team
 * publishes neither event. */
int run_scenario_roster_spawn_allies_team(void);
int run_scenario_roster_spawn_ally_is_quiet(void);
int run_scenario_roster_spawn_no_team_allies_nothing(void);
int run_scenario_roster_remove_bot(void);
int run_scenario_roster_set_team(void);
int run_scenario_lobby_add_bot(void);
int run_scenario_lobby_remove_bot(void);
int run_scenario_lobby_set_team(void);
int run_scenario_roster_bots_seat_past_the_human_cap(void);
int run_scenario_roster_spawn_named_start(void);

/* The loadout a spawn names for the one bot it builds: it fuels that tank
 * ahead of the spawn-loadout policy, it is answered with no policy there at
 * all, and it is spent on that tank rather than held for the seat. */
int run_scenario_roster_spawn_loadout_named(void);
int run_scenario_roster_spawn_loadout_without_policy(void);
int run_scenario_roster_spawn_loadout_not_next_life(void);

/* What the lobby says about its scenario (test_lobby_scenario_settings.c):
 * the settings body with and without one, byte for byte; the scripted game
 * type a commit sets and gives back, the same type on a server that booted
 * onto the map with no commit to set it, and the same again on a lobby the
 * last player has left; the settings a scenario does not go with; and the
 * control characters that never reach the event. */
int run_lobby_scenario_settings_plain_bytes(void);
int run_lobby_scenario_settings_scripted_bytes(void);
int run_lobby_scenario_settings_roundtrip(void);
int run_lobby_scenario_commit_sets_type(void);
int run_lobby_scenario_reset_keeps_rules(void);
int run_lobby_scenario_refuses_ranked(void);
int run_lobby_scenario_refuses_ai_none(void);
int run_lobby_scenario_refuses_game_type(void);
int run_lobby_scenario_boot_sets_type(void);
int run_lobby_scenario_identity_strips_controls(void);
int run_lobby_scenario_nolobby_boot_seats_template(void);

/* The policy a server holds for scripts players send it
 * (test_script_upload_policy.c): the word it is set from and the legacy flag
 * that stands for off, the spelling a preference is written in, its byte on
 * the lobby-settings event, and the copies the sim and the client keep. */
int run_script_upload_policy_resolve(void);
int run_script_upload_policy_word(void);
int run_script_upload_policy_codec(void);
int run_script_upload_policy_event(void);
/* Whether players may copy the server's scripts (same file): its byte on the
 * lobby-settings event, the sim's setting, and the client's copy. */
int run_script_sharing_codec(void);
int run_script_sharing_sim(void);
int run_script_sharing_client(void);

/* The lobby template (test_lobby_template.c): the engine seating a
 * scenario's teams where a lobby is built or rebuilt, reconciling one that
 * comes back from a round against what the host did to it, and leaving a
 * preview and a plain map alone. */
int run_lobby_template_map_commit_seats(void);
int run_survival_lobby_round(void);
int run_survival_lobby_round_full(void);
int run_survival_lobby_round_ds_order(void);
int run_loopback_unfield_tank(void);
int run_lobby_template_return_reconciles(void);
int run_lobby_template_return_unfields(void);
int run_lobby_template_reset_reseats(void);
int run_lobby_template_preview_is_inert(void);
int run_lobby_template_plain_map_clears(void);
int run_lobby_template_map_commit_drops_prior_bots(void);
int run_lobby_template_caps_humans_only(void);
int run_lobby_template_cap_seats_human_above_bots(void);
int run_lobby_template_cap_refuses_extra_human(void);
int run_lobby_template_cap_never_binds_bots(void);
int run_lobby_template_cancel_keeps_trim(void);
int run_lobby_template_cancel_keeps_empty_team(void);
int run_lobby_template_cancel_chain_rolls_back(void);
int run_lobby_template_commit_keeps_new_lobby(void);
int run_lobby_template_cancel_restores_path_inmem(void);
int run_lobby_template_cancel_restores_path_random(void);
int run_lobby_template_seat_carries_init(void);
int run_lobby_template_seat_carries_mode(void);
int run_lobby_template_mode_unknown_key_kept(void);
int run_lobby_template_no_mode_leaves_config(void);
int run_lobby_template_add_bot_takes_template(void);

/* The scripted game type (test_scripted_game_type.c): gameScripted resolving
 * through the base game the scenario declared, at the loadout and at the
 * start, the value going with the template when a plain map is committed,
 * and a client resolving it off the settings tail. */
int run_scripted_game_type_loadout_follows_base(void);
int run_scripted_game_type_no_base_is_strict(void);
int run_scripted_game_type_strict_ignores_base(void);
int run_scripted_game_type_start_follows_base(void);
int run_scripted_game_type_plain_map_clears_base(void);
int run_scripted_game_type_client_follows_settings(void);

/* The unfielded seat (test_unfielded_seat.c): a seat a bot holds with no
 * bot manager entry, no ClientSim and no tank behind it. What it counts for,
 * what the start sequence does with it, the two ops that field and unfield
 * it, and the byte that carries it to clients. */
int run_unfielded_seat_counts(void);
int run_unfielded_seat_is_empty(void);
int run_unfielded_seat_all_ready(void);
int run_unfielded_seat_start_skips_it(void);
int run_unfielded_seat_spawn_fields_it(void);
int run_unfielded_seat_survives_remove(void);
int run_unfielded_seat_unfield_is_quiet(void);
int run_unfielded_seat_unfield_keeps_sync(void);
int run_unfielded_seat_spawn_keeps_alliances(void);
int run_unfielded_seat_on_the_wire(void);

/* What a wave costs (test_scenario_wave_cost.c): the brain, the ClientSim and
 * the control subscription behind a held seat, built across the countdown
 * before the round is played, parked when the seat comes off the field and
 * handed back when it goes back on. Counted at the calls that do it, for one
 * seat and for six seats swapped twice, plus the four refields a parked
 * runner cannot serve, the three places it must not survive, and the warm
 * itself — every seat, a free first fielding, one seat it cannot serve, and a
 * countdown given up on before the round it was building for. */
int run_scenario_wave_cost_refield_resumes(void);
int run_scenario_wave_cost_seats_swap_counts(void);
int run_scenario_wave_cost_other_brain_rebuilds(void);
int run_scenario_wave_cost_brain_case_rebuilds(void);
int run_scenario_wave_cost_other_init_rebuilds(void);
int run_scenario_wave_cost_round_end_releases(void);
int run_scenario_wave_cost_all_parked_releases(void);
int run_scenario_wave_cost_rotation_releases(void);
int run_scenario_wave_cost_seat_leaving_releases(void);
int run_scenario_wave_cost_destroy_releases(void);
int run_scenario_wave_cost_countdown_warms_seats(void);
int run_scenario_wave_cost_warm_is_one_a_frame(void);
int run_scenario_wave_cost_warmed_field_is_free(void);
int run_scenario_wave_cost_warmed_seat_takes_session_dir(void);
int run_scenario_wave_cost_warmed_init_rebuilds(void);
int run_scenario_wave_cost_bot_init_keeps_park(void);
int run_scenario_wave_cost_template_init_warms(void);
int run_scenario_wave_cost_warm_skips_bad_brain(void);
int run_scenario_wave_cost_failed_build_leaves_nothing(void);
int run_scenario_wave_cost_abort_countdown_releases(void);
int run_scenario_wave_cost_all_ready_clears_skips(void);

/* The five comms ops (test_scenario_comms_arms.c). A line to the game, to a
 * team and to one player, with the destination filtered where the recipient
 * is; a sound at a square and a sound at no square; a console line. Plus the
 * two cases that keep the destination honest: the body decoder addresses a
 * rebuilt line to every recipient, and the in-process filter reads 0xFF as
 * everyone at a client that is not slot 0. */
int run_scenario_comms_msg_all(void);
int run_scenario_comms_msg_team(void);
int run_scenario_comms_msg_player(void);
int run_scenario_comms_sound(void);
int run_scenario_comms_log(void);
int run_scenario_comms_say(void);
/* THREE SHOTS = GO THERE — the shell-expiry detector behind the order
 * (tests/unit/test_three_shot_order.c). */
int run_three_shot_order_orders_allied_bots(void);
int run_three_shot_order_ignores_closed_ground(void);
int run_three_shot_order_needs_one_square(void);
int run_three_shot_order_window_and_reset(void);
int run_three_shot_order_needs_quiet_before(void);
int run_three_shot_order_quiet_counts_hits(void);
int run_three_shot_order_needs_quiet_after(void);
int run_three_shot_order_quiet_after_sees_a_shell_still_flying(void);
int run_three_shot_order_quiet_before_sees_a_shell_still_flying(void);

/* The detector's clock is the SERVER's tick, never the client input tick a
 * shell also carries, and an order can only be dropped on a real square. */
int run_three_shot_order_uses_the_server_tick(void);
int run_three_shot_order_server_tick_quiet_after(void);
int run_three_shot_order_ignores_a_forged_fire_tick(void);
int run_three_shot_order_rejects_off_map_squares(void);
int run_three_shot_order_scenario_refuses_off_map(void);
int run_scenario_comms_arm_records(void);
int run_scenario_comms_decoder_dest_player(void);
int run_scenario_comms_apply_non_zero_slot(void);

/* The two flow ops (test_scenario_flow_arms.c). A scripted end to the round,
 * the lobby line it leaves and the crediting it does not do; the game-time
 * change, the lengths it refuses and where the new length shows up. */
int run_scenario_flow_end_round(void);
int run_scenario_flow_end_round_resolve(void);
int run_scenario_flow_end_round_refusals(void);
int run_scenario_flow_set_game_time(void);
int run_scenario_flow_set_game_time_refusals(void);
int run_scenario_flow_arm_records(void);

/* The rules op (test_scenario_rule_arms.c). The index list against the table
 * it indexes, a rule written and read back, a rate the op's double carries
 * and an int32 could not, the two refusals and the table each leaves
 * untouched, the record the write puts in a recording, the records a lowered
 * cap brings down to it, and what the clamp leaves in the replay. */
int run_scenario_rule_index_matches_table(void);
int run_scenario_rule_set(void);
int run_scenario_rule_set_float(void);
int run_scenario_rule_refusals(void);
int run_scenario_rule_arm_records(void);
int run_scenario_rule_clamps_world(void);
int run_scenario_rule_clamp_records(void);

/* fill_to_caps (test_scenario_fill_caps.c). A mod that raises a cap and asks
 * for the map to start at it, for a base stock and for a pill's armour; the
 * same mod without the key, which raises the cap and moves nothing; and what
 * the fill moved, read back out of a recording. */
int run_scenario_fill_caps_raises_bases(void);
int run_scenario_fill_caps_raises_pills(void);
int run_scenario_fill_caps_off_changes_nothing(void);
int run_scenario_fill_caps_records(void);

/* The six lifecycle and lobby policy pointers
 * (test_scenario_policy_lifecycle.c). Where a tank starts, whether the base
 * sweep may end the round, whether the lobby may make another team, how many
 * people may join, what a spawning tank is handed and whether a dead tank may
 * come back — each driven at its own call site with a policy answering the
 * opposite of classic, and once more with nothing registered. */
int run_scenario_policy_choose_start(void);
int run_scenario_policy_choose_start_out_of_range(void);
int run_scenario_policy_allow_base_win(void);
int run_scenario_policy_allow_extra_teams(void);
int run_scenario_policy_team_set_extra_teams(void);
int run_scenario_policy_max_players(void);
int run_scenario_policy_spawn_loadout(void);
int run_scenario_policy_can_respawn(void);
int run_scenario_policy_null_is_classic(void);
int run_scenario_policy_hostile_answers(void);
int run_scenario_policy_named_start_outranks_choose_start(void);

/* The four combat policy pointers (test_scenario_policy_combat.c). What a
 * blow is worth, whether a square may be built on, whether an objective may
 * change hands and whether a blow may finish what it landed on — each at its
 * own call site with a policy answering the opposite of classic, the two kill
 * ops shown not to ask at all, and once more with nothing registered. */
int run_scenario_policy_damage_scale(void);
int run_scenario_policy_invulnerable_tank(void);
int run_scenario_policy_protected_builder(void);
int run_scenario_policy_protected_pill(void);
int run_scenario_policy_can_build(void);
int run_scenario_policy_can_capture(void);
int run_scenario_policy_kill_ops_ignore_can_die(void);
int run_scenario_policy_combat_null_is_classic(void);

/* The same rows answered from a script rather than from a vtable a case wrote
 * (test_scenario_policy_lua.c): the host's lookup, the arguments a script is
 * handed at each site, what a script that answers nil gets, the op a policy
 * may not issue while it is answering, and the raise that counts toward the
 * error limit. */
int run_scenario_policy_lua_allow_base_win(void);
int run_scenario_policy_lua_can_respawn(void);
int run_scenario_policy_lua_can_build(void);
int run_scenario_policy_lua_can_capture(void);
int run_scenario_policy_lua_announce(void);
int run_scenario_policy_lua_can_die(void);
int run_scenario_policy_lua_nil_is_classic(void);
int run_scenario_policy_lua_op_in_policy(void);
int run_scenario_policy_lua_error_counts(void);

/* And the three that carry a value out rather than answering a bool: the
 * start a script names, the loadout in both of its shapes, and the percent a
 * blow is priced at — each at its site, with nil, a raise, an answer the site
 * cannot use and no function at all reaching that policy's own classic
 * answer, which for damage_scale is a hundred and not a zero. */
int run_scenario_policy_lua_choose_start(void);
int run_scenario_policy_lua_spawn_loadout(void);
int run_scenario_policy_lua_damage_scale(void);
int run_scenario_policy_lua_value_classic(void);
int run_scenario_policy_lua_value_error_counts(void);
int run_scenario_policy_lua_value_in_policy(void);

/* The in-process game-event channel (test_game_event_channel.c): a subscriber
 * that asks for it hears the captures and the builder death on it rather than
 * on the control stream, with every byte of each event — the ones past
 * gameEventDataSize() included — where it has always been; one that asks for
 * nothing is unaffected; and a ClientSim raises none of the three. */
int run_game_event_channel_base_captured(void);
int run_game_event_channel_pill_captured(void);
int run_game_event_channel_lgm_lost(void);
int run_game_event_channel_control_only_subscriber(void);
int run_game_event_channel_client_emits_nothing(void);
/* The same two capture events after the payload grew: an objective handed to
 * nobody is published like a capture with no new owner and draws no client
 * line, the 0-based item index reaches the wire at both ends of the list, the
 * wire payload is four bytes with the fourth reserved, and the stats funnel
 * still finds the class and the square where they moved to. */
int run_game_event_channel_neutralised(void);
int run_game_event_channel_capture_index_base(void);
int run_game_event_channel_capture_wire_bytes(void);
int run_game_event_channel_neutralised_no_client_line(void);
int run_game_event_channel_capture_attribution(void);
int run_game_event_channel_subscriber_count(void);

/* The eight facts that had no event at all (test_game_events_new.c): a tank
 * spawning and respawning, a builder landing, a pill placed, picked up and
 * killed, a building job finished from each arm, a mine laid by a builder and
 * by a tank, and a mine going up carrying who laid it. Plus the one that must
 * never be serialized: a hidden mine reaches the host and the god-view
 * recording build and no client's snapshot. */
int run_game_events_tank_spawned(void);
int run_game_events_lgm_landed(void);
int run_game_events_pill_placed(void);
int run_game_events_pill_picked_up(void);
int run_game_events_pill_killed(void);
int run_game_events_built(void);
int run_game_events_mine_laid(void);
int run_game_events_mine_exploded(void);
int run_game_events_mine_placed_is_local(void);

/* The announce policy (test_scenario_announce.c): no policy is the classic
 * newswire; each kind is asked with the subject and actor its row names; a
 * refusal stamps the quiet byte on every fact that carries one; every client
 * line site reads that byte; the byte survives the wire, composed by hand; a
 * pill capture draws one line now that pillsSetPillOwner draws none; a vote
 * line is held back by not being sent; and a real loopback client receiving
 * the byte writes nothing. */
int run_scenario_announce_null_policy_is_classic(void);
int run_scenario_announce_policy_asked_per_kind(void);
int run_scenario_announce_quiet_stamped_on_every_fact(void);
int run_scenario_announce_client_lines_read_the_byte(void);
int run_scenario_announce_wire_bytes(void);
int run_scenario_announce_pill_line_written_once(void);
int run_scenario_announce_vote_line_held(void);
int run_scenario_announce_loopback_quiet_draws_no_line(void);

int run_scenario_host_metadata(void);
int run_scenario_host_rules_change_round(void);
int run_scenario_host_syntax_error_line(void);
int run_scenario_host_unknown_rule_key(void);
int run_scenario_host_api_too_new(void);
int run_scenario_host_no_script(void);
int run_scenario_host_manifest_roundtrip(void);
int run_scenario_host_trigger_manifest(void);
int run_scenario_host_trigger_where_type(void);
int run_scenario_host_trigger_action_no_op(void);
int run_scenario_host_trigger_where_no_field(void);
int run_scenario_host_trigger_text_cut(void);
int run_scenario_host_trigger_array_hole(void);
int run_scenario_host_trigger_no_when(void);
int run_scenario_host_team_init_read(void);
int run_scenario_host_seed_reproducible(void);
int run_scenario_host_edit_after_attach(void);
int run_scenario_host_reload_picks_up_edit(void);
int run_scenario_host_reload_bad_syntax(void);
int run_scenario_host_reload_bad_api(void);
int run_scenario_host_reload_applies_nothing(void);

/* The round's lifecycle (test_scenario_host.c): the two lifecycle
 * calls, a fresh set of globals per round, the error limit at its
 * boundary, the recursion-aware VM lock from one thread and from two,
 * and the roster audit. */
int run_scenario_host_fresh_globals_per_round(void);
int run_scenario_host_setup_in_window(void);
int run_scenario_host_start_on_first_running_tick(void);
int run_scenario_host_error_limit_boundary(void);
int run_scenario_host_disabled_stops_hooks(void);
int run_scenario_host_vm_lock_same_thread(void);
int run_scenario_host_vm_lock_second_thread(void);
int run_scenario_host_audit_human_lost(void);
int run_scenario_host_failed_start_drops_manifest(void);
int run_scenario_host_round_answers_its_own_start(void);
int run_scenario_host_opening_tank_under_rules(void);
int run_scenario_host_boot_failure_still_starts(void);
int run_scenario_host_round_after_scenario_is_classic(void);
int run_scenario_host_tag_follows_switch(void);
int run_scenario_host_chunk_events_reach_hooks(void);

/* A metatable on the script's own tables (test_scenario_host.c): what the
   host's reads of the declared data do and do not run. */
int run_scenario_host_metatable_raises(void);
int run_scenario_host_metatable_not_read(void);
int run_scenario_host_hook_via_global_metatable(void);
int run_scenario_host_script_env_is_its_own(void);
int run_scenario_host_manifest_read_from_its_own_env(void);
int run_scenario_host_manifest_reads_workshop(void);
int run_scenario_host_errors_counted_per_script(void);
int run_scenario_host_many_rules_all_applied(void);

/* What a file that declared scenario.kind = "mod" may not do
 * (test_scenario_host.c): the rows it is held back from at run time, the
 * same rows written down, and the four things the key itself can say. */
int run_scenario_host_mod_round_op_raises(void);
int run_scenario_host_mod_load_refusals(void);
int run_scenario_host_kind_word(void);

/* The scripts-off switch (test_scenario_host.c): what an attach does with
   it off, with and without a script beside the map, and with it back on. */
int run_scenario_host_disabled_refuses_script(void);
int run_scenario_host_disabled_plain_map(void);
int run_scenario_host_enabled_again(void);

/* The validator (test_scenario_validate.c): a script read in a stub VM and
 * checked against the map, the lobby template and the rule catalogue, each
 * problem carrying the key it is against and the line it is on. */
int run_scenario_validate_clean(void);
int run_scenario_validate_api_too_new(void);
int run_scenario_validate_lobby_shape(void);
int run_scenario_validate_team_init_reported(void);
int run_scenario_validate_unknown_rule(void);
int run_scenario_validate_rule_out_of_range(void);
int run_scenario_validate_rule_pair(void);
int run_scenario_validate_tag_past_map(void);
int run_scenario_validate_fifth_tag(void);
int run_scenario_validate_region_off_map(void);
int run_scenario_validate_bound_false_with_tags(void);
int run_scenario_validate_syntax_error_line(void);
int run_scenario_validate_lines_point_at_the_key(void);
int run_scenario_validate_wave_defense(void);
int run_scenario_validate_unknown_game(void);
int run_scenario_validate_source_syntax_error(void);
int run_scenario_validate_source_bad_key(void);
int run_scenario_validate_source_matches_file(void);
int run_scenario_validate_source_pushed_manifest(void);
int run_scenario_validate_source_pushed_conflict(void);
int run_scenario_validate_source_rule_range(void);
int run_scenario_validate_source_pushed_triggers(void);
int run_scenario_validate_trigger_caps(void);
int run_scenario_validate_trigger_hook(void);
int run_scenario_validate_trigger_field(void);
int run_scenario_validate_trigger_operator(void);
int run_scenario_validate_trigger_action(void);
int run_scenario_validate_trigger_unknown_operator(void);
int run_scenario_validate_trigger_action_field(void);
int run_scenario_validate_trigger_call_args(void);
int run_scenario_validate_trigger_call_no_name(void);
int run_scenario_validate_trigger_field_team(void);
int run_scenario_validate_trigger_announce_clear(void);
int run_scenario_validate_trigger_arg_literal(void);
int run_scenario_validate_rule_pair_key(void);

/* The binding table (test_scenario_lua.c): every row of the registry
 * called once, the three index rules, the nils an absent entity reads
 * as, the whole-map string, a shape error against the error limit, and
 * the rules, tags and regions a script declares read back. */
int run_scenario_lua_every_row_answers(void);
int run_scenario_lua_op_arguments_match_the_doc(void);
int run_scenario_lua_read_index_passes_through(void);
int run_scenario_lua_op_index_subtracts_one(void);
int run_scenario_lua_script_index_adds_one(void);
int run_scenario_lua_absent_reads_are_nil(void);
int run_scenario_lua_terrain_is_the_whole_map(void);
int run_scenario_lua_shape_error_counts(void);
int run_scenario_lua_rule_reads_the_table(void);
int run_scenario_lua_tags_and_regions(void);
int run_scenario_lua_refusals_in_round(void);
int run_scenario_lua_refusals_in_lobby(void);
int run_scenario_lua_op_index_reaches_the_payload(void);
int run_scenario_lua_add_answers_its_index(void);
int run_scenario_lua_queued_answers_queued(void);
int run_scenario_lua_shape_raises_refusal_does_not(void);
int run_scenario_lua_detail_carries_the_number(void);
int run_scenario_lua_teleport_start_refuses_bad_index(void);
int run_scenario_lua_spawn_bot_refuses_bad_start(void);
int run_scenario_lua_game_type_resolves_scripted(void);
int run_scenario_lua_panel_builds_bytes(void);
int run_scenario_lua_panel_words_and_numbers(void);
int run_scenario_lua_panel_refusals(void);
int run_scenario_lua_presentation_targets(void);
int run_scenario_lua_score_and_announce(void);
int run_scenario_lua_acting_rows_refuse_a_check(void);

/* The state a scenario runs in (test_scenario_sandbox.c): the names the
 * whitelist takes and the ones it keeps, the precompiled chunk the loader
 * refuses, collectgarbage without "stop", print on the server console, the
 * memory cap a script is refused at, the instruction budget one call is cut
 * off at, the two catchers that are not allowed to keep the error it raises,
 * the os.date format that is read before it reaches strftime, and the console
 * lines one call may print, the strings the library's C functions may build
 * or search, and the instructions one tick's calls may spend between them. */
int run_scenario_sandbox_removed_names_are_nil(void);
int run_scenario_sandbox_bytecode_chunk_refused(void);
int run_scenario_sandbox_collectgarbage_stop_refused(void);
int run_scenario_sandbox_print_reaches_the_console(void);
int run_scenario_sandbox_memory_cap_refuses(void);
int run_scenario_sandbox_state_survives_a_refusal(void);
int run_scenario_sandbox_instruction_budget_cuts_a_loop(void);
int run_scenario_sandbox_budget_is_per_call(void);
int run_scenario_sandbox_state_survives_the_budget(void);
int run_scenario_sandbox_budget_survives_a_nested_call(void);
int run_scenario_sandbox_budget_survives_a_pcall(void);
int run_scenario_sandbox_budget_survives_a_coroutine(void);
int run_scenario_sandbox_os_date_refuses_a_bad_format(void);
int run_scenario_sandbox_print_bounded_in_one_call(void);
int run_scenario_sandbox_print_allowance_returns(void);
int run_scenario_sandbox_string_cap_on_results(void);
int run_scenario_sandbox_string_cap_on_subjects(void);
int run_scenario_sandbox_tick_budget_cuts_a_drain(void);
int run_scenario_sandbox_tick_budget_returns(void);
int run_scenario_sandbox_tick_budget_survives_a_pcall(void);
int run_scenario_sandbox_tick_budget_spares_on_end(void);
int run_scenario_sandbox_tick_budget_switches_off(void);
int run_scenario_sandbox_pattern_bomb_stopped(void);
int run_scenario_sandbox_pattern_bomb_behind_pcall(void);
int run_scenario_sandbox_pattern_results(void);
int run_scenario_sandbox_pattern_charge_counts(void);
int run_scenario_sandbox_interpreted_cost(void);
int run_scenario_sandbox_tick_stats_recorded(void);
int run_scenario_sandbox_unsafe_opens_full_library(void);
int run_scenario_sandbox_unsafe_lifts_the_budgets(void);
int run_scenario_sandbox_unsafe_loads_bytecode(void);
int run_scenario_sandbox_unsafe_reaches_the_lobby(void);

/* Hostile scripts (test_scenario_hostile.c): one case per known way out of
 * the sandbox, each refused or switched off with the server still ticking. */
int run_scenario_hostile_endless_loop(void);
int run_scenario_hostile_memory_bomb(void);
int run_scenario_hostile_file_open(void);
int run_scenario_hostile_process_call(void);
int run_scenario_hostile_bytecode_chunk(void);
int run_scenario_hostile_debug_call(void);
int run_scenario_hostile_ffi_call(void);
int run_scenario_hostile_string_bomb(void);
int run_scenario_hostile_pattern_bomb(void);
int run_scenario_hostile_string_metatable_rewrite(void);
int run_scenario_hostile_op_flood(void);
int run_scenario_hostile_message_flood(void);
int run_scenario_hostile_hook_across_ticks(void);
int run_scenario_hostile_loop_behind_pcall(void);
int run_scenario_hostile_loop_behind_coroutine_resume(void);
int run_scenario_hostile_print_forged_line(void);

/* The bus events (test_scenario_events.c): the subscriber that only
 * queues, the bounded drain at the end of each tick, and what a full
 * queue does with the event that finds no room. */
int run_scenario_events_queued_then_drained(void);
int run_scenario_events_drain_reads_the_length_once(void);
int run_scenario_events_queued_during_a_drain_waits(void);
int run_scenario_events_overflow_boundary(void);
int run_scenario_events_overflow_counts_errors(void);
int run_scenario_events_no_scenario_delivers_nothing(void);
int run_scenario_events_lobby_tick_drains(void);
int run_scenario_events_control_reaches_the_queue(void);
int run_scenario_events_both_channels_in_publish_order(void);
int run_scenario_events_channel_says_which(void);
int run_scenario_events_overflow_covers_both(void);

/* The hooks (test_scenario_hooks.c): what a drained event becomes, the
 * payload each hook is handed, the mark that says whether the scenario
 * caused the fact, and the two lifecycle calls the drain makes. */
int run_scenario_hooks_every_hook_from_its_event(void);
int run_scenario_hooks_fire_one_tick_later(void);
int run_scenario_hooks_scripted_both_ways(void);
int run_scenario_hooks_spawn_drain_is_scripted(void);
int run_scenario_hooks_team_changed_on_difference(void);
int run_scenario_hooks_tick_and_end(void);
int run_scenario_hooks_error_counts_and_disables(void);
int run_scenario_hooks_trigger_runs_after_author(void);
int run_scenario_hooks_trigger_table_form_handler(void);
int run_scenario_hooks_trigger_stopped_by_false(void);
int run_scenario_hooks_trigger_shadowed_base(void);
int run_scenario_hooks_trigger_where_both_ways(void);
int run_scenario_hooks_trigger_call_reaches_script(void);
int run_scenario_hooks_router_matches_source(void);
int run_scenario_hooks_trigger_team_from_owner(void);
int run_scenario_hooks_trigger_tag_on_item(void);
int run_scenario_hooks_trigger_tag_ne_and_eq(void);
int run_scenario_hooks_trigger_region_holds_square(void);
int run_scenario_hooks_trigger_unknown_operator(void);
int run_scenario_hooks_trigger_on_policy_skipped(void);

/* What the host derives rather than hears (test_scenario_derived.c): the
 * timers a script sets, the regions it names, and the enter and leave hooks
 * that come from watching where the tanks are. */
int run_scenario_derived_timer_fires_on_its_tick(void);
int run_scenario_derived_timer_cancelled_and_stale(void);
int run_scenario_derived_timer_limit_boundary(void);
int run_scenario_derived_timers_die_with_the_round(void);
int run_scenario_derived_region_enter_and_leave(void);
int run_scenario_derived_define_region_adds_replaces_and_expires(void);
int run_scenario_derived_region_loop_terminates(void);
int run_scenario_derived_fixture_wins_without_on_tick(void);

/* The function catalogue (test_scenario_functions.c): the rows that name
 * every hook and every policy an author writes, the fields a trigger may
 * test on one, and whether the parameter list a row claims is the one the
 * host pushes. */
int run_scenario_functions_table(void);
int run_scenario_functions_fields(void);
int run_scenario_functions_match_dispatch(void);

/* The line beside each function (test_scenario_fndesc.c): a description for
 * every catalogue row and none for anything else, the lang ids behind them,
 * and the stub the editor inserts for a function that has not been written
 * yet. */
int run_scenario_fndesc_table(void);
int run_scenario_fnstub_forms(void);

/* The definitions a script already holds (test_scenario_fnscan.c): the four
 * spellings a definition takes, the line each is on, and what a line scan
 * over the text does not see. The second is which spelling wrote each one,
 * since the host reaches only a global and a field of the scenario table. */
int run_scenario_fnscan_forms(void);
int run_scenario_fnscan_spellings(void);

/* The WBSC container (test_scenario_package.c): the framing round trip,
 * the refusals a malformed buffer gets, the entry and brain lists, two
 * containers open at the same time, and the cap an entry is measured against
 * before it is read. */
int run_scenario_package_round_trip(void);
int run_scenario_package_bad_framing(void);
int run_scenario_package_entry_names(void);
int run_scenario_package_two_open(void);
int run_scenario_package_entry_cap(void);

/* manifest.json (test_scenario_manifest_json.c): the schema into the struct
 * and back out with the keys this build does not read kept, the refusals a
 * malformed manifest gets, the comparison that holds a manifest against the
 * table a script declared, and a team's init table read the way the Lua
 * reader reads it. */
int run_scenario_manifest_json_round_trip(void);
int run_scenario_manifest_json_refusals(void);
int run_scenario_manifest_agrees(void);
int run_scenario_manifest_from_values(void);
int run_scenario_manifest_json_team_init(void);
int run_scenario_manifest_json_number_range(void);
int run_scenario_manifest_json_kind(void);
int run_scenario_manifest_json_triggers(void);
int run_scenario_manifest_json_trigger_operator(void);
int run_scenario_manifest_json_trigger_where_type(void);
int run_scenario_manifest_json_trigger_no_when(void);
int run_scenario_manifest_json_trigger_action_no_op(void);
int run_scenario_manifest_json_trigger_where_no_field(void);
int run_scenario_manifest_json_trigger_text_cut(void);
/* The Workshop item and its author: read as digit strings and written back
 * as them, and the comparison that lets a table stating none agree with a
 * manifest that names one. */
int run_scenario_manifest_workshop_keys(void);
int run_scenario_manifest_agrees_workshop(void);

/* The Workshop item stamped into a scenario file that already exists
 * (test_scenario_workshop_id.c): a .scenario package and a packed map, with
 * everything but the manifest kept, and a loose script and a plain map
 * refused. */
int run_scenario_io_set_workshop_id(void);

/* Where a map file's map data ends (test_scenario_map_body.c): the measure
 * itself, the container found after it, the scripted tag it gives the
 * chooser, and the preview that stops at it. */
int run_scenario_map_body_length(void);
int run_scenario_map_find_container(void);
int run_scenario_map_has_script_chunk(void);
int run_scenario_map_preview_truncates(void);
int run_scenario_map_preview_passes_plain_bytes(void);
int run_scenario_map_body_use_local_compare(void);
int run_scenario_map_has_script_cached(void);

/* A scenario carried inside the map file (test_scenario_packed_map.c): the
 * container's script run in place of a loose one, the loose script that
 * overrides it, the manifest handed to a script that declares no table, the
 * refusal one that restates it and disagrees gets, and the round start that
 * hands the table over again. */
int run_scenario_packed_map_script_runs(void);
int run_scenario_packed_map_loose_overrides(void);
int run_scenario_packed_map_script_omits_table(void);
int run_scenario_packed_map_table_disagrees(void);
int run_scenario_packed_map_round_start_keeps_it(void);
int run_scenario_packed_map_upload_switch(void);
int run_scenario_packed_map_script_upload_policy(void);
int run_scenario_packed_map_team_init(void);

/* Writing a map's scenario into the map (test_scenario_pack.c): the container
 * a loose script packs into, the manifest that comes out of the script's own
 * table, the second pack that replaces the first rather than following it,
 * and the map with nothing to pack that is left alone. */
int run_scenario_pack_writes_container(void);
int run_scenario_pack_manifest_agrees(void);
int run_scenario_pack_replaces_trailer(void);
int run_scenario_pack_refuses_unscripted(void);
int run_scenario_pack_script_mod(void);
int run_scenario_pack_script_refusals(void);

/* The brain a scenario names (test_scenario_brain_name.c): the name a team
 * writes reaching the seat as that brain's init.lua, the name this server has
 * not got falling back to the server's own brain and being reported, the path
 * written where a name belongs being refused, and the two roster ops that
 * resolve a name of their own. */
int run_scenario_brain_name_resolves(void);
int run_scenario_brain_name_missing(void);
int run_scenario_brain_name_rejects_path(void);
int run_scenario_brain_op_resolves(void);
int run_scenario_brain_name_op_missing_refused(void);
int run_scenario_brain_name_mode_falls_back(void);
int run_scenario_brain_name_mode_no_brain(void);

/* The scenarios directory (test_scenario_dir.c): a .scenario package listed
 * from its manifest with no Lua run, a loose .lua listed through the
 * validator's stub VM, what is skipped, a file one directory down left out,
 * the second reading of an unchanged directory answered from the cache
 * without booting a VM, and the list encoded into the SCENARIO_LIST_RSP shape
 * against committed golden bytes and decoded back. */
int run_scenario_dir_lists_package(void);
int run_scenario_dir_lists_loose_script(void);
int run_scenario_dir_skips_junk(void);
int run_scenario_dir_skips_subdirectory(void);
int run_scenario_dir_list_cached(void);
int run_scenario_dir_merges_shipped_mods(void);
int run_scenario_dir_entry_roundtrip(void);
int run_scenario_dir_chunk_not_in_flight(void);

/* A script's details (test_scenario_callbacks.c): the manifest's callbacks
 * block kept, cut and warned about at load, the rules and callbacks packed
 * into one blob per file, fetched by a client one file at a time over the
 * loopback transport (a directory mod, the Survival and Soccer maps' own
 * scripts, a file nobody has, a request dropped and asked again, a server
 * that never answers), and which of two mods wins a rule both set in either
 * list order. */
int run_scenario_callbacks_manifest(void);
int run_scenario_callbacks_over_cap(void);
int run_scenario_callbacks_json(void);
int run_scenario_details_blob(void);
int run_scenario_details_fetch_dir_mod(void);
int run_scenario_details_fetch_survival(void);
int run_scenario_details_fetch_soccer(void);
int run_scenario_details_fetch_not_found(void);
int run_scenario_details_fetch_retry(void);
int run_scenario_details_fetch_give_up(void);
int run_scenario_details_override_order(void);
int run_scenario_details_reload_map_script(void);

/* A copy of one of the server's scripts (test_script_fetch.c): the request
 * body against committed hex, a .lua, a .scenario and a shipped mod fetched
 * whole over the loopback transport, each refusal the server answers, a
 * BUSY answer asked again, a server that never answers, the client's bulk
 * sink refusing a header that is not its answer, a copy that stops arriving,
 * and a round started while a copy is arriving. */
int run_script_fetch_req_golden(void);
int run_script_fetch_found_lua(void);
int run_script_fetch_found_package(void);
int run_script_fetch_found_shipped(void);
int run_script_fetch_not_found(void);
int run_script_fetch_disabled(void);
int run_script_fetch_too_large(void);
int run_script_fetch_busy_retry(void);
int run_script_fetch_give_up(void);
int run_script_fetch_sink_bound(void);
int run_script_fetch_stall_fails(void);
int run_script_fetch_round_start_abort(void);

/* The copy a player saves to their own Mods directory
 * (test_script_save_local.c): a .lua and a .scenario written whole through
 * a temporary file, a name already there in either case refused, a name
 * that is not a bare script file name refused, and a Mods directory made
 * when it is missing. */
int run_script_save_local_ok(void);
int run_script_save_local_exists(void);
int run_script_save_local_bad_name(void);
int run_script_save_local_creates_dir(void);

/* The three directories a mod can come from (test_scenario_mod_dirs.c): the
 * one the host was given, the player's own under SDL_GetPrefPath and the
 * mods that ship beside the executable, merged into one listing with the
 * order of that list as the precedence. */
int run_scenario_mod_dirs_user_dir_offered(void);
int run_scenario_mod_dirs_shipped_offered(void);
int run_scenario_mod_dirs_configured_wins(void);
int run_scenario_mod_dirs_user_beats_shipped(void);
int run_scenario_mod_dirs_merged_and_sorted(void);
int run_scenario_mod_dirs_same_dir_once(void);
int run_scenario_mod_dirs_all_missing_is_quiet(void);
int run_scenario_mod_dirs_attach_reads_shipped(void);
/* The Workshop directory between the player's own and the shipped one: its
 * place in the precedence, its rows' source in both listings, and the local
 * path, save and upload checks that read it. */
int run_scenario_mod_dirs_workshop_precedence(void);
int run_scenario_mod_dirs_workshop_local(void);
int run_scenario_mod_dirs_workshop_upload_clash(void);

/* The Workshop sync (test_workshop_sync.c): what an item folder holds, and
 * what a pass copies, indexes and removes in the Workshop directory. */
int run_workshop_sync_classify(void);
int run_workshop_sync_copies_and_indexes(void);
int run_workshop_sync_removes_unsubscribed(void);
int run_workshop_sync_unavailable_touches_nothing(void);
int run_workshop_sync_name_clash(void);
int run_workshop_sync_renamed_content(void);
int run_workshop_sync_bumps_script_dirs_gen(void);
int run_workshop_sync_disabled_item_left_alone(void);
int run_workshop_sync_recovers_bad_index(void);
int run_workshop_map_package_info(void);
int run_workshop_pack_loose_script(void);
int run_workshop_sync_index_rows(void);
int run_local_rows_carry_author(void);

/* Which scenario plays when a map and a mod both have a claim
 * (test_scenario_precedence.c): the three rules, the four points the
 * template is applied at, and the game a round is played by. */
int run_scenario_precedence_mod_over_map(void);
int run_scenario_precedence_none_restores_map(void);
int run_scenario_precedence_plain_map_keeps_mod(void);
int run_scenario_precedence_template_seats(void);
int run_scenario_precedence_reset_reapplies(void);
int run_scenario_precedence_reload_reseats(void);
int run_scenario_precedence_no_game_plays_strict(void);
int run_scenario_precedence_open_game_plays_open(void);
int run_scenario_precedence_reload_refuses_bound(void);

/* A round that plays more than one script at once
 * (test_scenario_compose.c): a map's own scenario with two mods behind
 * it, what the composite carries from each, what a pick does and does
 * not take off, and a reload of the whole list. Then what a composed
 * round does at the calls the host makes into it: the three rows a mod
 * is refused and its base is not, a policy-only mod asked behind a
 * scenario, two scripts on one predicate, two halving one blow, and the
 * one policy the list is not asked. And what two scripts asking for one
 * thing come to: a rule the later one wins and a rule only the earlier
 * one set, a region name each keeps its own rectangle under, the enter
 * hook staying inside the script that named it, and the record of both
 * clashes. And the map own script composed where the host list puts it,
 * with every region keeping the bit it had at the front. */
int run_scenario_compose_map_and_mods(void);
int run_scenario_compose_keeps_map_scenario(void);
int run_scenario_compose_mod_rules_load(void);
int run_scenario_compose_region_clash_loads(void);
int run_scenario_compose_reload_list(void);
int run_scenario_compose_mod_guard(void);
int run_scenario_compose_policy_mod_asked(void);
int run_scenario_compose_policy_any_false(void);
int run_scenario_compose_policy_damage_scale(void);
int run_scenario_compose_policy_base_win(void);
int run_scenario_compose_policy_first_answer(void);
int run_scenario_compose_rules_first_wins(void);
int run_scenario_compose_rules_keeps_earlier(void);
int run_scenario_compose_region_own_first(void);
int run_scenario_compose_region_borrowed(void);
int run_scenario_compose_region_hook_own(void);
int run_scenario_compose_conflicts_recorded(void);
int run_scenario_compose_map_script_placed(void);
int run_scenario_compose_library_copy_per_script(void);
int run_scenario_compose_game_copy_per_script(void);
int run_scenario_compose_game_nested_copy(void);
int run_scenario_compose_compat_write_stays_local(void);
int run_scenario_compose_pairs_game_complete(void);
int run_scenario_compose_unsafe_keeps_sharing(void);

/* The panel's display list (test_scenario_panel.c): the byte layout
 * decoded from a hand-written list, the refusal each malformed list
 * gets, the caps at their edges, and a decoded list written back out
 * and parsed again. */
int run_scenario_panel_parses_each_primitive(void);
int run_scenario_panel_refuses_malformed(void);
int run_scenario_panel_boundaries(void);
int run_scenario_panel_roundtrip(void);
/* And the timer primitive's text: the tick difference the drawer turns into
 * minutes and seconds, held to exact strings with no renderer behind it. */
int run_scenario_panel_timer_text(void);
/* And whether a scenario's announcement is still on screen, and for how
 * long — the other piece of the presentation's arithmetic with no renderer
 * in it. */
int run_scenario_announce_remaining(void);
/* The tablet UI's scenario panel square (test_scenario_panel_slot.c): a
 * quarter of the screen's shorter side in the game view's top-right, on a
 * phone, a tablet, and views too small for the full side. */
int run_scenario_panel_slot_rect(void);
/* The names and the panel parser the log viewer compiles in
 * (test_lv_sim_rules_names.c): a rule's name from its index and back, and
 * a hand-written two-primitive list through scnPanelParse. */
int run_lv_rule_names_and_panel_parse(void);
/* The scripted game type in the log viewer (test_lv_scripted_game_type.c):
 * a recorded round set to gameScripted decodes with header game type 4, and
 * a hand-written settings payload with byte 7 = 4 decodes to the same. */
int run_lv_scripted_game_type_header(void);
int run_lv_scripted_game_type_settings(void);
/* The scripts.json member a scripted round's recording carries
 * (test_scripts_record.c): written for a scenario and a mod in load order,
 * absent for a plain round and for a plain round after a scripted one, the
 * sim's cap on the text, and the writer on a hand-built description. */
int run_scripts_record_scripted_round(void);
int run_scripts_record_plain_round(void);
int run_scripts_record_scripted_then_plain(void);
int run_scripts_record_detach_before_stop(void);
int run_scripts_record_setter_cap(void);
int run_scripts_record_json_write(void);
/* The log viewer reading scripts.json into its LvScripts holder
 * (test_lv_scripts_json.c, with the scripted round recorded by
 * lv_scripts_fixture.c): a scripted round read in load order, a plain round
 * and an old recording read as none, and a malformed, wrong-version,
 * over-long or over-cap member held to what the holder takes. */
int run_lv_scripts_json_scripted_round(void);
int run_lv_scripts_json_plain_round(void);
int run_lv_scripts_json_old_recording(void);
int run_lv_scripts_json_malformed(void);
int run_lv_scripts_json_wrong_version(void);
int run_lv_scripts_json_hostile_values(void);
int run_lv_scripts_json_over_cap(void);
int run_lv_scripts_json_close_clears(void);
/* The log viewer's rules at the playhead (test_lv_rule_changes.c, with the
 * rule-change round recorded by lv_scripts_fixture.c): a mid-round change
 * collected at load, LvRules right after seeking either way, a plain round
 * on the classic values, hostile log_RuleSet records consumed and ignored in
 * a file and on a live feed, and the pill picture scaled to the cap. */
int run_lv_rule_changes_collected(void);
int run_lv_rule_changes_seek(void);
int run_lv_rule_changes_plain_round(void);
int run_lv_rule_changes_hostile(void);
int run_lv_rule_changes_live_feed(void);
int run_lv_rule_changes_armour_levels(void);
int run_lv_rule_changes_live_same_tick(void);
/* The log viewer's scenario panels, scores, announcement and markers at the
 * playhead (test_lv_presentation.c, with the presentation round recorded by
 * lv_scripts_fixture.c): a hand-written log played through and seeked both
 * ways, hostile records consumed and ignored, a recorded scripted round, a
 * rebuild over thousands of records and a plain round with every store
 * empty; the row the panel draws of the everyone, team and slot rows; and a
 * slot's team from log_TeamSet, kept across seeks with no line posted by a
 * rebuild; announcements posted to the newswire once by playback and never
 * by a rebuild; and which markers the followed player sees. */
int run_lv_presentation_seek(void);
int run_lv_presentation_hostile(void);
int run_lv_presentation_scripted_round(void);
int run_lv_presentation_rebuild_many(void);
int run_lv_presentation_plain_round(void);
int run_lv_presentation_panel_choice(void);
int run_lv_presentation_slot_team(void);
int run_lv_presentation_announce_posts(void);
int run_lv_presentation_marker_visible(void);
int run_lv_presentation_live_seek(void);
int run_lv_presentation_live_fast_forward(void);
/* The server's game tick at the playhead (test_lv_server_tick.c): a recorded
 * round's tick at end-of-log, hand-written anchors either side of a snapshot
 * and across a new round's run, records of the wrong length, a recording from
 * before the record, events and empty ticks alternating, and a live feed. */
int run_lv_server_tick_recorded(void);
int run_lv_server_tick_anchors(void);
int run_lv_server_tick_hostile(void);
int run_lv_server_tick_old_recording(void);
int run_lv_server_tick_alternating(void);
int run_lv_server_tick_live_feed(void);
/* Where the log viewer draws a declared region (test_lv_region_rect.c): at the
 * overview's edges, off it, and on the game view at 2x with a pan. */
int run_lv_region_rect_placement(void);

/* The four presentation control events (test_scenario_presentation_codec.c):
 * their body codecs against hand-written bytes, the refusals a short or
 * overrunning body gets, the broadcast recipient pair every decoder sets,
 * and the in-process filter the client applies before it stores. */
int run_scn_presentation_codec_bodies(void);
int run_scn_presentation_codec_refuses_short(void);
int run_scn_presentation_decoder_sets_broadcast(void);
int run_scn_presentation_client_filters(void);

/* The four presentation ops (test_scenario_presentation_arms.c): the panel
 * list published, recorded, replayed to a joiner and pared back to the
 * everyone-addressed lists in the spectator ring's snapshot, the coalescing
 * key, every refusal, and the score, announcement and marker arms; the
 * markers and scores replayed to a joiner, in the ring's snapshot, after the
 * reset, and to a late joiner over the loopback transport. */
int run_scn_arm_panel_publishes_and_records(void);
int run_scn_arm_panel_refusals(void);
int run_scn_arm_panel_one_update_per_tick(void);
int run_scn_arm_panel_replayed_to_joiner(void);
int run_scn_arm_panel_snapshot_bounded(void);
int run_scn_arm_score_announce_marker(void);
int run_scn_arm_markers_scores_replayed_to_joiner(void);
int run_scn_arm_markers_scores_snapshot(void);
int run_scn_arm_markers_scores_reset(void);
int run_scn_markers_scores_loopback_late_join(void);

/* The rules a scenario's manifest sets (test_scenario_rules_codec.c,
 * test_scenario_rules_published.c, test_scenario_rules_reaches_joiner.c):
 * the body codec against hand-written bytes and every refusal a malformed
 * body gets; the set published on an attach, emptied on a detach and never
 * written on a map that has never had a scenario; and the replay that hands
 * it to a client registering after the attach. */
int run_scenario_rules_codec(void);
int run_scenario_rules_published(void);
int run_scenario_rules_reaches_joiner(void);

/* A set too big for one control segment (test_scenario_rules_fragments.c):
 * the fragments a full set publishes, each through the body codec at the
 * capacity the delivery path really hands it and back into a client; and
 * what a reader holds between two fragments and after a stream is cut. */
int run_scenario_rules_fragments(void);
int run_scenario_rules_fragments_partial(void);

/* The bot hint (test_scenario_hint.c): the table a fixture brain gets back
 * whole, every refusal the arm answers with, and the record naming the seat
 * and the verb. */
int run_scenario_hint_reaches_brain(void);
int run_scenario_hint_refusals(void);
int run_scenario_hint_records(void);

/* The init table a bot is created with (test_bot_init_table.c): each
 * brain VM sees its own, none means an empty table, and the -bot-init
 * [arg] text maps to the pairs the flag's syntax describes. */
int run_bot_init_table_two_bots_keep_own(void);
int run_bot_init_table_empty_when_none(void);
int run_bot_init_arg_text_to_table(void);

/* New data for a bot already playing (test_scenario_bot_init.c): the op
 * lands a tick later and replaces the table whole, every refusal in the
 * row's contract answers under its own code, a seat off the field takes one
 * on the runner parked behind it, and the brain's BRAIN_INIT is rebuilt with
 * Brain.on_init called about it. */
int run_scenario_bot_init_lands(void);
int run_scenario_bot_init_refusals(void);
int run_scenario_bot_init_parked_seat(void);
int run_brain_on_init_update(void);

int run_console_kick_and_host(void);
int run_console_kick_host_without_newline(void);
int run_console_read_reports_eof(void);
int run_console_read_timeout_then_eof(void);

#ifdef WB_NETDEBUG
/* Net-debug input repro rig (test_netdebug_rig.c). Only declared and
 * built in WB_NETDEBUG configs. */
int run_netdebug_commanded_vs_executed(void);
int run_netdebug_overshoot_under_loss(void);
int run_netdebug_mine_once_under_loss(void);
int run_netdebug_error_offset_clamped(void);
#endif

/* Build a ready-to-tick ServerSim from the embedded Everard Island map
 * with one player added at slot 0. Caller is responsible for
 * serverSimDestroy. Returns NULL on failure. */
struct ServerSim *ut_make_running_sim(const char *player_name);

/* A sim that carries nothing but the classic rules, for a case that builds a
 * bare pill or base list and calls a function taking a sim only so it can
 * read a cap off it. Shared and never destroyed; do not tick it. */
struct GameSim *ut_rules_only_sim(void);

/* The sounds frontEndPlaySound was handed, recorded by the stub in
 * test_stubs.c so a test can assert which variant the client played.
 * ut_sound_get returns the sndEffects value at that index, or -1 past the
 * end. The recorder is bounded; sounds past its cap are not kept. */
void ut_sound_reset(void);
int  ut_sound_count(void);
int  ut_sound_get(int index);

/* The fixture brain in test_stubs.c. The unit binary has no Lua brain, so
 * luaBrainInstanceCreate is a stub there; arming it makes the stub report
 * success, so a test can drive a bot through botManagerAddBot, and record
 * the init table each bot was created with. ut_brain_stub_arm(false) puts
 * it back to refusing, which is how every other test finds it. */
void ut_brain_stub_arm(bool succeed);
bool ut_brain_stub_made(int player_num);
const ScnTable *ut_brain_stub_init(int player_num);
/* The team the slot held as its brain was made — the team serverSimAddBot
 * had already written and picked the slot's lobby start from. */
int ut_brain_stub_team(int player_num);
/* How many brains have been made for one slot, which is what tells a seat
 * fielded once from a seat fielded, taken off the field and fielded again;
 * 0 for a slot off the end. */
int ut_brain_stub_creates(int player_num);
/* How many brains have been destroyed, over every slot: the destroy call is
 * handed an instance and no player number, so there is no slot to file them
 * under. Both counts are reset by ut_brain_stub_arm. */
int ut_brain_stub_destroys(void);
/* Give every brain the stub makes from here on a real, bare lua_State, and
 * mark it running, so a test can watch what the sim writes INTO a brain —
 * a global published to it, a chunk run in it. Off by default and reset by
 * ut_brain_stub_arm, because every other case wants the cheap stub whose
 * instance has no VM at all and whose brain-facing paths are skipped. The
 * state holds no brain: no libraries are opened and nothing is loaded into
 * it, so a tick still does nothing. Destroyed with the instance. */
void ut_brain_stub_lua(bool withState);

/* The map editor's scenario script file (test_editor_script_file.c): where a
 * script sits beside a map, the round trip Save and re-opening the map make,
 * and the map that has no script beside it. The path rule mirrors
 * scnScriptPath, which the editor cannot call. The over-cap case holds the
 * script that is there but will not open, which the editor shows as an empty
 * buffer and must not write that buffer back over. The last case saves a map
 * under a new name: an unedited script is written beside that name so the
 * copy plays as the original did, and a buffer with no file read behind it
 * takes the new path without writing anything there. */
int run_editor_script_path(void);
int run_editor_script_round_trip(void);
int run_editor_script_missing(void);
int run_editor_script_over_cap(void);
int run_editor_script_follows_save_as(void);

/* The manifest behind the scenario panel's metadata, lobby, rules and tags
 * forms (test_editor_scenario_form.c): the empty manifest a new map starts
 * from, the rule list that updates a row rather than growing a second one for
 * the same rule, the team numbers the template hands out, both bounds, the
 * change a rules row describes beside a value, and the tags and regions the
 * tags view edits. The index case is the one that matters most: the editor
 * counts entities from 0 and the manifest's arrays are 1-based, and it asserts
 * that sum at both ends of every range. */
int run_editor_form_init(void);
int run_editor_form_rules(void);
int run_editor_form_rules_full(void);
int run_editor_form_teams(void);
int run_editor_form_rule_change(void);
int run_editor_form_tag_indices(void);
int run_editor_form_tags(void);
int run_editor_form_regions(void);
int run_editor_form_dirty_flag(void);
int run_editor_form_team_ids(void);
int run_editor_form_triggers(void);
int run_editor_form_triggers_full(void);
int run_editor_form_trigger_rows(void);
int run_editor_form_trigger_row_values(void);
int run_editor_form_trigger_action_text(void);
int run_editor_form_trigger_vocabulary(void);

/* Beside them, the one function a file that says it is a mod is not offered
 * (test_editor_scenario_form.c): the catalogue row the editor withholds, held
 * to the name the runtime keeps, and the line the check writes for a mod that
 * defines it anyway. */
int run_editor_form_mod_hides_base_win(void);
int run_editor_form_kind_line_survives_full_list(void);

/* The editor writing a scenario out (test_editor_scenario_pack.c): the chunk
 * on to the map and the standalone .scenario a mod is, both read back through
 * the container reader the server uses. A mod loses the bound flag, the tags
 * and the regions whatever the form held; a map form refuses a manifest that
 * is not built for its map; and a map that has been packed twice is the same
 * bytes as one packed once and still reads as a map. */
int run_editor_pack_mod_round_trip(void);
int run_editor_pack_map_round_trip(void);
int run_editor_pack_twice_identical(void);
int run_editor_pack_refuses_unbound(void);
int run_editor_pack_survives_map_save(void);

/* The virtual "Workshop" map folder (test_workshop_map_dir.c): offered at the
 * root of the map listing when the host has named a Workshop directory that
 * exists, resolved into that directory, and left out, or listed once beside a
 * real folder of that name, otherwise. */
int run_workshop_map_dir_offered_at_root(void);
int run_workshop_map_dir_resolve(void);
int run_workshop_map_dir_absent(void);
/* The client's side of it (test_workshop_map_dir.c): a map upload's USE_LOCAL
 * pre-check names a map in the Workshop directory "Workshop/<name>", and one
 * under data/maps relative to data/maps, with either separator. */
int run_workshop_use_local_rel_path(void);
/* And end to end (test_loopback_upload.c): with the server and the client
 * sharing a Workshop directory, uploading a map from it finishes through
 * USE_LOCAL with nothing sent on the bulk channel. */
int run_workshop_use_local(void);

/* The last status tile frontEndStatusTank was handed by the stub in
 * test_stubs.c: the 1-based player number, and the tankAlliance as an int so
 * the header does not have to pull screentank.h in. Both are -1 until the
 * first call. playersSetPlayer computes the tile itself on a join or a rename,
 * and this is the only way to read what it decided. */
int ut_status_tank_last_player(void);
int ut_status_tank_last_alliance(void);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_UNITTEST_HARNESS_H */
