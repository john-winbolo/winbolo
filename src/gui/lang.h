/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*********************************************************
*Name:          Lang
*Filename:      lang.h
*Author:        John Morrison
*Creation Date: 28/4/00
*Last Modified: 28/4/00
*Purpose:
*  Language string ID defines and loading interface.
*  The #defines are safe for inclusion by the Windows RC
*  compiler (RC_INVOKED). C declarations are guarded.
*********************************************************/

#ifndef _LANG_H
#define _LANG_H

/* -------------------------------------------------------
 * String ID table — matches the lookup table in lang.c
 * and the STRINGTABLE in the Win32 .rc file.
 * These are plain integer #defines, safe for RC_INVOKED.
 * ------------------------------------------------------- */

/* Opening dialog */
#define STR_DLGOPENING_TITLE                151
#define STR_DLGOPENING_TEXT                 152
#define STR_DLGOPENING_OPTION1              153
#define STR_DLGOPENING_OPTION2              154
#define STR_DLGOPENING_OPTION3              155
#define STR_DLGOPENING_SKIP                 156
#define STR_OK                              157
#define STR_DLGOPENING_BUTTON2              158

/* Language dialog */
#define STR_DLGLANG_TITLE                   159
#define STR_DLGLANG_NAME_CAPTION            160
#define STR_DLGLANG_NAME                    161
#define STR_DLGLANG_AUTHOR_CAPTION          162
#define STR_DLGLANG_AUTHOR                  163
#define STR_DLGOPENING_OPTION4              164
#define STR_DLGOPENING_OPTION0              165
#define STR_DLGLANG_NOTES_CAPTION           166
#define STR_DLGLANG_NOTES                   167
#define STR_DLGLANG_DEFAULTNOTE             168

/* About dialog */
#define STR_DLGABOUT_TITLE                  169
#define STR_DLGABOUT_BLURB                  170

/* Alliance dialog */
#define STR_DLGALLIANCE_TITLE               171
#define STR_DLGALLIANCE_ACCEPT              172
#define STR_DLGALLIANCE_DECLINE             173
#define STR_DLGALLIANCE_BLURB               174

/* Game finder dialog */
#define STR_DLGGAMEFINDER_REFRESHFIRST      175
#define STR_DLGGAMEFINDER_MESSAGEOFTHEDAY   176
#define STR_DLGGAMEFINDER_WRONGVERSION      177
#define STR_DLGGAMEFINDER_NOGAMESINPROGRESS 178
#define STR_YES                             179
#define STR_NO                              180
#define STR_DLGGAMEFINDER_YESADV            181
#define STR_DLGGAMEFINDER_OPEN              182
#define STR_DLGGAMEFINDER_TOURNAMENT        183
#define STR_DLGGAMEFINDER_STRICTTOURNAMENT  184

/* Game info dialog */
#define STR_DLGGAMEINFO_TITLE               185
#define STR_DLGGAMEINFO_MAPNAME             186
#define STR_DLGGAMEINFO_NUMPLAYERS          187
#define STR_DLGGAMEINFO_GAMETYPE            188
#define STR_DLGGAMEINFO_HIDDENMINES         189
#define STR_DLGGAMEINFO_ALLOWCOMPTANKS      190
#define STR_DLGGAMEINFO_TIMELIMIT           191

/* Game setup dialog */
#define STR_DLGGAMESETUP_BLURB              192
#define STR_CANCEL                          193
#define STR_DLGGAMESETUP_TITLE              194
#define STR_DLGGAMESETUP_CHOOSEMAP          195
#define STR_DLGGAMESETUP_SELECTEDMAP        196
#define STR_DLGGAMESETUP_SELECTEDMAPINBUILT 197
#define STR_DLGGAMESETUP_RADIO1             198
#define STR_DLGGAMESETUP_RADIO2             199
#define STR_DLGGAMESETUP_RADIO3             200
#define STR_DLGGAMESETUP_HIDDENMINES        201
#define STR_DLGGAMESETUP_ALLOWCOMPTANKS     202
#define STR_DLGGAMESETUP_ALLOWCOMPTANKSADV  203
#define STR_DLGGAMESETUP_PASSWORD           204
#define STR_DLGGAMESETUP_MINUTES            205
#define STR_DLGGAMESETUP_SECONDS            206
#define STR_DLGGAMESETUP_STARTDELAY         207
#define STR_DLGGAMESETUP_TIMELIMIT          208
#define STR_DLGGAMESETUP_ERROROPENINGMAP    209
#define STR_DLGGAMEINFO_TIMEREMAINING       210
#define STR_DLGGAMEINFO_OPEN                211
#define STR_DLGGAMEINFO_TOURN               212
#define STR_DLGGAMEINFO_STRICT              213
#define STR_DLGGAMEINFO_AIADV               214
#define STR_DLGGAMESETUP_DEFAULTNAME        215

