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
 * src/bolo/transport_udp_client.c. All other callers must include
 * client_sim.h and use the public accessor API.
 *********************************************************/
#ifndef CLIENT_SIM_INTERNAL_H
#define CLIENT_SIM_INTERNAL_H

#include "client_sim.h"

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

    /* Brain event buffer — filled in clientSimSyncFromSnapshot, consumed in screenMakeBrainInfoCS */
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

    /* Client viewport / display state (was screen.c module-level statics) */
    screen      view;
    screenMines mineView;
    BYTE        xOffset;
    BYTE        yOffset;
    bool        inPillView;
    BYTE        pillViewX;
    BYTE        pillViewY;
    int         cursorPosX;
    int         cursorPosY;
    char        mapName[MAP_STR_SIZE];
    int         gmeStartDelay;
    int32_t     gmeLength;
    time_t      timeStart;
    bool        needScreenReCalc;

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

    /* Per-bot brain path (full wire path like "Brains/NewAutopilot/init.lua").
     * Empty when the slot is using whatever the server's global botBrainPath
     * was at bot-create time. Drives the "Bot Code" combo in the lobby's
     * AiConfig form. */
    char     lobbyBotBrain[16][256];

    /* Brain codebases the server has on disk — populated from
     * PACKET_LOBBY_BRAIN_LIST on join. Used as the option list for the
     * AiConfig "Bot Code" combo. */
    BrainList lobbyBrainList;

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
};

#endif /* CLIENT_SIM_INTERNAL_H */
