/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_settings_workshop
 * Filename:      imgui_settings_workshop.cpp
 * Purpose:
 *   The Workshop section of the Settings dialog. See
 *   imgui_settings_workshop.h.
 *
 *   Subscribed lists the items the sync copied into the
 *   Workshop directory, from its index, and from Steam the
 *   rest: the skins the skin picker reads in place, the
 *   items still downloading, and the installed items the
 *   sync could not use. Publish lists the scripts in the
 *   player's own Mods directory and the maps under
 *   data/maps that carry a scenario, each opening the
 *   shared publish window: a loose script is packed first,
 *   the item is tagged Mod or Scenario from the file's
 *   kind, and the id Steam gives it is written back into
 *   the file that was sent.
 *
 *   The rows are built from directory reads and file reads,
 *   so they are built when the section first draws, on
 *   Refresh, when the sync reports a newly installed item
 *   and after a publish, and never on an ordinary frame.
 *
 *   Compiled into the desktop client only, beside
 *   workshop_sync.c and workshop_publish_modal.cpp.
 *********************************************************/

#include <cstring>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_dialog_utils.h"
#include "lobby/lobby_internal.h"  /* lobbyScenarioKindTag, lobbyDrawNameTag —
                                      the lobby's Mod / Scenario chip */
#include "workshop_publish_modal.h"
#include "imgui_settings_workshop.h"

extern "C" {
#include "../../../common/wb_log.h"
#include "../../lang.h"
#include "../../../steam/steam_wrapper.h"
#include "../skin_source.h"                    /* a subscribed skin's name */
#include "../../../scenario/scenario_host.h"  /* the local listing, a map's
                                                 package, the loose pack */
#include "scenario_chunk.h"                    /* scnIoSetWorkshopId */
#include "server_sim.h"                        /* serverSimNoteScriptDirsChanged */
#include "../workshop_sync.h"
}

/* How many rows either view reads at most. */
#define WS_ROWS_MAX 256

/* Every path this file builds. */
#define WS_PATH_MAX 1024

/* Where the player's maps are, relative to the working directory the client
   runs in, as the map chooser reads them. */
#define WS_MAPS_DIR "data/maps"

/* How many rows the list shows before it scrolls. */
#define WS_LIST_ROWS 8

enum WsView { WS_VIEW_SUBSCRIBED, WS_VIEW_PUBLISH };

/* The chip a row wears. */
enum WsKind {
    WS_KIND_NONE, WS_KIND_SCENARIO, WS_KIND_MOD, WS_KIND_MAP, WS_KIND_SKIN
};

/* Where a subscribed item stands. */
enum WsSubState {
    WS_SUB_INSTALLED,    /* usable where the game looks for it */
    WS_SUB_DOWNLOADING,  /* subscribed, Steam has not delivered it yet */
    WS_SUB_UNUSABLE      /* installed, but the sync could not use it */
};

struct WsSubRow {
    uint64_t   id;
    /* Empty when the item's name is not known: the row shows its id. */
    char       name[SERVER_SCENARIO_NAME_LEN + WORKSHOP_SYNC_FILE_MAX];
    WsKind     kind;
    WsSubState state;
};

struct WsPubRow {
    char     path[WS_PATH_MAX];
    /* What the publish window knows the item by: the file the id is written
       to. For a loose script that is the package it is packed into, not the
       .lua, so reopening the row while the upload runs finds it again after
       the pack has moved the .lua away and the package's own row replaced
       it. */
    char     key[WS_PATH_MAX];
    char     name[SERVER_SCENARIO_NAME_LEN + SERVER_SCENARIO_FILE_LEN];
    char     description[SERVER_SCENARIO_DESC_LEN];
    bool     mod;
    bool     isLua;
    uint64_t id;      /* where the id goes when it is published: for a loose
                         script, the package beside it */
    uint64_t author;
};

