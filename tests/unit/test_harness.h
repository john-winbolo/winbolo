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
int run_lobby_team_meta_codec_and_apply(void);
int run_lobby_bot_config_codec_and_apply(void);
int run_lobby_bot_brain_codec_and_apply(void);
int run_lobby_brain_list_codec_and_apply(void);
int run_lobby_sync_complete_codec_roundtrip(void);
int run_lobby_rating_posted_codec_roundtrip(void);
int run_newswire_mute_codec_and_apply(void);
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
int run_round_stats_build_summary(void);
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
int run_info_packet_view_policy_layout(void);
int run_info_packet_view_policy_length_tier(void);
int run_upload_cap_enforced(void);
int run_map_field_clamps_evil(void);
int run_map_field_clamps_passthrough(void);
int run_map_field_clamps_angry_start(void);
int run_map_reload_rollback(void);

/* In-memory BMAP parser (test_map_read_memory.c). mapReadFromMemory
 * must agree byte-for-byte with mapRead on valid maps, the WBN
 * preview conversion (file bytes -> compressed -> preview) must
 * round-trip, and malformed buffers must be rejected. */
int run_map_read_memory_matches_file(void);
int run_map_read_memory_handbuilt(void);
int run_map_convert_file_to_compressed(void);
int run_map_read_memory_rejects_garbage(void);
int run_upload_busy_predicate(void);
int run_upload_filename_safe(void);
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

/* A brain's mode manifest (test_brain_modes.c): brains/<brain>/modes.txt,
 * the API by which a brain tells the lobby which modes it has and which
 * difficulty levels each of them offers. Happy path, the synthesized
 * fallback when there is no manifest, malformed lines being skipped rather
 * than fatal, and the two fixed-size caps. */
int run_brain_modes_manifest_parses(void);
int run_brain_modes_missing_falls_back(void);
int run_brain_modes_malformed_lines_skipped(void);
int run_brain_modes_counts_clamped(void);
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

/* Spectator delayed-stream ring (test_spectator_ring.c): segmentation,
 * keyframe-at-segment-start, mid-interval seek + replay, segment isolation,
 * previous-generation read, cold start and the retention window boundary,
 * proven with synthetic byte payloads. */
int run_spectator_ring(void);

/* Spectator ring records without a .wbv log (test_spectator_ring_nolog.c):
 * a registered ring driven by serverSimTick (logWriteTick) populates and a
 * delay-0 seek returns a seed, with no logStart and no .wbv file. */
int run_spectator_ring_nolog(void);

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

/* Scripted map scenarios (test_scenario.c): sidecar discovery,
 * first-tick on_start, per-round VM reset, the map-following player
 * cap, the scripted win path, the engine owner setters, and the
 * error self-disable guard. */
int run_scenario_sidecar_load(void);
int run_scenario_on_start_first_tick(void);
int run_scenario_round_restart_fresh_state(void);
int run_scenario_player_cap_follows_map(void);
int run_scenario_end_round_scripted_win(void);
int run_scenario_owner_setters(void);
int run_scenario_error_disables_after_limit(void);
int run_scenario_give_pill(void);
int run_scenario_forced_game_type(void);
int run_scenario_choose_start(void);
int run_scenario_setup_pre_snapshot(void);
int run_scenario_enemy_roster(void);
int run_scenario_seeded_round_start(void);
int run_scenario_lobby_reset_recommit(void);

/* Per-slot spawn-loadout override across respawns
 * (test_spawn_loadout_respawn.c): game.spawn_bot(..., "open") arms
 * GameSim::spawnLoadout[slot], and the slot must come back fully armed
 * on EVERY respawn (tankDeath), not just the join (tankCreate), while
 * unarmed slots keep the sim's own rules and the leave-path clear stops
 * the override leaking to the next occupant. */
int run_spawn_loadout_open_every_respawn(void);
int run_spawn_loadout_human_unaffected(void);

int run_maprotate_boot_does_not_rotate_while_empty(void);
int run_maprotate_vote_return_is_not_terminal(void);

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

/* Incremental start-picker (test_starts_pick_incremental.c). The one-slot
 * cluster / farthest-first selection that auto-assigns a lobby start on
 * join, shared with startsAssignBatch's distance + validity logic. */
int run_starts_pick_cluster_nearest_teammate(void);
int run_starts_pick_farthest_when_solo(void);
int run_starts_pick_none_when_all_taken(void);

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

