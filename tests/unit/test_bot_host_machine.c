/*
 * Bot Hosts on one computer, the game bot limit and the slot override of a
 * Bot Host row (bot_host_machine.h, machine_id.h).
 *
 *   1. bot_host_machine_addr: 127.x.x.x and the server's own addresses are
 *      the server's computer; any other address (a shared public one too)
 *      is not.
 *   2. machine_id_token: SHA-256 gives the known "abc" digest; one id and
 *      one salt give one hash; another id or another salt gives another.
 *   3. machine_id_salt_repeat: a salt answered before gets no answer; the
 *      memory keeps the newest 64, so the 65th salt pushes out the first.
 *   4. bot_host_machine_server_group: a player's host on the server's
 *      computer (by address, or by token) gets 0; a listed host there keeps
 *      its slots; a host on another computer is alone.
 *   5. bot_host_machine_client_group: two hosts with one token and no
 *      server: the first-joined one leads and keeps its slots, the other
 *      gets 0 and names the leader; a host that offers no slot does not
 *      lead.
 *   6. bot_host_machine_leader_leaves: when the leader goes, the next
 *      first-joined host leads.
 *   7. bot_host_game_bot_limit: the server's slots plus each counted host;
 *      a group member, an opted-out host and a host in its CPU test count 0;
 *      no server limit is no limit; the server's CPU test with no host
 *      slots holds the limit.
 *   8. bot_host_slots_override: the override only lowers; a lower report
 *      wins; a value above the report is refused; a locked override
 *      refuses a change; a new lobby clears a lobby override but not a
 *      locked one; a rejoin resets it; the limit counts the override.
 *   9. bot_host_slots_arg_parse: several flags, by listed name or player
 *      name; bad input and a name given twice are refused with a reason.
 *  10. bot_host_machine_join_wait: a player's host that joins from the
 *      server's computer gets 0 slots on every tick, also while the salt
 *      answers are still to come (the bug: it had its full slots for up to
 *      5 s, and the lobby put a bot on it). A host on another computer
 *      gets no slot until its group is known; a known host keeps its
 *      group while a new round waits.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bot_host_machine.h"
#include "bot_host_lockstep.h"
#include "machine_id.h"
#include "test_harness.h"

static uint32_t ipOf(int a, int b, int c, int d) {
    uint32_t ip;
    uint8_t *p = (uint8_t *)&ip;
    p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)c; p[3] = (uint8_t)d;
    return ip;
}

int run_bot_host_machine_addr(void) {
    uint32_t own[2];
    own[0] = ipOf(192, 168, 0, 10);
    own[1] = ipOf(10, 0, 0, 5);
    UT_ASSERT(botHostAddrIsServerMachine(ipOf(127, 0, 0, 1), own, 2));
    UT_ASSERT(botHostAddrIsServerMachine(ipOf(127, 1, 2, 3), NULL, 0));
    UT_ASSERT(botHostAddrIsServerMachine(ipOf(192, 168, 0, 10), own, 2));
    UT_ASSERT(botHostAddrIsServerMachine(ipOf(10, 0, 0, 5), own, 2));
    UT_ASSERT(!botHostAddrIsServerMachine(ipOf(192, 168, 0, 58), own, 2));
    /* A public address the server shares with others behind one router is
     * not one of its own interface addresses, so it is no proof. */
    UT_ASSERT(!botHostAddrIsServerMachine(ipOf(203, 0, 113, 7), own, 2));
    UT_ASSERT(!botHostAddrIsServerMachine(0, own, 2));
    return 0;
}

