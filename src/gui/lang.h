/*
 * Copyright (c) 1998-2026 John Morrison.
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
#define STR_DLGOPENING_OPTION2              154
#define STR_OK                              157
#define STR_DLGOPENING_BUTTON2              158

/* Language dialog */
#define STR_DLGLANG_NAME                    161
#define STR_DLGLANG_AUTHOR_CAPTION          162
#define STR_DLGLANG_AUTHOR                  163
#define STR_DLGLANG_NOTES_CAPTION           166
#define STR_DLGLANG_NOTES                   167
#define STR_DLGLANG_DEFAULTNOTE             168

/* About dialog */
#define STR_DLGABOUT_TITLE                  169

/* Alliance dialog */
#define STR_DLGALLIANCE_TITLE               171
#define STR_DLGALLIANCE_ACCEPT              172
#define STR_DLGALLIANCE_DECLINE             173
#define STR_DLGALLIANCE_BLURB               174

/* Game finder dialog */
#define STR_YES                             179
#define STR_NO                              180

/* Game info dialog */
#define STR_DLGGAMEINFO_TITLE               185
#define STR_DLGGAMEINFO_MAPNAME             186
#define STR_DLGGAMEINFO_NUMPLAYERS          187
#define STR_DLGGAMEINFO_GAMETYPE            188
#define STR_DLGGAMEINFO_HIDDENMINES         189
#define STR_DLGGAMEINFO_TIMELIMIT           191

/* Game setup dialog */
#define STR_CANCEL                          193
#define STR_DLGGAMESETUP_TITLE              194
#define STR_DLGGAMESETUP_CHOOSEMAP          195
#define STR_DLGGAMESETUP_RADIO1             198
#define STR_DLGGAMESETUP_RADIO2             199
#define STR_DLGGAMESETUP_RADIO3             200
#define STR_DLGGAMESETUP_HIDDENMINES        201
#define STR_DLGGAMESETUP_PASSWORD           204
#define STR_DLGGAMESETUP_MINUTES            205
#define STR_DLGGAMESETUP_SECONDS            206
#define STR_DLGGAMESETUP_STARTDELAY         207
#define STR_DLGGAMESETUP_TIMELIMIT          208
#define STR_DLGGAMEINFO_TIMEREMAINING       210
#define STR_DLGGAMEINFO_OPEN                211
#define STR_DLGGAMEINFO_TOURN               212
#define STR_DLGGAMEINFO_STRICT              213
#define STR_DLGGAMEINFO_AIADV               214
#define STR_DLGGAMESETUP_DEFAULTNAME        215

/* Key setup dialog */
#define STR_DLGKEYSETUP_TITLE               216
#define STR_DLGKEYSETUP_TAB_KEYBOARD        1492
#define STR_DLGKEYSETUP_DRIVETANK           218
#define STR_DLGKEYSETUP_GUNRANGE            220
#define STR_DLGKEYSETUP_WEAPONS             221
#define STR_DLGKEYSETUP_VIEW                222
#define STR_DLGKEYSETUP_SCROLL              223
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
#define STR_DLGKEYSETUP_BASEVIEW            2003
#define STR_DLGKEYSETUP_ALLYVIEW            2004
#define STR_DLGKEYSETUP_OVERVIEWZOOM        1993
#define STR_DLGKEYSETUP_OVERVIEWFOLLOW      2005
#define STR_DLGKEYSETUP_OVERVIEWZOOMIN      2006
#define STR_DLGKEYSETUP_OVERVIEWZOOMOUT     2007
#define STR_DLGKEYSETUP_SCROLLUP            238
#define STR_DLGKEYSETUP_SCROLLDOWN          240
#define STR_DLGKEYSETUP_SCROLLLEFT          241
#define STR_DLGKEYSETUP_SCROLLRIGHT         242

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
#define STR_DLGNETINFO_SERVERPING           268
#define STR_DLGNETINFO_STATUS               270
#define STR_DLGNETINFO_ERRORS               271

/* Password dialog */
#define STR_DLGPASSWORD_TITLE               272
#define STR_DLGPASSWORD_BLURB               273

/* Set name dialog */
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
#define STR_DLGSYSINFO_TOTAL                286

/* TCP/IP setup dialog */
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

/* Game finder extra */

/* Brain errors */

/* Draw errors */

/* Font errors */

/* Game front errors */
#define STR_GAMEFRONT_LANFINDER_TITLE       348
#define STR_GAMEFRONT_TRACKERFINDER_TITLE   349
#define STR_GAMEFRONTERR_JOINGAME           350

/* Network client errors */

/* Sound errors */

/* General WinBolo errors */
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
 * sequencer picks these via tutorialResolveSegments() when
 * uiModeIsTablet() is true. Only the affected strings have siblings —
 * the rest fall through to the desktop entry unchanged. */
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

/* Controller-mode siblings of the tutorial strings whose desktop
 * wording references keyboard keys or the mouse. The sequencer picks
 * these when the active input is a gamepad, mirroring the _TOUCH
 * mechanism above. STR_TUTORIAL_RESPAWN1 is the message shown when
 * the player respawns at the second tutorial start (near the shore)
 * instead of out at sea. */
#define STR_TUTORIAL01_CTRL                 1830
#define STR_TUTORIAL02_CTRL                 1831
#define STR_TUTORIAL03_CTRL                 1832
#define STR_TUTORIAL04_CTRL                 1833
#define STR_TUTORIAL05_CTRL                 1834
#define STR_TUTORIAL06_CTRL                 1835
#define STR_TUTORIAL10_CTRL                 1836
#define STR_TUTORIAL14_CTRL                 1837
#define STR_TUTORIAL16_CTRL                 1838
#define STR_TUTORIAL18_CTRL                 1839
#define STR_TUTORIAL19_CTRL                 1840
#define STR_TUTORIAL21_CTRL                 1841
#define STR_TUTORIAL_START04_CTRL           1842
#define STR_TUTORIAL_RESPAWN1               1843

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
#define STRERR_HELPFILE                     462
#define STR_DLGSKIN_BLURB                   464

/* Player name error */
#define NETERR_MAPSERIALIZE                 467

/* System Info panel additions */
#define STR_DLGSYSINFO_FRAMERATE            482
#define STR_DLGSYSINFO_GRAPHICS             483
#define STR_DLGSYSINFO_AITANKS              484

/* Network Info panel additions */
#define STR_DLGNETINFO_SERVER               485
#define STR_DLGNETINFO_THISGAME             486
#define STR_DLGNETINFO_PINGGRAPH            487
#define STR_DLGNETINFO_KBIN                 488
#define STR_DLGNETINFO_KBOUT                489
#define STR_DLGNETINFO_PACKETS_RATE         490
#define STR_DLGNETINFO_KB_RATE              491

/* Game Info panel additions */
#define STR_DLGGAMEINFO_COPYSEED            492
#define STR_DLGGAMEINFO_AILABEL             493
#define STR_DLGGAMEINFO_FULLADV             494
#define STR_DLGGAMEINFO_UNLIMITED           495

/* Players panel */
#define STR_DLGPLAYERS_TITLE                496
#define STR_DLGPLAYERS_ALL                  497
#define STR_DLGPLAYERS_NONE                 498
#define STR_DLGPLAYERS_ALLIES               499
#define STR_DLGPLAYERS_NEARBY               500
#define STR_LEAVE_ALLIANCE                  501
#define STR_REQUEST_ALLIANCE                502
#define STR_ALLOW_NEW_PLAYERS               503

/* About modal */
#define STR_DLGABOUT_VERSION                504
#define STR_DLGABOUT_COPYRIGHT              505
#define STR_DLGABOUT_BOLOCOPYRIGHT          506

/* Join Game confirmation modal */
#define STR_DLGJOIN_TITLE                   507
#define STR_DLGJOIN_BLURB                   508
#define STR_DLGJOIN_BUTTON                  509

/* Change Player Name modal */
#define STR_DLGCHANGENAME_TITLE             510

/* Key Setup modal additions */
#define STR_DLGKEYSETUP_NONE_VAL            511
#define STR_DLGKEYSETUP_PRESSAKEY           512
#define STR_DLGKEYSETUP_PRESS_OR_CANCEL     513
#define STR_DLGKEYSETUP_CHANGE              514
#define STR_DLGKEYSETUP_COL_ACTION          515
#define STR_DLGKEYSETUP_COL_KEY             516
#define STR_DLGKEYSETUP_QUICKKEYS           517
#define STR_DLGKEYSETUP_TREE                518
#define STR_DLGKEYSETUP_ROAD                519
#define STR_DLGKEYSETUP_WALL                520
#define STR_DLGKEYSETUP_QUICKPILLBOX        521
#define STR_DLGKEYSETUP_QUICKMINE           522

/* Settings panel */
#define STR_DLGSETTINGS_TITLE               523
#define STR_DLGSETTINGS_PLAYER              524
#define STR_DLGSETTINGS_PLAYERNAME          525
#define STR_DLGSETTINGS_APPLY               526
#define STR_DLGSETTINGS_SETKEYS             527
#define STR_DLGSETTINGS_DISPLAY             528
#define STR_DLGSETTINGS_WINDOWSIZE          529
#define STR_DLGSETTINGS_RELSTEER            530
#define STR_DLGSETTINGS_RELSTEER_TIP        531
#define STR_DLGSETTINGS_TABLETMODE          532
#define STR_DLGSETTINGS_LABELS              533
#define STR_DLGSETTINGS_MSGNAMES            534
#define STR_DLGSETTINGS_TANKLABELS          535
#define STR_DLGSETTINGS_SOUND               536
#define STR_DLGSETTINGS_MESSAGES            537
#define STR_DLGSETTINGS_GAME                538

/* Menu bar */
#define STR_MENU_FILE                       539
#define STR_MENU_NEW                        540
#define STR_MENU_SAVE_MAP                   541
#define STR_MENU_MAP_OVERVIEW               1987
#define STR_MENU_OVERVIEW_IN_WINDOW         1990
#define STR_MENU_EXIT                       542
#define STR_MENU_EDIT                       543
#define STR_MENU_FRAME_RATE                 544
#define STR_MENU_WINDOW_SIZE                545
#define STR_MENU_NORMAL                     546
#define STR_MENU_DOUBLE                     547
#define STR_MENU_TRIPLE                     548
#define STR_MENU_QUAD                       549
#define STR_MENU_CUSTOM_RESIZABLE           550
#define STR_MENU_REQUIRES                   551
#define STR_MENU_SMOOTH_SCROLLING           552
#define STR_MENU_AUTO_SCROLLING             553
#define STR_MENU_SHOW_GUNSIGHT              554
#define STR_MENU_LETTERBOX_GRAY             1493
#define STR_MENU_MSG_NAMES_SUB              555
#define STR_MENU_TANK_LABELS_SUB            556
#define STR_NONE                            557
#define STR_SHORT                           558
#define STR_LONG                            559
#define STR_MENU_NO_OWN_LABEL               560
#define STR_MENU_PILLBOX_LABELS             561
#define STR_MENU_BASE_LABELS                562
#define STR_MENU_HIDE_MAIN                  563
#define STR_MENU_DEVICE                     564
#define STR_MENU_DESKTOP                    565
#define STR_MENU_WINBOLO                    566
#define STR_MENU_SETKEYS                    567
#define STR_MENU_SOUND_EFFECTS              568
#define STR_MENU_BACKGROUND_SOUND           569
#define STR_MENU_SOUND_KEEPALIVE            570
#define STR_MENU_NEWSWIRE_MSGS              571
#define STR_MENU_ASSISTANT_MSGS             572
#define STR_MENU_AI_MSGS                    573
#define STR_MENU_NETSTATUS_MSGS             574
#define STR_MENU_NETDEBUG_MSGS              575
#define STR_MENU_SETTINGS                   576
#define STR_MENU_PLAYERS                    577
#define STR_MENU_SEND_MESSAGE               578
#define STR_MENU_SELECT_ALL                 579
#define STR_MENU_SELECT_NONE                580
#define STR_MENU_SELECT_ALLIES              581
#define STR_MENU_SELECT_NEARBY              582
#define STR_MENU_BRAINS                     583
#define STR_MENU_MANUAL                     584
#define STR_MENU_HELP                       585
#define STR_MENU_ABOUT                      586
#define STR_MENU_LEAVE_GAME                 587
#define STR_BRAINSETTINGS_NONE              588
#define STR_BRAINSETTINGS_TITLE             589

/* Map overview pop-out */
#define STR_OVERVIEW_FOLLOWING              1988
#define STR_OVERVIEW_FREE                   1989
/* Settings panel additions (pre-game) */
#define STR_DLGSETTINGS_TUTORIAL            590
#define STR_DLGSETTINGS_PLAY_TUTORIAL       591
#define STR_DLGSETTINGS_SHOW_ON_MAIN        592
#define STR_DLGSETTINGS_CRASH_REPORTING     593
#define STR_DLGSETTINGS_ENABLE_CRASH        594
#define STR_DLGSETTINGS_CRASH_HELP          595
#define STR_DLGSETTINGS_CRASH_NEXTLAUNCH    596
#define STR_CLOSE                           597
#define STR_DLGSETTINGS_WINTITLE            598
#define STR_DLGKEYSETUP_WINTITLE            599

/* Lobby dialog */
#define STR_DLGLOBBY_WINTITLE               600
#define STR_UNKNOWN                         601
#define STR_DLGGAMEINFO_AIFULL              602
#define STR_DLGLOBBY_HIDDEN                 603
#define STR_DLGLOBBY_VISIBLE                604
#define STR_DLGLOBBY_DOWNLOADING            605
/* Shown in place of the download bar while the client is re-joining after
   a map change: the server has not started sending the new map yet, so
   there is no progress to report. */
#define STR_DLGLOBBY_AWAITING_MAP           1946
#define STR_DLGLOBBY_MAP_UNAVAILABLE        606
#define STR_DLGLOBBY_PILLBOXES              607
#define STR_DLGLOBBY_BASES                  608
#define STR_DLGLOBBY_STARTS                 609
#define STR_DLGLOBBY_SKIPMAP                610
#define STR_DLGLOBBY_CANCELSKIP             611
#define STR_DLGLOBBY_VOTES                  612
#define STR_DLGLOBBY_CHAT                   613
#define STR_DLGLOBBY_CHAT_GENERAL           1497
#define STR_DLGLOBBY_CHAT_TEAM              1498
/* Compass octant labels for a lobby player's reserved map start */
#define STR_COMPASS_N                       1499
#define STR_COMPASS_NE                      1500
#define STR_COMPASS_E                       1501
#define STR_COMPASS_SE                      1502
#define STR_COMPASS_S                       1503
#define STR_COMPASS_SW                      1504
#define STR_COMPASS_W                       1505
#define STR_COMPASS_NW                      1506
#define STR_COMPASS_C                       1507
/* "Unassigned" entry in the lobby player-list start combo (release) */
#define STR_DLGLOBBY_START_UNASSIGNED       1508
/* Map-preview label for a start with no reservation (a click target) */
#define STR_DLGLOBBY_START_OPEN             1509
/* Team-chat system lines when a player joins/leaves the local team */
#define STR_DLGLOBBY_TEAM_JOINED_FMT        1510
#define STR_DLGLOBBY_TEAM_LEFT_FMT          1511
/* Lobby roster header above the spectator list ({number} = live count) */
#define STR_DLGLOBBY_SPECTATORS_FMT         1848
/* Sender-name tag prepended to a spectator's lobby chat line */
#define STR_DLGLOBBY_SPECTATOR_TAG          1866
/* Map-preview start-picker tooltips and the assign-to-someone menu */
#define STR_STARTPICK_TIP_FREE_HOST         1512
#define STR_STARTPICK_TIP_FREE              1513
#define STR_STARTPICK_TIP_HELD_HOST         1514
#define STR_STARTPICK_TIP_HELD              1515
#define STR_STARTPICK_YOU                   1516
#define STR_STARTPICK_SWAP_WITH             1517
#define STR_STARTPICK_ASSIGN_TO             1518
#define STR_STARTPICK_SLOT_FALLBACK         1519
#define STR_STARTPICK_TIP_OFFSIDE           2116
#define STR_STARTPICK_TIP_OFFSIDE_HOST      2117
/* Held start you may join (LOBBY_SHARED_STARTS): non-host / host. */
#define STR_STARTPICK_TIP_HELD_JOIN         2127
#define STR_STARTPICK_TIP_HELD_JOIN_HOST    2128

/* Player-row badge tooltips */
#define STR_PLAYER_TIP_AI                   1520
#define STR_PLATFORM_WINDOWS                1521
#define STR_PLATFORM_LINUX                  1522
#define STR_PLATFORM_MACOS                  1523
#define STR_PLATFORM_IOS                    1524
#define STR_PLATFORM_ANDROID                1525
#define STR_PLATFORM_STEAMDECK              1526
#define STR_PLATFORM_WEB                    1527
#define STR_PLAYER_TIP_SUPPORTER_FMT        1528
#define STR_PLAYER_TIP_WBN_VERIFIED         1529
#define STR_PLAYER_TIP_STEAM_LINKED         1530
#define STR_PLAYER_TIP_STEAM_BUILD          1531
#define STR_DLGLOBBY_READY                  614
#define STR_DLGLOBBY_UNREADY                615
#define STR_DLGLOBBY_BALANCE_TEAMS          616
#define STR_DLGLOBBY_APPLY_BALANCE          617
#define STR_DLGLOBBY_DISMISS                618
#define STR_DLGLOBBY_LEAVE                  619
#define STR_DLGLOBBY_LEAVE_TITLE            620
#define STR_DLGLOBBY_LEAVE_BLURB            621
#define STR_DLGLOBBY_BOT_FMT                622
#define STR_DLGLOBBY_YOU_FMT                623
#define STR_DLGLOBBY_STARTING_FMT           624
#define STR_DLGLOBBY_LOSTCONNECTION         625
#define STR_DLGLOBBY_ME                     626
#define STR_DLGLOBBY_PLAYER_COL             627
#define STR_DLGLOBBY_PING_COL               628
#define STR_DLGLOBBY_TEAM_COL               629
#define STR_DLGLOBBY_READY_COL              630
#define STR_DLGLOBBY_SLOT_COL               631
#define STR_DLGLOBBY_NAME_COL               632
#define STR_DLGLOBBY_ACTION_COL             633
#define STR_DLGLOBBY_ADDBOT                 634
#define STR_DLGLOBBY_REMOVE                 635
#define STR_DLGLOBBY_MAP_TAB                636
#define STR_DLGLOBBY_GAME_LBL               637
#define STR_DLGLOBBY_MINES_LBL              638
#define STR_DLGLOBBY_AI_LBL                 639
#define STR_DLGLOBBY_TIME_LBL               640
#define STR_DLGLOBBY_TIME_HMS               642
#define STR_DLGLOBBY_TIME_MS                643
#define STR_DLGLOBBY_TIME_S                 644
#define STR_DLGLOBBY_MAP_LBL                645

/* Game Setup dialog additions */
#define STR_DLGGAMESETUP_WINTITLE           646
#define STR_DLGGAMESETUP_SELECTAMAP         647
#define STR_BACK                            648
#define STR_DLGGAMESETUP_BASES_STARTS       649
#define STR_DLGGAMESETUP_PILLBOXES_FMT      650
#define STR_DLGGAMESETUP_CHANGEMAP          651
#define STR_DLGGAMESETUP_GAMETYPE_LBL       652
#define STR_DLGGAMESETUP_OPENGAME_SHORT     653
#define STR_DLGGAMESETUP_HIDDENMINES_SHORT  654
#define STR_DLGGAMESETUP_AICOMPPLAYERS      655
#define STR_DLGGAMESETUP_ALLOW              656
#define STR_DLGGAMESETUP_ADVANTAGE          657
#define STR_DLGGAMESETUP_FULLMAP            658
#define STR_DLGGAMESETUP_TIMELIMIT_SHORT    659
#define STR_DLGGAMESETUP_TEAMSETUP          660
#define STR_DLGGAMESETUP_AINUM              661
#define STR_DLGGAMESETUP_NOBRAINS           662
#define STR_DLGGAMESETUP_BRAIN_COL          663
#define STR_DLGGAMESETUP_YOU                664
#define STR_DLGGAMESETUP_BOT_FMT            665
#define STR_DLGGAMESETUP_NOAI               666
#define STR_DLGGAMESETUP_ALLOWAI            667
#define STR_DLGGAMESETUP_ALLOWADV           668
#define STR_DLGGAMESETUP_ALLOWFULL          669
#define STR_DLGGAMESETUP_NOPREVIEW          670
#define STR_DLGGAMESETUP_STARTGAME          671
#define STR_DLGGAMESETUP_STRICT_SHORT       672
#define STR_DLGTCP_WINTITLE                 673
#define STR_ERR_TITLE                       674

