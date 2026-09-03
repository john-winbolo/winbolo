/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * server_sim_internal.h
 *
 * Implementation-only header for the ServerSim type.
 * Files that include this header see the full struct
 * definition and may access its fields directly.
 *
 * Outside src/server/server_sim.c,
 * src/server/server_lifecycle.c and the sources under
 * src/server/sim/, callers must include server_sim.h
 * (which exposes only a forward declaration) and use the
 * public accessor/mutator API.
 *********************************************************/
#ifndef SERVER_SIM_INTERNAL_H
#define SERVER_SIM_INTERNAL_H

#include <stddef.h>
#include <time.h>
#include "server_sim.h"
#include "control_event.h"  /* ControlEvent — stored by value in the lobby-chat buffer */
#include "game_sim.h"        /* GameSim layout — used by the sim field below */
#include "position_history.h" /* PosHistory — used by posHistory / lgmPosHistory */
#include "mapgen.h" /* MapGenConfig — embedded by value in randomMapConfig */
#include "brain_list_internal.h" /* BRAIN_LIST_PATH_LEN — brainPaths mirror */
#include "upload_policy.h"  /* UploadPolicy — broadcast in lobby-settings event */
#include "view_policy.h"    /* ViewPolicy / ViewCategory — broadcast in lobby-settings event */
#include "bot_manager.h"    /* BotManager — embedded by value below */
#include "round_stats.h"    /* AwardId, AwardResult — computeAwards output */
#include "attribution_track.h" /* AttrSlotIdentity — per-slot identity snapshot */
#include "transport_udp.h"  /* MAX_SPECTATORS — subscriber capacity */

/* PlayerRoundStats, NotableType, NotableEvent and NOTABLE_EVENTS_MAX are the
 * shared accumulator/timeline types, defined in round_stats.h (included above)
 * so the offline log viewer rebuilds them from the same records. */

/* Control-event subscriber capacity: one slot per tank, one for the local
 * host/SP ClientSim, plus one per possible spectator. Single source of truth
 * for both the subscriber arrays below and every loop bound in server_sim.c. */
#define SUBSCRIBER_SLOT_COUNT (MAX_TANKS + 1 + MAX_SPECTATORS)

/* Cap on the current-session lobby-chat catch-up buffer (oldest dropped). */
#define LOBBY_CHAT_BUFFER_MAX 200

/* Why a return-to-lobby countdown is running, which decides what the
 * returning lobby is told and who gets credited with the win. NONE also
 * covers a game-over that arrives with no countdown at all — a game-time
 * or tick limit expiring — which reports whatever the base sweep says. */
#define RETURN_REASON_NONE        0
#define RETURN_REASON_MANUAL_VOTE 1
#define RETURN_REASON_SURRENDER   2
#define RETURN_REASON_BASE_WIN    3
#define RETURN_REASON_ABANDONED   4

/* roundLogStartTick before the round's first log entry has been written. Not a
 * plausible tick, so it doubles as the "not latched yet" flag. */
#define ROUND_LOG_START_UNSET     0xFFFFFFFFu

struct ServerSim {
    GameSim      sim;    /* MUST be first member */

    /* Tick state */
    uint32_t     tick;
    /* The tick the current round's log segment starts at, latched the first
     * time serverSimLogTick writes while running. Clip times are measured from
     * it: ticks that ran before the log did (the startDelay hold advances the
     * sim without writing an entry) are not part of the round the viewer sees.
     * ROUND_LOG_START_UNSET until that first write, and reset to it whenever
     * tick is. */
    uint32_t     roundLogStartTick;
    int32_t      startDelay;
    int32_t      gameLength;
    int32_t      tickLimit;          /* 0 = unlimited; counts running game-ticks */
    int32_t      ticksRun;           /* Running-state tick counter */
    int32_t      gameTickLimit;      /* 0 = unlimited; ends the running game when reached (no loop exit). */
    int32_t      gameTicksRun;       /* Running-state tick counter paired with gameTickLimit; resets each game. */

    /* Server state machine */
    ServerState  state;
    bool         lobbyEnabled;       /* false = no-lobby mode (skip lobby, play immediately) */
    LobbyPlayer  lobbyPlayers[MAX_TANKS];