/* File scope because both settings shells share the Display tab. */
static WsView                s_view = WS_VIEW_SUBSCRIBED;
static std::vector<WsSubRow> s_subRows;
static std::vector<WsPubRow> s_pubRows;
static bool                  s_subDirty = true;  /* build before next draw */
static bool                  s_pubDirty = true;
static uint32_t              s_syncGen  = 0;

/* The file being published, handed to the two callbacks below. source is the
   row's file; published is what went to Steam, which for a loose script is
   the package it was packed into. */
struct WsPublishCtx {
    char source[WS_PATH_MAX];
    char published[WS_PATH_MAX];
    bool isLua;
};
static WsPublishCtx s_pubCtx;

static void wsMarkDirty(void) {
    s_subDirty = true;
    s_pubDirty = true;
}

/* ── Names ───────────────────────────────────────────────────────── */

static bool wsHasExt(const char *name, const char *ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);

    return n > e && SDL_strcasecmp(name + n - e, ext) == 0;
}

static const char *wsLeaf(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *back  = strrchr(path, '\\');

    if (back != nullptr && (slash == nullptr || back > slash)) slash = back;
    return (slash != nullptr) ? slash + 1 : path;
}

/* The file's name without its extension, for a row whose manifest names
   nothing. */
static void wsStem(const char *file, char *out, size_t outLen) {
    const char *leaf = wsLeaf(file);
    const char *dot  = strrchr(leaf, '.');
    size_t      n    = (dot != nullptr) ? (size_t)(dot - leaf) : strlen(leaf);

    if (n >= outLen) n = outLen - 1;
    memcpy(out, leaf, n);
    out[n] = '\0';
}

/* Update rather than Publish: the item is recorded and this account
   published it. The publish window offers the same choice by the same rule. */
static bool wsOwnItem(uint64_t id, uint64_t author) {
    return id != 0 && author != 0 && author == steam_get_steam_id();
}

/* ── Building the rows ───────────────────────────────────────────── */

