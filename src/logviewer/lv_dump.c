/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Text dump of a recording, with no window. lv_dump.h says what it is for and
 * how it is asked for.
 *
 * It plays the log forward from its start, one 20 ms step at a time, with
 * the decoder's listener set (lv_screenSetObserver), so no host callback
 * runs. Nothing here seeks.
 *
 * What a line can say is what the recording holds. A tank has a place, a
 * frame, a boat flag and its four stocks; it has no speed. A builder has a
 * place and a frame; it has no target and no job. A shell has a place and a
 * frame; it has no owner. A builder's death is a log_LostMan with the slot
 * and nothing else, so what killed it has to be read from the records of the
 * same step and the steps before it (shells, the Sound records, map changes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#endif

#include "lv_global.h"
#include "backend.h"
#include "logviewer.h"
#include "lv_log.h"
#include "lv_bolo_map.h"
#include "lv_pillbox.h"
#include "lv_bases.h"
#include "lv_players.h"
#include "lv_shells.h"
#include "lv_event_kind.h"
#include "lv_presentation.h"
#include "lv_dump.h"

/* Playback time per decoder step. */
#define DUMP_STEP_MS      20u
/* Most rounds listed. A log with more keeps the rest in the last one. */
#define DUMP_ROUNDS_MAX   64
/* The longest state line kept for the changed-since-last-step check. */
#define DUMP_BODY_LEN     512
/* A whole-log run stops this long after the log's measured end, as the
   load does, so a log the decoder cannot finish does not hang. */
#define DUMP_END_SLACK_MS 60000u

typedef struct {
  uint32_t startMs;   /* first step of the round */
  uint32_t endMs;     /* last step of the round */
  uint32_t firstTick;
  uint32_t lastTick;
} DumpRound;

typedef struct {
  FILE    *out;
  LogViewerState *lv;
  uint32_t ms;        /* the step being decoded */
  uint32_t tick;
  bool     inWindow;  /* records and state are written */
  bool     allEvents; /* event lines are written outside the window too */
  /* The last body written for each tank, builder, pillbox and base. */
  char     tank[MAX_TANKS][DUMP_BODY_LEN];
  char     lgm[MAX_TANKS][DUMP_BODY_LEN];
  char     pill[MAX_PILLS][DUMP_BODY_LEN];
  char     base[MAX_BASES][DUMP_BODY_LEN];
} DumpCtx;

/* The tick the Controls panel shows at ms. */
static uint32_t dumpTickAt(uint32_t ms) {
  return lv_screenHasServerTick() ? lv_screenServerTickAt(ms) : ms / DUMP_STEP_MS;
}

/* Text inside double quotes: a quote, a backslash and anything below a space
 * are written so the line stays one line. */
static void dumpQuoted(FILE *out, const char *text) {
  const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");

  fputc('"', out);
  for (; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\') {
      fputc('\\', out);
      fputc(*p, out);
    } else if (*p < 0x20) {
      fprintf(out, "\\x%02X", (unsigned)*p);
    } else {
      fputc(*p, out);
    }
  }
  fputc('"', out);
}

/* The same into a buffer, for the state lines that are compared. */
static void dumpQuotedTo(char *dest, size_t size, const char *text) {
  const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");
  size_t n = 0;

  if (size < 3) {
    if (size > 0) dest[0] = '\0';
    return;
  }
  dest[n++] = '"';
  for (; *p != '\0' && n + 6 < size; p++) {
    if (*p == '"' || *p == '\\') {
      dest[n++] = '\\';
      dest[n++] = (char)*p;
    } else if (*p < 0x20) {
      n += (size_t)snprintf(dest + n, size - n, "\\x%02X", (unsigned)*p);
    } else {
      dest[n++] = (char)*p;
    }
  }
  dest[n++] = '"';
  dest[n] = '\0';
}

static const char *dumpTerrainName(BYTE t) {
  switch (t) {
    case BUILDING:     return "building";
    case RIVER:        return "river";
    case SWAMP:        return "swamp";
    case CRATER:       return "crater";
    case ROAD:         return "road";
    case FOREST:       return "forest";
    case RUBBLE:       return "rubble";
    case GRASS:        return "grass";
    case HALFBUILDING: return "halfbuilding";
    case BOAT:         return "boat";
    case MINE_SWAMP:   return "mined_swamp";
    case MINE_CRATER:  return "mined_crater";
    case MINE_ROAD:    return "mined_road";
    case MINE_FOREST:  return "mined_forest";
    case MINE_RUBBLE:  return "mined_rubble";
    case MINE_GRASS:   return "mined_grass";
    case DEEP_SEA:     return "deep_sea";
    default:           return "unknown";
  }
}

static const char *dumpGameTypeName(BYTE gt) {
  switch (gt) {
    case gameOpen:             return "open";
    case gameTournament:       return "tournament";
    case gameStrictTournament: return "strict_tournament";
    case gameScripted:         return "scripted";
    default:                   return "unknown";
  }
}

