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
int run_lobby_settings_codec_and_apply(void);
int run_lobby_team_meta_codec_and_apply(void);
int run_lobby_bot_config_codec_and_apply(void);
int run_lobby_bot_brain_codec_and_apply(void);
int run_lobby_brain_list_codec_and_apply(void);
int run_command_codec_roundtrip_variants(void);
int run_command_codec_cmdseq_slot(void);
int run_command_codec_bounds_checks(void);
int run_command_rejected_parks_name_codes(void);
int run_command_rejected_ignores_other_slot(void);
int run_command_rejected_clear_resets_both_fields(void);
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
int run_vote_toggle_standalone_no_does_nothing(void);
int run_vote_toggle_invalid_mode_dropped(void);
int run_vote_toggle_yes_opens_vote(void);
int run_vote_toggle_no_during_running_records_answered(void);
int run_lobby_set_team_clamps_max_tanks(void);
int run_lobby_set_team_accepts_max_legal(void);
int run_lobby_set_team_accepts_unassigned(void);
int run_lobby_set_team_clamps_above_max(void);
int run_server_text_codec_via_chat_wire(void);
int run_md5_rfc1321_vectors(void);
int run_md5_streaming_matches_oneshot(void);
int run_md5_block_boundaries(void);
int run_ranked_flag_persists_with_one_player(void);
int run_ranked_shape_gate(void);
int run_lobby_lock_bit_lookup(void);
int run_lobby_lock_rejects_settings(void);
int run_lobby_lock_mask_roundtrip(void);
int run_upload_cap_enforced(void);
int run_map_field_clamps_evil(void);
int run_map_field_clamps_passthrough(void);
int run_map_field_clamps_angry_start(void);
int run_map_reload_rollback(void);
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
int run_lobby_map_list_chunked(void);
int run_lobby_map_search_chunked(void);
int run_wbn_bearer_state(void);
int run_wbn_rekey_codec(void);
int run_wbn_news_parse(void);
int run_wbn_country_cache(void);
int run_wbn_prefs_parse_get(void);
int run_wbn_prefs_parse_updatedat(void);
int run_wbn_prefs_decide(void);
int run_wbn_prefs_build_put_body(void);
int run_brain_crash_log_writes_file(void);
int run_brain_crash_log_falls_back_to_luaptr(void);
int run_brain_inbox_push_peek_fifo(void);
int run_brain_inbox_overflow_drops_oldest(void);
int run_brain_inbox_legacy_drain_fifo(void);
int run_brain_inbox_clear_resets(void);
int run_bolo_rand_golden_sequence(void);
int run_net_impair(void);

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
int run_humanless_round_does_not_autoend(void);
int run_host_departs_promotes_lowest_human(void);
int run_nonhost_departs_keeps_host(void);
int run_host_reassign_skips_bots(void);
int run_lobby_reset_clears_host_slot(void);

/* Tree-growth water regressions (test_treegrow_no_sea.c). A converged
 * grow target survives serverSimResetGameWorld and, on the next map,
 * points at open sea; the grow gate also failed to reject DEEP_SEA. */
int run_treegrow_never_plants_on_deep_sea(void);
int run_treegrow_reset_clears_stale_target(void);

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

/* CTRL_ALLIANCE_RESET batched alliance event (test_alliance_reset.c).
 * Replaces the O(N²) per-pair CTRL_ALLIANCE_ACCEPT burst that overflowed
 * the host's reliable control queue at game start with 16 players. */
int run_alliance_reset_codec_roundtrip(void);
int run_alliance_reset_decoder_rejects_short(void);
int run_alliance_reset_reapply_publishes_one_event(void);
int run_alliance_reset_apply_rebuilds_alliances(void);

/* Reliable control-event queue regression tests (test_control_event_queue.c).
 * Each captures a specific bug that shipped during the
 * reliable-control-events rollout. */
int run_queue_init_is_valid(void);
int run_queue_enqueue_advances_nextSeq(void);
int run_queue_ack_advance_within_range(void);
int run_queue_stale_ack_above_nextSeq(void);
int run_queue_wipe_resets_both_seqs(void);
int run_queue_enqueue_into_empty_after_wipe(void);
int run_queue_hasspace_at_capacity(void);
int run_control_ack_resend_due(void);
int run_control_seq_reset_detect(void);
int run_log_roundtrip_basic(void);
int run_log_roundtrip_snapshot_keeps_chain_synced(void);
int run_log_roundtrip_lobby_snapshot_is_empty_world(void);
int run_log_roundtrip_lobby_mode_drops_world_events(void);

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

/* Adaptive jitter buffer (test_jitter_buffer_grow.c): queue drains under
 * jitter deepen jitterTarget toward MAX, a steadily full queue shrinks it
 * back to MIN, and it never exceeds MAX. Always built (no WB_NETDEBUG gate). */
int run_jitter_buffer_grow(void);
int run_shell_projection(void);

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
int run_join_version_gate(void);

/* Map-send amplification gate (test_map_amp_gate.c): a crafted JOIN over a raw
 * loopback socket draws a JOIN_ACCEPT but no PACKET_MAP_DOWNLOAD until a
 * MAP_ACK 0xFFFF ready round-trip is sent, after which chunks flow. */
int run_map_amp_gate(void);

/* Map-desync resync queue logic (test_map_resync.c): the resync cut empties
 * the slot's map-event queue (ackedSeq == nextSeq); baked-in changes are not
 * re-sent and a post-cut change delivers exactly once; a duplicate request
 * while a resync is in flight does not re-cut (no event loss); the send gate
 * yields zero map events while in progress and resumes after. */
int run_map_resync_cut_and_deliver_once(void);
int run_map_resync_duplicate_request_no_recut(void);
int run_map_resync_send_gate_holds(void);
int run_map_resync_stale_gen_rejected(void);

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

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_UNITTEST_HARNESS_H */
