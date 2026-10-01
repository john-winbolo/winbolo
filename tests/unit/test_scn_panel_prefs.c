/*
 * The scenario panel's saved state and the lost-window rule.
 *
 *   run_scn_panel_popout_row
 *       the "open,x,y,w,h" row round-trips, sizes are clamped, and a
 *       row that is not five whole numbers is refused without touching
 *       the output.
 *
 *   run_scn_panel_yes_no
 *       the Yes/No words the shown flag and the close-ask setting are
 *       written as read back, and a word that is neither falls back.
 *       This is the "Don't ask again" setting's save and load.
 *
 *   run_scn_panel_rescue
 *       a window on a display stays where it is; a window whose display
 *       has gone, or that shows too little of its top strip, moves onto
 *       the primary display's usable area, centred and cut to fit. The
 *       caller hands in usable bounds, so a window whose top strip sits
 *       in the taskbar band is lost.
 *
 *   run_scn_panel_prefs_sections
 *       the shown flag and the pop-out row go through the real
 *       preferences file under their sections and keys, survive a
 *       reload, and only the shown flag is put in the cloud upload.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scn_panel_prefs.h"
#include "common/prefs.h"
#include "common/prefs_doc.h"
#include "test_harness.h"

int run_scn_panel_popout_row(void) {
    ScnPanelPopout in, out;
    char           buf[80];

    in.open = true;
    in.x = -1800;
    in.y = 40;
    in.w = 300;
    in.h = 300;
    scnPanelPopoutFormat(&in, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "1,-1800,40,300,300") == 0, "row is '%s'", buf);
    memset(&out, 0, sizeof(out));
    UT_ASSERT(scnPanelPopoutParse(buf, &out));
    UT_ASSERT(out.open && out.x == -1800 && out.y == 40 && out.w == 300 &&
              out.h == 300);

    /* Closed, never placed. */
    in.open = false;
    in.x = -1;
    in.y = -1;
    scnPanelPopoutFormat(&in, buf, sizeof(buf));
    UT_ASSERT(scnPanelPopoutParse(buf, &out));
    UT_ASSERT(!out.open && out.x == -1 && out.y == -1);

    /* Sizes are clamped into range. */
    UT_ASSERT(scnPanelPopoutParse("1,0,0,5,99999", &out));
    UT_ASSERT(out.w == SCN_PANEL_POPOUT_MIN_PX);
    UT_ASSERT(out.h == SCN_PANEL_POPOUT_MAX_PX);

    /* Refused rows leave the output alone. */
    out.x = 1234;
    UT_ASSERT(!scnPanelPopoutParse("", &out));
    UT_ASSERT(!scnPanelPopoutParse("1,2,3,4", &out));
    UT_ASSERT(!scnPanelPopoutParse("1,2,3,4,5,6", &out));
    UT_ASSERT(!scnPanelPopoutParse("2,0,0,300,300", &out));
    UT_ASSERT(!scnPanelPopoutParse("yes,0,0,300,300", &out));
    UT_ASSERT(!scnPanelPopoutParse(NULL, &out));
    UT_ASSERT(out.x == 1234);
    return 0;
}

int run_scn_panel_yes_no(void) {
    /* Save then load, both ways. */
    UT_ASSERT(scnPanelYesNoParse(scnPanelYesNoWord(true), false) == true);
    UT_ASSERT(scnPanelYesNoParse(scnPanelYesNoWord(false), true) == false);
    UT_ASSERT(strcmp(scnPanelYesNoWord(true), "Yes") == 0);
    UT_ASSERT(strcmp(scnPanelYesNoWord(false), "No") == 0);

    /* Hand-edited words. */
    UT_ASSERT(scnPanelYesNoParse("yes", false));
    UT_ASSERT(scnPanelYesNoParse("1", false));
    UT_ASSERT(!scnPanelYesNoParse("no", true));
    UT_ASSERT(!scnPanelYesNoParse("0", true));

    /* A missing or unknown word keeps the default: ask. */
    UT_ASSERT(scnPanelYesNoParse("", true));
    UT_ASSERT(scnPanelYesNoParse("maybe", true));
    UT_ASSERT(!scnPanelYesNoParse("maybe", false));
    UT_ASSERT(scnPanelYesNoParse(NULL, true));
    return 0;
}

