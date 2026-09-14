/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * client_sim_internal.h
 *
 * Defines the full ClientSim struct layout. The public
 * header (client_sim.h) exposes only a forward declaration;
 * files that need direct field access must include this
 * header instead and use the implementation-internal view.
 *
 * Allowed includers, all inside src/bolo/ — the client sim's own
 * translation units (client_sim.c, client_sim_control.c,
 * client_snapshot.c, client_net.c, client_ui_events.c,
 * client_render.c, viewport.c) and the sim sources that reach a
 * ClientSim while running (bases.c, brain_data.c, bot_manager.c,
 * lgm.c, pillbox.c, players.c, screenlgm.c, sounddist.c, tank.c,
 * transport_udp_client.c) — plus src/server/transport_udp_server.c,
 * a co-owner of the sim, for the wire caps declared here.
 * All other callers must include client_sim.h and use the
 * public accessor API.
 *********************************************************/
#ifndef CLIENT_SIM_INTERNAL_H
#define CLIENT_SIM_INTERNAL_H

#include <stddef.h>
#include "client_sim.h"
#include "game_sim.h"
#include "server_sim.h"   /* SubscriberHandle */
#include "viewport.h"
#include "transport.h"
#include "client_state.h"
#include "interpolation.h"
#include "messages.h"
#include "scroll.h"
#include "brain_list.h"
#include "round_stats.h"   /* RoundStatsSummary — lastRoundStats store */
#include "lobby_bot_pools.h" /* LOBBY_BOT_CATALOG_WIRE_MAX */
#include "upload_policy.h"
#include "view_policy.h"   /* ViewPolicy / VIEW_CATEGORY_COUNT — server view-rule mirror */
#include "ping_display.h"  /* PingDisplay — per-slot ping readout smoothing */
#include "wire_limits.h"   /* LOBBY_MAP_UPLOAD_MAX_BYTES */
#include "transport_udp.h" /* MAX_SPECTATORS */
#include "input_packet.h"  /* PING_SPAM_MAX_30S — client render backstop ring */
#include "control_event.h" /* LOBBY_SCENARIO_*_LEN — the scenario mirror below */

/* Internal helpers relocated from client_sim.h during the public-header
 * transitive-leak cleanup. These need GameSim's full layout, so they
 * belong on the internal side. Callers inside src/bolo/ that use these
 * include this header. */
#define MY_TANK(cs) (clientSimGetGameSim(cs)->tanks[clientSimGetMyPlayerNum(cs)])
#define MY_LGM(cs)  (clientSimGetGameSim(cs)->lgmen[clientSimGetMyPlayerNum(cs)])

/* Recover the owning ClientSim from a GameSim* for the frontEnd
 * active-cs gate. Relies on the "GameSim sim MUST be first member"
 * invariant declared above (and mirrored in server_sim.h). When the
 * GameSim belongs to a ServerSim (sim->isServer == true) the cast
 * would yield a bogus pointer, so we return NULL; the gate then
 * suppresses safely because NULL never matches the registered active
 * humanSim. Callers in bolo/<*> with only a GameSim* in scope use this
 * to feed frontEnd*(cs, ...) calls without having to thread cs
 * through every signature. */
static inline struct ClientSim *clientSimFromSim(struct GameSim *sim) {
    if (sim == NULL || sim->isServer) return NULL;
    return (struct ClientSim *)sim;
}

/* Camera / visible-tile state bundled into one struct so viewport math
 * functions in viewport.c can take a ViewPort* without depending on
 * the full ClientSim layout. Embedded by value in struct ClientSim
 * below; reached externally through clientSimViewport /
 * clientSimViewportMut. */
struct ViewPort {
    BYTE        xOffset;
    BYTE        yOffset;
    screen      view;
    screenMines mineView;
    screenHidden hiddenView;
    /* What the camera is parked on. viewKind is a ViewStateKind
     * (client_command.h): VIEW_KIND_TANK follows the local tank, the other
     * three watch one item. viewTarget is the pill or base index (0-based,
     * the same numbering the server's CMD_VIEW_STATE expects) or the ally's
     * player number, and is 0 in tank view. viewX/viewY are the map square
     * the item view is centred on; a pill or base view keeps them fixed,
     * an ally view rewrites them each display tick as the ally drives. */
    uint8_t     viewKind;
    BYTE        viewTarget;
    BYTE        viewX;
    BYTE        viewY;
    int         cursorPosX;
    int         cursorPosY;
    bool        needRecalc;
};

/* One captured spectator forward record: the server's 9-byte transport header
 * ([u8 isKf][u32 gameTick BE][u32 segment BE]) parsed off, followed by the raw
 * ring payload it framed. Queued in arrival (ring-seq) order on the spectator
 * feed below; the spectator session pops them in order to translate + pump.
 * payload is owned (malloc), freed when the node is popped or the feed cleared;
 * payload is NULL when payloadLen is 0 (an idle tick carries no events). */
typedef struct ClientSpecRecordNode {
    bool      isKeyframe;
    uint32_t  gameTick;
    uint32_t  segment;
    uint8_t  *payload;
    uint32_t  payloadLen;
    struct ClientSpecRecordNode *next;
} ClientSpecRecordNode;

/* Per-connection spectator feed buffer. The bulk sink captures the one seed
 * blob (the delayed ring keyframe, raw — translated in a later slice) and the
 * ordered forward records that follow it, both off CHANNEL_BULK; the spectator
 * session drains them via the clientSimSpectator* accessors. Empty (all NULL/0)
 * until a spectator connect populates it; freed on teardown. */
typedef struct {
    uint8_t              *seedBlob;     /* owned; NULL until the seed lands */
    uint32_t              seedLen;
    bool                  seedReady;
    ClientSpecRecordNode *recordHead;   /* FIFO: oldest at head */
    ClientSpecRecordNode *recordTail;
    uint32_t              recordCount;
    /* Cold-start countdown the server sends raw on CHANNEL_CONTROL while the
     * delayed ring fills, before the seed is ready. countdownReceived gates the
     * value's validity; remaining is in game ticks (~50/sec). */
    uint32_t              countdownRemaining;
    bool                  countdownReceived;
    /* Live-lobby mode mirror, copied one-way from the transport's
     * specLiveLobby (the transport is the source of truth). True while the
     * spectator is fed the live lobby control bus; false once the delayed ring
     * feed begins. The spectator session reads it to alternate the read-only
     * lobby and the delayed game view. */
    bool                  liveLobby;
} ClientSpectatorFeed;

