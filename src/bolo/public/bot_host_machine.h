/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Bot Host machines
 *Filename:      bot_host_machine.h
 *Purpose:
 *  The server's rules for Bot Hosts on one computer, the
 *  game bot limit, and the slot override of a Bot Host
 *  row (-bothost-slots and the lobby). No network and no
 *  state: the server gives the numbers, so the unit tests
 *  can check each rule.
 *********************************************************/

#ifndef BOT_HOST_MACHINE_H
#define BOT_HOST_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "machine_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How a host was found on the same computer as another. */
enum {
    BOT_HOST_MACHINE_HOW_NONE    = 0,
    BOT_HOST_MACHINE_HOW_ADDRESS = 1,   /* its address is the server's */
    BOT_HOST_MACHINE_HOW_TOKEN   = 2    /* its machine token matched */
};

/* What a host shares its computer with. */
enum {
    BOT_HOST_MACHINE_ALONE  = 0,  /* nothing, or it leads its group */
    BOT_HOST_MACHINE_SERVER = 1,  /* the server's computer */
    BOT_HOST_MACHINE_HOST   = 2   /* the computer of the host "other" */
};

/* One host as the grouping sees it. */
typedef struct {
    bool     present;      /* connected */
    bool     playerHost;   /* a player's own host (in that player's client) */
    bool     addrServer;   /* its address is the server's computer
                            * (botHostAddrIsServerMachine) */
    bool     hashHave;     /* it answered the current salt */
    uint8_t  hash[MACHINE_HASH_LEN];
    uint32_t joinSeq;      /* lower = joined first */
    int      offered;      /* the slots it offers (after the override) */
} BotHostMachineIn;

typedef struct {
    uint8_t  kind;         /* BOT_HOST_MACHINE_* */
    int8_t   other;        /* BOT_HOST_MACHINE_HOST: the host it shares
                            * with (its group's leader); else -1 */
    uint8_t  how;          /* BOT_HOST_MACHINE_HOW_* */
    bool     zero;         /* it gets 0 slots and counts 0 */
} BotHostMachineOut;

/* True when ip (network order) is the server's computer: 127.x.x.x, or one
 * of the server's own addresses (own, n of them, network order). A shared
 * public address is never proof, so only these count. */
bool botHostAddrIsServerMachine(uint32_t ip, const uint32_t *own, int n);

/* The groups of hosts on one computer.
 *   - With the server (the address check, or the server's own hash):
 *     kind SERVER; a player's host gets 0, a listed host keeps its slots.
 *   - Hosts only (equal hashes): the first-joined host that offers slots
 *     leads and keeps them; the others are kind HOST with other = the
 *     leader, and get 0. When the leader goes, the next one leads.
 * serverHash may be NULL (the server has no id). out has n entries. */
void botHostMachineGroup(const BotHostMachineIn *in, int n,
                         const uint8_t *serverHash, BotHostMachineOut *out);

/* One step of the same-computer check, once a tick. known[i] is true when
 * host i's group is decided; the server sets it false when the host joins.
 * waiting: answers to the current salt are still to come and the wait
 * limit has not passed.
 *   - Not waiting: every group is worked out again (botHostMachineGroup)
 *     and each present host is known.
 *   - Waiting: a known host keeps its group. A host not yet known is
 *     decided at once when its address or the server's hash shows the
 *     server's computer; else it stays not known.
 * A host not present is not known. out and known have n entries. */
void botHostMachineStep(const BotHostMachineIn *in, int n,
                        const uint8_t *serverHash, bool waiting,
                        BotHostMachineOut *out, bool *known);

/* The slots a host may be given: 0 while its group is not known, 0 when
 * its group gives it 0, else offered (after the override). */
int botHostMachineSlots(bool known, const BotHostMachineOut *m, int offered);

/* True when the server tells a host it is on the server's computer
 * (CTRL_BOT_HOST_SAME_SERVER): its group is known, it is a player's host
 * there (kind SERVER and 0 slots), and it was not told on this connection
 * (sent). A listed host there, and hosts that share another computer, are
 * never told. */
bool botHostMachineTellSameServer(bool known, const BotHostMachineOut *m,
                                  bool sent);

/* The game bot limit: the server's bot slots (serverCap: -1 no limit, -2
 * the CPU test runs, which counts 0) plus counted[i] for each host (the
 * slots of a host that takes bots and is not zeroed; 0 otherwise).
 * -1: no limit. -2: the CPU test runs and no host gives a slot. */
int botHostGameBotLimit(int serverCap, const int *counted, int n);