/* Key setup dialog */
#define STR_DLGKEYSETUP_TITLE               216
#define STR_DLGKEYSETUP_BLURB               217
#define STR_DLGKEYSETUP_DRIVETANK           218
#define STR_DLGKEYSETUP_TURNTANK            219
#define STR_DLGKEYSETUP_GUNRANGE            220
#define STR_DLGKEYSETUP_WEAPONS             221
#define STR_DLGKEYSETUP_VIEW                222
#define STR_DLGKEYSETUP_SCROLL              223
#define STR_DLGKEYSETUP_LEFT                224
#define STR_DLGKEYSETUP_RIGHT               225
#define STR_DLGKEYSETUP_AUTOSLOWDOWN        226
#define STR_DLGKEYSETUP_AUTOGUNSIGHT        227
#define STR_DLGKEYSETUP_FASTER              228
#define STR_DLGKEYSETUP_SLOWER              229
#define STR_DLGKEYSETUP_TURNLEFT            230
#define STR_DLGKEYSETUP_TURNRIGHT           231
#define STR_DLGKEYSETUP_INCREASE            232
#define STR_DLGKEYSETUP_DECREASE            233
#define STR_DLGKEYSETUP_SHOOT               234
#define STR_DLGKEYSETUP_LAYMINE             235
#define STR_DLGKEYSETUP_TANKVIEW            236
#define STR_DLGKEYSETUP_PILLVIEW            237
#define STR_DLGKEYSETUP_SCROLLUP            238
#define STR_DLGKEYSETUP_SCROLLDOWN          240
#define STR_DLGKEYSETUP_SCROLLLEFT          241
#define STR_DLGKEYSETUP_SCROLLRIGHT         242
#define STR_DLGKEYSETUP_NEWKEYFOR           243
#define STR_DLGKEYSETUP_FORWARD             244
#define STR_DLGKEYSETUP_BACKWARD            245
#define STR_DLGKEYSETUP_ROTATELEFT          246
#define STR_DLGKEYSETUP_ROTATERIGHT         247
#define STR_DLGKEYSETUP_INCREASERANGE       248
#define STR_DLGKEYSETUP_DECREASERANGE       249
#define STR_DLGKEYSETUP_SETSHOOT            250
#define STR_DLGKEYSETUP_SETLAYMINE          251
#define STR_DLGKEYSETUP_SETTANKVIEW         252
#define STR_DLGKEYSETUP_SETPILLVIEW         253
#define STR_DLGKEYSETUP_SETSCROLLUP         254
#define STR_DLGKEYSETUP_SETSCROLLDOWN       255
#define STR_DLGKEYSETUP_SETSCROLLLEFT       256
#define STR_DLGKEYSETUP_SETSCROLLRIGHT      257

/* Messages dialog */
#define STR_DLGMSG_BUTTON                   258
#define STR_DLGMSG_ALLPLAYERS               259
#define STR_DLGMSG_ALLALLIES                260
#define STR_DLGMSG_NEARBY                   261
#define STR_DLGMSG_SELECTION                262
#define STR_DLGMSG_SENDPLAYER               263
#define STR_DLGMSG_SENDPLAYERS              264

/* Network info dialog */
#define STR_DLGNETINFO_TITLE                265
#define STR_DLGNETINFO_SERVERADDRESS        266
#define STR_DLGNETINFO_THISGAMEADDRESS      267
#define STR_DLGNETINFO_SERVERPING           268
#define STR_DLGNETINFO_PACKETS              269
#define STR_DLGNETINFO_STATUS               270
#define STR_DLGNETINFO_ERRORS               271