struct ClientSim {
    GameSim     sim;    /* MUST be first member */
    BYTE        myPlayerNum; /* Server-assigned player number; tanks[myPlayerNum] is our tank */

    /* Client-side prediction state */
    ClientState clientState;

    /* Interpolation state for other players' tanks */
    InterpContext interpCtx;

    /* Adaptive display-delay controller for render-time interpolation.
     * Zero-initialised by the clientSimCreate memset (depth 1, first frame
     * discrete). */
    InterpRenderCtl interpRenderCtl;

    /* Server shell snapshots for UDP mode */
    ShellSnapshot serverShellSnaps[MAX_SNAPSHOT_SHELLS];
    int         serverShellCount;

    /* Client-side predicted shells (Phase 4 of client-prediction) */
    PredictedShell predictedShells[MAX_PREDICTED_SHELLS];
    int         predictedShellCount;

    /* Render-only forward-projected shells for human clients. Parallel to
     * serverShellSnaps (never mutated): anchored to each snapshot by age and
     * advanced per game tick so incoming shells render at their present
     * position. Empty for bots, which read raw serverShellSnaps. */
    ProjectedShell projectedShells[MAX_SNAPSHOT_SHELLS];
    int         projectedShellCount;

    /* Min-over-window RTT the transport stamps in each PONG; the snapshot
     * path anchors forward-projection to it rather than the display ping. */
    uint16_t    projectionPingMs;

    /* Per-slot conditioning for the ping the player rows render — smoothing,
     * repaint deadband and colour-band hysteresis over the raw RTT each
     * snapshot carries. Display only; projectionPingMs above is what the
     * sim-affecting paths read. */
    PingDisplay displayPing[MAX_TANKS];

    /* Reconciliation stats — current 1s window + last completed window */
    uint16_t reconCountThisWindow;
    float    reconErrSumPx, reconErrMaxPx;
    uint32_t reconWindowStartTick;     /* game-tick the window opened */
    uint16_t reconCountLastSec;
    float    reconErrAvgPxLast, reconErrMaxPxLast;

    /* Render-only error offset: reconciliation corrections accumulate here
     * and decay to zero; the sim never reads these. */
    float errX, errY;       /* world units */
    float errAngle;         /* TURNTYPE units (bradians) */

    /* Countdown of remaining local-tank snapshots over which reconcile
     * corrections use the enlarged base-unblock position clamp. Set when a
     * base next to the tank just became drivable (armour fell to capturable),
     * so the high-RTT catch-up onto the now-passable tile glides instead of
     * snapping; decremented once per local-tank snapshot back to 0. */
    int basePassableSmoothSnapshots;

    /* Pending human-player build request */
    BYTE        pendingBuildAction;
    BYTE        pendingBuildX;
    BYTE        pendingBuildY;

    /* Current building item selected (was BsCurrent) */
    buildSelect currentBuildSelect;

    /* Are we running? */
    bool        running;

    /* Is this a bot ClientSim? (bot sims must not trigger frontend UI calls) */
    bool        isBot;

    /* Single-player session — the local spServerSim runs in-process via
     * transport_local. Lobby UI hides multiplayer-only controls (Allow
     * new players / Disallow once started) and routes setting changes
     * directly to the server instead of through the UDP packet path. */
    bool        isSinglePlayer;
    /* LAN-only host session — set by gamefront when "Host LAN game"
     * was chosen. Mirrors gameFrontIsLanOnly. The lobby UI uses this
     * (and isSinglePlayer) to hide WBN-derived badges and skip the
     * connectivity test. Server-side, the cfg flags are already
     * forced off in gameFrontSetupServer so this is presentation-
     * only on the client. */
    bool        isLanOnly;

    /* Brain state (per-instance, moved from static globals in client_sim.c) */
    uint32_t    brainHoldKeys;
    uint32_t    brainTapKeys;
    BuildInfo  *brainBuildInfo;
    PlayerBitMap brainsWantAllies;
    PlayerBitMap brainsMessageDest;
    char        brainsMessage[FILENAME_MAX];
    unsigned short brainsNumObjects;
    ObjectInfo  brainObjects[1024];
    aiType      allowComputerTanks;

    /* Brain event buffer — filled in clientSimSyncFromSnapshot, consumed in brainDataMakeInfo */
    GameEvent  brainEvents[MAX_BRAIN_EVENTS];
    int        brainEventCount;
    uint32_t   lastServerTick;
    bool       lastServerDestroyed; /* Previous snapshot's destroyed bit, for death/respawn edges */
    uint8_t    brainLastAssistMsg;  /* ASSIST_MSG_* or 0 */

    /* Smart pings this client has been sent and is still drawing. Written by
     * the EVENT_PING arm in client_snapshot.c, read by the GUI overlay
     * through clientSimGetPings. A plain array with a write cursor: entries
     * are expired by age at read time, so nothing has to tick it. */
    ClientPing  pings[MAX_CLIENT_PINGS];
    int         pingWriteIdx;

    /* Per-target smart-ping mute this client has set (bit N set = player N's
     * pings are muted). Session-scoped UI state mirroring what was sent to the
     * server via CMD_PLAYER_PING_MUTE; the server is the authority that drops
     * the pings, this only reflects/persists the toggle. Zeroed with the
     * ClientSim (clientSimAlloc calloc's it). */
    PlayerBitMap pingMutedByMe;

    /* Client render backstop against a flood of pings from one sender: a
     * per-sender ring of the wall-clock ms at which a ping from that sender
     * was last DRAWN, newest overwriting oldest. The EVENT_PING arm counts the
     * entries inside the 5s and 30s windows and drops the excess before drawing
     * a marker, playing a sound or adding a newswire line — the same caps the
     * server relay limiter uses (input_packet.h). 0 is an empty slot. */
    uint32_t     pingRenderMs[MAX_TANKS][PING_SPAM_MAX_30S];
    uint8_t      pingRenderIdx[MAX_TANKS];

    /* Per-instance fog-of-war brain map (was global sbm[256][256] in screenbrainmap.c) */
    BYTE        brainMap[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];