/* A host's slots with the override (-1: none). It only lowers. */
int botHostSlotsEffective(int reported, int override);

/* The override for a host that reports `reported` slots: a value above
 * them is refused (false). 0xFF clears it (*out = -1). */
bool botHostSlotsOverrideValid(int reported, int value, int *out);

/* One -bothost-slots argument "name=N" (N 0..16). The name is the text
 * before the last '='. False, with a reason in err, for bad input. */
bool botHostSlotsArgParse(const char *arg, char *name, size_t nameSz,
                          int *n, char *err, size_t errSz);

/* The -bothost-slots flags of the server: one per host. */
#define BOT_HOST_SLOT_FLAGS_MAX  16
#define BOT_HOST_SLOT_NAME_LEN   32   /* == JOIN_BOT_HOST_NAME_LEN */
typedef struct {
    struct {
        char name[BOT_HOST_SLOT_NAME_LEN];
        int  n;
    } flag[BOT_HOST_SLOT_FLAGS_MAX];
    int count;
} BotHostSlotFlags;

/* Adds one "name=N" flag. False, with a reason in err, for bad input, a
 * name given before, or too many flags. */
bool botHostSlotFlagsAdd(BotHostSlotFlags *f, const char *arg, char *err,
                         size_t errSz);
/* The flag's N for a host, by its listed name or its player's name (either
 * may be NULL; no case difference); -1 when no flag names it. */
int  botHostSlotFlagsFind(const BotHostSlotFlags *f, const char *hostName,
                          const char *playerName);

/* The slot override of one Bot Host row. */
typedef struct {
    int  override;   /* -1: none */
    bool locked;     /* -bothost-slots set it */
} BotHostSlotOverride;

/* A host joins (or joins again): no override, or the flag's locked one. */
void botHostSlotOverrideJoin(BotHostSlotOverride *o, const BotHostSlotFlags *f,
                             const char *hostName, const char *playerName);
/* The lobby sets value (0..reported, or 0xFF for none). False when it is
 * locked or the value is above the reported slots. */
bool botHostSlotOverrideSet(BotHostSlotOverride *o, int reported, int value);
/* A new lobby: an override from the lobby goes; a locked one stays. */
void botHostSlotOverrideNewLobby(BotHostSlotOverride *o);
/* Who may change a row's slots (LST_BOT_HOST_SLOTS): only the player at a
 * player's computer (playerHost, its owner's player number), on their own
 * row. A Bot Host program's slots are its own (--slots-max). */
bool botHostSlotSetAllowed(bool playerHost, int owner, int player);

/* -bothost-dynamic-slots for one run: 0 in a determinism run (-seed with
 * -brain-lua-seed and -brain-no-budget-kill), whose one canonical game must
 * not follow the machine's free CPU; else dynamic as given (below 0: the
 * default). */
int  botHostDynamicForRun(int dynamic, bool seed, bool luaSeed,
                          bool noBudgetKill);

/* A percent from text: "0" to "100". False for any other text. */
bool botHostParsePercent(const char *text, int *out);

/* Dynamic bot slots (bot_host_link.h): each place has a start, a ceiling
 * and a live limit between them that goes up while the place has free CPU
 * and down by the overrun rules. Each value below 0 keeps its default. */
typedef struct {
    int dynamic;           /* -bothost-dynamic-slots <0|1> */
    int serverMax;         /* -botcapacity-max: the server's ceiling */
    int clientStart;       /* -bothost-client-start: 0 = the host's slots */
    int raiseReservePct;   /* -bothost-raise-reserve-pct */
    int clientReservePct;  /* -bothost-client-reserve-pct */
    int raiseLagMaxMs;     /* -bothost-raise-lag-max-ms, 0: no lag check */
    int raiseStep;         /* -bothost-raise-step */
    int raiseAfterCutSecs; /* -bothost-raise-after-cut-secs */
    int carryLimit;        /* -bothost-carry-limit <0|1> */
    int reserveRecentSecs; /* -bothost-reserve-recent-secs, 0: off */
} BotHostDynamicFlags;
/* Every value at its default (-1). */
void botHostDynamicFlagsInit(BotHostDynamicFlags *f);
/* The KEEL values (-bothost-keel): fixed slots as before dynamic slots,
 * no reserve. serverMax 0 makes the server's ceiling its start. */
void botHostDynamicFlagsKeel(BotHostDynamicFlags *f);

#ifdef __cplusplus
}
#endif

#endif /* BOT_HOST_MACHINE_H */