/* A record kind's name, as lv_log.h spells it without the log_ prefix. */
static const char *dumpRecordName(BYTE code) {
  switch (code) {
#define DUMP_REC(x) case log_##x: return #x
    DUMP_REC(PlayerJoined);     DUMP_REC(PlayerQuit);      DUMP_REC(PlayerLocation);
    DUMP_REC(LgmLocation);      DUMP_REC(MapChange);       DUMP_REC(Shell);
    DUMP_REC(SoundBuild);       DUMP_REC(SoundFarm);       DUMP_REC(SoundShoot);
    DUMP_REC(SoundHitTank);     DUMP_REC(SoundHitTree);    DUMP_REC(SoundHitWall);
    DUMP_REC(SoundMineLay);     DUMP_REC(SoundMineExplode); DUMP_REC(SoundExplosion);
    DUMP_REC(SoundBigExplosion); DUMP_REC(SoundManDie);    DUMP_REC(MessageServer);
    DUMP_REC(MessageAll);       DUMP_REC(MessagePlayers);  DUMP_REC(ChangeName);
    DUMP_REC(AllyRequest);      DUMP_REC(AllyAccept);      DUMP_REC(AllyLeave);
    DUMP_REC(BaseSetOwner);     DUMP_REC(BaseSetStock);    DUMP_REC(PillSetOwner);
    DUMP_REC(PillSetHealth);    DUMP_REC(PillSetPlace);    DUMP_REC(PillSetInTank);
    DUMP_REC(SaveMap);          DUMP_REC(LostMan);         DUMP_REC(KillPlayer);
    DUMP_REC(PlayerRejoin);     DUMP_REC(PlayerLeaving);   DUMP_REC(PlayerDied);
    DUMP_REC(LobbyEnter);       DUMP_REC(LobbyExit);       DUMP_REC(PlayerReady);
    DUMP_REC(PlayerUnready);    DUMP_REC(TeamSet);         DUMP_REC(CountdownStart);
    DUMP_REC(CountdownCancel);  DUMP_REC(MapSkipVote);     DUMP_REC(MapSkipApplied);
    DUMP_REC(BalanceApplied);   DUMP_REC(GameVoteStart);   DUMP_REC(GameVoteCast);
    DUMP_REC(GameVoteEnd);      DUMP_REC(SpectatorJoined); DUMP_REC(SpectatorLeft);
    DUMP_REC(SpectatorChat);    DUMP_REC(GameSettings);    DUMP_REC(Ping);
    DUMP_REC(TankSetStock);     DUMP_REC(TankSetModifiers); DUMP_REC(EntityChange);
    DUMP_REC(EntityMasks);      DUMP_REC(ServerText);      DUMP_REC(GameTimeSet);
    DUMP_REC(RuleSet);          DUMP_REC(ScnPanel);        DUMP_REC(ScnScore);
    DUMP_REC(ScnAnnounce);      DUMP_REC(ScnMarker);       DUMP_REC(ScnHint);
    DUMP_REC(ServerTick);       DUMP_REC(ScnStatus);       DUMP_REC(VoiceEveryone);
    DUMP_REC(MapName);
#undef DUMP_REC
    default: return "Unknown";
  }
}

/* A pascal string at bytes[at]: its length byte, then the text. Written
 * quoted; nothing is written when the bytes stop short. */
static void dumpPascal(FILE *out, const char *key, const BYTE *bytes, int len, int at) {
  char text[260];
  int  n;

  if (at >= len) {
    return;
  }
  n = bytes[at];
  if (at + 1 + n > len) {
    n = len - at - 1;
  }
  memcpy(text, bytes + at + 1, (size_t)n);
  text[n] = '\0';
  fprintf(out, " %s=", key);
  dumpQuoted(out, text);
}

/* The fields of the record kinds that say where something was or what
 * happened to it, by the layout lv_screenProcessLog reads. A kind not named
 * here still has its bytes on the line. */
