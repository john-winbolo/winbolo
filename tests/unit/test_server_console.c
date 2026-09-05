/*
 * Coverage for the dedicated server's operator console (server_console.c).
 *
 * The command set used to be a static function next to main() in
 * servermain.c, which no test binary links. It now parses a line here and
 * reaches the server through ServerConsoleOps, so these tests can drive
 * every command with recording callbacks and no server at all.
 *
 * runLine() feeds a line exactly the way both input loops do: the
 * operator's line is kept verbatim in saveBuff and lower-cased in keyBuff,
 * and the command word is matched against the lower-cased copy.
 */

#include <string.h>

#include "server_console.h"
#include "test_harness.h"

typedef struct {
    int  lockCalls;
    bool lastLock;
    int  infoCalls;
    int  saveMapCalls;
    char lastPath[SERVER_CONSOLE_LINE];
    bool saveMapResult;
    int  sayCalls;
    char lastSay[SERVER_CONSOLE_LINE];
    int  logSayCalls;
    int  lastLogLen;
    char lastLog[256];
    int  statusCalls;
    int  kickCalls;
    char lastKick[64];
    int  hostCalls;
    char lastHost[64];
    bool hostResult;
} ConsoleRec;

static ConsoleRec g_rec;

static void recSetLock(bool locked) {
    g_rec.lockCalls++;
    g_rec.lastLock = locked;
}

static void recInfo(void) { g_rec.infoCalls++; }

static bool recSaveMap(const char *path) {
    g_rec.saveMapCalls++;
    snprintf(g_rec.lastPath, sizeof(g_rec.lastPath), "%s", path);
    return g_rec.saveMapResult;
}

static void recSay(const char *text) {
    g_rec.sayCalls++;
    snprintf(g_rec.lastSay, sizeof(g_rec.lastSay), "%s", text);
}

static void recLogSay(const char *pstr) {
    g_rec.logSayCalls++;
    g_rec.lastLogLen = (unsigned char)pstr[0];
    memcpy(g_rec.lastLog, pstr + 1, (size_t)g_rec.lastLogLen);
    g_rec.lastLog[g_rec.lastLogLen] = '\0';
}

static void recStatus(void) { g_rec.statusCalls++; }

static void recKick(const char *name) {
    g_rec.kickCalls++;
    snprintf(g_rec.lastKick, sizeof(g_rec.lastKick), "%s", name);
}

static bool recSetHost(const char *name) {
    g_rec.hostCalls++;
    snprintf(g_rec.lastHost, sizeof(g_rec.lastHost), "%s", name);
    return g_rec.hostResult;
}

static const ServerConsoleOps kRecOps = {
    recSetLock, recInfo, recSaveMap, recSay,
    recLogSay,  recStatus, recKick,  recSetHost
};

/* Fresh recorder; both "did it work" ops answer yes unless a test says
 * otherwise. */
static void recReset(void) {
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.saveMapResult = true;
    g_rec.hostResult = true;
}

/* Run one line through the dispatcher the way the input loops do. */
static void runLine(const char *line) {
    char keyBuff[SERVER_CONSOLE_LINE];
    char saveBuff[SERVER_CONSOLE_LINE];

    snprintf(saveBuff, sizeof(saveBuff), "%s", line);
    snprintf(keyBuff, sizeof(keyBuff), "%s", line);
    strlower(keyBuff);
    serverConsoleDispatch(&kRecOps, keyBuff, saveBuff);
}

int run_console_lock_unlock(void) {
    recReset();
    runLine("lock\n");
    UT_ASSERT_MSG(g_rec.lockCalls == 1 && g_rec.lastLock == true,
                  "lock should lock the server (calls=%d locked=%d)",
                  g_rec.lockCalls, (int)g_rec.lastLock);

    /* "unlock" must be tested before "lock" or it matches neither. */
    recReset();
    runLine("unlock\n");
    UT_ASSERT_MSG(g_rec.lockCalls == 1 && g_rec.lastLock == false,
                  "unlock should unlock the server (calls=%d locked=%d)",
                  g_rec.lockCalls, (int)g_rec.lastLock);

    /* The operator's capitalisation must not matter. */
    recReset();
    runLine("UnLock\n");
    UT_ASSERT_MSG(g_rec.lockCalls == 1 && g_rec.lastLock == false,
                  "UnLock should unlock the server (calls=%d locked=%d)",
                  g_rec.lockCalls, (int)g_rec.lastLock);
    return 0;
}