    /* ── Lobby Layout A — auto-ally team metadata + bot configs ────
     * teams[] is presentation: name, color, naming pool — keyed by
     * teamNumber 1..MAX_TANKS-1. teams[0] is reserved for "Unassigned"
     * and never has metadata. Persists across rounds with the rest of
     * the lobby state. */
    TeamMetadata    teams[MAX_TANKS];
    LobbyBotConfig  botConfigs[MAX_TANKS];

    BotManager      botMgr;  /* per-sim bot manager — initialised by botManagerInitInSim */

    /* Per-bot brain selection as an index into brainList. 0xFF means
     * "use the global botBrainPath" (the CLI-configured default). The
     * lobby AiConfig dropdown writes here via
     * PACKET_LOBBY_SET_BOT_BRAIN so different bots in the same lobby
     * can run different brains. */
    uint8_t         botBrainIdx[MAX_TANKS];

    /* Discovered brain codebases under brains/ — sent to clients via
     * PACKET_LOBBY_BRAIN_LIST so the AiConfig combo can list them. */
    BrainList       brainList;

    /* Server-private mirror of the on-disk paths for each entry in
     * brainList. Populated in lockstep with brainList by brainListScan
     * and indexed identically (brainPaths[i] is the disk path for
     * brainList.entries[i]). Kept off the public catalogue so the path
     * never appears on the public API or the wire. */
    char            brainPaths[BRAIN_LIST_MAX][BRAIN_LIST_PATH_LEN];

    /* Layout A lobby flags — all persist across rounds. */
    bool     openHost;             /* anyone can edit when true */
    BYTE     hostSlot;             /* current lobby host's player slot; 0 = slot 0 */
    bool     allowNewPlayers;      /* live state — drives PACKET_LOCK_TOGGLE */
    bool     autoLockOnGameStart;  /* if true, set allowNewPlayers=false on game start */
    bool     savedAllowNewPlayers; /* what allowNewPlayers was before autoLockOnGameStart fired */
    bool     ranked;               /* LST_RANKED — server enforces bots-off and
                                    * rejects gameType=Open while true. Toggle-on
                                    * also removes existing bots from teams. */
    bool     firstJoinerBecomesHost; /* dedicated-server promotion mode. Set
                                      * via -firstjoinhost. When true and slot
                                      * 0 is empty (or unowned), the next
                                      * incoming player is promoted to host.
                                      * Consumed by the join handler. */
    uint16_t serverLocks;          /* LOBBY_LOCK_* bitmask, set from CLI */
    /* WBN lobby_update batching: lobby/setting/map changes mark the
     * snapshot dirty; it is flushed on the periodic WBN tick at most
     * every WBN_LOBBY_UPDATE_INTERVAL seconds, and force-sent before
     * the lobby countdown starts. */
    bool     wbnLobbyDirty;        /* a lobby field changed since last send */
    time_t   wbnLobbyLastSent;     /* last server/lobby_update send time */
    bool     wbnSessionRotating;   /* TRUE between round-end and the next
                                      * session's register: suppresses
                                      * lobby_update so the next round's map
                                      * never reports against the old (just-
                                      * quit) server_key. The change is held
                                      * as dirty and flushed on the new key. */
    uint8_t  mapMd5[16];           /* MD5 of the active map's BMAPBOLO bytes */
    bool     mapMd5Valid;          /* mapMd5 holds a usable hash */
    char     mapMd5Hex[33];        /* mapMd5 as 32 lowercase hex chars + NUL; "" when invalid */
    UploadPolicy uploadPolicy;     /* mirrored from server-startup config */
    /* Per-category visibility rules, indexed by ViewCategory. Set from
     * the CLI / hosting prefs at startup and from the lobby via
     * LST_PILL_VIEW / LST_BASE_VIEW / LST_ALLY_VIEW; broadcast in the
     * lobby-settings event. decaySecs only matters for viewPolicyDecay
     * but is carried for every category so the lobby UI can keep the
     * host's value while they flip between modes. */
    ViewPolicy viewPolicy[VIEW_CATEGORY_COUNT];
    uint16_t   viewDecaySecs[VIEW_CATEGORY_COUNT];
    BYTE     maxPlayers;           /* cap on join slots; 0 falls back to MAX_TANKS */
    BYTE     maxBots;              /* cap on AI bots in the lobby; 0 = no cap */
    BYTE     maxSpectators;        /* 0 = spectating disabled */
    uint32_t specDelayTicks;       /* spectator view delay in ticks (50 ticks/s) */
    bool     worldPreLoaded;       /* TRUE while the world is fresh from
                                    * serverSimCreate*; FALSE after the first
                                    * serverSimResetGameWorld. Drives the
                                    * all-ready detector to pick
                                    * StartGameInPlace vs countdown+StartGame. */