static void dumpRecordFields(DumpCtx *c, BYTE code, const BYTE *b, int len) {
  FILE *out = c->out;

  switch (code) {
    case log_PlayerLocation:
      if (len >= 5) {
        fprintf(out, " slot=%u x=%u y=%u px=%u py=%u frame=%u boat=%u",
                b[0], b[1], b[2], b[3] >> 4, b[3] & 15, b[4] >> 4, b[4] & 15);
        if (b[1] == 0) {
          fprintf(out, " ignored=1");   /* x 0 is no place: the decoder skips it */
        }
      }
      break;
    case log_LgmLocation:
      if (len >= 4) {
        fprintf(out, " slot=%u frame=%u x=%u y=%u px=%u py=%u",
                b[0] >> 4, b[0] & 15, b[1], b[2], b[3] >> 4, b[3] & 15);
      }
      break;
    case log_Shell:
      if (len >= 4) {
        fprintf(out, " x=%u y=%u px=%u py=%u frame=%u",
                b[0], b[1], b[2] >> 4, b[2] & 15, b[3]);
      }
      break;
    case log_MapChange:
      if (len >= 3) {
        /* The record is not applied yet, so the map still holds the old one. */
        BYTE old = lv_mapGetPos(&c->lv->mp, b[0], b[1]);
        fprintf(out, " x=%u y=%u old=%u old_name=%s new=%u new_name=%s",
                b[0], b[1], old, dumpTerrainName(old), b[2], dumpTerrainName(b[2]));
      }
      break;
    case log_SoundBuild:      case log_SoundFarm:        case log_SoundShoot:
    case log_SoundHitTank:    case log_SoundHitTree:     case log_SoundHitWall:
    case log_SoundMineLay:    case log_SoundMineExplode: case log_SoundExplosion:
    case log_SoundBigExplosion: case log_SoundManDie:
      if (len >= 2) {
        fprintf(out, " x=%u y=%u", b[0], b[1]);
      }
      break;
    case log_LostMan:
    case log_PlayerDied:
    case log_PlayerQuit:
    case log_PlayerRejoin:
    case log_PlayerLeaving:
    case log_AllyLeave:
      if (len >= 1) {
        fprintf(out, " slot=%u", b[0]);
      }
      break;
    case log_KillPlayer:
      if (len >= 2) {
        fprintf(out, " victim=%u killer=%u", b[0], b[1]);
      }
      break;
    case log_TankSetStock:
      if (len >= 5) {
        fprintf(out, " slot=%u shells=%u mines=%u armour=%u trees=%u",
                b[0], b[1], b[2], b[3], b[4]);
      }
      break;
    case log_BaseSetOwner:
      if (len >= 3) {
        fprintf(out, " index=%u owner=%u migrate=%u", b[0], b[1], b[2]);
      }
      break;
    case log_BaseSetStock:
      if (len >= 4) {
        fprintf(out, " index=%u shells=%u mines=%u armour=%u", b[0], b[1], b[2], b[3]);
      }
      break;
    case log_PillSetOwner:
      if (len >= 3) {
        fprintf(out, " index=%u owner=%u migrate=%u", b[0], b[1], b[2]);
      }
      break;
    case log_PillSetPlace:
      if (len >= 3) {
        fprintf(out, " index=%u x=%u y=%u", b[0], b[1], b[2]);
      }
      break;
    case log_PillSetHealth:
      if (c->lv->loadedLogVersion >= LOG_VERSION_V3) {
        if (len >= 2) fprintf(out, " index=%u armour=%u", b[0], b[1]);
      } else if (len >= 1) {
        fprintf(out, " index=%u armour=%u", b[0] >> 4, b[0] & 15);
      }
      break;
    case log_PillSetInTank:
      if (len >= 1) {
        fprintf(out, " index=%u in_tank=%u", b[0] >> 4, b[0] & 15);
      }
      break;
    case log_AllyRequest:
    case log_AllyAccept:
      if (len >= 2) {
        fprintf(out, " slot=%u other=%u", b[0], b[1]);
      }
      break;
    case log_TeamSet:
      if (len >= 2) {
        fprintf(out, " slot=%u team=%u", b[0], b[1]);
      }
      break;
    case log_ServerTick:
      if (len >= 4) {
        fprintf(out, " server_tick=%u",
                ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) |
                ((unsigned)b[2] << 8) | (unsigned)b[3]);
      }
      break;
    case log_PlayerJoined:
      if (len >= 5) {
        fprintf(out, " slot=%u", b[0]);
        dumpPascal(out, "name", b, len, 5);
      }
      break;
    case log_ChangeName:
      if (len >= 1) {
        fprintf(out, " slot=%u", b[0]);
        dumpPascal(out, "name", b, len, 1);
      }
      break;
    case log_VoiceEveryone:
      if (len >= 1) {
        fprintf(out, " everyone=%u", b[0]);
      }
      break;
    case log_MapName:
      dumpPascal(out, "name", b, len, 0);
      break;
    case log_MessageAll:
      if (len >= 1) {
        fprintf(out, " sender=%u", b[0]);
        dumpPascal(out, "text", b, len, 1);
      }
      break;
    case log_MessagePlayers:
      if (len >= 2) {
        fprintf(out, " sender=%u to=%u", b[0], b[1]);
        dumpPascal(out, "text", b, len, 2);
      }
      break;
    case log_MessageServer:
      dumpPascal(out, "text", b, len, 0);
      break;
    case log_ServerText:
      if (len >= 2) {
        fprintf(out, " dest_team=%u dest_player=%u", b[0], b[1]);
        dumpPascal(out, "text", b, len, 2);
      }
      break;
    case log_Ping:
      if (len >= 6) {
        fprintf(out, " sender=%u kind=%u wx=%u wy=%u", b[0], b[1],
                ((unsigned)b[2] << 8) | b[3], ((unsigned)b[4] << 8) | b[5]);
      }
      break;
    case log_ScnHint:
      if (len >= 1) {
        fprintf(out, " slot=%u", b[0]);
        dumpPascal(out, "verb", b, len, 1);
      }
      break;
    default:
      break;
  }
}

/* The listener's record call: one "rec" line per log record in the window. */
static void dumpOnRecord(void *ctx, BYTE code, const BYTE *bytes, int len, int frameLen) {
  DumpCtx *c = (DumpCtx *)ctx;
  int      i;

  if (c->inWindow == FALSE) {
    return;
  }
  fprintf(c->out, "rec tick=%u ms=%u kind=%s code=%u", (unsigned)c->tick,
          (unsigned)c->ms, dumpRecordName(code), (unsigned)code);
  if (bytes == NULL) {
    /* An older file: the record has no frame to take the bytes from. */
    fprintf(c->out, " bytes=not_framed\n");
    return;
  }
  fprintf(c->out, " len=%d", frameLen);
  dumpRecordFields(c, code, bytes, len);
  fprintf(c->out, " bytes=");
  for (i = 0; i < len; i++) {
    fprintf(c->out, "%02X", bytes[i]);
  }
  if (len < frameLen) {
    fprintf(c->out, "...");
  }
  fputc('\n', c->out);
}

