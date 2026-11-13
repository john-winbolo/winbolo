/*
 * WbvRender's command line and the frame-to-tick arithmetic
 * (src/tools/wbv_render_args.c). No SDL: the drawing is the log viewer's,
 * which its own tests cover.
 */

#include "test_harness.h"
#include "wbv_render_args.h"

#include <stdarg.h>
#include <string.h>

static bool parse(WbvRenderArgs *out, char *err, size_t errLen, int argc, ...) {
    char *argv[24];
    va_list ap;
    int i;

    argv[0] = "WbvRender";
    va_start(ap, argc);
    for (i = 1; i < argc && i < 24; i++) {
        argv[i] = va_arg(ap, char *);
    }
    va_end(ap);
    return wbvRenderParseArgs(argc, argv, out, err, errLen);
}

int run_wbv_render_args_and_frame_ticks(void) {
    WbvRenderArgs a;
    char err[256];
    uint32_t ms = 0;
    uint32_t f;

    /* Times: seconds, fractions, minutes. */
    UT_ASSERT(wbvRenderParseSeconds("0", &ms) && ms == 0);
    UT_ASSERT(wbvRenderParseSeconds("12", &ms) && ms == 12000);
    UT_ASSERT(wbvRenderParseSeconds("12.5", &ms) && ms == 12500);
    UT_ASSERT(wbvRenderParseSeconds("0.02", &ms) && ms == 20);
    UT_ASSERT(wbvRenderParseSeconds("1.23456", &ms) && ms == 1234);
    UT_ASSERT(wbvRenderParseSeconds("1:05.25", &ms) && ms == 65250);
    UT_ASSERT(!wbvRenderParseSeconds("-1", &ms));
    UT_ASSERT(!wbvRenderParseSeconds("1:75", &ms));
    UT_ASSERT(!wbvRenderParseSeconds("abc", &ms));
    UT_ASSERT(!wbvRenderParseSeconds("1.", &ms));
    UT_ASSERT(!wbvRenderParseSeconds("", &ms));

    /* Frame n is n * 50 / fps ticks after the first frame, rounded down. */
    for (f = 0; f < 1000; f++) {
        UT_ASSERT(wbvRenderFrameTicks(f, 50) == f);
        UT_ASSERT(wbvRenderFrameTicks(f, 25) == f * 2);
    }
    UT_ASSERT(wbvRenderFrameTicks(0, 30) == 0);
    UT_ASSERT(wbvRenderFrameTicks(1, 30) == 1);   /* 33.3 ms -> tick 1 */
    UT_ASSERT(wbvRenderFrameTicks(2, 30) == 3);   /* 66.7 ms -> tick 3 */
    UT_ASSERT(wbvRenderFrameTicks(3, 30) == 5);   /* 100 ms  -> tick 5 */
    UT_ASSERT(wbvRenderFrameTicks(30, 30) == 50); /* one second exactly */
    UT_ASSERT(wbvRenderFrameTicks(60, 60) == 50);
    UT_ASSERT(wbvRenderFrameTicks(5, 0) == 0);
    /* No overflow for a long round at a high rate: 3 hours at 200 fps. */
    UT_ASSERT(wbvRenderFrameTicks(3u * 3600u * 200u, 200) == 3u * 3600u * 50u);

    /* Frame counts: ten seconds is 500 frames at 50, 300 at 30. */
    UT_ASSERT(wbvRenderFrameCount(10000, 50) == 500);
    UT_ASSERT(wbvRenderFrameCount(10000, 30) == 300);
    UT_ASSERT(wbvRenderFrameCount(10010, 50) == 501);
    UT_ASSERT(wbvRenderFrameCount(1, 50) == 1);
    UT_ASSERT(wbvRenderFrameCount(0, 50) == 0);

    /* Defaults. */
    UT_ASSERT(parse(&a, err, sizeof(err), 4, "in.wbv", "-out", "frames"));
    UT_ASSERT(strcmp(a.inPath, "in.wbv") == 0);
    UT_ASSERT(strcmp(a.outDir, "frames") == 0);
    UT_ASSERT(!a.rawOut && a.fps == 50 && a.scale == 1);
    UT_ASSERT(a.fromMs == 0 && !a.haveTo && !a.haveView && a.follow == NULL);
    UT_ASSERT(a.viewW == WBV_RENDER_DEFAULT_W && a.viewH == WBV_RENDER_DEFAULT_H);

    /* Every option. */
    UT_ASSERT(parse(&a, err, sizeof(err), 15, "-out", "-", "-from", "60",
                    "-to", "70.5", "-fps", "30", "-follow", "Bob",
                    "-size", "20", "10", "in.wbv"));
    UT_ASSERT(a.rawOut && a.outDir == NULL);
    UT_ASSERT(a.fromMs == 60000 && a.haveTo && a.toMs == 70500 && a.fps == 30);
    UT_ASSERT(strcmp(a.follow, "Bob") == 0);
    UT_ASSERT(a.viewW == 20 && a.viewH == 10);

    UT_ASSERT(parse(&a, err, sizeof(err), 13, "in.wbv", "-out", "d",
                    "-view", "100", "90", "32", "18", "-scale", "2",
                    "-self", "Ann"));
    UT_ASSERT(a.haveView && a.viewX == 100 && a.viewY == 90);
    UT_ASSERT(a.viewW == 32 && a.viewH == 18 && a.scale == 2);
    UT_ASSERT(strcmp(a.self, "Ann") == 0);

    /* Refusals carry a reason. */
    UT_ASSERT(!parse(&a, err, sizeof(err), 2, "in.wbv"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 3, "-out", "d"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 8, "in.wbv", "-out", "d",
                     "-from", "10", "-to", "5"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 11, "in.wbv", "-out", "d",
                     "-view", "1", "2", "3", "4", "-follow", "Bob"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 6, "in.wbv", "-out", "d",
                     "-view", "1"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 6, "in.wbv", "-out", "d",
                     "-scale", "9"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 5, "in.wbv", "-out", "d", "-bogus"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 5, "a.wbv", "b.wbv", "-out", "d"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 6, "in.wbv", "-out", "d",
                     "-fps", "0"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 6, "in.wbv", "-out", "d",
                     "-fps", "201"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(parse(&a, err, sizeof(err), 6, "in.wbv", "-out", "d",
                    "-fps", "200") && a.fps == 200);
    UT_ASSERT(!parse(&a, err, sizeof(err), 12, "in.wbv", "-out", "d",
                     "-view", "1", "2", "3", "4", "-size", "20", "10"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 6, "in.wbv", "-out", "d",
                     "-follow", ""));
    UT_ASSERT(err[0] != '\0');

    /* A -view must stay on the map: x + w and y + h are 255 or less. The
     * screen code would otherwise move it back and print nothing. */
    UT_ASSERT(!parse(&a, err, sizeof(err), 9, "in.wbv", "-out", "d",
                     "-view", "240", "0", "40", "30"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 9, "in.wbv", "-out", "d",
                     "-view", "0", "230", "40", "30"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(!parse(&a, err, sizeof(err), 9, "in.wbv", "-out", "d",
                     "-view", "255", "0", "1", "1"));
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT(parse(&a, err, sizeof(err), 9, "in.wbv", "-out", "d",
                    "-view", "215", "225", "40", "30"));
    UT_ASSERT(a.viewX == 215 && a.viewY == 225);
    UT_ASSERT(parse(&a, err, sizeof(err), 9, "in.wbv", "-out", "d",
                    "-view", "0", "0", "255", "255"));

    /* -follow: all digits below the player count is a player number, and
     * anything else is a name. WbvRender turns a number into that player's
     * name on each frame until it finds their tank, so a player who joins
     * late is still followed. */
    UT_ASSERT(wbvRenderFollowNumber("3", 16) == 3);
    UT_ASSERT(wbvRenderFollowNumber("0", 16) == 0);
    UT_ASSERT(wbvRenderFollowNumber("15", 16) == 15);
    UT_ASSERT(wbvRenderFollowNumber("07", 16) == 7);
    UT_ASSERT(wbvRenderFollowNumber("16", 16) == -1);
    UT_ASSERT(wbvRenderFollowNumber("123456789012", 16) == -1);
    UT_ASSERT(wbvRenderFollowNumber("Bob", 16) == -1);
    UT_ASSERT(wbvRenderFollowNumber("3a", 16) == -1);
    UT_ASSERT(wbvRenderFollowNumber("a3", 16) == -1);
    UT_ASSERT(wbvRenderFollowNumber("", 16) == -1);
    UT_ASSERT(wbvRenderFollowNumber(NULL, 16) == -1);

    /* Help is a refusal with no reason. */
    UT_ASSERT(!parse(&a, err, sizeof(err), 2, "-h"));
    UT_ASSERT(err[0] == '\0');
    return 0;
}