int run_scn_panel_rescue(void) {
    /* Primary 1920x1080 at the origin, a second display to its left. */
    ScnPanelRect two[2] = { { 0, 0, 1920, 1080 }, { -1920, 0, 1920, 1080 } };
    ScnPanelRect usable = { 0, 0, 1920, 1040 };   /* taskbar at the bottom */
    ScnPanelRect win, out;

    /* On the primary: found. */
    win.x = 100; win.y = 100; win.w = 300; win.h = 300;
    UT_ASSERT(!scnPanelRescueRect(&win, two, 2, &usable, &out));

    /* On the second display while it is there: found. */
    win.x = -1000; win.y = 200;
    UT_ASSERT(!scnPanelRescueRect(&win, two, 2, &usable, &out));

    /* The second display is gone: lost, centred on the primary. */
    UT_ASSERT(scnPanelRescueRect(&win, two, 1, &usable, &out));
    UT_ASSERT(out.w == 300 && out.h == 300);
    UT_ASSERT_MSG(out.x == 810 && out.y == 370, "moved to %d,%d", out.x,
                  out.y);

    /* Straddling both displays: found. */
    win.x = -150; win.y = 10;
    UT_ASSERT(!scnPanelRescueRect(&win, two, 2, &usable, &out));

    /* Only 20 px of the right edge on screen: lost. */
    win.x = -280; win.y = 10;
    UT_ASSERT(scnPanelRescueRect(&win, two, 1, &usable, &out));
    /* Exactly the minimum strip: found. */
    win.x = -(300 - SCN_PANEL_RESCUE_MIN_PX);
    UT_ASSERT(!scnPanelRescueRect(&win, two, 1, &usable, &out));

    /* The title bar above the top of every display: lost, even though the
       bottom of the window shows. */
    win.x = 100; win.y = -200;
    UT_ASSERT(scnPanelRescueRect(&win, two, 2, &usable, &out));
    /* Half the top strip on: found. Less than half: lost. */
    win.y = -(SCN_PANEL_RESCUE_MIN_PX / 2);
    UT_ASSERT(!scnPanelRescueRect(&win, two, 2, &usable, &out));
    win.y = -(SCN_PANEL_RESCUE_MIN_PX / 2) - 1;
    UT_ASSERT(scnPanelRescueRect(&win, two, 2, &usable, &out));

    /* Below the bottom: lost. */
    win.y = 1070;
    UT_ASSERT(scnPanelRescueRect(&win, two, 2, &usable, &out));

    /* A window bigger than the usable area is cut to fit. */
    win.x = 5000; win.y = 0; win.w = 2500; win.h = 1500;
    UT_ASSERT(scnPanelRescueRect(&win, two, 2, &usable, &out));
    UT_ASSERT(out.x == 0 && out.y == 0 && out.w == 1920 && out.h == 1040);

    /* A primary whose usable area does not start at the origin. */
    {
        ScnPanelRect off = { 2560, 0, 1280, 1024 };
        ScnPanelRect offUsable = { 2560, 30, 1280, 994 };
        win.x = 0; win.y = 0; win.w = 200; win.h = 200;
        UT_ASSERT(scnPanelRescueRect(&win, &off, 1, &offUsable, &out));
        UT_ASSERT(out.x == 2560 + 540 && out.y == 30 + 397);
    }

    /* What the caller really passes: each display's usable bounds. The
       primary's taskbar is the bottom 40 px. A window whose top strip
       sits in that band shows 35 px of it on the full display, which
       would count as found, but the taskbar covers it: lost, and moved
       onto the usable area. */
    {
        ScnPanelRect usableTwo[2] = { { 0, 0, 1920, 1040 },
                                      { -1920, 0, 1920, 1080 } };
        win.x = 100; win.y = 1045; win.w = 300; win.h = 300;
        UT_ASSERT_MSG(!scnPanelRescueRect(&win, two, 2, &usable, &out),
                      "full bounds should see 35 px of the strip");
        UT_ASSERT_MSG(scnPanelRescueRect(&win, usableTwo, 2, &usable, &out),
                      "a title bar under the taskbar was counted as found");
        UT_ASSERT_MSG(out.x == 810 && out.y == 370, "moved to %d,%d", out.x,
                      out.y);
        /* Half the strip above the band: found. */
        win.y = 1040 - SCN_PANEL_RESCUE_MIN_PX / 2;
        UT_ASSERT(!scnPanelRescueRect(&win, usableTwo, 2, &usable, &out));
        win.y = 1040 - SCN_PANEL_RESCUE_MIN_PX / 2 + 1;
        UT_ASSERT(scnPanelRescueRect(&win, usableTwo, 2, &usable, &out));
    }

    /* No displays reported: nothing to check against, left alone. */
    win.x = 99999;
    UT_ASSERT(!scnPanelRescueRect(&win, two, 0, &usable, &out));
    return 0;
}