int run_console_info_and_status(void) {
    recReset();
    runLine("info\n");
    UT_ASSERT_MSG(g_rec.infoCalls == 1, "info should print game information");

    recReset();
    runLine("status\n");
    UT_ASSERT_MSG(g_rec.statusCalls == 1, "status should list the players");
    return 0;
}

int run_console_savemap_path(void) {
    recReset();
    runLine("savemap mymap\n");
    UT_ASSERT_MSG(g_rec.saveMapCalls == 1 && strcmp(g_rec.lastPath, "mymap.map") == 0,
                  ".map should be appended, got '%s'", g_rec.lastPath);

    recReset();
    runLine("savemap already.map\n");
    UT_ASSERT_MSG(g_rec.saveMapCalls == 1 && strcmp(g_rec.lastPath, "already.map") == 0,
                  ".map must not be doubled, got '%s'", g_rec.lastPath);

    /* Leading whitespace after the command word is skipped, and the path
     * comes from the original-case copy — a lower-cased path would not
     * open on a case-sensitive filesystem. */
    recReset();
    runLine("savemap \t Ashes_To_Ashes\n");
    UT_ASSERT_MSG(g_rec.saveMapCalls == 1 &&
                  strcmp(g_rec.lastPath, "Ashes_To_Ashes.map") == 0,
                  "savemap should keep the operator's case, got '%s'", g_rec.lastPath);

    /* No filename at all is refused rather than saved to "". */
    recReset();
    runLine("savemap\n");
    UT_ASSERT_MSG(g_rec.saveMapCalls == 0,
                  "savemap with no filename should not save (calls=%d)",
                  g_rec.saveMapCalls);

    recReset();
    runLine("savemap    \n");
    UT_ASSERT_MSG(g_rec.saveMapCalls == 0,
                  "savemap with only whitespace should not save (calls=%d)",
                  g_rec.saveMapCalls);
    return 0;
}

int run_console_unknown_command_is_inert(void) {
    recReset();
    runLine("wibble\n");
    UT_ASSERT_MSG(g_rec.lockCalls == 0 && g_rec.infoCalls == 0 &&
                  g_rec.saveMapCalls == 0 && g_rec.sayCalls == 0 &&
                  g_rec.statusCalls == 0 && g_rec.kickCalls == 0 &&
                  g_rec.hostCalls == 0,
                  "an unknown command must not reach the server");

    /* A bare newline (the operator just pressed return) and the empty line
     * the loop supplies on a select() timeout are both no-ops. */
    recReset();
    runLine("\n");
    runLine("");
    UT_ASSERT_MSG(g_rec.lockCalls == 0 && g_rec.infoCalls == 0 &&
                  g_rec.saveMapCalls == 0 && g_rec.sayCalls == 0 &&
                  g_rec.statusCalls == 0 && g_rec.kickCalls == 0 &&
                  g_rec.hostCalls == 0,
                  "an empty line must not reach the server");
    return 0;
}

int run_console_kick_and_host(void) {
    /* kick and host name-match case-insensitively (the help text says so),
     * so both deliberately read the lower-cased copy of the line. */
    recReset();
    runLine("kick Fred\n");
    UT_ASSERT_MSG(g_rec.kickCalls == 1 && strcmp(g_rec.lastKick, "fred") == 0,
                  "kick should pass the name through, got '%s'", g_rec.lastKick);

    recReset();
    runLine("host Fred\n");
    UT_ASSERT_MSG(g_rec.hostCalls == 1 && strcmp(g_rec.lastHost, "fred") == 0,
                  "host should pass the name through, got '%s'", g_rec.lastHost);

    /* No such player: the command reports it and changes nothing. */
    recReset();
    g_rec.hostResult = false;
    runLine("host Nobody\n");
    UT_ASSERT_MSG(g_rec.hostCalls == 1, "host should still be attempted");
    return 0;
}
