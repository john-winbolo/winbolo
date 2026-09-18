/*
 * The scenario host's hooks: what a queued event becomes when the drain
 * hands it back.
 *
 * Every case asserts the values a hook was handed, not that it ran. The
 * payload a hook sees is a run of bytes the emitting callback wrote, and
 * two of the events name their item before their player where the hook
 * names them the other way round, so a case that only counted calls would
 * pass with the arguments in the wrong order.
 *
 * A script says what it was handed by printing a marked line. A scenario
 * state has no io, and the sandbox routes print to the server console, so
 * the record is read off the sim's console callback.
 *
 * Most cases raise the event at the sim rather than staging the world that
 * would produce it: serverSimAddEvent and serverSimPublishControl are the
 * two calls every one of these facts reaches the host through, so a fact
 * put on either of them arrives exactly as the engine's own would. The two
 * cases about the actor mark are the exception — what they are about is
 * which path the fact came down, so they take the real one.
 *
 * run_scenario_hooks_every_hook_from_its_event
 *      — one event per hook, and the payload each hook was handed
 * run_scenario_hooks_fire_one_tick_later
 *      — nothing runs inside the publish; the next tick is what runs it
 * run_scenario_hooks_scripted_both_ways
 *      — the same fact through the op funnel and outside it: scripted true
 *        and scripted false
 * run_scenario_hooks_spawn_drain_is_scripted
 *      — a bot the script asked for lands a tick later through the roster
 *        drain, and the events it publishes on the way in are still the
 *        script's
 * run_scenario_hooks_team_changed_on_difference
 *      — on_team_changed on a slot event carrying a different team, and not
 *        on one carrying the same team
 * run_scenario_hooks_tick_and_end
 *      — on_tick on every running tick with the tick the sim is on, and
 *        on_end once, where the round moves into game over
 * run_scenario_hooks_error_counts_and_disables
 *      — a hook that raises counts one toward the limit, and a scenario
 *        switched off runs no more of them
 * run_scenario_hooks_trigger_runs_after_author
 *      — the author's own function first, then the hook's triggers in the
 *        order the table wrote them; a hook no trigger names is untouched
 * run_scenario_hooks_trigger_stopped_by_false
 *      — an explicit false from the author's hook stops the triggers;
 *        returning nothing does not
 * run_scenario_hooks_trigger_where_both_ways
 *      — a where-row over a plain payload field, on the payload it names
 *        and on another
 * run_scenario_hooks_trigger_call_reaches_script
 *      — a call action runs a top-level function of the author's script,
 *        with a payload field and a literal
 * run_scenario_hooks_router_matches_source
 *      — the router embedded in the binary is the .lua it was generated
 *        from
 * run_scenario_hooks_trigger_team_from_owner
 *      — a team read off an owner, matching and not, and nil where the
 *        owner is nobody
 * run_scenario_hooks_trigger_tag_on_item
 *      — the tags on a pillbox and on a base, each asked with its own kind
 *        word
 * run_scenario_hooks_trigger_tag_ne_and_eq
 *      — ne on a set is absence; eq on one never holds
 * run_scenario_hooks_trigger_region_holds_square
 *      — the region a hook's square pair falls inside, and one outside it
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.callbacks, for the console
                                    * watcher the records are read off */
#include "server_sim_lifecycle.h"  /* SetLobbyEnabled, StartGame, bot config */
#include "server_sim_scenario.h"   /* the op funnel */
#include "control_event.h"
#include "input_packet.h"
#include "everard_map.h"
#include "scenario_host.h"
#include "scenario_events.h"
#include "test_harness.h"

/* ── Fixtures ─────────────────────────────────────────────────────── */

static void shkScriptFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SCRIPT_SUFFIX);
}

/* A hook says what it read by printing a marked line. A scenario state has
 * no io, and its print goes to the server console, which the watcher below
 * catches. The mark is what tells a hook's line from the host's own: both
 * arrive on the same console. */
#define SHK_NOTE_MARK "note:"

static char   shkNote[8192];
static size_t shkNoteLen;

static void shkReset(void) {
    shkNote[0] = '\0';
    shkNoteLen = 0;
}

/* One console line, kept if a hook wrote it, with the newline the record's
   readers count whole lines by. A record past the buffer is truncated
   rather than overrunning it. */
static void shkNoteLine(const char *msg) {
    size_t mark = strlen(SHK_NOTE_MARK);
    size_t room;
    size_t n;

    /* Two bytes short of the end is as far as a line can start: one for the
       newline and one for the terminator. */
    if (msg == NULL || strncmp(msg, SHK_NOTE_MARK, mark) != 0 ||
        shkNoteLen + 2 > sizeof(shkNote)) {
        return;
    }
    n    = strlen(msg + mark);
    room = sizeof(shkNote) - 2 - shkNoteLen;
    if (n > room) {
        n = room;
    }
    memcpy(shkNote + shkNoteLen, msg + mark, n);
    shkNoteLen += n;
    shkNote[shkNoteLen++] = '\n';
    shkNote[shkNoteLen]   = '\0';
}

/* Only consoleMessage is replaced, never the ctx beside it, which the sim's
   other callbacks read. */
static void (*shkConsolePrev)(void *ctx, char *msg) = NULL;

static void shkConsoleCb(void *ctx, char *msg) {
    if (shkConsolePrev != NULL) {
        shkConsolePrev(ctx, msg);
    }
    shkNoteLine(msg);
}

static void shkWatchConsole(ServerSim *sim) {
    shkConsolePrev = sim->sim.callbacks.consoleMessage;
    sim->sim.callbacks.consoleMessage = shkConsoleCb;
}

static void shkUnwatchConsole(ServerSim *sim) {
    sim->sim.callbacks.consoleMessage = shkConsolePrev;
    shkConsolePrev = NULL;
}

/* The script: a scenario table, the note function every hook writes its
 * line with, and the hooks the case wants.
 *
 * extra goes inside the scenario table, after the two keys every case
 * wants. A trigger is stated in the table rather than handed to a host
 * separately, so a case that wants triggers writes them where a scenario
 * would. A trailing comma is legal in a Lua table, so "" leaves the table
 * exactly as it was. */
