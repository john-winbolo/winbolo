/*
 * Who wrote a scenario script and when it last changed (scenario_identity.h):
 * the author and updated fields of the manifest, read from a script's own
 * scenario table and from a package's manifest.json, carried to a lobby in
 * each scenario-list row and the details reply, and stated by every shipped
 * script.
 *
 * run_scenario_identity_clean        — the author cleaner drops control
 *                                      characters, invisible format
 *                                      characters (bidi controls,
 *                                      separators, zero-width) and broken
 *                                      UTF-8, trims, and cuts at a
 *                                      character boundary; the updated
 *                                      check takes the one form and real
 *                                      dates only
 * run_scenario_identity_lua          — a script's table: both fields read,
 *                                      a missing one warns and reads as
 *                                      unknown, a bad time warns, a dirty
 *                                      or overlong author is cleaned and
 *                                      warns, one of only zero-width
 *                                      characters reads as unknown, no
 *                                      warning is a problem, and a .lua
 *                                      named directly that is not there
 *                                      reads nothing (-validate exits 1)
 * run_scenario_identity_json         — manifest.json the same way, a value
 *                                      of the wrong type warns, the writer
 *                                      writes the pair back and leaves an
 *                                      unstated one out, and a table that
 *                                      states a different pair disagrees
 * run_scenario_identity_blob         — the blob's writer and reader, and
 *                                      the bytes a reader refuses
 * run_scenario_identity_list_wire    — a scenario-list chunk with an
 *                                      identity in each row, decoded back,
 *                                      and a row whose identity is cut
 *                                      short is not taken
 * run_scenario_identity_details_fetch — a mod in the scenarios directory,
 *                                      asked for over the real loopback
 *                                      transport under loss, arrives with
 *                                      its author and updated time beside
 *                                      its details, and the listen host's
 *                                      own lookup answers the same
 * run_scenario_identity_details_extra — a found reply with one byte after
 *                                      its identity is not taken: the file
 *                                      reads as no details, while the same
 *                                      reply without that byte is found
 * run_scenario_identity_shipped      — every shipped script, data/mods and
 *                                      the data/maps scenario scripts, says
 *                                      author "WinBolo" and a valid updated
 *
 * Reads the ClientSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"        /* the scenario-list accumulator */
#include "client_connect_state.h"
#include "client_net.h"                 /* clientSimGetConnectState */
#include "server_sim.h"
#include "transport_udp.h"              /* udpClientHandleLobbyScenarioListRsp */
#include "transport_udp_internal.h"     /* PACKET_HEADER_SIZE, UDP_MAX_PAYLOAD */
#include "transport_udp_server_internal.h" /* udpServerPackScenarioListChunk,
                                              udpServer.bulkSend */
#include "bulk_transfer.h"              /* BULK_KIND_SCENARIO_DETAILS */
#include "scenario_defs.h"              /* ScnDirEntry */
#include "scenario_dir.h"
#include "scenario_host.h"
#include "scenario_identity.h"
#include "scenario_issues.h"
#include "scenario_manifest.h"
#include "scenario_manifest_json.h"
#include "scenario_validate.h"
#include "threads.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* The repository, so the shipped scripts are found whatever the working
 * directory is. CMake passes the absolute path; the fallback is the path
 * from the source root, for a run started there. */
#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

#define SI_AUTHOR  "Jo Bloggs"
#define SI_UPDATED "2026-10-03T20:47Z"

/* ── Helpers ──────────────────────────────────────────────────────── */

/* The warning under key, or NULL when the result holds none. */
static const ScnValidateIssue *siWarning(const ScnValidateResult *r,
                                         const char *key) {
    uint16_t i;

    for (i = 0; i < r->warnCount; i++) {
        if (strcmp(r->warnings[i].key, key) == 0) return &r->warnings[i];
    }
    return NULL;
}

/* Is any problem (not a warning) about author or updated? */
static bool siIssueAboutIdentity(const ScnValidateResult *r) {
    uint16_t i;

    for (i = 0; i < r->count; i++) {
        if (strcmp(r->issues[i].key, "author") == 0 ||
            strcmp(r->issues[i].key, "updated") == 0) {
            return true;
        }
    }
    return false;
}

/* text checked the way -validate checks a loose .lua, into a result the
 * caller frees. NULL when the result could not be allocated. */
static ScnValidateResult *siValidate(const char *text, const char *name) {
    ScnValidateResult *r = (ScnValidateResult *)calloc(1, sizeof(*r));

    if (r == NULL) return NULL;
    (void)scenarioValidateSource(NULL, text, strlen(text), name, NULL, r);
    return r;
}

/* ── 1. The cleaner and the form ──────────────────────────────────── */