/* Password dialog */
#define STR_DLGPASSWORD_TITLE               272
#define STR_DLGPASSWORD_BLURB               273

/* Set name dialog */
#define STR_DLGSETNAME_TITLE                274
#define STR_DLGSETNAME_BLURB                275

/* Tracker setup dialog */
#define STR_DLGTRACKER_TITLE                276
#define STR_DLGTRACKER_USETRACKER           277
#define STR_DLGTRACKER_TRACKERADDRESS       278
#define STR_DLGTRACKER_TRACKERPORT          279

/* System info dialog */
#define STR_DLGSYSINFO_TITLE                280
#define STR_DLGSYSINFO_CPUUSAGE             281
#define STR_DLGSYSINFO_SIMMODELING          282
#define STR_DLGSYSINFO_COMPROCESSING        283
#define STR_DLGSYSINFO_GRAHPICSDISPLAY      284
#define STR_DLGSYSINFO_AICONTROLPROCESSING  285
#define STR_DLGSYSINFO_TOTAL                286
#define STR_DLGSYSINFO_GRAPHICSFPS          287

/* TCP/IP setup dialog */
#define STR_DLGTCP_TITLE                    288
#define STR_DLGTCP_BLURB                    289
#define STR_DLGTCP_JOIN                     290
#define STR_DLGTCP_REJOIN                   291
#define STR_DLGTCP_NEW                      292
#define STR_DLGTCP_TRACKERSETUP             293
#define STR_DLGTCP_REMEMBER                 294
#define STR_DLGTCP_MACHINENAME              295
#define STR_DLGTCP_THEREUDP                 296
#define STR_DLGTCP_USUDP                    297
#define STR_DLGTCP_NAME                     298
#define STR_DLGTCP_NEWBLURB                 299
#define STR_DLGTCP_JOINBLURB                300
#define STR_DLGTCP_REJOINBLURB              301
#define STR_ERR_DLGTCP_PORTS                302
#define STR_ERR_DLGTCP_NOTRIGHT             303
#define STR_ERR_DLGTCP_SUBCLASS             304

/* Game finder extra */
#define STR_DLGGAMEFINDER_JOINADDRESS       305
#define STR_DLGGAMEFINDER_REFRESH           306
#define STR_DLGGAMEFINDER_PLAYERNAME        307
#define STR_DLGGAMEFINDER_MOTD              308
#define STR_DLGGAMEFINDER_STATUSBLURB       309
#define STR_DLGGAMEFINDER_SELECTEDGAME      310
#define STR_DLGGAMEFINDER_BRAINS            311
#define STR_DLGGAMEFINDER_PASSWORD          312
#define STR_DLGGAMEFINDER_PILLS             313
#define STR_DLGGAMEFINDER_BASES             314
#define STR_DLGGAMEFINDER_HIDDENMINES       315
#define STR_DLGGAMEFINDER_NUMPLAYERS        316
#define STR_DLGGAMEFINDER_VERSION           317
#define STR_DLGGAMEFINDER_GAMETYPE          318
#define STR_DLGGAMEFINDER_PORT              319
#define STR_DLGGAMEFINDER_ADDRESS           320

/* Brain errors */
#define STR_BRAINERR_BRAINDIR               321
#define STR_BRAINERR_LAUNCH                 322
#define STR_BRAINERR_LAUNCHMAIN             323
#define STR_BRAINERR_INIT                   324
#define STR_BRAINERR_EXEC                   325

/* Draw errors */
#define STR_DRAWERROR_CREATEOBJECT          326
#define STR_DRAWERROR_SETCOOPLEVEL          327
#define STR_DRAWERROR_CREATEPRIMARY         328
#define STR_DRAWERROR_GETPIXELFORMAT        329
#define STR_DRAWERROR_GETDESC               330
#define STR_DRAWERROR_TOOFEWCOLOURS         331
#define STR_DRAWERROR_BUFFERCREATE          332
#define STR_DRAWERROR_GETDCFAILED           333
#define STR_DRAWERROR_CLIPPERFAILED         334
#define STR_DRAWERROR_BRUSH                 335
#define STR_DRAWERROR_PEN                   336
#define STR_DRAW_GAMESTARTSIN               337
#define STR_DRAWERROR_RELEASEDC             338
#define STR_DRAW_PILLBOXVIEW                339