int run_scn_panel_prefs_sections(void) {
    char           jsonPath[1024];
    char           corrupt[1100];
    char           key[128];
    char           popKey[128 + 8];
    char           longName[200];
    char           buf[128];
    char          *body;
    PrefsDoc      *up;
    ScnPanelPopout in, got;

    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath),
                            "scn_panel_prefs.json"));
    snprintf(corrupt, sizeof(corrupt), "%s.corrupt", jsonPath);
    remove(jsonPath);
    remove(corrupt);
    prefsInit(jsonPath);

    /* The keys: the script's name, a control byte made readable, and the
       panel id after a '#'. */
    scnPanelPrefsKey("Survival.scenario.lua", key, sizeof(key));
    UT_ASSERT(strcmp(key, "Survival.scenario.lua") == 0);
    scnPanelPrefsPopoutKey("Survival.scenario.lua", 0, popKey, sizeof(popKey));
    UT_ASSERT_MSG(strcmp(popKey, "Survival.scenario.lua#0") == 0,
                  "pop-out key is '%s'", popKey);
    scnPanelPrefsKey("Bad\tName.lua", buf, sizeof(buf));
    UT_ASSERT(strcmp(buf, "Bad_Name.lua") == 0);

    /* A long name is cut at the same place in both keys, so the rows of
       one script still line up. */
    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    scnPanelPrefsKey(longName, key, sizeof(key));
    scnPanelPrefsPopoutKey(longName, 0, popKey, sizeof(popKey));
    UT_ASSERT(strlen(key) == sizeof(key) - 1);
    UT_ASSERT(strncmp(popKey, key, strlen(key)) == 0);
    UT_ASSERT(strcmp(popKey + strlen(key), "#0") == 0);

    /* Write both rows the way gamefront.c does. */
    scnPanelPrefsKey("Survival.scenario.lua", key, sizeof(key));
    scnPanelPrefsPopoutKey("Survival.scenario.lua", 0, popKey, sizeof(popKey));
    UT_ASSERT(prefsSetString(SCN_PANEL_PREFS_SHOWN_SECTION, key,
                             scnPanelYesNoWord(false)));
    in.open = true;
    in.x = -1500;
    in.y = 60;
    in.w = 320;
    in.h = 280;
    scnPanelPopoutFormat(&in, buf, sizeof(buf));
    UT_ASSERT(prefsSetString(SCN_PANEL_PREFS_POPOUT_SECTION, popKey, buf));
    prefsShutdown();

    /* Reload and read them back. */
    prefsInit(jsonPath);
    prefsGetString(SCN_PANEL_PREFS_SHOWN_SECTION, key, "", buf, sizeof(buf));
    UT_ASSERT_MSG(scnPanelYesNoParse(buf, true) == false,
                  "shown row reads '%s' after a reload", buf);
    prefsGetString(SCN_PANEL_PREFS_POPOUT_SECTION, popKey, "", buf,
                   sizeof(buf));
    memset(&got, 0, sizeof(got));
    UT_ASSERT_MSG(scnPanelPopoutParse(buf, &got),
                  "pop-out row '%s' did not read after a reload", buf);
    UT_ASSERT(got.open && got.x == -1500 && got.y == 60 && got.w == 320 &&
              got.h == 280);

    /* The upload carries the shown flag and never the desktop position. */
    body = prefsSerializeForUpload();
    UT_ASSERT(body != NULL);
    up = prefsDocParseJson(body);
    free(body);
    UT_ASSERT(up != NULL);
    prefsDocGetString(up, SCN_PANEL_PREFS_SHOWN_SECTION, key, "<absent>", buf,
                      sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "No") == 0,
                  "the shown flag is not in the upload: '%s'", buf);
    prefsDocGetString(up, SCN_PANEL_PREFS_POPOUT_SECTION, popKey, "<absent>",
                      buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "<absent>") == 0,
                  "the pop-out position leaked into the upload: '%s'", buf);
    prefsDocFree(up);

    prefsShutdown();
    remove(jsonPath);
    remove(corrupt);
    return 0;
}