/* The listener's event call: one "event" line per events panel line. */
static void dumpOnEvent(void *ctx, int kind, int mapX, int mapY, const char *text) {
  DumpCtx *c = (DumpCtx *)ctx;

  if (c->inWindow == FALSE && c->allEvents == FALSE) {
    return;
  }
  fprintf(c->out, "event tick=%u ms=%u kind=%s", (unsigned)c->tick, (unsigned)c->ms,
          (kind >= 0 && kind < LV_EVK_COUNT) ? lvEventKindKey(kind) : "other");
  if (mapX >= 0 && mapY >= 0) {
    fprintf(c->out, " x=%d y=%d", mapX, mapY);
  }
  fprintf(c->out, " text=");
  dumpQuoted(c->out, text);
  fputc('\n', c->out);
}

/* A slot's name and the two teams the decoder holds for it: the alliance
 * group (team) and the lobby team a log_TeamSet named (lobby_team). */
static int dumpPlayerBody(char *dest, size_t size, BYTE slot) {
  char name[PLAYER_NAME_LEN + 8];
  char quoted[PLAYER_NAME_LEN * 4 + 8];
  BYTE lobbyTeam = 0;
  bool haveLobby;

  name[0] = '\0';
  lv_playersGetPlayerName(slot, name, sizeof(name));
  dumpQuotedTo(quoted, sizeof(quoted), name);
  haveLobby = lv_screenGetSlotTeam(slot, &lobbyTeam);
  if (haveLobby) {
    return snprintf(dest, size, "slot=%u name=%s bot=%d team=%u lobby_team=%u",
                    slot, quoted, lv_playersIsBot(slot) ? 1 : 0,
                    lv_playersGetTeamId(slot), lobbyTeam);
  }
  return snprintf(dest, size, "slot=%u name=%s bot=%d team=%u lobby_team=unknown",
                  slot, quoted, lv_playersIsBot(slot) ? 1 : 0,
                  lv_playersGetTeamId(slot));
}

/* End a text that holds an open "[" with "]". When the text filled its
   buffer the "]" takes the place of the last character, so a line that was
   cut short still reads as a closed list. */
static void dumpCloseList(char *dest, size_t size) {
  size_t len;

  if (size < 2) return;
  len = strlen(dest);
  if (len + 1 < size) {
    dest[len]     = ']';
    dest[len + 1] = '\0';
  } else {
    dest[size - 2] = ']';
    dest[size - 1] = '\0';
  }
}

static void dumpTankBody(DumpCtx *c, BYTE slot, char *dest, size_t size) {
  BYTE mx = 0, my = 0, px = 0, py = 0, frame = 0;
  bool boat = FALSE;
  int  n;
  BYTE i, total;
  bool first = TRUE;

  if (lv_playersIsInUse(slot) == FALSE) {
    snprintf(dest, size, "slot=%u in_use=0", slot);
    return;
  }
  lv_playersGetTankDetails(slot, &mx, &my, &px, &py, &frame, &boat);
  n = dumpPlayerBody(dest, size, slot);
  if (n < 0 || (size_t)n >= size) return;
  /* A tank with no place is off the map: dead, or not in the round yet. wx
     and wy are the place in world units, 256 to a map square; the recording
     gives 16 steps to a square, so they move 16 at a time. */
  n += snprintf(dest + n, size - (size_t)n,
                " alive=%d on_map=%d x=%u y=%u px=%u py=%u wx=%u wy=%u frame=%u boat=%d"
                " armour=%u shells=%u mines=%u trees=%u kills=%u deaths=%u pills=[",
                c->lv->gameViewHud[slot].alive ? 1 : 0, (mx != 0 || my != 0) ? 1 : 0,
                mx, my, px, py, (unsigned)mx * 256u + (unsigned)px * 16u,
                (unsigned)my * 256u + (unsigned)py * 16u, frame, boat ? 1 : 0,
                c->lv->tankInv[slot].armour, c->lv->tankInv[slot].shells,
                c->lv->tankInv[slot].mines, c->lv->tankInv[slot].trees,
                (unsigned)c->lv->kills[slot], (unsigned)c->lv->deaths[slot]);
  if (n < 0) return;
  if ((size_t)n >= size) {
    /* Cut short inside or just before the list: close it all the same. */
    dumpCloseList(dest, size);
    return;
  }
  /* The pillboxes this tank carries. */
  total = lv_pillsGetNumPills(&c->lv->pb);
  for (i = 1; i <= total && i <= MAX_PILLS; i++) {
    pillbox p;
    memset(&p, 0, sizeof(p));
    lv_pillsGetPill(&c->lv->pb, &p, i);
    if (p.inTank && p.owner == slot && (size_t)n + 8 < size) {
      n += snprintf(dest + n, size - (size_t)n, first ? "%u" : ",%u", i);
      first = FALSE;
    }
  }
  dumpCloseList(dest, size);
}

static void dumpLgmBody(BYTE slot, char *dest, size_t size) {
  BYTE mx = 0, my = 0, px = 0, py = 0, frame = 0;
  bool isOut = FALSE, isDead = FALSE;
  const char *state;

  if (lv_playersIsInUse(slot) == FALSE) {
    snprintf(dest, size, "slot=%u in_use=0", slot);
    return;
  }
  lv_playersGetLgmDetails(slot, &mx, &my, &px, &py, &frame);
  lv_playersGetLgmStatus(slot, &isOut, &isDead);
  /* The decoder holds three things: out of the tank this step (a
     log_LgmLocation named it), dead (a log_LostMan, or the parachute frame),
     and neither, which is in the tank. */
  if (isDead && isOut) {
    state = "dead_parachuting_in";
  } else if (isDead) {
    state = "dead";
  } else if (isOut) {
    state = "out";
  } else {
    state = "in_tank";
  }
  if (isOut) {
    snprintf(dest, size, "slot=%u state=%s x=%u y=%u px=%u py=%u wx=%u wy=%u frame=%u",
             slot, state, mx, my, px, py, (unsigned)mx * 256u + (unsigned)px * 16u,
             (unsigned)my * 256u + (unsigned)py * 16u, frame);
  } else {
    snprintf(dest, size, "slot=%u state=%s", slot, state);
  }
}

