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
 *       the shown flag, the pop-out row and the layout row go through
 *       the real preferences file under their sections and keys, survive
 *       a reload, and the pop-out row is kept out of the cloud upload.
 *
 *   run_scn_panel_layout_row
 *       the "x,y,scale,bg,text" layout row round-trips with both
 *       opacities, clamps them, reads an old "x,y,scale,alpha" row by
 *       migrating its one opacity, and refuses any other row.
 *
 *   run_scn_opacity_migration
 *       the rule that turns one old opacity into the two new ones:
 *       text = old, background = round(old * 43 / 100), -1 stays -1; the
 *       background percent maps straight onto the backing's alpha, with
 *       the default 43 giving the 110 of 255 the backing always had; and
 *       a preferences file with only the old [WINDOW] key of each window
 *       loads both new values from it, while a file with the new keys
 *       does not read the old one.
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
    scnPanelLayoutBgFormat(70, buf, sizeof(buf));
    UT_ASSERT(prefsSetString(SCN_PANEL_PREFS_BG_SECTION, key, buf));
    scnPanelLayoutFormat(40, 50, 150, 80, buf, sizeof(buf));
    UT_ASSERT(prefsSetString("SCENARIO PANEL", key, buf));
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
    {
        int  lx = 0, ly = 0, ls = 0, lbg = 0, ltext = 0;
        char bgBuf[32];
        prefsGetString("SCENARIO PANEL", key, "", buf, sizeof(buf));
        prefsGetString(SCN_PANEL_PREFS_BG_SECTION, key, "", bgBuf,
                       sizeof(bgBuf));
        UT_ASSERT_MSG(strcmp(buf, "40,50,150,80") == 0,
                      "layout row is '%s' after a reload", buf);
        UT_ASSERT_MSG(scnPanelLayoutParse(buf, bgBuf, &lx, &ly, &ls, &lbg,
                                          &ltext),
                      "layout row '%s' did not read after a reload", buf);
        UT_ASSERT(lx == 40 && ly == 50 && ls == 150 && lbg == 70 &&
                  ltext == 80);
    }

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
    /* The layout row and its background value both sync. */
    prefsDocGetString(up, "SCENARIO PANEL", key, "<absent>", buf,
                      sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "40,50,150,80") == 0,
                  "the layout row is not in the upload: '%s'", buf);
    prefsDocGetString(up, SCN_PANEL_PREFS_BG_SECTION, key, "<absent>", buf,
                      sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "70") == 0,
                  "the background value is not in the upload: '%s'", buf);
    prefsDocFree(up);

    prefsShutdown();
    remove(jsonPath);
    remove(corrupt);
    return 0;
}