    /* Game-settings mirrors — needed for live mid-lobby change broadcasts.
     * The authoritative values live in GameSim/serverSim CLI args; these
     * track the most recently broadcast value so we can detect/refuse
     * locked changes and emit SETTING_CHG diffs cleanly. */
    uint8_t  aiPolicy;             /* mirrors aiType passed at create */
    bool     timeLimit;            /* derived from gameLength != UNLIMITED */
    uint16_t timeMinutes;          /* user-facing minutes (display + edit) */

    int32_t      countdownTicks;     /* Countdown timer (in ticks) */
    int32_t      originalGameLength; /* Cached for reset between rounds */

    /* Operator-configured lobby settings captured once at startup (end of
     * serverSimApplyInstanceConfig). Restored by serverSimResetLobbyToDefaults
     * when the last human leaves the lobby, so the next joiner lands in the
     * server's configured defaults instead of whatever the previous occupants
     * left behind. */
    struct {
        bool     valid;
        gameType gameType;
        bool     hiddenMines;
        aiType   botAiType;
        uint8_t  aiPolicy;
        bool     timeLimit;
        uint16_t timeMinutes;
        int32_t  gameLength;
        bool     openHost;
        bool     autoLockOnGameStart;
        bool     ranked;
        uint16_t serverLocks;
        ViewPolicy viewPolicy[VIEW_CATEGORY_COUNT];
        uint16_t   viewDecaySecs[VIEW_CATEGORY_COUNT];
    } originalLobbySettings;
    bool         hadPlayersEver;     /* For auto-close detection */
    bool         roundHadHuman;      /* A human was present during this running
                                      * round; gates the return-to-lobby when
                                      * the last human leaves so a bot-only
                                      * game start doesn't loop. */
    bool         quitOnWin;          /* Server should check for win condition */
    bool         autoCloseOnEmpty;   /* Server should close when all players leave */
    bool         mapRotateEnabled;   /* No-lobby map-rotation mode: a win or an
                                      * empty server boots everyone, picks the
                                      * next map, and restarts a fresh round
                                      * instead of quitting. Set by -maprotate. */
    char         pendingWinMessage[512]; /* Win message to send after returning to lobby */
    bool         emptyResetEnabled;  /* Reset to lobby when empty for emptyResetMinutes */
    int          emptyResetMinutes;  /* Minutes before empty reset (default 5) */
    int32_t      emptyResetTicks;    /* Countdown ticks remaining (-1 = not counting) */

    /* Map reload — cached compressed map for between-round resets */
    BYTE        *cachedMapData;      /* Compressed map buffer (malloc'd) */
    int          cachedMapDataLen;   /* Length of compressed data */

    /* Lobby preview-map state. When a player picks a map from the
     * chooser (Server Maps click OR a completed upload), the server
     * stashes the prior committed map here before applying the
     * preview — so Cancel can revert without re-reading from disk.
     * NULL when no preview is pending; freed on Commit. Stays
     * preserved across successive previews (we keep the ORIGINAL
     * committed map, not the most-recent preview, so one Cancel
     * rolls back to where the user started). */
    BYTE        *previousMapData;
    int          previousMapDataLen;
    char         previousMapName[MAP_STR_SIZE];

    /* Info packet fields — stored at creation for server browser responses */
    char         mapName[MAP_STR_SIZE];
    uint32_t     timeCreated;
    unsigned short serverPort;

