/*
 * The lobby's CPU estimate for each Bot Host row (bot_host_cpu.h,
 * CTRL_BOT_HOST_CPU).
 *
 *   1. bot_host_cpu_percent: think time over (wall time x cores), rounded,
 *      0..100; above 0 and below 1 is BOT_HOST_CPU_UNDER_ONE; no wall time
 *      or no cores is unknown; a wire byte that is not a percent reads as
 *      unknown.
 *   2. bot_host_cpu_meter: one-second buckets, the last five added up; the
 *      estimate is unknown until the first bucket ends, 0 with no bots, and
 *      a total that goes down counts as no think time.
 *   3. bot_host_cpu_codec: CTRL_BOT_HOST_CPU keeps idx and cpu through the
 *      wire form; more bytes after them are skipped; a short body or a bad
 *      row is refused; a cpu byte that is not a percent reads as unknown.
 *   4. bot_host_cpu_client_row: a client row gets its percent from
 *      CTRL_BOT_HOST_CPU; a row with no CTRL_BOT_HOST_CPU (an older server)
 *      stays unknown; a new CTRL_BOT_HOST_STATUS for the same host keeps the
 *      percent; the row going clears it, and a new host in that row starts
 *      unknown.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "bot_host_cpu.h"
#include "test_harness.h"

#define NS_PER_S 1000000000ull

/* ================================================================
 * 1. The percent
 * ================================================================ */
int run_bot_host_cpu_percent(void) {
    /* 2 s of think time in 5 s on 4 cores: 10%. */
    UT_ASSERT(botHostCpuPercent(2000.0, 5000.0, 4) == 10);
    /* The machine's cores count: the same think time on 8 cores is 5%. */
    UT_ASSERT(botHostCpuPercent(2000.0, 5000.0, 8) == 5);
    /* Rounded to the nearest: 12.4% and 12.5%. */
    UT_ASSERT(botHostCpuPercent(124.0, 1000.0, 1) == 12);
    UT_ASSERT(botHostCpuPercent(125.0, 1000.0, 1) == 13);
    /* Clamped at 100: more think time than wall time x cores (a think
     * that lost its core to another program counts all its wall time). */
    UT_ASSERT(botHostCpuPercent(5000.0, 1000.0, 2) == 100);
    UT_ASSERT(botHostCpuPercent(1999.0, 1000.0, 2) == 100);
    UT_ASSERT(botHostCpuPercent(1980.0, 1000.0, 2) == 99);
    /* None at all is 0%. */
    UT_ASSERT(botHostCpuPercent(0.0, 5000.0, 4) == 0);
    /* Above 0 and below 1%: the "<1%" byte, also for 0.99%. */
    UT_ASSERT(botHostCpuPercent(1.0, 5000.0, 16) == BOT_HOST_CPU_UNDER_ONE);
    UT_ASSERT(botHostCpuPercent(9.9, 1000.0, 1) == BOT_HOST_CPU_UNDER_ONE);
    UT_ASSERT(botHostCpuPercent(10.0, 1000.0, 1) == 1);
    /* Nothing to divide by, or a bad think time: unknown. */
    UT_ASSERT(botHostCpuPercent(10.0, 0.0, 4) == BOT_HOST_CPU_UNKNOWN);
    UT_ASSERT(botHostCpuPercent(10.0, 1000.0, 0) == BOT_HOST_CPU_UNKNOWN);
    UT_ASSERT(botHostCpuPercent(-1.0, 1000.0, 4) == BOT_HOST_CPU_UNKNOWN);

    /* The wire byte: a percent and the two others stay; the rest is
     * unknown. */
    UT_ASSERT(botHostCpuFromWire(0) == 0);
    UT_ASSERT(botHostCpuFromWire(57) == 57);
    UT_ASSERT(botHostCpuFromWire(100) == 100);
    UT_ASSERT(botHostCpuFromWire(BOT_HOST_CPU_UNDER_ONE) == BOT_HOST_CPU_UNDER_ONE);
    UT_ASSERT(botHostCpuFromWire(BOT_HOST_CPU_UNKNOWN) == BOT_HOST_CPU_UNKNOWN);
    UT_ASSERT(botHostCpuFromWire(101) == BOT_HOST_CPU_UNKNOWN);
    UT_ASSERT(botHostCpuFromWire(0xFD) == BOT_HOST_CPU_UNKNOWN);
    return 0;
}

/* ================================================================
 * 2. The meter
 * ================================================================ */

/* Runs the meter at 50 samples a second for secs seconds with thinkPerS
 * ms of think time each second. Gives the last estimate. */