int run_scenario_identity_clean(void) {
    char out[SCN_AUTHOR_LEN];
    char tight[8];
    char text[32];
    char longer[80];

    /* Plain text is kept as it is, and says so. */
    UT_ASSERT(scnIdentityCleanAuthor(out, sizeof(out), "WinBolo", 7));
    UT_ASSERT(strcmp(out, "WinBolo") == 0);
    /* UTF-8 is text, not a fault. */
    UT_ASSERT(scnIdentityCleanAuthor(out, sizeof(out), "J\xC3\xB6rg", 5));
    UT_ASSERT(strcmp(out, "J\xC3\xB6rg") == 0);

    /* Control characters out, NUL and DEL and C1 included. */
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out), "a\nb\0c\x7F" "d",
                                      7));
    UT_ASSERT_MSG(strcmp(out, "abcd") == 0, "cleaned to '%s'", out);
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out), "x\xC2\x85y", 4));
    UT_ASSERT_MSG(strcmp(out, "xy") == 0, "a C1 control was kept: '%s'", out);
    /* Broken UTF-8 out, byte by byte: a stray continuation, a lead byte with
       no continuation, an overlong form and a surrogate. */
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out),
                                      "a\x80" "b\xC3" "c\xC0\xAF" "d"
                                      "\xED\xA0\x80" "e", 12));
    UT_ASSERT_MSG(strcmp(out, "abcde") == 0, "cleaned to '%s'", out);
    /* Spaces at the ends trimmed, inner ones kept. */
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out), "  Jo  Bloggs ", 13));
    UT_ASSERT_MSG(strcmp(out, "Jo  Bloggs") == 0, "trimmed to '%s'", out);
    /* Only control characters and spaces: nothing left. */
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out), " \t ", 3));
    UT_ASSERT(out[0] == '\0');

    /* Invisible format characters out: a terminal applies the bidi ones,
       which could reorder the rest of a DS log line. Each is put between
       an 'a' and a 'b'. */
    {
        static const char *const fmt[] = {
            "\xD8\x9C",                                   /* U+061C ALM */
            "\xE2\x80\x8B", "\xE2\x80\x8C", "\xE2\x80\x8D", /* ZWSP..ZWJ */
            "\xE2\x80\x8E", "\xE2\x80\x8F",                 /* LRM RLM */
            "\xE2\x80\xA8", "\xE2\x80\xA9",                 /* LS PS */
            "\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAC", /* LRE RLE PDF */
            "\xE2\x80\xAD", "\xE2\x80\xAE",                 /* LRO RLO */
            "\xE2\x81\xA0",                               /* WJ */
            "\xE2\x81\xA6", "\xE2\x81\xA7", "\xE2\x81\xA8", /* LRI RLI FSI */
            "\xE2\x81\xA9",                               /* PDI */
            "\xEF\xBB\xBF",                               /* BOM / ZWNBSP */
        };
        char   in[16];
        size_t k;

        for (k = 0; k < sizeof(fmt) / sizeof(fmt[0]); k++) {
            snprintf(in, sizeof(in), "a%sb", fmt[k]);
            UT_ASSERT_MSG(!scnIdentityCleanAuthor(out, sizeof(out), in,
                                                  strlen(in)),
                          "format character %u was not reported",
                          (unsigned)k);
            UT_ASSERT_MSG(strcmp(out, "ab") == 0,
                          "format character %u kept: '%s'", (unsigned)k, out);
        }
    }
    /* Their neighbours are text and stay: U+200A hair space, U+2010
       hyphen, U+202F narrow no-break space, U+061B Arabic semicolon. */
    UT_ASSERT(scnIdentityCleanAuthor(out, sizeof(out),
                                     "a\xE2\x80\x8A" "b\xE2\x80\x90" "c"
                                     "\xE2\x80\xAF" "d\xD8\x9B", 15));
    UT_ASSERT(strcmp(out, "a\xE2\x80\x8A" "b\xE2\x80\x90" "c\xE2\x80\xAF"
                          "d\xD8\x9B") == 0);
    /* A reordered name: the override goes, the letters stay in file
       order. */
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out),
                                      "Jo\xE2\x80\xAE" "olleh.lua", 14));
    UT_ASSERT_MSG(strcmp(out, "Joolleh.lua") == 0, "cleaned to '%s'", out);
    /* Only zero-width characters: nothing left, so the author is
       unknown rather than "by " and an empty name. */
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out),
                                      "\xE2\x80\x8B\xEF\xBB\xBF\xE2\x81\xA0",
                                      9));
    UT_ASSERT(out[0] == '\0');

    /* Too long: cut at the cap, and never inside a character. Seven bytes
       of room, and "ab" then three two-byte letters is eight. */
    UT_ASSERT(!scnIdentityCleanAuthor(tight, sizeof(tight),
                                      "ab\xC3\xB6\xC3\xB6\xC3\xB6", 8));
    UT_ASSERT_MSG(strcmp(tight, "ab\xC3\xB6\xC3\xB6") == 0,
                  "cut to '%s' (%u bytes)", tight, (unsigned)strlen(tight));
    memset(longer, 'x', sizeof(longer) - 1);
    longer[sizeof(longer) - 1] = '\0';
    UT_ASSERT(!scnIdentityCleanAuthor(out, sizeof(out), longer,
                                      strlen(longer)));
    UT_ASSERT_MSG(strlen(out) == SCN_AUTHOR_LEN - 1, "cut to %u bytes",
                  (unsigned)strlen(out));

    /* The one form, and real minutes only. */
    UT_ASSERT(scnIdentityUpdatedValid("2026-10-03T20:47Z"));
    UT_ASSERT(scnIdentityUpdatedValid("2024-02-29T00:00Z"));   /* leap */
    UT_ASSERT(scnIdentityUpdatedValid("2000-02-29T23:59Z"));   /* 400 */
    UT_ASSERT(!scnIdentityUpdatedValid("1900-02-29T12:00Z"));  /* 100 */
    UT_ASSERT(!scnIdentityUpdatedValid("2026-02-29T12:00Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-04-31T12:00Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-13-01T12:00Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-00-01T12:00Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-00T12:00Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-03T24:00Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-03T20:60Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-03T20:47"));     /* no Z */
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-03T20:47:00Z")); /* secs */
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-03 20:47Z"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-03T20:47+00"));
    UT_ASSERT(!scnIdentityUpdatedValid("2026-10-03"));
    UT_ASSERT(!scnIdentityUpdatedValid(""));
    UT_ASSERT(!scnIdentityUpdatedValid(NULL));

    /* As a person reads it, and "" for a value that is not one. */
    scnIdentityUpdatedText("2026-10-03T20:47Z", text, sizeof(text));
    UT_ASSERT_MSG(strcmp(text, "2026-10-03 20:47 UTC") == 0, "shown as '%s'",
                  text);
    scnIdentityUpdatedText("yesterday", text, sizeof(text));
    UT_ASSERT(text[0] == '\0');
    return 0;
}