    /* Per-player input queues — allows 2 inputs per server timer callback */
#define SERVER_INPUT_QUEUE_SIZE 16  /* Must be power of 2 */
    InputPacket  inputQueue[MAX_TANKS][SERVER_INPUT_QUEUE_SIZE];
    uint8_t      inputQueueHead[MAX_TANKS];  /* Next slot to write */
    uint8_t      inputQueueTail[MAX_TANKS];  /* Next slot to read */
    bool         playerConnected[MAX_TANKS];
    uint32_t     lastProcessedInput[MAX_TANKS];  /* Tick of last processed input per player */
    uint8_t      lastInputButtons[MAX_TANKS];    /* Last button bitmask for stall continuity */
    uint32_t     lastActionAppliedTick[MAX_TANKS]; /* newest input tick whose one-shot
                                                    * action (fire/mine/build) was
                                                    * executed or harvested */
    uint8_t      pendingHarvestActions[MAX_TANKS]; /* harvested LAY_MINE bit awaiting
                                                    * the next applied input */
    BYTE         pendingHarvestBuildAction[MAX_TANKS]; /* harvested build (0 = none) */
    BYTE         pendingHarvestBuildX[MAX_TANKS];
    BYTE         pendingHarvestBuildY[MAX_TANKS];
    uint16_t     playerPing[MAX_TANKS];           /* Per-player ping in ms (server-measured RTT) */

    /* Input jitter buffer — delay processing until buffer reaches target depth */
#define JITTER_BUFFER_MIN       1   /* Minimum buffer depth (ticks) */
#define JITTER_BUFFER_MAX       6   /* Maximum buffer depth (ticks) — 6 ≈ 60ms,
                                     * headroom for ~25ms jitter plus the
                                     * send-cadence bunching it rides on */
#define JITTER_BUFFER_DEFAULT   2   /* Starting depth before we have data */
#define JITTER_GROW_THRESHOLD   2   /* Consecutive stalls before growing */
#define JITTER_STARVE_GROW_THRESHOLD 2 /* Drain events before growing — a queue
                                        * that empties when input was expected
                                        * is the reliable too-shallow signal,
                                        * counted even while re-filling */
#define JITTER_SHRINK_INTERVAL 100  /* Ticks of no stalls before shrinking */
#define LAG_COMP_MAX_TICKS 14       /* 280ms max rewind (14 history entries); viewTick measures
                                     * the true view age (~ping+20ms+jitterWait), ~2x the old
                                     * ping/2 estimate, so the cap is raised from 12 to keep the
                                     * high-ping case from clipping. POSITION_HISTORY_SIZE=16
                                     * (320ms) has headroom. */
/* Consecutive dry half-steps before a stall is treated as genuine loss
 * and the server stall-advances (consumes the tick). At/below this, a
 * dry half-step is routine send-burst cadence ripple: repeat held buttons
 * and wait, so the in-flight real input still applies at its true tick.
 * Bounds reintroduced overshoot to this many half-steps under real loss;
 * tune up if localhost recon/s isn't ~0, down if high-ping overshoot
 * returns. */
#define STALL_ADVANCE_DRY_TICKS 4
    uint8_t inputBufferFilled[MAX_TANKS];  /* true once initial fill reached */
    uint8_t  jitterTarget[MAX_TANKS];      /* Current adaptive buffer depth */
    uint8_t  jitterStallCount[MAX_TANKS];  /* Consecutive ticks queue was empty when expected */
    uint8_t  jitterStarveCount[MAX_TANKS]; /* Recent drain events (queue emptied when input expected) */
    uint16_t jitterStableTicks[MAX_TANKS]; /* Ticks since last stall */
    uint8_t inputDryTicks[MAX_TANKS]; /* consecutive half-steps with no fresh
                                       * input; gates stall-advance vs wait */

    /* Per-player input-pipeline instrumentation — window counters reset
     * each logged second, plus one gauge. Reads/writes only; never gate
     * sim behaviour on these. Logged once per second by the [netstat] line. */
    uint16_t statStallTicks[MAX_TANKS];         /* stall-branch executions this window */
    uint16_t statGapFillTicks[MAX_TANKS];       /* gap-filled ticks this window */
    uint16_t statDroppedStaleInputs[MAX_TANKS]; /* stale queue entries skipped this window */
    uint16_t statDroppedEdge[MAX_TANKS];  /* stale drops that were a recent button change (diagnostic) */
    uint16_t statCatchupTicks[MAX_TANKS];       /* extra catch-up dequeues this window —
                                                 * one per backlog-bleed apply when
                                                 * post-dequeue depth > jitterTarget + 1 */
    uint8_t  statLastRewindTicks[MAX_TANKS];    /* gauge: most recent lag-comp rewind */

#ifdef WB_NETDEBUG
    /* Net-debug rig: sim-executed turn half-steps and mine lays per
     * player. Test-only — never compiled into production builds. */
    uint32_t dbgExecTurnTicks[MAX_TANKS];
    uint32_t dbgMineLays[MAX_TANKS];
#endif