static void wsBuildSubscribed(void) {
    std::vector<WorkshopSyncRow>     index(WS_ROWS_MAX);
    std::vector<ServerScenarioEntry> listed(WS_ROWS_MAX);
    char dir[WS_PATH_MAX];
    int  nIndex = workshopSyncIndexRows(index.data(), WS_ROWS_MAX);
    int  nLocal = scenarioHostListLocalScripts(listed.data(), WS_ROWS_MAX);
    bool haveDir = scenarioHostWorkshopDir(dir, sizeof(dir));

    s_subRows.clear();
    for (int i = 0; i < nIndex; i++) {
        const char *file = index[(size_t)i].file;
        WsSubRow    r;

        memset(&r, 0, sizeof(r));
        r.id    = index[(size_t)i].id;
        r.state = WS_SUB_INSTALLED;
        r.kind  = WS_KIND_NONE;
        wsStem(file, r.name, sizeof(r.name));

        if (wsHasExt(file, ".map")) {
            /* A map is a scenario when it carries one, and a map otherwise. */
            char                path[WS_PATH_MAX * 2];
            ServerScenarioEntry e;

            r.kind = WS_KIND_MAP;
            if (haveDir &&
                (size_t)SDL_snprintf(path, sizeof(path), "%s/%s", dir, file) <
                    sizeof(path) &&
                scenarioHostMapPackageInfo(path, &e)) {
                r.kind = e.keepsWinCondition ? WS_KIND_MOD : WS_KIND_SCENARIO;
                if (e.name[0] != '\0') {
                    SDL_strlcpy(r.name, e.name, sizeof(r.name));
                }
            }
        } else {
            /* A .lua or .scenario: the Workshop directory's own row of the
               local listing, which is the manifest's name and kind. */
            for (int j = 0; j < nLocal; j++) {
                const ServerScenarioEntry *e = &listed[(size_t)j];

                if (e->source == SERVER_SCENARIO_SOURCE_WORKSHOP &&
                    strcmp(e->file, file) == 0) {
                    r.kind = e->keepsWinCondition ? WS_KIND_MOD
                                                  : WS_KIND_SCENARIO;
                    if (e->name[0] != '\0') {
                        SDL_strlcpy(r.name, e->name, sizeof(r.name));
                    }
                    break;
                }
            }
        }
        s_subRows.push_back(r);
    }

    /* Every other subscription comes from Steam, so the list holds what the
       overlay does: a skin, which the sync leaves where Steam put it for the
       skin picker; an item still downloading, named by its id until it
       arrives; and an installed item the sync skipped, for having nothing the
       game reads or a file name a lower item already holds (the sync's log
       line says which). An item the player disabled in Steam is not coming
       and is not listed. */
    int count = steam_workshop_subscribed_count();
    for (int i = 0; i < count && (int)s_subRows.size() < WS_ROWS_MAX; i++) {
        char     folder[WS_PATH_MAX];
        uint64_t id = 0;
        bool     indexed = false;
        bool     installed = steam_workshop_item(i, &id, folder, sizeof(folder));

        if (id == 0 || (!installed && steam_workshop_item_disabled(id))) {
            continue;
        }
        for (int j = 0; j < nIndex; j++) {
            if (index[(size_t)j].id == id) {
                indexed = true;
                break;
            }
        }
        if (indexed) continue;

        WsSubRow r;
        memset(&r, 0, sizeof(r));
        r.id    = id;
        r.kind  = WS_KIND_NONE;
        r.state = WS_SUB_DOWNLOADING;
        if (installed) {
            if (workshopSyncClassify(folder, nullptr, 0, nullptr) ==
                WORKSHOP_ITEM_SKIN) {
                /* Named from its skin.ini, as the skin picker names it. */
                SkinSource *skin = skinSourceOpen(folder);
                r.kind  = WS_KIND_SKIN;
                r.state = WS_SUB_INSTALLED;
                if (skin != nullptr) {
                    SkinInfo info;
                    skinSourceReadIni(skin, &info);
                    SDL_strlcpy(r.name, info.name, sizeof(r.name));
                    skinSourceClose(skin);
                }
            } else {
                r.state = WS_SUB_UNUSABLE;
            }
        }
        s_subRows.push_back(r);
    }
    s_subDirty = false;
}