/* ── 2. A script's own table ──────────────────────────────────────── */

int run_scenario_identity_lua(void) {
    ScnValidateResult      *r;
    const ScnValidateIssue *w;
    char                    script[1024];
    char                    longAuthor[80];

    /* Both stated: read, and nothing to warn about. */
    r = siValidate("scenario = { name = \"Both\", api = 1, kind = \"mod\",\n"
                   "  bound = false, author = \"" SI_AUTHOR "\",\n"
                   "  updated = \"" SI_UPDATED "\" }\n",
                   "both.lua");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(strcmp(r->manifest.author, SI_AUTHOR) == 0,
                  "author read as '%s'", r->manifest.author);
    UT_ASSERT_MSG(strcmp(r->manifest.updated, SI_UPDATED) == 0,
                  "updated read as '%s'", r->manifest.updated);
    UT_ASSERT_MSG(siWarning(r, "author") == NULL &&
                      siWarning(r, "updated") == NULL,
                  "%u warnings for a script that states both",
                  (unsigned)r->warnCount);
    free(r);

    /* Neither stated: unknown, one warning each, and still no problem. */
    r = siValidate("scenario = { name = \"Neither\", api = 1, kind = \"mod\",\n"
                   "  bound = false }\n",
                   "neither.lua");
    UT_ASSERT(r != NULL);
    UT_ASSERT(r->manifest.author[0] == '\0' && r->manifest.updated[0] == '\0');
    UT_ASSERT_MSG(siWarning(r, "author") != NULL,
                  "no warning for a missing author");
    UT_ASSERT_MSG(siWarning(r, "updated") != NULL,
                  "no warning for a missing updated");
    UT_ASSERT_MSG(!siIssueAboutIdentity(r) && r->count == 0,
                  "a missing author or updated was a problem (%u problems)",
                  (unsigned)r->count);
    free(r);

    /* A time that is not one: unknown, and the warning quotes it. */
    r = siValidate("scenario = { name = \"Bad\", api = 1, kind = \"mod\",\n"
                   "  bound = false, author = \"A\",\n"
                   "  updated = \"2026-02-30T10:00Z\" }\n",
                   "bad_time.lua");
    UT_ASSERT(r != NULL);
    UT_ASSERT(strcmp(r->manifest.author, "A") == 0);
    UT_ASSERT_MSG(r->manifest.updated[0] == '\0',
                  "a date the calendar lacks was kept: '%s'",
                  r->manifest.updated);
    w = siWarning(r, "updated");
    UT_ASSERT_MSG(w != NULL && strstr(w->message, "2026-02-30T10:00Z") != NULL,
                  "the bad time was not named: %s",
                  w != NULL ? w->message : "(no warning)");
    UT_ASSERT(siWarning(r, "author") == NULL);
    UT_ASSERT(!siIssueAboutIdentity(r));
    free(r);

    /* A number where a string goes: a warning, not a problem. */
    r = siValidate("scenario = { name = \"Typed\", api = 1, kind = \"mod\",\n"
                   "  bound = false, author = 7, updated = 202610032047 }\n",
                   "typed.lua");
    UT_ASSERT(r != NULL);
    UT_ASSERT(r->manifest.author[0] == '\0' && r->manifest.updated[0] == '\0');
    UT_ASSERT(siWarning(r, "author") != NULL);
    UT_ASSERT(siWarning(r, "updated") != NULL);
    UT_ASSERT(!siIssueAboutIdentity(r));
    free(r);

    /* Control characters, a NUL among them, and spaces at the ends: what is
       shown is cleaned, and the warning says what was used. */
    r = siValidate("scenario = { name = \"Dirty\", api = 1, kind = \"mod\",\n"
                   "  bound = false, author = \"  Jo\\0\\27[31m\\nBloggs \",\n"
                   "  updated = \"" SI_UPDATED "\" }\n",
                   "dirty.lua");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(strcmp(r->manifest.author, "Jo[31mBloggs") == 0,
                  "the author was cleaned to '%s'", r->manifest.author);
    w = siWarning(r, "author");
    UT_ASSERT_MSG(w != NULL && strstr(w->message, "'Jo[31mBloggs'") != NULL,
                  "the cleaned author was not named: %s",
                  w != NULL ? w->message : "(no warning)");
    UT_ASSERT(!siIssueAboutIdentity(r));
    free(r);

    /* Too long: cut to the cap, and warned about. */
    memset(longAuthor, 'w', sizeof(longAuthor) - 1);
    longAuthor[sizeof(longAuthor) - 1] = '\0';
    snprintf(script, sizeof(script),
             "scenario = { name = \"Long\", api = 1, kind = \"mod\",\n"
             "  bound = false, author = \"%s\",\n"
             "  updated = \"" SI_UPDATED "\" }\n", longAuthor);
    r = siValidate(script, "long.lua");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(strlen(r->manifest.author) == SCN_AUTHOR_LEN - 1,
                  "a %u-byte author was kept as %u bytes",
                  (unsigned)strlen(longAuthor),
                  (unsigned)strlen(r->manifest.author));
    UT_ASSERT(siWarning(r, "author") != NULL);
    UT_ASSERT(!siIssueAboutIdentity(r));
    free(r);

    /* Only zero-width characters and a bidi override: empty once cleaned,
       so unknown, with the empty-author warning, and still no problem. */
    r = siValidate("scenario = { name = \"Ghost\", api = 1, kind = \"mod\",\n"
                   "  bound = false,\n"
                   "  author = \"\xE2\x80\x8B\xE2\x80\xAE\xEF\xBB\xBF\",\n"
                   "  updated = \"" SI_UPDATED "\" }\n",
                   "ghost.lua");
    UT_ASSERT(r != NULL);
    UT_ASSERT_MSG(r->manifest.author[0] == '\0',
                  "an invisible author was kept as '%s'", r->manifest.author);
    w = siWarning(r, "author");
    UT_ASSERT_MSG(w != NULL && strstr(w->message, "empty") != NULL,
                  "no empty-author warning: %s",
                  w != NULL ? w->message : "(no warning)");
    UT_ASSERT(!siIssueAboutIdentity(r));
    free(r);

    /* A .lua named directly that is not there: nothing read, no manifest
       and no problem, which is what WinBoloDS -validate turns into exit 1
       for a direct .lua (a typo in a path must not pass a check). */
    r = (ScnValidateResult *)calloc(1, sizeof(*r));
    UT_ASSERT(r != NULL);
    (void)scenarioValidateScript(NULL, "wbtest_no_such_script_typo.lua", r);
    UT_ASSERT_MSG(!r->haveManifest && r->count == 0,
                  "a missing script read as manifest %d, %u problems",
                  (int)r->haveManifest, (unsigned)r->count);
    free(r);
    return 0;
}

