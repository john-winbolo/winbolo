/*********************************************************
 * client_sim_internal.h
 *
 * Defines the full ClientSim struct layout. The public
 * header (client_sim.h) exposes only a forward declaration;
 * files that need direct field access must include this
 * header instead and use the implementation-internal view.
 *
 * Allowed includers: src/bolo/client_sim.c,
 * src/bolo/client_sim_control.c, src/bolo/client_snapshot.c,
 * src/bolo/transport_udp_client.c, src/bolo/viewport.c,
 * src/bolo/client_mapload.c.
 * All other callers must include client_sim.h and use the
 * public accessor API.
 *********************************************************/
#ifndef CLIENT_SIM_INTERNAL_H
#define CLIENT_SIM_INTERNAL_H

#include <stddef.h>
#include "client_sim.h"
#include "game_sim.h"
#include "viewport.h"
#include "transport.h"
#include "client_state.h"
#include "interpolation.h"
#include "messages.h"
#include "scroll.h"
#include "brain_list.h"

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
    bool        inPillView;
    BYTE        pillViewX;
    BYTE        pillViewY;
    int         cursorPosX;
    int         cursorPosY;
    bool        needRecalc;
};

struct ClientSim {
    GameSim     sim;    /* MUST be first member */
    BYTE        myPlayerNum; /* Server-assigned player number; tanks[myPlayerNum] is our tank */

    /* Client-side prediction state */
    ClientState clientState;

    /* Interpolation state for other players' tanks */
    InterpContext interpCtx;

    /* Server shell snapshots for UDP mode */
    ShellSnapshot serverShellSnaps[MAX_SNAPSHOT_SHELLS];
    int         serverShellCount;

    /* Client-side predicted shells (Phase 4 of client-prediction) */
    PredictedShell predictedShells[MAX_PREDICTED_SHELLS];
    int         predictedShellCount;

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
    uint8_t    lastServerArmour;    /* Previous server snapshot armour for death detection */
    uint8_t    brainLastAssistMsg;  /* ASSIST_MSG_* or 0 */

    /* Per-instance fog-of-war brain map (was global sbm[256][256] in screenbrainmap.c) */
    BYTE        brainMap[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE];

    /* Per-instance message state (was messages.c globals) */
    MessageState messages;

    /* Per-instance scroll state (was scroll.c globals) */
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

    /* Lobby state (client-side mirror of server lobby) */
    ClientLobbySlot  lobbySlots[16];    /* MAX_TANKS */
    int              countdownSeconds;  /* 0 = not counting down */
    bool             mapDownloadComplete; /* Gate for ready button */
    bool             inLobby;           /* TRUE if server is lobby-enabled */
    char             lobbyChatHistory[4096]; /* Lobby chat buffer with player names */

    /* Lobby game settings (received from server in LOBBY_STATE packet) */
    gameType         lobbyGameType;
    bool             lobbyHiddenMines;
    uint8_t          lobbyAiType;       /* 0=none, 1=yes, 2=yesAdvantage, 3=full */
    int32_t          lobbyTimeLimit;    /* Game length in ticks (-1 = unlimited) */
    uint8_t          lobbyPillCount;
    uint8_t          lobbyBaseCount;
    uint8_t          lobbyStartCount;
    bool             mapSkipAvailable;  /* Server has map rotation with >1 map */
    bool             mapSkipVotes[16];  /* Mirror of server vote state */
    bool             mapSkipMyVote;     /* Local tracking of own vote */

    /* Team balance proposal from WBN */
    uint8_t  balanceProposal[16];      /* Proposed team per slot (0 = none) */
    bool     balanceProposalActive;    /* TRUE if a proposal is being displayed */

    /* ── Layout A lobby state (client-side mirror of server) ──────
     * Populated from PACKET_LOBBY_*_CHG broadcasts. The team metadata
     * table starts zeroed (in_use=0 → "Team N" defaults); each
     * TEAM_META_CHG broadcast updates one entry. Bot configs same
     * shape, indexed by slot. */
    uint8_t  lobbyTeamInUse[16];
    uint8_t  lobbyTeamColor[16];
    uint8_t  lobbyTeamPool[16];
    char     lobbyTeamName[16][32];     /* LOBBY_TEAM_NAME_LEN */

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

    /* Server-side map directory listing — populated from
     * PACKET_LOBBY_MAP_LIST_RSP. The chooser's listProvider sends a
     * PACKET_LOBBY_MAP_LIST_REQ each time the user navigates into a
     * folder; the latest response lives here. lobbyMapListPath echoes
     * the requested path so the client can ignore stale responses
     * if it has navigated away. */
#define LOBBY_MAP_LIST_MAX 64
#define LOBBY_MAP_LIST_NAME_LEN 128
    char     lobbyMapListPath[256];
    int      lobbyMapListCount;
    char     lobbyMapListNames[LOBBY_MAP_LIST_MAX][LOBBY_MAP_LIST_NAME_LEN];
    uint8_t  lobbyMapListIsFolder[LOBBY_MAP_LIST_MAX];
    int64_t  lobbyMapListModTime[LOBBY_MAP_LIST_MAX];
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

    /* Upload progress — driven by the Upload tab and the
     * PACKET_LOBBY_MAP_UPLOAD_ACK/DONE handlers. status: 0=idle,
     * 1=announce-sent, 2=ack-received-sending-chunks, 3=done,
     * 4=rejected. */
    uint8_t  lobbyMapUploadStatus;
    uint8_t  lobbyMapUploadRejectCode; /* server's reject byte, if any */
    char     lobbyMapUploadFinalPath[256]; /* server-relative path */

    /* Winbolo.net preview result — driven by
     * PACKET_LOBBY_PREVIEW_WBN_DONE. status: 0=idle,
     * 1=in-flight, 2=ok, 3=error. errMsg is the server-supplied
     * message when status == 3. */
    uint8_t  lobbyWbnPreviewStatus;
    char     lobbyWbnPreviewErrMsg[128];
    char     lobbyWbnPreviewFinalPath[256];

    bool     lobbyOpenHost;
    bool     lobbyAutoLockOnGameStart;
    uint16_t lobbyServerLocks;

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
     * clientSimCreate preserves these three fields across its memset
     * (save/restore in the function body) so that clientSimResetForMapLoad
     * can keep the connection alive while wiping map-dependent state. */
    Transport transport;
    bool      hasTransport;
    bool      isUdpTransport;

    /* Read-only ControlEvent observer (test-only — see
     * clientSimSetControlObserver in client_sim.h). Preserved across
     * clientSimCreate's memset alongside the transport fields. */
    ControlObserverCb controlObserverCb;
    void             *controlObserverCtx;
};

BOLO_STATIC_ASSERT(offsetof(struct ClientSim, sim) == 0,
                   ClientSim_sim_must_be_first_member);

#endif /* CLIENT_SIM_INTERNAL_H */