/* Game Browser dialog */
#define STR_DLGBROWSER_WINTITLE             675
#define STR_DLGBROWSER_REFRESH              676
#define STR_DLGBROWSER_LOADING              677
#define STR_DLGBROWSER_TRACKER_INSTRUCTION  678
#define STR_DLGBROWSER_LAN_INSTRUCTION      679
#define STR_DLGBROWSER_GAMES_LOADED         680
#define STR_DLGBROWSER_NO_GAMES             681
#define STR_DLGBROWSER_SEARCH_FAILED        682
#define STR_DLGBROWSER_SEARCHING            683
#define STR_DLGBROWSER_COL_SERVER           684
#define STR_DLGBROWSER_COL_MAP              685
#define STR_DLGBROWSER_COL_PLAYERS          686
#define STR_DLGBROWSER_COL_TYPE             687
#define STR_DLGBROWSER_COL_AI               688
#define STR_DLGBROWSER_COL_BASES            689
#define STR_DLGBROWSER_COL_PILLS            690
#define STR_DLGBROWSER_COL_PING             691
#define STR_DLGBROWSER_FILTER               692
#define STR_DLGBROWSER_FILTER_ALLTYPES      693
#define STR_DLGBROWSER_FILTER_UNLOCKED      694
#define STR_DLGBROWSER_FILTER_ALLLOBBY      695
#define STR_DLGBROWSER_FILTER_INLOBBY       696
#define STR_DLGBROWSER_FILTER_STARTING      697
#define STR_DLGBROWSER_FILTER_INGAME        1793
#define STR_DLGBROWSER_COL_VER              1794
#define STR_DLGBROWSER_TYPE_AI              1795
#define STR_DLGBROWSER_TYPE_MINES           1796
#define STR_DLGBROWSER_LOCK_NONEWPLAYERS    1797
#define STR_DLGBROWSER_LOCK_PASSWORD        1798
#define STR_DLGBROWSER_AUTOLOCK_HINT        1799
#define STR_DLGBROWSER_SPECTATE             1800
#define STR_DLGBROWSER_AUTO_REFRESH         1801
#define STR_DLGBROWSER_ST_LOBBY             1802
#define STR_DLGBROWSER_ST_INGAME            1803
#define STR_DLGBROWSER_SELECT_SERVER        1814
#define STR_DLGBROWSER_PREVIEW_UNAVAIL      1815
#define STR_DLGBROWSER_PREVIEW_ENLARGE      1816
#define STR_DLGBROWSER_ENLARGE_BTN          1829
#define STR_DLGBROWSER_AI_PLAYERS           1817
#define STR_DLGBROWSER_WBN_PLAYERS          1818
#define STR_DLGBROWSER_ST_LOCKED            1819
#define STR_DLGBROWSER_ST_NORESP            1820
#define STR_DLGBROWSER_RND_ABBR             1821
#define STR_DLGBROWSER_TYPE_TOURN_ABBR      1822
#define STR_DLGBROWSER_STATUS_PINGING       698
#define STR_DLGBROWSER_STATUS               699
#define STR_DLGBROWSER_NEWGAME              700
#define STR_DLGBROWSER_PLAYER_NAME_BTN      701
#define STR_DLGBROWSER_MANUAL               702
#define STR_DLGBROWSER_ERR_VERSION          703
#define STR_DLGBROWSER_ERR_NEEDNAME         704
#define STR_DLGBROWSER_AI_ADV               706
#define STR_DLGBROWSER_AI_FULL              707

/* WBN Log Browser dialog */
#define STR_DLGWBN_WINTITLE                 708
#define STR_DLGWBN_TITLE                    709
#define STR_DLGWBN_TAB_RECENT               710
#define STR_DLGWBN_TAB_TOPRATED             711
#define STR_DLGWBN_TAB_MOSTDOWNLOADED       712
#define STR_DLGWBN_TAB_SEARCH               713
#define STR_DLGWBN_LOADING                  714
#define STR_DLGWBN_NOCONNECT                715
#define STR_DLGWBN_MINPLAYERS               716
#define STR_DLGWBN_SEARCHFILTERS            717
#define STR_DLGWBN_HINT_PLAYER              718
#define STR_DLGWBN_HINT_MAP                 719
#define STR_DLGWBN_SEARCH_BTN               720
#define STR_DLGWBN_ERROR                    721
#define STR_DLGWBN_COL_MAP                  722
#define STR_DLGWBN_COL_TYPE                 723
#define STR_DLGWBN_COL_PLAYERS              724
#define STR_DLGWBN_COL_RATING               725
#define STR_DLGWBN_COL_SIZE                 726
#define STR_DLGWBN_COL_DATE                 727
#define STR_DLGWBN_PLAYERS_LBL              728
#define STR_DLGWBN_NUMPLAYERS_FMT           729
#define STR_DLGWBN_DURATION                 730
#define STR_DLGWBN_RATING                   731
#define STR_DLGWBN_DOWNLOADS_FMT            732
#define STR_DLGWBN_SIZE                     733
#define STR_DLGWBN_COMMENTS_FMT             734
#define STR_DLGWBN_NOCOMMENTS               735
#define STR_DLGWBN_ADDCOMMENT               736
#define STR_DLGWBN_HINT_COMMENT             737
#define STR_DLGWBN_POST                     738
#define STR_DLGWBN_POSTED                   739
#define STR_DLGWBN_LOADINGDETAIL            740
#define STR_DLGWBN_DOWNLOADING              741
#define STR_DLGWBN_VIEWLOG                  742
#define STR_DLGWBN_NOLOG                    743
#define STR_DLGWBN_PREV                     744
#define STR_DLGWBN_NEXT                     745
#define STR_DLGWBN_PAGE                     746
#define STR_DLGWBN_OPENFILE                 747
#define STR_DLGWBN_PARSERR                  748
#define STR_DLGWBN_NETERR                   749
#define STR_DLGWBN_LOADERR                  750
#define STR_DLGWBN_DOWNLOAD_FAILED          751
#define STR_DLGWBN_NOSAVEPATH               752
#define STR_DLGWBN_SELECTPROMPT             753
#define STR_DLGWBN_FETCHERR                 754
#define STR_DLGWBN_UNKNOWN_ERR              755
#define STR_DLGWBN_FILEFILTER               756
#define STR_DLGWBN_TAB_MYGAMES              1863
#define STR_DLGWBN_MYGAMES_SIGNIN           1864
#define STR_DLGWBN_MYGAMES_NONE             1865

/* Lobby "Last round" panel — between-rounds scoreboard + awards.
 * 1866 reserved for STR_DLGLOBBY_SPECTATOR_TAG (merges from main). */
#define STR_DLGLOBBY_LASTROUND_TITLE        1899
#define STR_DLGLOBBY_LASTROUND_COL_NAME     1900
#define STR_DLGLOBBY_LASTROUND_COL_KILLS    1901
#define STR_DLGLOBBY_LASTROUND_COL_DEATHS   1902
#define STR_DLGLOBBY_LASTROUND_COL_BASE     1903
#define STR_DLGLOBBY_LASTROUND_COL_PILL     1904
#define STR_DLGLOBBY_LASTROUND_COL_DMG      1905
#define STR_DLGLOBBY_LASTROUND_COL_BUILDS   1874
#define STR_DLGLOBBY_LASTROUND_AWARDS       1947
#define STR_DLGLOBBY_LASTROUND_NOPLAYER     1876
#define STR_DLGLOBBY_LASTROUND_NEMESIS_FMT  1877
#define STR_DLGLOBBY_AWARD_MOST_KILLS       1878
#define STR_DLGLOBBY_AWARD_MOST_DEATHS      1879
#define STR_DLGLOBBY_AWARD_BEST_KD          1880
#define STR_DLGLOBBY_AWARD_MOST_BASE        1881
#define STR_DLGLOBBY_AWARD_MOST_PILL        1882
#define STR_DLGLOBBY_AWARD_NEMESIS          1883
#define STR_DLGLOBBY_AWARD_DEMOLITION       1884
#define STR_DLGLOBBY_AWARD_SHARPSHOOTER     1885
#define STR_DLGLOBBY_AWARD_WARMONGER        1886
#define STR_DLGLOBBY_AWARD_SURVIVOR         1887
#define STR_DLGLOBBY_AWARD_ENGINEER         1888
#define STR_DLGLOBBY_AWARD_SAPPER           1889
#define STR_DLGLOBBY_AWARD_FISH_FOOD        1890
#define STR_DLGLOBBY_AWARD_LGM_HUNTER       1891
#define STR_DLGLOBBY_AWARD_CANNON_FODDER    1892
#define STR_DLGLOBBY_AWARD_LUMBERJACK       1893
#define STR_DLGLOBBY_AWARD_WASTEFUL         1894
#define STR_DLGLOBBY_AWARD_BIGGEST_FUMBLE   1895
#define STR_DLGLOBBY_LASTROUND_BTN          1896
#define STR_DLGLOBBY_LASTROUND_COL_LGMK     1897
#define STR_DLGLOBBY_LASTROUND_COL_LGMD     1898
#define STR_DLGLOBBY_LASTROUND_COL_SCNSCORE 2327  /* scenario score column, untitled */

/* Lobby "Last round" panel — the round's highlight clips. */
#define STR_DLGLOBBY_HL_HEADER               1922
#define STR_DLGLOBBY_HL_NONE                 1923
#define STR_DLGLOBBY_HL_AWARD_FMT            1924
#define STR_DLGLOBBY_HL_AWARD_VS_FMT         1925
#define STR_DLGLOBBY_HL_WIPE_FMT             1926
#define STR_DLGLOBBY_HL_STEAL_FMT            1927
#define STR_DLGLOBBY_HL_LGM_FMT              1928
#define STR_DLGLOBBY_HL_FUMBLE_FMT           1929
#define STR_DLGLOBBY_HL_DROWN_PILLS_FMT      1930
#define STR_DLGLOBBY_HL_DROWN_FMT            1931
#define STR_DLGLOBBY_HL_COLLAPSE_FMT         1932
#define STR_DLGLOBBY_HL_COLLAPSE_NOACTOR_FMT 1933
#define STR_DLGLOBBY_HL_TURNING_FMT          1934
#define STR_DLGLOBBY_HL_TURNING_NOACTOR_FMT  1935
#define STR_DLGLOBBY_HL_GENERIC              1936
#define STR_DLGLOBBY_HL_PICKUP_FMT           1944
#define STR_DLGLOBBY_HL_DENSITY_FMT          1945

/* Lobby "Last round" panel — the reel's delivery state. */
#define STR_DLGLOBBY_REEL_WAITING            1938
#define STR_DLGLOBBY_REEL_DOWNLOADING        1939
#define STR_DLGLOBBY_REEL_DISABLED           1940
#define STR_DLGLOBBY_REEL_NONE               1941
#define STR_DLGLOBBY_REEL_TOO_LARGE          1942

/* SetName dialog additions */
#define STR_DLGSETNAME_WINTITLE             757
#define STR_DLGSETNAME_WBN_LOCKED           758
#define STR_DLGSETNAME_PLEASE_ENTER         759
#define STR_DLGSETNAME_BLANK_ERR            760
#define STR_DLGSETNAME_STAR_ERR             761
#define STR_DLGSETNAME_INUSE_ERR            762

/* Skin selection dialog additions */
#define STR_DLGSKIN_WINTITLE                763
#define STR_DLGSKIN_NOSKIN                  764
#define STR_DLGSKIN_NA                      765
#define STR_DLGSKIN_SELECT                  766
#define STR_DLGSKIN_NAME_LBL                767
#define STR_DLGSKIN_AUTHOR_LBL              768
#define STR_DLGSKIN_NOTES_LBL               769
#define STR_DLGSKIN_LOADERR                 770

/* Skin section of the Display & Sound settings tab */
#define STR_DLGSETTINGS_SKIN                1948
#define STR_DLGSKIN_DEFAULT                 1949
#define STR_DLGSKIN_SRC_BUILTIN             1950
#define STR_DLGSKIN_SRC_USER                1951
#define STR_DLGSKIN_SRC_WORKSHOP            1952
#define STR_DLGSKIN_DOWNLOADING             1953
#define STR_DLGSKIN_OPENFOLDER              1954
#define STR_DLGSKIN_BROWSE_WORKSHOP         1955
#define STR_DLGSKIN_PUBLISH                 1956
#define STR_DLGSKIN_TILEDETAIL              1957
#define STR_DLGSKIN_TILEDETAIL_CLASSIC      1958
#define STR_DLGSKIN_TILEDETAIL_MATCHZOOM    1959
#define STR_DLGSKIN_TILEDETAIL_HIGH         1960
#define STR_DLGSKIN_ANIMSMOOTH              1961
#define STR_DLGSKIN_ANIMSMOOTH_CLASSIC      1962
#define STR_DLGSKIN_ANIMSMOOTH_PIXEL        1963
#define STR_DLGSKIN_ANIMSMOOTH_SMOOTH       1964
#define STR_DLGSKIN_SMOOTHSHELLS            1965
#define STR_DLGSKIN_TEXFILTER               1966
#define STR_DLGSKIN_TEXFILTER_NEAREST       1967
#define STR_DLGSKIN_TEXFILTER_LINEAR        1968
#define STR_DLGSKIN_TEXFILTER_PIXELART      1969
#define STR_DLGSKIN_TILEDETAIL_ONESIZE_TIP  1970
#define STR_DLGSKIN_TEXFILTER_TIP           1971
#define STR_DLGSKIN_PUBLISH_HEADING         1972
#define STR_DLGSKIN_PUBLISH_NAME            1973
#define STR_DLGSKIN_PUBLISH_DESC            1974
#define STR_DLGSKIN_PUBLISH_UPDATE          1975
#define STR_DLGSKIN_PUBLISH_NEW             1976
#define STR_DLGSKIN_PUBLISH_GO              1977
#define STR_DLGSKIN_PUBLISH_WORKING         1978
#define STR_DLGSKIN_PUBLISH_DONE            1979
#define STR_DLGSKIN_PUBLISH_FAILED          1980
#define STR_DLGSKIN_PUBLISH_LEGAL           1981
#define STR_DLGSKIN_PUBLISH_OPENITEM        1982
#define STR_DLGSKIN_PUBLISH_NEEDUSER        1983
#define STR_DLGSKIN_TILEDETAIL_PARTIAL_TIP  1984
#define STR_DLGSKIN_RECFILTER_LBL           1985
#define STR_DLGSKIN_RECOMMENDED_TAG         1986
/* Fog of war style: the dropdown, its four looks, and a line each saying
 * what the look does. Sits with the other graphics settings because it is
 * one - it changes how the client paints ground the player is remembering,
 * and changes nothing the server sends. */
#define STR_DLGSKIN_FOGSTYLE                2334
#define STR_DLGSKIN_FOGSTYLE_TIP            2335
#define STR_DLGSKIN_FOGSTYLE_GREY           2336
#define STR_DLGSKIN_FOGSTYLE_GREY_TIP       2337
#define STR_DLGSKIN_FOGSTYLE_DARK           2338
#define STR_DLGSKIN_FOGSTYLE_DARK_TIP       2339
#define STR_DLGSKIN_FOGSTYLE_DARKROADS      2340
#define STR_DLGSKIN_FOGSTYLE_DARKROADS_TIP  2341
#define STR_DLGSKIN_FOGSTYLE_NONE           2342
#define STR_DLGSKIN_FOGSTYLE_NONE_TIP       2343
/* Shown under the dropdown while None is picked, because None is the one
 * choice a player could read as switching fog of war off. It does not: the
 * server still decides what this client is sent and what it may draw. */
#define STR_DLGSKIN_FOGSTYLE_NONE_NOTE      2344

/* Tracker Setup dialog */
#define STR_DLGTRACKER_WINTITLE             771
#define STR_DLGTRACKER_INVALIDPORT          772

/* WinBolo.net section */
#define STR_DLGWBN_CHECKING                 773
#define STR_DLGWBN_LABEL                    774
#define STR_DLGWBN_SIGNED_IN                775
#define STR_DLGWBN_EXPIRES                  776
#define STR_DLGWBN_SIGN_OUT                 777
#define STR_DLGWBN_NOT_SIGNED_IN            778
#define STR_DLGWBN_SIGN_IN_BTN              779
#define STR_DLGWBN_SIGNIN_TITLE             780
#define STR_DLGWBN_SIGNIN_BLURB             781
#define STR_DLGWBN_USERNAME                 782
#define STR_DLGWBN_PASSWORD                 783
#define STR_DLGWBN_SIGNINGIN                784
#define STR_DLGWBN_SIGNIN_OK                785
#define STR_DLGWBN_NEEDCREDS                786

/* Stands in for the sign in / sign out button while a game is running, where
 * the account is fixed for the session. */
#define STR_DLGWBN_ACCOUNT_LOCKED           1943

/* Welcome dialog */
#define STR_DLGWELCOME_WINTITLE             787
#define STR_DLGWELCOME_SINGLE               788
#define STR_DLGWELCOME_LOCAL                789
#define STR_DLGWELCOME_MAPEDITOR            790
#define STR_DLGWELCOME_LOGVIEWER            791
#define STR_DLGWELCOME_INTERNET             792

/* The two faces of the welcome screen's full screen button. Which one is
 * shown is decided from the window's real state, not from the preference. */
#define STR_DLGWELCOME_SWITCH_CLASSIC       1991
#define STR_DLGWELCOME_SWITCH_FULLSCREEN    1992
/* Map Chooser dialog */
#define STR_MAPCHOOSER_EVERARD              793
#define STR_MAPCHOOSER_LOADMAP              794
#define STR_MAPCHOOSER_LOADDEVICE           795
#define STR_MAPCHOOSER_GENRANDOM            796
#define STR_MAPCHOOSER_RANDOMMAP            797
#define STR_MAPCHOOSER_CLICKGEN             798
#define STR_MAPCHOOSER_NOPREVIEW            799
#define STR_MAPCHOOSER_STATS                800
#define STR_MAPCHOOSER_MAPFILES             801
#define STR_MAPCHOOSER_ALLFILES             802

/* Tablet HUD */
#define STR_TABLET_STATUS_TITLE             803
#define STR_TABLET_KILLS_DEATHS             804
#define STR_TABLET_TANK_RESOURCES           805
#define STR_TABLET_SHELLS                   806
#define STR_TABLET_MINES                    807
#define STR_TABLET_ARMOUR                   808
#define STR_TABLET_TREES                    809
#define STR_TABLET_PILLBOXES                810
#define STR_TABLET_BASES                    811
#define STR_TABLET_TANKS                    812
#define STR_TABLET_PILL_FMT                 813
#define STR_TABLET_BASE_FMT                 814
#define STR_TABLET_TANK_FMT                 815
#define STR_TABLET_BTN_MSG                  816
#define STR_TABLET_BTN_PLY                  817
#define STR_TABLET_BTN_SET                  818

/* Generic dialog titles (cross-dialog) */
#define STR_DLGSETPLAYERNAME_TITLE          819

/* Map editor menu bar */
#define STR_MAPEDIT_MENU_OPEN               837
#define STR_MAPEDIT_MENU_RECENT             838
#define STR_MAPEDIT_MENU_SAVE               839
#define STR_MAPEDIT_MENU_SAVEAS             840
#define STR_MAPEDIT_MENU_EXPORT             841
#define STR_MAPEDIT_MENU_RETURN             842
#define STR_MAPEDIT_MENU_UNDO               843
#define STR_MAPEDIT_MENU_REDO               844
#define STR_MAPEDIT_MENU_CUT                845
#define STR_MAPEDIT_MENU_COPY               846
#define STR_MAPEDIT_MENU_PASTE              847
#define STR_MAPEDIT_MENU_MAP                848
#define STR_MAPEDIT_MENU_RANDOMMAP          849
#define STR_MAPEDIT_MENU_TEXT               850
#define STR_MAPEDIT_MENU_IMPORTIMG          851
#define STR_MAPEDIT_MENU_VALIDATE           852
#define STR_MAPEDIT_MENU_MIRROR_H           853
#define STR_MAPEDIT_MENU_MIRROR_V           854
#define STR_MAPEDIT_MENU_ROTATE_90          855
#define STR_MAPEDIT_MENU_ROTATE_180         856
#define STR_MAPEDIT_SCOPE_SELECTION         857
#define STR_MAPEDIT_SCOPE_FULLMAP           858
#define STR_MAPEDIT_MENU_OPTIONS            859
#define STR_MAPEDIT_MENU_CENTER             860
#define STR_MAPEDIT_MENU_POINT_STARTS       861
#define STR_MAPEDIT_MENU_SHOWGRID           862
#define STR_MAPEDIT_MENU_SHOWMINES          863
#define STR_MAPEDIT_MENU_SHOWPILLRANGES     864
#define STR_MAPEDIT_MENU_WINDOW             865

/* Map editor windows */
#define STR_MAPEDIT_WIN_TERRAIN             866
#define STR_MAPEDIT_WIN_TOOLS               867
#define STR_MAPEDIT_WIN_INSPECTOR           868
#define STR_MAPEDIT_WIN_OBJECTS             869
#define STR_MAPEDIT_WIN_OVERVIEW            870
#define STR_MAPEDIT_WIN_STATS               871
#define STR_MAPEDIT_WIN_STAMP_LIB           872