    /* Overview fog memory — the whole map seeded dimmed when it lands, then
     * what this player has seen live over it. Maintained every display tick
     * whether or not the overview is shown. Distinct from brainMap above:
     * that one is the brains' terrain fog and is revealed by map changes
     * anywhere; this one only ever rewrites what is inside a live region. */
    OverviewMap overview;
    /* A fresh map wants the memory seeded, but the install can run before
     * the local slot is settled (clientSimConnectLocal installs at step 5,
     * the slot funnel is step 7) and the seed draws pills, bases and mines
     * from the local player's point of view. The install arms this instead;
     * the next overview tick seeds with myPlayerNum settled. */
    bool overviewSeedPending;

    /* Per-instance message state (was messages.c globals) */
    MessageState messages;

    /* Per-instance scroll state (was scroll.c globals). */
    ScrollState scroll;

    /* Per-instance label state (was labels.c globals) */
    bool        labelOwnTank;
    labelLen    labelMessage;
    labelLen    labelTankLabel;

    /* Per-instance last player name (was players.c global) */
    char        myLastPlayerName[PLAYER_NAME_LEN];

    /* Client viewport / display state (was screen.c module-level statics).
     * The 10 camera/visible-tile fields are bundled into a ViewPort
     * substruct so viewport math can run against it directly; mapName,
     * gmeStartDelay, gmeLength, and timeStart remain on ClientSim because
     * they are round / map metadata rather than camera state. */
    ViewPort    viewport;
    char        mapName[MAP_STR_SIZE];
    int         gmeStartDelay;
    int32_t     gmeLength;
    time_t      timeStart;

    /* Network state (moved from network.c globals) */
    netType     networkGameType;
    netStatus   netStat;
    NetChatSendFunc         chatSendFunc;
    NetNameChangeSendFunc   nameChangeSendFunc;
    NetAllianceRequestFunc  allianceRequestFunc;
    NetAllianceAcceptFunc   allianceAcceptFunc;
    NetAllianceLeaveFunc    allianceLeaveFunc;
    NetLockToggleSendFunc   lockToggleSendFunc;

    /* Server address info (for brain info, replaces netClientGetServerAddress) */
    struct in_addr serverAddress;
    unsigned short serverPort;

    /* Reverse-DNS of the server address for the lobby / net-info UI only —
     * visual information, never used to connect. Filled asynchronously by the
     * DNS lookup thread (see netProcessedDnsLookup) and read by the GUI; both
     * sides guard access with the client mutex. serverHostName is "" until a
     * PTR record resolves; serverHostResultIp records which IP it belongs to
     * so a stale name is never shown against a changed address.
     * serverHostReqIp debounces the request: the IP a lookup was last queued
     * for, so the per-frame GUI doesn't re-queue every frame (and re-queues
     * after a reconnect that resets this struct). */
    char serverHostReqIp[64];
    char serverHostResultIp[64];
    char serverHostName[256];

    /* Lobby state (client-side mirror of server lobby) */
    ClientLobbySlot  lobbySlots[16];    /* MAX_TANKS */
    ClientSpectatorSlot spectatorSlots[MAX_SPECTATORS]; /* spectator roster mirror */
    int              countdownSeconds;  /* 0 = not counting down */
    bool             mapDownloadComplete; /* Gate for ready button */
    bool             inLobby;           /* TRUE if server is lobby-enabled */
    bool             lobbySyncSettled;  /* FALSE during the join sync replay;
                                         * set TRUE by CTRL_LOBBY_SYNC_COMPLETE.
                                         * Lobby event sounds play only when set,
                                         * so the roster replay burst is silent. */
    /* Who the server says is producing voice right now, one bit per slot
     * (CTRL_VOICE_TALKING). Lobby and countdown only — the server stops
     * sending it once a round starts, and sends one empty set on the way
     * out, so this is 0 in a running game. Raw: the local mute list is not
     * folded in here. */
    PlayerBitMap     voiceTalkingMap;
    char             lobbyChatHistory[4096]; /* Lobby chat buffer with player names */
    char             lobbyTeamChatHistory[4096]; /* Team-only lobby chat buffer */

    /* Lobby game settings (received from server in LOBBY_STATE packet) */
    gameType         lobbyGameType;
    bool             lobbyHiddenMines;
    uint8_t          lobbyAiType;       /* 0=none, 1=yes, 2=yesAdvantage, 3=full */
    int32_t          lobbyTimeLimit;    /* Game length in ticks (-1 = unlimited) */
    uint8_t          lobbyPillCount;
    uint8_t          lobbyBaseCount;
    uint8_t          lobbyStartCount;
    UploadPolicy     uploadPolicy;      /* server map-upload policy; ALLOW until first event */
    /* Server visibility rules, indexed by ViewCategory. Raw mirror of the
     * lobby-settings event; until one lands these hold the same three a
     * server starts with (clientSimCreate), so what the overview draws
     * before the settings arrive matches what the server is sending. */
    ViewPolicy       viewPolicy[VIEW_CATEGORY_COUNT];
    uint16_t         viewDecaySecs[VIEW_CATEGORY_COUNT];
    bool             classicMode;   /* server is running classic mode; raw
                                     * mirror of the lobby-settings event,
                                     * false until the first one lands */
    bool             alliesInTrees; /* server sends allies standing in trees
                                     * to their allies; raw mirror of the
                                     * lobby-settings event, false until the
                                     * first one lands */
    uint8_t          overviewWindow;  /* OverviewWindow the server asked for */
    uint8_t          lineOfSight;     /* LineOfSightMode the server asked for */
    ServerVoiceMode  serverVoiceMode; /* what the server does with the voice
                                       * its clients send it; raw mirror of
                                       * the lobby-settings event. Zero is
                                       * serverVoiceOn, so a client that has
                                       * not been told yet behaves as it did
                                       * before the server carried the mode */
    /* The client's own copy of the proximity clocks a viewPolicyDecay
     * category runs on, for the local player as the viewer. The server keeps
     * the same clocks and they are what decides which rects it sends; these
     * are display only — what the overview may draw a block round, and how
     * bright, with the data that has already arrived. Same convention as the
     * server's: 0 means the player has never been near that item, so a stamp
     * is never 0. Cleared with the rest of the per-round view state. */
    uint32_t         viewDecayTick;              /* display ticks this round */
    uint32_t         pillNearTick[MAX_PILLS];
    uint32_t         baseNearTick[MAX_BASES];
    uint32_t         allyNearTick[MAX_TANKS];
    /* The last map square each remote tank was seen on, and the display tick
     * that sighting landed. A tank outside our viewport arrives as a hidden
     * stub, which zeroes its players entry so the renderer stops drawing a
     * ghost — this is the separate record the ally view centres on instead of
     * the map origin, and the tick is how long the slot has been out of
     * sight. Both are 0 for a slot never seen this round, and both are
     * cleared with the rest of the per-round view state. */
    BYTE             allyLastMapX[MAX_TANKS];
    BYTE             allyLastMapY[MAX_TANKS];
    uint32_t         allySeenTick[MAX_TANKS];
    /* The ally the stale-view timer belongs to, and the display tick the
     * current run of watching it started. A tick of 0 means no run is being
     * timed. */
    BYTE             allyViewStubTarget;
    uint32_t         allyViewStubTick;
    bool             mapSkipAvailable;  /* Server has map rotation with >1 map */
    bool             lobbyAvailable;    /* Server runs a lobby (CTRL_LOBBY_SETTINGS
                                         * inLobby). False on -nolobby/-maprotate;
                                         * gates the in-game vote UI. */
    bool             mapSkipVotes[16];  /* Mirror of server vote state */
    bool             mapSkipMyVote;     /* Local tracking of own vote */

