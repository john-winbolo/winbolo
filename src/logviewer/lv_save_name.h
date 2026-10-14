/*
 * lv_save_name.h - the file name Save Log File offers
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * A log's own file name is often a key of letters and numbers. The name
 * offered for a saved copy is made from what the log says about the game:
 *
 *   2026-10-13 14.05 Oil Rig - Canuck, Eryan vs rooklift, 2 Bots.wbv
 *
 * the date and time, the map, and the players: every person by name, by team
 * where the log gives each of them a team, and then how many bots there
 * were. A part the log does not have is left out.
 *
 * Three things are here, with no SDL and no decoder in them, so the unit
 * tests can run them:
 *   - the list of the players of a whole log, kept as the log is decoded;
 *   - the map a lobby chose, read from the server's line about it;
 *   - the name itself.
 */

#ifndef LV_SAVE_NAME_H
#define LV_SAVE_NAME_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* --- The players of a whole log ------------------------------------------ */

#define LV_ROSTER_MAX      32 /* players kept; a log seats 16 at one time */
#define LV_ROSTER_SLOTS    16 /* MAX_TANKS */
#define LV_ROSTER_NAME_LEN 65 /* PLAYER_NAME_LEN */

typedef struct {
  char          name[LV_ROSTER_NAME_LEN];
  unsigned char team; /* 0 is no team */
  unsigned char bot;  /* 1 for a computer player */
} LvRosterRow;

/* Every player a log has had, in the order they came, and the row the
 * player in each slot has now (-1 for an empty slot). A slot is used again
 * after a player leaves, so the slot alone does not name a player. */
typedef struct {
  LvRosterRow row[LV_ROSTER_MAX];
  int         count;
  int         slotRow[LV_ROSTER_SLOTS];
} LvRoster;

static inline void lvRosterClear(LvRoster *r) {
  int i;
  r->count = 0;
  for (i = 0; i < LV_ROSTER_SLOTS; i++) {
    r->slotRow[i] = -1;
  }
}

/* A player is in a slot: a join, or a player a snapshot brought. A name the
 * list has already (the player left and came back) gets its old row. A
 * nameless player, or one past the end of a full list, is not kept. */
static inline void lvRosterJoin(LvRoster *r, int slot, const char *name) {
  int i;
  if (slot < 0 || slot >= LV_ROSTER_SLOTS) {
    return;
  }
  r->slotRow[slot] = -1;
  if (name == NULL || name[0] == '\0') {
    return;
  }
  for (i = 0; i < r->count; i++) {
    if (strncmp(r->row[i].name, name, LV_ROSTER_NAME_LEN - 1) == 0) {
      r->slotRow[slot] = i;
      return;
    }
  }
  if (r->count >= LV_ROSTER_MAX) {
    return;
  }
  snprintf(r->row[r->count].name, LV_ROSTER_NAME_LEN, "%s", name);
  r->row[r->count].team = 0;
  r->row[r->count].bot  = 0;
  r->slotRow[slot] = r->count;
  r->count++;
}

/* The player in a slot has a new name. */
static inline void lvRosterRename(LvRoster *r, int slot, const char *name) {
  if (slot < 0 || slot >= LV_ROSTER_SLOTS || r->slotRow[slot] < 0 ||
      name == NULL || name[0] == '\0') {
    return;
  }
  snprintf(r->row[r->slotRow[slot]].name, LV_ROSTER_NAME_LEN, "%s", name);
}

/* The player in a slot is on a team. The last team a player was on is the
 * one kept: a player who leaves a team at the end of the round is still
 * listed with the side played for. */
static inline void lvRosterTeam(LvRoster *r, int slot, unsigned char team) {
  if (slot < 0 || slot >= LV_ROSTER_SLOTS || r->slotRow[slot] < 0 || team == 0) {
    return;
  }
  r->row[r->slotRow[slot]].team = team;
}

/* The player in a slot is a bot, or is not: what the log's join record (or
 * a snapshot) says of it. Told after each lvRosterJoin. */
static inline void lvRosterBot(LvRoster *r, int slot, bool isBot) {
  if (slot < 0 || slot >= LV_ROSTER_SLOTS || r->slotRow[slot] < 0) {
    return;
  }
  r->row[r->slotRow[slot]].bot = isBot ? 1 : 0;
}

/* The player in a slot left. The row stays: the player was in the game. */
static inline void lvRosterQuit(LvRoster *r, int slot) {
  if (slot >= 0 && slot < LV_ROSTER_SLOTS) {
    r->slotRow[slot] = -1;
  }
}

/* --- The map a lobby chose ------------------------------------------------ */