    PosHistory   posHistory[MAX_TANKS];           /* Position history for lag compensation */
    PosHistory   lgmPosHistory[MAX_TANKS];        /* LGM position history for lag compensation */

    /* Which player is currently being processed in the tick loop.
     * Used by serverSimCbMessageAdd to target assistant messages. */
    uint8_t      currentTickPlayer;

    /* Event buffer — populated during tick, consumed by snapshot sender */
    GameEvent    events[MAX_SNAPSHOT_EVENTS];
    uint8_t      eventCount;

    /* Separate map-change event buffer — never competes with sound/game
     * events for slots, so map changes are never silently dropped. */
#define MAX_MAP_EVENTS 256
    GameEvent    mapEvents[MAX_MAP_EVENTS];
    uint16_t     mapEventCount;

    /* What each connected client's copy of the terrain looks like: every
     * tile the server has actually sent it. Identical to the real map while
     * every map event is broadcast; the CRC in that client's snapshot header
     * and its resync blob read this copy, so the two ends agree about the
     * map the client was really given. The handle array holds
     * &clientKnownMapObj[i] so the bolo_map.c entry points, which all take a
     * `map *`, can be called against a slot's copy. */
    struct mapObj clientKnownMapObj[MAX_TANKS];
    map           clientKnownMap[MAX_TANKS];

    /* Previous pill/base state for change detection */
    PillSnapshot prevPills[MAX_SNAPSHOT_PILLS];
    BaseSnapshot prevBases[MAX_SNAPSHOT_BASES];
    uint8_t      prevPillCount;
    uint8_t      prevBaseCount;

    /* Per-recipient last closest base (1-based, or BASE_NOT_FOUND). When a
     * recipient's closest base changes, its current stock is pushed immediately
     * so ammo appears on arrival instead of waiting for the next full-sync. */
    uint8_t      lastClosestBase[MAX_TANKS];

    /* Full state sync tracking, per recipient. Each client's own per-client
     * snapshot build manages its own full-sync cadence; a scalar here let the
     * first client built each interval consume it and starve the rest. */
    uint32_t     lastFullSyncTick[MAX_TANKS];

    /* viewPolicyDecay: last sim tick the player's tank was within
     * VIEW_DECAY_NEAR_TILES of the item. 0 = never (not viewable). */
    uint32_t     pillNearTick[MAX_TANKS][MAX_PILLS];
    uint32_t     baseNearTick[MAX_TANKS][MAX_BASES];
    uint32_t     allyNearTick[MAX_TANKS][MAX_TANKS];

    /* What each player last reported viewing (CMD_VIEW_STATE). Only
     * consulted when a category's policy is viewPolicyKey. */
    uint8_t      viewKind[MAX_TANKS];     /* 0=tank, 1=pill, 2=base, 3=ally */
    uint8_t      viewTarget[MAX_TANKS];

    /* Last map square the builder saw this player's tank at; keeps a dead
     * or tankless player's view anchored instead of falling back to the
     * whole map. */
    uint8_t      lastTankMX[MAX_TANKS];
    uint8_t      lastTankMY[MAX_TANKS];
    bool         lastTankValid[MAX_TANKS];

    /* Bot configuration — cached from CLI args for lobby bot creation */
    char         botBrainPath[260];       /* Brain path for lobby bot creation */
    aiType       botAiType;               /* AI advantage level for bots */

    /* WBN registration — cached from CLI args for re-registration between rounds */
    bool         hasPassword;             /* Server has a password set */
    char         password[MAP_STR_SIZE];  /* Runtime join-handshake password text */

    /* WBN team balance proposal */
    BalanceProposal balanceProposal;

    bool mapSkipVotes[MAX_TANKS]; /* per-slot map skip vote */