/* ── 3. A package's manifest.json ─────────────────────────────────── */

static ScnManifestDoc *siParse(const char *json, ScnValidateResult *sink) {
    ScnParseReport rep;
    char           err[256];

    memset(&rep, 0, sizeof(rep));
    rep.sink = sink;
    err[0]   = '\0';
    return scnManifestParse((const uint8_t *)json, strlen(json), &rep, err,
                            sizeof(err));
}

int run_scenario_identity_json(void) {
    ScnValidateResult      *r;
    ScnManifestDoc         *doc;
    ScnManifestDoc         *again;
    const ScenarioManifest *m;
    ScenarioManifest       *lua;
    char                   *text;
    char                    err[256];
    char                    key[64];

    r = (ScnValidateResult *)calloc(1, sizeof(*r));
    UT_ASSERT(r != NULL);

    /* Both stated. */
    doc = siParse("{ \"manifest\": 1, \"api\": 1, \"name\": \"Packed\",\n"
                  "  \"bound\": false, \"script\": \"main.lua\",\n"
                  "  \"author\": \"" SI_AUTHOR "\",\n"
                  "  \"updated\": \"" SI_UPDATED "\" }\n", r);
    UT_ASSERT_MSG(doc != NULL, "the manifest was refused");
    m = scnManifestValues(doc);
    UT_ASSERT_MSG(strcmp(m->author, SI_AUTHOR) == 0, "author read as '%s'",
                  m->author);
    UT_ASSERT_MSG(strcmp(m->updated, SI_UPDATED) == 0, "updated read as '%s'",
                  m->updated);
    UT_ASSERT_MSG(r->warnCount == 0, "%u warnings, the first '%s'",
                  (unsigned)r->warnCount,
                  r->warnCount > 0 ? r->warnings[0].message : "");

    /* Written back, and read back the same. */
    text = scnManifestWrite(doc, err, sizeof(err));
    UT_ASSERT_MSG(text != NULL, "the manifest would not write: %s", err);
    UT_ASSERT(strstr(text, "\"author\"") != NULL);
    UT_ASSERT(strstr(text, SI_UPDATED) != NULL);
    again = siParse(text, NULL);
    free(text);
    UT_ASSERT(again != NULL);
    UT_ASSERT(strcmp(scnManifestValues(again)->author, SI_AUTHOR) == 0);
    UT_ASSERT(strcmp(scnManifestValues(again)->updated, SI_UPDATED) == 0);
    scnManifestFree(again);

    /* A table that states the same pair agrees, one that states another
       does not, and one that states none leaves it to the manifest. */
    lua = (ScenarioManifest *)calloc(1, sizeof(*lua));
    UT_ASSERT(lua != NULL);
    *lua = *m;
    UT_ASSERT(scnManifestAgrees(m, lua, key, sizeof(key), err, sizeof(err)));
    snprintf(lua->author, sizeof(lua->author), "%s", "Someone Else");
    key[0] = '\0';
    UT_ASSERT_MSG(!scnManifestAgrees(m, lua, key, sizeof(key), err,
                                     sizeof(err)),
                  "a different author agreed");
    UT_ASSERT_MSG(strcmp(key, "author") == 0, "the disagreement was on '%s'",
                  key);
    lua->author[0] = '\0';
    snprintf(lua->updated, sizeof(lua->updated), "%s", "2020-01-01T00:00Z");
    key[0] = '\0';
    UT_ASSERT(!scnManifestAgrees(m, lua, key, sizeof(key), err, sizeof(err)));
    UT_ASSERT_MSG(strcmp(key, "updated") == 0, "the disagreement was on '%s'",
                  key);
    lua->updated[0] = '\0';
    UT_ASSERT_MSG(scnManifestAgrees(m, lua, key, sizeof(key), err,
                                    sizeof(err)),
                  "a table that states neither disagreed on %s: %s", key, err);
    free(lua);
    scnManifestFree(doc);

    /* Neither stated: unknown, a warning each, and nothing written back. */
    memset(r, 0, sizeof(*r));
    doc = siParse("{ \"manifest\": 1, \"api\": 1, \"name\": \"Bare\",\n"
                  "  \"bound\": false, \"script\": \"main.lua\" }\n", r);
    UT_ASSERT(doc != NULL);
    m = scnManifestValues(doc);
    UT_ASSERT(m->author[0] == '\0' && m->updated[0] == '\0');
    UT_ASSERT(siWarning(r, "author") != NULL);
    UT_ASSERT(siWarning(r, "updated") != NULL);
    UT_ASSERT_MSG(r->count == 0, "%u problems for a manifest with no author",
                  (unsigned)r->count);
    text = scnManifestWrite(doc, err, sizeof(err));
    UT_ASSERT(text != NULL);
    UT_ASSERT_MSG(strstr(text, "\"author\"") == NULL &&
                      strstr(text, "\"updated\"") == NULL,
                  "an unstated pair was written: %s", text);
    free(text);
    scnManifestFree(doc);

    /* The wrong types, a bad time and a dirty author: warnings, no
       refusal. */
    memset(r, 0, sizeof(*r));
    doc = siParse("{ \"manifest\": 1, \"api\": 1, \"name\": \"Odd\",\n"
                  "  \"bound\": false, \"script\": \"main.lua\",\n"
                  "  \"author\": [\"x\"], \"updated\": 5 }\n", r);
    UT_ASSERT_MSG(doc != NULL, "a manifest with odd identity was refused");
    m = scnManifestValues(doc);
    UT_ASSERT(m->author[0] == '\0' && m->updated[0] == '\0');
    UT_ASSERT(siWarning(r, "author") != NULL);
    UT_ASSERT(siWarning(r, "updated") != NULL);
    scnManifestFree(doc);

    memset(r, 0, sizeof(*r));
    doc = siParse("{ \"manifest\": 1, \"api\": 1, \"name\": \"Dirty\",\n"
                  "  \"bound\": false, \"script\": \"main.lua\",\n"
                  "  \"author\": \"Jo\\u0007\\tBloggs\",\n"
                  "  \"updated\": \"2026-10-03T20:47:00Z\" }\n", r);
    UT_ASSERT(doc != NULL);
    m = scnManifestValues(doc);
    UT_ASSERT_MSG(strcmp(m->author, "JoBloggs") == 0, "cleaned to '%s'",
                  m->author);
    UT_ASSERT_MSG(m->updated[0] == '\0', "a time with seconds was kept: %s",
                  m->updated);
    UT_ASSERT(siWarning(r, "author") != NULL);
    UT_ASSERT(siWarning(r, "updated") != NULL);
    scnManifestFree(doc);

    free(r);
    return 0;
}