int run_machine_id_token(void) {
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t d[32], h1[32], h2[32], h3[32], h4[32];
    uint8_t idA[MACHINE_ID_LEN], idB[MACHINE_ID_LEN];
    uint8_t s1[MACHINE_SALT_LEN], s2[MACHINE_SALT_LEN];
    uint8_t big[1000];
    machineSha256("abc", 3, d);
    UT_ASSERT(memcmp(d, abc, 32) == 0);
    /* A message longer than one block works too (no crash, stable). */
    memset(big, 'a', sizeof(big));
    machineSha256(big, sizeof(big), h1);
    machineSha256(big, sizeof(big), h2);
    UT_ASSERT(memcmp(h1, h2, 32) == 0);

    memset(idA, 0x11, sizeof(idA));
    memset(idB, 0x11, sizeof(idB));
    idB[31] = 0x12;
    memset(s1, 0x21, sizeof(s1));
    memset(s2, 0x21, sizeof(s2));
    s2[0] = 0x22;
    machineIdHashSalt(idA, s1, h1);
    machineIdHashSalt(idA, s1, h2);
    machineIdHashSalt(idB, s1, h3);
    machineIdHashSalt(idA, s2, h4);
    UT_ASSERT(memcmp(h1, h2, 32) == 0);   /* one computer: equal */
    UT_ASSERT(memcmp(h1, h3, 32) != 0);   /* another computer */
    UT_ASSERT(memcmp(h1, h4, 32) != 0);   /* another salt */
    return 0;
}

int run_machine_id_salt_repeat(void) {
    MachineSaltMemory m;
    uint8_t id[MACHINE_ID_LEN], salt[MACHINE_SALT_LEN], h[32], first[32];
    int i;
    memset(&m, 0, sizeof(m));
    memset(id, 0x42, sizeof(id));
    memset(salt, 0, sizeof(salt));
    salt[0] = 1;
    UT_ASSERT(machineSaltAnswer(&m, id, salt, first));
    /* The same salt again: no answer (no replay of an old answer). */
    UT_ASSERT(!machineSaltAnswer(&m, id, salt, h));
    /* 64 more salts push the first one out of the memory. */
    for (i = 0; i < MACHINE_SALT_KEEP - 1; i++) {
        salt[1] = (uint8_t)(i + 1);
        UT_ASSERT(machineSaltAnswer(&m, id, salt, h));
    }
    UT_ASSERT(m.count == MACHINE_SALT_KEEP);
    salt[1] = 0;
    UT_ASSERT(machineSaltMemorySeen(&m, salt));
    salt[1] = 0xEE;
    UT_ASSERT(machineSaltAnswer(&m, id, salt, h));
    salt[1] = 0;
    UT_ASSERT(!machineSaltMemorySeen(&m, salt));
    UT_ASSERT(machineSaltAnswer(&m, id, salt, h));
    UT_ASSERT(memcmp(h, first, 32) == 0);
    return 0;
}

static void hostIn(BotHostMachineIn *in, bool player, bool addrServer,
                   int hashByte, uint32_t seq, int offered) {
    memset(in, 0, sizeof(*in));
    in->present = true;
    in->playerHost = player;
    in->addrServer = addrServer;
    in->hashHave = hashByte >= 0;
    if (hashByte >= 0) memset(in->hash, hashByte, sizeof(in->hash));
    in->joinSeq = seq;
    in->offered = offered;
}

int run_bot_host_machine_server_group(void) {
    BotHostMachineIn in[4];
    BotHostMachineOut out[4];
    uint8_t server[MACHINE_HASH_LEN];
    memset(server, 0xAA, sizeof(server));
    hostIn(&in[0], true, true, -1, 1, 4);     /* player host, by address */
    hostIn(&in[1], true, false, 0xAA, 2, 4);  /* player host, by token */
    hostIn(&in[2], false, false, 0xAA, 3, 4); /* listed host, by token */
    hostIn(&in[3], true, false, 0xBB, 4, 4);  /* another computer */
    botHostMachineGroup(in, 4, server, out);
    UT_ASSERT(out[0].kind == BOT_HOST_MACHINE_SERVER && out[0].zero &&
              out[0].how == BOT_HOST_MACHINE_HOW_ADDRESS);
    UT_ASSERT(out[1].kind == BOT_HOST_MACHINE_SERVER && out[1].zero &&
              out[1].how == BOT_HOST_MACHINE_HOW_TOKEN);
    UT_ASSERT(out[2].kind == BOT_HOST_MACHINE_SERVER && !out[2].zero);
    UT_ASSERT(out[3].kind == BOT_HOST_MACHINE_ALONE && !out[3].zero &&
              out[3].other == -1);
    /* No server hash: only the address check finds the server's computer. */
    botHostMachineGroup(in, 4, NULL, out);
    UT_ASSERT(out[0].kind == BOT_HOST_MACHINE_SERVER && out[0].zero);
    UT_ASSERT(out[1].kind == BOT_HOST_MACHINE_ALONE ||
              out[1].kind == BOT_HOST_MACHINE_HOST);
    return 0;
}