    /* In-game vote state (back-to-lobby + surrender). Indexed by
     * (kind - 1). See netpacks.h GAME_VOTE_KIND_* / docs/voting_plan.md. */
    struct ServerGameVote {
        uint8_t  kind;             /* GAME_VOTE_KIND_* (BACK_TO_LOBBY/SURRENDER) */
        uint8_t  active;           /* GAME_VOTE_ACTIVE_* */
        uint8_t  triggerSrc;       /* GAME_VOTE_TRIGGER_* */
        uint8_t  teamId;           /* surrender only; 0 = all-teams */
        uint16_t votesMask;        /* bit i = slot i voted yes */
        uint16_t answeredMask;     /* bit i = slot i has answered (yes or no) */
        uint64_t startMs;          /* SDL_GetTicks-style epoch (server frame time) */
        uint64_t deadlineMs;       /* startMs + GAME_VOTE_DEADLINE_SECONDS*1000 */
        uint64_t lastHeartbeatMs;  /* drives 1Hz broadcast */
        uint64_t concludedAtMs;    /* >0 once active != RUNNING; for auto-dismiss */
        /* Pre-pass grace: once the vote first becomes unanimous we
         * give voters 5 s to change their mind before concluding
         * PASSED. 0 = not pending, else the wall-clock at which the
         * pass will actually fire. If a voter retracts during the
         * grace (yesCount drops below threshold), this clears back
         * to 0 and the vote keeps running normally. */
        uint64_t pendingPassUntilMs;
    } gameVotes[2];
    uint64_t gameVoteWallMs;       /* monotonic ms since serverSim start */

    /* Forced return-to-lobby countdown (e.g. from a vote-pass). When
     * > 0, the running-state tick decrements this each call; at 0
     * we call serverSimEnterGameOver. Carried in every snapshot
     * header so clients can render their own "Returning to lobby in
     * N" indicator off the value. */
    int32_t  returnToLobbyTicks;
    /* Why the current countdown is running, or why the round just ended —
     * see RETURN_REASON_*. Decides the message the returning lobby gets and
     * whether WinBolo.net win events are credited. Reset by
     * serverSimGameVoteResetAll. */
    uint8_t  returnToLobbyReason;
    /* The team that gave up, when returnToLobbyReason is SURRENDER. The
     * opposing side is credited with the win; the base sweep never fires on
     * a surrender. Reset by serverSimGameVoteResetAll. */
    uint8_t  returnToLobbyTeamId;

    /* Map directory rotation — validated map file paths for random selection */
    char       **mapDirFiles;             /* Array of validated map file paths (malloc'd) */
    int          mapDirCount;             /* Number of valid maps in the array */
    /* Operator-configured root for all server-side map I/O: lobby map
     * list, SET_MAP path resolution, search. NULL → fall back to the
     * built-in "data/maps". Captured in serverSimMapDirBuild from the
     * -mapdir CLI arg with any trailing slash stripped. */
    char        *mapDirPath;
    /* Absolute directory backing the virtual "Uploads/" folder for
     * PERSIST-policy uploads. Empty → "<mapDirPath>/Uploads". Set from
     * ServerInstanceConfig.uploadPersistDir at startup. */
    char         uploadPersistDir[FILENAME_MAX];

    /* Random map generation (for -randommap mode) */
    bool         randomMapEnabled;       /* true when using -randommap */
    MapGenConfig randomMapConfig;        /* last used config */
    bool         randomMapFixedSeed;     /* true = same map every round */

    /* Server message configuration (was servermessages.c globals) */
    bool         isServerQuiet;
    char         serverMessageLogFile[FILENAME_MAX];
    bool         serverMessageUseLogFile;

    /* Log recording configuration (set from CLI args, used by serverSimStartGame) */
    bool         wantLogging;
    char         userLogFileName[512];

    /* Post-game stats accumulator — populated during running, reset per round. */
    PlayerRoundStats roundStats[MAX_TANKS];
    NotableEvent     notableEvents[NOTABLE_EVENTS_MAX];
    int              notableEventCount;

    /* Per-round attribution record stream (packed attribution_track.h records),
     * appended during a running round and reset each round. */
    uint8_t         *trackBuf;         /* growable per-round attribution record stream */
    size_t           trackLen;         /* bytes used */
    size_t           trackCap;         /* bytes allocated */
    uint32_t         trackRecordCount; /* records appended this round */
    bool             trackTruncated;   /* set once ATTRIBUTION_TRACK_CAP_BYTES is hit */
    AttrSlotIdentity trackIdentity[MAX_TANKS]; /* snapshot at game-over freeze */