static bool shkPutTable(const char *mapPath, const char *extra,
                        const char *body) {
    char  path[512];
    char  lua[16384];
    FILE *f;

    snprintf(lua, sizeof(lua),
             "scenario = { name = \"Hooks\", api = 1, %s }\n"
             "local function note(s)\n"
             "  print(\"" SHK_NOTE_MARK "\" .. tostring(s))\n"
             "end\n"
             "%s", extra, body);

    shkScriptFor(mapPath, path, sizeof(path));
    f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static bool shkPut(const char *mapPath, const char *body) {
    return shkPutTable(mapPath, "", body);
}

static void shkDrop(const char *mapPath) {
    char path[512];
    shkScriptFor(mapPath, path, sizeof(path));
    remove(path);
}

/* A sim that has not started, ready to be attached to and then started.
 * The host has to be attached before the start, because the round start is
 * where a round's hooks are resolved. */
static ServerSim *shkSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    /* Watched from here, which is before every case's attach: a script that
       prints at its top level prints while the attach is still running. */
    shkWatchConsole(sim);
    shkReset();
    return sim;
}

/* What the hooks have written so far, or "" when none has. */
static void shkRead(char *out, size_t outLen) {
    snprintf(out, outLen, "%s", shkNote);
}

/* How many whole lines the record has that begin with what was asked for.
 *
 * Anchored at the start of a line, and every caller that wants the whole of
 * one passes its newline on the end. Both halves matter: without the anchor
 * a label that ends with another label counts that other one too, which is
 * a trap the first case to write "replace" beside "full_replace" walks
 * straight into — and the name would have said lines while the answer meant
 * substrings. */
static int shkLines(const char *text, const char *line) {
    const char *p     = text;
    size_t      n     = strlen(line);
    int         count = 0;

    if (n == 0) {
        return 0;
    }
    while ((p = strstr(p, line)) != NULL) {
        if (p == text || p[-1] == '\n') {
            count++;
        }
        p += n;
    }
    return count;
}

/* The round start and the start's own publishes are the first thing in the
 * queue, and they are not what any of these cases is about. One tick takes
 * them, and the record goes with them. */
static void shkFlush(ServerSim *sim) {
    serverSimTick(sim);
    shkReset();
}

/* ── Raising one fact ─────────────────────────────────────────────── */

/* A game event, built byte by byte and put on the channel the engine's own
 * emit puts it on. data past the event's wire size is written too, because
 * that is where two of the payloads a hook reads actually sit. */
static void shkRaise(ServerSim *sim, uint8_t type, const uint8_t *data,
                     size_t len) {
    GameEvent ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    if (len > sizeof(ev.data)) {
        len = sizeof(ev.data);
    }
    memcpy(ev.data, data, len);
    serverSimAddEvent(sim, &ev);
}

/* The first seat with nobody in it. A spawn that leaves the seat to the
 * handler is answered true and "queued" and nothing else: it is not
 * promised a seat until the drain runs, so ScnOpOut carries one only when
 * the op asked for one. A case that has to name the seat in an assertion
 * therefore asks for it, which changes nothing about the path being tested
 * — a named seat queues and lands in the same drain as an unnamed one. */
static BYTE shkFreeSeat(ServerSim *sim) {
    BYTE i;

    for (i = 0; i < MAX_TANKS; i++) {
        ServerSimRosterSlot slot;
        if (!serverSimGetRosterSlot(sim, i, &slot)) {
            return i;
        }
    }
    return SCN_NONE;
}

/* One lobby slot event, as serverSimFillLobbySlotEvent builds one for a
 * seat that is taken. */
static void shkPublishSlot(ServerSim *sim, BYTE slot, BYTE team) {
    ControlEvent evt;

    memset(&evt, 0, sizeof(evt));
    evt.type                        = CTRL_LOBBY_SLOT;
    evt.u.lobbySlot.playerNum       = slot;
    evt.u.lobbySlot.slot.connected  = true;
    evt.u.lobbySlot.slot.teamNumber = team;
    serverSimPublishControl(sim, &evt);
}

/* ── What the whole game was told ─────────────────────────────────── */

/* The line a switched-off scenario owes the players goes out as a
 * CTRL_SERVER_TEXT, which is where every client's delivery reads it. */
typedef struct {
    int  count;
    char last[SCN_TEXT_MAX];
} ShkText;

static void shkTextCb(void *ctx, const ControlEvent *evt) {
    ShkText *c = (ShkText *)ctx;
    if (evt->type != CTRL_SERVER_TEXT) {
        return;
    }
    c->count++;
    snprintf(c->last, sizeof(c->last), "%s", evt->u.serverText.text);
}

/* Registration replays the server's current state to the new subscriber, so
   the record is cleared afterwards and counts only what happens next. */
static void shkWatchText(ServerSim *sim, ShkText *c) {
    memset(c, 0, sizeof(*c));
    (void)serverSimRegisterSubscriber(sim, shkTextCb, c);
    memset(c, 0, sizeof(*c));
}

/* ── 1. Every hook, from its event, with its payload ──────────────── */

/* One script defining every event hook, one event apiece, and the exact
 * line each hook is expected to have written.
 *
 * The numbers are the case's own and are chosen to be distinct from one
 * another, so an argument taken from the wrong byte cannot land on the
 * value the assertion wanted. Where the event and the hook order their
 * arguments differently — both pill hooks that name a player, and the tank
 * kill — the expected line is written in the hook's order and the raised
 * bytes in the event's, which is the whole point of the pair.
 *
 * Item indices are 0-based on an event and 1-based in a script, so every
 * expected index below is one more than the byte raised. Player slots are
 * the same number on both sides.
 */