/* Font errors */
#define STR_FONTERR_NOCOURIERFONT           340

/* Game front errors */
#define STR_GAMEFRONTERR_CORRUPTPREFS       341
#define STR_GAMEFRONTERR_WINDOW             342
#define STR_GAMEFRONTERR_DDRAW              343
#define STR_GAMEFRONTERR_DSOUND             344
#define STR_GAMEFRONTERR_DINPUT             345
#define STR_GAMEFRONTERR_CURSOR             346
#define STR_GAMEFRONTERR_FONTS              347
#define STR_GAMEFRONT_LANFINDER_TITLE       348
#define STR_GAMEFRONT_TRACKERFINDER_TITLE   349
#define STR_GAMEFRONTERR_JOINGAME           350
#define STR_GAMEFRONTERR_SPAWNSERVER        351
#define STR_GAMEFRONTERR_NETSINGLEPLAYER    352
#define STR_GAMEFRONT_SERVERSTARTMSG        353
#define STR_GAMEFRONT_INPUTERR_CREATE       354
#define STR_GAMEFRONT_INPUTERR_DATAFORMAT   355
#define STR_GAMEFRONT_INPUTERR_COOPLEVEL    356

/* Network client errors */
#define STR_NETCLIENTERR_WINSOCKFAILSTARTUP 357
#define STR_NETCLIENTERR_CREATEUDPFAIL      358
#define STR_NETCLIENTERR_CREATETCPFAIL      359
#define STR_NETCLIENTERR_BINDUDPFAIL        360
#define STR_NETCLIENTERR_CHAINFAIL          361
#define STR_NETCLIENTERR_TRACKERVERSIONFAIL 362
#define STR_NETCLIENTERR_TRACKERDNSFAIL     363
#define STR_NETCLIENTERR_TRACKERCONNECTFAIL 364
#define STR_NETCLIENTERR_TRACKERNOBLOCK     365
#define STR_NETCLIENT_TRACKERGETRESPONSE    366
#define STR_NETCLIENT_TRACKERPROCESSRESPONSE 367
#define STR_NETCLIENTERR_TRACKERNODATA      368
#define STR_NETCLIENT_TRACKERCONNECT        369
#define STR_NETCLIENT_IDLE                  370
#define STR_NETCLIENTERR_BIND               371
#define STR_NETCLIENTERR_BROADCAST          372
#define STR_NETCLIENT_GETRESPONSES          373

/* Sound errors */
#define STR_SOUNDERR_FILENOTFOUND           374
#define STR_SOUNDERR_HARDWAREINUSE          375
#define STR_SOUNDERR_CREATEFAILED           376
#define STR_SOUNDERR_COOPFAILED             377
#define STR_SOUNDERR_PRIMARYBUFFFAIL        378
#define STR_SOUNDERR_LOADSOUNDFAILED        379

/* General WinBolo errors */
#define STR_WBERR_MUTEXCREATE               380
#define STR_WBERR_BRAINLISTLOAD             381
#define STR_WBERR_KEYCLASSSETUP             382
#define STR_WBTIMELIMIT_END                 383
#define STR_WBERR_SAVEMAP                   384

/* Tutorial strings */
#define STR_TUTORIAL01                      385
#define STR_TUTORIAL02                      386
#define STR_TUTORIAL03                      387
#define STR_TUTORIAL04                      388
#define STR_TUTORIAL05                      389
#define STR_TUTORIAL06                      390
#define STR_TUTORIAL07                      391
#define STR_TUTORIAL08                      392
#define STR_TUTORIAL09                      393
#define STR_TUTORIAL10                      394
#define STR_TUTORIAL11                      395
#define STR_TUTORIAL12                      396
#define STR_TUTORIAL13                      397
#define STR_TUTORIAL14                      398
#define STR_TUTORIAL15                      399
#define STR_TUTORIAL16                      400
#define STR_TUTORIAL17                      401
#define STR_TUTORIAL18                      402
#define STR_TUTORIAL19                      403
#define STR_TUTORIAL20                      404
#define STR_TUTORIAL21                      405
#define STR_TUTORIAL22                      406
#define STR_TUTORIAL23                      407
#define STR_TUTORIAL24                      408
#define STR_TUTORIAL25                      409
#define STR_TUTORIAL_START01                410
#define STR_TUTORIAL_START02                411
#define STR_TUTORIAL_START03                412
#define STR_TUTORIAL_START04                413