    /* In-process control event subscribers (bot ClientSims, SP humanSim,
     * and live-lobby spectators). */
    ControlSubscriber subscribers[SUBSCRIBER_SLOT_COUNT];
    uint16_t          subscriberGen[SUBSCRIBER_SLOT_COUNT];
    int               numSubscribers;
    bool              publishing;

    /* Spectator roster enumerator (registered by the transport layer). Invoked
     * during sync-replay to emit one CTRL_SPECTATOR_SLOT per connected
     * spectator; NULL when no enumerator is registered. */
    SpectatorRosterEnumFn specRosterEnum;
    void                 *specRosterEnumCtx;

    /* Current-session lobby-chat catch-up buffer. Broadcast player chat
     * (CTRL_CHAT, destPlayer 0xFF) and spectator chat (CTRL_SPECTATOR_CHAT)
     * captured while the server is in lobby/countdown, cleared at game start,
     * capped at LOBBY_CHAT_BUFFER_MAX (oldest dropped). Replayed to a spectator
     * on its delayed->live drain-flip so it sees the lobby chat sent while it
     * was still finishing the delayed game; fresh joins get nothing. */
    ControlEvent lobbyChatBuffer[LOBBY_CHAT_BUFFER_MAX];
    int          lobbyChatCount;
};

BOLO_STATIC_ASSERT(offsetof(struct ServerSim, sim) == 0,
                   ServerSim_sim_must_be_first_member);

/* Per-recipient visibility region: the client's tank screen plus a screen for
 * each allied pillbox, base and tank the view policies let through. Shared by
 * the snapshot cull and the best-effort game-event cull. Sized for the worst
 * case — the tank plus every pill, base and other tank at once. */
#define MAX_VIEWPORTS (1 + MAX_PILLS + MAX_BASES + MAX_TANKS)

typedef struct {
    int minMX, maxMX, minMY, maxMY;
} ViewportRect;

/* Fill `out` (capacity maxOut) with clientIdx's tank screen plus one screen
 * per allied pillbox, base and tank that its category's ViewPolicy allows.
 * A client with no tank gets a screen at its last known position instead.
 * Returns the count, which can be 0 (a slot that never had a tank, with every
 * category off). */
int  serverSimBuildViewports(ServerSim *sim, BYTE clientIdx, ViewportRect *out, int maxOut);
bool inAnyViewport(const ViewportRect *vps, int count, int mx, int my);

/* Per-client copies of the terrain (clientKnownMap above). Seed points a
 * slot's handle at its storage and copies the live map into it; SeedAll does
 * every slot. Called wherever the real map is installed or replaced — sim
 * create, map load, lobby map change, round reset — and for one slot when a
 * player joins, so the copy a client is given always starts as the map the
 * server holds. */
void serverSimShadowSeed(ServerSim *sim, BYTE slot);
void serverSimShadowSeedAll(ServerSim *sim);

/* Write this tick's map changes into every slot's copy. Called once per
 * running frame from the tick core, after both half-steps have finished
 * filling mapEvents and before any transport drains them, so in-process
 * clients and bots track the same way UDP clients do. Apply writes one tile
 * into every slot's copy — the per-event step ShadowTick loops over, also
 * called directly by the transport's test-only map-event injector, which
 * stages a change without a tick to carry it. */
void serverSimShadowApply(ServerSim *sim, BYTE x, BYTE y, BYTE terrain);
void serverSimShadowTick(ServerSim *sim);

/* serverSimGetCompressedMap over one slot's copy of the terrain, with the
 * live pills, bases and starts. The blob a client downloads on join or
 * resync comes from here, so it carries the terrain that client's snapshot
 * checksum is computed over. Returns the compressed length. */
int  serverSimGetCompressedMapFor(ServerSim *sim, BYTE slot, BYTE *output);

/* Stamp the proximity clocks the viewPolicyDecay categories read: for every
 * connected player with a live tank, every pill/base/tank within
 * VIEW_DECAY_NEAR_TILES map squares of it. Only categories set to
 * viewPolicyDecay are walked. Called once per running half-step from the tick
 * core in server_sim_tick.c. */
void serverSimUpdateViewDecay(ServerSim *sim);