    /* In-game vote mirror (back-to-lobby + surrender). Indexed by
     * (kind - 1). See netpacks.h GAME_VOTE_KIND_*. */
    struct ClientGameVote {
        uint8_t  kind;
        uint8_t  active;            /* GAME_VOTE_ACTIVE_* */
        uint8_t  triggerSrc;        /* GAME_VOTE_TRIGGER_* */
        uint8_t  teamId;
        uint8_t  threshold;
        uint8_t  yesCount;
        uint8_t  noCount;
        uint8_t  eligibleCount;
        uint8_t  secondsRemaining;
        uint16_t votes;             /* bitmask of slots that voted yes */
        bool     widgetVisible;     /* local UI state — X closes, menu reopens */
        uint32_t concludedAtMs;     /* SDL_GetTicks() when active left RUNNING; 0 = still running / pre-start */
    } gameVotes[2];

    /* Tracks the seconds value most recently emitted for the
     * "Returning to lobby in N" newswire line. Used to dedupe so we
     * only push the line once per integer-second crossing of the
     * snapshot header's returnToLobbyTicks field. 0 means no
     * countdown active right now. */
    uint8_t  lastReturnToLobbySecs;

    /* Team balance proposal from WBN */
    uint8_t  balanceProposal[16];      /* Proposed team per slot (0 = none) */
    bool     balanceProposalActive;    /* TRUE if a proposal is being displayed */
    uint64_t lastBalanceProposalArrivedMs; /* SDL_GetTicks at the last non-empty
                                            * CTRL_BALANCE_PROPOSAL arrival —
                                            * latched by the dispatcher so the
                                            * lobby UI can show a short "Teams
                                            * balanced" success label even when
                                            * the auto-apply clear publish that
                                            * follows resets balanceProposalActive
                                            * before the next render frame. */
    uint64_t lastBalanceFailedMs;          /* SDL_GetTicks at the last
                                            * CTRL_BALANCE_FAILED arrival.
                                            * Lobby UI compares against the
                                            * popup-confirm timestamp to flip
                                            * "Asking WBN…" to a failure pill
                                            * immediately on server error,
                                            * bypassing the 8 s NOREPLY
                                            * fallback. */
    uint8_t  lastBalanceFailedReason;      /* Mirrors CTRL_BALANCE_FAILED.reasonCode. */

    /* ── Layout A lobby state (client-side mirror of server) ──────
     * Populated from PACKET_LOBBY_*_CHG broadcasts. The team metadata
     * table starts zeroed (in_use=0 → "Team N" defaults); each
     * TEAM_META_CHG broadcast updates one entry. Bot configs same
     * shape, indexed by slot. */
    uint8_t  lobbyTeamInUse[16];
    uint8_t  lobbyTeamColor[16];
    uint8_t  lobbyTeamPool[16];
    uint8_t  lobbyTeamStartSide[16];    /* START_SIDE_* (start_sides.h) */
    char     lobbyTeamName[16][32];     /* LOBBY_TEAM_NAME_LEN */

    /* Per-bot mode index into the brain's own mode list (brain_list.h);
     * 0 is the default mode every ordinary game uses. lobbyBotDifficulty
     * is an index into THAT mode's level list. */
    uint8_t  lobbyBotMode[16];
    uint8_t  lobbyBotDifficulty[16];
    uint8_t  lobbyBotPersonality[16];

    /* Per-bot brain catalogue index. 0xFF means the slot is using the
     * server's CLI-configured default brain. Otherwise indexes into
     * lobbyBrainList.entries[] to recover the wire path / display name.
     * Initialised to 0xFF in clientSimCreate (memset(0) would alias
     * a valid catalogue index). */
    uint8_t  lobbyBotBrainIdx[16];

    /* Brain codebases the server has on disk — populated from
     * PACKET_LOBBY_BRAIN_LIST on join. Used as the option list for the
     * AiConfig "Bot Code" combo. */
    BrainList lobbyBrainList;

    /* Last finished round's scoreboard + awards, received via
     * CTRL_ROUND_STATS at game over. Round-only: cleared when the next
     * countdown starts. lastRoundStatsValid gates whether the panel shows. */
    RoundStatsSummary lastRoundStats;
    bool              lastRoundStatsValid;

    /* Bumped on every CTRL_ROUND_RATING_POSTED that names the round
     * lastRoundStats describes and came from another player. The recap's
     * WinBolo.net block watches it for movement and re-reads the round's
     * ratings and comments; the value itself carries no meaning. */
    uint32_t          ratingPostedSeq;

    /* Reassembly of the server's bot-pool catalog, streamed as
     * CTRL_LOBBY_BOT_POOL_CHUNK fragments during join sync. Fragments
     * arrive in order on the reliable control channel; on the final
     * fragment the assembled blob is installed via
     * lobbyBotPoolsDeserializeInstall (replacing the process-global pool
     * table so the lobby dropdown shows the SERVER's pools). */
    uint8_t  lobbyPoolChunkExpected;   /* total fragment count; 0 = idle */
    uint8_t  lobbyPoolNextSeq;         /* next in-order fragment expected */
    uint32_t lobbyPoolBlobLen;         /* bytes assembled so far */
    uint8_t  lobbyPoolBlob[LOBBY_BOT_CATALOG_WIRE_MAX];