/* Map editor terrain palette */
#define STR_MAPEDIT_TERR_DEEPSEA            873
#define STR_MAPEDIT_TERR_GRASS              874
#define STR_MAPEDIT_TERR_FOREST             875
#define STR_MAPEDIT_TERR_ROAD               876
#define STR_MAPEDIT_TERR_BUILDING           877
#define STR_MAPEDIT_TERR_HALFBUILDING       878
#define STR_MAPEDIT_TERR_RIVER              879
#define STR_MAPEDIT_TERR_SWAMP              880
#define STR_MAPEDIT_TERR_CRATER             881
#define STR_MAPEDIT_TERR_RUBBLE             882
#define STR_MAPEDIT_TERR_BOAT               883
#define STR_MAPEDIT_TERR_MINETOOL           884
#define STR_MAPEDIT_MINE_CHECKERED          885
#define STR_MAPEDIT_MINE_FULL               886
#define STR_MAPEDIT_MINE_RANDOM             887
#define STR_MAPEDIT_MINE_CLEAR              888

/* Map editor drawing tools */
#define STR_MAPEDIT_TOOL_PENCIL             889
#define STR_MAPEDIT_TOOL_LINE               890
#define STR_MAPEDIT_TOOL_RECT               891
#define STR_MAPEDIT_TOOL_RECTFILL           892
#define STR_MAPEDIT_TOOL_OVAL               893
#define STR_MAPEDIT_TOOL_OVALFILL           894
#define STR_MAPEDIT_TOOL_SELECT             895
#define STR_MAPEDIT_TOOL_FILL               896
#define STR_MAPEDIT_TOOL_MAZE               897
#define STR_MAPEDIT_TOOL_GENERATE           898
#define STR_MAPEDIT_TOOL_WAND               899
#define STR_MAPEDIT_TOOL_TEXT               900
#define STR_MAPEDIT_TOOL_TEXT_TIP           901
#define STR_MAPEDIT_BRUSH                   902
#define STR_MAPEDIT_BRUSH_SQUARE            903
#define STR_MAPEDIT_BRUSH_CIRCLE            904
#define STR_MAPEDIT_OBJTOOL_BASE            905
#define STR_MAPEDIT_OBJTOOL_PILL            906
#define STR_MAPEDIT_OBJTOOL_START           907

/* Map editor status bar */
#define STR_MAPEDIT_STATUS_TILE             908
#define STR_MAPEDIT_STATUS_TILE_NONE        909
#define STR_MAPEDIT_STATUS_ZOOM             910
#define STR_MAPEDIT_STATUS_MODIFIED         911

/* Map editor modals */
#define STR_MAPEDIT_UNSAVED_TITLE           912
#define STR_MAPEDIT_UNSAVED_BLURB           913
#define STR_MAPEDIT_GOTO_TITLE              914
#define STR_MAPEDIT_GOTO_X                  915
#define STR_MAPEDIT_GOTO_Y                  916

/* Map editor inspector */
#define STR_MAPEDIT_INSP_NOSEL              917
#define STR_MAPEDIT_INSP_BASE_HASH          918
#define STR_MAPEDIT_INSP_PILL_HASH          919
#define STR_MAPEDIT_INSP_START_HASH         920
#define STR_MAPEDIT_INSP_POSITION           921
#define STR_MAPEDIT_INSP_OWNER              922
#define STR_MAPEDIT_INSP_ARMOUR             923
#define STR_MAPEDIT_INSP_SPEED              924
#define STR_MAPEDIT_INSP_DIR                925
#define STR_MAPEDIT_OWNER_NEUTRAL           926
#define STR_MAPEDIT_OWNER_PLAYER            927

/* Map editor object list */
#define STR_MAPEDIT_OBJ_BASES_COUNT         928
#define STR_MAPEDIT_OBJ_PILLS_COUNT         929
#define STR_MAPEDIT_OBJ_STARTS_COUNT        930
#define STR_MAPEDIT_OBJ_BASE_ROW            931
#define STR_MAPEDIT_OBJ_PILL_ROW            932
#define STR_MAPEDIT_OBJ_START_ROW           933

/* Map editor validation panel */
#define STR_MAPEDIT_VALIDATION              934
#define STR_MAPEDIT_VAL_NOISSUES            935
#define STR_MAPEDIT_VAL_ERRORS              936
#define STR_MAPEDIT_VAL_NOERRORS            937
#define STR_MAPEDIT_VAL_WARNINGS            938
#define STR_MAPEDIT_VAL_NOWARNINGS          939
#define STR_MAPEDIT_VAL_ERRORS_TITLE        940
#define STR_MAPEDIT_VAL_HASERRORS           941

/* Map editor generate dialog */
#define STR_MAPEDIT_GENRANDMAP_TITLE        942
#define STR_MAPEDIT_GENRANDAREA_TITLE       943
#define STR_MAPEDIT_GENSCOPE_SELECTION      944
#define STR_MAPEDIT_GENSCOPE_FULLMAP        945

/* Map editor text tool dialog */
#define STR_MAPEDIT_TEXT_TITLE              946
#define STR_MAPEDIT_TEXT_LABEL              947
#define STR_MAPEDIT_TEXT_BUILTIN            948
#define STR_MAPEDIT_TEXT_SYSTEM             949
#define STR_MAPEDIT_TEXT_FONTSIZE           950
#define STR_MAPEDIT_TEXT_STYLE              951
#define STR_MAPEDIT_TEXT_FONTSIZE_S         952
#define STR_MAPEDIT_TEXT_FONTSIZE_M         953
#define STR_MAPEDIT_TEXT_FONTSIZE_L         954
#define STR_MAPEDIT_TEXT_FONTSIZE_XL        955
#define STR_MAPEDIT_TEXT_STYLE_REGULAR      956
#define STR_MAPEDIT_TEXT_STYLE_BOLD         957
#define STR_MAPEDIT_TEXT_STYLE_ITALIC       958
#define STR_MAPEDIT_TEXT_STYLE_BOLDITALIC   959
#define STR_MAPEDIT_TEXT_FONTFAMILY         960
#define STR_MAPEDIT_TEXT_FONTBUNDLED        961
#define STR_MAPEDIT_TEXT_SIZEPX             962
#define STR_MAPEDIT_TEXT_NOFONTS            963
#define STR_MAPEDIT_TEXT_TERR_TEXT          964
#define STR_MAPEDIT_TEXT_TERR_EDGE          965
#define STR_MAPEDIT_TEXT_TERR_BG            966
#define STR_MAPEDIT_TEXT_PREVIEW            967
#define STR_MAPEDIT_TEXT_TOOLARGE           968
#define STR_MAPEDIT_GENERATE_BTN            969

/* Map editor maze settings */
#define STR_MAPEDIT_MAZE_TITLE              970
#define STR_MAPEDIT_MAZE_REGION             971
#define STR_MAPEDIT_DRAG_HINT               972
#define STR_MAPEDIT_ALGORITHM               973
#define STR_MAPEDIT_ALGO_LABYRINTH          974
#define STR_MAPEDIT_ALGO_OPEN               975
#define STR_MAPEDIT_WALL                    976
#define STR_MAPEDIT_CORRIDOR                977
#define STR_MAPEDIT_ENTRIES                 978
#define STR_MAPEDIT_ROOMS                   979
#define STR_MAPEDIT_WALLTERRAIN             980
#define STR_MAPEDIT_CORRIDORTERRAIN         981
#define STR_MAPEDIT_GENSETTINGS_TITLE       982

/* Map editor statistics panel */
#define STR_MAPEDIT_STATS_TERRAIN           983
#define STR_MAPEDIT_STATS_OBJ_LINE          984
#define STR_MAPEDIT_STATS_MINES             985
#define STR_MAPEDIT_STATS_SPATIAL           986
#define STR_MAPEDIT_STATS_LAND_COVERAGE     987
#define STR_MAPEDIT_STATS_LARGEST           988
#define STR_MAPEDIT_STATS_BASE_SPACING      989
#define STR_MAPEDIT_STATS_PILL_SPACING      990
#define STR_MAPEDIT_STATS_SYMMETRY          991
#define STR_MAPEDIT_STATS_MIRROR_H          992
#define STR_MAPEDIT_STATS_MIRROR_V          993
#define STR_MAPEDIT_STATS_4CORNER           994
#define STR_MAPEDIT_STATS_ROTATE180         995
#define STR_MAPEDIT_STATS_STALE             996
#define STR_MAPEDIT_STATS_REFRESH           997
#define STR_MAPEDIT_STATS_STARTS            998
#define STR_MAPEDIT_TERRNAME_HALFBLD        999

/* Map editor image import */
#define STR_MAPEDIT_IMG_TITLE               1000
#define STR_MAPEDIT_IMG_FILE                1001
#define STR_MAPEDIT_IMG_NONE                1002
#define STR_MAPEDIT_BROWSE                  1003
#define STR_MAPEDIT_IMG_SOURCE              1004
#define STR_MAPEDIT_IMG_PREVIEW_LBL         1005
#define STR_MAPEDIT_IMG_SCALEMODE           1006
#define STR_MAPEDIT_IMG_FIT_SEL             1007
#define STR_MAPEDIT_IMG_FIT_PLAY            1008
#define STR_MAPEDIT_IMG_OUTPUT              1009
#define STR_MAPEDIT_IMG_COLORS              1010
#define STR_MAPEDIT_IMG_REDETECT            1011
#define STR_MAPEDIT_IMG_COLORMAP            1012
#define STR_MAPEDIT_IMPORT_BTN              1013

/* Map editor stamp library */
#define STR_MAPEDIT_STAMP_BUNDLED           1014
#define STR_MAPEDIT_STAMP_USER              1015
#define STR_MAPEDIT_STAMP_NOUSER            1016
#define STR_MAPEDIT_STAMP_HINT              1017
#define STR_MAPEDIT_DELETE                  1018
#define STR_MAPEDIT_STAMP_SAVECLIP          1019
#define STR_MAPEDIT_STAMP_TIP_COPY          1020
#define STR_MAPEDIT_STAMP_IMPORT            1021
#define STR_MAPEDIT_STAMP_UNTITLED          1022
#define STR_MAPEDIT_STAMP_SAVE_TITLE        1023
#define STR_MAPEDIT_STAMP_SAVE_BLURB        1024
#define STR_MAPEDIT_STAMP_SAVE_NAME         1025
#define STR_MAPEDIT_SAVE_BTN                1026

/* Map editor export PNG */
#define STR_MAPEDIT_EXPORT_TITLE            1027
#define STR_MAPEDIT_PREVIEW                 1028
#define STR_MAPEDIT_EXPORT_MODE             1029
#define STR_MAPEDIT_EXPORT_FULL             1030
#define STR_MAPEDIT_EXPORT_PREVSIZE         1031
#define STR_MAPEDIT_EXPORT_OPTIONS          1032
#define STR_MAPEDIT_EXPORT_SHOWOBJ          1033
#define STR_MAPEDIT_EXPORT_SHOWMINES        1034
#define STR_MAPEDIT_EXPORT_SHOWGRID         1035
#define STR_MAPEDIT_EXPORT_BTN              1036

/* Map generator panel (mapgen_imgui.cpp) */
#define STR_MAPGEN_LOCKED_TIP               1037
#define STR_MAPGEN_UNLOCKED_TIP             1038
#define STR_MAPGEN_GENERATOR                1039
#define STR_MAPGEN_TYPE_TOURNAMENT          1040
#define STR_MAPGEN_TYPE_NATURAL             1041
#define STR_MAPGEN_TYPE_MAZE                1042
#define STR_MAPGEN_TYPE_FRACTAL             1043
#define STR_MAPGEN_SEED                     1044
#define STR_MAPGEN_RANDOMIZE                1045
#define STR_MAPGEN_SYMMETRY                 1046
#define STR_MAPGEN_SYM_4CORNER              1047
#define STR_MAPGEN_SYM_MIRROR_H             1048
#define STR_MAPGEN_SYM_MIRROR_V             1049
#define STR_MAPGEN_SYM_ROT180               1050
#define STR_MAPGEN_SYM_ROT90                1051
#define STR_MAPGEN_LANDMASS_PCT             1052
#define STR_MAPGEN_ROUGHNESS                1053
#define STR_MAPGEN_ROUGH_LOW                1054
#define STR_MAPGEN_ROUGH_MED                1055
#define STR_MAPGEN_ROUGH_HIGH               1056
#define STR_MAPGEN_INCLUDE_ROADS            1057
#define STR_MAPGEN_MAP_STYLE                1058
#define STR_MAPGEN_STYLE_OCEAN              1059
#define STR_MAPGEN_STYLE_CONTINENT          1060
#define STR_MAPGEN_STYLE_ISLANDS            1061
#define STR_MAPGEN_STYLE_ARCHIPELAGO        1062
#define STR_MAPGEN_STYLE_INLAND             1063
#define STR_MAPGEN_TERRAIN_MIX              1064
#define STR_MAPGEN_GRASS_PCT                1065
#define STR_MAPGEN_FOREST_PCT               1066
#define STR_MAPGEN_BUILDING_PCT             1067
#define STR_MAPGEN_SWAMP_PCT                1068
#define STR_MAPGEN_RIVER_PCT                1069
#define STR_MAPGEN_BOAT_PCT                 1070
#define STR_MAPGEN_REMAINING_GRASS          1071
#define STR_MAPGEN_MINE_DENSITY             1072
#define STR_MAPGEN_RIVER_COUNT              1073
#define STR_MAPGEN_CITY_COUNT               1074
#define STR_MAPGEN_MAZE_COUNT               1075
#define STR_MAPGEN_WALL_THICKNESS           1076
#define STR_MAPGEN_CORRIDOR_WIDTH           1077
#define STR_MAPGEN_CITY_ROOMS               1078
#define STR_MAPGEN_LAND_COVERAGE            1079
#define STR_MAPGEN_DETAIL_PASSES            1080
#define STR_MAPGEN_COAST_JAG                1081
#define STR_MAPGEN_TERRAIN_LAYERS           1082
#define STR_MAPGEN_RIVERS                   1083
#define STR_MAPGEN_LAKES                    1084

/* Log viewer sim-replay messages */
#define STR_LV_PLAYER_JOINED                1085
#define STR_LV_HAS_JOINED                   1086
#define STR_LV_ALLY_REQUEST                 1087
#define STR_LV_ALLY_ACCEPT                  1088
#define STR_LV_ALLY_LEAVE                   1089
#define STR_LV_MSG_ALL                      1090
#define STR_LV_MSG_PLAYERS                  1091
#define STR_LV_MSG_SERVER                   1092
#define STR_LV_PLAYER_DIED                  1093
#define STR_LV_PLAYER_KILLED                1094
#define STR_LV_PLAYER_REJOINED              1095
#define STR_LV_PLAYER_LEAVING               1096
#define STR_LV_LOBBY_OPENED                 1097
#define STR_LV_GAME_STARTED                 1098
#define STR_LV_PLAYER_READY                 1099
#define STR_LV_PLAYER_UNREADY               1100
#define STR_LV_PLAYER_LEFT_TEAM             1101
#define STR_LV_PLAYER_JOINED_TEAM           1102
#define STR_LV_COUNTDOWN_START              1103
#define STR_LV_COUNTDOWN_CANCEL             1104
#define STR_LV_MAP_SKIP_VOTE                1105
#define STR_LV_MAP_SKIPPED                  1106
#define STR_LV_TEAM_BALANCE                 1107

/* Log viewer main menu */
#define STR_LV_MENU_OPEN                    1108
#define STR_LV_MENU_ACTION                  1109
#define STR_LV_PLAY                         1110
#define STR_LV_PAUSE                        1111
#define STR_LV_STOP                         1112
#define STR_LV_FAST_FORWARD                 1113
#define STR_LV_REWIND                       1114
#define STR_LV_MODE                         1115
#define STR_LV_MODE_INFO                    1116
#define STR_LV_SELECT_TEAM                  1117
#define STR_LV_USE_TEAM_COLOURS             1118
#define STR_LV_TANK_CENTRED                 1119
#define STR_LV_DNS_LOOKUPS                  1120
#define STR_LV_TEAM_COLOURS                 1121
#define STR_LV_MENU_WINDOWS                 1122
#define STR_LV_WIN_CONTROLS                 1123
#define STR_LV_WIN_EVENTS                   1124
#define STR_LV_WIN_GAMEINFO                 1125
#define STR_LV_WIN_ITEMINFO                 1126
#define STR_LV_RESET_WINDOWS                1127

/* Log viewer dialogs */
#define STR_LV_ASSIGN_COLOURS_HINT          1128
#define STR_LV_PLAYER_LBL                   1129
#define STR_LV_NEUTRAL_LBL                  1130
#define STR_LV_ABOUT_TITLE                  1131
#define STR_LV_VERSION_FMT                  1132
#define STR_LV_LICENSE                      1133
#define STR_LV_WEBSITE_LBL                  1134

/* Log viewer team colour names */
#define STR_LV_COL_GREY                     1135
#define STR_LV_COL_KHAKI                    1136
#define STR_LV_COL_GREEN                    1137
#define STR_LV_COL_PINK                     1138
#define STR_LV_COL_YELLOW                   1139
#define STR_LV_COL_LIGHTBLUE                1140
#define STR_LV_COL_ORANGE                   1141
#define STR_LV_COL_LIGHTPURPLE              1142
#define STR_LV_COL_AQUA                     1143
#define STR_LV_COL_LIGHTGREEN               1144
#define STR_LV_COL_LIGHTGREY                1145
#define STR_LV_COL_RED                      1146
#define STR_LV_COL_BLUE                     1147
#define STR_LV_COL_BROWN                    1148
#define STR_LV_COL_LIGHTPINK                1149
#define STR_LV_COL_PALEGREEN                1150
#define STR_LV_COL_PURPLE                   1151

/* Log viewer game info panel */
#define STR_LV_SECONDS                      1152
#define STR_LV_INFO_MAP                     1153
#define STR_LV_INFO_GAMETYPE                1154
#define STR_LV_INFO_HIDDENMINES             1155
#define STR_LV_INFO_COMPUTER_TANKS          1156
#define STR_LV_INFO_TIME_LIMIT              1157
#define STR_LV_INFO_START_DELAY             1158
#define STR_LV_INFO_WBN_KEY                 1159
#define STR_LV_INFO_START_TIME              1160

/* Log viewer item info panel */
#define STR_LV_OWNER_FMT                    1161
#define STR_LV_POSITION_XY                  1162
#define STR_LV_NO_ITEM_SELECTED             1163
#define STR_LV_LOCATION_FMT                 1164
#define STR_LV_OWNER_LBL                    1165
#define STR_LV_ARMOUR_FMT                   1166
#define STR_LV_SHELLS_FMT                   1167
#define STR_LV_MINES_FMT                    1168
#define STR_LV_IN_TANK_FMT                  1169
#define STR_LV_CENTER_ON_MAP                1170

/* Log viewer playback controls */
#define STR_LV_REW_BTN                      1171
#define STR_LV_PLAY_BTN                     1172
#define STR_LV_FWD_BTN                      1173
#define STR_LV_SPEED_LBL                    1174
#define STR_LV_TIME_FMT                     1175
#define STR_LV_TIME_REMAINING               1176
#define STR_LV_NO_LOG_LOADED                1177

/* Log viewer events panel */
#define STR_LV_COPY                         1178
#define STR_LV_COPY_ALL                     1179
#define STR_LV_CLEAR_ALL                    1180
#define STR_LV_AUTO_SCROLL                  1181

/* Log viewer end-of-log marker */
#define STR_LV_END_OF_LOG                   1182

/* iOS Settings panel additions */
#define STR_DLGSETTINGS_ZOOM                1183
#define STR_DLGSETTINGS_PERFORMANCE         1184
#define STR_DLGSETTINGS_FPS_FMT             1185

/* iOS disconnect-to-menu (distinct from NETERR_LOSTCONNECTION which
 * mentions dropping to single-player mode) */
#define NETERR_LOSTCONNECTION_RETURN_MENU   1186

/* Language picker (Settings → Display) */
#define STR_DLGSETTINGS_LANGUAGE_LBL        1187
#define STR_DLGLANG_MIDGAME_NOTE            1188

/* Hosted multiplayer / NAT traversal — Phase 1-4 networking work */
#define STR_DLGBROWSER_NEWGAME_PORTFWD_TIP  1189
#define STR_GAMEFRONTERR_MAPLOAD            1190
#define STR_GAMEFRONTERR_STARTSERVER        1191
#define STR_DLGLOBBY_PORTMAP_CHECKING       1192
#define STR_DLGLOBBY_PORTMAP_ACCESSIBLE     1193
#define STR_DLGLOBBY_PORTMAP_UNREACHABLE    1194
#define STR_DLGLOBBY_PORTMAP_DETAIL_PENDING     1195
#define STR_DLGLOBBY_PORTMAP_DETAIL_SUCCEEDED   1196
#define STR_DLGLOBBY_PORTMAP_DETAIL_HOLE_PUNCH  1197
#define STR_DLGLOBBY_PORTMAP_DETAIL_SYMMETRIC   1198
#define STR_DLGLOBBY_PORTMAP_DETAIL_FAILED      1199
#define STR_DLGLOBBY_PORTMAP_POPUP_TITLE    1200
#define STR_DLGLOBBY_TEST_CONNECTIVITY      1201
#define STR_DLGLOBBY_TEST_TESTING           1202
#define STR_DLGLOBBY_TEST_REACHABLE         1203
#define STR_DLGLOBBY_TEST_NO_REPLY          1204
#define STR_DLGSETTINGS_NETWORK             1205
#define STR_DLGSETTINGS_NET_HELP            1206
#define STR_DLGSETTINGS_USE_UPNP            1207
#define STR_DLGSETTINGS_USE_NATTRAV         1208

