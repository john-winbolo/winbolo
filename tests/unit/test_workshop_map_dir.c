/*
 * The virtual "Workshop" map folder.
 *
 * A desktop host tells its sim where subscribed Workshop items are copied to
 * (serverSimSetWorkshopMapDir). The root of the map listing then offers that
 * directory as a folder named "Workshop", and "Workshop" / "Workshop/<name>"
 * resolve into it, the same way "Uploads" resolves to the persist directory.
 * These cases stand a sim on two scratch directories, one as the map root and
 * one as the Workshop directory, and read the listing and the resolve back.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server/sim/server_sim_shared.h" /* serverSimResolveMapPath */
#include "transport_udp.h" /* transportUdpClientUseLocalRelPath */
#include "test_harness.h"

/* Room for everything these cases put in a directory, and one over. */
#define WMD_MAX_ENTRIES 8

/* The map root and the Workshop directory, both under this test's scratch
   directory. The files in them only need a .map name: the listing reads the
   name and the directory entry, not the bytes. */
static bool wmdMakeDirs(char *root, size_t rootLen, char *ws, size_t wsLen) {
    if (!utScratchPath(root, rootLen, "maps")) return false;
    if (!utScratchPath(ws, wsLen, "workshop")) return false;
    return SDL_CreateDirectory(root) && SDL_CreateDirectory(ws);
}

static bool wmdWriteMap(const char *dir, const char *name) {
    char path[FILENAME_MAX];
    static const char bytes[] = "not a real map";

    SDL_snprintf(path, sizeof(path), "%s/%s", dir, name);
    return SDL_SaveFile(path, bytes, sizeof(bytes) - 1);
}

/* How many entries in the listing are called name, and whether the last one
   found is a folder. */
static int wmdCountNamed(const ServerMapEntry *entries, int count,
                         const char *name, bool *isFolder) {
    int found = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (SDL_strcasecmp(entries[i].name, name) == 0) {
            found++;
            if (isFolder != NULL) *isFolder = entries[i].isFolder;
        }
    }
    return found;
}

int run_workshop_map_dir_offered_at_root(void) {
    char           root[FILENAME_MAX];
    char           ws[FILENAME_MAX];
    ServerMapEntry entries[WMD_MAX_ENTRIES];
    ServerSim     *sim;
    bool           isFolder = false;
    int            got;

    UT_ASSERT(wmdMakeDirs(root, sizeof(root), ws, sizeof(ws)));
    UT_ASSERT(wmdWriteMap(root, "b.map"));
    UT_ASSERT(wmdWriteMap(ws, "a.map"));

    sim = ut_make_running_sim("Workshop");
    UT_ASSERT(sim != NULL);
    serverSimInstallMapDirList(sim, NULL, 0, root);
    serverSimSetWorkshopMapDir(sim, ws);
    UT_ASSERT_MSG(strcmp(serverSimGetWorkshopMapDir(sim), ws) == 0,
                  "getter answered '%s'", serverSimGetWorkshopMapDir(sim));

    /* The root: the map root's own file, and the Workshop folder ahead of it
       because folders sort first. */
    got = serverSimEnumerateMapDir(sim, "", entries, WMD_MAX_ENTRIES);
    UT_ASSERT_MSG(got == 2, "root listing held %d entries", got);
    UT_ASSERT(wmdCountNamed(entries, got, "Workshop", &isFolder) == 1);
    UT_ASSERT(isFolder);
    UT_ASSERT(strcmp(entries[0].name, "Workshop") == 0);
    UT_ASSERT(entries[0].size == 0);
    UT_ASSERT(!entries[0].scripted);
    UT_ASSERT(strcmp(entries[1].name, "b.map") == 0);

    /* NULL is the root as well. */
    got = serverSimEnumerateMapDir(sim, NULL, entries, WMD_MAX_ENTRIES);
    UT_ASSERT(got == 2);
    UT_ASSERT(wmdCountNamed(entries, got, "Workshop", NULL) == 1);

    /* Inside the folder is the Workshop directory's contents. */
    got = serverSimEnumerateMapDir(sim, "Workshop", entries, WMD_MAX_ENTRIES);
    UT_ASSERT_MSG(got == 1, "Workshop listing held %d entries", got);
    UT_ASSERT(strcmp(entries[0].name, "a.map") == 0);
    UT_ASSERT(!entries[0].isFolder);

    /* A listing that is already full does not grow past its cap. */
    got = serverSimEnumerateMapDir(sim, "", entries, 1);
    UT_ASSERT_MSG(got == 1, "a one-entry listing held %d", got);
    UT_ASSERT(strcmp(entries[0].name, "b.map") == 0);

    serverSimDestroy(sim);
    return 0;
}