static uint8_t meterRun(BotHostCpuMeter *m, uint64_t *now, double *total,
                        int secs, double thinkPerS, int cores, int bots) {
    uint8_t v = 0;
    int i;
    for (i = 0; i < secs * 50; i++) {
        *now += NS_PER_S / 50;
        *total += thinkPerS / 50.0;
        v = botHostCpuMeterSample(m, *now, *total, cores, bots);
    }
    return v;
}

int run_bot_host_cpu_meter(void) {
    BotHostCpuMeter m;
    uint64_t now = 1000 * NS_PER_S;
    double total = 0.0;
    uint8_t v;

    botHostCpuMeterInit(&m);
    UT_ASSERT(m.value == BOT_HOST_CPU_UNKNOWN);
    /* The first sample starts the first bucket; until it ends there is no
     * estimate. */
    UT_ASSERT(botHostCpuMeterSample(&m, now, total, 4, 2) == BOT_HOST_CPU_UNKNOWN);
    now += NS_PER_S / 2;
    total += 200.0;
    UT_ASSERT(botHostCpuMeterSample(&m, now, total, 4, 2) == BOT_HOST_CPU_UNKNOWN);
    /* With no bots it is 0 all the same. */
    UT_ASSERT(botHostCpuMeterSample(&m, now, total, 4, 0) == 0);

    /* 400 ms of think time a second on 4 cores: 10%. */
    botHostCpuMeterInit(&m);
    total = 0.0;
    (void)botHostCpuMeterSample(&m, now, total, 4, 2);
    v = meterRun(&m, &now, &total, 6, 400.0, 4, 2);
    UT_ASSERT_MSG(v == 10, "steady 10%% read %u", (unsigned)v);

    /* The window is the last 5 s: after a step to 1200 ms a second (30%),
     * the estimate moves over the next 5 s and then holds at 30%. */
    v = meterRun(&m, &now, &total, 1, 1200.0, 4, 2);
    UT_ASSERT_MSG(v > 10 && v < 30, "one second after the step: %u", (unsigned)v);
    v = meterRun(&m, &now, &total, 2, 1200.0, 4, 2);
    UT_ASSERT_MSG(v > 10 && v < 30, "three seconds after the step: %u", (unsigned)v);
    v = meterRun(&m, &now, &total, 3, 1200.0, 4, 2);
    UT_ASSERT_MSG(v == 30, "six seconds after the step: %u", (unsigned)v);

    /* The bots go: 0 at once, whatever the window still holds. */
    v = meterRun(&m, &now, &total, 1, 0.0, 4, 0);
    UT_ASSERT(v == 0);

    /* A little think time on many cores: "<1%". */
    botHostCpuMeterInit(&m);
    total = 0.0;
    (void)botHostCpuMeterSample(&m, now, total, 16, 1);
    v = meterRun(&m, &now, &total, 5, 50.0, 16, 1);
    UT_ASSERT_MSG(v == BOT_HOST_CPU_UNDER_ONE, "0.3%% read %u", (unsigned)v);

    /* More than the cores can do (preempted thinks): 100. */
    v = meterRun(&m, &now, &total, 6, 40000.0, 16, 1);
    UT_ASSERT(v == 100);

    /* A total that goes down (a new game's counters) counts as no think
     * time, not as a negative one. */
    botHostCpuMeterInit(&m);
    total = 5000.0;
    (void)botHostCpuMeterSample(&m, now, total, 4, 1);
    now += NS_PER_S;
    total = 10.0;
    v = botHostCpuMeterSample(&m, now, total, 4, 1);
    UT_ASSERT_MSG(v == 0, "a smaller total read %u", (unsigned)v);
    return 0;
}

/* ================================================================
 * 3. The wire form
 * ================================================================ */