/* Player-name validation (Phase 2). */
#define STR_NAME_INVALID_EMPTY              1209
#define STR_NAME_INVALID_CHARS              1210
#define STR_NAME_INVALID_MIXED_SCRIPTS      1211
#define STR_NAME_INVALID_RESERVED_PREFIX    1212
#define STR_NAME_INVALID_RESERVED_SUFFIX    1213

/* Verified-priority collision policy (Phase 5). */
#define STR_NAME_RENAMED_BY_VERIFIED        1215
#define STR_NAME_TAKEN_BY_VERIFIED          1216
#define STR_NAME_TAKEN_BY_OTHER_VERIFIED    1217

/* Localized server→client messages (Phase 9d). Sent over the wire as
 * langid + args by serverSendJoinReject / serverSendServerMessage so
 * each client renders in its own locale. */
#define STR_REJECT_INCORRECT_PASSWORD       1218
#define STR_REJECT_GAME_LOCKED              1219
#define STR_REJECT_SERVER_FULL              1220
#define STR_REJECT_NAME_POOL_EXHAUSTED      1221
#define STR_REJECT_INVALID_PLAYER_NAME      1222
#define STR_REJECT_WBN_VERIFY_FAILED        1223
#define STR_KICK_ANNOUNCE                   1224

/* Log viewer comments panel */
#define STR_LV_WIN_COMMENTS                 1225
#define STR_LV_INFO_SIGNIN_TO_COMMENT       1226
#define STR_LV_INFO_NO_WBN_KEY              1227

/* Log viewer File menu — open log from WinBolo.net */
#define STR_LV_MENU_OPEN_WBN                1228

/* Log viewer zoom menu */
#define STR_LV_ZOOM                         1229
#define STR_LV_ZOOM_IN                      1230
#define STR_LV_ZOOM_OUT                     1231

/* System Info panel — server-side bot/sim telemetry labels.
 * Bare nouns (no trailing colon, no format specifiers); colons and
 * numeric format specifiers stay literal in the C format strings. */
#define STR_DLGSYSINFO_SERVER               1232
#define STR_DLGSYSINFO_BOTPOOL              1233
#define STR_DLGSYSINFO_TICK                 1234
#define STR_DLGSYSINFO_BRAIN                1235
#define STR_DLGSYSINFO_SIMULATION           1236
#define STR_DLGSYSINFO_BOTPREP              1237
#define STR_DLGSYSINFO_BRAIN_OVERRUNS       1238

/* macOS native menu — App / File / Window menu items mirrored by
 * src/gui/sdl3/platform/mac_menubar.mm. Not used by the in-window menu. */
#define STR_MENU_ABOUT_APP                  1239
#define STR_MENU_PREFERENCES                1240
#define STR_MENU_SERVICES                   1241
#define STR_MENU_HIDE_APP                   1242
#define STR_MENU_HIDE_OTHERS                1243
#define STR_MENU_SHOW_ALL                   1244
#define STR_MENU_QUIT_APP                   1245
#define STR_MENU_WINDOW                     1246
#define STR_MENU_MINIMIZE                   1247
#define STR_MENU_ZOOM                       1248
#define STR_MENU_ENTER_FULL_SCREEN          1249
#define STR_MENU_BRING_ALL_TO_FRONT         1250
#define STR_MENU_FIND_INTERNET_GAME         1251
#define STR_MENU_FIND_LAN_GAME              1252
#define STR_MENU_JOIN_BY_ADDRESS            1253
#define STR_MENU_OPEN_MAP_EDITOR            1254
#define STR_MENU_OPEN_LOG_VIEWER            1255

/* macOS Log Viewer app-menu items — mirror the WinBolo About/Hide/Quit
 * trio but with "Log Viewer" wording so the standalone Log Viewer.app's
 * app menu reads correctly. Embedded Log Viewer reuses WinBolo's app
 * menu so these are not surfaced there. */
#define STR_MENU_ABOUT_LV                   1256
#define STR_MENU_HIDE_LV                    1257
#define STR_MENU_QUIT_LV                    1258

/* macOS Map Editor app-menu items — same idea as the Log Viewer trio.
 * Standalone MapEditor.app shows these; embedded reuses WinBolo's
 * app menu. About is not wired (no in-app About dialog exists yet)
 * so STR_MENU_ABOUT_ME has no companion menu item for now. */
#define STR_MENU_HIDE_ME                    1259
#define STR_MENU_QUIT_ME                    1260

/* Settings → Network: WinBolo.net news auto-show toggle. */
#define STR_DLGSETTINGS_NEWS_AUTOSHOW       1261

/* News popup + welcome News button strings. */
#define STR_DLGWELCOME_NEWS                 1262
#define STR_DLGNEWS_TITLE                   1263
#define STR_DLGNEWS_CONSENT_TITLE           1264
#define STR_DLGNEWS_CONSENT_BODY1           1265
#define STR_DLGNEWS_CONSENT_BODY2           1266
#define STR_DLGNEWS_SHOW                    1267
#define STR_DLGNEWS_DONT_SHOW               1268
#define STR_DLGNEWS_LOADING                 1269
#define STR_DLGNEWS_DONT_AUTOSHOW           1270
#define STR_DLGNEWS_COMMENTS_FMT            1271
#define STR_DLGLOBBY_KICKED                 1272

/* Lobby — map chooser */
#define STR_DLGLOBBY_CHOOSEMAP_TITLE        1273
#define STR_DLGLOBBY_TAB_SERVERMAPS         1274
#define STR_DLGLOBBY_TAB_LOCALMAPS          1275
#define STR_DLGLOBBY_TAB_UPLOAD             1276
#define STR_DLGLOBBY_TAB_GENERATE           1277
#define STR_DLGLOBBY_TAB_WBNMAPS            1278
#define STR_DLGLOBBY_CLOSEMAP_PROMPT        1279
#define STR_DLGLOBBY_CLOSEMAP_QUESTION      1280
#define STR_DLGLOBBY_USETHISMAP             1281
#define STR_DLGLOBBY_REVERTCLOSE            1282
#define STR_DLGLOBBY_KEEPPICKING            1283
#define STR_DLGLOBBY_CANCEL_MAPCHOOSER      1284
/* Lobby — map chooser (continued) */
#define STR_DLGLOBBY_CHOOSE_MAP_BTN         1388

/* Client-side pre-flight version-mismatch error. Surfaced by the
 * direct-connect / rejoin entry when an info-request reveals the
 * server runs a different build than this client. {string1} = server
 * version (X.Y.Z), {string2} = client version (X.Y.Z). */
#define STR_REJECT_VERSION_MISMATCH         1389

/* Lobby — upload + WBN errors */
#define STR_DLGLOBBY_UPLOAD_ERR_INFLIGHT        1285
#define STR_DLGLOBBY_UPLOAD_ERR_DISABLED        1286
#define STR_DLGLOBBY_UPLOAD_ERR_FULL            1287
#define STR_DLGLOBBY_UPLOAD_ERR_COOLDOWN        1288
#define STR_DLGLOBBY_UPLOAD_ERR_REJECTED        1289
#define STR_DLGLOBBY_WBN_ERR_DOWNLOAD           1290
#define STR_DLGLOBBY_WBN_ERR_BADRESPONSE        1291
#define STR_DLGLOBBY_WBN_ERR_NETERROR           1292
#define STR_DLGLOBBY_WBN_ERR_HTTPERROR          1293
#define STR_DLGLOBBY_WBN_ERR_MAPFAILED          1294
#define STR_DLGLOBBY_WBN_ERR_TOOBIG             1295
#define STR_DLGLOBBY_WBN_ERR_OOM                1296
#define STR_DLGLOBBY_WBN_ERR_BADSEARCHRESPONSE  1297

/* Lobby — team controls + player list + AiConfig */

/* Allow-new-players + ranked controls */
#define STR_DLGLOBBY_ALLOW_NOW              1298
#define STR_DLGLOBBY_ALLOW_DURING           1299
#define STR_DLGLOBBY_TOOLTIP_ALLOWNOW       1300
#define STR_DLGLOBBY_TOOLTIP_ALLOWDURING    1301
#define STR_DLGLOBBY_TOOLTIP_RANKED_AUTOLOCK 1302
#define STR_DLGLOBBY_RANKED                 1303
#define STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED  1304
#define STR_DLGLOBBY_TOOLTIP_RANKED_NOTHOST 1305
#define STR_DLGLOBBY_TOOLTIP_RANKED_BOTS    1306
#define STR_DLGLOBBY_TOOLTIP_RANKED_INFO    1307

/* Team-grouped player list */
#define STR_DLGLOBBY_TEAM_HEADER            1308
#define STR_DLGLOBBY_TEAM_MEMBERS_1         1309
#define STR_DLGLOBBY_TEAM_MEMBERS_N         1310
#define STR_DLGLOBBY_TEAM_1BOT              1311
#define STR_DLGLOBBY_TEAM_NBOTS             1312
#define STR_DLGLOBBY_JOIN_TEAM              1313
#define STR_DLGLOBBY_TOOLTIP_JOIN           1314
#define STR_DLGLOBBY_BOT_NAMING             1315
#define STR_DLGLOBBY_ADDBOT_LBL             1316
#define STR_DLGLOBBY_TOOLTIP_ADDBOT         1317
#define STR_DLGLOBBY_TOOLTIP_RMTEAM         1318
#define STR_DLGLOBBY_TOOLTIP_DRAG           1319
#define STR_DLGLOBBY_TAG_HOST               1320
#define STR_DLGLOBBY_TAG_ADMIN              1321
#define STR_DLGLOBBY_TAG_BOT                1322
#define STR_DLGLOBBY_PILL_READY             1323
#define STR_DLGLOBBY_PILL_NOTREADY          1324
#define STR_DLGLOBBY_TOOLTIP_CONFIG         1325
#define STR_DLGLOBBY_TOOLTIP_RMBOT          1326
#define STR_DLGLOBBY_TOOLTIP_KICK           1327
#define STR_DLGLOBBY_ADD_TEAM               1328
#define STR_DLGLOBBY_UNASSIGNED_FMT         1329
#define STR_DLGLOBBY_KICK_FMT               1330

/* Bot AiConfig panel */
#define STR_DLGLOBBY_BOTCFG_NAME            1331
#define STR_DLGLOBBY_BOTCFG_REROLL          1332
#define STR_DLGLOBBY_BOTCFG_REROLL_TIP      1333
#define STR_DLGLOBBY_BOTCFG_CODE            1334
#define STR_DLGLOBBY_BOTCFG_NONE            1335
#define STR_DLGLOBBY_BOTCFG_DIFFICULTY      1336
#define STR_DLGLOBBY_BOTCFG_EASY            1337
#define STR_DLGLOBBY_BOTCFG_NORMAL          1338
#define STR_DLGLOBBY_BOTCFG_HARD            1339
#define STR_DLGLOBBY_BOTCFG_PERSONALITY     1340
#define STR_DLGLOBBY_BOTCFG_AGGRESSIVE      1341
#define STR_DLGLOBBY_BOTCFG_DEFENSIVE       1342
#define STR_DLGLOBBY_BOTCFG_SNIPER          1343
#define STR_DLGLOBBY_BOTCFG_DONE            1344
/* Gear-hover tooltip: "Currently: <bot name> . <difficulty>" ({string1}). */
#define STR_DLGLOBBY_BOTCFG_CURRENTLY       1847

/* Middle bot difficulty (wire value 1; Easy / Hard reuse 1337 / 1339).
 * 2172, not 2164: main's #323 claimed 2164 for STR_DLGPLAYERS_SELECT while
 * this branch was out, and the two defines live far enough apart in this
 * file that the merge kept both without a conflict. langGetText linear-scans
 * and returns the first hit, so the collision would have drawn the Players
 * panel's "Select:" as "Medium". The already-merged id keeps 2164. */
#define STR_DLGLOBBY_BOTCFG_MEDIUM          2172

/* Bot difficulty blurbs: tagline (leading token coloured) + description. */
#define STR_BOT_DIFF_TAG_EASY               2165
#define STR_BOT_DIFF_TAG_MEDIUM             2166
#define STR_BOT_DIFF_TAG_HARD               2167
#define STR_BOT_DIFF_DESC_EASY              2168
#define STR_BOT_DIFF_DESC_MEDIUM            2169
#define STR_BOT_DIFF_DESC_HARD              2170

/* Bot AiConfig: the Mode dropdown's label. The mode NAMES themselves are
 * data (brains/<brain>/modes.txt), not strings, so this is the only one. */
#define STR_DLGLOBBY_BOTCFG_MODE            2171

/* Title of the dialog a bot's announce line in lobby team chat opens: the
 * brain's own commands.txt. {string1} = the brain's name ("GoalHunter"). */
#define STR_DLGLOBBY_BOT_DOCS_TITLE         2214

/* A general "Copy" button label, for any dialog that puts its body on the
 * clipboard. STR_LV_COPY is the same word but belongs to the log viewer's
 * events panel; this one is not tied to a screen. */
#define STR_COPY                            2215

/* Lobby — Balance/Reject/Lock/RankedShape */
/* Balance from WBN */
#define STR_DLGLOBBY_BAL_BTN                1345
#define STR_DLGLOBBY_BAL_POPUP_TITLE        1346
#define STR_DLGLOBBY_BAL_POPUP_BODY         1347
#define STR_DLGLOBBY_BAL_POPUP_BOT_SINGULAR 1348
#define STR_DLGLOBBY_BAL_POPUP_BOT_PLURAL   1349
#define STR_DLGLOBBY_BAL_BOTS_INCLUDED      1350
#define STR_DLGLOBBY_BAL_HUMANS_ONLY        1351
#define STR_DLGLOBBY_BAL_GO                 1352
#define STR_DLGLOBBY_BAL_TOOLTIP_NOTHOST    1353
#define STR_DLGLOBBY_BAL_TOOLTIP_NOTENOUGH  1354
#define STR_DLGLOBBY_BAL_TOOLTIP_INFO       1355
#define STR_DLGLOBBY_BAL_STATUS_ASKING      1356
#define STR_DLGLOBBY_BAL_STATUS_BALANCED    1357
#define STR_DLGLOBBY_BAL_STATUS_NOREPLY     1358
/* Reject toast */
#define STR_DLGLOBBY_REJECT_DEFAULT         1359
#define STR_DLGLOBBY_REJECT_NOTHOST         1360
#define STR_DLGLOBBY_REJECT_LOCKED          1361
#define STR_DLGLOBBY_REJECT_INVALID         1362
#define STR_DLGLOBBY_REJECT_FMT             1363
/* Lock badge fallback */
#define STR_DLGLOBBY_LOCK_BADGE             1364
/* Ranked shape tooltip */
#define STR_DLGLOBBY_RANKED_SHAPE_TIP       1365
#define STR_DLGLOBBY_RANKED_SHAPE_TEAM      1366
#define STR_DLGLOBBY_RANKED_SHAPE_TEAMS     1367

/* Lobby — game settings panel + header */
/* Game settings panel */
#define STR_DLGLOBBY_SETTINGS_HEADER        1368
#define STR_DLGLOBBY_OPENHOST_NOTE          1369
#define STR_DLGLOBBY_GAMETYPE_LBL           1370
#define STR_DLGLOBBY_AI_SECTION_LBL         1371
#define STR_DLGLOBBY_AI_NONE                1372
#define STR_DLGLOBBY_AI_ALLOW               1373
#define STR_DLGLOBBY_AI_ADVANTAGE           1374
#define STR_DLGLOBBY_AI_FULLADV             1375
#define STR_DLGLOBBY_OTHER_LBL              1376
#define STR_DLGLOBBY_OPENHOST_CB            1377
#define STR_DLGLOBBY_OPENHOST_TOOLTIP       1378
#define STR_DLGLOBBY_TIMELIMIT_MIN          1379
#define STR_DLGLOBBY_PASSWORD_CB            1380
#define STR_DLGLOBBY_PASSWORD_TOOLTIP       1381
#define STR_DLGLOBBY_SERVER_SECTION_LBL     2585
/* Desktop header */
#define STR_DLGLOBBY_SERVERDISP_SP          1382
#define STR_DLGLOBBY_SERVERDISP_INTERNET    1383
/* Connectivity badge */
#define STR_DLGLOBBY_CONN_TEST_TIP          1384
/* Visibility block (pill / base / allied tank view rules) */
#define STR_DLGLOBBY_VISIBILITY_LBL         1994
#define STR_DLGLOBBY_VIEW_PILL              1995
#define STR_DLGLOBBY_VIEW_BASE              1996
#define STR_DLGLOBBY_VIEW_ALLY              1997
#define STR_DLGLOBBY_VIEW_ALWAYS            1998
#define STR_DLGLOBBY_VIEW_KEY               1999
#define STR_DLGLOBBY_VIEW_DECAY             2000
#define STR_DLGLOBBY_VIEW_OFF               2001
#define STR_DLGLOBBY_VIEW_DECAY_SECS        2002
#define STR_DLGLOBBY_CLASSIC_MODE_CB        2008
#define STR_DLGLOBBY_CLASSIC_MODE_TIP       2009
#define STR_MENU_CLASSIC_MODE_TIP           2010
#define STR_DLGLOBBY_ALLIES_TREES_CB        2011
#define STR_DLGLOBBY_ALLIES_TREES_TIP       2012
/* Map overview live block, and what blocks sight inside it */
#define STR_DLGLOBBY_OVERVIEW_WINDOW        2154
#define STR_DLGLOBBY_OVERVIEW_WINDOW_TIP    2155
#define STR_DLGLOBBY_LINE_OF_SIGHT_CB       2156
#define STR_DLGLOBBY_LINE_OF_SIGHT_TIP      2157
#define STR_DLGLOBBY_WINDOW_EXPANDED        2158
#define STR_DLGLOBBY_WINDOW_CLASSIC         2159
/* Visibility presets: the named sets the lobby dropdown offers, the row
 * each one draws in the Details table, and the words the row that is not
 * a preset needs. */
#define STR_DLGLOBBY_PRESET_CLASSIC              2173
#define STR_DLGLOBBY_PRESET_CLASSIC_DESC         2174
#define STR_DLGLOBBY_PRESET_CLASSIC_OVERVIEW     2175
#define STR_DLGLOBBY_PRESET_CLASSIC_OVERVIEW_DESC 2176
#define STR_DLGLOBBY_PRESET_EXPANDED             2177
#define STR_DLGLOBBY_PRESET_EXPANDED_DESC        2178
#define STR_DLGLOBBY_PRESET_MAXVIEW              2179
#define STR_DLGLOBBY_PRESET_MAXVIEW_DESC         2180
#define STR_DLGLOBBY_PRESET_SIGHT                2181
#define STR_DLGLOBBY_PRESET_SIGHT_DESC           2182
#define STR_DLGLOBBY_PRESET_CUSTOM               2183
#define STR_DLGLOBBY_PRESET_CUSTOM_DESC          2184
#define STR_DLGLOBBY_VIS_DETAILS_BTN             2185
#define STR_DLGLOBBY_VIS_DETAILS_TIP             2186
#define STR_DLGLOBBY_VIS_PRESET_LOCKED_TIP       2187
#define STR_DLGLOBBY_VIS_SHORT_PILLS             2188
#define STR_DLGLOBBY_VIS_SHORT_BASES             2189
#define STR_DLGLOBBY_VIS_SHORT_ALLIES            2190
#define STR_DLGLOBBY_VIS_SHORT_OVERVIEW          2191
#define STR_DLGLOBBY_VIEW_POLICY_TIP             2192
/* The third overview window: no map overview and no full screen map. Its
 * own id rather than a shared "None", the way every other value word here
 * belongs to the setting it names. */
#define STR_DLGLOBBY_WINDOW_NONE                 2193
/* What a server browser row says when the game never advertised its
 * visibility rules at all - an old server, or a tracker that has not
 * learned the fields. Such a game is named Classic, which is what it
 * plays like, and this says why there is nothing behind the name. */
#define STR_DLGBROWSER_VIEWS_UNKNOWN             2194
/* The Expanded overview window spelled out in full, for the one column
 * cell that has room to say what the mode gives you. One whole sentence
 * rather than the Expanded word plus a suffix, so a translator can put
 * the parts in whatever order the language wants. */