/* Viewer-side decode of per-tank stocks (test_lv_tank_stocks.c): hand-built
 * snapshot bodies and forward records through lv_specSeedLoad /
 * lv_specRecordPump. Covers the four bytes on the end of a player block, a
 * block written before they existed (no stocks, and the fields in front of them
 * still decode), a not-in-use slot, the log_TankSetStock record, and an unknown
 * record type skipped by its framed length. */
int run_lv_tank_stocks_from_snapshot(void);
int run_lv_tank_stocks_snapshot_without_tail(void);
int run_lv_tank_stocks_from_record(void);

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

/* The lobby's talking set over the loopback transport (test_voice_talking_set.c),
 * read off the watching client's mirror of it: a talker who goes quiet after the
 * countdown has begun still ages out of the set (the silence is measured on a
 * clock that keeps running while the sim's tick does not), and the only talker
 * leaving empties the set on every other client rather than leaving them lit. */
int run_voice_talking_stops_in_countdown(void);
int run_voice_talking_clears_on_leave(void);

int run_bases_closest_for_player(void);
int run_base_stock_visibility(void);
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

/* Tank destroyed state (test_tank_death_state.c): armour is a plain
 * 0..TANK_FULL_ARMOUR value that clamps at zero and the destroyed state is
 * stored on the tank, so a hit greater than the armour remaining destroys it
 * while a hit that exactly empties the armour leaves it alive at zero. A
 * destroyed tank still reads as destroyed after a snapshot round trip. */
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
 * gives the tank a 29x29 block and every viewable pillbox a 15x15 one,
 * trimmed at the map edges, tank rect first and pills in index order, and
 * never writes more rects than the caller allowed for; and
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
 * rules a server ships with produce the region set the overview has always
 * had; always sweeps a category, key grants only what the player is watching
 * and off grants nothing, each kind in its own block size and in the order the
 * farewell stamp replays; and a decay window runs from full brightness through
 * the fade to nothing, with an item the player could never watch earning
 * nothing from having been driven past. */
int run_overview_policy_baseline(void);
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
 * ground past the ramp fully fogged, the fade rises square by square out of
 * every edge and is darker diagonally off a corner than the same way out of an
 * edge, overlapping regions take the brightest answer, a region against the
 * map border keeps its brightness to the border without writing past the end
 * of the mask, and no regions at all fogs the whole map. */
int run_overview_fog(void);

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

int run_stall_advances_processed_tick(void);
int run_stall_mine_late_lays_once(void);
int run_stall_mine_duplicate_not_relaid(void);
int run_stall_fire_not_harvested(void);
int run_stall_never_fires(void);
int run_stall_brief_trough_no_advance(void);
int run_stall_long_dry_advances(void);

/* Backlog catch-up (test_input_catchup.c): a standing input queue above the
 * jitter target bleeds at +1 input per sub-tick (cap 2 applies/sub-tick) so a
 * jitter-spike backlog drains in ~1s instead of ratcheting input latency;
 * steady state never triggers it. Always built (no WB_NETDEBUG gate). */
int run_input_catchup(void);
int run_catchup_ignores_redundant_duplicates(void);

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
/* Server lock/unlock notice over CHANNEL_GAME (test_lock_channel.c): the
 * "locked to new players" message now rides the reliable game channel, not the
 * snapshot reliable tail. */
int run_lock_channel(void);
/* Server-map preview over CHANNEL_BULK (test_loopback_preview.c): a real .map
 * file streamed back under loss and reassembled byte-identical on the client. */
int run_loopback_map_preview(void);
/* Client->server map upload over CHANNEL_BULK (test_loopback_upload.c): a map
 * uploaded under loss completes and the server decodes the reassembled bytes. */
int run_loopback_map_upload(void);
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
int run_ping_dispatch_new_round_clears_rate_limit(void);
int run_ping_reaches_team_only(void);

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

/* The sounds frontEndPlaySound was handed, recorded by the stub in
 * test_stubs.c so a test can assert which variant the client played.
 * ut_sound_get returns the sndEffects value at that index, or -1 past the
 * end. The recorder is bounded; sounds past its cap are not kept. */
void ut_sound_reset(void);
int  ut_sound_count(void);
int  ut_sound_get(int index);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_UNITTEST_HARNESS_H */