static const char *const kShkBody =
    "function on_tank_killed(p, k, c, s)"
    " note(\"tank_killed \"..p..\" \"..k..\" \"..c..\" \"..tostring(s)) end\n"
    "function on_tank_spawned(p, x, y, r, s)"
    " note(\"tank_spawned \"..p..\" \"..x..\" \"..y..\" \"..tostring(r)"
    "..\" \"..tostring(s)) end\n"
    "function on_lgm_died(p, k, x, y, s)"
    " note(\"lgm_died \"..p..\" \"..k..\" \"..x..\" \"..y..\" \"..tostring(s))"
    " end\n"
    "function on_lgm_landed(p, x, y, s)"
    " note(\"lgm_landed \"..p..\" \"..x..\" \"..y..\" \"..tostring(s)) end\n"
    "function on_base_captured(n, o, w, s)"
    " note(\"base_captured \"..n..\" \"..o..\" \"..w..\" \"..tostring(s)) end\n"
    "function on_base_neutralized(n, o, s)"
    " note(\"base_neutralized \"..n..\" \"..o..\" \"..tostring(s)) end\n"
    "function on_pill_captured(n, o, w, s)"
    " note(\"pill_captured \"..n..\" \"..o..\" \"..w..\" \"..tostring(s)) end\n"
    "function on_pill_placed(n, p, a, s)"
    " note(\"pill_placed \"..n..\" \"..p..\" \"..a..\" \"..tostring(s)) end\n"
    "function on_pill_picked_up(n, p, s)"
    " note(\"pill_picked_up \"..n..\" \"..p..\" \"..tostring(s)) end\n"
    "function on_pill_killed(n, by, s)"
    " note(\"pill_killed \"..n..\" \"..by..\" \"..tostring(s)) end\n"
    "function on_built(p, a, x, y, s)"
    " note(\"built \"..p..\" \"..a..\" \"..x..\" \"..y..\" \"..tostring(s))"
    " end\n"
    "function on_mine_laid(p, x, y, s)"
    " note(\"mine_laid \"..p..\" \"..x..\" \"..y..\" \"..tostring(s)) end\n"
    "function on_mine_explosion(x, y, l, s)"
    " note(\"mine_explosion \"..x..\" \"..y..\" \"..l..\" \"..tostring(s))"
    " end\n"
    "function on_player_join(p, s)"
    " note(\"player_join \"..p..\" \"..tostring(s)) end\n"
    "function on_player_leave(p, s)"
    " note(\"player_leave \"..p..\" \"..tostring(s)) end\n"
    "function on_lobby(p, s)"
    " note(\"lobby \"..p..\" \"..tostring(s)) end\n"
    "function on_team_changed(p, t, s)"
    " note(\"team_changed \"..p..\" \"..t..\" \"..tostring(s)) end\n"
    "function on_chat(p, text, s)"
    " note(\"chat \"..p..\" \"..text..\" \"..tostring(s)) end\n";

/* One raised event: the type, the bytes, and the line the hook it reaches
 * must have written. */
typedef struct {
    uint8_t     type;
    uint8_t     data[8];
    size_t      len;
    const char *want;
} ShkGameCase;

static const ShkGameCase kShkGame[] = {
    /* [killer, killed, cause, carriedPills, trees, mapX, mapY] — the hook
       takes the victim first and the cause as a word. */
    { EVENT_TANK_KILLED,   { 5, 2, LAST_DEATH_BY_SHELL, 1, 0, 40, 41 }, 7,
      "tank_killed 2 5 shell false\n" },
    /* [player, mx, my, respawn] — the flag reaches Lua as a boolean. */
    { EVENT_TANK_SPAWNED,  { 3, 44, 45, 1 }, 4,
      "tank_spawned 3 44 45 true false\n" },
    /* [victim, killer, quiet, mx, my] — the quiet byte is the announce
       policy's answer and no business of a hook; the square behind it is
       where the man died. */
    { EVENT_LGM_LOST,      { 6, 7, 0, 48, 49 }, 5,
      "lgm_died 6 7 48 49 false\n" },
    { EVENT_LGM_LANDED,    { 8, 50, 51 }, 3,
      "lgm_landed 8 50 51 false\n" },
    /* [new, old, index, quiet, class, mapX, mapY] — a new owner that is
       somebody is a capture. */
    { EVENT_BASE_CAPTURED, { 9, 10, 3, 0, CAPTURE_CLASS_ENEMY, 52, 53 }, 7,
      "base_captured 4 10 9 false\n" },
    /* the same event with nobody as the new owner is the other hook. */
    { EVENT_BASE_CAPTURED, { NEUTRAL, 11, 4, 0, CAPTURE_CLASS_NEUTRAL, 54,
                             55 }, 7,
      "base_neutralized 5 11 false\n" },
    { EVENT_PILL_CAPTURED, { 12, 13, 5, 0, CAPTURE_CLASS_ENEMY, 56, 57 }, 7,
      "pill_captured 6 13 12 false\n" },
    /* [player, index, mx, my, armour] — the hook names the pillbox first, and
       the armour behind the player is what separates a pillbox a builder put
       up from one a corpse dropped. */
    { EVENT_PILL_PLACED,   { 14, 6, 58, 59, 15 }, 5,
      "pill_placed 7 14 15 false\n" },
    /* [player, index] — the same swap. */
    { EVENT_PILL_PICKED_UP,{ 1, 7 }, 2,
      "pill_picked_up 8 1 false\n" },
    /* [index, attacker] — this one already leads with the item. */
    { EVENT_PILL_KILLED,   { 8, 4 }, 2,
      "pill_killed 9 4 false\n" },
    /* [player, action, mx, my] — the action byte is the builder's request
       code and reaches Lua as a word. */
    { EVENT_BUILT,         { 0, (uint8_t)builderJobRoad, 60, 61 }, 4,
      "built 0 road 60 61 false\n" },
    /* and the one code that is spelled differently on this event than in an
       order: a build of kind pill here is always a repair. */
    { EVENT_BUILT,         { 0, (uint8_t)builderJobPill, 62, 63 }, 4,
      "built 0 repair 62 63 false\n" },
    { EVENT_MINE_PLACED,   { 15, 64, 65 }, 3,
      "mine_laid 15 64 65 false\n" },
    /* [mx, my, layer] — the layer sits past the event's wire size, so a
       hook reads it and a client never does. */
    { EVENT_MINE_EXPLODED, { 66, 67, 9 }, 3,
      "mine_explosion 66 67 9 false\n" },
};