#define STR_DLGLOBBY_WINDOW_EXPANDED_LONG        2195
/* Lobby "Other" column — the host switch for smart pings */
#define STR_DLGLOBBY_SMART_PINGS_CB              2203
/* Settings > Display & Sound > Full Screen */
#define STR_DLGSETTINGS_FULLSCREEN          2013
#define STR_DLGSETTINGS_NEWS_TRANSPARENCY   2014
#define STR_DLGSETTINGS_NEWS_TRANSPARENCY_TIP 2015
#define STR_DLGSETTINGS_NEWS_AUTOHIDE       2016
#define STR_DLGSETTINGS_NEWS_AUTOHIDE_TIP   2017
#define STR_DLGSETTINGS_BUILD_TRANSPARENCY  2018
#define STR_DLGSETTINGS_BUILD_TRANSPARENCY_TIP 2019
#define STR_DLGSETTINGS_STATUS_TRANSPARENCY 2020
#define STR_DLGSETTINGS_STATUS_TRANSPARENCY_TIP 2021
#define STR_DLGSETTINGS_FULLSCREEN_TIP      2022
/* Item view caption — the corner label and the full screen map's caption */
#define STR_ITEMVIEW_PILL                   2023
#define STR_ITEMVIEW_BASE                   2024
#define STR_ITEMVIEW_ALLY                   2025
#define STR_ITEMVIEW_ALLY_NAMED             2026
/* Server browser visibility tag in the detail pane */
#define STR_DLGBROWSER_VIEWS_LBL            2027
#define STR_DLGBROWSER_VIEWS_CLASSIC        2028
#define STR_DLGBROWSER_VIEWS_ALLYTREES      2029
/* Lobby team start side: header selector, row start cell and dropdown actions */
#define STR_DLGLOBBY_TEAM_SIDE              2102
#define STR_DLGLOBBY_SIDE_ANY               2103
#define STR_DLGLOBBY_SIDE_N                 2104
#define STR_DLGLOBBY_SIDE_E                 2105
#define STR_DLGLOBBY_SIDE_S                 2106
#define STR_DLGLOBBY_SIDE_W                 2107
#define STR_DLGLOBBY_START_SEA              2108
#define STR_DLGLOBBY_START_TEAM_SIDE        2109
#define STR_DLGLOBBY_START_AUTO             2110
#define STR_DLGLOBBY_START_UNASSIGN         2111
#define STR_DLGLOBBY_START_OFFSIDE_SUFFIX   2112
#define STR_DLGLOBBY_TOOLTIP_TEAM_SIDE      2113
#define STR_DLGLOBBY_TOOLTIP_START_SEA      2114
#define STR_DLGLOBBY_TOOLTIP_START_OFFSIDE  2115
/* Map-preview compass: the two-team N/S or E/W shortcut hover text */
#define STR_DLGLOBBY_TOOLTIP_COMPASS_SET    2124
#define STR_DLGLOBBY_TOOLTIP_COMPASS_CLEAR  2125
/* Gamepad rebinding (Configure Keys → Controller section) */
#define STR_GP_SECTION                      1467
#define STR_GP_REBIND_PROMPT                1468
#define STR_GP_ACTION_FIRE                  1469
#define STR_GP_ACTION_MINE                  1470
#define STR_GP_ACTION_BUILD_CONFIRM         1471
#define STR_GP_ACTION_VIEW_CYCLE            1472
#define STR_GP_ACTION_GUNSIGHT_DEC          1473
#define STR_GP_ACTION_GUNSIGHT_INC          1474
#define STR_GP_ACTION_BUILD_PREV            1475
#define STR_GP_ACTION_BUILD_NEXT            1476
#define STR_GP_ACTION_BUILD_CURSOR_TOGGLE   1477
#define STR_GP_ACTION_QUICK_CHAT            1478
#define STR_GP_ACTION_PAUSE                 1479
#define STR_GP_ACTION_VIEW_PLAYERS          1480

/* Controller Mode pref + connect prompt */
#define STR_CTRL_MODE_HEADER                1481
#define STR_CTRL_MODE_OFF                   1482
#define STR_CTRL_MODE_ON                    1483
#define STR_CTRL_MODE_AUTO                  1484
#define STR_CTRL_MODE_ASK                   1485
#define STR_CTRL_PROMPT_TITLE               1486
#define STR_CTRL_PROMPT_LINE1               1487
#define STR_CTRL_PROMPT_LINE2               1488
#define STR_CTRL_PROMPT_DESC                1489
#define STR_CTRL_PROMPT_NOTNOW              1490
#define STR_CTRL_PROMPT_DONTASK             1491

/* Controller-disconnected dialog */
#define STR_CTRL_DISC_TITLE                 1804
#define STR_CTRL_DISC_MESSAGE               1805
#define STR_CTRL_DISC_BUTTON                1806
#define STR_VOTE_RESPOND_IN_PLAYERS         1807
#define STR_ALLIANCE_RESPOND_IN_PLAYERS     1808

/* On-screen keyboard (controller text entry) */
#define STR_OSK_LEGEND                      1809
#define STR_OSK_BKSP                        1810
#define STR_OSK_SHIFT                       1811
#define STR_OSK_SPACE                       1812
#define STR_OSK_ENTER                       1813

/* In-game settings — UI scale override */
#define STR_DLGSETTINGS_UISCALE             1823
#define STR_DLGSETTINGS_UISCALE_AUTO        1824
#define STR_DLGSETTINGS_UISCALE_SMALL       1825
#define STR_DLGSETTINGS_UISCALE_MEDIUM      1826
#define STR_DLGSETTINGS_UISCALE_LARGE       1827
#define STR_ALLIANCE_RANKED_DISABLED        1828

/* Settings dialog — tab labels (shared by the pre-game modal and the
 * in-game overlay). General/Controls/Network reuse existing strings. */
#define STR_DLGSETTINGS_TAB_DISPLAYSOUND    1844
#define STR_DLGSETTINGS_TAB_GAMEHUD         1845
#define STR_DLGSETTINGS_TAB_SESSION         1846

/* Hosting settings tab */
#define STR_DLGSETTINGS_TAB_HOSTING             1906
#define STR_DLGSETTINGS_HOSTING_PORT            1907
#define STR_DLGSETTINGS_HOSTING_ALLOWSPEC       1908
#define STR_DLGSETTINGS_HOSTING_MAXSPEC         1909
#define STR_DLGSETTINGS_HOSTING_MAPUPLOADS      1910
#define STR_DLGSETTINGS_HOSTING_UPLOAD_OFF      1911
#define STR_DLGSETTINGS_HOSTING_UPLOAD_ALLOW    1912
#define STR_DLGSETTINGS_HOSTING_UPLOAD_PERSIST  1913
#define STR_DLGSETTINGS_HOSTING_UPLOADDIR       1914
#define STR_DLGSETTINGS_HOSTING_UPLOAD_MAXFILES 1915
#define STR_DLGSETTINGS_HOSTING_UPLOAD_MAXSTORAGE 1916
#define STR_DLGSETTINGS_HOSTING_ENABLELOG       1917
#define STR_DLGSETTINGS_HOSTING_LOGDIR          1919
#define STR_DLGSETTINGS_HOSTING_SERVEREPLAY     1937
#define STR_DLGSETTINGS_HOSTING_APPLYNOTE       1920
#define STR_DLGSETTINGS_HOSTING_VOICE           2119
#define STR_DLGSETTINGS_HOSTING_VOICE_ON        2120
#define STR_DLGSETTINGS_HOSTING_VOICE_OFF       2121
#define STR_DLGSETTINGS_HOSTING_VOICE_PROXIMITY 2122
#define STR_DLGSETTINGS_HOSTING_VOICE_TIP       2123

/* Log viewer Options menu */
#define STR_LV_HIDE_LOBBY                   1921

/* Voice section of the Display/Sound settings tab */
#define STR_DLGSETTINGS_VOICE                   2030
#define STR_DLGSETTINGS_VOICE_LOOPBACK          2031
#define STR_DLGSETTINGS_VOICE_MICGAIN           2032
#define STR_DLGSETTINGS_VOICE_LEVEL             2033
#define STR_DLGSETTINGS_VOICE_ENABLE            2034
#define STR_DLGSETTINGS_VOICE_MODE              2035
#define STR_DLGSETTINGS_VOICE_MODE_OFF          2036
#define STR_DLGSETTINGS_VOICE_MODE_PTT          2037
#define STR_DLGSETTINGS_VOICE_MODE_OPEN         2038
#define STR_DLGSETTINGS_VOICE_PTTKEY            2039
#define STR_DLGSETTINGS_VOICE_TRANSMITTING      2040
#define STR_DLGSETTINGS_VOICE_NOTTRANSMITTING   2041
#define STR_DLGSETTINGS_VOICE_VOLUME            2042
#define STR_DLGSETTINGS_VOICE_TANKICONS         2052
#define STR_DLGSETTINGS_VOICE_ECHOCANCEL        2053
#define STR_DLGSETTINGS_VOICE_MICTEST_RECORDING 2054
#define STR_DLGSETTINGS_VOICE_MICTEST_PLAYING   2055
#define STR_DLGSETTINGS_VOICE_ECHOCANCEL_UNAVAILABLE 2056
#define STR_DLGSETTINGS_VOICE_ECHOCANCEL_PLATFORM 2057
#define STR_DLGSETTINGS_VOICE_MICDEVICE         2060
#define STR_DLGSETTINGS_VOICE_OUTDEVICE         2061
#define STR_DLGSETTINGS_VOICE_DEVICE_DEFAULT    2062
#define STR_DLGSETTINGS_VOICE_SERVER_OFF        2126

/* Key setup — push to talk binding */
#define STR_DLGKEYSETUP_PUSHTOTALK              2043

/* Key setup — self-mute binding */
#define STR_DLGKEYSETUP_MUTEMIC                 2059

/* Players panel — microphone state icon */
#define STR_PLAYER_TIP_VOICE_TALKING            2044
#define STR_PLAYER_TIP_VOICE_IDLE               2045
#define STR_PLAYER_TIP_VOICE_SELFMUTED          2046
#define STR_PLAYER_TIP_VOICE_NOMIC              2047
#define STR_PLAYER_TIP_VOICE_MUTEDBYYOU         2048
#define STR_PLAYER_TIP_VOICE_SELF               2049
#define STR_PLAYER_TIP_VOICE_SELF_NOMIC         2050
#define STR_PLAYER_TIP_VOICE_SELF_MUTED         2051

/* Players panel — per-player smart-ping mute toggle (independent of the
   voice/chat mute above) */
#define STR_PLAYER_TIP_PING_SHOWN               2162
#define STR_PLAYER_TIP_PING_MUTED               2163

/* Players panel — the label in front of the All/None/Allies/Nearby row.
   Carries its own colon: a language that does not use one, or that puts a
   space before it, has nowhere to say so if the colon is added in code. */
#define STR_DLGPLAYERS_SELECT                   2164

/* Players panel — per-player playback volume slider */
#define STR_PLAYER_TIP_VOICE_VOLUME             2090

/* Lobby — the local player's voice sub-row */
#define STR_DLGLOBBY_TOOLTIP_VOICE              2095
#define STR_DLGLOBBY_VOICE_OFF                  2096

/* Lobby chat — said once when somebody else is heard and voice is off here */
#define STR_DLGLOBBY_VOICE_HINT                 2153

/* Players panel — a player muted here who is talking anyway */
#define STR_PLAYER_TIP_VOICE_MUTEDBYYOU_TALKING 2097

/* Sound settings — the three volumes, of which the third is
   STR_DLGSETTINGS_VOICE_VOLUME above */
#define STR_DLGSETTINGS_MASTER_VOLUME           2100
#define STR_DLGSETTINGS_EFFECTS_VOLUME          2101

/* Map editor validation */
#define STR_MAPVALIDATE_TOO_MANY_BASES      820
#define STR_MAPVALIDATE_TOO_MANY_PILLS      821
#define STR_MAPVALIDATE_TOO_MANY_STARTS     822
#define STR_MAPVALIDATE_BASE_BAD_TERRAIN    823
#define STR_MAPVALIDATE_BASE_BORDER_ZONE    824
#define STR_MAPVALIDATE_PILL_BAD_TERRAIN    825
#define STR_MAPVALIDATE_PILL_BORDER_ZONE    826
#define STR_MAPVALIDATE_START_NOT_DEEP      827
#define STR_MAPVALIDATE_START_BORDER_ZONE   828
#define STR_MAPVALIDATE_NO_STARTS           829
#define STR_MAPVALIDATE_ONE_START           830
#define STR_MAPVALIDATE_BASE_OVERLAP        831
#define STR_MAPVALIDATE_PILL_OVERLAP        832
#define STR_MAPVALIDATE_BASE_PILL_OVERLAP   833
#define STR_MAPVALIDATE_START_OVERLAP       834
#define STR_MAPVALIDATE_START_BASE_OVERLAP  835
#define STR_MAPVALIDATE_START_PILL_OVERLAP  836

/* In-game votes — back-to-lobby and surrender (menu items, widget
 * titles, ready-screen buttons, disabled-state tooltips) */
#define STR_VOTE_BACK_TO_LOBBY              1390
#define STR_VOTE_SURRENDER                  1391
#define STR_VOTE_DRAW_TAG                   1392
#define STR_VOTE_NEEDS_RUNNING_TIP          1393
#define STR_VOTE_SURRENDER_PICK_TEAM_TIP    1394
#define STR_VOTE_SURRENDER_TWO_TEAMS_TIP    1395
/* Menu item to re-open a closed vote widget. {string1} = the vote name
 * (STR_VOTE_BACK_TO_LOBBY / STR_VOTE_SURRENDER). */
#define STR_VOTE_SHOW                       1862

/* Logviewer — in-game vote events */
#define STR_LV_VOTE_START_LOBBY             1396
#define STR_LV_VOTE_START_SURRENDER         1397
#define STR_LV_VOTE_CAST_YES                1398
#define STR_LV_VOTE_CAST_NO                 1399
#define STR_LV_VOTE_PASSED                  1400
#define STR_LV_VOTE_FAILED                  1401

/* Live-spectator feed: leave-confirm modal, phase badges, window title,
 * events-timeline lines, and the pre-roll countdown overlay. */
#define STR_LV_SPEC_LEAVE_CONFIRM           1849
#define STR_LV_SPEC_PHASE_LOBBY             1850
#define STR_LV_SPEC_PHASE_LIVE              1851
#define STR_LV_SPEC_PHASE_GAMEOVER          1852
#define STR_LV_SPEC_WINDOW_TITLE_FMT        1853
#define STR_LV_SPEC_JOINED                  1854
#define STR_LV_SPEC_LEFT                    1855
#define STR_LV_SPEC_CHAT                    1856
#define STR_LV_SPEC_BEGINS_IN_FMT           1857
#define STR_LV_SPEC_CONNECTING              1858
#define STR_LV_SPEC_CONNECTION_LOST         1859
#define STR_LV_SPEC_BEGINS_NOW              1860
#define STR_LV_SPEC_WINDOW_TITLE_NOMAP_FMT  1861

/* Volume — slider label and Mac preset submenu items */
#define STR_MENU_VOLUME                     1402
#define STR_VOLUME_MUTE                     1403

/* Caption shown in the playfield during the game-over → lobby
 * transition, after a round ends and before the lobby UI appears. */
#define STR_RETURNING_TO_LOBBY              1404

/* Reject toast — server rejected an action because the lobby is in
 * the wrong state (e.g. balance request while a proposal is already
 * in flight). */
#define STR_DLGLOBBY_REJECT_BAD_STATE       1405

/* Balance — host-side failure pill: shown when the server reports
 * that a WBN balance call returned without a usable proposal, so
 * the host doesn't have to wait out the 8 s NOREPLY fallback. */
#define STR_DLGLOBBY_BAL_STATUS_FAILED      1406

/* Network Info — inbound snapshot loss for the last 1-second window.
 * {number} = percentage, {number2} = lost count, {number3} = total expected. */
#define STR_DLGNETINFO_LOSS                 1407

/* Network Info — client prediction reconciliations for the last 1-second
 * window. {number} = reconciles/sec, {string1} = avg position error px,
 * {string2} = max position error px, {string3} = live render-error-offset
 * magnitude px (the correction currently being smoothed out). */
#define STR_DLGNETINFO_RECONCILE            1494

/* Shown when the client gives up after repeated unrecoverable map-desync
 * resyncs and disconnects, asking the player to rejoin. */
#define STR_KICK_MAP_DESYNC                 1495

/* Reject toast — server refused an Add Bot because the lobby is already
 * at the operator-configured -maxbots cap. */
#define STR_DLGLOBBY_REJECT_BOT_LIMIT       1496

/* WinBolo.net 1v1 ladder position shown in the status block.
 * STR_DLGWBN_RANK: {number} = position, {number2} = ranked-player total. */
#define STR_DLGWBN_RANK                     1408
#define STR_DLGWBN_UNRANKED                 1409

/* My Stats dialog — per-mode WinBolo.net play stats. */
#define STR_DLGWBN_STATS_BTN                1410
#define STR_DLGWBN_STATS_TITLE              1411
#define STR_DLGWBN_STATS_NONE               1412
#define STR_DLGWBN_STATS_OPEN               1413
#define STR_DLGWBN_STATS_TOURN              1414
#define STR_DLGWBN_STATS_STRICT             1415
#define STR_DLGWBN_STATS_GAMES              1416
#define STR_DLGWBN_STATS_BASES              1417
#define STR_DLGWBN_STATS_PILLS              1418
#define STR_DLGWBN_STATS_TANKS              1419
#define STR_DLGWBN_STATS_SCORE              1420
#define STR_DLGWBN_STATS_WINS               1421
#define STR_DLGWBN_STATS_LOSES              1422
#define STR_DLGWBN_STATS_RANK               1423

/* First-run online onboarding wizard */
#define STR_DLGONBOARD_TITLE                1424
#define STR_DLGONBOARD_ACCOUNT_TITLE        1425
#define STR_DLGONBOARD_ACCOUNT_DESC         1426
#define STR_DLGONBOARD_NAME_TITLE           1427
#define STR_DLGONBOARD_NAME_DESC            1428
#define STR_DLGONBOARD_KEYS_TITLE           1429
#define STR_DLGONBOARD_KEYS_DESC            1430
#define STR_DLGONBOARD_NEXT                 1431
#define STR_DLGONBOARD_SKIP                 1432
#define STR_DLGONBOARD_FINISH               1433

/* WinBolo.net browser signup link */
#define STR_DLGWBN_CREATE_ACCOUNT           1434

/* WinBolo.net Steam-auth status suffix */
#define STR_DLGWBN_VIA_STEAM                1435

/* In-client Steam signup form (login popup) */
#define STR_DLGWBN_CREATE_STEAM             1436
#define STR_DLGWBN_EMAIL_OPTIONAL           1437
#define STR_DLGWBN_CREATE_BTN               1438
#define STR_DLGWBN_ERR_USERNAME_TAKEN       1439
#define STR_DLGWBN_ERR_EMAIL_TAKEN          1440
#define STR_DLGWBN_ERR_STEAM_LINKED         1441
#define STR_DLGWBN_ERR_RATE_LIMITED         1442
#define STR_DLGWBN_ERR_USERNAME_UNAVAILABLE 1443
#define STR_DLGWBN_ERR_USERNAME_TOO_LONG    1444
#define STR_DLGWBN_ERR_USERNAME_REQUIRED    1445
#define STR_DLGWBN_ERR_EMAIL_INVALID        1446
#define STR_DLGWBN_ERR_GENERIC              1447

/* Column headers for the My Stats table */
#define STR_DLGWBN_STATS_COL_RANK           1448
#define STR_DLGWBN_STATS_COL_RATING         1449
#define STR_DLGWBN_STATS_COL_GAMES          1450
#define STR_DLGWBN_STATS_COL_WL             1451
#define STR_DLGWBN_STATS_COL_BASES          1452
#define STR_DLGWBN_STATS_COL_PILLS          1453
#define STR_DLGWBN_STATS_COL_TANKS          1454

/* In-client Steam sign-in (existing account) + popup section labels */
#define STR_DLGWBN_SIGNIN_STEAM_BTN         1455
#define STR_DLGWBN_OR_PASSWORD              1456
#define STR_DLGWBN_LINK_HINT                1457
#define STR_DLGWBN_ERR_STEAM_NOT_LINKED     1458
#define STR_DLGWBN_CREATE_BROWSER           1459
#define STR_DLGWBN_EMAIL                    1460
#define STR_DLGWBN_OR                       1461
#define STR_DLGWBN_ERR_STEAM_TICKET         1462
#define STR_DLGWBN_SIGNIN_HEADER            1463
#define STR_DLGLOBBY_MAKE_HOST              1464
#define STR_DLGLOBBY_MAKE_HOST_FMT          1465
#define STR_DLGLOBBY_HOST_CHANGED_FMT       1466