static void wsBuildPublish(void) {
    std::vector<ServerScenarioEntry> listed(WS_ROWS_MAX);
    int n = scenarioHostListLocalScripts(listed.data(), WS_ROWS_MAX);

    s_pubRows.clear();

    /* The player's own Mods directory. A Workshop row is somebody else's
       work, and a bound loose script ships inside its map. */
    for (int i = 0; i < n; i++) {
        const ServerScenarioEntry *e = &listed[(size_t)i];
        WsPubRow                   r;

        if (e->source != SERVER_SCENARIO_SOURCE_SERVER) continue;
        memset(&r, 0, sizeof(r));
        r.isLua = wsHasExt(e->file, ".lua");
        if (r.isLua && e->bound) continue;
        if (!scenarioHostLocalScriptPath(e->file, r.path, sizeof(r.path))) {
            continue;
        }
        if (e->name[0] != '\0') {
            SDL_strlcpy(r.name, e->name, sizeof(r.name));
        } else {
            wsStem(e->file, r.name, sizeof(r.name));
        }
        SDL_strlcpy(r.description, e->description, sizeof(r.description));
        r.mod    = e->keepsWinCondition;
        r.id     = e->workshopId;
        r.author = e->workshopAuthor;
        SDL_strlcpy(r.key, r.path, sizeof(r.key));

        /* A loose script is published as the package it is packed into, so
           the id that counts is the one that package already carries, and
           the package is what the window knows it by. */
        if (r.isLua) {
            char   stem[SERVER_SCENARIO_FILE_LEN];
            char   packed[SERVER_SCENARIO_FILE_LEN + 16];
            size_t dirLen = (size_t)(wsLeaf(r.path) - r.path);

            wsStem(e->file, stem, sizeof(stem));
            SDL_snprintf(packed, sizeof(packed), "%s.scenario", stem);
            SDL_snprintf(r.key, sizeof(r.key), "%.*s%s", (int)dirLen, r.path,
                         packed);
            for (int j = 0; j < n; j++) {
                if (listed[(size_t)j].source == SERVER_SCENARIO_SOURCE_SERVER &&
                    SDL_strcasecmp(listed[(size_t)j].file, packed) == 0) {
                    r.id     = listed[(size_t)j].workshopId;
                    r.author = listed[(size_t)j].workshopAuthor;
                    break;
                }
            }
        }
        s_pubRows.push_back(r);
    }

    /* The maps under data/maps, subfolders included, that carry a scenario.
       A NULL pattern is SDL's walk into subdirectories, which answers
       "sub/X.map" for a map one folder down. */
    int    count = 0;
    char **names = SDL_GlobDirectory(WS_MAPS_DIR, nullptr, 0, &count);
    if (names != nullptr) {
        for (int i = 0; i < count && (int)s_pubRows.size() < WS_ROWS_MAX;
             i++) {
            ServerScenarioEntry e;
            WsPubRow            r;

            if (names[i] == nullptr || !wsHasExt(names[i], ".map")) continue;
            memset(&r, 0, sizeof(r));
            SDL_snprintf(r.path, sizeof(r.path), "%s/%s", WS_MAPS_DIR,
                         names[i]);
            if (!scenarioHostMapPackageInfo(r.path, &e)) continue;
            SDL_strlcpy(r.key, r.path, sizeof(r.key));
            if (e.name[0] != '\0') {
                SDL_strlcpy(r.name, e.name, sizeof(r.name));
            } else {
                wsStem(names[i], r.name, sizeof(r.name));
            }
            SDL_strlcpy(r.description, e.description, sizeof(r.description));
            r.mod    = e.keepsWinCondition;
            r.id     = e.workshopId;
            r.author = e.workshopAuthor;
            s_pubRows.push_back(r);
        }
        SDL_free(names);
    }
    s_pubDirty = false;
}

/* ── The publish window's two callbacks ──────────────────────────── */

/* The one file the item holds, and no preview. A loose script is packed to
   Mods/<stem>.scenario first and its .lua moved to Mods/Sources, and the
   package is what is sent. */
static bool wsBuildContent(void *ctx, const char *folder, char *previewOut,
                           size_t previewLen) {
    WsPublishCtx *c = (WsPublishCtx *)ctx;
    char          dest[WS_PATH_MAX * 2];

    (void)previewLen;
    previewOut[0] = '\0';

    if (c->isLua) {
        char err[512];

        err[0] = '\0';
        if (!scenarioHostPackLooseScript(c->source, c->published,
                                         sizeof(c->published), err,
                                         sizeof(err))) {
            WB_LOG_WARN(WB_LOG_CAT_CLIENT, "workshop: %s could not be packed "
                        "for publishing: %s", c->source, err);
            return false;
        }
        /* The .lua is in Mods/Sources now. A second press in this window, a
           retry or an Update, sends the package rather than packing a path
           that is gone. */
        SDL_strlcpy(c->source, c->published, sizeof(c->source));
        c->isLua = false;
        /* The Mods directory changed under the rows either way. */
        wsMarkDirty();
    } else {
        SDL_strlcpy(c->published, c->source, sizeof(c->published));
    }

    SDL_snprintf(dest, sizeof(dest), "%s/%s", folder, wsLeaf(c->published));
    return workshopPublishCopyFile(c->published, dest);
}

/* The item's id and author into the file that was sent, so its row offers
   Update from now on. */