int run_bot_host_machine_client_group(void) {
    BotHostMachineIn in[3];
    BotHostMachineOut out[3];
    hostIn(&in[0], true, false, 0x33, 5, 4);
    hostIn(&in[1], true, false, 0x33, 2, 4);   /* joined first */
    hostIn(&in[2], true, false, 0x44, 1, 4);   /* another computer */
    botHostMachineGroup(in, 3, NULL, out);
    UT_ASSERT(out[1].kind == BOT_HOST_MACHINE_ALONE && !out[1].zero &&
              out[1].how == BOT_HOST_MACHINE_HOW_TOKEN);
    UT_ASSERT(out[0].kind == BOT_HOST_MACHINE_HOST && out[0].zero &&
              out[0].other == 1);
    UT_ASSERT(out[2].kind == BOT_HOST_MACHINE_ALONE && !out[2].zero &&
              out[2].how == BOT_HOST_MACHINE_HOW_NONE);
    /* The first-joined host offers no slot (opted out): the next leads. */
    in[1].offered = 0;
    botHostMachineGroup(in, 3, NULL, out);
    UT_ASSERT(!out[0].zero && out[1].zero && out[1].other == 0);
    /* No answer: no proof, so a different computer. */
    in[1].offered = 4;
    in[0].hashHave = false;
    botHostMachineGroup(in, 3, NULL, out);
    UT_ASSERT(!out[0].zero && !out[1].zero);
    return 0;
}

int run_bot_host_machine_leader_leaves(void) {
    BotHostMachineIn in[3];
    BotHostMachineOut out[3];
    hostIn(&in[0], true, false, 0x55, 1, 4);
    hostIn(&in[1], true, false, 0x55, 3, 4);
    hostIn(&in[2], false, false, 0x55, 2, 4);
    botHostMachineGroup(in, 3, NULL, out);
    UT_ASSERT(!out[0].zero && out[1].zero && out[2].zero);
    UT_ASSERT(out[1].other == 0 && out[2].other == 0);
    in[0].present = false;   /* the leader goes */
    botHostMachineGroup(in, 3, NULL, out);
    UT_ASSERT(out[2].kind == BOT_HOST_MACHINE_ALONE && !out[2].zero);
    UT_ASSERT(out[1].zero && out[1].other == 2);
    in[2].present = false;   /* only one is left: alone */
    botHostMachineGroup(in, 3, NULL, out);
    UT_ASSERT(!out[1].zero && out[1].kind == BOT_HOST_MACHINE_ALONE);
    return 0;
}

int run_bot_host_game_bot_limit(void) {
    BotHostMachineIn in[4];
    BotHostMachineOut out[4];
    int counted[4], i;
    bool takes[4] = { true, true, true, false };   /* 3: in its CPU test */
    UT_ASSERT(botHostGameBotLimit(4, NULL, 0) == 4);
    UT_ASSERT(botHostGameBotLimit(-1, NULL, 0) == -1);
    UT_ASSERT(botHostGameBotLimit(-2, NULL, 0) == -2);
    hostIn(&in[0], true, false, 0x66, 1, 3);
    hostIn(&in[1], true, false, 0x66, 2, 3);   /* same computer as 0 */
    hostIn(&in[2], true, false, 0x77, 3, 0);   /* opted out */
    hostIn(&in[3], false, false, 0x88, 4, 5);
    botHostMachineGroup(in, 4, NULL, out);
    for (i = 0; i < 4; i++) {
        counted[i] = (takes[i] && !out[i].zero) ? in[i].offered : 0;
    }
    UT_ASSERT_MSG(botHostGameBotLimit(4, counted, 4) == 7, "limit %d",
                  botHostGameBotLimit(4, counted, 4));
    /* The CPU test runs on the server: it counts 0, the hosts count. */
    UT_ASSERT(botHostGameBotLimit(-2, counted, 4) == 3);
    /* More than 16 is 16. */
    counted[0] = 16;
    UT_ASSERT(botHostGameBotLimit(4, counted, 4) == 16);
    return 0;
}