/* BEGIN generated country name IDs — tools/gen_countries.py; do not hand-edit. */
/* Country names */
#define STR_COUNTRY_AD 1532
#define STR_COUNTRY_AE 1533
#define STR_COUNTRY_AF 1534
#define STR_COUNTRY_AG 1535
#define STR_COUNTRY_AI 1536
#define STR_COUNTRY_AL 1537
#define STR_COUNTRY_AM 1538
#define STR_COUNTRY_AO 1539
#define STR_COUNTRY_AQ 1540
#define STR_COUNTRY_AR 1541
#define STR_COUNTRY_AS 1542
#define STR_COUNTRY_AT 1543
#define STR_COUNTRY_AU 1544
#define STR_COUNTRY_AW 1545
#define STR_COUNTRY_AX 1546
#define STR_COUNTRY_AZ 1547
#define STR_COUNTRY_BA 1548
#define STR_COUNTRY_BB 1549
#define STR_COUNTRY_BD 1550
#define STR_COUNTRY_BE 1551
#define STR_COUNTRY_BF 1552
#define STR_COUNTRY_BG 1553
#define STR_COUNTRY_BH 1554
#define STR_COUNTRY_BI 1555
#define STR_COUNTRY_BJ 1556
#define STR_COUNTRY_BL 1557
#define STR_COUNTRY_BM 1558
#define STR_COUNTRY_BN 1559
#define STR_COUNTRY_BO 1560
#define STR_COUNTRY_BQ 1561
#define STR_COUNTRY_BR 1562
#define STR_COUNTRY_BS 1563
#define STR_COUNTRY_BT 1564
#define STR_COUNTRY_BV 1565
#define STR_COUNTRY_BW 1566
#define STR_COUNTRY_BY 1567
#define STR_COUNTRY_BZ 1568
#define STR_COUNTRY_CA 1569
#define STR_COUNTRY_CC 1570
#define STR_COUNTRY_CD 1571
#define STR_COUNTRY_CF 1572
#define STR_COUNTRY_CG 1573
#define STR_COUNTRY_CH 1574
#define STR_COUNTRY_CI 1575
#define STR_COUNTRY_CK 1576
#define STR_COUNTRY_CL 1577
#define STR_COUNTRY_CM 1578
#define STR_COUNTRY_CN 1579
#define STR_COUNTRY_CO 1580
#define STR_COUNTRY_CP 1581
#define STR_COUNTRY_CR 1582
#define STR_COUNTRY_CU 1583
#define STR_COUNTRY_CV 1584
#define STR_COUNTRY_CW 1585
#define STR_COUNTRY_CX 1586
#define STR_COUNTRY_CY 1587
#define STR_COUNTRY_CZ 1588
#define STR_COUNTRY_DE 1589
#define STR_COUNTRY_DG 1590
#define STR_COUNTRY_DJ 1591
#define STR_COUNTRY_DK 1592
#define STR_COUNTRY_DM 1593
#define STR_COUNTRY_DO 1594
#define STR_COUNTRY_DZ 1595
#define STR_COUNTRY_EC 1596
#define STR_COUNTRY_EE 1597
#define STR_COUNTRY_EG 1598
#define STR_COUNTRY_EH 1599
#define STR_COUNTRY_ER 1600
#define STR_COUNTRY_ES 1601
#define STR_COUNTRY_ET 1602
#define STR_COUNTRY_EU 1603
#define STR_COUNTRY_FI 1604
#define STR_COUNTRY_FJ 1605
#define STR_COUNTRY_FK 1606
#define STR_COUNTRY_FM 1607
#define STR_COUNTRY_FO 1608
#define STR_COUNTRY_FR 1609
#define STR_COUNTRY_GA 1610
#define STR_COUNTRY_GB 1611
#define STR_COUNTRY_GD 1612
#define STR_COUNTRY_GE 1613
#define STR_COUNTRY_GF 1614
#define STR_COUNTRY_GG 1615
#define STR_COUNTRY_GH 1616
#define STR_COUNTRY_GI 1617
#define STR_COUNTRY_GL 1618
#define STR_COUNTRY_GM 1619
#define STR_COUNTRY_GN 1620
#define STR_COUNTRY_GP 1621
#define STR_COUNTRY_GQ 1622
#define STR_COUNTRY_GR 1623
#define STR_COUNTRY_GS 1624
#define STR_COUNTRY_GT 1625
#define STR_COUNTRY_GU 1626
#define STR_COUNTRY_GW 1627
#define STR_COUNTRY_GY 1628
#define STR_COUNTRY_HK 1629
#define STR_COUNTRY_HM 1630
#define STR_COUNTRY_HN 1631
#define STR_COUNTRY_HR 1632
#define STR_COUNTRY_HT 1633
#define STR_COUNTRY_HU 1634
#define STR_COUNTRY_IC 1635
#define STR_COUNTRY_ID 1636
#define STR_COUNTRY_IE 1637
#define STR_COUNTRY_IL 1638
#define STR_COUNTRY_IM 1639
#define STR_COUNTRY_IN 1640
#define STR_COUNTRY_IO 1641
#define STR_COUNTRY_IQ 1642
#define STR_COUNTRY_IR 1643
#define STR_COUNTRY_IS 1644
#define STR_COUNTRY_IT 1645
#define STR_COUNTRY_JE 1646
#define STR_COUNTRY_JM 1647
#define STR_COUNTRY_JO 1648
#define STR_COUNTRY_JP 1649
#define STR_COUNTRY_KE 1650
#define STR_COUNTRY_KG 1651
#define STR_COUNTRY_KH 1652
#define STR_COUNTRY_KI 1653
#define STR_COUNTRY_KM 1654
#define STR_COUNTRY_KN 1655
#define STR_COUNTRY_KP 1656
#define STR_COUNTRY_KR 1657
#define STR_COUNTRY_KW 1658
#define STR_COUNTRY_KY 1659
#define STR_COUNTRY_KZ 1660
#define STR_COUNTRY_LA 1661
#define STR_COUNTRY_LB 1662
#define STR_COUNTRY_LC 1663
#define STR_COUNTRY_LI 1664
#define STR_COUNTRY_LK 1665
#define STR_COUNTRY_LR 1666
#define STR_COUNTRY_LS 1667
#define STR_COUNTRY_LT 1668
#define STR_COUNTRY_LU 1669
#define STR_COUNTRY_LV 1670
#define STR_COUNTRY_LY 1671
#define STR_COUNTRY_MA 1672
#define STR_COUNTRY_MC 1673
#define STR_COUNTRY_MD 1674
#define STR_COUNTRY_ME 1675
#define STR_COUNTRY_MF 1676
#define STR_COUNTRY_MG 1677
#define STR_COUNTRY_MH 1678
#define STR_COUNTRY_MK 1679
#define STR_COUNTRY_ML 1680
#define STR_COUNTRY_MM 1681
#define STR_COUNTRY_MN 1682
#define STR_COUNTRY_MO 1683
#define STR_COUNTRY_MP 1684
#define STR_COUNTRY_MQ 1685
#define STR_COUNTRY_MR 1686
#define STR_COUNTRY_MS 1687
#define STR_COUNTRY_MT 1688
#define STR_COUNTRY_MU 1689
#define STR_COUNTRY_MV 1690
#define STR_COUNTRY_MW 1691
#define STR_COUNTRY_MX 1692
#define STR_COUNTRY_MY 1693
#define STR_COUNTRY_MZ 1694
#define STR_COUNTRY_NA 1695
#define STR_COUNTRY_NC 1696
#define STR_COUNTRY_NE 1697
#define STR_COUNTRY_NF 1698
#define STR_COUNTRY_NG 1699
#define STR_COUNTRY_NI 1700
#define STR_COUNTRY_NL 1701
#define STR_COUNTRY_NO 1702
#define STR_COUNTRY_NP 1703
#define STR_COUNTRY_NR 1704
#define STR_COUNTRY_NU 1705
#define STR_COUNTRY_NZ 1706
#define STR_COUNTRY_OM 1707
#define STR_COUNTRY_PA 1708
#define STR_COUNTRY_PC 1709
#define STR_COUNTRY_PE 1710
#define STR_COUNTRY_PF 1711
#define STR_COUNTRY_PG 1712
#define STR_COUNTRY_PH 1713
#define STR_COUNTRY_PK 1714
#define STR_COUNTRY_PL 1715
#define STR_COUNTRY_PM 1716
#define STR_COUNTRY_PN 1717
#define STR_COUNTRY_PR 1718
#define STR_COUNTRY_PS 1719
#define STR_COUNTRY_PT 1720
#define STR_COUNTRY_PW 1721
#define STR_COUNTRY_PY 1722
#define STR_COUNTRY_QA 1723
#define STR_COUNTRY_RE 1724
#define STR_COUNTRY_RO 1725
#define STR_COUNTRY_RS 1726
#define STR_COUNTRY_RU 1727
#define STR_COUNTRY_RW 1728
#define STR_COUNTRY_SA 1729
#define STR_COUNTRY_SB 1730
#define STR_COUNTRY_SC 1731
#define STR_COUNTRY_SD 1732
#define STR_COUNTRY_SE 1733
#define STR_COUNTRY_SG 1734
#define STR_COUNTRY_SH 1735
#define STR_COUNTRY_SI 1736
#define STR_COUNTRY_SJ 1737
#define STR_COUNTRY_SK 1738
#define STR_COUNTRY_SL 1739
#define STR_COUNTRY_SM 1740
#define STR_COUNTRY_SN 1741
#define STR_COUNTRY_SO 1742
#define STR_COUNTRY_SR 1743
#define STR_COUNTRY_SS 1744
#define STR_COUNTRY_ST 1745
#define STR_COUNTRY_SV 1746
#define STR_COUNTRY_SX 1747
#define STR_COUNTRY_SY 1748
#define STR_COUNTRY_SZ 1749
#define STR_COUNTRY_TC 1750
#define STR_COUNTRY_TD 1751
#define STR_COUNTRY_TF 1752
#define STR_COUNTRY_TG 1753
#define STR_COUNTRY_TH 1754
#define STR_COUNTRY_TJ 1755
#define STR_COUNTRY_TK 1756
#define STR_COUNTRY_TL 1757
#define STR_COUNTRY_TM 1758
#define STR_COUNTRY_TN 1759
#define STR_COUNTRY_TO 1760
#define STR_COUNTRY_TR 1761
#define STR_COUNTRY_TT 1762
#define STR_COUNTRY_TV 1763
#define STR_COUNTRY_TW 1764
#define STR_COUNTRY_TZ 1765
#define STR_COUNTRY_UA 1766
#define STR_COUNTRY_UG 1767
#define STR_COUNTRY_UM 1768
#define STR_COUNTRY_UN 1769
#define STR_COUNTRY_US 1770
#define STR_COUNTRY_UY 1771
#define STR_COUNTRY_UZ 1772
#define STR_COUNTRY_VA 1773
#define STR_COUNTRY_VC 1774
#define STR_COUNTRY_VE 1775
#define STR_COUNTRY_VG 1776
#define STR_COUNTRY_VI 1777
#define STR_COUNTRY_VN 1778
#define STR_COUNTRY_VU 1779
#define STR_COUNTRY_WF 1780
#define STR_COUNTRY_WS 1781
#define STR_COUNTRY_XK 1782
#define STR_COUNTRY_XX 1783
#define STR_COUNTRY_YE 1784
#define STR_COUNTRY_YT 1785
#define STR_COUNTRY_ZA 1786
#define STR_COUNTRY_ZM 1787
#define STR_COUNTRY_ZW 1788
/* END generated country name IDs */

/* About modal — credits & links */
#define STR_DLGABOUT_ADDITIONAL_PROG        1789
#define STR_DLGABOUT_THIRD_PARTY            1790
#define STR_DLGABOUT_AUTHORS                1791
#define STR_DLGABOUT_FORUMS                 1792

/* Web client — join code / connect error dialogs */
#define STR_WEB_JOIN_NEED_SIGNIN            1867
#define STR_WEB_JOIN_NOT_ACCEPTING          1868
#define STR_WEB_JOIN_LINK_INVALID           1869
#define STR_WEB_JOIN_GAME_FULL              1870
#define STR_WEB_JOIN_CODE_UNREACHABLE       1871
#define STR_WEB_CONNECT_FAILED              1872
#define STR_WEB_JOIN_NO_RESPONSE            1873

/* Players menu — open the players panel */
#define STR_MENU_PLAYERS_PANEL                  2058

/* Sound setup: message box when no sound effect at all could be loaded */
#define STR_SOUND_LOAD_FAILED               2118

/* Smart ping (League-style pie menu). The six MESSAGE_PING_* lines are the
 * newswire text a received ping posts; the six STR_PING_* names label the
 * pie's slices, and the direct-ping rows in Key Setup; STR_DLGKEYSETUP_PING
 * heads that section and labels its first chord row, with the two _ALT names
 * for the other two; the rest name the binding chord pieces the key-setup
 * field shows. */
#define MESSAGE_PING_STANDARD               2129
#define MESSAGE_PING_CAUTION                2130
#define MESSAGE_PING_ASSIST                 2131
#define MESSAGE_PING_ATTACK                 2132
#define MESSAGE_PING_ON_MY_WAY              2133
#define MESSAGE_PING_BOT_COMMAND            2134
#define STR_PING_STANDARD                   2135
#define STR_PING_CAUTION                    2136
#define STR_PING_ASSIST                     2137
#define STR_PING_ATTACK                     2138
#define STR_PING_ON_MY_WAY                  2139
#define STR_PING_BOT_COMMAND                2140
#define STR_DLGKEYSETUP_PING                2141
#define STR_DLGKEYSETUP_PING_ALT            2142
#define STR_DLGKEYSETUP_PRESSACHORD         2143
#define STR_PING_MOD_CTRL                   2144
#define STR_PING_MOD_ALT                    2145
#define STR_PING_MOD_SHIFT                  2146
#define STR_PING_MOUSE_LEFT                 2147
#define STR_PING_MOUSE_MIDDLE               2148
#define STR_PING_MOUSE_RIGHT                2149
#define STR_PING_MOUSE_X1                   2150
#define STR_PING_MOUSE_X2                   2151
#define STR_DLGKEYSETUP_PING_ALT2           2152

/* The fourth game type, beside STR_DLGGAMEINFO_OPEN / _TOURN / _STRICT:
 * what a round whose rules come from a scenario is called. */
#define STR_DLGGAMEINFO_SCRIPTED            2196

/* The host's scripts preference, and the two lines the lobby draws about
 * the map's scenario: the one that names it, and the one that stands in
 * its place when the host has the preference switched off. */
#define STR_DLGSETTINGS_HOSTING_SCRIPTS     2197
#define STR_DLGLOBBY_SCENARIO_LBL           2198
#define STR_DLGLOBBY_SCRIPTS_OFF            2199

/* The lobby's reload button, for a host editing its map's script. */
#define STR_DLGLOBBY_RELOAD_SCENARIO        2200

/* Two more lines for the reject toast, beside the STR_DLGLOBBY_REJECT_*
 * group above: a setting the map's scenario decides, and a command sent
 * again before the server will take another. */
#define STR_DLGLOBBY_REJECT_SCENARIO        2201
#define STR_DLGLOBBY_REJECT_COOLDOWN        2202

/* The lobby's scenario chooser: the button on the scenario line that opens
 * it, the dialog's own title, the row that picks none, the two states the
 * list can be in before it has rows, the reason an entry tied to its own map
 * cannot be picked, and the two numbers a row carries. ON_MAP is the line
 * that names the committed map and the scenario a host picked to play over
 * it, rather than naming the scenario alone. */
#define STR_DLGLOBBY_CHOOSE_SCENARIO        2204
#define STR_DLGLOBBY_SCENARIO_TITLE         2205
#define STR_DLGLOBBY_SCENARIO_NONE          2206
#define STR_DLGLOBBY_SCENARIO_WAITING       2207
#define STR_DLGLOBBY_SCENARIO_EMPTY         2208
#define STR_DLGLOBBY_SCENARIO_BOUND         2209
#define STR_DLGLOBBY_SCENARIO_MAXPLAYERS    2210
#define STR_DLGLOBBY_SCENARIO_BOTS          2211
#define STR_DLGLOBBY_SCENARIO_ON_MAP        2212

/* The two lines the lobby draws about what is running, and the dialog behind
 * the icon on each of them. Two lines rather than one because a round has at
 * most one scenario and any number of mods, and the two are not the same
 * question: the scenario arrives with the committed map, so changing it means
 * changing the map, while the mods are what the host picked and are the only
 * half a host edits from here.
 *
 * MODS_LBL is the map panel's, which lists the names and nothing else and
 * goes away when there are none — that panel is a summary of what is loaded
 * and an empty line there is noise. NONE and ACTIVE are the Server Settings
 * column's, which says "none" out loud instead, because that is where the
 * control to change it lives and a button with no label over it explains
 * nothing. ACTIVE carries the count as well as the names, so a host who has
 * stacked more mods than the line has room for still reads how many are
 * running. The count is the whole number and the names stop at the last one
 * that fits, which is why the count is there and not a total taken from the
 * names shown.
 *
 * Both are the summary half of the Server Settings row and neither names
 * mods any more: ENABLED_CB is the checkbox label at the head of that row
 * and says the word once. ACTIVE opens with the dash that binds the two, in
 * the string rather than drawn beside it so a language that joins them
 * differently can move it.
 *
 * FILTER is the chooser's name box, NO_MATCH what the list says when nothing
 * matches it, and DETAILS / DETAILS_TITLE the icon's tooltip and the caption
 * of the dialog it opens. DETAILS is the Server Settings row's button label
 * as well, which opens the chooser rather than that dialog — the word is the
 * same and the row has no icon to hang a tooltip on, so MODS_DETAILS_TIP is
 * what says which of the two it opens. */
#define STR_DLGLOBBY_MODS_LBL               2586
#define STR_DLGLOBBY_MODS_NONE              2587
#define STR_DLGLOBBY_MODS_ACTIVE            2588
#define STR_DLGLOBBY_SCENARIO_FILTER        2589
#define STR_DLGLOBBY_SCENARIO_NO_MATCH      2590
#define STR_DLGLOBBY_SCENARIO_DETAILS       2591
#define STR_DLGLOBBY_SCENARIO_DETAILS_TITLE 2592
#define STR_DLGLOBBY_MODS_ENABLED_CB        2609
#define STR_DLGLOBBY_MODS_DETAILS_TIP       2610
/* The note under the round column's heading, which says what the order of
 * that list means. A host can see the list is ordered and cannot see which
 * end of it wins, and the answer is not guessable: the round runs the files
 * top to bottom, and the first one to answer a question is the one whose
 * answer stands. */
#define STR_DLGLOBBY_SCENARIO_ORDER_NOTE    2611
/* What the left column says when the server offers nothing it has not
 * already handed to the round. A row in the round is taken off that column
 * rather than drawn and refused, so with a short directory the column can
 * empty out, and "this server offers nothing" would be a lie about a
 * server whose whole catalogue is in play. */
#define STR_DLGLOBBY_SCENARIO_ALL_IN_ROUND  2612

/* The header line's mods readout and its hover. The line says "Mods: Yes (3)"
 * or "Mods: No", built from MODS_LBL and the plain STR_YES / STR_NO, so the
 * answer is worded the way the smart-ping entry beside it words its own.
 * HEAD_N is the count that follows the Yes, kept a string of its own because a
 * language that brackets a number differently has to be able to say so.
 *
 * TIP_ON, TIP_OFF and TIP_NONE are the first line of the hover, one per case:
 * the mods are running, the mods are picked but the setting is off, or there
 * are none. Each is a sentence rather than a label, because the hover is the
 * only place a joiner is told which of the three the round is in — the
 * checkbox that decides it is in the host-only settings panel. The names
 * follow, numbered, and need no string of their own.
 *
 * OFF_NOTE is the same fact on the map panel's Mods: line, which lists the
 * names whether or not they run. It is short because it sits at the end of a
 * line of names that may already have wrapped. */
#define STR_DLGLOBBY_MODS_HEAD_N            2613
#define STR_DLGLOBBY_MODS_TIP_ON            2614
#define STR_DLGLOBBY_MODS_TIP_OFF           2615
#define STR_DLGLOBBY_MODS_TIP_NONE          2616
#define STR_DLGLOBBY_MODS_OFF_NOTE          2617

/* The settings window opened off the scenario panel's gear, and the one row
 * in it. TITLE names the panel the window belongs to rather than the setting
 * it holds, because the window is placed beside the gear it was opened from
 * and has to say which panel that was once two are on screen.
 *
 * OPACITY_LBL labels the slider. LINE1 and LINE2 are the two lines under it,
 * kept apart so each wraps on its own: the first says what the number fades
 * and the second says what it leaves alone, and a language that needs two
 * lines for either still gets a break in the same place. */
#define STR_SCNPANEL_SETTINGS_TITLE         2618
#define STR_SCNPANEL_OPACITY_LBL            2619
#define STR_SCNPANEL_OPACITY_LINE1          2620
#define STR_SCNPANEL_OPACITY_LINE2          2621

/* The two-column chooser. OFFERED heads the catalogue on the left and ROUND
 * heads the round's own list on the right, so the two columns say what they
 * are rather than leaving a host to work it out from what is in them.
 *
 * ADD, DROP, EARLIER and LATER are the four arrow buttons' tooltips. They are
 * worded as what the arrow does and not as where it points, because a
 * left-pointing arrow beside a row is only obvious once you already know the
 * two columns are one list each.
 *
 * IN_ROUND, ROUND_FULL, MAP_LOCK and REPLACES are the four things the chooser
 * has to say about a row the host is reaching for. The first three are
 * reasons a row cannot move over and are shown on the disabled arrow; the
 * fourth is what the arrow says when the move will work but will also drop
 * the scenario already there.
 *
 * MAP_LOCK is also the note under the round's locked row, which is the
 * committed map's own scenario. A host who wants a different one changes the
 * map, so that is what it says.
 *
 * ROUND_NONE is what the right column says when the round runs nothing. Said
 * out loud for the reason the empty catalogue is: an empty column cannot tell
 * a round with nothing picked from a column that failed to draw. */