static bool wsRecordId(void *ctx, uint64_t id, uint64_t author) {
    WsPublishCtx *c = (WsPublishCtx *)ctx;
    char          err[512];
    bool          ok;

    err[0] = '\0';
    ok = scnIoSetWorkshopId(c->published, id, author, err, sizeof(err));
    if (!ok) {
        WB_LOG_WARN(WB_LOG_CAT_CLIENT, "workshop: %s", err);
    }
    serverSimNoteScriptDirsChanged();
    wsMarkDirty();
    return ok;
}

/* The tag is set from the row each time the window is opened for one. */
static WorkshopPublishSpec s_spec = {
    "##WorkshopPublish",
    STR_DLGSETTINGS_WORKSHOP_PUB_HEADING,
    STR_DLGSETTINGS_WORKSHOP_PUB_UPDATE,
    "Scenario",
    wsBuildContent,
    wsRecordId,
    &s_pubCtx,
};

/* ── Drawing ─────────────────────────────────────────────────────── */

/* A button that switches the view, drawn pressed while its view is shown. */
static void wsViewButton(langid label, WsView view) {
    const bool active = (s_view == view);

    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(langGetText(label))) {
        s_view = view;
    }
    if (active) {
        ImGui::PopStyleColor();
    }
    imguiHandOnHover();
}

/* The row's chip after its name: Mod and Scenario the way the lobby draws
   them, and Map and Skin in the same chip in the style's own frame colours. */
static void wsKindChip(WsKind kind, float s) {
    if (kind == WS_KIND_MOD || kind == WS_KIND_SCENARIO) {
        lobbyScenarioKindTag(kind == WS_KIND_MOD, s);
    } else if (kind == WS_KIND_MAP || kind == WS_KIND_SKIN) {
        /* Centred on the name beside it, as lobbyScenarioKindTag centres. */
        float mid = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) *
                    0.5f;
        float h   = lobbyNameTagHeight(s);

        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                             (mid - h * 0.5f - ImGui::GetCursorScreenPos().y));
        lobbyDrawNameTag(langGetText(kind == WS_KIND_MAP
                                         ? STR_DLGSETTINGS_WORKSHOP_TAG_MAP
                                         : STR_DLGSETTINGS_SKIN),
                         ImGui::GetColorU32(ImGuiCol_FrameBg),
                         ImGui::GetColorU32(ImGuiCol_Text),
                         ImGui::GetColorU32(ImGuiCol_Border), s);
    }
}

static bool wsBeginList(const char *id, int columns, int rows) {
    const float lineH = ImGui::GetFrameHeightWithSpacing();
    const int   shown = (rows < WS_LIST_ROWS) ? rows : WS_LIST_ROWS;

    return ImGui::BeginTable(id, columns,
                             ImGuiTableFlags_ScrollY |
                                 ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_BordersOuter,
                             ImVec2(0.0f, lineH * (float)shown +
                                              ImGui::GetStyle().CellPadding.y *
                                                  2.0f));
}