    /* Server-side map directory listing — populated from
     * PACKET_LOBBY_MAP_LIST_RSP. The chooser's listProvider sends a
     * PACKET_LOBBY_MAP_LIST_REQ each time the user navigates into a
     * folder; the latest response lives here. lobbyMapListPath echoes
     * the requested path so the client can ignore stale responses
     * if it has navigated away. */
/* Per-directory ceiling on the Server Maps + Map Search caches.
 * Matches MAP_CHOOSER_MAX_MAPS (imgui_mapchooser.h:38) so a
 * directory's content is visible end-to-end. The wire side already
 * supports arbitrary counts via PACKET_LOBBY_MAP_LIST_RSP chunking;
 * this cap is the in-memory ClientSim ceiling. Per-ClientSim cost
 * at 512: ~67 KB for the listing arrays, same again for the search
 * arrays — ~130 KB total per ClientSim. */
#define LOBBY_MAP_LIST_MAX 512
#define LOBBY_MAP_LIST_NAME_LEN 128
    char     lobbyMapListPath[256];
    int      lobbyMapListCount;
    char     lobbyMapListNames[LOBBY_MAP_LIST_MAX][LOBBY_MAP_LIST_NAME_LEN];
    uint8_t  lobbyMapListIsFolder[LOBBY_MAP_LIST_MAX];
    int64_t  lobbyMapListModTime[LOBBY_MAP_LIST_MAX];
    /* The map has a script beside it on the server. False for a folder, and
       false throughout from a server that runs no scenario library. */
    bool     lobbyMapListScripted[LOBBY_MAP_LIST_MAX];
    bool     lobbyMapListReady;     /* true once a response arrives */
    /* Most-recently requested path (set when the client sends
     * MAP_LIST_REQ; cleared when the matching response arrives). The
     * lobby's listProvider compares this against the current relPath
     * to decide whether to re-issue a request on path change. */
    char     lobbyMapListReqPath[256];
    bool     lobbyMapListInFlight; /* true after send, false on response */

    /* Monotonic counter incremented whenever the client receives a
     * PACKET_LOBBY_MAP_CHANGE (i.e. the server told us to invalidate
     * and re-download the map). UI poll-and-compare against a cached
     * last-seen value to detect map changes even when the download
     * + completion happen inside a single render frame (MP-host
     * loopback case — the `mapDownloadComplete = false; ...; true`
     * transition never lives long enough for a frame-boundary edge
     * detector to catch). */
    uint32_t lobbyMapChangeSeq;

    /* Recursive search results — populated by PACKET_LOBBY_MAP_SEARCH_RSP.
     * Same shape as the list cache but the names are RELATIVE PATHS
     * from lobbyMapSearchPath (so a file in a subfolder reads as
     * "Subdir/Foo.map" rather than just "Foo.map"). The query and
     * path together form the cache key — a list listing for path X
     * and a search for query="foo" in path X are different
     * responses, stored side-by-side. */
    char     lobbyMapSearchPath[256];
    char     lobbyMapSearchQuery[128];
    int      lobbyMapSearchCount;
    char     lobbyMapSearchNames[LOBBY_MAP_LIST_MAX][LOBBY_MAP_LIST_NAME_LEN];
    uint8_t  lobbyMapSearchIsFolder[LOBBY_MAP_LIST_MAX];
    int64_t  lobbyMapSearchModTime[LOBBY_MAP_LIST_MAX];
    bool     lobbyMapSearchReady;
    char     lobbyMapSearchReqPath[256];
    char     lobbyMapSearchReqQuery[128];
    bool     lobbyMapSearchInFlight;

    /* Server-map preview byte stream — driven by the Server Maps tab
     * in MP. The chooser asks for a map's raw .map bytes via
     * PACKET_LOBBY_MAP_PREVIEW_REQ; the server streams them back over
     * CHANNEL_BULK behind a bulk-transfer stream header (or _ERR on failure).
     * The bytes accumulate here and are rasterised on the GUI thread
     * once complete, the same way the WBN tab handles its async
     * download. lobbyMapPreviewReqPath is the path we asked for;
     * lobbyMapPreviewPath echoes the path the completed bytes belong
     * to so the GUI can ignore a stale response after navigating. */
    char     lobbyMapPreviewReqPath[256];
    char     lobbyMapPreviewPath[256];
    bool     lobbyMapPreviewInFlight;
    bool     lobbyMapPreviewReady;   /* full byte stream received */
    bool     lobbyMapPreviewError;   /* server replied _ERR */
    uint32_t lobbyMapPreviewTotal;   /* expected total bytes from the stream header */
    uint32_t lobbyMapPreviewReceived;/* bytes accumulated so far */
    uint8_t  lobbyMapPreviewBytes[LOBBY_MAP_UPLOAD_MAX_BYTES];

    /* Upload progress — driven by the Upload tab and the
     * PACKET_LOBBY_MAP_UPLOAD_ACK/DONE handlers. status: 0=idle,
     * 1=announce-sent, 2=ack-received-sending-chunks, 3=done,
     * 4=rejected. */
    uint8_t  lobbyMapUploadStatus;
    uint8_t  lobbyMapUploadRejectCode; /* server's reject byte, if any */
    char     lobbyMapUploadFinalPath[256]; /* server-relative path */
    /* Set by the PACKET_LOBBY_MAP_USE_LOCAL_NACK handler when the
     * server can't fulfil the MD5-skip-upload shortcut. The Upload
     * tab's pump loop notices this on the next frame and falls back
     * to the regular PACKET_LOBBY_MAP_UPLOAD_BEGIN / CHUNK flow. */
    bool     lobbyMapUseLocalNeedsFallback;

    /* Winbolo.net preview result — driven by
     * PACKET_LOBBY_PREVIEW_WBN_DONE. status: 0=idle,
     * 1=in-flight, 2=ok, 3=error. errMsg is the server-supplied
     * message when status == 3. */
    uint8_t  lobbyWbnPreviewStatus;
    char     lobbyWbnPreviewErrMsg[128];
    char     lobbyWbnPreviewFinalPath[256];