#define STR_DLGLOBBY_SCENARIO_OFFERED       2593
#define STR_DLGLOBBY_SCENARIO_ROUND         2594
#define STR_DLGLOBBY_SCENARIO_ADD           2595
#define STR_DLGLOBBY_SCENARIO_DROP          2596
#define STR_DLGLOBBY_SCENARIO_EARLIER       2597
#define STR_DLGLOBBY_SCENARIO_LATER         2598
#define STR_DLGLOBBY_SCENARIO_MAP_LOCK      2599
#define STR_DLGLOBBY_SCENARIO_IN_ROUND      2600
#define STR_DLGLOBBY_SCENARIO_ROUND_NONE    2601
#define STR_DLGLOBBY_SCENARIO_REPLACES      2602
#define STR_DLGLOBBY_SCENARIO_ROUND_FULL    2603

/* The one-word tag after a row's name, and in the details dialog beside the
 * name it describes. One word and not a sentence: it is read at a glance
 * beside forty other rows, and the sentence that explains the difference is
 * the map editor's, where an author is setting the field rather than reading
 * it.
 *
 * KIND_ALL, KIND_MODS and KIND_SCENARIOS are the three settings of the
 * catalogue's kind filter, which is the box beside the name filter. All is
 * first because it is what the column opens on. */
#define STR_DLGLOBBY_SCENARIO_TAG_MOD       2604
#define STR_DLGLOBBY_SCENARIO_TAG_SCENARIO  2605
#define STR_DLGLOBBY_SCENARIO_KIND_ALL      2606
#define STR_DLGLOBBY_SCENARIO_KIND_MODS     2607
#define STR_DLGLOBBY_SCENARIO_KIND_SCENARIOS 2608

/* The two hosting settings that go with a scenario carried inside a map: the
 * switch that decides whether a map a player uploaded may bring one, and the
 * directory of scenarios this host offers on their own. */
#define STR_DLGSETTINGS_HOSTING_UPLOADSCRIPTS 2213
#define STR_DLGSETTINGS_HOSTING_SCENARIODIR  2216

/* Rule change descriptions */

/* What a simulation rule's value does to it, beside the number: the words
 * simRulesPhrase renders a SimRuleChange as. {string1} rather than {number}
 * because a multiple can be 2.6 and {number} is an integer. */
#define STR_RULE_UNCHANGED                  2217
#define STR_RULE_FASTER                     2218
#define STR_RULE_SLOWER                     2219
#define STR_RULE_MORE                       2220
#define STR_RULE_FEWER                      2221
#define STR_RULE_TWICE                      2222
#define STR_RULE_HALF                       2223
#define STR_RULE_ON                         2224
#define STR_RULE_OFF                        2225

/* The log viewer's own "back to WinBolo" wording, which is not the map
 * editor's STR_MAPEDIT_MENU_RETURN ("Return to Menu"): embedded, the viewer
 * returns to the main menu and the editor to the screen that opened it. */
#define STR_LV_RETURN_MAIN_MENU             2226

/* Map view — the simplified view when zoomed out */
#define STR_DLGSETTINGS_MAPVIEW             2227
#define STR_DLGSETTINGS_SIMPLEZOOM          2228
#define STR_DLGSETTINGS_SIMPLEZOOM_TIP      2229
#define STR_DLGSETTINGS_SIMPLEZOOM_OVERVIEW 2230
#define STR_DLGSETTINGS_SIMPLEZOOM_OVERVIEW_TIP 2231

/* The map editor's scenario script pane: the window and its Window-menu
 * entry, the toolbar above the text, and the line the pane shows for
 * whatever the last read or write did. A script is a loose X.scenario.lua
 * beside X.map, so a map with no file yet has nowhere to keep one. */
#define STR_MAPEDIT_SCENARIO_TITLE          2232
#define STR_MAPEDIT_SCENARIO_NO_MAP         2233
#define STR_MAPEDIT_SCENARIO_SAVE           2234
#define STR_MAPEDIT_SCENARIO_RELOAD         2235
#define STR_MAPEDIT_SCENARIO_UNSAVED        2236
#define STR_MAPEDIT_SCENARIO_LOADED         2237
#define STR_MAPEDIT_SCENARIO_NO_SCRIPT      2238
#define STR_MAPEDIT_SCENARIO_SAVED          2239
#define STR_MAPEDIT_SCENARIO_READ_FAILED    2240
#define STR_MAPEDIT_SCENARIO_WRITE_FAILED   2241
#define STR_MAPEDIT_SCENARIO_TOO_BIG        2242
#define STR_MAPEDIT_SCENARIO_SAVE_REFUSED   2243

/* The rest of the scenario panel: the row of buttons that picks which view
 * the body shows, and the three forms over the manifest the editor holds in
 * memory — what the scenario is called and how it plays, the lobby template
 * it seats, and the simulation rules the author set. The words for the game
 * type itself are STR_DLGGAMEINFO_OPEN / _TOURN / _STRICT, which already say
 * them everywhere else. */
#define STR_MAPEDIT_SCENARIO_VIEW_SCRIPT     2244
#define STR_MAPEDIT_SCENARIO_VIEW_METADATA   2245
#define STR_MAPEDIT_SCENARIO_VIEW_LOBBY      2246
#define STR_MAPEDIT_SCENARIO_VIEW_RULES      2247
#define STR_MAPEDIT_SCENARIO_NAME            2248
#define STR_MAPEDIT_SCENARIO_DESCRIPTION     2249
#define STR_MAPEDIT_SCENARIO_API             2250
#define STR_MAPEDIT_SCENARIO_GAME            2251
#define STR_MAPEDIT_SCENARIO_GAME_NONE       2252
#define STR_MAPEDIT_SCENARIO_BOUND           2253
#define STR_MAPEDIT_SCENARIO_BOUND_NOTE      2254
#define STR_MAPEDIT_SCENARIO_FILL_TO_CAPS    2255
#define STR_MAPEDIT_SCENARIO_MAX_PLAYERS     2256
#define STR_MAPEDIT_SCENARIO_MAX_PLAYERS_ANY 2257
#define STR_MAPEDIT_SCENARIO_EXTRA_TEAMS     2258
#define STR_MAPEDIT_SCENARIO_TEAM            2259
#define STR_MAPEDIT_SCENARIO_TEAM_ID         2260
#define STR_MAPEDIT_SCENARIO_TEAM_BOTS       2261
#define STR_MAPEDIT_SCENARIO_TEAM_MAX_BOTS   2262
#define STR_MAPEDIT_SCENARIO_TEAM_FIELDED    2263
#define STR_MAPEDIT_SCENARIO_TEAM_BRAIN      2264
#define STR_MAPEDIT_SCENARIO_BRAIN_IS_NAME   2265
#define STR_MAPEDIT_SCENARIO_INIT            2266
#define STR_MAPEDIT_SCENARIO_INIT_KEY        2267
#define STR_MAPEDIT_SCENARIO_INIT_VALUE      2268
#define STR_MAPEDIT_SCENARIO_ADD_PAIR        2269
#define STR_MAPEDIT_SCENARIO_INIT_FULL       2270
#define STR_MAPEDIT_SCENARIO_ADD_TEAM        2271
#define STR_MAPEDIT_SCENARIO_REMOVE_TEAM     2272
#define STR_MAPEDIT_SCENARIO_TEAMS_FULL      2273
#define STR_MAPEDIT_SCENARIO_NO_TEAMS        2274
#define STR_MAPEDIT_SCENARIO_REMOVE          2275
#define STR_MAPEDIT_SCENARIO_CLASSIC         2276
#define STR_MAPEDIT_SCENARIO_NO_RULES        2277
#define STR_MAPEDIT_SCENARIO_ADD_RULE        2278
#define STR_MAPEDIT_SCENARIO_FILTER          2279
#define STR_MAPEDIT_SCENARIO_RULES_FULL      2280
#define STR_MAPEDIT_SCENARIO_ADD             2429
#define STR_MAPEDIT_SCENARIO_RANGE           2430
#define STR_MAPEDIT_SCENARIO_RULE_NO_SEL     2431
/* Written before the range on a row whose value is outside it. Only a row
 * that is wrong carries it, so it reads as a mark and not as a column. */
#define STR_MAPEDIT_SCENARIO_RULE_RANGE_BAD  2479

/* The script pane's check: the button that runs the validator, the list of
 * what it found under the editor, and the popup that lists the game.* calls a
 * script may make. The one line about tags is there because the editor hands
 * the validator no sim — it makes none — so the one check that reads a map
 * does not run here. */
#define STR_MAPEDIT_SCENARIO_VALIDATE        2281
#define STR_MAPEDIT_SCENARIO_ISSUES          2282
#define STR_MAPEDIT_SCENARIO_NO_ISSUES       2283
#define STR_MAPEDIT_SCENARIO_NO_SIM_CHECKS   2284
#define STR_MAPEDIT_SCENARIO_ISSUES_DROPPED  2285
#define STR_MAPEDIT_SCENARIO_CALLS           2286

/* Saving the scenario itself: the chunk written on to the map file, the
 * standalone .scenario a mod is, and what stopped either of them. The line
 * after a successful pack says the loose script still wins, because an author
 * who packs and then tests is otherwise running the file beside the map
 * without being told. */
#define STR_MAPEDIT_SCENARIO_PACK_MAP         2287
#define STR_MAPEDIT_SCENARIO_SAVE_MOD         2288
#define STR_MAPEDIT_SCENARIO_PACK_NO_MAP      2289
#define STR_MAPEDIT_SCENARIO_PACKED           2290
#define STR_MAPEDIT_SCENARIO_MOD_SAVED        2291
#define STR_MAPEDIT_SCENARIO_PACK_ISSUES      2292
#define STR_MAPEDIT_SCENARIO_PACK_CONFLICT    2293
#define STR_MAPEDIT_SCENARIO_PACK_FAILED      2294
#define STR_MAPEDIT_SCENARIO_FROM_PACKAGE     2295
#define STR_MAPEDIT_SCENARIO_PACK_READ_FAILED 2296
#define STR_MAPEDIT_SCENARIO_CHUNK_KEPT       2297
#define STR_MAPEDIT_SCENARIO_CHUNK_LOST       2298

/* The tags view: the named tags an author puts on this map's pills, bases and
 * starts, and the named rectangles of squares drawn beside them. A row names
 * the entity the editor numbers it as, which is one less than the number the
 * manifest and a script spell it with. A region's bounds come from the
 * selection tool, so the line about selecting first is what an empty selection
 * says. The last line is what Save as Mod leaves out: a mod plays over a map
 * it has never seen, so it can carry neither. */
#define STR_MAPEDIT_SCENARIO_VIEW_TAGS        2299
#define STR_MAPEDIT_SCENARIO_PILLS            2300
#define STR_MAPEDIT_SCENARIO_BASES            2301
#define STR_MAPEDIT_SCENARIO_STARTS           2302
#define STR_MAPEDIT_SCENARIO_NO_ENTITIES      2303
#define STR_MAPEDIT_SCENARIO_ADD_TAG          2304
#define STR_MAPEDIT_SCENARIO_TAG              2305
#define STR_MAPEDIT_SCENARIO_TAGS_FULL        2306
#define STR_MAPEDIT_SCENARIO_PILL_ROW         2307
#define STR_MAPEDIT_SCENARIO_BASE_ROW         2308
#define STR_MAPEDIT_SCENARIO_START_ROW        2309
#define STR_MAPEDIT_SCENARIO_REGIONS          2310
#define STR_MAPEDIT_SCENARIO_NO_REGIONS       2311
#define STR_MAPEDIT_SCENARIO_REGION_NAME      2312
#define STR_MAPEDIT_SCENARIO_ADD_REGION       2313
#define STR_MAPEDIT_SCENARIO_REGION_FROM_SEL  2314
#define STR_MAPEDIT_SCENARIO_REGION_NO_SEL    2315
#define STR_MAPEDIT_SCENARIO_REGIONS_FULL     2316
#define STR_MAPEDIT_SCENARIO_REGION_X         2317
#define STR_MAPEDIT_SCENARIO_REGION_Y         2318
#define STR_MAPEDIT_SCENARIO_REGION_W         2319
#define STR_MAPEDIT_SCENARIO_REGION_H         2320
#define STR_MAPEDIT_SCENARIO_REGIONS_ON_MAP   2321
#define STR_MAPEDIT_SCENARIO_MOD_DROPS        2322

/* What Reload does, on the button in the script pane's toolbar. The button is
 * live whenever the pane knows where the script goes, with or without a file
 * behind it, so the line says that a script written beside the map after the
 * map was opened is read by pressing it. */
#define STR_MAPEDIT_SCENARIO_RELOAD_TIP       2323

/* The issues list read against text that has moved on. The check keeps what it
 * found while the author types, because the other problems are still worth
 * reading, so this line says the numbers beside them were the numbers in the
 * script as it stood when the check ran. */
#define STR_MAPEDIT_SCENARIO_CHECK_STALE      2324

/* A team number the lobby form will hold and the scenario cannot use: one
 * outside 1 to MAX_TANKS - 1, or one another team in the template already has.
 * The validator reports both when the scenario is packed; these two say it
 * under the field while the number is being typed. */
#define STR_MAPEDIT_SCENARIO_TEAM_ID_RANGE    2325
#define STR_MAPEDIT_SCENARIO_TEAM_ID_TAKEN    2326

/* The functions view: every hook and policy a scenario may define, which of
 * them this script has written, and the two things an author does from the
 * list — start one that is not there, or go to one that is. The words for
 * Add and for the filter box are the rules view's, which already say them
 * in this panel.
 *
 * The last five are what a definition the scanner found can be wrong about.
 * A colon puts an implicit self in front of the parameters while the host
 * calls the field with the hook's own arguments, so every argument shifts
 * by one and a hook that looks written behaves wrongly. Two definitions of
 * one name are both live Lua and the later one silently replaces the
 * earlier, so the row says there are two rather than showing one of them.
 * The other two are hooks the host never finds at all: it reads a hook off
 * the globals and off the scenario table, so a local and a field of any
 * other table are written, listed and never run. */
#define STR_MAPEDIT_SCENARIO_VIEW_FUNCTIONS   2470
#define STR_MAPEDIT_SCENARIO_FN_GOTO          2471
#define STR_MAPEDIT_SCENARIO_FN_IN_SCRIPT     2472
#define STR_MAPEDIT_SCENARIO_FN_ANSWERS       2473
#define STR_MAPEDIT_SCENARIO_FN_GOTO_ONE      2474
#define STR_MAPEDIT_SCENARIO_FN_NONE_YET      2475
#define STR_MAPEDIT_SCENARIO_FN_AT_LINE       2476
#define STR_MAPEDIT_SCENARIO_FN_COLON         2477
#define STR_MAPEDIT_SCENARIO_FN_TWICE         2478
#define STR_MAPEDIT_SCENARIO_FN_LOCAL         2577
#define STR_MAPEDIT_SCENARIO_FN_TABLE         2578

/* The triggers view: the triggers a scenario declares, each one a hook to
 * listen on with a list of tests and a list of actions under it. The view adds
 * and drops whole triggers and sets which hook each runs on; the line beside a
 * trigger counts what it carries, so an author knows what Remove is about to
 * take away.
 *
 * The combo offers hooks alone. A policy is a question the host asks and reads
 * the answer to, which a list of actions has none to give, so a trigger that
 * named one would never run. */
#define STR_MAPEDIT_SCENARIO_VIEW_TRIGGERS    2480
#define STR_MAPEDIT_SCENARIO_NO_TRIGGERS      2481
#define STR_MAPEDIT_SCENARIO_TRIGGER_WHEN     2482
#define STR_MAPEDIT_SCENARIO_TRIGGER_ROWS     2483
#define STR_MAPEDIT_SCENARIO_ADD_TRIGGER      2484
#define STR_MAPEDIT_SCENARIO_TRIGGERS_FULL    2485

/* The tests under one trigger: the list itself, the button that adds a row and
 * the two lines that stand in for it, and the three labels a row is drawn
 * with — the field of the hook's payload, the operator, and the value it is
 * held against.
 *
 * A value is a literal the author states or a reference to another field of
 * the same payload, which is what the read-it-off-the-event box switches
 * between. The three "none yet" lines are what a tag, a region or a team
 * picker says instead of opening on an empty list. */
#define STR_MAPEDIT_SCENARIO_TESTS              2550
#define STR_MAPEDIT_SCENARIO_NO_TESTS           2551
#define STR_MAPEDIT_SCENARIO_ADD_TEST           2552
#define STR_MAPEDIT_SCENARIO_TESTS_FULL         2553
#define STR_MAPEDIT_SCENARIO_HOOK_NO_FIELDS     2554
#define STR_MAPEDIT_SCENARIO_TEST_FIELD         2555
#define STR_MAPEDIT_SCENARIO_TEST_OP            2556
#define STR_MAPEDIT_SCENARIO_TEST_VALUE         2557
#define STR_MAPEDIT_SCENARIO_VALUE_FROM_PAYLOAD 2558
#define STR_MAPEDIT_SCENARIO_NO_TAGS_YET        2559
#define STR_MAPEDIT_SCENARIO_NO_REGIONS_YET     2560
#define STR_MAPEDIT_SCENARIO_NO_TEAMS_YET       2561

/* The actions under one trigger: the list, the button that adds a row and the
 * line that stands in for it at the cap, the op combo's label, and the two
 * buttons that state one more of an op's arguments or one less.
 *
 * call is the action that runs a function of the author's own script rather
 * than a row of the game table, so it has a name to state and a list of what
 * the script defines; nothing types the arguments it passes on, so each says
 * whether it is a number or text. The last line is what a second argument
 * wanting the one long line an action carries is told. */
#define STR_MAPEDIT_SCENARIO_ACTIONS            2562
#define STR_MAPEDIT_SCENARIO_NO_ACTIONS         2563
#define STR_MAPEDIT_SCENARIO_ADD_ACTION         2564
#define STR_MAPEDIT_SCENARIO_ACTIONS_FULL       2565
#define STR_MAPEDIT_SCENARIO_ACTION_OP          2566
#define STR_MAPEDIT_SCENARIO_ADD_ARG            2567
#define STR_MAPEDIT_SCENARIO_DROP_ARG           2568
#define STR_MAPEDIT_SCENARIO_CALL_FUNCTION      2569
#define STR_MAPEDIT_SCENARIO_CALL_RUNS_SCRIPT   2570
#define STR_MAPEDIT_SCENARIO_CALL_NO_FUNCTIONS  2571
#define STR_MAPEDIT_SCENARIO_ARG_NUMBER         2572
#define STR_MAPEDIT_SCENARIO_ARG_TEXT           2573
#define STR_MAPEDIT_SCENARIO_TEXT_ONE_LONG      2574

/* The lobby's rules popup: the button on the scenario line, the window's
 * caption, and the four columns a row is drawn in — the rule, what the
 * classic game plays it at, what the scenario set it to, and what that does
 * to it in words. */
#define STR_DLGLOBBY_SCENARIO_RULES         2328
#define STR_DLGLOBBY_SCENARIO_RULES_TITLE   2329
#define STR_DLGLOBBY_RULES_COL_RULE         2330
#define STR_DLGLOBBY_RULES_COL_CLASSIC      2331
#define STR_DLGLOBBY_RULES_COL_SCENARIO     2332
#define STR_DLGLOBBY_RULES_COL_CHANGE       2333

/* The same popup's per-rule detail: the button on a row that opens it, the
 * caption naming the rule as the manifest spells it, and the one label the
 * four columns above do not already provide. The other three lines in there
 * are labelled with the column ids, which are the same words. */
#define STR_DLGLOBBY_RULE_DETAIL_TITLE      2432
#define STR_DLGLOBBY_RULES_INFO             2433
#define STR_DLGLOBBY_RULES_RANGE            2434

/* A row of a script's details table whose rule a script higher on the list
 * also sets, so this script's value does not play. {string1} = the script
 * that wins (its name, or its file where it has none), {string2} = the value
 * that plays. */
#define STR_DLGLOBBY_RULES_OVERRIDDEN       2622

/* Under the rules table of a mod, on a server that has mods turned off. */
#define STR_DLGLOBBY_DETAILS_MODS_OFF       2624
/* The details dialog's table of what a script implements, from the
 * callbacks block of its manifest: the heading over it (one per kind of
 * script), its three column headers, and the three words the Type column
 * uses (SCN_CB_TYPE_* in scenario_callbacks.h: a hook whose return the engine
 * ignores, a policy whose answer it uses, a hook only a trigger defines). */
#define STR_DLGLOBBY_DETAILS_IMPLEMENTS_MOD      2623
#define STR_DLGLOBBY_DETAILS_IMPLEMENTS_SCENARIO 2625
#define STR_DLGLOBBY_DETAILS_COL_METHOD          2626
#define STR_DLGLOBBY_DETAILS_COL_TYPE            2627
#define STR_DLGLOBBY_DETAILS_COL_OVERVIEW        2628
#define STR_DLGLOBBY_DETAILS_TYPE_EVENT          2629
#define STR_DLGLOBBY_DETAILS_TYPE_QUERY          2630
#define STR_DLGLOBBY_DETAILS_TYPE_TRIGGER        2631
/* The details dialog's rules table: the header over a script's own value.
 * Its own id rather than STR_DLGLOBBY_RULES_COL_SCENARIO, which the host's
 * Rules popup still uses, because the dialog shows mods too. */