int run_bot_host_slots_override(void) {
    BotHostSlotFlags flags;
    BotHostSlotOverride o;
    char err[128];
    int v, counted[2];
    UT_ASSERT(botHostSlotsEffective(4, -1) == 4);
    UT_ASSERT(botHostSlotsEffective(4, 2) == 2);
    UT_ASSERT(botHostSlotsEffective(1, 2) == 1);   /* a lower report wins */
    UT_ASSERT(botHostSlotsOverrideValid(4, 4, &v) && v == 4);
    UT_ASSERT(botHostSlotsOverrideValid(4, 0, &v) && v == 0);
    UT_ASSERT(botHostSlotsOverrideValid(4, 0xFF, &v) && v == -1);
    UT_ASSERT(!botHostSlotsOverrideValid(4, 5, &v));   /* only lowers */

    memset(&flags, 0, sizeof(flags));
    UT_ASSERT(botHostSlotFlagsAdd(&flags, "basement=1", err, sizeof(err)));
    /* A host no flag names: the lobby may set and clear it. */
    botHostSlotOverrideJoin(&o, &flags, "attic", "Andrew");
    UT_ASSERT(o.override == -1 && !o.locked);
    UT_ASSERT(botHostSlotOverrideSet(&o, 4, 2) && o.override == 2);
    UT_ASSERT(!botHostSlotOverrideSet(&o, 4, 6) && o.override == 2);
    counted[0] = botHostSlotsEffective(4, o.override);
    counted[1] = 3;
    UT_ASSERT(botHostGameBotLimit(2, counted, 2) == 7);
    botHostSlotOverrideNewLobby(&o);
    UT_ASSERT(o.override == -1);
    UT_ASSERT(botHostSlotOverrideSet(&o, 4, 1));
    /* It joins again: the override resets. */
    botHostSlotOverrideJoin(&o, &flags, "attic", "Andrew");
    UT_ASSERT(o.override == -1 && !o.locked);

    /* The flag's host: locked, the lobby cannot change it. */
    botHostSlotOverrideJoin(&o, &flags, "Basement", NULL);
    UT_ASSERT(o.locked && o.override == 1);
    UT_ASSERT(!botHostSlotOverrideSet(&o, 4, 0) && o.override == 1);
    UT_ASSERT(!botHostSlotOverrideSet(&o, 4, 0xFF) && o.override == 1);
    botHostSlotOverrideNewLobby(&o);
    UT_ASSERT(o.override == 1);
    /* A locked value only lowers too. */
    UT_ASSERT(botHostSlotsEffective(0, o.override) == 0);
    return 0;
}

int run_bot_host_slots_arg_parse(void) {
    BotHostSlotFlags flags;
    char name[32], err[128];
    int n;
    UT_ASSERT(botHostSlotsArgParse("basement=2", name, sizeof(name), &n, err,
                                   sizeof(err)));
    UT_ASSERT(strcmp(name, "basement") == 0 && n == 2);
    /* The last '=' splits: a name may hold one. */
    UT_ASSERT(botHostSlotsArgParse("a=b=16", name, sizeof(name), &n, err,
                                   sizeof(err)));
    UT_ASSERT(strcmp(name, "a=b") == 0 && n == 16);
    UT_ASSERT(!botHostSlotsArgParse("basement", name, sizeof(name), &n, err,
                                    sizeof(err)) && err[0] != '\0');
    UT_ASSERT(!botHostSlotsArgParse("=3", name, sizeof(name), &n, err,
                                    sizeof(err)) && err[0] != '\0');
    UT_ASSERT(!botHostSlotsArgParse("x=", name, sizeof(name), &n, err,
                                    sizeof(err)) && err[0] != '\0');
    UT_ASSERT(!botHostSlotsArgParse("x=17", name, sizeof(name), &n, err,
                                    sizeof(err)) && err[0] != '\0');
    UT_ASSERT(!botHostSlotsArgParse("x=-1", name, sizeof(name), &n, err,
                                    sizeof(err)) && err[0] != '\0');
    UT_ASSERT(!botHostSlotsArgParse("x=2a", name, sizeof(name), &n, err,
                                    sizeof(err)) && err[0] != '\0');
    UT_ASSERT(!botHostSlotsArgParse(
                  "a_name_that_is_much_too_long_for_a_host=1", name,
                  sizeof(name), &n, err, sizeof(err)) && err[0] != '\0');

    memset(&flags, 0, sizeof(flags));
    UT_ASSERT(botHostSlotFlagsAdd(&flags, "basement=2", err, sizeof(err)));
    UT_ASSERT(botHostSlotFlagsAdd(&flags, "Andrew=0", err, sizeof(err)));
    UT_ASSERT(flags.count == 2);
    UT_ASSERT(!botHostSlotFlagsAdd(&flags, "BASEMENT=3", err, sizeof(err)) &&
              err[0] != '\0');
    UT_ASSERT(!botHostSlotFlagsAdd(&flags, "bad", err, sizeof(err)));
    UT_ASSERT(flags.count == 2);
    UT_ASSERT(botHostSlotFlagsFind(&flags, "basement", NULL) == 2);
    /* A player's host by its player's name. */
    UT_ASSERT(botHostSlotFlagsFind(&flags, "Andrew's host", "Andrew") == 0);
    UT_ASSERT(botHostSlotFlagsFind(&flags, "attic", "Elvis") == -1);
    return 0;
}