    bool     lobbyOpenHost;
    BYTE     lobbyHostSlot;
    bool     lobbyHostSlotKnown;  /* false until the first lobby-settings
                                   * snapshot of a lobby session lands, so
                                   * the initial host assignment is not
                                   * announced as a change. Reset whenever
                                   * lobbyChatHistory is cleared. */
    bool     lobbyAutoLockOnGameStart;
    bool     lobbyRanked;  /* server flagged this as a ranked game:
                            * bots are forbidden, game type "Open" is
                            * forbidden. Mirrored on every client so
                            * they can render the ranked badge / lock
                            * affordance. Toggle is host/admin only. */
    bool     lobbyAllowNewPlayers; /* server's allowNewPlayers state —
                                    * mirrors sim->allowNewPlayers via
                                    * CTRL_LOBBY_SETTINGS so the host's
                                    * "Now" checkbox reflects the real
                                    * server state instead of a stale
                                    * local intent. */
    bool     lobbyWbnAvailable;    /* server's winbolonetIsRunning() —
                                    * mirrored to remote clients so the
                                    * lobby can hide WBN-mediated UI
                                    * (Balance from WBN) when the host
                                    * process isn't signed in to WBN. */
    uint32_t lobbyServerLocks;

    /* The scenario the lobby's map is running, mirrored from
     * CTRL_LOBBY_SETTINGS. lobbyScenarioSource is a LobbyScenarioSource
     * value stored raw; 0 is "no scenario" and the three strings are then
     * empty. A settings body from a lobby with no scenario carries none of
     * these bytes at all, and the decoder's memset leaves exactly that, so
     * a plain lobby needs no special case here. */
    uint8_t  lobbyScenarioSource;
    char     lobbyScenarioName[LOBBY_SCENARIO_NAME_LEN];
    char     lobbyScenarioFileName[LOBBY_SCENARIO_FILE_LEN];
    char     lobbyScenarioDescription[LOBBY_SCENARIO_DESC_LEN];
    bool     lobbyScenarioExtraTeams;

    /* Most recent server reject — surfaced via toast/log when set.
     * lobbyLastRejectPacket is set to 0 when no pending message. */
    uint8_t  lobbyLastRejectPacket;
    uint8_t  lobbyLastRejectReason;

    /* Steam achievement: first capture tracking (per-game) */
    bool     hasAnyBaseCaptured;
    bool     hasAnyPillCaptured;

    /* Steam achievement: per-game death/loss counters (zeroed by memset in clientSimCreate) */
    uint16_t myDeathsThisGame;
    uint16_t myLgmLossesThisGame;

    /* Live per-slot scoreboard, counted from the reliable game-event
     * stream for every slot (not just ours). Zeroed at CTRL_GAME_PHASE_RUNNING. */
    ClientPlayerStats liveStats[MAX_TANKS];

    /* Steam achievement: player count tracking (ACH_PLAYERS_6/8/16) */
    uint8_t  maxPlayersSeenThisGame;

    /* Steam achievement: lonely lobby tracking (ACH_LONELY_LOBBY) */
    uint32_t lobbyAloneStartTick;

    /* Steam achievement: rapid death tracking (ACH_RAPID_DEATH) */
    uint32_t deathTimestamps[10];
    uint8_t  deathTimestampIdx;

    /* Transport binding — owned by ClientSim and managed via the
     * client_net.h API (clientSimConnectUdp / clientSimConnectLocal /
     * clientSimDisconnect). Embedded by value because the underlying
     * transport constructors return Transport by value.
     *
     * clientSimCreate preserves these four fields across its memset
     * (save/restore in the function body). */
    Transport transport;
    bool      hasTransport;
    bool      isUdpTransport;
    /* True when the bound transport advances the server itself — i.e. its
     * tick() runs serverSimTick (the active local transport). False for the
     * passive local transport (host timer thread ticks the server) and for
     * UDP (the remote server ticks itself). Frontend tick loops gate their
     * extra keys-half transport pump on this via
     * clientSimTransportTicksServer. */
    bool      transportTicksServer;

    /* Spectator feed — populated by the UDP transport's bulk sink while
     * connected as a tankless spectator; drained by the spectator session.
     * Zeroed at create, freed on teardown (clientSimSpectatorFeedClear). */
    ClientSpectatorFeed spectatorFeed;

    /* True when this ClientSim connected as a tankless spectator (the
     * spectator arg of clientSimConnectUdp). A spectator claims no tank
     * slot, so myPlayerNum stays at its create-time 0 — an in-bounds value
     * that aliases real player slot 0. Control handlers that branch on "is
     * this my slot / from me" consult this flag to treat a spectator as
     * having no self, so slot 0 is never mistaken for the viewer. */
    bool isSpectator;

    /* In-process server bound by clientSimConnectLocal{,Passive}. NULL
     * for UDP and disconnected clients. Read by the local-transport
     * branch of CTRL_LOBBY_MAP_CHANGE to fetch the freshly-compressed
     * map without going through the wire MAP_DOWNLOAD machinery.
     * Preserved across clientSimCreate's memset alongside the
     * transport fields (connection-lifetime state). */
    struct ServerSim *boundServerSim;

    /* Unified rejection-reason buffer. Local connect failures write
     * directly here; UDP JOIN_REJECT mirrors its reason here too. The
     * clientSimGetConnectErrorReason accessor reads from this field. */
    char      connectErrorReason[256];

    SubscriberHandle autoSubHandle;    /* Returned by serverSimRegisterClientSubscriber
                                        * inside clientSimConnectLocal{,Passive}; cleared
                                        * to SUBSCRIBER_HANDLE_INVALID at create-time and
                                        * after clientSimDisconnect unregisters it. */

    /* Read-only ControlEvent observer (test-only — see
     * clientSimSetControlObserver in client_sim.h). Preserved across
     * clientSimCreate's memset alongside the transport fields. */
    ControlObserverCb controlObserverCb;
    void             *controlObserverCtx;

    /* Transport-internal observer. Wired by the UDP transport to react
     * to events whose side effects live below the bolo-lib boundary
     * (joinState transitions, re-join trigger, WBN re-auth, etc.).
     * Separate slot from controlObserverCb so tests don't displace it. */
    ControlObserverCb transportObserverCb;
    void             *transportObserverCtx;