/* ── 4. The blob ──────────────────────────────────────────────────── */

int run_scenario_identity_blob(void) {
    uint8_t blob[SCN_IDENTITY_BLOB_MAX + 4];
    uint8_t bad[8];
    char    author[SCN_AUTHOR_LEN];
    char    updated[SCN_UPDATED_LEN];
    char    full[SCN_AUTHOR_LEN];
    size_t  len;
    int     took;

    /* Round trip, with bytes after the pair left alone. */
    len = scnIdentityBlobWrite(blob, sizeof(blob), SI_AUTHOR, SI_UPDATED);
    UT_ASSERT_MSG(len == 2 + strlen(SI_AUTHOR) + SCN_UPDATED_CHARS,
                  "the blob is %u bytes", (unsigned)len);
    UT_ASSERT(blob[0] == strlen(SI_AUTHOR));
    blob[len] = 0xEE;
    took = scnIdentityBlobRead(blob, len + 1, author, sizeof(author), updated,
                               sizeof(updated));
    UT_ASSERT_MSG(took == (int)len, "the reader took %d of %u bytes", took,
                  (unsigned)len);
    UT_ASSERT(strcmp(author, SI_AUTHOR) == 0);
    UT_ASSERT(strcmp(updated, SI_UPDATED) == 0);

    /* Not stated: two zero lengths, read back as "". */
    len = scnIdentityBlobWrite(blob, sizeof(blob), "", NULL);
    UT_ASSERT(len == 2 && blob[0] == 0 && blob[1] == 0);
    took = scnIdentityBlobRead(blob, len, author, sizeof(author), updated,
                               sizeof(updated));
    UT_ASSERT(took == 2 && author[0] == '\0' && updated[0] == '\0');

    /* The writer writes what a reader would keep: a bad time as none, and a
       dirty author cleaned. */
    len = scnIdentityBlobWrite(blob, sizeof(blob), " A\nB ", "soon");
    UT_ASSERT(len == 4 && blob[0] == 2 && blob[3] == 0);

    /* At its largest, it fits SCN_IDENTITY_BLOB_MAX exactly. */
    memset(full, 'f', sizeof(full) - 1);
    full[sizeof(full) - 1] = '\0';
    len = scnIdentityBlobWrite(blob, SCN_IDENTITY_BLOB_MAX, full, SI_UPDATED);
    UT_ASSERT_MSG(len == SCN_IDENTITY_BLOB_MAX, "the full blob is %u bytes",
                  (unsigned)len);
    /* A buffer one short is refused rather than overrun. */
    UT_ASSERT(scnIdentityBlobWrite(blob, SCN_IDENTITY_BLOB_MAX - 1, full,
                                   SI_UPDATED) == 0);

    /* What a reader refuses: too short, a length past the end, a length
       past the field. */
    UT_ASSERT(scnIdentityBlobRead(blob, 1, author, sizeof(author), updated,
                                  sizeof(updated)) < 0);
    bad[0] = 5;
    bad[1] = 'a';
    UT_ASSERT(scnIdentityBlobRead(bad, 2, author, sizeof(author), updated,
                                  sizeof(updated)) < 0);
    bad[0] = 0;
    bad[1] = SCN_UPDATED_CHARS + 1;
    UT_ASSERT(scnIdentityBlobRead(bad, sizeof(bad), author, sizeof(author),
                                  updated, sizeof(updated)) < 0);
    bad[0] = SCN_AUTHOR_LEN;
    UT_ASSERT(scnIdentityBlobRead(bad, sizeof(bad), author, sizeof(author),
                                  updated, sizeof(updated)) < 0);
    /* An updated of the right length that is not a time is taken as bytes
       and kept as none. */
    memcpy(blob, "\x00\x11" "2026-99-99T99:99Z", 19);
    took = scnIdentityBlobRead(blob, 19, author, sizeof(author), updated,
                               sizeof(updated));
    UT_ASSERT(took == 19 && updated[0] == '\0');
    return 0;
}