/* A log that opens in a lobby has the map the lobby opened with in its
 * header: the map of the round before. When the host chooses another, the
 * server writes a line "Map changed to <name>" (handleLobbyMapChange in
 * src/server/server_dedicated_log.c), and that is the only place the log
 * names the map the round is then played on. The name in such a line, or
 * NULL when the line is not one or names nothing. */
static inline const char *lvMapChangeName(const char *serverLine) {
  static const char prefix[] = "Map changed to ";
  size_t            n        = sizeof(prefix) - 1;
  if (serverLine == NULL || strncmp(serverLine, prefix, n) != 0 ||
      serverLine[n] == '\0') {
    return NULL;
  }
  return serverLine + n;
}

/* What a log's records say the round's map is, as the decoder reads them in
 * order. Only a line written between the lobby opening and the round
 * starting counts: a server that goes on to its next map at the end of a
 * round can write the same line after it, and that map is the next log's. */
typedef struct {
  bool inLobby;
  bool known;
  char name[64];
} LvLobbyMap;

static inline void lvLobbyMapClear(LvLobbyMap *m) {
  m->inLobby = false;
  m->known   = false;
  m->name[0] = '\0';
}
static inline void lvLobbyMapEnter(LvLobbyMap *m) { m->inLobby = true; }
static inline void lvLobbyMapExit(LvLobbyMap *m) { m->inLobby = false; }

/* A map name the lobby gave: the server's line, or a map skip vote's. */
static inline void lvLobbyMapNamed(LvLobbyMap *m, const char *name) {
  if (!m->inLobby || name == NULL || name[0] == '\0') {
    return;
  }
  snprintf(m->name, sizeof(m->name), "%s", name);
  m->known = true;
}

/* --- The date in a server's own log name ---------------------------------- */

/* The server names a log "<YYYYMMDD>t<HHMMSS>_<map>" (in
 * serverDedicatedLogMakeAutoName), stamped when that round's log was
 * opened. TRUE, with the parts, when a file name starts with such a stamp
 * and the stamp is a possible date and time. */
static inline bool lvSaveNameStamp(const char *fileName, int *year, int *month,
                                   int *day, int *hour, int *minute) {
  int i, v[5];
  if (fileName == NULL) {
    return false;
  }
  for (i = 0; i < 16; i++) {
    char c = fileName[i];
    if (i == 8) {
      if (c != 't' && c != 'T') return false;
    } else if (i == 15) {
      if (c != '_') return false;
    } else if (c < '0' || c > '9') {
      return false;
    }
  }
#define LV_SN_D(i) (fileName[i] - '0')
  v[0] = LV_SN_D(0) * 1000 + LV_SN_D(1) * 100 + LV_SN_D(2) * 10 + LV_SN_D(3);
  v[1] = LV_SN_D(4) * 10 + LV_SN_D(5);
  v[2] = LV_SN_D(6) * 10 + LV_SN_D(7);
  v[3] = LV_SN_D(9) * 10 + LV_SN_D(10);
  v[4] = LV_SN_D(11) * 10 + LV_SN_D(12);
#undef LV_SN_D
  if (v[0] < 1990 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 ||
      v[3] > 23 || v[4] > 59) {
    return false;
  }
  *year = v[0]; *month = v[1]; *day = v[2]; *hour = v[3]; *minute = v[4];
  return true;
}

/* --- The name -------------------------------------------------------------- */

#define LV_SAVE_NAME_MAX_BYTES  180 /* the whole name, ".wbv" and all */
#define LV_SAVE_NAME_EXT        ".wbv"
#define LV_SAVE_NAME_STEM_BYTES (LV_SAVE_NAME_MAX_BYTES - 4) /* without it */
#define LV_SAVE_NAME_MAP_BYTES  64  /* of them, the most the map can have */

/* The length of text[0..len) without a UTF-8 character that a cut left
 * unfinished at its end. */
static inline size_t lvSaveNameWholeChars(const char *text, size_t len) {
  size_t back = 0;
  while (back < 3 && back < len &&
         ((unsigned char)text[len - 1 - back] & 0xC0) == 0x80) {
    back++;
  }
  if (back < len) {
    unsigned char lead = (unsigned char)text[len - 1 - back];
    size_t        need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
    if (need > back + 1) {
      return len - 1 - back;
    }
  }
  return len;
}

/* TRUE when Windows keeps a name for a device: CON, PRN, AUX, NUL, COM1 to
 * COM9 and LPT1 to LPT9, alone or with anything after a dot. */