/* An owner as a line says it: the slot and its team, or neutral. */
static int dumpOwner(char *dest, size_t size, BYTE owner) {
  if (owner == NEUTRAL) {
    return snprintf(dest, size, "owner=neutral team=none");
  }
  return snprintf(dest, size, "owner=%u team=%u", owner, lv_playersGetTeamForOwner(owner));
}

static void dumpPillBody(DumpCtx *c, BYTE num, char *dest, size_t size) {
  pillbox p;
  char    owner[48];

  memset(&p, 0, sizeof(p));
  p.owner = NEUTRAL;
  lv_pillsGetPill(&c->lv->pb, &p, num);
  dumpOwner(owner, sizeof(owner), p.owner);
  snprintf(dest, size, "num=%u on_map=%d x=%u y=%u %s armour=%u speed=%u in_tank=%d",
           num, lv_pillsIsActive(&c->lv->pb, num) ? 1 : 0, p.x, p.y, owner,
           p.armour, p.speed, p.inTank ? 1 : 0);
}

static void dumpBaseBody(DumpCtx *c, BYTE num, char *dest, size_t size) {
  base b;
  char owner[48];

  memset(&b, 0, sizeof(b));
  b.owner = NEUTRAL;
  lv_basesGetBase(&c->lv->bs, &b, num);
  dumpOwner(owner, sizeof(owner), b.owner);
  snprintf(dest, size, "num=%u on_map=%d x=%u y=%u %s armour=%u shells=%u mines=%u",
           num, lv_basesIsActive(&c->lv->bs, num) ? 1 : 0, b.x, b.y, owner,
           b.armour, b.shells, b.mines);
}

/* One state line, when it is due: always in a full listing (an empty slot
 * only if it was listed before), and otherwise only when the body is not the
 * one written last. */
static void dumpStateLine(DumpCtx *c, const char *what, char *last, const char *body,
                          bool full, bool empty) {
  bool changed = strcmp(last, body) != 0;

  if (full ? (!empty || (changed && last[0] != '\0')) : changed) {
    fprintf(c->out, "%s tick=%u ms=%u line=%s %s\n", what, (unsigned)c->tick,
            (unsigned)c->ms, (full || last[0] == '\0') ? "full" : "changed", body);
  }
  snprintf(last, DUMP_BODY_LEN, "%s", body);
}

/* The world after the step: every tank, builder, pillbox and base in a full
 * listing, else only the lines that changed. The shells are the ones this
 * step's records gave; the decoder keeps a shell for one step only. */
static void dumpState(DumpCtx *c, bool full) {
  char   body[DUMP_BODY_LEN];
  BYTE   i, total;
  shells s;

  for (i = 0; i < MAX_TANKS; i++) {
    dumpTankBody(c, i, body, sizeof(body));
    dumpStateLine(c, "tank", c->tank[i], body, full, lv_playersIsInUse(i) == FALSE);
  }
  for (i = 0; i < MAX_TANKS; i++) {
    dumpLgmBody(i, body, sizeof(body));
    dumpStateLine(c, "lgm", c->lgm[i], body, full, lv_playersIsInUse(i) == FALSE);
  }
  for (s = c->lv->shs; s != NULL; s = s->next) {
    fprintf(c->out, "shell tick=%u ms=%u x=%u y=%u px=%u py=%u wx=%u wy=%u frame=%u\n",
            (unsigned)c->tick, (unsigned)c->ms, s->mx, s->my, s->px, s->py,
            (unsigned)s->mx * 256u + (unsigned)s->px * 16u,
            (unsigned)s->my * 256u + (unsigned)s->py * 16u, s->frame);
  }
  total = lv_pillsGetNumPills(&c->lv->pb);
  for (i = 1; i <= total && i <= MAX_PILLS; i++) {
    dumpPillBody(c, i, body, sizeof(body));
    dumpStateLine(c, "pill", c->pill[i - 1], body, full, FALSE);
  }
  total = lv_basesGetNumBases(&c->lv->bs);
  for (i = 1; i <= total && i <= MAX_BASES; i++) {
    dumpBaseBody(c, i, body, sizeof(body));
    dumpStateLine(c, "base", c->base[i - 1], body, full, FALSE);
  }
}

/* Split the log into rounds by the tick: it only goes down where a new round
 * starts it again. A log with no server tick is one round. The step counters
 * in this file are 64 bits wide, so a total near the end of a uint32_t
 * cannot make "ms += DUMP_STEP_MS" go back to 0 and the loop run for ever. */
static int dumpFindRounds(uint32_t totalMs, DumpRound *rounds) {
  uint64_t ms;
  uint32_t prev = 0;
  int      n = 0;

  for (ms = DUMP_STEP_MS; ms <= totalMs; ms += DUMP_STEP_MS) {
    uint32_t tick = dumpTickAt((uint32_t)ms);
    if (n == 0 || (tick < prev && n < DUMP_ROUNDS_MAX)) {
      rounds[n].startMs   = (uint32_t)ms;
      rounds[n].firstTick = tick;
      n++;
    }
    rounds[n - 1].endMs    = (uint32_t)ms;
    rounds[n - 1].lastTick = tick;
    prev = tick;
  }
  return n;
}