/* Touch-mode (tablet/mobile) siblings of the tutorial strings whose
 * desktop wording references hardware keys or mouse clicks. The
 * sequencer picks these via tutorialResolveText() when uiModeIsTablet()
 * is true. Only the affected strings have siblings — the rest fall
 * through to the desktop entry unchanged. */
#define STR_TUTORIAL01_TOUCH                468
#define STR_TUTORIAL02_TOUCH                469
#define STR_TUTORIAL03_TOUCH                470
#define STR_TUTORIAL04_TOUCH                471
#define STR_TUTORIAL05_TOUCH                472
#define STR_TUTORIAL06_TOUCH                473
#define STR_TUTORIAL10_TOUCH                474
#define STR_TUTORIAL14_TOUCH                475
#define STR_TUTORIAL16_TOUCH                476
#define STR_TUTORIAL18_TOUCH                477
#define STR_TUTORIAL19_TOUCH                478
#define STR_TUTORIAL21_TOUCH                479
#define STR_TUTORIAL_START01_TOUCH          480
#define STR_TUTORIAL_START04_TOUCH          481

/* LGM (little green man) messages */
#define LGM_MAN_DEAD                        414
#define LGM_NO_BUILD                        415
#define LGM_INSUFFICIENT_TREES              416
#define LGM_NO_PILLS                        417
#define LGM_INSUFFICIENT_MINES              418
#define LGM_BUILDTANK                       419
#define LGM_NO_TREE                         420
#define LGM_PILL_NO_NEED_REPAIR             430
#define LGM_PILL_NO_BUILD_ON_MINE           431
#define LGM_NO_BUILD_UNDER_BOAT             465

/* In-game messages */
#define MESSAGE_ASSISTANT                   432
#define MESSAGE_NEWSWIRE                    433
#define MESSAGE_AI                          434
#define MESSAGE_CHANGENAME                  435
#define MESSAGE_NETSERVER                   436
#define MESSAGE_THIS_COMPUTER               437
#define MESSAGE_CAPTURE_BASE                438
#define MESSAGE_CAPTURE_PILL                439
#define MESSAGE_STOLE_PILL                  440
#define MESSAGE_STOLE_BASE                  441
#define MESSAGE_LGM_DEAD                    442
#define MESSAGE_SAVED_MAP                   443
#define MESSAGE_QUIT_GAME                   444
#define MESSAGE_TIME_LIMIT_EXPIRED          445
#define MESSAGE_TANKSUNK                    446

/* Network errors */
#define NETERR_TRACKERDNS                   447
#define NETERR_JOININIT                     448
#define NETERR_GAMEFULL                     449
#define NETERR_SERVERCONNECT                450
#define NETERR_PASSWORDWRONG                451
#define NETERR_PLAYERNAMEINUSE              452
#define NETERR_NONEWPLAYERS                 453
#define NETERR_CONNECTNOJOIN                454

/* Network status */
#define NET_STATUS_JOINING                  455
#define NET_STATUS_OK                       456
#define NET_STATUS_FAILED                   457
#define NETERR_LOSTCONNECTION               458

/* Misc dialog strings */
#define STR_DLGMSG_TITLE                    459
#define STR_DLGOPENING_BUTTON3              460
#define STR_DLGOPENING_BUTTON4              461
#define STRERR_HELPFILE                     462

/* Player name error */
#define STR_DLGGAMEFINDER_PLAYERWRONG       466
#define NETERR_MAPSERIALIZE                 467

/* -------------------------------------------------------
 * C declarations — not processed by the RC compiler
 * ------------------------------------------------------- */
#ifndef RC_INVOKED

typedef unsigned int langid;

#include "../bolo/global.h"

bool langSetup(void);
void langCleanup(void);
bool langLoadFile(char *filename, char *langName);
void langGetFileName(char *fileName);
char *langGetText(langid id);
char *langGetText2(langid id);

#endif /* RC_INVOKED */

#endif /* _LANG_H */