/* ── 5. The scenario-list rows ────────────────────────────────────── */

static void siFill(ScnDirEntry *e, const char *file, const char *name,
                   const char *author, const char *updated) {
    memset(e, 0, sizeof(*e));
    SDL_strlcpy(e->file, file, sizeof(e->file));
    SDL_strlcpy(e->name, name, sizeof(e->name));
    SDL_strlcpy(e->author, author, sizeof(e->author));
    SDL_strlcpy(e->updated, updated, sizeof(e->updated));
    e->bound = false;
}

/* A client with a scenario-list request in flight, as the accumulator
 * needs. */
static ClientSim *siFreshClientSim(void) {
    ClientSim *cs = clientSimAlloc();

    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    cs->lobbyScenarioListInFlight = true;
    cs->lobbyScenarioListStarted  = false;
    return cs;
}

int run_scenario_identity_list_wire(void) {
    ScnDirEntry entries[3];
    uint8_t     buf[UDP_MAX_PAYLOAD];
    ClientSim  *cs;
    int         len;
    int         next = 0;

    siFill(&entries[0], "a.lua", "Alpha", SI_AUTHOR, SI_UPDATED);
    siFill(&entries[1], "b.lua", "Beta", "", "");            /* unknown */
    siFill(&entries[2], "c.lua", "Gamma", "WinBolo", "2026-01-31T09:05Z");

    len = udpServerPackScenarioListChunk(buf, (int)sizeof(buf), entries, 3, 0,
                                         &next);
    UT_ASSERT(len > 0 && next == 3);

    /* Read back. */
    cs = siFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len);
    UT_ASSERT(clientSimGetLobbyScenarioListCount(cs) == 3);
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListAuthor(cs, 0), SI_AUTHOR) ==
              0);
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListUpdated(cs, 0),
                     SI_UPDATED) == 0);
    /* Not stated: the lobby shows unknown. */
    UT_ASSERT(clientSimGetLobbyScenarioListAuthor(cs, 1)[0] == '\0');
    UT_ASSERT(clientSimGetLobbyScenarioListUpdated(cs, 1)[0] == '\0');
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListFile(cs, 2), "c.lua") == 0);
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListAuthor(cs, 2), "WinBolo") ==
              0);
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListUpdated(cs, 2),
                     "2026-01-31T09:05Z") == 0);
    /* Out of range answers "". */
    UT_ASSERT(clientSimGetLobbyScenarioListAuthor(cs, 3)[0] == '\0');
    UT_ASSERT(clientSimGetLobbyScenarioListAuthor(cs, -1)[0] == '\0');
    clientSimDestroy(cs);

    /* The last row's identity cut short: the rows in front of it stand and
       that row is not taken. */
    cs = siFreshClientSim();
    UT_ASSERT(cs != NULL);
    udpClientHandleLobbyScenarioListRsp(cs, buf, len - 1);
    UT_ASSERT(clientSimGetLobbyScenarioListCount(cs) == 2);
    UT_ASSERT(strcmp(clientSimGetLobbyScenarioListFile(cs, 1), "b.lua") == 0);
    clientSimDestroy(cs);
    return 0;
}

/* ── 6. The details reply, over the wire ──────────────────────────── */

#define SI_CONNECT_MAX 2000
#define SI_FETCH_MAX   3000   /* re-asks and bulk resends under loss */

static char siDir[256];

static void siRemoveTree(const char *path) {
    char **names;
    int    count = 0;
    int    i;

    names = SDL_GlobDirectory(path, "*", 0, &count);
    if (names != NULL) {
        for (i = 0; i < count; i++) {
            char child[512];

            if (names[i] == NULL || names[i][0] == '\0') continue;
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            remove(child);
        }
        SDL_free(names);
    }
    SDL_RemovePath(path);
}