/* The first step of the round at or past tick, or 0 when the round ends
 * before it. */
static uint32_t dumpStepAtTick(const DumpRound *r, uint32_t tick) {
  uint64_t ms;

  for (ms = r->startMs; ms <= r->endMs; ms += DUMP_STEP_MS) {
    if (dumpTickAt((uint32_t)ms) >= tick) {
      return (uint32_t)ms;
    }
  }
  return 0;
}

/* The last step of the round at or before tick. */
static uint32_t dumpLastStepAtTick(const DumpRound *r, uint32_t tick) {
  uint64_t ms;
  uint32_t found = r->startMs;

  for (ms = r->startMs; ms <= r->endMs; ms += DUMP_STEP_MS) {
    if (dumpTickAt((uint32_t)ms) <= tick) {
      found = (uint32_t)ms;
    } else {
      break;
    }
  }
  return found;
}

static void dumpHeader(DumpCtx *c, const LvDumpOptions *o, const DumpRound *rounds,
                       int numRounds) {
  LogViewerState *lv = c->lv;
  char name[PLAYER_NAME_LEN + 8];
  BYTE slot;
  int  i;

  fprintf(c->out, "# WinBolo log dump. One record per line, key=value.\n");
  fprintf(c->out, "# tick is the number the Log Viewer's Controls panel shows; ms is playback time.\n");
  fprintf(c->out, "log path=");
  dumpQuoted(c->out, o->logPath);
  fprintf(c->out, "\nlog format_version=%u map=", (unsigned)lv->loadedLogVersion);
  dumpQuoted(c->out, lv->mapName);
  fprintf(c->out, " game_type=%u game_type_name=%s game_version=%u.%u.%u hidden_mines=%u"
          " total_ms=%u game_start_ms=%u\n",
          (unsigned)lv->gt, dumpGameTypeName(lv->gt), (unsigned)lv->versionMajor,
          (unsigned)lv->versionMinor, (unsigned)lv->versionRevision,
          (unsigned)lv->allowHiddenMines, (unsigned)lv->totalTimeMs,
          (unsigned)lv_screenGameStartMs());
  if (lv_screenHasServerTick()) {
    fprintf(c->out, "log server_ticks=1 tick_source=log_ServerTick rounds=%d\n", numRounds);
  } else {
    fprintf(c->out, "log server_ticks=0 tick_source=playback_ms/20 rounds=%d\n", numRounds);
  }
  for (i = 0; i < numRounds; i++) {
    fprintf(c->out, "round n=%d start_ms=%u end_ms=%u first_tick=%u last_tick=%u\n",
            i + 1, (unsigned)rounds[i].startMs, (unsigned)rounds[i].endMs,
            (unsigned)rounds[i].firstTick, (unsigned)rounds[i].lastTick);
  }
  /* Every name the log gives a slot, from the load's walk of the whole file.
     The team and bot flag of a player are on its tank lines. */
  for (slot = 0; slot < MAX_TANKS; slot++) {
    name[0] = '\0';
    if (lv_screenGetLoggedPlayerName(slot, name, sizeof(name)) && name[0] != '\0') {
      fprintf(c->out, "player slot=%u name=", slot);
      dumpQuoted(c->out, name);
      fputc('\n', c->out);
    }
  }
}

/* The players as the decoder holds them now. */
static void dumpPlayersNow(DumpCtx *c, const char *when) {
  char body[DUMP_BODY_LEN];
  BYTE slot;

  for (slot = 0; slot < MAX_TANKS; slot++) {
    if (lv_playersIsInUse(slot)) {
      dumpPlayerBody(body, sizeof(body), slot);
      fprintf(c->out, "player_at at=%s ms=%u %s\n", when, (unsigned)c->lv->timeRunning, body);
    }
  }
}

/* Copy the finished file to stdout. */
static void dumpCopyToStdout(const char *path) {
  char   buf[4096];
  size_t n;
  FILE  *f;

#ifdef _WIN32
  /* A program built for the Windows subsystem has no stdout unless the
     caller gave it a pipe or a file. */
  if (_fileno(stdout) < 0) {
    return;
  }
#endif
  f = fopen(path, "rb");
  if (f == NULL) {
    return;
  }
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    fwrite(buf, 1, n, stdout);
  }
  fclose(f);
  fflush(stdout);
}