int run_bot_host_cpu_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_CPU);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_CPU);
    ControlEvent in, out;
    uint8_t buf[16];
    size_t len = 0;

    UT_ASSERT(enc != NULL && dec != NULL);
    UT_ASSERT(strcmp(controlEventTypeName(CTRL_BOT_HOST_CPU),
                     "CTRL_BOT_HOST_CPU") == 0);

    /* Server to client: a row and its percent. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_CPU;
    in.u.botHostCpu.idx = 3;
    in.u.botHostCpu.cpu = 17;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(len == 2);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.type == CTRL_BOT_HOST_CPU);
    UT_ASSERT(out.u.botHostCpu.idx == 3 && out.u.botHostCpu.cpu == 17);

    /* The server's own row, and the "<1%" byte. */
    in.u.botHostCpu.idx = BOT_HOST_STATUS_ROWS - 1;
    in.u.botHostCpu.cpu = BOT_HOST_CPU_UNDER_ONE;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.u.botHostCpu.idx == BOT_HOST_STATUS_ROWS - 1);
    UT_ASSERT(out.u.botHostCpu.cpu == BOT_HOST_CPU_UNDER_ONE);

    /* Host to server: its own estimate. */
    in.u.botHostCpu.idx = BOT_HOST_CPU_IDX_SELF;
    in.u.botHostCpu.cpu = 100;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.u.botHostCpu.idx == BOT_HOST_CPU_IDX_SELF);
    UT_ASSERT(out.u.botHostCpu.cpu == 100);

    /* A later sender may add bytes: they are skipped. */
    buf[0] = 2; buf[1] = 44; buf[2] = 0xAB; buf[3] = 0xCD;
    UT_ASSERT(dec(buf, 4, &out));
    UT_ASSERT(out.u.botHostCpu.idx == 2 && out.u.botHostCpu.cpu == 44);

    /* A byte that is not a percent reads as unknown. */
    buf[1] = 150;
    UT_ASSERT(dec(buf, 2, &out));
    UT_ASSERT(out.u.botHostCpu.cpu == BOT_HOST_CPU_UNKNOWN);

    /* Too short, or a row past the list: refused. */
    UT_ASSERT(!dec(buf, 1, &out));
    UT_ASSERT(!dec(buf, 0, &out));
    buf[0] = BOT_HOST_STATUS_ROWS;
    buf[1] = 10;
    UT_ASSERT(!dec(buf, 2, &out));

    /* No room to encode. */
    UT_ASSERT(enc(&in, NULL, buf, 1, &len) != ENCODE_OK);
    return 0;
}

/* ================================================================
 * 4. The client's row
 * ================================================================ */
static void applyStatus(ClientSim *cs, uint8_t idx, bool connected,
                        const char *name) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_STATUS;
    evt.u.botHostStatus.idx = idx;
    evt.u.botHostStatus.connected = connected ? 1 : 0;
    if (connected) {
        evt.u.botHostStatus.state = BOT_HOST_SYNC_READY;
        evt.u.botHostStatus.percent = 100;
        evt.u.botHostStatus.slots = 4;
        SDL_strlcpy(evt.u.botHostStatus.name, name,
                    sizeof(evt.u.botHostStatus.name));
    }
    clientSimApplyControl(cs, &evt);
}

static void applyCpu(ClientSim *cs, uint8_t idx, uint8_t cpu) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_CPU;
    evt.u.botHostCpu.idx = idx;
    evt.u.botHostCpu.cpu = cpu;
    clientSimApplyControl(cs, &evt);
}

int run_bot_host_cpu_client_row(void) {
    ClientSim *cs = clientSimAlloc();
    const ClientBotHostRow *r;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    /* A row from an older server: no CTRL_BOT_HOST_CPU, no percent. */
    applyStatus(cs, 0, true, "Laptop");
    r = clientSimGetBotHostRow(cs, 0);
    UT_ASSERT(r != NULL && r->connected);
    UT_ASSERT_MSG(r->cpu == BOT_HOST_CPU_UNKNOWN, "no estimate read %u",
                  (unsigned)r->cpu);

    /* The estimate comes after the row. */
    applyCpu(cs, 0, 23);
    UT_ASSERT(clientSimGetBotHostRow(cs, 0)->cpu == 23);

    /* The same host's row changes (a bot more): the percent stays. */
    applyStatus(cs, 0, true, "Laptop");
    UT_ASSERT(clientSimGetBotHostRow(cs, 0)->cpu == 23);
    applyCpu(cs, 0, BOT_HOST_CPU_UNDER_ONE);
    UT_ASSERT(clientSimGetBotHostRow(cs, 0)->cpu == BOT_HOST_CPU_UNDER_ONE);

    /* An estimate for a row that is not there is not kept. */
    applyCpu(cs, 4, 50);
    UT_ASSERT(!clientSimGetBotHostRow(cs, 4)->connected);
    applyStatus(cs, 4, true, "Desk");
    UT_ASSERT(clientSimGetBotHostRow(cs, 4)->cpu == BOT_HOST_CPU_UNKNOWN);

    /* The host goes: the percent goes with it, and the next host in that
     * row starts with none. */
    applyStatus(cs, 0, false, "");
    UT_ASSERT(!clientSimGetBotHostRow(cs, 0)->connected);
    applyStatus(cs, 0, true, "Other");
    UT_ASSERT(clientSimGetBotHostRow(cs, 0)->cpu == BOT_HOST_CPU_UNKNOWN);

    /* The server's own row works the same way. */
    applyStatus(cs, CLIENT_BOT_HOST_SERVER_ROW, true, "Server");
    applyCpu(cs, CLIENT_BOT_HOST_SERVER_ROW, 0);
    UT_ASSERT(clientSimGetBotHostRow(cs, CLIENT_BOT_HOST_SERVER_ROW)->cpu == 0);

    /* A row index past the list is ignored. */
    applyCpu(cs, CLIENT_BOT_HOST_ROWS, 5);

    clientSimDestroy(cs);
    return 0;
}