    /* Last view state reported to the server with CMD_VIEW_STATE.
     * clientSimSyncViewState compares the viewport against these each
     * display tick and only sends when they differ. Both start at 0xFF —
     * no kind or target has that value, so the first tick of a session (and
     * of every round, which resets them) always reports. */
    uint8_t lastSentViewKind;
    uint8_t lastSentViewTarget;

    /* Pending alliance request from another player. 0xFF when none.
     * Set by the CTRL_ALLIANCE_REQUEST arm when toPlayer == myPlayerNum;
     * frontends poll via clientSimGetPendingAllianceRequest and clear
     * with clientSimClearPendingAllianceRequest after popping the
     * dialog (or auto-rejecting). */
    BYTE pendingAllianceRequestFrom;
};

BOLO_STATIC_ASSERT(offsetof(struct ClientSim, sim) == 0,
                   ClientSim_sim_must_be_first_member);

/* Internal accessors for the connect-path fields. The local-transport
 * branch of CTRL_LOBBY_MAP_CHANGE reads the bound server through
 * clientSimGetBoundServerSim; clientSimConnectLocal{,Passive} record
 * the binding via clientSimSetBoundServerSim. clientSimSetConnectErrorReason
 * is the single writer used by both connect paths and (Phase 2) the UDP
 * JOIN_REJECT handler. */
void                    clientSimSetBoundServerSim(ClientSim *cs, struct ServerSim *sim);
struct ServerSim       *clientSimGetBoundServerSim(const ClientSim *cs);
void                    clientSimSetConnectErrorReason(ClientSim *cs, const char *str);

/* One bit per player slot: the remote tanks this client currently believes
 * are alive. The client holds no tanks[] object for anyone but itself, so a
 * remote tank's armour is only ever seen through the interpolation context —
 * this turns that into the "is it alive" input the ally-view helpers in
 * players.c take. The local slot is never set. */
PlayerBitMap            clientSimAllyViewMask(const ClientSim *cs);

/* ── Decay view clocks ───────────────────────────────────────────────
 * The clocks above run on the display tick, which is the 20ms game tick, so a
 * decay window in seconds converts at GAME_NUMGAMETICKS_SEC. The server's own
 * clocks run on its 10ms tick and convert at GAME_NUMTOTALTICKS_SEC; both come
 * out as the same stretch of real time. */
#define CLIENT_VIEW_DECAY_TICKS_SEC GAME_NUMGAMETICKS_SEC

/* Advances the clock and re-stamps every item the local tank is within
 * VIEW_DECAY_NEAR_TILES of, for the categories on viewPolicyDecay. Proximity
 * and nothing else: alliance, armour and the rest are the region build's
 * business, and an item can change hands long after the player drove past it.
 * A tank slot whose players entry has been zeroed by a hidden stub has no
 * position to measure from and is skipped, not read as the map origin.
 * Runs once per display tick, before the overview reads the clocks. */
void                    clientSimViewDecayTick(ClientSim *cs);

/* Clears the clocks back to "never been near anything", along with the
 * last-seen record the ally view leans on. Called wherever the round's view
 * state is reset. */
void                    clientSimResetViewDecay(ClientSim *cs);

/* TRUE when the camera is parked on an item whose category is on
 * viewPolicyDecay and whose clock has run out. The server has already stopped
 * sending that item's squares, so the view has nothing left to show. */
bool                    clientSimViewDecayExpired(const ClientSim *cs);

/* One bit per item of a category: that item may be picked while cycling. All
 * of them under viewPolicyAlways, viewPolicyKey and viewPolicyOff, so those
 * three cycle exactly as they always have; under viewPolicyDecay the items
 * whose clocks have run out drop out, because the server has stopped sending
 * their squares and parking on one would show nothing. This is the one place
 * the per-item decay question is asked for cycling. */
PlayerBitMap            clientSimViewEligibleMask(const ClientSim *cs,
                                                  ViewCategory cat);

/* Fills the masks and remembered squares the item-view cycling takes. The
 * split is the same one OverviewViewInputs uses: ClientSim knows the rules
 * and the clocks, viewport.c knows what to do with them, and neither has to
 * know the other's layout. */
void                    clientSimFillViewCycleInputs(const ClientSim *cs,
                                                     ViewCycleInputs *in);

/* How long the ally view sits on a target it has stopped receiving before it
 * gives up on it. Under viewPolicyKey the server only starts sending an
 * ally's rect once the claim arrives, so entering the view always costs about
 * a round trip of stale ground; two seconds is well clear of that and short
 * enough that an ally who died while out of sight does not strand the camera.
 * The client is never told that directly — while a claim is valid the server
 * grants the rect, so a run of stubs this long means the rect is gone. */
#define CLIENT_VIEW_ALLY_STUB_GRACE_TICKS (2 * CLIENT_VIEW_DECAY_TICKS_SEC)

/* TRUE when the ally view has gone that long without a real update for the
 * tank it is watching. Advances the timer, so it belongs on the per-tick
 * upkeep path and nowhere else. */
bool                    clientSimAllyViewStubExpired(ClientSim *cs);

/* TRUE while an ally view is still waiting on the first real record for the
 * ally it is watching. Between applying the server's CTRL_VIEW_TARGET answer
 * and that ally's first snapshot the client holds no data for them at all, so
 * the ordinary "is this still watchable" test cannot pass yet and the per-tick
 * upkeep has to leave the view alone. FALSE once anything has arrived for that
 * slot this round, and in every view but the ally one. */
bool                    clientSimAllyViewAwaitingFirstData(const ClientSim *cs);

/* Park the camera on the ally the server named in its CTRL_VIEW_TARGET
 * answer. The entry point in viewport.c that does the work is private to that
 * file, so the control dispatcher comes through here rather than reaching
 * into the viewport itself. */
void                    clientSimApplyAllyViewTarget(ClientSim *cs, BYTE target,
                                                     BYTE mapX, BYTE mapY);

/* Fills the view rules, clocks and camera state the overview's region build
 * takes. Declared here rather than reached through overview_map.h so this
 * header stays clear of the overview module. */
struct OverviewViewInputs;
void                    clientSimFillOverviewViewInputs(const ClientSim *cs,
                                                        struct OverviewViewInputs *in);