int run_workshop_map_dir_resolve(void) {
    char           root[FILENAME_MAX];
    char           ws[FILENAME_MAX];
    char           out[FILENAME_MAX];
    char           want[FILENAME_MAX];
    ServerMapEntry entries[WMD_MAX_ENTRIES];
    ServerSim     *sim;

    UT_ASSERT(wmdMakeDirs(root, sizeof(root), ws, sizeof(ws)));
    UT_ASSERT(wmdWriteMap(ws, "a.map"));

    sim = ut_make_running_sim("Workshop");
    UT_ASSERT(sim != NULL);
    serverSimInstallMapDirList(sim, NULL, 0, root);
    serverSimSetWorkshopMapDir(sim, ws);

    serverSimResolveMapPath(sim, "Workshop", out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, ws) == 0, "'Workshop' resolved to '%s'", out);

    serverSimResolveMapPath(sim, "Workshop/a.map", out, sizeof(out));
    SDL_snprintf(want, sizeof(want), "%s/a.map", ws);
    UT_ASSERT_MSG(strcmp(out, want) == 0,
                  "'Workshop/a.map' resolved to '%s'", out);

    /* A name that only starts with the folder's is an ordinary path. */
    serverSimResolveMapPath(sim, "Workshops/a.map", out, sizeof(out));
    SDL_snprintf(want, sizeof(want), "%s/Workshops/a.map", root);
    UT_ASSERT_MSG(strcmp(out, want) == 0,
                  "'Workshops/a.map' resolved to '%s'", out);

    /* Climbing out of the folder is refused before anything resolves. */
    UT_ASSERT(serverSimEnumerateMapDir(sim, "Workshop/../x.map", entries,
                                       WMD_MAX_ENTRIES) == -1);

    /* Cleared, the folder's name is a path under the map root again. */
    serverSimSetWorkshopMapDir(sim, "");
    UT_ASSERT(serverSimGetWorkshopMapDir(sim)[0] == '\0');
    serverSimResolveMapPath(sim, "Workshop/a.map", out, sizeof(out));
    SDL_snprintf(want, sizeof(want), "%s/Workshop/a.map", root);
    UT_ASSERT_MSG(strcmp(out, want) == 0,
                  "cleared, 'Workshop/a.map' resolved to '%s'", out);

    /* And NULL clears it the same way. */
    serverSimSetWorkshopMapDir(sim, ws);
    serverSimSetWorkshopMapDir(sim, NULL);
    serverSimResolveMapPath(sim, "Workshop", out, sizeof(out));
    SDL_snprintf(want, sizeof(want), "%s/Workshop", root);
    UT_ASSERT_MSG(strcmp(out, want) == 0,
                  "cleared by NULL, 'Workshop' resolved to '%s'", out);

    serverSimDestroy(sim);
    return 0;
}

int run_workshop_map_dir_absent(void) {
    char           root[FILENAME_MAX];
    char           ws[FILENAME_MAX];
    char           missing[FILENAME_MAX];
    char           realFolder[FILENAME_MAX];
    char           out[FILENAME_MAX];
    char           want[FILENAME_MAX];
    ServerMapEntry entries[WMD_MAX_ENTRIES];
    ServerSim     *sim;
    bool           isFolder = false;
    int            got;

    UT_ASSERT(wmdMakeDirs(root, sizeof(root), ws, sizeof(ws)));
    UT_ASSERT(wmdWriteMap(root, "b.map"));
    UT_ASSERT(wmdWriteMap(ws, "a.map"));
    UT_ASSERT(utScratchPath(missing, sizeof(missing), "not-there"));

    sim = ut_make_running_sim("Workshop");
    UT_ASSERT(sim != NULL);
    serverSimInstallMapDirList(sim, NULL, 0, root);

    /* Never set: no folder. */
    got = serverSimEnumerateMapDir(sim, "", entries, WMD_MAX_ENTRIES);
    UT_ASSERT(got == 1);
    UT_ASSERT(wmdCountNamed(entries, got, "Workshop", NULL) == 0);

    /* Set to a directory that does not exist, as on a host with nothing
       subscribed yet: still no folder. */
    serverSimSetWorkshopMapDir(sim, missing);
    got = serverSimEnumerateMapDir(sim, "", entries, WMD_MAX_ENTRIES);
    UT_ASSERT(got == 1);
    UT_ASSERT(wmdCountNamed(entries, got, "Workshop", NULL) == 0);

    /* A real folder of that name under the map root is listed once, whether
       or not the Workshop directory is set. */
    SDL_snprintf(realFolder, sizeof(realFolder), "%s/Workshop", root);
    UT_ASSERT(SDL_CreateDirectory(realFolder));

    serverSimSetWorkshopMapDir(sim, NULL);
    got = serverSimEnumerateMapDir(sim, "", entries, WMD_MAX_ENTRIES);
    UT_ASSERT(got == 2);
    UT_ASSERT(wmdCountNamed(entries, got, "Workshop", &isFolder) == 1);
    UT_ASSERT(isFolder);

    serverSimSetWorkshopMapDir(sim, ws);
    got = serverSimEnumerateMapDir(sim, "", entries, WMD_MAX_ENTRIES);
    UT_ASSERT_MSG(got == 2, "root listing held %d entries", got);
    UT_ASSERT(wmdCountNamed(entries, got, "Workshop", &isFolder) == 1);
    UT_ASSERT(isFolder);

    /* The row listed is the real folder, so the path leads there too, the
       way the lobby's chooser opens it, and not to the Workshop
       directory. */
    serverSimResolveMapPath(sim, "Workshop", out, sizeof(out));
    UT_ASSERT_MSG(strcmp(out, realFolder) == 0,
                  "with a real folder, 'Workshop' resolved to '%s'", out);
    serverSimResolveMapPath(sim, "Workshop/x.map", out, sizeof(out));
    SDL_snprintf(want, sizeof(want), "%s/x.map", realFolder);
    UT_ASSERT_MSG(strcmp(out, want) == 0,
                  "with a real folder, 'Workshop/x.map' resolved to '%s'", out);

    UT_ASSERT(wmdWriteMap(realFolder, "x.map"));
    got = serverSimEnumerateMapDir(sim, "Workshop", entries, WMD_MAX_ENTRIES);
    UT_ASSERT_MSG(got == 1, "the real folder listed %d entries", got);
    UT_ASSERT(wmdCountNamed(entries, got, "x.map", NULL) == 1);
    UT_ASSERT_MSG(wmdCountNamed(entries, got, "a.map", NULL) == 0,
                  "listing 'Workshop' read the Workshop directory");

    serverSimDestroy(sim);
    return 0;
}

