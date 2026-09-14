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

#ifndef _WIN32
  #include <stdio.h>
  #include <unistd.h>
#endif

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
    int  reloadCalls;
    bool reloadResult;
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

static bool recReloadScenario(char *msg, size_t msgLen) {
    g_rec.reloadCalls++;
    snprintf(msg, msgLen, "%s",
             g_rec.reloadResult ? "reloaded" : "nothing to reload");
    return g_rec.reloadResult;
}

/* Positional, like the server's own list, so a new entry goes last. */
static const ServerConsoleOps kRecOps = {
    recSetLock, recInfo, recSaveMap, recSay,
    recLogSay,  recStatus, recKick,  recSetHost,
    recReloadScenario
};

/* Fresh recorder; both "did it work" ops answer yes unless a test says
 * otherwise. */
static void recReset(void) {
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec.saveMapResult = true;
    g_rec.hostResult = true;
    g_rec.reloadResult = true;
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

/* say sends the message to every player and records it in the replay log.
 * Both copies must read the operator's own capitalisation: the command
 * word is matched against a lower-cased copy of the line, and the message
 * used to be taken from that copy, so "Say Round starts in 5" reached
 * every player as "round starts in 5". */
int run_console_say_keeps_case(void) {
    recReset();
    runLine("Say Round starts in 5\n");
    UT_ASSERT_MSG(g_rec.sayCalls == 1,
                  "say should broadcast once (calls=%d)", g_rec.sayCalls);
    /* The broadcast still carries the line's trailing newline — trimming
     * that is a separate change. */
    UT_ASSERT_MSG(strcmp(g_rec.lastSay, "Round starts in 5\n") == 0,
                  "the broadcast should keep the operator's case, got '%s'",
                  g_rec.lastSay);
    UT_ASSERT_MSG(g_rec.logSayCalls == 1 &&
                  strcmp(g_rec.lastLog, "Round starts in 5") == 0,
                  "the logged message should keep the operator's case, got '%s'",
                  g_rec.lastLog);
    UT_ASSERT_MSG(g_rec.lastLogLen == 17,
                  "the pascal length byte should count the trimmed message, got %d",
                  g_rec.lastLogLen);

    /* A line with no trailing newline logs the whole message. */
    recReset();
    runLine("say GG");
    UT_ASSERT_MSG(g_rec.sayCalls == 1 && strcmp(g_rec.lastSay, "GG") == 0,
                  "say without a newline should broadcast the message, got '%s'",
                  g_rec.lastSay);
    UT_ASSERT_MSG(g_rec.logSayCalls == 1 && g_rec.lastLogLen == 2 &&
                  strcmp(g_rec.lastLog, "GG") == 0,
                  "say without a newline should log the message, got '%s'",
                  g_rec.lastLog);

    /* "say" with no message is not the say command at all — the branch
     * matches "say " — so it reaches nothing. */
    recReset();
    runLine("say\n");
    UT_ASSERT_MSG(g_rec.sayCalls == 0 && g_rec.logSayCalls == 0,
                  "say with no message should send nothing");
    return 0;
}

/* A console line does not always end in a newline: a command piped in
 * without a final one, or stdin at EOF, delivers the line bare. kick and
 * host used to strip the last character unconditionally, which ate the
 * last character of the name — and wrote a NUL one byte in front of the
 * buffer when the name was empty as well (an out-of-bounds write these
 * cases trip under a sanitiser build). */
int run_console_kick_host_without_newline(void) {
    recReset();
    runLine("kick fred");
    UT_ASSERT_MSG(g_rec.kickCalls == 1 && strcmp(g_rec.lastKick, "fred") == 0,
                  "a name with no trailing newline must keep its last "
                  "character, got '%s'", g_rec.lastKick);

    recReset();
    runLine("host fred");
    UT_ASSERT_MSG(g_rec.hostCalls == 1 && strcmp(g_rec.lastHost, "fred") == 0,
                  "a name with no trailing newline must keep its last "
                  "character, got '%s'", g_rec.lastHost);

    /* Nothing after the command word, and no newline to strip. */
    recReset();
    runLine("kick ");
    UT_ASSERT_MSG(g_rec.kickCalls == 1 && g_rec.lastKick[0] == '\0',
                  "an empty kick name should stay empty, got '%s'",
                  g_rec.lastKick);

    recReset();
    runLine("host ");
    UT_ASSERT_MSG(g_rec.hostCalls == 1 && g_rec.lastHost[0] == '\0',
                  "an empty host name should stay empty, got '%s'",
                  g_rec.lastHost);

    /* A line that came through a CRLF pipe loses both bytes. */
    recReset();
    runLine("kick fred\r\n");
    UT_ASSERT_MSG(g_rec.kickCalls == 1 && strcmp(g_rec.lastKick, "fred") == 0,
                  "a CRLF line should not leave a stray return, got '%s'",
                  g_rec.lastKick);

    /* A name longer than the 32-character buffer is truncated, not
     * overrun. */
    recReset();
    runLine("kick 0123456789012345678901234567890123456789\n");
    UT_ASSERT_MSG(g_rec.kickCalls == 1 && strlen(g_rec.lastKick) == 32,
                  "an over-long name should be cut to 32 characters, got %d",
                  (int)strlen(g_rec.lastKick));
    return 0;
}

/* The console reader must tell its caller that stdin has ended.
 *
 * The Linux loop used to throw away the result of fgets. Once stdin hit
 * EOF — a server put in the background without -noinput, or a closed
 * pipe — select() reported the descriptor readable for ever, fgets failed
 * without touching the buffer, and the loop re-ran whatever command it
 * had read last on every pass with no delay: a core burnt, and every
 * player spammed if that command happened to be a say.
 *
 * Windows reads its console on a background thread instead, so the reader
 * (and this test) are POSIX-only. */
int run_console_read_reports_eof(void) {
#ifndef _WIN32
    char path[] = "ut_console_readXXXXXX";
    char buf[SERVER_CONSOLE_LINE];
    ServerConsoleRead r;
    FILE *f;
    int fd;
    int i;

    fd = mkstemp(path);
    UT_ASSERT_MSG(fd >= 0, "could not create a temp file for the reader test");
    f = fdopen(fd, "w+");
    UT_ASSERT_MSG(f != NULL, "could not open the temp file");
    fputs("lock\nSay Hello\n", f);
    fflush(f);
    rewind(f);

    r = serverConsoleReadLine(f, buf, sizeof(buf), 1);
    UT_ASSERT_MSG(r == SERVER_CONSOLE_READ_LINE && strcmp(buf, "lock\n") == 0,
                  "first line should read back, got %d '%s'", (int)r, buf);

    r = serverConsoleReadLine(f, buf, sizeof(buf), 1);
    UT_ASSERT_MSG(r == SERVER_CONSOLE_READ_LINE && strcmp(buf, "Say Hello\n") == 0,
                  "second line should read back, got %d '%s'", (int)r, buf);

    /* End of the stream, and it stays that way — with the buffer cleared,
     * so a caller that dispatches what is in it runs nothing. */
    for (i = 0; i < 3; i++) {
        r = serverConsoleReadLine(f, buf, sizeof(buf), 1);
        UT_ASSERT_MSG(r == SERVER_CONSOLE_READ_EOF,
                      "read %d past the end should report EOF, got %d", i, (int)r);
        UT_ASSERT_MSG(buf[0] == '\0',
                      "the buffer should be empty at EOF, got '%s'", buf);
    }

    fclose(f);
    remove(path);
#endif
    return 0;
}

/* A console with nothing typed on it times out and leaves an empty
 * buffer, and a pipe whose writer goes away reports EOF rather than
 * handing back the last line again. */
int run_console_read_timeout_then_eof(void) {
#ifndef _WIN32
    int fds[2];
    char buf[SERVER_CONSOLE_LINE];
    ServerConsoleRead r;
    FILE *f;

    UT_ASSERT_MSG(pipe(fds) == 0, "could not create a pipe");
    f = fdopen(fds[0], "r");
    UT_ASSERT_MSG(f != NULL, "could not open the pipe read end");

    /* Nothing written yet. */
    r = serverConsoleReadLine(f, buf, sizeof(buf), 1);
    UT_ASSERT_MSG(r == SERVER_CONSOLE_READ_TIMEOUT,
                  "an idle console should time out, got %d", (int)r);
    UT_ASSERT_MSG(buf[0] == '\0',
                  "a timeout should leave the buffer empty, got '%s'", buf);

    UT_ASSERT_MSG(write(fds[1], "status\n", 7) == 7, "could not write to the pipe");
    r = serverConsoleReadLine(f, buf, sizeof(buf), 1);
    UT_ASSERT_MSG(r == SERVER_CONSOLE_READ_LINE && strcmp(buf, "status\n") == 0,
                  "a written line should read back, got %d '%s'", (int)r, buf);

    /* The writer goes away: the read end is readable at once and stays
     * that way, which is exactly the case the loop used to spin on. */
    close(fds[1]);
    r = serverConsoleReadLine(f, buf, sizeof(buf), 1);
    UT_ASSERT_MSG(r == SERVER_CONSOLE_READ_EOF,
                  "a closed pipe should report EOF, got %d", (int)r);
    UT_ASSERT_MSG(buf[0] == '\0',
                  "EOF should leave the buffer empty, got '%s'", buf);

    fclose(f);
#endif
    return 0;
}