static inline bool lvSaveNameIsDevice(const char *stem) {
  static const char *const three[] = {"CON", "PRN", "AUX", "NUL"};
  char   up[6];
  size_t n = 0, i;
  while (stem[n] != '\0' && stem[n] != '.' && n < 5) {
    char c = stem[n];
    up[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
  }
  if (stem[n] != '\0' && stem[n] != '.') {
    return false;
  }
  up[n] = '\0';
  for (i = 0; i < 4; i++) {
    if (strcmp(up, three[i]) == 0) {
      return true;
    }
  }
  return n == 4 && (strncmp(up, "COM", 3) == 0 || strncmp(up, "LPT", 3) == 0) &&
         up[3] >= '1' && up[3] <= '9';
}

/* Add text to a part of the name, made fit for a file name: a character a
 * file name cannot hold becomes '.', as the map name always has; spaces at
 * the two ends go and a run of them becomes one. Text past the end of the
 * buffer is dropped, never a part of a UTF-8 character. Returns the new
 * length. */
static inline size_t lvSaveNameAddClean(char *buf, size_t size, size_t len,
                                        const char *text) {
  size_t start = len;
  if (text == NULL) {
    return len;
  }
  for (; *text != '\0' && len + 1 < size; text++) {
    unsigned char c = (unsigned char)*text;
    if (c < 0x20 || c == 0x7F || strchr("\\/:*?\"<>|", c) != NULL) {
      c = '.';
    }
    if (c == ' ' && (len == start || buf[len - 1] == ' ')) {
      continue;
    }
    buf[len++] = (char)c;
  }
  if (*text != '\0') {
    len = start + lvSaveNameWholeChars(buf + start, len - start);
  }
  while (len > start && buf[len - 1] == ' ') {
    len--;
  }
  buf[len] = '\0';
  return len;
}

static inline size_t lvSaveNameAddRaw(char *buf, size_t size, size_t len,
                                      const char *text) {
  for (; *text != '\0' && len + 1 < size; text++) {
    buf[len++] = *text;
  }
  buf[len] = '\0';
  return len;
}

/* The name offered for a saved log, ".wbv" and all.
 *   year, month, day   the game's date; year 0 or less is no date
 *   hour, minute       its time of day; hour below 0 is no time
 *   mapName            NULL or "" is no map
 *   names, teams       the players, in the order they came; teams may be
 *                      NULL, and team 0 is no team
 *   bots               not 0 for a player that is a bot; NULL is no bots
 *   nameCount          how many
 *
 * Every person is named. The bots are not: they are counted, and the count
 * is the last thing in the name, "4 Bots" or "1 Bot". The people are by
 * team, with " vs " between the teams, when every one of them has a team and
 * there are two teams or more; otherwise they are one list.
 *
 * The whole name is LV_SAVE_NAME_MAX_BYTES bytes at most. When the people do
 * not all fit, as many whole names as do fit are given, taken a player from
 * each team in turn so no side goes unnamed for another's sake, and
 * " + N more" counts the rest of them. No name is ever cut short. The name
 * has no character Windows refuses, no dot or space before ".wbv", and is
 * not a name Windows keeps for a device. With nothing to make a name of it
 * is "log.wbv". */
static inline void lvSaveNameBuild(char *out, size_t outSize, int year,
                                   int month, int day, int hour, int minute,
                                   const char *mapName, const char *const *names,
                                   const unsigned char *teams,
                                   const unsigned char *bots, int nameCount) {
  char   buf[LV_SAVE_NAME_MAX_BYTES + LV_ROSTER_NAME_LEN + 80];
  char   mapClean[LV_SAVE_NAME_MAP_BYTES + 1];
  char   clean[LV_ROSTER_MAX][LV_ROSTER_NAME_LEN]; /* the people, made fit */
  int    team[LV_ROSTER_MAX];     /* each one's team */
  int    rank[LV_ROSTER_MAX];     /* each one's turn to be named */
  int    people = 0;              /* how many are in clean[] */
  int    unlisted = 0;            /* people past the end of clean[] */
  int    botCount = 0;
  int    teamList[LV_ROSTER_MAX]; /* the teams, lowest first */
  int    teamCount = 0;
  bool   byTeam;
  size_t len = 0, head;
  int    i, j, t, k, picked;

  if (out == NULL || outSize == 0) {
    return;
  }
  buf[0] = '\0';

  if (year > 0) {
    char date[40];
    if (hour >= 0) {
      snprintf(date, sizeof(date), "%04d-%02d-%02d %02d.%02d", year % 10000,
               month % 100, day % 100, hour % 100, minute % 100);
    } else {
      snprintf(date, sizeof(date), "%04d-%02d-%02d", year % 10000, month % 100,
               day % 100);
    }
    len = lvSaveNameAddRaw(buf, sizeof(buf), len, date);
  }
  if (lvSaveNameAddClean(mapClean, sizeof(mapClean), 0, mapName) > 0) {
    if (len > 0) {
      len = lvSaveNameAddRaw(buf, sizeof(buf), len, " ");
    }
    len = lvSaveNameAddRaw(buf, sizeof(buf), len, mapClean);
  }
  head = len;

  /* The people that have a name once it is made fit, and the bots. */
  if (names == NULL) {
    nameCount = 0;
  }
  for (i = 0; i < nameCount; i++) {
    if (bots != NULL && bots[i] != 0) {
      botCount++;
    } else if (people >= LV_ROSTER_MAX) {
      if (names[i] != NULL && names[i][0] != '\0') {
        unlisted++;
      }
    } else if (lvSaveNameAddClean(clean[people], sizeof(clean[people]), 0,
                                  names[i]) > 0) {
      team[people] = (teams != NULL) ? teams[i] : 0;
      rank[people] = -1;
      people++;
    }
  }

  /* By team only when every one has a team and there are two or more. */
  byTeam = (people > 0 && unlisted == 0);
  for (i = 0; byTeam && i < people; i++) {
    if (team[i] == 0) {
      byTeam = false;
      break;
    }
    for (j = 0; j < teamCount && teamList[j] != team[i]; j++) {
    }
    if (j == teamCount) {
      for (j = teamCount; j > 0 && teamList[j - 1] > team[i]; j--) {
        teamList[j] = teamList[j - 1];
      }
      teamList[j] = team[i];
      teamCount++;
    }
  }
  if (teamCount < 2) {
    byTeam = false;
  }

  /* Each one's turn to be named, for a name that cannot hold them all. */
  if (byTeam) {
    picked = 0;
    while (picked < people) {
      for (t = 0; t < teamCount; t++) {
        for (i = 0; i < people; i++) {
          if (rank[i] < 0 && team[i] == teamList[t]) {
            rank[i] = picked++;
            break;
          }
        }
      }
    }
  } else {
    for (i = 0; i < people; i++) {
      rank[i] = i;
    }
    teamCount = 1;
  }

  /* All of them if they fit; if not, one fewer, until what is left fits. */
  for (k = people;; k--) {
    int  left      = people - k + unlisted;
    bool firstTeam = true;
    len      = head;
    buf[len] = '\0';
    if (len > 0 && (k > 0 || left > 0 || botCount > 0)) {
      len = lvSaveNameAddRaw(buf, sizeof(buf), len, " - ");
    }
    for (t = 0; t < teamCount; t++) {
      bool firstName = true;
      for (i = 0; i < people; i++) {
        if (rank[i] >= k || (byTeam && team[i] != teamList[t])) {
          continue;
        }
        if (firstName && !firstTeam) {
          len = lvSaveNameAddRaw(buf, sizeof(buf), len, " vs ");
        } else if (!firstName) {
          len = lvSaveNameAddRaw(buf, sizeof(buf), len, ", ");
        }
        len = lvSaveNameAddRaw(buf, sizeof(buf), len, clean[i]);
        firstName = false;
        firstTeam = false;
      }
    }
    if (left > 0) {
      char rest[32];
      snprintf(rest, sizeof(rest), "%s+ %d more", (k > 0) ? " " : "", left);
      len = lvSaveNameAddRaw(buf, sizeof(buf), len, rest);
    }
    if (botCount > 0) {
      char count[32];
      snprintf(count, sizeof(count), "%s%d %s", (k > 0 || left > 0) ? ", " : "",
               botCount, (botCount == 1) ? "Bot" : "Bots");
      len = lvSaveNameAddRaw(buf, sizeof(buf), len, count);
    }
    if (len <= LV_SAVE_NAME_STEM_BYTES || k <= 1) {
      break;
    }
  }

  if (lvSaveNameIsDevice(buf)) {
    memmove(buf + 4, buf, len + 1);
    memcpy(buf, "log ", 4);
    len += 4;
  }
  /* The date, the map and one name are always less than the limit, so this
     cut is for nothing the viewer gives: it only keeps the promise. */
  if (len > LV_SAVE_NAME_STEM_BYTES) {
    len = lvSaveNameWholeChars(buf, LV_SAVE_NAME_STEM_BYTES);
  }
  while (len > 0 && (buf[len - 1] == '.' || buf[len - 1] == ' ')) {
    len--;
  }
  buf[len] = '\0';
  if (len == 0) {
    snprintf(buf, sizeof(buf), "log");
  }
  snprintf(out, outSize, "%s%s", buf, LV_SAVE_NAME_EXT);
}

#endif /* LV_SAVE_NAME_H */