/* The client's half: the path a map upload's USE_LOCAL pre-check names. A
   map under data/maps is named relative to it, and one in this computer's
   Workshop directory as "Workshop/<name>", which the server resolves into its
   own Workshop directory. Only strings are compared; no file is read. */
int run_workshop_use_local_rel_path(void) {
    char ws[FILENAME_MAX];
    char wsBack[FILENAME_MAX];
    char wsSlash[FILENAME_MAX];
    char path[FILENAME_MAX];
    char rel[256];
    char *p;

    UT_ASSERT(utScratchPath(ws, sizeof(ws), "workshop"));
    SDL_strlcpy(wsBack, ws, sizeof(wsBack));
    for (p = wsBack; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    SDL_snprintf(wsSlash, sizeof(wsSlash), "%s/", ws);

    /* A map under data/maps, as before. */
    UT_ASSERT(transportUdpClientUseLocalRelPath("data/maps/A/b.map", ws,
                                                rel, sizeof(rel)));
    UT_ASSERT_MSG(strcmp(rel, "A/b.map") == 0, "data/maps gave '%s'", rel);

    /* A map in the Workshop directory. */
    SDL_snprintf(path, sizeof(path), "%s/b.map", ws);
    UT_ASSERT(transportUdpClientUseLocalRelPath(path, ws, rel, sizeof(rel)));
    UT_ASSERT_MSG(strcmp(rel, "Workshop/b.map") == 0,
                  "'%s' gave '%s'", path, rel);

    /* The same with Windows separators throughout, in the path and in the
       directory, and with the directory's trailing separator. */
    SDL_snprintf(path, sizeof(path), "%s\\b.map", wsBack);
    UT_ASSERT(transportUdpClientUseLocalRelPath(path, wsBack, rel, sizeof(rel)));
    UT_ASSERT_MSG(strcmp(rel, "Workshop/b.map") == 0,
                  "'%s' gave '%s'", path, rel);
    UT_ASSERT(transportUdpClientUseLocalRelPath(path, wsSlash, rel, sizeof(rel)));
    UT_ASSERT_MSG(strcmp(rel, "Workshop/b.map") == 0,
                  "'%s' against '%s' gave '%s'", path, wsSlash, rel);

    /* An absolute path somewhere else is under neither. */
    UT_ASSERT(!transportUdpClientUseLocalRelPath("/somewhere/else/b.map", ws,
                                                 rel, sizeof(rel)));
    UT_ASSERT_MSG(rel[0] == '\0', "an unrelated path gave '%s'", rel);

    /* A directory that only starts with the Workshop directory's name. */
    SDL_snprintf(path, sizeof(path), "%ss/b.map", ws);
    UT_ASSERT(!transportUdpClientUseLocalRelPath(path, ws, rel, sizeof(rel)));
    UT_ASSERT_MSG(rel[0] == '\0', "'%s' gave '%s'", path, rel);

    /* With no Workshop directory, a Workshop path is not offered. */
    SDL_snprintf(path, sizeof(path), "%s/b.map", ws);
    UT_ASSERT(!transportUdpClientUseLocalRelPath(path, NULL, rel, sizeof(rel)));
    UT_ASSERT(rel[0] == '\0');
    UT_ASSERT(!transportUdpClientUseLocalRelPath(path, "", rel, sizeof(rel)));
    UT_ASSERT(rel[0] == '\0');

    /* The directory itself, and the directory with only a separator after
       it, name no file. */
    UT_ASSERT(!transportUdpClientUseLocalRelPath(ws, ws, rel, sizeof(rel)));
    UT_ASSERT(rel[0] == '\0');
    UT_ASSERT(!transportUdpClientUseLocalRelPath(wsSlash, ws, rel, sizeof(rel)));
    UT_ASSERT(rel[0] == '\0');

    return 0;
}