static bool siWriteText(const char *name, const char *text) {
    char  path[512];
    FILE *f;
    bool  ok;

    snprintf(path, sizeof(path), "%s/%s", siDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    fclose(f);
    return ok;
}

static bool siPredConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool siPredSettled(LoopbackHarness *h, void *user) {
    ClientScnDetailsState st =
        clientSimGetLobbyScenarioDetails(h->cs, (const char *)user, NULL, NULL);

    return st == CLIENT_SCN_DETAILS_FOUND || st == CLIENT_SCN_DETAILS_NONE;
}

static const char kSiMod[] =
    "scenario = {\n"
    "  name = \"Identified\", api = 1, kind = \"mod\", bound = false,\n"
    "  author = \"" SI_AUTHOR "\",\n"
    "  updated = \"" SI_UPDATED "\",\n"
    "  rules = { tank_full_shells = 80 },\n"
    "}\n";

static const char kSiBareMod[] =
    "scenario = { name = \"Anonymous\", api = 1, kind = \"mod\",\n"
    "  bound = false }\n";

int run_scenario_identity_details_fetch(void) {
    LoopbackHarness h;
    ScenarioHost   *slot = NULL;
    const char     *author  = NULL;
    const char     *updated = NULL;
    char            dAuthor[SCN_AUTHOR_LEN];
    char            dUpdated[SCN_UPDATED_LEN];
    bool            found;
    int             at;

    snprintf(siDir, sizeof(siDir), "%s", "wbtest_scenario_identity_fetch");
    siRemoveTree(siDir);
    UT_ASSERT(SDL_CreateDirectory(siDir));
    UT_ASSERT(siWriteText("Identified.lua", kSiMod));
    UT_ASSERT(siWriteText("Anonymous.lua", kSiBareMod));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Asker", /*lobbyMode*/ true,
                                       "loss=5,burst=2", 0x1D3471u),
                  "the harness did not come up");
    if (loopbackHarnessPumpUntil(&h, SI_CONNECT_MAX, siPredConnected, NULL) <
        0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never connected");
    }
    threadsWaitForMutex();
    serverSimSetScenarioDir(h.sim, siDir);
    scenarioHostRegisterScenarioLister(h.sim);
    scenarioHostFollowMap(h.sim, &slot);
    threadsReleaseMutex();

    /* The identity arrives beside the details. */
    clientSimLobbyScenarioDetailsWant(h.cs, "Identified.lua");
    at = loopbackHarnessPumpUntil(&h, SI_FETCH_MAX, siPredSettled,
                                  (void *)"Identified.lua");
    UT_ASSERT_MSG(at >= 0, "no answer within %d pumps", SI_FETCH_MAX);
    UT_ASSERT(clientSimGetLobbyScenarioDetails(h.cs, "Identified.lua", NULL,
                                               NULL) ==
              CLIENT_SCN_DETAILS_FOUND);
    UT_ASSERT_MSG(clientSimGetLobbyScenarioIdentity(h.cs, "Identified.lua",
                                                    &author, &updated),
                  "the details arrived with no identity");
    UT_ASSERT_MSG(strcmp(author, SI_AUTHOR) == 0, "author arrived as '%s'",
                  author);
    UT_ASSERT_MSG(strcmp(updated, SI_UPDATED) == 0, "updated arrived as '%s'",
                  updated);

    /* A file that states neither: known, and both "". */
    clientSimLobbyScenarioDetailsWant(h.cs, "Anonymous.lua");
    at = loopbackHarnessPumpUntil(&h, SI_FETCH_MAX, siPredSettled,
                                  (void *)"Anonymous.lua");
    UT_ASSERT(at >= 0);
    UT_ASSERT(clientSimGetLobbyScenarioIdentity(h.cs, "Anonymous.lua",
                                                &author, &updated));
    UT_ASSERT(author[0] == '\0' && updated[0] == '\0');

    /* The listen host reads the same off the sim. */
    threadsWaitForMutex();
    found = serverSimScenarioIdentity(h.sim, "Identified.lua", dAuthor,
                                      sizeof(dAuthor), dUpdated,
                                      sizeof(dUpdated));
    threadsReleaseMutex();
    UT_ASSERT(found);
    UT_ASSERT(strcmp(dAuthor, SI_AUTHOR) == 0);
    UT_ASSERT(strcmp(dUpdated, SI_UPDATED) == 0);

    UT_ASSERT(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_CONNECTED);
    threadsWaitForMutex();
    if (slot != NULL) scenarioHostDetach(slot);
    threadsReleaseMutex();
    loopbackHarnessStop(&h);
    siRemoveTree(siDir);
    return 0;
}

/* The author the staged replies carry, which no file on the server states,
 * so a found answer with it can only be the staged one. */
#define SI_STAGED_AUTHOR "Staged Reply"

/* The client asks for file and the server's bulk stream to it is then held
 * by a hand-built details reply of len bytes, so the server drops the real
 * request and the staged reply is the answer. The pump count to settle, or
 * -1 when the reply was not staged or no answer came. */
static int siStageDetails(LoopbackHarness *h, int slot, const char *file,
                          const uint8_t *reply, size_t len) {
    BulkStreamHeader sh;
    bool             staged;

    clientSimLobbyScenarioDetailsWant(h->cs, file);
    loopbackHarnessPumpClientOnly(h, 1);   /* the request goes out: ASKED */
    memset(&sh, 0, sizeof(sh));
    sh.kind      = BULK_KIND_SCENARIO_DETAILS;
    sh.gen       = 1;
    sh.totalSize = (uint32_t)len;
    sh.pathLen   = (uint8_t)strlen(file);
    memcpy(sh.path, file, (size_t)sh.pathLen + 1);
    threadsWaitForMutex();
    staged = bulkSenderBegin(&udpServer.bulkSend[slot], &sh, reply,
                             (uint32_t)len);
    threadsReleaseMutex();
    if (!staged) return -1;
    return loopbackHarnessPumpUntil(h, SI_FETCH_MAX, siPredSettled,
                                    (void *)file);
}

static bool siPredSenderIdle(LoopbackHarness *h, void *user) {
    bool idle;

    (void)h;
    threadsWaitForMutex();
    idle = !bulkSenderBusy(&udpServer.bulkSend[*(const int *)user]);
    threadsReleaseMutex();
    return idle;
}