int run_bot_host_machine_join_wait(void) {
    BotHostMachineIn in[3];
    BotHostMachineOut out[3];
    bool known[3];
    uint8_t server[MACHINE_HASH_LEN];
    memset(server, 0xAA, sizeof(server));
    memset(out, 0, sizeof(out));
    memset(known, 0, sizeof(known));
    out[0].other = out[1].other = out[2].other = -1;
    /* A listed host on another computer, known from an earlier round. */
    hostIn(&in[0], false, false, 0x11, 1, 3);
    botHostMachineStep(in, 1, server, false, out, known);
    UT_ASSERT(known[0] && botHostMachineSlots(known[0], &out[0], 3) == 3);
    /* The player Canuck's host joins from 127.0.0.1 with 4 slots. Its
     * group is cleared (as at a join), and the salt answers are still to
     * come. It has 0 slots at once, before any answer. */
    hostIn(&in[1], true, true, -1, 2, 4);
    in[0].hashHave = false;     /* a new round: no answer yet */
    memset(&out[1], 0, sizeof(out[1]));
    out[1].other = -1;
    known[1] = false;
    UT_ASSERT(botHostMachineSlots(known[1], &out[1], 4) == 0);
    botHostMachineStep(in, 2, server, true, out, known);
    UT_ASSERT(known[1]);
    UT_ASSERT(out[1].kind == BOT_HOST_MACHINE_SERVER && out[1].zero &&
              out[1].how == BOT_HOST_MACHINE_HOW_ADDRESS);
    UT_ASSERT(botHostMachineSlots(known[1], &out[1], 4) == 0);
    /* The known host keeps its group and its slots while the round waits. */
    UT_ASSERT(known[0] && botHostMachineSlots(known[0], &out[0], 3) == 3);
    /* The same by token: the address is a LAN one the server does not
     * hold, but the answer matches the server's hash. */
    hostIn(&in[2], true, false, 0xAA, 3, 4);
    memset(&out[2], 0, sizeof(out[2]));
    out[2].other = -1;
    known[2] = false;
    botHostMachineStep(in, 3, server, true, out, known);
    UT_ASSERT(known[2] && out[2].zero &&
              out[2].how == BOT_HOST_MACHINE_HOW_TOKEN);
    UT_ASSERT(botHostMachineSlots(known[2], &out[2], 4) == 0);
    /* A host on another computer with no answer yet: 0 slots while the
     * round waits, its slots once the wait is over. */
    hostIn(&in[2], true, false, -1, 3, 4);
    memset(&out[2], 0, sizeof(out[2]));
    out[2].other = -1;
    known[2] = false;
    botHostMachineStep(in, 3, server, true, out, known);
    UT_ASSERT(!known[2] && botHostMachineSlots(known[2], &out[2], 4) == 0);
    botHostMachineStep(in, 3, server, false, out, known);
    UT_ASSERT(known[2] && out[2].kind == BOT_HOST_MACHINE_ALONE);
    UT_ASSERT(botHostMachineSlots(known[2], &out[2], 4) == 4);
    /* After the wait, Canuck's host still has 0 slots. */
    UT_ASSERT(known[1] && botHostMachineSlots(known[1], &out[1], 4) == 0);
    /* A host that goes is not known. */
    in[2].present = false;
    botHostMachineStep(in, 3, server, true, out, known);
    UT_ASSERT(!known[2]);
    return 0;
}