static void wsDrawSubscribed(float s) {
    if (s_subRows.empty()) {
        ImGui::TextDisabled("%s",
                            langGetText(STR_DLGSETTINGS_WORKSHOP_NONE_SUBSCRIBED));
        return;
    }
    if (!wsBeginList("##wssub", 3, (int)s_subRows.size())) return;
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##open", ImGuiTableColumnFlags_WidthFixed);
    for (size_t i = 0; i < s_subRows.size(); i++) {
        const WsSubRow &r = s_subRows[i];

        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        if (r.name[0] != '\0') {
            ImGui::TextUnformatted(r.name);
        } else {
            MessageArgs args = {};
            SDL_snprintf(args.string1, sizeof(args.string1), "%llu",
                         (unsigned long long)r.id);
            ImGui::TextUnformatted(
                langGetTextFmt(STR_DLGSETTINGS_WORKSHOP_ITEM, &args));
        }
        wsKindChip(r.kind, s);

        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(
            "%s", langGetText(r.state == WS_SUB_INSTALLED
                                  ? STR_DLGSETTINGS_WORKSHOP_INSTALLED
                              : r.state == WS_SUB_DOWNLOADING
                                  ? STR_DLGSKIN_DOWNLOADING
                                  : STR_DLGSETTINGS_WORKSHOP_UNUSABLE));

        ImGui::TableSetColumnIndex(2);
        if (ImGui::SmallButton(langGetText(STR_DLGSETTINGS_WORKSHOP_OPEN))) {
            steam_workshop_open_item_page(r.id);
        }
        imguiHandOnHover();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void wsDrawPublish(float s) {
    if (s_pubRows.empty()) {
        ImGui::TextDisabled("%s",
                            langGetText(STR_DLGSETTINGS_WORKSHOP_NONE_PUBLISH));
        return;
    }
    if (!wsBeginList("##wspub", 2, (int)s_pubRows.size())) return;
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##go", ImGuiTableColumnFlags_WidthFixed);
    for (size_t i = 0; i < s_pubRows.size(); i++) {
        const WsPubRow &r   = s_pubRows[i];
        const bool      own = wsOwnItem(r.id, r.author);

        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(r.name);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip("%s", r.path);
        }
        wsKindChip(r.mod ? WS_KIND_MOD : WS_KIND_SCENARIO, s);

        ImGui::TableSetColumnIndex(1);
        /* Another item still uploading refuses this one, so the button is
           drawn disabled until that upload is over. */
        ImGui::BeginDisabled(workshopPublishBusyFor(r.key));
        if (ImGui::SmallButton(own ? langGetText(STR_DLGSETTINGS_WORKSHOP_UPDATE)
                                   : langGetText(STR_DLGSKIN_PUBLISH_GO))) {
            /* The window opens only when no other item is still uploading,
               and the context is touched only once it has said yes. The
               published path is left alone: it is set when Publish is
               pressed, and an upload this row resumes still needs it to
               write its id back to. */
            if (workshopPublishPrepare(r.key, r.name, r.description, r.id,
                                       r.author)) {
                SDL_strlcpy(s_pubCtx.source, r.path, sizeof(s_pubCtx.source));
                s_pubCtx.isLua = r.isLua;
                s_spec.tag     = r.mod ? "Mod" : "Scenario";
                ImGui::OpenPopup(s_spec.popupId);
            }
        }
        ImGui::EndDisabled();
        imguiHandOnHover();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void imguiSettingsWorkshopSection(void) {
    if (!steam_workshop_available()) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float  s = dialogComputeScale((int)display.x, (int)display.y);

    /* A newly installed item moves the sync's generation on. */
    if (workshopSyncGeneration() != s_syncGen) {
        s_syncGen = workshopSyncGeneration();
        wsMarkDirty();
    }

    ImGui::Spacing();
    ImGui::SeparatorText(langGetText(STR_DLGSETTINGS_WORKSHOP_HEADING));

    wsViewButton(STR_DLGSETTINGS_WORKSHOP_SUBSCRIBED, WS_VIEW_SUBSCRIBED);
    ImGui::SameLine();
    wsViewButton(STR_DLGSETTINGS_WORKSHOP_PUBLISH, WS_VIEW_PUBLISH);
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_DLGBROWSER_REFRESH))) {
        /* The sync first, so a subscription made since the last pass is
           copied in before the rows are read. */
        workshopSyncRun();
        wsMarkDirty();
    }
    imguiHandOnHover();

    if (s_view == WS_VIEW_SUBSCRIBED) {
        if (s_subDirty) wsBuildSubscribed();
        wsDrawSubscribed(s);
    } else {
        if (s_pubDirty) wsBuildPublish();
        wsDrawPublish(s);
    }

    workshopPublishDraw(&s_spec);
}