int lvDumpRun(const LvDumpOptions *o) {
  static DumpCtx   ctx;   /* too big for the stack; one dump at a time */
  static DumpRound rounds[DUMP_ROUNDS_MAX];
  LvDecodeObserver observer;
  LogViewerState  *lv;
  char   outPath[FILENAME_MAX];
  char   logPath[FILENAME_MAX];
  int    hideLobby;
  int    numRounds;
  int    result = LV_DUMP_OK;
  bool   window;
  int    roundIdx = -1;
  uint32_t targetMs = 0, targetTick = 0, fromMs = 0, toMs = 0, fromTick = 0, toTick = 0;
  uint32_t endMs;
  uint32_t span;
  int    i;
  int    n;

  if (o == NULL || o->logPath == NULL || o->logPath[0] == '\0') {
    return LV_DUMP_ERR_ARGS;
  }
  /* A path too long for its buffer is refused. Cut short it would name
     some other file, and the dump would read or write that one. */
  n = snprintf(logPath, sizeof(logPath), "%s", o->logPath);
  if (n < 0 || (size_t)n >= sizeof(logPath)) {
    return LV_DUMP_ERR_ARGS;
  }
  if (o->outPath != NULL && o->outPath[0] != '\0') {
    n = snprintf(outPath, sizeof(outPath), "%s", o->outPath);
  } else {
    n = snprintf(outPath, sizeof(outPath), "%s.dump.txt", o->logPath);
  }
  if (n < 0 || (size_t)n >= sizeof(outPath)) {
    return LV_DUMP_ERR_ARGS;
  }
  window = (o->haveTick || o->haveMs) ? TRUE : FALSE;
  span   = o->span;

  memset(&ctx, 0, sizeof(ctx));
  lv = lv_decoderCreate(false);
  if (lv == NULL) {
    return LV_DUMP_ERR_OPEN;
  }
  ctx.lv = lv;
  observer.record = dumpOnRecord;
  observer.event  = dumpOnEvent;
  observer.ctx    = &ctx;
  /* The whole file is the window, so the load does not jump to the round's
     start and playback begins at the log's first byte. */
  hideLobby = lv_screenGetHideLobby();
  lv_screenSetHideLobby(0);
  lv_screenSetObserver(&observer);
  /* Opened before the log, so a log that will not open is said in the file
     too: a caller with no console has nowhere else to read it. */
  ctx.out = fopen(outPath, "wb");
  if (ctx.out == NULL) {
    result = LV_DUMP_ERR_WRITE;
    goto done;
  }

  if (lv_screenLoadMap(logPath, 4) != TRUE) {
    fprintf(ctx.out, "error text=\"the log could not be opened or is not a WinBolo log\" path=");
    dumpQuoted(ctx.out, logPath);
    fputc('\n', ctx.out);
    result = LV_DUMP_ERR_OPEN;
    goto done;
  }
  /* No sounds and no item panel updates, as in a fast forward. */
  lv->fastForwarding = TRUE;

  numRounds = dumpFindRounds(lv->totalTimeMs, rounds);
  dumpHeader(&ctx, o, rounds, numRounds);

  if (window) {
    if (numRounds == 0) {
      fprintf(ctx.out, "error text=\"the log holds no playback steps\"\n");
      result = LV_DUMP_ERR_TICK;
      goto done;
    }
    if (o->round > numRounds || o->round < 0) {
      fprintf(ctx.out, "error text=\"the log has %d rounds; round %d was asked for\"\n",
              numRounds, o->round);
      result = LV_DUMP_ERR_TICK;
      goto done;
    }
    if (o->haveTick) {
      targetTick = o->tick;
      for (i = 0; i < numRounds && roundIdx < 0; i++) {
        if (o->round != 0 && o->round != i + 1) {
          continue;
        }
        if (targetTick >= rounds[i].firstTick && targetTick <= rounds[i].lastTick) {
          roundIdx = i;
        }
      }
      if (roundIdx < 0) {
        fprintf(ctx.out, "error text=\"tick %u is not in %s; see the round lines above "
                "for the ticks each round holds\"\n", (unsigned)targetTick,
                o->round != 0 ? "that round" : "any round of the log");
        result = LV_DUMP_ERR_TICK;
        goto done;
      }
      targetMs = dumpStepAtTick(&rounds[roundIdx], targetTick);
    } else {
      /* Up to the next whole step, and inside the log. */
      targetMs = ((o->ms + DUMP_STEP_MS - 1u) / DUMP_STEP_MS) * DUMP_STEP_MS;
      if (targetMs < DUMP_STEP_MS) targetMs = DUMP_STEP_MS;
      if (targetMs > rounds[numRounds - 1].endMs) {
        fprintf(ctx.out, "error text=\"%u ms is past the end of the log (%u ms)\"\n",
                (unsigned)o->ms, (unsigned)rounds[numRounds - 1].endMs);
        result = LV_DUMP_ERR_TICK;
        goto done;
      }
      for (i = 0; i < numRounds; i++) {
        if (targetMs >= rounds[i].startMs && targetMs <= rounds[i].endMs) {
          roundIdx = i;
        }
      }
      if (roundIdx < 0 || (o->round != 0 && o->round != roundIdx + 1)) {
        fprintf(ctx.out, "error text=\"%u ms is not in round %d\"\n",
                (unsigned)o->ms, o->round);
        result = LV_DUMP_ERR_TICK;
        goto done;
      }
      targetTick = dumpTickAt(targetMs);
    }
    fromTick = (targetTick > span) ? targetTick - span : 0;
    toTick   = (targetTick + span >= targetTick) ? targetTick + span : 0xFFFFFFFFu;
    fromMs   = dumpStepAtTick(&rounds[roundIdx], fromTick);
    if (fromMs == 0 || fromMs > targetMs) fromMs = targetMs;
    toMs     = dumpLastStepAtTick(&rounds[roundIdx], toTick);
    if (toMs < targetMs) toMs = targetMs;
    endMs    = toMs;
    fprintf(ctx.out, "dump mode=window tick=%u ms=%u round=%d span=%u from_tick=%u to_tick=%u"
            " from_ms=%u to_ms=%u\n", (unsigned)dumpTickAt(targetMs), (unsigned)targetMs,
            roundIdx + 1, (unsigned)span, (unsigned)dumpTickAt(fromMs),
            (unsigned)dumpTickAt(toMs), (unsigned)fromMs, (unsigned)toMs);
    if (o->haveTick && dumpTickAt(targetMs) != targetTick) {
      /* Playback steps two server ticks at a time, so an odd or an even
         tick may fall between two steps. */
      fprintf(ctx.out, "note text=\"no playback step lands on tick %u; the step at tick %u "
              "is the first at or past it\"\n", (unsigned)targetTick,
              (unsigned)dumpTickAt(targetMs));
    }
  } else {
    ctx.allEvents = TRUE;
    endMs = lv->totalTimeMs + DUMP_END_SLACK_MS;
    fprintf(ctx.out, "dump mode=events\n");
  }

  while (lv->isPlaying == TRUE && (uint64_t)lv->timeRunning + DUMP_STEP_MS <= endMs) {
    uint32_t ms = lv->timeRunning + DUMP_STEP_MS;
    bool     in = window && ms >= fromMs;
    bool     first = in && ctx.inWindow == FALSE;
    bool     target = in && ms == targetMs;

    ctx.ms       = ms;
    ctx.tick     = dumpTickAt(ms);
    if (first) {
      dumpPlayersNow(&ctx, "window_start");
    }
    ctx.inWindow = in;
    if (target) {
      fprintf(ctx.out, "==== TICK %u ==== ms=%u round=%d (asked for %s %u)\n",
              (unsigned)ctx.tick, (unsigned)ms, roundIdx + 1,
              o->haveTick ? "tick" : "ms", (unsigned)(o->haveTick ? o->tick : o->ms));
    }
    if (in) {
      fprintf(ctx.out, "step tick=%u ms=%u%s\n", (unsigned)ctx.tick, (unsigned)ms,
              target ? " target=1" : "");
    }
    lv_screenLogTick();
    if (in) {
      dumpState(&ctx, first || target);
    }
    if (target) {
      fprintf(ctx.out, "==== END OF TICK %u ====\n", (unsigned)ctx.tick);
    }
  }
  if (window && lv->timeRunning < toMs) {
    fprintf(ctx.out, "note text=\"the log ended at %u ms, before the end of the window\"\n",
            (unsigned)lv->timeRunning);
  }
  if (!window) {
    dumpPlayersNow(&ctx, "end");
  }
  fprintf(ctx.out, "end ms=%u\n", (unsigned)lv->timeRunning);

done:
  if (ctx.out != NULL) {
    if (ferror(ctx.out) && result == LV_DUMP_OK) {
      result = LV_DUMP_ERR_WRITE;
    }
    if (fclose(ctx.out) != 0 && result == LV_DUMP_OK) {
      result = LV_DUMP_ERR_WRITE;
    }
    ctx.out = NULL;
    if (o->toStdout) {
      dumpCopyToStdout(outPath);
    }
  }
  lv_screenSetObserver(NULL);
  lv_decoderDestroy(lv);
  lv_screenSetHideLobby(hideLobby);
  return result;
}