int run_scn_panel_layout_row(void) {
    char buf[80];
    char bgBuf[16];
    int  x, y, scale, bg, text;
    int  ox, oy, oscale, oalpha;

    /* The row this code writes is four numbers, the text opacity last, and
       the background is a value of its own. Together they round-trip. */
    scnPanelLayoutFormat(-1, 200, 125, 100, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "-1,200,125,100") == 0, "row is '%s'", buf);
    scnPanelLayoutBgFormat(43, bgBuf, sizeof(bgBuf));
    UT_ASSERT_MSG(strcmp(bgBuf, "43") == 0, "background is '%s'", bgBuf);
    x = y = scale = bg = text = 0;
    UT_ASSERT(scnPanelLayoutParse(buf, bgBuf, &x, &y, &scale, &bg, &text));
    UT_ASSERT(x == -1 && y == 200 && scale == 125 && bg == 43 && text == 100);
    scnPanelLayoutFormat(5, 6, 150, 35, buf, sizeof(buf));
    scnPanelLayoutBgFormat(0, bgBuf, sizeof(bgBuf));
    UT_ASSERT(scnPanelLayoutParse(buf, bgBuf, &x, &y, &scale, &bg, &text));
    UT_ASSERT_MSG(bg == 0 && text == 35, "gave %d,%d", bg, text);

    /* An older client reads the first four numbers with "%d,%d,%d,%d", as
       main and 2.1 do. It gets the text opacity as its one opacity, which is
       the look it had, and not the background percent. */
    scnPanelLayoutFormat(30, 40, 100, 100, buf, sizeof(buf));
    UT_ASSERT(sscanf(buf, "%d,%d,%d,%d", &ox, &oy, &oscale, &oalpha) == 4);
    UT_ASSERT_MSG(ox == 30 && oy == 40 && oscale == 100 && oalpha == 100,
                  "an old client read %d,%d,%d,%d", ox, oy, oscale, oalpha);

    /* Never set stays never set, background value included; out of range
       is clamped. */
    UT_ASSERT(scnPanelLayoutParse("10,20,-1,-1", "-1", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT(scale == -1 && bg == -1 && text == -1);
    UT_ASSERT(scnPanelLayoutParse("10,20,100,-7", "250", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT_MSG(bg == 100 && text == 0, "clamped to %d,%d", bg, text);

    /* A four-number row with no background value, or one that is not a
       whole number, is the row main and 2.1 write: its one opacity gives
       both, so the panel looks as it did. */
    UT_ASSERT(scnPanelLayoutParse("30,40,100,100", "", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT(x == 30 && y == 40 && scale == 100);
    UT_ASSERT_MSG(bg == 43 && text == 100, "old 100 gave %d,%d", bg, text);
    UT_ASSERT(scnPanelLayoutParse("30,40,100,60", NULL, &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT_MSG(bg == 26 && text == 60, "old 60 gave %d,%d", bg, text);
    UT_ASSERT(scnPanelLayoutParse("30,40,100,60", "abc", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT_MSG(bg == 26 && text == 60, "bad bg gave %d,%d", bg, text);
    UT_ASSERT(scnPanelLayoutParse("30,40,100,60", "50x", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT_MSG(bg == 26 && text == 60, "bad bg gave %d,%d", bg, text);
    UT_ASSERT(scnPanelLayoutParse("30,40,100,-1", "", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT(bg == -1 && text == -1);

    /* A five-number row from an earlier build of this change: both
       opacities are its own, and a background value is not read. */
    UT_ASSERT(scnPanelLayoutParse("10,20,100,70,80", "", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT_MSG(x == 10 && y == 20 && scale == 100 && bg == 70 &&
                      text == 80,
                  "five-number row gave %d,%d", bg, text);
    UT_ASSERT(scnPanelLayoutParse("10,20,100,70,80", "15", &x, &y, &scale,
                                  &bg, &text));
    UT_ASSERT_MSG(bg == 70 && text == 80, "five-number row gave %d,%d", bg,
                  text);
    UT_ASSERT(scnPanelLayoutParse("10,20,-1,-1,-1", "", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT(scale == -1 && bg == -1 && text == -1);
    UT_ASSERT(scnPanelLayoutParse("10,20,100,250,-7", "", &x, &y, &scale, &bg,
                                  &text));
    UT_ASSERT_MSG(bg == 100 && text == 0, "clamped to %d,%d", bg, text);

    /* Any other row is refused and the outputs are left alone. */
    x = y = scale = bg = text = 7;
    UT_ASSERT(!scnPanelLayoutParse("", "43", &x, &y, &scale, &bg, &text));
    UT_ASSERT(!scnPanelLayoutParse(NULL, "43", &x, &y, &scale, &bg, &text));
    UT_ASSERT(!scnPanelLayoutParse("1,2,3", "43", &x, &y, &scale, &bg,
                                   &text));
    UT_ASSERT(!scnPanelLayoutParse("1,2,3,x", "43", &x, &y, &scale, &bg,
                                   &text));
    UT_ASSERT(!scnPanelLayoutParse("1,2,3,4 junk", "43", &x, &y, &scale, &bg,
                                   &text));
    UT_ASSERT(!scnPanelLayoutParse("1,2,3,4,x", "", &x, &y, &scale, &bg,
                                   &text));
    UT_ASSERT(!scnPanelLayoutParse("1,2,3,4,5,6", "", &x, &y, &scale, &bg,
                                   &text));
    UT_ASSERT(!scnPanelLayoutParse("1,2,3,4,5 junk", "", &x, &y, &scale, &bg,
                                   &text));
    UT_ASSERT(x == 7 && y == 7 && scale == 7 && bg == 7 && text == 7);
    return 0;
}

int run_scn_opacity_migration(void) {
    char jsonPath[1024];
    char corrupt[1100];
    char bgBuf[32], textBuf[32], oldBuf[32];
    int  bg, text, v;

    /* The rule, for every old value: text keeps it, background is
       round(old * 43 / 100). */
    for (v = 0; v <= 100; v++) {
        bg = text = -5;
        scnOpacityMigrate(v, &bg, &text);
        UT_ASSERT_MSG(text == v, "old %d gave text %d", v, text);
        UT_ASSERT_MSG(bg == (int)((v * 43.0) / 100.0 + 0.5),
                      "old %d gave background %d", v, bg);
    }
    scnOpacityMigrate(100, &bg, &text);
    UT_ASSERT(bg == SCN_BG_OPACITY_DEFAULT && text == SCN_TEXT_OPACITY_DEFAULT);
    scnOpacityMigrate(50, &bg, &text);
    UT_ASSERT_MSG(bg == 22 && text == 50, "old 50 gave %d,%d", bg, text);
    scnOpacityMigrate(-1, &bg, &text);
    UT_ASSERT(bg == -1 && text == -1);
    scnOpacityMigrate(250, &bg, &text);
    UT_ASSERT(bg == 43 && text == 100);

    /* The background percent is the backing's real alpha. The default is
       the 110 of 255 the backing had before there was a setting, and an
       old value migrated keeps the backing within two steps of what the
       old rule (old * 110 / 100) drew. Two, because the percent is rounded
       to a whole number before the alpha is. */
    UT_ASSERT(scnBackingAlpha(100) == 255);
    UT_ASSERT(scnBackingAlpha(0) == 0);
    UT_ASSERT_MSG(scnBackingAlpha(SCN_BG_OPACITY_DEFAULT) == 110,
                  "default backing alpha is %d",
                  scnBackingAlpha(SCN_BG_OPACITY_DEFAULT));
    UT_ASSERT(scnBackingAlpha(-1) == 110);
    for (v = 0; v <= 100; v++) {
        int oldAlpha = (v * 110 + 50) / 100;
        scnOpacityMigrate(v, &bg, &text);
        UT_ASSERT_MSG(abs(scnBackingAlpha(bg) - oldAlpha) <= 2,
                      "old %d: backing %d, was %d", v, scnBackingAlpha(bg),
                      oldAlpha);
    }

    /* From the strings the [WINDOW] keys hold. */
    scnOpacityFromPrefs("", "", "60", &bg, &text);
    UT_ASSERT_MSG(bg == 26 && text == 60, "old key 60 gave %d,%d", bg, text);
    scnOpacityFromPrefs("", "", "", &bg, &text);
    UT_ASSERT(bg == -1 && text == -1);
    scnOpacityFromPrefs("", "", "-1", &bg, &text);
    UT_ASSERT(bg == -1 && text == -1);
    scnOpacityFromPrefs("90", "", "60", &bg, &text);
    UT_ASSERT_MSG(bg == 90 && text == -1, "new key ignored: %d,%d", bg, text);
    scnOpacityFromPrefs("-1", "35", "60", &bg, &text);
    UT_ASSERT(bg == -1 && text == 35);
    scnOpacityFromPrefs("120", "-3", "", &bg, &text);
    UT_ASSERT(bg == 100 && text == 0);

    /* Through the real preferences file: a player with only the old key of
       each window gets both new values from it after a reload. */
    UT_ASSERT(utScratchPath(jsonPath, sizeof(jsonPath),
                            "scn_opacity_migration.json"));
    snprintf(corrupt, sizeof(corrupt), "%s.corrupt", jsonPath);
    remove(jsonPath);
    remove(corrupt);
    prefsInit(jsonPath);
    UT_ASSERT(prefsSetString("WINDOW", SCN_PANEL_PREFS_OLD_KEY, "100"));
    UT_ASSERT(prefsSetString("WINDOW", SCN_TEXT_PREFS_OLD_KEY, "60"));
    prefsShutdown();

    prefsInit(jsonPath);
    prefsGetString("WINDOW", SCN_PANEL_PREFS_BG_KEY, "", bgBuf, sizeof(bgBuf));
    prefsGetString("WINDOW", SCN_PANEL_PREFS_TEXT_KEY, "", textBuf,
                   sizeof(textBuf));
    prefsGetString("WINDOW", SCN_PANEL_PREFS_OLD_KEY, "", oldBuf,
                   sizeof(oldBuf));
    scnOpacityFromPrefs(bgBuf, textBuf, oldBuf, &bg, &text);
    UT_ASSERT_MSG(bg == 43 && text == 100, "panel gave %d,%d", bg, text);
    prefsGetString("WINDOW", SCN_TEXT_PREFS_BG_KEY, "", bgBuf, sizeof(bgBuf));
    prefsGetString("WINDOW", SCN_TEXT_PREFS_TEXT_KEY, "", textBuf,
                   sizeof(textBuf));
    prefsGetString("WINDOW", SCN_TEXT_PREFS_OLD_KEY, "", oldBuf,
                   sizeof(oldBuf));
    scnOpacityFromPrefs(bgBuf, textBuf, oldBuf, &bg, &text);
    UT_ASSERT_MSG(bg == 26 && text == 60, "text window gave %d,%d", bg, text);

    /* Once the new keys are written, they are what is read. */
    UT_ASSERT(prefsSetString("WINDOW", SCN_TEXT_PREFS_BG_KEY, "80"));
    UT_ASSERT(prefsSetString("WINDOW", SCN_TEXT_PREFS_TEXT_KEY, "90"));
    prefsShutdown();
    prefsInit(jsonPath);
    prefsGetString("WINDOW", SCN_TEXT_PREFS_BG_KEY, "", bgBuf, sizeof(bgBuf));
    prefsGetString("WINDOW", SCN_TEXT_PREFS_TEXT_KEY, "", textBuf,
                   sizeof(textBuf));
    prefsGetString("WINDOW", SCN_TEXT_PREFS_OLD_KEY, "", oldBuf,
                   sizeof(oldBuf));
    scnOpacityFromPrefs(bgBuf, textBuf, oldBuf, &bg, &text);
    UT_ASSERT_MSG(bg == 80 && text == 90, "new keys gave %d,%d", bg, text);

    /* A scenario's layout row, through the real file. A row from main or
       2.1 with no background value is migrated; an earlier build's
       five-number row keeps its own two values; and once this code writes
       both rows, they are what is read. */
    {
        char row[64];
        int  x = 0, y = 0, scale = 0;
        UT_ASSERT(prefsSetString("SCENARIO PANEL", "Old.lua", "30,40,100,60"));
        UT_ASSERT(prefsSetString("SCENARIO PANEL", "Mid.lua",
                                 "10,20,100,70,80"));
        prefsShutdown();
        prefsInit(jsonPath);
        prefsGetString("SCENARIO PANEL", "Old.lua", "", row, sizeof(row));
        prefsGetString(SCN_PANEL_PREFS_BG_SECTION, "Old.lua", "", bgBuf,
                       sizeof(bgBuf));
        UT_ASSERT(scnPanelLayoutParse(row, bgBuf, &x, &y, &scale, &bg, &text));
        UT_ASSERT_MSG(bg == 26 && text == 60, "old row gave %d,%d", bg, text);
        prefsGetString("SCENARIO PANEL", "Mid.lua", "", row, sizeof(row));
        prefsGetString(SCN_PANEL_PREFS_BG_SECTION, "Mid.lua", "", bgBuf,
                       sizeof(bgBuf));
        UT_ASSERT(scnPanelLayoutParse(row, bgBuf, &x, &y, &scale, &bg, &text));
        UT_ASSERT_MSG(bg == 70 && text == 80, "five-number row gave %d,%d", bg,
                      text);

        /* Written again as this code writes it, both migrated values
           stay. */
        scnPanelLayoutBgFormat(26, row, sizeof(row));
        UT_ASSERT(prefsSetString(SCN_PANEL_PREFS_BG_SECTION, "Old.lua", row));
        scnPanelLayoutFormat(30, 40, 100, 60, row, sizeof(row));
        UT_ASSERT(prefsSetString("SCENARIO PANEL", "Old.lua", row));
        prefsShutdown();
        prefsInit(jsonPath);
        prefsGetString("SCENARIO PANEL", "Old.lua", "", row, sizeof(row));
        UT_ASSERT_MSG(strcmp(row, "30,40,100,60") == 0, "row is '%s'", row);
        prefsGetString(SCN_PANEL_PREFS_BG_SECTION, "Old.lua", "", bgBuf,
                       sizeof(bgBuf));
        UT_ASSERT(scnPanelLayoutParse(row, bgBuf, &x, &y, &scale, &bg, &text));
        UT_ASSERT_MSG(bg == 26 && text == 60, "rewritten row gave %d,%d", bg,
                      text);
    }

    prefsShutdown();
    remove(jsonPath);
    remove(corrupt);
    return 0;
}