int run_scenario_identity_details_extra(void) {
    LoopbackHarness h;
    ScenarioHost   *slot = NULL;
    uint8_t         details[SCN_DETAILS_MAX];
    uint8_t         reply[BULK_SCN_DETAILS_BLOB_MAX];
    const char     *author  = NULL;
    const char     *updated = NULL;
    size_t          len = 0;
    int             dLen;
    int             idLen;
    int             player;
    int             at;

    snprintf(siDir, sizeof(siDir), "%s", "wbtest_scenario_identity_extra");
    siRemoveTree(siDir);
    UT_ASSERT(SDL_CreateDirectory(siDir));
    UT_ASSERT(siWriteText("Identified.lua", kSiMod));

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Asker", /*lobbyMode*/ true, NULL,
                                       0xE47A0001u),
                  "the harness did not come up");
    if (loopbackHarnessPumpUntil(&h, SI_CONNECT_MAX, siPredConnected, NULL) <
        0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never connected");
    }
    player = clientSimGetMyPlayerNum(h.cs);
    threadsWaitForMutex();
    serverSimSetScenarioDir(h.sim, siDir);
    scenarioHostRegisterScenarioLister(h.sim);
    scenarioHostFollowMap(h.sim, &slot);
    dLen = serverSimScenarioDetails(h.sim, "Identified.lua", details,
                                    sizeof(details));
    threadsReleaseMutex();
    UT_ASSERT(dLen > 0);

    /* [FOUND][detailsLen][details][settingsLen 0][identity], laid out the
       way the server lays it out, with an author the file does not state. */
    reply[len++] = BULK_SCN_DETAILS_FOUND;
    reply[len++] = (uint8_t)(dLen >> 8);
    reply[len++] = (uint8_t)dLen;
    memcpy(reply + len, details, (size_t)dLen);
    len += (size_t)dLen;
    reply[len++] = 0;
    reply[len++] = 0;
    idLen = (int)scnIdentityBlobWrite(reply + len, sizeof(reply) - len - 1,
                                      SI_STAGED_AUTHOR, SI_UPDATED);
    UT_ASSERT(idLen > 0);
    len += (size_t)idLen;

    /* The exact reply is taken, with the staged author. */
    at = siStageDetails(&h, player, "Identified.lua", reply, len);
    UT_ASSERT_MSG(at >= 0, "the exact reply was not staged or not answered");
    UT_ASSERT(clientSimGetLobbyScenarioDetails(h.cs, "Identified.lua", NULL,
                                               NULL) ==
              CLIENT_SCN_DETAILS_FOUND);
    UT_ASSERT(clientSimGetLobbyScenarioIdentity(h.cs, "Identified.lua",
                                                &author, &updated));
    UT_ASSERT_MSG(strcmp(author, SI_STAGED_AUTHOR) == 0,
                  "the answer taken was not the staged one (author '%s')",
                  author);

    /* One byte more after the identity: a broken reply, so no details. The
       server first answers the requests it held back, which the forgotten
       slot does not take. */
    UT_ASSERT(loopbackHarnessPumpUntil(&h, SI_FETCH_MAX, siPredSenderIdle,
                                       &player) >= 0);
    loopbackHarnessPumpUntil(&h, 50, NULL, NULL);
    UT_ASSERT(loopbackHarnessPumpUntil(&h, SI_FETCH_MAX, siPredSenderIdle,
                                       &player) >= 0);
    clientSimLobbyScenarioDetailsForget(h.cs);
    reply[len++] = 0x5A;
    at = siStageDetails(&h, player, "Identified.lua", reply, len);
    UT_ASSERT_MSG(at >= 0, "the long reply was not staged or not answered");
    /* Settled before the first re-ask, so the reply made the slot NONE and
       the client did not give up on it. */
    UT_ASSERT_MSG(at < LOBBY_SCN_DETAILS_TIMEOUT_TICKS,
                  "the long reply settled after %d pumps", at);
    UT_ASSERT_MSG(clientSimGetLobbyScenarioDetails(h.cs, "Identified.lua",
                                                   NULL, NULL) ==
                      CLIENT_SCN_DETAILS_NONE,
                  "a reply with a byte after its identity was taken");
    UT_ASSERT(!clientSimGetLobbyScenarioIdentity(h.cs, "Identified.lua",
                                                 &author, &updated));

    UT_ASSERT(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_CONNECTED);
    threadsWaitForMutex();
    if (slot != NULL) scenarioHostDetach(slot);
    threadsReleaseMutex();
    loopbackHarnessStop(&h);
    siRemoveTree(siDir);
    return 0;
}

/* ── 7. The shipped scripts ───────────────────────────────────────── */

/* Every file in dir matching pattern, checked: the count, or -1 after a
 * failure has been reported. */
static int siCheckShipped(const char *dir, const char *pattern) {
    char **names;
    int    count = 0;
    int    checked = 0;
    int    i;

    names = SDL_GlobDirectory(dir, pattern, 0, &count);
    if (names == NULL) {
        fprintf(stderr, "  FAIL: %s could not be listed\n", dir);
        return -1;
    }
    for (i = 0; i < count; i++) {
        char               path[1024];
        char              *text;
        size_t             len = 0;
        ScnValidateResult *r;
        bool               good;

        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        text = (char *)SDL_LoadFile(path, &len);
        if (text == NULL) {
            fprintf(stderr, "  FAIL: %s could not be read\n", path);
            SDL_free(names);
            return -1;
        }
        r = (ScnValidateResult *)calloc(1, sizeof(*r));
        if (r == NULL) {
            SDL_free(text);
            SDL_free(names);
            return -1;
        }
        (void)scenarioValidateSource(NULL, text, len, path, NULL, r);
        SDL_free(text);
        good = strcmp(r->manifest.author, "WinBolo") == 0 &&
               scnIdentityUpdatedValid(r->manifest.updated) &&
               siWarning(r, "author") == NULL &&
               siWarning(r, "updated") == NULL;
        if (!good) {
            fprintf(stderr, "  FAIL: %s: author '%s', updated '%s'\n", path,
                    r->manifest.author, r->manifest.updated);
            free(r);
            SDL_free(names);
            return -1;
        }
        free(r);
        checked++;
    }
    SDL_free(names);
    return checked;
}

int run_scenario_identity_shipped(void) {
    int mods;
    int maps;

    mods = siCheckShipped(WB_REPO_ROOT_DIR "/data/mods", "*.lua");
    UT_ASSERT(mods >= 0);
    maps = siCheckShipped(WB_REPO_ROOT_DIR "/data/maps", "*.scenario.lua");
    UT_ASSERT(maps >= 0);
    fprintf(stderr, "  shipped scripts checked: %d mods, %d map scripts\n",
            mods, maps);
    UT_ASSERT_MSG(mods > 0 && maps > 0, "no shipped scripts were found");
    return 0;
}