int run_scenario_hooks_every_hook_from_its_event(void) {
    static const char *const kMap = "scnhook_every.map";
    ServerSim    *sim;
    ScenarioHost *h;
    ControlEvent  evt;
    char          rec[8192];
    char          err[512];
    size_t        i;

    shkReset();
    UT_ASSERT(shkPut(kMap, kShkBody));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    for (i = 0; i < sizeof(kShkGame) / sizeof(kShkGame[0]); i++) {
        shkRaise(sim, kShkGame[i].type, kShkGame[i].data, kShkGame[i].len);
    }

    memset(&evt, 0, sizeof(evt));
    evt.type                      = CTRL_PLAYER_JOIN;
    evt.u.playerJoin.playerNum    = 11;
    serverSimPublishControl(sim, &evt);

    memset(&evt, 0, sizeof(evt));
    evt.type                      = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum   = 12;
    serverSimPublishControl(sim, &evt);

    /* One slot event is both hooks it can be: the seat changed, and the
       team differs from the one the round opened with. */
    shkPublishSlot(sim, 13, 6);

    memset(&evt, 0, sizeof(evt));
    evt.type                = CTRL_CHAT;
    evt.u.chat.fromPlayer   = 4;
    evt.u.chat.destPlayer   = 0xFF;
    evt.u.chat.bodyLen      = 5;
    memcpy(evt.u.chat.body, "hello", 5);
    serverSimPublishControl(sim, &evt);

    serverSimTick(sim);
    shkRead(rec, sizeof(rec));

    for (i = 0; i < sizeof(kShkGame) / sizeof(kShkGame[0]); i++) {
        UT_ASSERT_MSG(shkLines(rec, kShkGame[i].want) == 1,
                      "expected the line '%.*s' once; the record was:\n%s",
                      (int)(strlen(kShkGame[i].want) - 1), kShkGame[i].want,
                      rec);
    }
    UT_ASSERT_MSG(shkLines(rec, "player_join 11 false\n") == 1,
                  "the join hook did not see slot 11; the record was:\n%s",
                  rec);
    UT_ASSERT_MSG(shkLines(rec, "player_leave 12 false\n") == 1,
                  "the leave hook did not see slot 12; the record was:\n%s",
                  rec);
    UT_ASSERT_MSG(shkLines(rec, "lobby 13 false\n") == 1,
                  "the lobby hook did not see slot 13; the record was:\n%s",
                  rec);
    UT_ASSERT_MSG(shkLines(rec, "team_changed 13 6 false\n") == 1,
                  "the team hook did not see slot 13 on team 6; the record "
                  "was:\n%s", rec);
    UT_ASSERT_MSG(shkLines(rec, "chat 4 hello false\n") == 1,
                  "the chat hook did not see slot 4's line; the record "
                  "was:\n%s", rec);

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* ── 2. A tick later, not inside the publish ──────────────────────── */

/* The whole reason the host queues rather than calling: the subscriber runs
 * from inside the work that raised the event, where the sim's state is
 * half-written and a publish is forbidden. So the record must still be
 * empty when the publish returns, and the hook must have run by the end of
 * the next tick. */
int run_scenario_hooks_fire_one_tick_later(void) {
    static const char *const kMap = "scnhook_later.map";
    /* [index, attacker]. The two are different numbers, and neither is the
       other's, so a hook that took them in the wrong order fails here
       rather than reading right by coincidence. Pillbox 2 on the event is
       pillbox 3 to a script. */
    static const uint8_t     kKilled[2] = { 2, 7 };
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPut(kMap,
                     "function on_pill_killed(n, by, s)"
                     " note(\"pill_killed \"..n..\" \"..by) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkRaise(sim, EVENT_PILL_KILLED, kKilled, sizeof(kKilled));

    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(rec[0] == '\0',
                  "a hook ran inside the publish; the record was:\n%s", rec);
    UT_ASSERT_MSG(scenarioEventsWaiting(scenarioHostEventQueue(h)) >= 1,
                  "the event did not reach the queue");

    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(shkLines(rec, "pill_killed 3 7\n") == 1,
                  "the tick after the publish did not run the hook once; the "
                  "record was:\n%s", rec);

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* ── 3. The mark, from both sides ─────────────────────────────────── */

/* The first base the map has that is there to be taken. Named by the
 * property the case needs rather than by a number: a map with none is a
 * fixture problem and says so. */
static int shkFirstBase(ServerSim *sim, ServerSimBaseInfo *out) {
    BYTE n = serverSimGetBaseCount(sim);
    BYTE i;

    for (i = 1; i <= n; i++) {
        if (serverSimGetBaseInfo(sim, i, out) && out->active) {
            return (int)i;
        }
    }
    return -1;
}

/* One fact, twice. Through the op funnel it is the scenario's own doing and
 * the hook is told so; raised the way the engine raises it for anybody else
 * — outside the funnel and outside the roster drain — it is not. Either
 * half alone would prove nothing: a mark that is always set and a mark that
 * is never set each pass one of them. */
int run_scenario_hooks_scripted_both_ways(void) {
    static const char *const kMap = "scnhook_actor.map";
    ServerSim        *sim;
    ScenarioHost     *h;
    ServerSimBaseInfo base;
    ScenarioOp        op;
    ScnOpResult       r;
    uint8_t           data[7];
    char              rec[2048];
    char              err[512];
    char              want[128];
    int               index;
    BYTE              before;
    BYTE              owner;

    shkReset();
    UT_ASSERT(shkPut(kMap,
                     "function on_base_captured(n, o, w, s)"
                     " note(\"base_captured \"..n..\" \"..o..\" \"..w"
                     "..\" \"..tostring(s)) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);
    shkFlush(sim);

    index = shkFirstBase(sim, &base);
    UT_ASSERT_MSG(index > 0, "the map has no base to hand over");
    before = base.owner;
    owner  = (BYTE)((before == 0) ? 1 : 0);

    /* Through the funnel. The op's base index is 0-based and the hook's is
       1-based, so the two differ by the one conversion. */
    memset(&op, 0, sizeof(op));
    op.type                   = SCN_OP_BASE_SET_OWNER;
    op.u.baseSetOwner.base    = (BYTE)(index - 1);
    op.u.baseSetOwner.owner   = owner;
    r = serverSimApplyScenarioOp(sim, &op, NULL);
    UT_ASSERT_MSG(r == SCN_OP_OK, "the hand-over was refused: %d", (int)r);

    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    snprintf(want, sizeof(want), "base_captured %d %d %d true\n", index,
             (int)before, (int)owner);
    UT_ASSERT_MSG(shkLines(rec, want) == 1,
                  "the scenario's own hand-over was not marked as its own; "
                  "expected '%.*s', the record was:\n%s",
                  (int)(strlen(want) - 1), want, rec);

    /* And the same fact from outside it. */
    memset(data, 0, sizeof(data));
    data[0] = owner;
    data[1] = before;
    data[2] = (uint8_t)(index - 1);
    data[4] = CAPTURE_CLASS_ENEMY;
    shkRaise(sim, EVENT_BASE_CAPTURED, data, sizeof(data));

    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    snprintf(want, sizeof(want), "base_captured %d %d %d false\n", index,
             (int)before, (int)owner);
    UT_ASSERT_MSG(shkLines(rec, want) == 1,
                  "a hand-over the scenario did not make was marked as its "
                  "own; expected '%.*s', the record was:\n%s",
                  (int)(strlen(want) - 1), want, rec);

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* ── 4. The spawn drain carries the mark ──────────────────────────── */

/* A bot costs a Lua VM and a ClientSim, so the spawn op queues and the sim
 * makes the change a tick later, from serverSimScenarioDrainRoster — which
 * is outside the op handler entirely. The join it publishes on the way in
 * is still the script's doing, and this is the case that says the mark
 * covers the drain and not only the handler. */
int run_scenario_hooks_spawn_drain_is_scripted(void) {
    static const char *const kMap   = "scnhook_spawn.map";
    static const char *const kBrain = "test_scenario_hooks_brain.lua";
    ServerSim    *sim;
    ScenarioHost *h;
    ScenarioOp    op;
    ScnOpOut      out;
    ScnOpResult   r;
    FILE         *bf;
    char          rec[2048];
    char          err[512];
    char          want[128];
    BYTE          seat;

    shkReset();
    bf = fopen(kBrain, "wb");
    UT_ASSERT(bf != NULL);
    fputs("-- fixture\n", bf);
    fclose(bf);

    UT_ASSERT(shkPut(kMap,
                     "function on_player_join(p, s)"
                     " note(\"player_join \"..p..\" \"..tostring(s)) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, kBrain);
    ut_brain_stub_arm(true);
    shkFlush(sim);

    /* The seat is named rather than left to the handler, because this case
       has to say in an assertion which seat the bot joined in and an op
       that names none is not answered one until the drain has run. What is
       being tested is untouched: the spawn queues either way and lands in
       the same drain. */
    seat = shkFreeSeat(sim);
    UT_ASSERT_MSG(seat != SCN_NONE, "the roster has no free seat to spawn in");

    memset(&op, 0, sizeof(op));
    memset(&out, 0, sizeof(out));
    op.type                     = SCN_OP_ROSTER_SPAWN_BOT;
    op.u.rosterSpawnBot.slot    = seat;
    op.u.rosterSpawnBot.start   = SCN_NONE;
    SDL_strlcpy(op.u.rosterSpawnBot.brain, kBrain, SCN_PATH_MAX);
    r = serverSimApplyScenarioOp(sim, &op, &out);
    UT_ASSERT_MSG(r == SCN_OP_QUEUED, "the spawn answered %d, expected queued",
                  (int)r);

    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(rec[0] == '\0',
                  "the seat landed inside the op; the record was:\n%s", rec);

    /* One tick makes the change; the next runs the hooks it published. */
    serverSimTick(sim);
    UT_ASSERT_MSG(out.slot == seat,
                  "the spawn named seat %d and answered %d", (int)seat,
                  (int)out.slot);
    UT_ASSERT_MSG(serverSimIsBot(sim, seat),
                  "the queued spawn did not land in seat %d", (int)seat);
    serverSimTick(sim);

    shkRead(rec, sizeof(rec));
    snprintf(want, sizeof(want), "player_join %d true\n", (int)seat);
    UT_ASSERT_MSG(shkLines(rec, want) == 1,
                  "the bot the script asked for did not join as the script's "
                  "doing; expected '%.*s', the record was:\n%s",
                  (int)(strlen(want) - 1), want, rec);

    ut_brain_stub_arm(false);
    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    remove(kBrain);
    return 0;
}

/* ── 5. A team change is a difference ─────────────────────────────── */

/* The slot event carries the team a seat is on and nothing about the team
 * it was on, so the host keeps its own copy and compares. Two events for
 * one seat: the first moves it, the second says the same thing again. The
 * seat changed twice; the team changed once. */
int run_scenario_hooks_team_changed_on_difference(void) {
    static const char *const kMap = "scnhook_team.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPut(kMap,
                     "function on_team_changed(p, t, s)"
                     " note(\"team_changed \"..p..\" \"..t) end\n"
                     "function on_lobby(p, s) note(\"lobby \"..p) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkPublishSlot(sim, 4, 3);
    shkPublishSlot(sim, 4, 3);
    serverSimTick(sim);

    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(shkLines(rec, "lobby 4\n") == 2,
                  "the seat changed twice and the lobby hook ran %d times; "
                  "the record was:\n%s", shkLines(rec, "lobby 4\n"), rec);
    UT_ASSERT_MSG(shkLines(rec, "team_changed 4 3\n") == 1,
                  "the team changed once and the team hook ran %d times; the "
                  "record was:\n%s", shkLines(rec, "team_changed 4 3\n"), rec);

    /* And a third event moving it somewhere else is a change again. */
    shkPublishSlot(sim, 4, 5);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(shkLines(rec, "team_changed 4 5\n") == 1,
                  "moving the seat to another team did not report a change; "
                  "the record was:\n%s", rec);

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* ── 6. on_tick and on_end ────────────────────────────────────────── */

/* on_tick is every running tick and takes the tick the sim is on; on_end is
 * the move into game over and is made once.
 *
 * The script ends the round from its third on_tick, so the end is one the
 * hook itself caused, from inside the same tick — which is the case the
 * order in scnTick exists for.
 *
 * The tick number is held against what the sim says rather than against a
 * number this case counted: nothing here should depend on where in a frame
 * the sim advances its clock.
 */
static uint32_t shkLastTick(const char *rec, bool *found) {
    static const char kTag[] = "tick ";
    const char       *p      = rec;
    const char       *last   = NULL;

    *found = false;
    /* At the start of a line, as the counter above matches: a label that
       ended with this one would otherwise be read as a tick of its own. */
    while ((p = strstr(p, kTag)) != NULL) {
        if (p == rec || p[-1] == '\n') {
            last = p;
        }
        p += sizeof(kTag) - 1;
    }
    if (last == NULL) {
        return 0;
    }
    *found = true;
    return (uint32_t)strtoul(last + sizeof(kTag) - 1, NULL, 10);
}

int run_scenario_hooks_tick_and_end(void) {
    static const char *const kMap = "scnhook_tick.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[4096];
    char          err[512];
    uint32_t      saw;
    bool          found;
    int           i;

    shkReset();
    UT_ASSERT(shkPut(kMap,
                     "local n = 0\n"
                     "function on_tick(t)\n"
                     "  n = n + 1\n"
                     "  note(\"tick \"..t)\n"
                     "  if n == 3 then game.end_round(\"done\") end\n"
                     "end\n"
                     "function on_end() note(\"end\") end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);
    shkReset();

    /* Three running ticks. Each must have run on_tick with the tick the sim
       is on when the frame returns. */
    for (i = 0; i < 3; i++) {
        serverSimTick(sim);
        shkRead(rec, sizeof(rec));
        saw = shkLastTick(rec, &found);
        UT_ASSERT_MSG(found, "on_tick did not run on running tick %d; the "
                             "record was:\n%s", i, rec);
        UT_ASSERT_MSG(saw == serverSimGetTick(sim),
                      "on_tick was handed %lu and the sim is on %lu",
                      (unsigned long)saw,
                      (unsigned long)serverSimGetTick(sim));
    }

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "the third on_tick did not end the round");
    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(shkLines(rec, "end\n") == 1,
                  "on_end ran %d times on the tick the round ended; the "
                  "record was:\n%s", shkLines(rec, "end\n"), rec);

    /* And the ticks after it are neither: the round is over and stays
       over. */
    serverSimTick(sim);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(shkLines(rec, "end\n") == 1,
                  "on_end ran again after the round ended; the record "
                  "was:\n%s", rec);
    UT_ASSERT_MSG(shkLines(rec, "tick ") == 3,
                  "on_tick ran %d times, expected the three running ticks; "
                  "the record was:\n%s", shkLines(rec, "tick "), rec);

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* ── 7. A hook that raises, and a scenario switched off ───────────── */

/* One raising hook counts one toward SCN_ERROR_LIMIT, which the boundary
 * is what proves: one short of the limit and the round plays on, one more
 * and the scenario is off with a line the whole game reads. After that
 * nothing else the script defined runs at all, which the second hook — one
 * that would have returned perfectly well — is there to show.
 */
int run_scenario_hooks_error_counts_and_disables(void) {
    static const char *const kMap       = "scnhook_errors.map";
    static const uint8_t     kKilled[2] = { 1, 2 };
    static const uint8_t     kPlaced[4] = { 3, 4, 20, 21 };
    ServerSim    *sim;
    ScenarioHost *h;
    ShkText       text;
    char          rec[2048];
    char          err[512];
    int           i;

    shkReset();
    UT_ASSERT(shkPut(kMap,
                     "function on_pill_killed(n, by, s)"
                     " error(\"no\") end\n"
                     "function on_pill_placed(n, p, s)"
                     " note(\"pill_placed \"..n) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkWatchText(sim, &text);
    shkFlush(sim);
    text.count = 0;

    /* One short of the limit. */
    for (i = 0; i < SCN_ERROR_LIMIT - 1; i++) {
        shkRaise(sim, EVENT_PILL_KILLED, kKilled, sizeof(kKilled));
    }
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 0,
                  "%d raising hooks switched the scenario off one short of "
                  "the limit of %d: '%s'", SCN_ERROR_LIMIT - 1,
                  SCN_ERROR_LIMIT, text.last);

    /* And the one that reaches it. */
    shkRaise(sim, EVENT_PILL_KILLED, kKilled, sizeof(kKilled));
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 1,
                  "the %dth raising hook sent %d lines, expected the one "
                  "saying the scenario is off", SCN_ERROR_LIMIT, text.count);

    /* Off is off: a hook that would have run does not. */
    shkReset();
    shkRaise(sim, EVENT_PILL_PLACED, kPlaced, sizeof(kPlaced));
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    UT_ASSERT_MSG(rec[0] == '\0',
                  "a switched-off scenario still ran a hook; the record "
                  "was:\n%s", rec);

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* ── 8. Triggers ──────────────────────────────────────────────────── */

/* A scenario's triggers are data in its table, and the router that turns
 * them into calls is shipped in the binary. These cases boot a host on
 * fixture trigger data, raise a real event, and assert what the actions
 * did — the same console record every case above reads, since game.log
 * writes a line to it.
 *
 * The embedded router, for the case that holds it against the file it was
 * generated from. */
#include "scenario_triggers.inc"  /* kScnTriggersLua, SCN_TRIGGERS_LUA_LEN */

/* Where the router's source lives. CMake passes the absolute path; the
 * fallback is the path from the source root, for a run started there. */
#ifndef WB_SCENARIO_SRC_DIR
#define WB_SCENARIO_SRC_DIR "src/scenario"
#endif

/* One player joining, which is on_player_join(p) and the shortest payload a
 * where-row can test. */
static void shkPublishJoin(ServerSim *sim, BYTE slot) {
    ControlEvent evt;

    memset(&evt, 0, sizeof(evt));
    evt.type                   = CTRL_PLAYER_JOIN;
    evt.u.playerJoin.playerNum = slot;
    serverSimPublishControl(sim, &evt);
}

/* The record, whole and in order. Every case below asserts the exact lines
 * rather than counting them: what a trigger case is about is which ran and
 * in what order, and a count can say neither.
 *
 * A macro rather than a function because the assertion returns from the case
 * it failed in. */
#define SHK_IS(rec, want, what)                                             \
    UT_ASSERT_MSG(strcmp((rec), (want)) == 0,                               \
                  "%s\nexpected:\n%s\ngot:\n%s", (what), (want), (rec))

/* The author's function first, then the hook's triggers in the order the
 * table wrote them, and a hook no trigger names left alone. */
int run_scenario_hooks_trigger_runs_after_author(void) {
    static const char *const kMap = "scnhook_trig_order.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "triggers = {\n"
        "  { when = \"on_player_join\","
        " actions = { { \"log\", \"" SHK_NOTE_MARK "first\" } } },\n"
        "  { when = \"on_player_join\","
        " actions = { { \"log\", \"" SHK_NOTE_MARK "second\" } } },\n"
        "}",
        "function on_player_join(p) note(\"author \"..p) end\n"
        "function on_player_leave(p) note(\"leave \"..p) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkPublishJoin(sim, 11);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "author 11\nfirst\nsecond\n",
           "the author's own function runs first, then the hook's triggers "
           "in the order the table wrote them");

    /* A hook no trigger names is the function the script wrote and nothing
       around it. */
    shkReset();
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type                    = CTRL_PLAYER_LEAVE;
        evt.u.playerLeave.playerNum = 12;
        serverSimPublishControl(sim, &evt);
    }
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "leave 12\n",
           "on_player_leave is named by no trigger, so it is left as the "
           "script wrote it");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* An explicit false from the author's hook stops the triggers. Falling off
 * the end returns nil and does not, which is what keeps a handler that does
 * not care about triggers out of their way. */
int run_scenario_hooks_trigger_stopped_by_false(void) {
    static const char *const kMap = "scnhook_trig_false.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "triggers = {\n"
        "  { when = \"on_player_join\","
        " actions = { { \"log\", \"" SHK_NOTE_MARK "ran\" } } },\n"
        "}",
        "function on_player_join(p)\n"
        "  note(\"author \"..p)\n"
        "  if p == 5 then return false end\n"
        "end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkPublishJoin(sim, 5);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "author 5\n",
           "the author's hook returned false, which stops the trigger");

    shkReset();
    shkPublishJoin(sim, 6);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "author 6\nran\n",
           "the same hook returned nothing, which does not stop the trigger");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* A where-row over a plain payload field, both ways: the trigger fires on
 * the payload the row names and on no other. */
int run_scenario_hooks_trigger_where_both_ways(void) {
    static const char *const kMap = "scnhook_trig_where.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "triggers = {\n"
        "  { when = \"on_player_join\","
        " where = { { \"p\", \"eq\", 7 } },"
        " actions = { { \"log\", \"" SHK_NOTE_MARK "matched\" } } },\n"
        "}",
        "function on_player_join(p) note(\"author \"..p) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkPublishJoin(sim, 7);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "author 7\nmatched\n",
           "the row holds on slot 7, so the action runs");

    shkReset();
    shkPublishJoin(sim, 8);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "author 8\n",
           "the row does not hold on slot 8, so the action does not run");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* A call action runs a top-level function of the author's own script, and
 * carries it both a payload field and a literal. The script defines no
 * on_player_join of its own, so this is also the router installing a hook
 * where there was nothing to chain on to. */
int run_scenario_hooks_trigger_call_reaches_script(void) {
    static const char *const kMap = "scnhook_trig_call.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "triggers = {\n"
        "  { when = \"on_player_join\","
        " actions = { { \"call\", \"shout\", { field = \"p\" },"
        " \"loud\" } } },\n"
        "}",
        "function shout(p, word)\n"
        "  note(\"shout \"..tostring(p)..\" \"..tostring(word))\n"
        "end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkPublishJoin(sim, 9);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "shout 9 loud\n",
           "the call reached the script's own function with the joining "
           "seat off the payload and the literal beside it");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* The router in the binary is the file it was generated from.
 *
 * tools/embed_lua.py is run by hand and its output committed, the way the
 * lang table's generator is, so nothing in the build holds the two
 * together. This does: a .lua edited without re-running the generator fails
 * here rather than shipping a binary that runs the previous router. */
int run_scenario_hooks_router_matches_source(void) {
    static const char *const kPath =
        WB_SCENARIO_SRC_DIR "/scenario_triggers.lua";
    unsigned char *src;
    long           size;
    size_t         got;
    FILE          *f;

    f = fopen(kPath, "rb");
    UT_ASSERT_MSG(f != NULL, "could not open %s", kPath);

    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        UT_ASSERT_MSG(0, "could not measure %s", kPath);
    }

    src = (unsigned char *)malloc((size_t)size);
    if (src == NULL) {
        fclose(f);
        UT_ASSERT_MSG(0, "no memory for %d bytes of %s", (int)size, kPath);
    }
    got = fread(src, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(src);
        UT_ASSERT_MSG(0, "read %d of %d bytes from %s",
                      (int)got, (int)size, kPath);
    }

    if ((size_t)size != SCN_TRIGGERS_LUA_LEN) {
        free(src);
        UT_ASSERT_MSG(0,
                      "the embedded router is %d bytes and %s is %d; re-run "
                      "tools/embed_lua.py and commit both files",
                      (int)SCN_TRIGGERS_LUA_LEN, kPath, (int)size);
    }
    if (memcmp(src, kScnTriggersLua, (size_t)size) != 0) {
        free(src);
        UT_ASSERT_MSG(0,
                      "the embedded router is %d bytes of something other "
                      "than %s; re-run tools/embed_lua.py and commit both "
                      "files", (int)SCN_TRIGGERS_LUA_LEN, kPath);
    }
    free(src);
    return 0;
}

/* ── 9. Triggers on a derived field ───────────────────────────────── */

/* Three fields a hook carries without being handed them: the team behind a
 * seat or an owner, the tags on the pillbox or base an event names, and the
 * region the square pair it carries falls inside. The router works each out
 * when the trigger fires; none of them is an argument.
 *
 * A team is one value and takes the ordinary operators. A tag and a region
 * are sets and answer one question, which in asks and ne asks the other way
 * round.
 *
 * The teams are the roster's own: server_sim_players.c seats player 0 on
 * team 1 and player 1 on team 2, and the first case holds the sim to that
 * rather than assuming it, so a change there reads as a changed default
 * instead of a trigger that quietly stopped firing. */

/* [new, old, index, quiet, class, mapX, mapY], as the engine's own emit
 * writes one. The hook takes the item first and the old owner before the
 * new, so a case naming an owner names it here in the other order. */
static void shkRaiseCapture(ServerSim *sim, uint8_t type, uint8_t newOwner,
                            uint8_t oldOwner, uint8_t index) {
    uint8_t data[7];

    data[0] = newOwner;
    data[1] = oldOwner;
    data[2] = index;
    data[3] = 0;
    data[4] = (uint8_t)CAPTURE_CLASS_ENEMY;
    data[5] = 52;
    data[6] = 53;
    shkRaise(sim, type, data, sizeof(data));
}

/* A team off an owner, which is nil where the owner is nobody. */
int run_scenario_hooks_trigger_team_from_owner(void) {
    static const char *const kMap = "scnhook_trig_team.map";
    ServerSim          *sim;
    ScenarioHost       *h;
    ServerSimRosterSlot one;
    ServerSimRosterSlot two;
    char                rec[2048];
    char                err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "triggers = {\n"
        "  { when = \"on_base_captured\","
        " where = { { \"old_team\", \"eq\", 1 } },"
        " actions = { { \"log\", \"" SHK_NOTE_MARK "team\" } } },\n"
        "}",
        "function on_base_captured(n, old, new)\n"
        "  note(\"cap \"..n..\" \"..old)\n"
        "end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "One", false);
    serverSimAddPlayer(sim, 1, "Two", false);
    shkFlush(sim);

    UT_ASSERT(serverSimGetRosterSlot(sim, 0, &one));
    UT_ASSERT(serverSimGetRosterSlot(sim, 1, &two));
    UT_ASSERT_MSG(one.team == 1 && two.team == 2,
                  "the roster seated the two players on teams %d and %d, "
                  "and this case is written against 1 and 2",
                  (int)one.team, (int)two.team);

    /* Base 4 taken off player 0, who is on the team the row names. */
    shkRaiseCapture(sim, EVENT_BASE_CAPTURED, 9, 0, 3);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "cap 4 0\nteam\n",
           "the old owner is on team 1, so the row holds");

    /* And off player 1, who is not. */
    shkReset();
    shkRaiseCapture(sim, EVENT_BASE_CAPTURED, 9, 1, 3);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "cap 4 1\n",
           "the old owner is on team 2, so the row does not hold");

    /* And off nobody. An owner may be NEUTRAL, which is the whole reason it
       is a different type from a seat: there is no team to test, so the row
       does not hold rather than answering some team. */
    shkReset();
    shkRaiseCapture(sim, EVENT_BASE_CAPTURED, 9, NEUTRAL, 3);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "cap 4 255\n",
           "the old owner is nobody, so there is no team and the row does "
           "not hold");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* The tags on the item a hook names, on a pillbox and on a base. The two
 * carry different tags, so a kind word taken from the wrong parameter finds
 * nothing and the case fails rather than passing by coincidence. */
int run_scenario_hooks_trigger_tag_on_item(void) {
    static const char *const kMap = "scnhook_trig_tag.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "tags = { pills = { [1] = \"outer\" },"
        " bases = { [1] = \"keep\" } },\n"
        "triggers = {\n"
        "  { when = \"on_pill_captured\","
        " where = { { \"tag\", \"in\", \"outer\" } },"
        " actions = { { \"log\", \"" SHK_NOTE_MARK "pill\" } } },\n"
        "  { when = \"on_base_captured\","
        " where = { { \"tag\", \"in\", \"keep\" } },"
        " actions = { { \"log\", \"" SHK_NOTE_MARK "base\" } } },\n"
        "}",
        "function on_pill_captured(n, old, new) note(\"pillcap \"..n) end\n"
        "function on_base_captured(n, old, new) note(\"basecap \"..n) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    /* Pillbox 1 carries outer; the event's index is one below the number a
       script counts by. */
    shkRaiseCapture(sim, EVENT_PILL_CAPTURED, 3, 4, 0);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "pillcap 1\npill\n",
           "pillbox 1 is tagged outer, so the row holds");

    shkReset();
    shkRaiseCapture(sim, EVENT_PILL_CAPTURED, 3, 4, 1);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "pillcap 2\n",
           "pillbox 2 carries no tag, so the row does not hold");

    shkReset();
    shkRaiseCapture(sim, EVENT_BASE_CAPTURED, 9, 10, 0);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "basecap 1\nbase\n",
           "base 1 is tagged keep, so the row holds — and the kind word came "
           "off the base parameter rather than the pillbox one");

    shkReset();
    shkRaiseCapture(sim, EVENT_BASE_CAPTURED, 9, 10, 1);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "basecap 2\n",
           "base 2 carries no tag, so the row does not hold");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* ne on a set is "does not hold this", and eq asks a set something it
 * cannot answer and so never holds. Both triggers sit on one hook, so each
 * raise puts both questions at once. */
int run_scenario_hooks_trigger_tag_ne_and_eq(void) {
    static const char *const kMap = "scnhook_trig_tagne.map";
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "tags = { pills = { [1] = \"outer\" } },\n"
        "triggers = {\n"
        "  { when = \"on_pill_captured\","
        " where = { { \"tag\", \"ne\", \"outer\" } },"
        " actions = { { \"log\", \"" SHK_NOTE_MARK "absent\" } } },\n"
        "  { when = \"on_pill_captured\","
        " where = { { \"tag\", \"eq\", \"outer\" } },"
        " actions = { { \"log\", \"" SHK_NOTE_MARK "equal\" } } },\n"
        "}",
        "function on_pill_captured(n, old, new) note(\"pillcap \"..n) end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkRaiseCapture(sim, EVENT_PILL_CAPTURED, 3, 4, 0);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "pillcap 1\n",
           "pillbox 1 carries outer, so ne does not hold — and eq does not "
           "hold on a set whatever it carries");

    shkReset();
    shkRaiseCapture(sim, EVENT_PILL_CAPTURED, 3, 4, 1);
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "pillcap 2\nabsent\n",
           "pillbox 2 carries no tag, so ne holds and eq still does not");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}

/* The region a hook's square pair falls inside, asked of one named
 * rectangle rather than worked out by listing them. */
int run_scenario_hooks_trigger_region_holds_square(void) {
    static const char *const kMap = "scnhook_trig_region.map";
    /* [mx, my, layer] — the layer sits past the event's wire size, which is
       where the hook reads it from. */
    static const uint8_t     kInside[3]  = { 62, 62, 0 };
    static const uint8_t     kOutside[3] = { 10, 10, 0 };
    ServerSim    *sim;
    ScenarioHost *h;
    char          rec[2048];
    char          err[512];

    shkReset();
    UT_ASSERT(shkPutTable(
        kMap,
        "regions = { keep = { x = 60, y = 60, w = 4, h = 4 } },\n"
        "triggers = {\n"
        "  { when = \"on_mine_explosion\","
        " where = { { \"region\", \"in\", \"keep\" } },"
        " actions = { { \"log\", \"" SHK_NOTE_MARK "inside\" } } },\n"
        "}",
        "function on_mine_explosion(mx, my, l)\n"
        "  note(\"boom \"..mx..\" \"..my)\n"
        "end\n"));
    sim = shkSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    shkFlush(sim);

    shkRaise(sim, EVENT_MINE_EXPLODED, kInside, sizeof(kInside));
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "boom 62 62\ninside\n",
           "the square is inside the keep, so the row holds");

    shkReset();
    shkRaise(sim, EVENT_MINE_EXPLODED, kOutside, sizeof(kOutside));
    serverSimTick(sim);
    shkRead(rec, sizeof(rec));
    SHK_IS(rec, "boom 10 10\n",
           "the square is outside the keep, so the row does not hold");

    scenarioHostDetach(h);
    shkUnwatchConsole(sim);
    serverSimDestroy(sim);
    shkDrop(kMap);
    shkReset();
    return 0;
}
