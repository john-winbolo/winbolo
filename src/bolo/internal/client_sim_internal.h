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
 * src/bolo/client_net.c.
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
#include "upload_policy.h"

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
    UploadPolicy     uploadPolicy;      /* server map-upload policy; ALLOW until first event */
    bool             mapSkipAvailable;  /* Server has map rotation with >1 map */
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
     * (save/restore in the function body). */
    Transport transport;
    bool      hasTransport;
    bool      isUdpTransport;

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

/* Decompress `buf`/`len` into the ClientSim's map/pills/bases/starts,
 * stash the map name, and prime the viewport + mine-visibility state.
 * Does NOT call clientSimCreate; the ClientSim must already be alive
 * and initialised. The caller is responsible for clientSimSetupSelf
 * and for the snapshot apply that follows. */
bool installCompressedMap(ClientSim *cs, const BYTE *buf, int len, const char *name);

#endif /* CLIENT_SIM_INTERNAL_H */