int lvDumpWanted(int argc, char **argv) {
  return (argc > 1 && argv[1] != NULL && strcmp(argv[1], "-dump") == 0) ? 1 : 0;
}

/* A whole unsigned number, and nothing else in the text. */
static bool dumpParseU32(const char *text, uint32_t *out) {
  char              *end = NULL;
  unsigned long long v;

  if (text == NULL || text[0] < '0' || text[0] > '9') {
    return FALSE;
  }
  v = strtoull(text, &end, 10);
  if (end == NULL || *end != '\0') {
    return FALSE;
  }
  /* A number too large for the 32 bits it goes into is refused, not cut
     down to some other number: "-tick 4294967297" must not dump tick 1.
     strtoull stops at its own largest value for a longer text, and that is
     over the limit too. */
  if (v > 0xFFFFFFFFull) {
    return FALSE;
  }
  *out = (uint32_t)v;
  return TRUE;
}

int lvDumpMain(int argc, char **argv) {
  LvDumpOptions o;
  uint32_t      value = 0;
  int           i;
  int           result;

  memset(&o, 0, sizeof(o));
  o.span     = LV_DUMP_DEFAULT_SPAN;
  o.toStdout = 1;
  if (argc < 3 || lvDumpWanted(argc, argv) == 0) {
    fprintf(stderr, "usage: -dump <log.wbv> [-tick N | -ms M] [-span S] [-round R] [-out <file>]\n");
    return LV_DUMP_ERR_ARGS;
  }
  o.logPath = argv[2];
  for (i = 3; i < argc; i++) {
    const char *arg  = argv[i];
    const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

    if (strcmp(arg, "-out") == 0 && next != NULL) {
      o.outPath = next;
    } else if (strcmp(arg, "-tick") == 0 && dumpParseU32(next, &value)) {
      o.haveTick = 1;
      o.tick     = value;
    } else if (strcmp(arg, "-ms") == 0 && dumpParseU32(next, &value)) {
      o.haveMs = 1;
      o.ms     = value;
    } else if (strcmp(arg, "-span") == 0 && dumpParseU32(next, &value)) {
      o.span = value;
    } else if (strcmp(arg, "-round") == 0 && dumpParseU32(next, &value) &&
               value <= 0x7FFFFFFFu) {
      o.round = (int)value;
    } else {
      fprintf(stderr, "-dump: \"%s\" was not understood\n", arg);
      return LV_DUMP_ERR_ARGS;
    }
    i++;
  }
  result = lvDumpRun(&o);
  if (result != LV_DUMP_OK) {
    fprintf(stderr, "-dump: failed with code %d\n", result);
  }
  return result;
}
