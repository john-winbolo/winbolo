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
#define STR_DLGLOBBY_MAP_UNAVAILABLE        606
#define STR_DLGLOBBY_PILLBOXES              607
#define STR_DLGLOBBY_BASES                  608
#define STR_DLGLOBBY_STARTS                 609
#define STR_DLGLOBBY_SKIPMAP                610
#define STR_DLGLOBBY_CANCELSKIP             611
#define STR_DLGLOBBY_VOTES                  612
#define STR_DLGLOBBY_CHAT                   613
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
#define STR_DLGWBN_FETCHERR                 754
#define STR_DLGWBN_UNKNOWN_ERR              755
#define STR_DLGWBN_FILEFILTER               756

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

/* Welcome dialog */
#define STR_DLGWELCOME_WINTITLE             787
#define STR_DLGWELCOME_SINGLE               788
#define STR_DLGWELCOME_LOCAL                789
#define STR_DLGWELCOME_MAPEDITOR            790
#define STR_DLGWELCOME_LOGVIEWER            791
#define STR_DLGWELCOME_INTERNET             792

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

/* Graphics section: tile detail / animation smoothness / skin info */
#define STR_DLGSETTINGS_GFX_DISCLAIMER      1209
#define STR_DLGSETTINGS_TILEDETAIL          1210
#define STR_DLGSETTINGS_TD_CLASSIC          1211
#define STR_DLGSETTINGS_TD_MATCHZOOM        1212
#define STR_DLGSETTINGS_TD_HIGHDETAIL       1213
#define STR_DLGSETTINGS_TD_MATCHZOOM_HINT   1214
#define STR_DLGSETTINGS_ANIMSMOOTH          1215
#define STR_DLGSETTINGS_AS_MATCH            1216
#define STR_DLGSETTINGS_AS_MAX              1217
#define STR_DLGSETTINGS_FORCESMOOTHSHELLS   1218
#define STR_DLGSETTINGS_FORCESMOOTH_TIP     1219
#define STR_DLGSETTINGS_SKINS_DISCLAIMER    1220
#define STR_DLGSETTINGS_SKIN                1221
#define STR_DLGSETTINGS_PREVIEW             1222
#define STR_DLGSETTINGS_INTERP              1223
#define STR_DLGSETTINGS_INTERP_NEAREST      1224
#define STR_DLGSETTINGS_INTERP_LINEAR       1225
#define STR_DLGSETTINGS_INTERP_PIXELART     1226

/* Player-name validation (Phase 2). */
#define STR_NAME_INVALID_EMPTY              1227
#define STR_NAME_INVALID_CHARS              1228
#define STR_NAME_INVALID_MIXED_SCRIPTS      1229
#define STR_NAME_INVALID_RESERVED_PREFIX    1230
#define STR_NAME_INVALID_RESERVED_SUFFIX    1231

/* Settings → Display: country-flag rendering preference (Phase 4). */
#define STR_DLGSETTINGS_SHOW_COUNTRY_FLAGS  1232

/* Verified-priority collision policy (Phase 5). */
#define STR_NAME_RENAMED_BY_VERIFIED        1233
#define STR_NAME_TAKEN_BY_VERIFIED          1234
#define STR_NAME_TAKEN_BY_OTHER_VERIFIED    1235

/* Localized server→client messages (Phase 9d). Sent over the wire as
 * langid + args by serverSendJoinReject / serverSendServerMessage so
 * each client renders in its own locale. */
#define STR_REJECT_INCORRECT_PASSWORD       1236
#define STR_REJECT_GAME_LOCKED              1237
#define STR_REJECT_SERVER_FULL              1238
#define STR_REJECT_NAME_POOL_EXHAUSTED      1239
#define STR_REJECT_INVALID_PLAYER_NAME      1240
#define STR_REJECT_WBN_VERIFY_FAILED        1241
#define STR_KICK_ANNOUNCE                   1242

/* Log viewer comments panel */
#define STR_LV_WIN_COMMENTS                 1243
#define STR_LV_INFO_SIGNIN_TO_COMMENT       1244
#define STR_LV_INFO_NO_WBN_KEY              1245

/* Log viewer File menu — open log from WinBolo.net */
#define STR_LV_MENU_OPEN_WBN                1246

/* Log viewer zoom menu */
#define STR_LV_ZOOM                         1247
#define STR_LV_ZOOM_IN                      1248
#define STR_LV_ZOOM_OUT                     1249

/* System Info panel — server-side bot/sim telemetry labels.
 * Bare nouns (no trailing colon, no format specifiers); colons and
 * numeric format specifiers stay literal in the C format strings. */
#define STR_DLGSYSINFO_SERVER               1250
#define STR_DLGSYSINFO_BOTPOOL              1251
#define STR_DLGSYSINFO_TICK                 1252
#define STR_DLGSYSINFO_BRAIN                1253
#define STR_DLGSYSINFO_SIMULATION           1254
#define STR_DLGSYSINFO_BOTPREP              1255
#define STR_DLGSYSINFO_BRAIN_OVERRUNS       1256

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

/* -------------------------------------------------------
 * C declarations — not processed by the RC compiler
 * ------------------------------------------------------- */
#ifndef RC_INVOKED

#include "../bolo/global.h"
#include "../bolo/player_flags.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int langid;

/* Header values parsed from a loaded lang/<code>.txt. Returned by
 * langGetLoadedMeta() once a translation has been loaded. The picker
 * UI uses these for the language list. */
typedef struct {
    char name[64];
    char author[64];
    char notes[256];
} LangFileMeta;

/* Per-message arguments substituted into named placeholders by
 * langGetTextFmt(). The placeholders are:
 *   {player}                    -> playerName
 *   {other}                     -> otherName
 *   {number}/{number2..4}       -> rendered as %d
 *   {string1}/{string2}         -> arbitrary short strings (e.g. a
 *                                  pre-formatted "%.1f", a duration
 *                                  label, etc.)
 * Substitution is non-recursive — braces inside a substituted value
 * (e.g. a player name with "{ACCEL}" in it) are NOT rescanned. */
#define LANG_MSGARG_STRING_LEN 64

typedef struct {
    char    playerName[PLAYER_NAME_LEN];
    uint8_t playerFlags;        /* PLAYER_FLAG_* bits */
    char    playerCountry[3];   /* ISO 3166-1 alpha-2 + NUL; "" if unknown */
    char    otherName[PLAYER_NAME_LEN];
    uint8_t otherFlags;
    char    otherCountry[3];
    int  number;
    int  number2;
    int  number3;
    int  number4;
    char string1[LANG_MSGARG_STRING_LEN];
    char string2[LANG_MSGARG_STRING_LEN];
} MessageArgs;

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