#define STR_DLGLOBBY_DETAILS_COL_NEW_VALUE       2632

/* The map panel's warning under the scenario and mods lines, shown only when
 * the server was started with -allow-unsafe-scripts. A sentence rather than a
 * tag, because it is the one place a joiner is told that the scripts they are
 * about to play under are not held to the sandbox. */
#define STR_DLGLOBBY_SCENARIO_UNSAFE             2633

/* Rule descriptions */

/* What each simulation rule governs, one line apiece, shown wherever a rule
 * is named: the editor's rules form and the lobby's rules popup. Named for
 * the rule as SIM_RULE_LIST spells it, and in that order, so the table in
 * sim_rules_phrase.c is generated from the list rather than written out. A
 * rule's own name is not translated — it is what a manifest, a script and an
 * operator line all spell — so there is no id for it here.
 *
 * The numbers run in four stretches rather than one. The first eleven rules
 * had 2334 to 2344, which the fog style strings took as well; moving these
 * eleven to the end was the smaller change of the two. Fifty-three more are
 * the rules the table gained after the middle stretch was numbered, and the
 * last pair are the pillmassage rules, which start again past the map
 * editor's scenario strings because everything up to them was taken. The
 * order of the block is SIM_RULE_LIST's throughout, which is the order that
 * matters, and a hole in the numbers costs nothing: langTable is searched by
 * id rather than indexed by it. */
#define STR_RULE_DESC_tank_reload_ticks          2539
#define STR_RULE_DESC_tank_full_shells           2540
#define STR_RULE_DESC_tank_full_mines            2541
#define STR_RULE_DESC_tank_full_trees            2542
#define STR_RULE_DESC_tank_full_armour           2543
#define STR_RULE_DESC_tank_death_ticks           2544
#define STR_RULE_DESC_tank_water_ticks           2545
#define STR_RULE_DESC_shell_damage               2546
#define STR_RULE_DESC_mine_damage                2547
#define STR_RULE_DESC_mine_damage_range          2486
#define STR_RULE_DESC_mine_fatal_divisor         2487
#define STR_RULE_DESC_water_loss_shells          2488
#define STR_RULE_DESC_water_loss_mines           2489
#define STR_RULE_DESC_just_fired_ticks           2548
#define STR_RULE_DESC_tree_hide_distance         2490
#define STR_RULE_DESC_gunsight_min               2549
#define STR_RULE_DESC_gunsight_max               2345
#define STR_RULE_DESC_tank_accel_rate            2346
#define STR_RULE_DESC_tank_decel_rate            2347
#define STR_RULE_DESC_tank_brake_rate            2348
#define STR_RULE_DESC_tank_autoslow_rate         2349
#define STR_RULE_DESC_tank_min_move              2350
#define STR_RULE_DESC_tank_hit_radius            2491
#define STR_RULE_DESC_tank_collision_distance    2492
#define STR_RULE_DESC_tank_nudge_threshold       2493
#define STR_RULE_DESC_tank_nudge_amount          2494
#define STR_RULE_DESC_tank_nudge_iterations      2495
#define STR_RULE_DESC_tank_bump_decay_shift      2496
#define STR_RULE_DESC_tank_pill_pickup_inset     2497
#define STR_RULE_DESC_tank_boat_exit_inset       2498
#define STR_RULE_DESC_tank_slide_step            2499
#define STR_RULE_DESC_tank_wall_glide            2500
#define STR_RULE_DESC_speed_road                 2351
#define STR_RULE_DESC_speed_grass                2352
#define STR_RULE_DESC_speed_forest               2353
#define STR_RULE_DESC_speed_river                2354
#define STR_RULE_DESC_speed_swamp                2355
#define STR_RULE_DESC_speed_crater               2356
#define STR_RULE_DESC_speed_rubble               2357
#define STR_RULE_DESC_speed_boat                 2358
#define STR_RULE_DESC_speed_deep_sea             2359
#define STR_RULE_DESC_speed_refuel_base          2360
#define STR_RULE_DESC_turn_road                  2361
#define STR_RULE_DESC_turn_grass                 2362
#define STR_RULE_DESC_turn_forest                2363
#define STR_RULE_DESC_turn_river                 2364
#define STR_RULE_DESC_turn_swamp                 2365
#define STR_RULE_DESC_turn_crater                2366
#define STR_RULE_DESC_turn_rubble                2367
#define STR_RULE_DESC_turn_boat                  2368
#define STR_RULE_DESC_turn_deep_sea              2369
#define STR_RULE_DESC_turn_refuel_base           2370
#define STR_RULE_DESC_man_speed_road             2501
#define STR_RULE_DESC_man_speed_grass            2502
#define STR_RULE_DESC_man_speed_forest           2503
#define STR_RULE_DESC_man_speed_river            2504
#define STR_RULE_DESC_man_speed_swamp            2505
#define STR_RULE_DESC_man_speed_crater           2506
#define STR_RULE_DESC_man_speed_rubble           2507
#define STR_RULE_DESC_man_speed_boat             2508
#define STR_RULE_DESC_man_speed_deep_sea         2509
#define STR_RULE_DESC_man_speed_refuel_base      2510
#define STR_RULE_DESC_shell_life                 2371
#define STR_RULE_DESC_shell_speed                2372
#define STR_RULE_DESC_shell_start_add            2373
#define STR_RULE_DESC_lgm_build_ticks            2374
#define STR_RULE_DESC_lgm_cost_road              2375
#define STR_RULE_DESC_lgm_cost_building          2376
#define STR_RULE_DESC_lgm_cost_repair_building   2377
#define STR_RULE_DESC_lgm_cost_pill_repair       2378
#define STR_RULE_DESC_lgm_cost_boat              2379
#define STR_RULE_DESC_lgm_cost_pill_new          2380
#define STR_RULE_DESC_lgm_cost_mine              2381
#define STR_RULE_DESC_lgm_pill_repair_load       2382
#define STR_RULE_DESC_lgm_gather_trees           2383
#define STR_RULE_DESC_lgm_helicopter_speed       2384
#define STR_RULE_DESC_lgm_arrive_tolerance       2511
#define STR_RULE_DESC_lgm_return_tolerance       2512
#define STR_RULE_DESC_lgm_pill_drop_search       2513
#define STR_RULE_DESC_lgm_boat_leave_offset      2514
#define STR_RULE_DESC_lgm_boat_return_offset     2515
#define STR_RULE_DESC_pill_max_armour            2385
#define STR_RULE_DESC_pill_attack_ticks          2386
#define STR_RULE_DESC_pill_attack_min_ticks      2387
#define STR_RULE_DESC_pill_cooldown_ticks        2388
#define STR_RULE_DESC_pill_repair_amount         2389
#define STR_RULE_DESC_pill_range                 2390
#define STR_RULE_DESC_pill_shell_damage          2516
#define STR_RULE_DESC_pill_angry_divisor         2517
#define STR_RULE_DESC_pill_fire_length           2518
#define STR_RULE_DESC_pill_base_defend_range     2519
#define STR_RULE_DESC_pill_aim_iterations        2520
#define STR_RULE_DESC_pill_massage_range         2575
#define STR_RULE_DESC_pill_massage_cosine        2576
#define STR_RULE_DESC_base_full_armour           2391
#define STR_RULE_DESC_base_full_shells           2392
#define STR_RULE_DESC_base_full_mines            2393
#define STR_RULE_DESC_base_capture_armour        2394
#define STR_RULE_DESC_base_hit_armour            2395
#define STR_RULE_DESC_base_min_armour            2396
#define STR_RULE_DESC_base_min_shells            2397
#define STR_RULE_DESC_base_min_mines             2398
#define STR_RULE_DESC_base_armour_give           2399
#define STR_RULE_DESC_base_shells_give           2400
#define STR_RULE_DESC_base_mines_give            2401
#define STR_RULE_DESC_base_refuel_armour_ticks   2402
#define STR_RULE_DESC_base_refuel_shells_ticks   2403
#define STR_RULE_DESC_base_refuel_mines_ticks    2404
#define STR_RULE_DESC_base_regen_ticks           2405
#define STR_RULE_DESC_base_status_range          2521
#define STR_RULE_DESC_base_reveal_range          2522
#define STR_RULE_DESC_building_life              2406
#define STR_RULE_DESC_rubble_life                2407
#define STR_RULE_DESC_grass_life                 2408
#define STR_RULE_DESC_swamp_life                 2409
#define STR_RULE_DESC_mine_fuse_ticks            2410
#define STR_RULE_DESC_big_explosion_threshold    2411
#define STR_RULE_DESC_tank_explosion_damage      2523
#define STR_RULE_DESC_tank_explosion_length      2524
#define STR_RULE_DESC_tank_explosion_move        2525
#define STR_RULE_DESC_tank_explosion_update_ticks 2526
#define STR_RULE_DESC_tank_explosion_width       2527
#define STR_RULE_DESC_tank_explosion_height      2528
#define STR_RULE_DESC_start_tank_range           2529
#define STR_RULE_DESC_start_pill_range           2530
#define STR_RULE_DESC_start_base_range           2531
#define STR_RULE_DESC_start_spawn_separation     2532
#define STR_RULE_DESC_start_scatter_max          2533
#define STR_RULE_DESC_start_neutral_threshold_pct 2534
#define STR_RULE_DESC_sound_soft_range           2535
#define STR_RULE_DESC_sound_none_range           2536
#define STR_RULE_DESC_flood_fill_ticks           2537
#define STR_RULE_DESC_tree_grow_ticks            2412
#define STR_RULE_DESC_tree_grow_initial_ticks    2413
#define STR_RULE_DESC_tree_grow_initial_score    2538
#define STR_RULE_DESC_tree_weight_forest         2414
#define STR_RULE_DESC_tree_weight_grass          2415
#define STR_RULE_DESC_tree_weight_river          2416
#define STR_RULE_DESC_tree_weight_boat           2417
#define STR_RULE_DESC_tree_weight_deep_sea       2418
#define STR_RULE_DESC_tree_weight_swamp          2419
#define STR_RULE_DESC_tree_weight_rubble         2420
#define STR_RULE_DESC_tree_weight_building       2421
#define STR_RULE_DESC_tree_weight_half_building  2422
#define STR_RULE_DESC_tree_weight_crater         2423
#define STR_RULE_DESC_tree_weight_road           2424
#define STR_RULE_DESC_tree_weight_mine           2425

/* Rule range wording */

/* The range a rule accepts, in words: the two ends of a fixed range, a floor
 * with no ceiling of its own, and the wrapper for a rule another rule also
 * caps. {string1} and {string2} rather than {number} because an end can be
 * 0.01 and because the second half of the wrapper is a rule's name. */
#define STR_RULE_RANGE_BETWEEN                   2426
#define STR_RULE_RANGE_FROM                      2427
#define STR_RULE_RANGE_CAPPED                    2428

/* Scenario function descriptions */

/* One line per function a scenario author writes, for the list the editor
 * shows them in: the 25 hooks, then the 10 policies, in the order
 * SCN_HOOK_LIST and SCN_POLICY_LIST hold them.
 *
 * The tail of each symbol is the catalogue's own id column rather than the
 * function's name, because that is the token the description table pastes
 * onto. For a hook the id is the name without its on_ prefix, so
 * STR_SCNFN_DESC_TANK_KILLED is on_tank_killed's; for a policy it is the
 * name in upper case. The names themselves are in SCN_HOOK_LIST and
 * SCN_POLICY_LIST in scenario_lua.h, and docs/SCENARIO_API.md is where each
 * one is set out at length. */
#define STR_SCNFN_DESC_SETUP                 2435
#define STR_SCNFN_DESC_START                 2436
#define STR_SCNFN_DESC_TICK                  2437
#define STR_SCNFN_DESC_END                   2438
#define STR_SCNFN_DESC_LOBBY                 2439
#define STR_SCNFN_DESC_PLAYER_JOIN           2440
#define STR_SCNFN_DESC_PLAYER_LEAVE          2441
#define STR_SCNFN_DESC_TEAM_CHANGED          2442
#define STR_SCNFN_DESC_CHAT                  2443
#define STR_SCNFN_DESC_PING                  2444
#define STR_SCNFN_DESC_TANK_SPAWNED          2445
#define STR_SCNFN_DESC_TANK_KILLED           2446
#define STR_SCNFN_DESC_LGM_DIED              2447
#define STR_SCNFN_DESC_LGM_LANDED            2448
#define STR_SCNFN_DESC_BASE_CAPTURED         2449
#define STR_SCNFN_DESC_BASE_NEUTRALIZED      2450
#define STR_SCNFN_DESC_PILL_CAPTURED         2451
#define STR_SCNFN_DESC_PILL_PLACED           2452
#define STR_SCNFN_DESC_PILL_PICKED_UP        2453
#define STR_SCNFN_DESC_PILL_KILLED           2454
#define STR_SCNFN_DESC_BUILT                 2455
#define STR_SCNFN_DESC_MINE_LAID             2456
#define STR_SCNFN_DESC_MINE_EXPLOSION        2457
#define STR_SCNFN_DESC_ENTER_REGION          2458
#define STR_SCNFN_DESC_LEAVE_REGION          2459
#define STR_SCNFN_DESC_ALLOW_EXTRA_TEAMS     2460
#define STR_SCNFN_DESC_ALLOW_BASE_WIN        2461
#define STR_SCNFN_DESC_CAN_RESPAWN           2462
#define STR_SCNFN_DESC_CAN_BUILD             2463
#define STR_SCNFN_DESC_CAN_CAPTURE           2464
#define STR_SCNFN_DESC_ANNOUNCE              2465
#define STR_SCNFN_DESC_CAN_DIE               2466
#define STR_SCNFN_DESC_ON_CHOOSE_START       2467
#define STR_SCNFN_DESC_SPAWN_LOADOUT         2468
#define STR_SCNFN_DESC_DAMAGE_SCALE          2469

/* The scenario panel's kind control: what the file being edited is allowed
 * to decide. Not the same question as "Built for this map", which is which
 * map the file was written for. */
#define STR_MAPEDIT_SCENARIO_KIND            2579
#define STR_MAPEDIT_SCENARIO_KIND_SCENARIO   2580
#define STR_MAPEDIT_SCENARIO_KIND_MOD        2581
#define STR_MAPEDIT_SCENARIO_KIND_NOTE       2582
#define STR_MAPEDIT_SCENARIO_GAME_MOD        2583

/* What stands where a win-deciding control has gone while the kind says Mod.
 * The game type above is one; this is the other, in the functions view, where
 * the one function whose answer ends a round is left out of the list.
 * {string1} is that function's name, read off the catalogue rather than
 * written into the line. */
#define STR_MAPEDIT_SCENARIO_FN_MOD          2584

/* The hosting settings' combo for scripts players send this host: off, keep
 * for the session, or keep for good, and the directory kept ones go to.
 * "Off" is the map uploads' STR_DLGSETTINGS_HOSTING_UPLOAD_OFF. */
#define STR_DLGSETTINGS_HOSTING_SCRIPTUPLOADS    2634
#define STR_DLGSETTINGS_HOSTING_SCRIPTUPLOAD_SESSION 2635
#define STR_DLGSETTINGS_HOSTING_SCRIPTUPLOAD_KEEP 2636
#define STR_DLGSETTINGS_HOSTING_SCRIPTUPLOADDIR  2637

/* The Mods chooser's rows from the player's own computer. The three words
 * under a row say where it is: on the server, sent there by a player, on
 * this computer. The button sends a file only this computer holds, and its
 * greyed reason for a player who may not change the round. The other
 * greyed reasons are STR_DLGLOBBY_SCRIPT_ERR_DISABLED below and the map
 * upload's STR_DLGLOBBY_UPLOAD_ERR_INFLIGHT. */
#define STR_DLGLOBBY_SCENARIO_SRC_SERVER         2638
#define STR_DLGLOBBY_SCENARIO_SRC_UPLOADED       2639
#define STR_DLGLOBBY_SCENARIO_SRC_LOCAL          2640
#define STR_DLGLOBBY_SCENARIO_SEND               2641
#define STR_DLGLOBBY_SCENARIO_SEND_NOT_HOST      2642

/* Why a server refused a script, where no map upload reason says it: the
 * server takes no scripts, or holds one of that name already. */
#define STR_DLGLOBBY_SCRIPT_ERR_DISABLED         2643
#define STR_DLGLOBBY_SCRIPT_ERR_NAME_TAKEN       2644
/* And the reasons its accept callback gives, one per SCRIPT_REFUSE_* code
 * on the DONE reply: it could not save the file, the package's manifest will
 * not parse, the script will not load ({number} is the line), it declares no
 * scenario table, it asks for a newer api ({number} asked, {number2} the
 * server's), it declares a kind the server does not know, or it is bound to
 * a map. */
#define STR_DLGLOBBY_SCRIPT_ERR_WRITE            2645
#define STR_DLGLOBBY_SCRIPT_ERR_MANIFEST         2646
#define STR_DLGLOBBY_SCRIPT_ERR_SYNTAX           2647
#define STR_DLGLOBBY_SCRIPT_ERR_NO_TABLE         2648
#define STR_DLGLOBBY_SCRIPT_ERR_API              2649
#define STR_DLGLOBBY_SCRIPT_ERR_KIND             2650
#define STR_DLGLOBBY_SCRIPT_ERR_BOUND            2651

/* The details dialog's Settings section, for a script that declares
 * settings in its scenario table (scenario_settings.h): the heading, the
 * line a player who is not the host sees under it, the line a host sees
 * on a server too old to take a change, and a dropdown entry for the
 * declared default ({number} = the value). The settings' own labels are
 * the script's text, not strings here. */
#define STR_DLGLOBBY_DETAILS_SETTINGS            2652
#define STR_DLGLOBBY_DETAILS_SETTINGS_HOST       2653
#define STR_DLGLOBBY_DETAILS_SETTINGS_OLD        2654
#define STR_DLGLOBBY_DETAILS_SETTING_DEFAULT     2655

/* -------------------------------------------------------
 * C declarations — not processed by the RC compiler
 * ------------------------------------------------------- */
#ifndef RC_INVOKED

#include "global.h"
#include "player_flags.h"
#include "lang_message.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Header values parsed from a loaded lang/<code>.txt. Returned by
 * langGetLoadedMeta() once a translation has been loaded. The picker
 * UI uses these for the language list. */
typedef struct {
    char name[64];
    char author[64];
    char notes[256];
} LangFileMeta;

bool langSetup(void);
void langCleanup(void);

/* Load the translation file at `path` (UTF-8, optional BOM, see
 * lang/en.txt for the format). Replaces any previously loaded
 * translation. Returns FALSE on I/O error; the override table is
 * cleared in that case so langGetText() falls back to English. */
bool langLoadFile(const char *path);

/* Free the override table loaded by langLoadFile() and reset the
 * loaded-file metadata. */
void langUnloadFile(void);

/* Returns the header metadata of the currently-loaded file, or NULL
 * if no translation is loaded. */
const LangFileMeta *langGetLoadedMeta(void);

void langGetFileName(char *fileName);
char *langGetText(langid id);

/* Like langGetText, but expands the named placeholders {player},
 * {other}, {number} from `args`. If `args` is NULL it behaves like
 * langGetText. The result points into a small thread-local ring of
 * buffers; the pointer remains valid until the calling thread makes
 * LANG_FMT_RING_BUFFERS more calls (currently 4). */
const char *langGetTextFmt(langid id, const MessageArgs *args);

/* -------------------------------------------------------
 * Language picker — scans lang/<code>.txt files for the
 * Settings → Display dropdown. Header-only parse (does
 * not load the body of any file).
 * ------------------------------------------------------- */
typedef struct {
    char         code[32];           /* basename, lowercased — e.g. "en", "pt-br" */
    char         path[FILENAME_MAX]; /* path passed to langLoadFile() */
    LangFileMeta meta;               /* parsed name=/author=/notes= */
} LangFileEntry;

/* Scan lang/ for *.txt and return a heap-allocated array of entries.
 * The list always begins with a synthetic English entry whose path is
 * empty and whose meta.name is STR_DLGLANG_NAME — selecting it calls
 * langUnloadFile() rather than langLoadFile(). Caller must free with
 * langPickerFreeEntries(). Returns NULL with *outCount = 0 only on
 * allocation failure (the synthetic English entry alone is otherwise
 * always present). */
LangFileEntry *langPickerScan(int *outCount);
void           langPickerFreeEntries(LangFileEntry *entries, int count);

/* Run on first launch (or when the persisted language code is empty):
 * walks SDL_GetPreferredLocales() and matches each preferred locale
 * against the entries returned by langPickerScan(). On a hit, calls
 * langLoadFile() on the matching file and copies the chosen code into
 * outCode (sized outSize bytes); on no hit, leaves the override table
 * empty (English) and writes "" to outCode. */
void langAutoDetect(char *outCode, int outSize);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* RC_INVOKED */

#endif /* _LANG_H */
