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
#include "viewport.h"

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

BOLO_STATIC_ASSERT(offsetof(struct ClientSim, sim) == 0,
                   ClientSim_sim_must_be_first_member);

#endif /* CLIENT_SIM_INTERNAL_H */