/* ── Render-only error smoothing ─────────────────────────────────────
 * Reconciliation corrections are deposited into the per-axis render
 * offset (errX/errY/errAngle) and decayed to zero so they slide instead
 * of snapping. The offset lives purely in the draw path; the sim never
 * reads it. */
#define CLIENT_ERR_POS_CLAMP    256.0f  /* one map square, world units, per axis */
#define CLIENT_ERR_ANGLE_CLAMP   16.0f  /* bradians */
#define CLIENT_ERR_DECAY_TAU_MS  40.0f  /* err *= exp(-dt/tau); gone in ~120ms */

/* Enlarged position clamp (4 map squares) used only while a base next to the
 * tank has just become drivable, so the catch-up correction glides instead of
 * snapping. At 800 u/s max road speed this covers RTT up to ~1.28s; genuine
 * teleports past 4 tiles still snap. */
#define CLIENT_ERR_POS_CLAMP_BASE_UNBLOCK 1024.0f
/* Number of subsequent local-tank snapshots over which base-unblock
 * corrections use the enlarged clamp. */
#define CLIENT_BASE_UNBLOCK_SMOOTH_SNAPSHOTS 3

/* err += delta; if |result| (per-axis position against posClamp, separately
 * the angle against CLIENT_ERR_ANGLE_CLAMP, with the angle wrapped into
 * [-128,128)) exceeds its clamp, zero ALL components — a genuine teleport
 * should snap, not slide. Returns true if the offset was zeroed. */
bool clientErrSmoothAccumulate(float *errX, float *errY, float *errAngle,
                               float dX, float dY, float dAngle,
                               float posClamp);

/* Exponential decay toward zero: err *= expf(-dtMs / CLIENT_ERR_DECAY_TAU_MS). */
void clientErrSmoothDecay(float *errX, float *errY, float *errAngle, float dtMs);

/* Rendered (not simulated) local-tank pose: world position and angle
 * with the render-only error offset applied. RENDER CODE ONLY — using
 * this for prediction, brains, or collision reintroduces the
 * asymmetric-state bug the offset exists to avoid. */
void clientSimGetRenderedTankPos(ClientSim *cs, WORLD *x, WORLD *y, float *angle);

/* Build the visible shell list: other players' shells from the projected
 * layer (or, for a bot, the server snapshots its brain perceives) plus the
 * local player's own predictions. Squares outside [left,rightExcl) x
 * [top,bottomExcl) are skipped, and originX/originY are subtracted from the
 * square written out — pass the viewport and its scroll offset for the
 * classic 17x17 list, or the whole map and 0,0 for absolute squares. */
void clientSimBuildShellList(ClientSim *cs, screenBullets *sb,
                             int left, int rightExcl,
                             int top, int bottomExcl,
                             int originX, int originY);

/* Decompress `buf`/`len` into the ClientSim's map/pills/bases/starts,
 * stash the map name, and prime the mine-visibility/render state.
 * Does NOT call clientSimCreate; the ClientSim must already be alive
 * and initialised. The caller is responsible for clientSimSetupSelf
 * and for the snapshot apply that follows.
 *
 * initViewport selects whether to (re)initialise the viewport: true on a
 * first map load (allocates the view buffers and parks the camera at the
 * map origin); false on a mid-game resync (keeps the already-allocated
 * view buffers and the player's current camera offsets while still
 * recalculating and redrawing the swapped-in terrain). Returns false if
 * the blob fails to decode, leaving the prior map in place. */
bool installCompressedMap(ClientSim *cs, const BYTE *buf, int len, const char *name,
                          bool initViewport);

/* Spectator feed capture — called by the UDP transport's bulk sink (T2; the
 * transport already reaches ClientSim internals). The session-facing drain
 * accessors are the public clientSimSpectator* functions in client_sim.h. */

/* Store the spectator seed blob, taking ownership of `blob` (freed by the feed
 * on drain/teardown). Replaces and frees any prior seed. Marks the seed ready. */
void clientSimSpectatorPushSeed(ClientSim *cs, uint8_t *blob, uint32_t len);

/* Append one forward record to the feed's FIFO, copying `payloadLen` bytes from
 * `payload` (payload may be NULL when payloadLen is 0). Returns false (and adds
 * nothing) on allocation failure. */
bool clientSimSpectatorPushRecord(ClientSim *cs, bool isKeyframe,
                                  uint32_t gameTick, uint32_t segment,
                                  const uint8_t *payload, uint32_t payloadLen);

/* Record the latest cold-start countdown (remaining game ticks) the server
 * sent on CHANNEL_CONTROL. Marks the countdown received so the session can
 * show its pre-seed overlay. */
void clientSimSpectatorSetCountdown(ClientSim *cs, uint32_t remainingTicks);

/* Mirror the transport's spectator live-lobby mode onto the sim (one-way: the
 * transport owns the bit). True = fed the live lobby control bus; false = on
 * the delayed ring feed. The session reads it via clientSimSpectatorIsLiveLobby
 * (public) to decide which view to host. */
void clientSimSpectatorSetLiveLobby(ClientSim *cs, bool liveLobby);

/* Free the seed and every queued record, returning the feed to empty. */
void clientSimSpectatorFeedClear(ClientSim *cs);

/* Drop a slot's ping smoothing state so an arriving player doesn't inherit
 * the previous occupant's average. T2: called by players.c when the slot
 * empties, never by a frontend — the readout is something frontends only
 * read (clientSimGetPlayerPing / clientSimGetPlayerPingBand in client_sim.h),
 * so the reset has no business on the public API. */
void clientSimResetPlayerDisplayPing(ClientSim *cs, BYTE playerNum);

/* Record a ping this client should draw. T2: the EVENT_PING arm of the
 * snapshot ingest is the only caller, and it must stay that way — a ping is
 * something the server decided this client may see, so a frontend that could
 * call this could paint a marker nobody sent. A local echo needs no separate
 * path either, because the server sends the sender its own ping back like
 * everyone else's. Frontends only read the ring, through clientSimGetPings
 * (client_sim.h). */
void clientSimAddPing(ClientSim *cs, uint8_t sender, uint8_t kind,
                      uint16_t worldX, uint16_t worldY, uint32_t nowMs);

#endif /* CLIENT_SIM_INTERNAL_H */