/* Re-check every connected player's reported view (viewKind/viewTarget) and
 * reset it to VIEW_KIND_TANK when the claimed target no longer earns a view:
 * the category is viewPolicyOff, the target is out of range or no longer
 * qualifies (allied / alive / not carried), or — under viewPolicyDecay — the
 * viewer's proximity clock for it has expired. There is no server→client
 * event: the rect simply stops being built and the client exits the view on
 * its own. Called once per running half-step alongside
 * serverSimUpdateViewDecay. */
void serverSimValidateViewTargets(ServerSim *sim);

/* Fill `out` with base `baseIdx0`'s (0-based) current shells/mines/armour as an
 * EVENT_BASE_STOCK (data[0]=baseIdx0, data[1]=armour, data[2]=shells,
 * data[3]=mines). */
void serverSimBuildBaseStockEvent(ServerSim *sim, BYTE baseIdx0, GameEvent *out);

/* If `closest` (1-based, or BASE_NOT_FOUND) differs from the recipient's last
 * recorded closest base, fill `out` with that base's current stock as an
 * EVENT_BASE_STOCK and return true; always updates the recorded value.
 * Returns false (no event) when unchanged or BASE_NOT_FOUND. */
bool serverSimTakeClosestBaseStock(ServerSim *sim, BYTE recipient, BYTE closest, GameEvent *out);

/* Compute `recipient`'s closest neutral/allied base and, when it has just
 * changed, take its arrival base-stock push via serverSimTakeClosestBaseStock.
 * Returns true and fills `out` when a push should be delivered. The local
 * (in-process) transport calls this after building a snapshot: unlike the UDP
 * path it has no event channel to drain, and the snapshot build no longer
 * emits the push (it shared sim state across clients). Bots are excluded, as
 * in the build — they read base stock via the periodic full sync. */
bool serverSimTakeArrivalBaseStock(ServerSim *sim, BYTE clientIdx, GameEvent *out);

/* World-unit ceiling for selecting `client`'s closest base when gating which
 * base's stock to send. Wider than the client's display range (BASE_STATUS_RANGE)
 * by an RTT-sized margin so the stock is cached before the client's display
 * switches to that base. See the definition for the margin formula. */
WORLD serverSimClosestBaseSendRange(const ServerSim *sim, BYTE client);

/* Server-side handler for PACKET_GAME_VOTE_TOGGLE. T2 because the only
 * legitimate callers are the two transport-layer dispatch sites — the
 * UDP wire handler (src/server/transport_udp_server.c) and the
 * in-process local-transport handler (src/bolo/transport_local.c).
 * GUI code reaches it indirectly via clientSimNetSendGameVoteToggle on
 * the T1 surface. */
void serverSimGameVoteToggle(ServerSim *sim, uint8_t playerNum,
                             uint8_t kind, uint8_t toggleMode);

/* Compute how many posHistory entries to rewind hit-detection for a fired
 * input. When viewTick is known (non-zero, not ahead of simTick), rewind the
 * real view age: (simTick - viewTick) server ticks / 2 = posHistory entries
 * (history records once per game tick = 20ms). Otherwise fall back to the
 * ping-based estimate (pingMs/2 + interp buffer). Result is clamped to
 * LAG_COMP_MAX_TICKS. viewTick is client-supplied, but so is pingMs today, so
 * the trust model is unchanged and the cap bounds any abuse. T2 so the unit
 * test can include and drive it directly. */
uint8_t serverSimComputeLagCompTicks(uint32_t simTick, uint32_t viewTick,
                                     uint16_t pingMs);

/* Test/inspection accessor: pointer to slot's round stats, or NULL if
 * slot >= MAX_TANKS. */
const PlayerRoundStats *serverSimGetRoundStats(const ServerSim *sim, BYTE slot);

/* computeAwards / roundStatsApplyRecord — the shared derivation over the
 * public record/stats types — are declared in round_stats_derive.h. */

/* Build the curated end-of-round summary (per-connected-slot scoreboard
 * rows + computed awards) from the finalized accumulator. wbnLogKey is
 * left empty. Reads sim->roundStats / playerConnected; touches no state. */
void serverSimBuildRoundStatsSummary(ServerSim *sim, RoundStatsSummary *out);

#endif /* SERVER_SIM_INTERNAL_H */