/* The server's wait for its own child's moves (bot_host_lockstep.h): only
 * the child's seats are waited for; a new input of each meets the wait; a
 * missing one waits until the timeout, then the tick goes on (LATE); no
 * seat, or a wait of 0, never waits. */
/* Arm twice with a new input of each "on" seat between: after this,
 * each "on" seat is a waited seat (the new-seat rule is met). */
static void lockstepWarm(BotHostLockstep *ls, const bool *on, uint32_t *cnt,
                         int n) {
    int i;
    botHostLockstepReset(ls);
    botHostLockstepArm(ls, on, cnt, n);
    for (i = 0; i < n && i < BOT_HOST_LOCKSTEP_SEATS; i++) {
        if (on[i]) cnt[i]++;
    }
    botHostLockstepArm(ls, on, cnt, n);
}

int run_bot_host_lockstep(void) {
    BotHostLockstep ls;
    bool on[BOT_HOST_LOCKSTEP_SEATS];
    uint32_t cnt[BOT_HOST_LOCKSTEP_SEATS];
    int i;

    memset(on, 0, sizeof(on));
    memset(cnt, 0, sizeof(cnt));
    on[1] = on[3] = on[4] = true;
    cnt[1] = 10;
    cnt[3] = 0xFFFFFFFEu;   /* the count may wrap */
    cnt[4] = 7;
    cnt[2] = 5;             /* not the child's: never waited for */

    /* A new seat is not waited for until its first new input. */
    botHostLockstepReset(&ls);
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 0);
    UT_ASSERT(botHostLockstepStep(0, 0, 0.0, 15) == BOT_HOST_LOCKSTEP_NONE);
    cnt[1]++; cnt[3]++; cnt[4]++;   /* cnt[3] is now 0xFFFFFFFF */
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 3);
    UT_ASSERT(!ls.want[2]);
    UT_ASSERT(botHostLockstepMissing(&ls, cnt, BOT_HOST_LOCKSTEP_SEATS) == 3);
    UT_ASSERT(botHostLockstepStep(3, 3, 0.0, 15) == BOT_HOST_LOCKSTEP_WAIT);

    cnt[2]++;               /* another seat's move does not count */
    cnt[1]++;
    UT_ASSERT(botHostLockstepMissing(&ls, cnt, BOT_HOST_LOCKSTEP_SEATS) == 2);
    cnt[3]++;               /* wraps to 0: still a new move */
    UT_ASSERT(botHostLockstepMissing(&ls, cnt, BOT_HOST_LOCKSTEP_SEATS) == 1);
    UT_ASSERT(botHostLockstepStep(1, 3, 14.9, 15) == BOT_HOST_LOCKSTEP_WAIT);
    UT_ASSERT(botHostLockstepStep(1, 3, 15.0, 15) == BOT_HOST_LOCKSTEP_LATE);
    UT_ASSERT(botHostLockstepStep(1, 3, 40.0, 15) == BOT_HOST_LOCKSTEP_LATE);
    cnt[4] += 2;            /* two moves count as one seat met */
    UT_ASSERT(botHostLockstepMissing(&ls, cnt, BOT_HOST_LOCKSTEP_SEATS) == 0);
    UT_ASSERT(botHostLockstepStep(0, 3, 0.1, 15) == BOT_HOST_LOCKSTEP_MET);
    /* Met wins over the timeout. */
    UT_ASSERT(botHostLockstepStep(0, 3, 99.0, 15) == BOT_HOST_LOCKSTEP_MET);

    /* Next arm: seat 4's two moves count, so it stays waited for. */
    cnt[1]++; cnt[3]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 3);
    /* The idle rule: seat 4 sends nothing more. One arm with no new
     * input: still waited for (one late tick is not idle). */
    cnt[1]++; cnt[3]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 3);
    UT_ASSERT(ls.want[4]);
    /* Two arms with no new input: not waited for. */
    cnt[1]++; cnt[3]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 2);
    UT_ASSERT(!ls.want[4]);
    UT_ASSERT(botHostLockstepMissing(&ls, cnt, BOT_HOST_LOCKSTEP_SEATS) == 2);
    /* More idle arms: still not waited for. */
    cnt[1]++; cnt[3]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(!ls.want[4]);
    /* Its next new input makes it a waited seat at the next arm. */
    cnt[1]++; cnt[3]++; cnt[4]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 3);
    UT_ASSERT(ls.want[4]);

    /* A seat that leaves the child and comes back is a new seat. */
    on[3] = false;
    cnt[1]++; cnt[3]++; cnt[4]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 2);
    UT_ASSERT(!ls.want[3]);
    on[3] = true;
    cnt[1]++; cnt[3]++; cnt[4]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(!ls.want[3]);
    cnt[1]++; cnt[3]++; cnt[4]++;
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.want[3]);
    UT_ASSERT(ls.wanted == 3);

    /* No seat, or no wait: never waits. */
    memset(on, 0, sizeof(on));
    botHostLockstepArm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS);
    UT_ASSERT(ls.wanted == 0);
    UT_ASSERT(botHostLockstepStep(0, 0, 0.0, 15) == BOT_HOST_LOCKSTEP_NONE);
    UT_ASSERT(botHostLockstepStep(2, 2, 0.0, 0) == BOT_HOST_LOCKSTEP_NONE);
    UT_ASSERT(botHostLockstepStep(2, 2, 0.0, -1) == BOT_HOST_LOCKSTEP_NONE);

    /* A short table: seats past it are not waited for. */
    for (i = 0; i < BOT_HOST_LOCKSTEP_SEATS; i++) on[i] = true;
    lockstepWarm(&ls, on, cnt, 4);
    UT_ASSERT(ls.wanted == 4);
    UT_ASSERT(botHostLockstepMissing(&ls, cnt, 2) == 2);
    /* More seats than the table holds: only the table's are armed. */
    lockstepWarm(&ls, on, cnt, BOT_HOST_LOCKSTEP_SEATS + 5);
    UT_ASSERT(ls.wanted == BOT_HOST_LOCKSTEP_SEATS);
    /* NULL is safe, and clears the history. */
    botHostLockstepArm(&ls, NULL, cnt, 4);
    UT_ASSERT(ls.wanted == 0);
    UT_ASSERT(!ls.seen[0]);
    UT_ASSERT(botHostLockstepMissing(NULL, cnt, 4) == 0);
    botHostLockstepArm(NULL, on, cnt, 4);
    botHostLockstepReset(NULL);

    /* The pending timeout needs both the ticks and the wall time. */
    UT_ASSERT(!botHostPendingExpired(100, 100, 5000, 5000));
    /* -asap: 501 ticks in 2 s is not a timeout. */
    UT_ASSERT(!botHostPendingExpired(601, 100, 7000, 5000));
    /* A slow server: 10.001 s with 400 ticks is not a timeout. */
    UT_ASSERT(!botHostPendingExpired(500, 100, 15001, 5000));
    /* Exactly at the limits: not yet. */
    UT_ASSERT(!botHostPendingExpired(600, 100, 15000, 5000));
    UT_ASSERT(!botHostPendingExpired(601, 100, 15000, 5000));
    UT_ASSERT(!botHostPendingExpired(600, 100, 15001, 5000));
    /* Both past: a timeout. */
    UT_ASSERT(botHostPendingExpired(601, 100, 15001, 5000));
    UT_ASSERT(botHostPendingExpired(100000, 100, 900000, 5000));
    /* The tick count wraps. */
    UT_ASSERT(botHostPendingExpired(400, 0xFFFFFF00u, 15001, 5000));
    UT_ASSERT(!botHostPendingExpired(10, 0xFFFFFF00u, 15001, 5000));
    return 0;
}
