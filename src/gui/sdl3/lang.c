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
*Filename:      lang.c
*Author:        John Morrison
*Purpose:
*  Cross-platform string table (English built-in) plus an
*  optional runtime override table loaded from data/lang/<code>.txt.
*  The static langTable[] below remains the source of truth
*  for English; non-English builds layer overrides on top.
*********************************************************/

#include "../lang.h"
#include "../../common/wb_log.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__IPHONEOS__)
#include <dirent.h>
#endif

static char langFileName[FILENAME_MAX];

typedef struct { unsigned int id; const char *text; } LangEntry;

static const LangEntry langTable[] = {
    {154,  "TCP/IP"},
    {157,  "OK"},
    {158,  "Quit"},
    {161,  "English (Default)"},
    {162,  "Author: "},
    {163,  "John Morrison"},
    {166,  "Notes: "},
    {167,  "None."},
    {168,  "Note: For incomplete translations the missing items default back to English."},
    {169,  "About WinBolo"},
    {171,  "Alliance Request"},
    {172,  "Accept"},
    {173,  "Reject"},
    {174,  "{player} requests alliance. Accept?"},
    {179,  "Yes"},
    {180,  "No"},
    {185,  "Game Info"},
    {186,  "Map: "},
    {187,  "Players: {number}"},
    {188,  "Game Type: "},
    {189,  "Hidden Mines: "},
    {191,  "Time Limit:"},
    {193,  "Cancel"},
    {194,  "Game Setup"},
    {195,  "Choose map"},
    {198,  "Open Game (pre-armed)"},
    {199,  "Tournament (free ammo early)"},
    {200,  "Strict Tournament (no free ammo)"},
    {201,  "Allow Hidden Mines"},
    {204,  "Game Password"},
    {205,  "minutes"},
    {206,  "seconds"},
    {207,  "Game start delay"},
    {208,  "Game time limit"},
    {210,  "About {number} minute(s)"},
    {211,  "Open"},
    {212,  "Tournament"},
    {213,  "Strict Tournament"},
    {214,  "Yes (Advantage)"},
    {215,  "Me"},
    {216,  "Key Setup"},
    {218,  "Drive Tank"},
    {220,  "Gun Range"},
    {221,  "Weapons"},
    {222,  "Views"},
    {223,  "Scroll"},
    {226,  "Auto Slowdown"},
    {227,  "Auto Hide Gunsight"},
    {228,  "Faster"},
    {229,  "Slower"},
    {230,  "Turn Left"},
    {231,  "Turn Right"},
    {232,  "Increase"},
    {233,  "Decrease"},
    {234,  "Shoot"},
    {235,  "Lay Mine"},
    {236,  "Tank View"},
    {237,  "Pill View"},
    {2003, "Base View"},
    {2004, "Allied Tank View"},
    {1993, "Map Zoom (hold)"},
    {2005, "Map Follow (toggle)"},
    {2006, "Map Zoom In"},
    {2007, "Map Zoom Out"},
    {238,  "Up"},
    {240,  "Down"},
    {241,  "Left"},
    {242,  "Right"},
    {258,  "Send"},
    {259,  "All Players"},
    {260,  "All Allies"},
    {261,  "All Nearby"},
    {262,  "Selected Players"},
    {263,  "Sending message to {number} player"},
    {264,  "Sending message to {number} players"},
    {265,  "Network Info"},
    {268,  "Server ping: {number} ms"},
    {270,  "Status:"},
    {271,  "Net errors: {number}  \xC2\xB7  {number2} resync"},
    {272,  "Password Required"},
    {273,  "This game requires a password:"},
    {275,  "Enter the new player name for your tank:"},
    {276,  "Tracker Config"},
    {277,  "Use Tracker"},
    {278,  "Tracker Address:"},
    {279,  "Tracker Port:"},
    {280,  "System Info"},
    {281,  "CPU Usage:"},
    {282,  "Sim Modeling:"},
    {283,  "Com Processing:"},
    {286,  "Total:"},
    {289,  "To join an internet Bolo game, you must give the name (or IP address) of a host machine running Bolo, and the UDP port number of the Bolo process on that machine."},
    {290,  "Join"},
    {291,  "Rejoin"},
    {292,  "New"},
    {293,  "Tracker Setup"},
    {294,  "Remember player name"},
    {295,  "Machine Name (or IP address):"},
    {296,  "UDP port of Bolo on that machine:"},
    {297,  "UDP port for Bolo on this machine:"},
    {298,  "Your player name for the game:"},
    {299,  "Click \"New\" to begin a game"},
    {300,  "Click \"Join\" to join an existing game"},
    {301,  "Click \"Rejoin\" to rejoin a game and reclaim your old possessions"},
    {302,  "Server and own ports are the same!"},
    {303,  "Something isn't correct here..."},
    {348,  "LAN Game Finder"},
    {349,  "Tracker Game Finder"},
    {350,  "An error occurred trying to join the game"},
    {383,  "Timelimit has expired. The Game has ended."},
    {384,  "Error Saving Map (Disk full?)"},
    {385,  "Ahead of you is a short river leading inland. Hold\ndown the {ACCEL} key to drive your boat to the end of\nthe river.\n\nWhen you get there, keep holding {ACCEL}. The tank will\ndisembark from the boat and the boat will be left\nmoored at the end of the river"},
    {386,  "You are now on the grass. The tank moves quite\nquickly on grass.\n\n\nKeep pressing {ACCEL} to move ahead to the forest."},
    {387,  "You are now in the forest. The tank moves more\nslowly in the forest than it does on grass.\n\n\nKeep pressing {ACCEL} to move ahead to the swamp."},
    {388,  "You are now in the swamp. The tank moves very\nslowly in swamp. Fortunately there is a road ahead.\n\n\nKeep pressing {ACCEL} to move ahead to the road."},
    {389,  "You are now on the road. The tank can move very\nquickly on road and if you press {ACCEL} the tank will\nspeed up.\n\n\nIf you find yourself going too fast you can press {BRAKE}"},
    {390,  "There are some buildings ahead. Buildings are solid\nobstacles that you cannot drive through, so you will\nhave to find your way through the maze to the other\nside.\n\nTo make the tank turn left press {LEFT}\nTo make the tank turn right press {RIGHT}"},
    {391,  "These round objects with the red guns poking out are\nautomatic pillboxes. They will shoot at any tank\nthat comes within range.\n\n\nFortunately the buildings provide protection from the\nshots, so the pillboxes will not be able to hit you\nunless you wait so long that they manage to\ncompletely shoot their way through the buildings."},
    {392,  "Forest also provides protection from pillboxes, but in\na different way. As well as slowing down the tank a\nlot, driving through forest also limits visibility.\n\n\nWhen you are inside forest you can still clearly see\neverything outside the forest, but pillboxes (and\nother players) cannot see your tank unless you get\nvery close to them."},
    {393,  "Line up your tank with the middle of the strip of\nforest and drive due North. If you go straight\nand don't stray too close to the edges of the forest,\nthe pillboxes will not see you and they will not shoot.\n\n\nDon't drive outside the forest or you'll get blown to pieces.\n"},
    {394,  "There is one other simple way to escape pillboxes -\nspeed.\n\n\nMake sure you are correctly lined up with the centre\nof the road, and hold {ACCEL} to go full speed past the\npillboxes."},
    {395,  "When they notice you they will start shooting, but if\nyou keep going straight ahead and don't lose your\nnerve you will be out of range before the shots can\nhit you.\n\n\nDon't run off the road into the marsh or you'll be a\nsitting duck target for the pillboxes, and you won't be\nable to move quickly enough to escape."},
    {396,  "Ahead of you is a refuelling base. Refuelling bases provide\nshells for you to shoot, mines for you to lay secret traps\nwith, and they also repair your tank's armour back to full\nstrength if it has been damaged.\n\n\nStop on the refuelling base and load up your tank\nwith shells."},
    {397,  "You are on the refuelling base. Your tank will now\nload up with shells and mines, as the indicators on\nthe right will show. Ahead of you, beyond the\nbuildings, is a minefield."},
    {398,  "If you drive your tank into the minefield it will be\ndestroyed. Fortunately you can clear a minefield by\nshooting. Press {FIRE} to shoot.\n\nShoot a hole through the building, and then land a\nshell in the middle of the minefield to detonate it.\nWhen you blow up the mines they will make a big\ncrater in the ground, which you can drive through,\nalthough it is as slow as driving through swamp."},
    {399,  "Ahead is another minefield. Do the same trick again:\nDrive onto the refuelling base, shoot through the\nwall, and land a shell in the middle of the minefield to\ndetonate it.\n\nThis time though, the minefield is adjacent to a large\nriver, and water will flood into the craters. You can\ndrive the tank through the water like you can through\ncraters and marsh."},
    {400,  "WinBolo automatically scrolls the window to show you\nthe area around your tank but you can use the\n{SCROLL_UP}/{SCROLL_DOWN}/{SCROLL_LEFT}/{SCROLL_RIGHT}\nkeys to manually override the view WinBolo chooses.\n\nPress {SCROLL_UP} now to get a better view of the\nriver ahead of you."},
    {401,  "As well as being very slow, driving through water has\nanother disadvantage: It will also deplete your tank's\nsupplies of shells and mines.\n\n\nWhen you get to the other side your tank will still be\nin working condition, but you will have lost all\noffensive capability."},
    {402,  "This time we are going to be more subtle about\ncrossing the river. Instead of just driving through the\nwater, we are going to build a bridge.\n\n\nSelect tree farming mode by clicking on the forest\nicon (top left of the window) and click on the forest.\nYour builder will run out of the tank and harvest one\ntree for building materials."},
    {403,  "After farming some tree, select the road/bridge tool\n(the second button from the top) and click the mouse\non the river at the end of the road.\n\n\nYour builder will run out of the tank and build one\nsegment of bridge. Click again to complete the bridge\nand then drive across to the other side."},
    {404,  "This river is wider than the last, and at the centre it is\ntoo deep for you to build a bridge. If you try to just\ndrive through the tank will sink to the bottom and be\ndestroyed."},
    {405,  "To cross this river you will have to build a boat. Boats\ntake five trees, so click on the five trees to farm them.\nThen select the wall/boat tool (the third button from\nthe top) and click on the water to build a boat.\n\nIf you click on the land you will build a wall instead\nand you will have to farm some more trees to build a\nboat. Drive onto the boat and cross the river to the\nother side."},
    {406,  "There is a pillbox ahead, and some refuelling bases to\nprovide you with ammunition. Collect shells from the\nrefuelling base and attack the pillbox.\n\n\nIt will shoot back, but if you tank gets hit you can easily\nreturn to the refuelling base to get your armour repaired\nback to full strength again."},
    {407,  "With patience and caution you should be able to destroy this\npillbox. Use the skills you have learned to make it easier:\n\n\nHide in the forest so that the pillbox can't see you,\nfarm trees, build roads to make your tank move\nfaster, and build buildings to provide cover against\npillbox shots."},
    {408,  "After you manage to defeat this pillbox, your final\nchallenge of the tutorial is to take out the two\npillboxes guarding the exit, build a boat, and escape.\n\n\nTake special care to watch your tank's armour level -\nif your tank is destroyed you'll be given a new tank\nnearby so you can try again."},
    {409,  "Congratulations. You have completed the WinBolo\ntutorial.\n\n\n\nNow organise some friends to play with and find out\nwhat it is like to compete against intelligent human\nopponents instead of stationary unthinking targets.\n\nIf you'd like to replay the tutorial later, you can\nlaunch it again from the Settings menu."},
    {410,  "Welcome to the WinBolo tutorial. This island introduces\nthe basic principles of WinBolo and leads you through\nthem one at a time. Each new principle will be\ndescribed in a window like this one.\n\nAfter reading each message,\npress {DISMISS} to continue."},
    {411,  "WinBolo is the only authorized clone of Stuart Cheshire's classic Macintosh network game, Bolo.\nYou can find strategy hints and other information from the official website:\n\nhttps://www.winbolo.com\n\nOr come join us on Reddit and Discord\n"},
    {412,  "One quick note about Internet play.\n\nIf you join an online game and there are a lot of network errors\nthen leave the game straight away before\nyou ruin the game for the other players."},
    {413,  "You are in control of a tank, which is currently on a\nboat at sea. Press {ACCEL} to make the tank (and boat) go\nforwards, and press {BRAKE} to make it slow down and\nstop.\n\nDon't tap the keys as if you are typing a letter -\npress and hold them until the tank is going at the speed\nyou want and then let go. Press {ACCEL} now to make the\nboat drive forwards towards the land.\n(Press {DISMISS} first to dismiss the window)"},
    {414,  "You cannot build until your new man parachutes in"},
    {415,  "You cannot build that there"},
    {416,  "You don't have the trees you require to build that"},
    {417,  "You have no pillbox to place"},
    {418,  "You have no mines to place"},
    {419,  "The man cannot build on a tank"},
    {420,  "There is no tree to farm there"},
    {430,  "That pillbox does not need repairing"},
    {431,  "The man cannot build on a mine. It would kill him."},
    {432,  "Online Assistant"},
    {433,  "Newswire"},
    {434,  "Brain"},
    {435,  "\"{other}\" has handed control over to \"{player}\""},
    {436,  "Network Server"},
    {437,  "This Computer"},
    {438,  "{player} captured a Neutral Base"},
    {439,  "{player} captured a Neutral Pillbox"},
    {440,  "{player} just stole pillbox from {other}"},
    {441,  "{player} just stole base from {other}"},
    {442,  "{player} just lost his builder"},
    {443,  "{player} just saved map file"},
    {444,  "{player} has quit game"},
    {445,  "Time Limit has expired. Game is over. Go in peace"},
    {446,  "Tank Sunk in Deep Sea"},
    {447,  "The tracker hostname lookup failed. Tracker notification disabled"},
    {448,  "There was no response from the game you tried to contact.\nThis usually means 1 of 3 things:\n1. You have the address and port fields incorrect.\n2. You are behind some kind of firewall preventing UDP access.\n3. Check that your version of WinBolo is the same as the game"},
    {449,  "This game is currently full"},
    {450,  "Failed to make a connection with the server. Try a different machine or wait a few seconds and try again."},
    {451,  "The password is incorrect"},
    {452,  "Your player name is in use. Select another and try again"},
    {453,  "Sorry, this game is currently not accepting players"},
    {454,  "You were able to connect to the game but you were not allowed to join. This probably means that your player name has been taken. Please try again with a new name"},
    {455,  "Joining"},
    {456,  "OK"},
    {457,  "Failed"},
    {458,  "You have lost your connection to the server.\nNow dropping you to single player mode"},
    {459,  "Send Message"},
    {462,  "Error opening help file."},
    {464,  "If not all skin items are present then the default built in skin/sounds are used."},
    {465,  "The man cannot build under your boat"},
    {467,  "The server was unable to prepare the map data. Please try again"},

    /* System Info panel additions */
    {482,  "Frame Rate:"},
    {483,  "Graphics:"},
    {484,  "AI Tanks:"},

    /* Network Info panel additions */
    {485,  "Server:"},
    {486,  "This game:"},
    {487,  "Ping: min {number} / avg {number2} / max {number3} ms"},
    {488,  "KB/s In:"},
    {489,  "KB/s Out:"},
    {490,  "Packets/sec: {number} in / {number2} out"},
    {491,  "KB/sec: {string1} in / {string2} out"},

    /* Game Info panel additions */
    {492,  "Copy Seed"},
    {493,  "AI Tanks:"},
    {494,  "Full Advantage"},
    {495,  "Unlimited"},

    /* Players panel */
    {496,  "Players"},
    {497,  "All"},
    {498,  "None"},
    {499,  "Allies"},
    {500,  "Nearby"},
    {501,  "Leave Alliance"},
    {502,  "Request Alliance"},
    {503,  "Allow New Players"},

    /* About modal */
    {504,  "WinBolo v1.0.1.7"},
    {505,  "Copyright 1998-2008 John Morrison"},
    {506,  "Bolo Copyright 1987-1995 Stuart Cheshire"},

    /* Join Game confirmation modal */
    {507,  "Join Game?"},
    {508,  "Leave current game and join server?"},
    {509,  "Join"},

    /* Change Player Name modal */
    {510,  "Change Player Name"},

    /* Key Setup modal additions */
    {511,  "(none)"},
    {512,  "Press a key..."},
    {513,  "Press a key to assign, or click Cancel."},
    {514,  "Change"},
    {515,  "Action"},
    {516,  "Key"},
    {517,  "Quick Keys"},
    {518,  "Tree"},
    {519,  "Road"},
    {520,  "Wall"},
    {521,  "Pillbox"},
    {522,  "Mine"},

    /* Settings panel */
    {523,  "Settings"},
    {524,  "Player"},
    {525,  "Player Name:"},
    {526,  "Apply"},
    {527,  "Set Keys..."},
    {528,  "Display"},
    {529,  "Window Size:"},
    {530,  "Relative Steering"},
    {531,  "When off, joystick points the tank directly.\nWhen on, joystick turns left/right relative to tank."},
    {532,  "Tablet UI Mode"},
    {533,  "Labels"},
    {534,  "Message Names:"},
    {535,  "Tank Labels:"},
    {536,  "Sound"},
    {537,  "Messages"},
    {538,  "Game"},

    /* Menu bar */
    {539,  "File"},
    {540,  "New"},
    {541,  "Save Map"},
    {1987, "Map Overview"},
    {1990, "Full Screen"},
    {542,  "Exit"},
    {543,  "Edit"},
    {544,  "Frame Rate"},
    {545,  "Window Size"},
    {546,  "Normal"},
    {547,  "Double"},
    {548,  "Triple"},
    {549,  "Quad"},
    {550,  "Custom (Resizable)"},
    {551,  "Requires {number}x{number2} - exceeds display"},
    {552,  "Smooth Scrolling"},
    {553,  "Automatic Scrolling"},
    {554,  "Show Gunsight"},
    {555,  "Message Sender Names"},
    {556,  "Tank Labels"},
    {557,  "None"},
    {558,  "Short"},
    {559,  "Long"},
    {560,  "Don't label own tank"},
    {561,  "Pillbox Labels"},
    {562,  "Refuelling Base Labels"},
    {563,  "Hide Main View"},
    {564,  "Device:"},
    {565,  "Desktop"},
    {566,  "WinBolo"},
    {567,  "Set Keys"},
    {568,  "Sound Effects"},
    {569,  "Background Sound"},
    {570,  "Sound Keepalive"},
    {571,  "Newswire Messages"},
    {572,  "Assistant Messages"},
    {573,  "AI Brain Messages"},
    {574,  "Network Status Messages"},
    {575,  "Network Debug Messages"},
    {576,  "Settings..."},
    {577,  "Players"},
    {578,  "Send Message"},
    {579,  "Select All"},
    {580,  "Select None"},
    {581,  "Select Allies"},
    {582,  "Select Nearby Tanks"},
    {583,  "Brains"},
    {584,  "Manual"},
    {585,  "Help"},
    {586,  "About"},
    {587,  "Leave Game"},
    {588,  "(no settings)"},
    {589,  "Brain Settings"},

    /* Map overview pop-out */
    {1988, "following"},
    {1989, "free"},

    {590,  "Tutorial"},
    {591,  "Play Tutorial"},
    {592,  "Show on main menu"},
    {593,  "Crash Reporting"},
    {594,  "Enable Crash Reporting"},
    {595,  "Help improve WinBolo by sending crash reports"},
    {596,  "Changes take effect on next launch."},
    {597,  "Close"},
    {598,  "WinBolo - Settings"},
    {599,  "WinBolo - Key Setup"},
    {600,  "WinBolo - Game Lobby"},
    {601,  "Unknown"},
    {602,  "Yes (Full)"},
    {603,  "Hidden"},
    {604,  "Visible"},
    {605,  "Downloading map..."},
    {1946, "Waiting for server..."},
    {606,  "Map preview unavailable"},
    {607,  "Pillboxes:"},
    {608,  "Bases:"},
    {609,  "Starts:"},
    {610,  "Skip Map"},
    {611,  "Cancel Skip"},
    {612,  "{number}/{number2} votes to skip"},
    {613,  "Chat"},
    {1497, "General"},
    {1498, "Team"},
    {1499, "N"},
    {1500, "NE"},
    {1501, "E"},
    {1502, "SE"},
    {1503, "S"},
    {1504, "SW"},
    {1505, "W"},
    {1506, "NW"},
    {1507, "C"},
    {1508, "Unassigned"},
    {1509, "(open)"},
    {1510, "{player} joined {string1}."},
    {1511, "{player} left {string1}."},
    {1512, "Start #{number}\nClick to choose; right-click to assign to someone else"},
    {1513, "Start #{number}\nClick to choose this start"},
    {1514, "Start #{number} - {player}\nDrag to move; right-click to assign to someone else"},
    {1515, "Start #{number} - {player}"},
    {1516, "you"},
    {1517, "Swap this start with:"},
    {1518, "Assign start to:"},
    {1519, "(slot)"},
    {1520, "AI player"},
    {1521, "Windows"},
    {1522, "Linux"},
    {1523, "macOS"},
    {1524, "iOS"},
    {1525, "Android"},
    {1526, "Steam Deck"},
    {1527, "Web"},
    {1528, "{string1} — Supporter"},
    {1529, "Verified WinBolo.net account"},
    {1530, "Steam account linked"},
    {1531, "Playing via Steam"},
    {614,  "Ready"},
    {615,  "Unready"},
    {616,  "Balance Teams"},
    {617,  "Apply Balance"},
    {618,  "Dismiss"},
    {619,  "Leave"},
    {620,  "Leave Game?"},
    {621,  "Are you sure you want to leave this game?"},
    {622,  "{player} [Bot]"},
    {623,  "{player} (You)"},
    {624,  "Starting in {number}..."},
    {625,  "You have lost your connection to the server."},
    {626,  "Me"},
    {627,  "Player"},
    {628,  "Ping"},
    {629,  "Team"},
    {630,  "Ready"},
    {631,  "Slot"},
    {632,  "Player Name"},
    {633,  "Action"},
    {634,  "Add Bot"},
    {635,  "Remove"},
    {636,  "Map"},
    {637,  "Game:"},
    {638,  "Mines:"},
    {639,  "AI:"},
    {640,  "Time:"},
    {642,  "{number}h {string1}m {string2}s"},
    {643,  "{number}m {string1}s"},
    {644,  "{number}s"},
    {645,  "Map:"},
    {646,  "WinBolo - Game Setup"},
    {647,  "Select a map:"},
    {648,  "Back"},
    {649,  "Bases: {number}  Starts: {number2}"},
    {650,  "Pillboxes: {number}"},
    {651,  "Change Map"},
    {652,  "Game Type"},
    {653,  "Open Game"},
    {654,  "Hidden Mines"},
    {655,  "AI Computer Players"},
    {656,  "Allow"},
    {657,  "Advantage"},
    {658,  "Full Map"},
    {659,  "Time Limit"},
    {660,  "Team Setup"},
    {661,  "Number of AI players:"},
    {662,  "No brains found"},
    {663,  "Brain"},
    {664,  "You"},
    {665,  "Bot {number}"},
    {666,  "No computer tanks"},
    {667,  "Allow computer tanks"},
    {668,  "Allow with advantage"},
    {669,  "Allow with full map"},
    {670,  "No preview"},
    {671,  "Start Game"},
    {672,  "Strict"},
    {673,  "WinBolo - UDP (Internet) Setup"},
    {674,  "Error"},
    {675,  "WinBolo - {string1}"},
    {676,  "Refresh"},
    {677,  "Loading games list{string1}"},
    {678,  "Click Refresh to load games..."},
    {679,  "Click Refresh to scan..."},
    {680,  "Games loaded."},
    {681,  "No games found. Start one!"},
    {682,  "Search failed."},
    {683,  "Searching..."},
    {684,  "Server"},
    {685,  "Map"},
    {686,  "Players"},
    {687,  "Type"},
    {688,  "AI"},
    {689,  "Bases"},
    {690,  "Pills"},
    {691,  "Ping"},
    {692,  "Filter:"},
    {693,  "All Types"},
    {694,  "Unlocked Only"},
    {695,  "All Lobby"},
    {696,  "In Lobby"},
    {697,  "Starting"},
    {698,  "Status: {string1}  |  {number} servers  |  Pinging {number2}..."},
    {699,  "Status: {string1}  |  {number} servers"},
    {700,  "New Game"},
    {701,  "Player Name"},
    {702,  "Manual"},
    {703,  "Server is running a different version of WinBolo."},
    {704,  "You must set a player name first."},
    {706,  "Adv"},
    {707,  "Full"},
    {708,  "WinBolo - Log Browser"},
    {709,  "WinBolo.net Log Browser"},
    {710,  "Recent"},
    {711,  "Top Rated"},
    {712,  "Most Downloaded"},
    {713,  "Search"},
    {714,  "Loading..."},
    {715,  "Could not connect to WinBolo.net"},
    {716,  "Min Players"},
    {717,  "Search Filters:"},
    {718,  "Player"},
    {719,  "Map"},
    {720,  "Search"},
    {721,  "Error: {string1}"},
    {722,  "Map"},
    {723,  "Type"},
    {724,  "Players"},
    {725,  "Rating"},
    {726,  "Size"},
    {727,  "Date"},
    {728,  "Players: "},
    {729,  "{number} player(s)"},
    {730,  "Duration: {string1}"},
    {731,  "Rating: {string1}/10 ({number})"},
    {732,  "Downloads: {number}"},
    {733,  "Size: {string1}"},
    {734,  "Comments ({number}):"},
    {735,  "No comments yet."},
    {736,  "Add Comment:"},
    {737,  "Write a comment..."},
    {738,  "Post"},
    {739,  "Comment posted!"},
    {740,  "Loading details..."},
    {741,  "Downloading..."},
    {742,  "View Log"},
    {743,  "Log file is not available for this game."},
    {744,  "< Prev"},
    {745,  "Next >"},
    {746,  "Page {number} of {number2}  ({number3} total)"},
    {747,  "Open File..."},
    {748,  "Failed to parse response"},
    {749,  "Network error"},
    {750,  "Failed to load details"},
    {751,  "Download failed"},
    {752,  "Could not determine save path"},
    {753,  "Select a game to view its details"},
    {754,  "Fetch failed"},
    {755,  "Unknown error"},
    {756,  "WinBolo Log Files"},
    {1863, "My Games"},
    {1864, "Login or create an account to view your recent games"},
    {1865, "You don't have any recorded games yet."},
    /* Lobby "Last round" panel — between-rounds scoreboard + awards. */
    {1899, "Last round"},
    {1900, "Name"},
    {1901, "Kills"},
    {1902, "Deaths"},
    {1903, "Base"},
    {1904, "Pill"},
    {1905, "Damage"},
    {1874, "Builds"},
    {1947, "Awards"},
    {1876, "(empty)"},
    {1877, "{string1} owned {string2}"},
    {1878, "Most Kills"},
    {1879, "Most Deaths"},
    {1880, "Best K/D"},
    {1881, "Most Base Captures"},
    {1882, "Most Pillbox Captures"},
    {1883, "Nemesis"},
    {1884, "Demolition"},
    {1885, "Sharpshooter"},
    {1886, "Warmonger"},
    {1887, "Survivor"},
    {1888, "Engineer"},
    {1889, "Sapper"},
    {1890, "Fish Food"},
    {1891, "LGM Hunter"},
    {1892, "Cannon Fodder"},
    {1893, "Lumberjack"},
    {1894, "Wasteful"},
    {1895, "Biggest Fumble"},
    {1896, "Last round"},
    {1897, "Builder Kills"},
    {1898, "Builder Deaths"},
    /* Lobby "Last round" panel — the round's highlight clips. */
    {1922, "Highlights"},
    {1923, "No highlights this round"},
    {1924, "{string1} ({player})"},
    {1925, "{string1} ({player} vs {other})"},
    {1926, "Team wipe: {number} down ({player})"},
    {1927, "Steal ({player} from {other})"},
    {1928, "LGM sweep: {number} down ({player})"},
    {1929, "Fumble: {number} pills dropped ({player})"},
    {1930, "Drowned with {number} pills ({player})"},
    {1931, "Drowned ({player})"},
    {1932, "Front collapse: {number} tiles taken ({player})"},
    {1933, "Front collapse: {number} tiles taken"},
    {1934, "Turning point: {number} tiles swung ({player})"},
    {1935, "Turning point: {number} tiles swung"},
    {1936, "Highlight"},
    {1944, "Pill sweep: {number} grabbed ({player})"},
    {1945, "All-out action: {number} events"},
    /* Lobby "Last round" panel — the reel's delivery state. */
    {1938, "Asking the server for the replay..."},
    {1939, "Downloading replay... {number}%"},
    {1940, "This server does not share replays"},
    {1941, "No replay available for this round"},
    {1942, "The round was too long to send"},
    {757,  "WinBolo - Set Player Name"},
    {758,  "Your player name is set by WinBolo.net."},
    {759,  "Please enter your player name."},
    {760,  "Sorry, you can not leave this blank"},
    {761,  "Sorry, names can not begin with a '*'"},
    {762,  "That name is already in use by another player."},
    {763,  "WinBolo - Skin Selection"},
    {764,  "No Skin (Default)"},
    {765,  "N/A"},
    {766,  "Select a skin:"},
    {767,  "Name:"},
    {768,  "Author:"},
    {769,  "Notes:"},
    {770,  "Unable to load Skin File"},
    {771,  "WinBolo - Tracker Config"},
    {772,  "Invalid port number."},
    {773,  "Checking WinBolo.net..."},
    {774,  "WinBolo.net:"},
    {775,  "Signed in"},
    {776,  "(expires {string1})"},
    {777,  "Sign out of WBN"},
    {778,  "Not signed in"},
    {779,  "Sign in to WBN"},
    {780,  "Sign in to WinBolo.net"},
    {781,  "Sign in to WinBolo.net to track your rank and stats, and take part in the community forums."},
    {782,  "Username:"},
    {783,  "Password:"},
    {784,  "Signing in..."},
    {785,  "Sign in"},
    {786,  "Please enter your username and password."},
    {1943, "WinBolo.net accounts can only be changed from the main menu, not during a game."},
    {787,  "WinBolo - Game Selection"},
    {788,  "Single Player"},
    {789,  "Local"},
    {790,  "Map Editor"},
    {791,  "Log Viewer"},
    {792,  "Internet"},
    {793,  "Everard Island (Inbuilt)"},
    {794,  "Load a Map"},
    {795,  "Load from Device..."},
    {796,  "Generate Random Map"},
    {797,  "Random Map"},
    {798,  "Click Generate to preview"},
    {799,  "No preview available"},
    {800,  "Pillboxes: {number}  Bases: {number2}  Starts: {number3}"},
    {801,  "Map Files"},
    {802,  "All Files"},
    {803,  "Status"},
    {804,  "Kills: {number}  Deaths: {number2}"},
    {805,  "Tank Resources"},
    {806,  "Shells"},
    {807,  "Mines"},
    {808,  "Armour"},
    {809,  "Trees"},
    {810,  "Pillboxes"},
    {811,  "Bases"},
    {812,  "Tanks"},
    {813,  "Pill {number}"},
    {814,  "Base {number}"},
    {815,  "Tank {number}"},
    {816,  "Msg"},
    {817,  "Ply"},
    {818,  "Set"},
    {819,  "Set Player Name"},

    /* Map editor validation */
    {820,  "Too many bases: {number} (max {number2})"},
    {821,  "Too many pillboxes: {number} (max {number2})"},
    {822,  "Too many starts: {number} (max {number2})"},
    {823,  "Base #{number} at ({number2},{number3}): on non-traversable terrain"},
    {824,  "Base #{number} at ({number2},{number3}): in mine border zone"},
    {825,  "Pillbox #{number} at ({number2},{number3}): on non-traversable terrain"},
    {826,  "Pillbox #{number} at ({number2},{number3}): in mine border zone"},
    {827,  "Start #{number} at ({number2},{number3}): must be on deep sea"},
    {828,  "Start #{number} at ({number2},{number3}): in mine border zone"},
    {829,  "No start positions placed \xe2\x80\x94 map is unplayable"},
    {830,  "Only 1 start position \xe2\x80\x94 single player only"},
    {831,  "Base #{number} and Base #{number2} overlap at ({number3},{number4})"},
    {832,  "Pillbox #{number} and Pillbox #{number2} overlap at ({number3},{number4})"},
    {833,  "Base #{number} and Pillbox #{number2} overlap at ({number3},{number4})"},
    {834,  "Start #{number} and Start #{number2} overlap at ({number3},{number4})"},
    {835,  "Start #{number} and Base #{number2} overlap at ({number3},{number4})"},
    {836,  "Start #{number} and Pillbox #{number2} overlap at ({number3},{number4})"},

    /* Map editor menu bar */
    {837,  "Open..."},
    {838,  "Recent Files"},
    {839,  "Save"},
    {840,  "Save As..."},
    {841,  "Export as PNG..."},
    {842,  "Return to Menu"},
    {843,  "Undo"},
    {844,  "Redo"},
    {845,  "Cut"},
    {846,  "Copy"},
    {847,  "Paste"},
    {848,  "Map"},
    {849,  "Random Map..."},
    {850,  "Text..."},
    {851,  "Import Image..."},
    {852,  "Validate"},
    {853,  "Mirror Horizontal {string1}"},
    {854,  "Mirror Vertical {string1}"},
    {855,  "Rotate 90\xC2\xB0 CW {string1}"},
    {856,  "Rotate 180\xC2\xB0 {string1}"},
    {857,  "(selection)"},
    {858,  "(full map)"},
    {859,  "Options"},
    {860,  "Center Map"},
    {861,  "Point Start Points"},
    {862,  "Show Grid"},
    {863,  "Show Mines"},
    {864,  "Show Pillbox Ranges"},
    {865,  "Window"},

    /* Map editor windows */
    {866,  "Terrain"},
    {867,  "Tools"},
    {868,  "Inspector"},
    {869,  "Objects"},
    {870,  "Overview"},
    {871,  "Statistics"},
    {872,  "Stamp Library"},

    /* Map editor terrain palette */
    {873,  "Deep Sea"},
    {874,  "Grass"},
    {875,  "Forest"},
    {876,  "Road"},
    {877,  "Building"},
    {878,  "Half Building"},
    {879,  "River"},
    {880,  "Swamp"},
    {881,  "Crater"},
    {882,  "Rubble"},
    {883,  "Boat"},
    {884,  "Mine Tool"},
    {885,  "Checkered"},
    {886,  "Full"},
    {887,  "Random"},
    {888,  "Clear Mines"},

    /* Map editor drawing tools */
    {889,  "Pencil"},
    {890,  "Line"},
    {891,  "Rectangle"},
    {892,  "Filled Rectangle"},
    {893,  "Oval"},
    {894,  "Filled Oval"},
    {895,  "Selection"},
    {896,  "Fill"},
    {897,  "Maze"},
    {898,  "Generate"},
    {899,  "Wand"},
    {900,  "Text"},
    {901,  "Text (stamp text onto map)"},
    {902,  "Brush"},
    {903,  "Square"},
    {904,  "Circle"},
    {905,  "Base"},
    {906,  "Pillbox"},
    {907,  "Start"},

    /* Map editor status bar */
    {908,  "Tile: {number}, {number2}"},
    {909,  "Tile: --"},
    {910,  "Zoom: {string1}"},
    {911,  "Modified"},

    /* Map editor modals */
    {912,  "Unsaved Changes"},
    {913,  "The map has unsaved changes.\nDo you want to save before continuing?"},
    {914,  "Go To Coordinates"},
    {915,  "X"},
    {916,  "Y"},

    /* Map editor inspector */
    {917,  "No object selected"},
    {918,  "Base #{number}"},
    {919,  "Pillbox #{number}"},
    {920,  "Start #{number}"},
    {921,  "Position: ({number}, {number2})"},
    {922,  "Owner"},
    {923,  "Armour"},
    {924,  "Speed"},
    {925,  "Direction"},
    {926,  "Neutral"},
    {927,  "Player {number}"},

    /* Map editor object list */
    {928,  "Bases ({number}/{number2})"},
    {929,  "Pillboxes ({number}/{number2})"},
    {930,  "Starts ({number}/{number2})"},
    {931,  "#{number}  ({number2}, {number3})  {string1}"},
    {932,  "#{number}  ({number2}, {number3})  {string1}  Armour: {number4}"},
    {933,  "#{number}  ({number2}, {number3})  Dir: {string1}"},

    /* Map editor validation panel */
    {934,  "Validation"},
    {935,  "No issues"},
    {936,  "{number} error(s)"},
    {937,  "0 errors"},
    {938,  "{number} warning(s)"},
    {939,  "0 warnings"},
    {940,  "Validation Errors"},
    {941,  "Map has {number} error(s). Fix them before saving."},

    /* Map editor generate dialog */
    {942,  "Generate Random Map"},
    {943,  "Generate Random Area"},
    {944,  "Scope: Selection ({number},{number2})-({number3},{number4})"},
    {945,  "Scope: Full map ({number},{number2})-({number3},{number4})"},

    /* Map editor text tool dialog */
    {946,  "Text Tool"},
    {947,  "Text:"},
    {948,  "Built-in"},
    {949,  "System Font"},
    {950,  "Font Size"},
    {951,  "Style"},
    {952,  "Small (5x7)"},
    {953,  "Medium (10x14)"},
    {954,  "Large (15x21)"},
    {955,  "XL (20x28)"},
    {956,  "Regular"},
    {957,  "Bold"},
    {958,  "Italic"},
    {959,  "Bold Italic"},
    {960,  "Font Family"},
    {961,  "{string1} (bundled)"},
    {962,  "Size (px)"},
    {963,  "No fonts found. Add .ttf files to data/fonts/"},
    {964,  "Text Terrain"},
    {965,  "Edge Terrain"},
    {966,  "Background"},
    {967,  "Preview ({number}x{number2} tiles):"},
    {968,  "Text too large (max 256x256 tiles) or font error"},
    {969,  "Generate"},

    /* Map editor maze settings */
    {970,  "Maze Settings"},
    {971,  "Region: ({number},{number2})-({number3},{number4})  {string1}"},
    {972,  "Drag on map to generate"},
    {973,  "Algorithm"},
    {974,  "Labyrinth"},
    {975,  "Open"},
    {976,  "Wall"},
    {977,  "Corridor"},
    {978,  "Entries"},
    {979,  "Rooms"},
    {980,  "Wall Terrain"},
    {981,  "Corridor Terrain"},
    {982,  "Generate Settings"},

    /* Map editor statistics panel */
    {983,  "Terrain Distribution"},
    {984,  "{string1}: {number} / {number2}"},
    {985,  "Mines: {number} ({string1}% land)"},
    {986,  "Spatial Analysis"},
    {987,  "Land coverage: {string1}%"},
    {988,  "Largest landmass: {string1}%"},
    {989,  "Base spacing: {string1} tiles"},
    {990,  "Pill spacing: {string1} tiles"},
    {991,  "Symmetry: {string1} ({string2}%)"},
    {992,  "Mirror-H:   {string1}%"},
    {993,  "Mirror-V:   {string1}%"},
    {994,  "4-corner:   {string1}%"},
    {995,  "Rotate-180: {string1}%"},
    {996,  "(stale \xe2\x80\x94 click Refresh)"},
    {997,  "Refresh Spatial"},
    {998,  "Starts"},
    {999,  "Half-Building"},

    /* Map editor image import */
    {1000, "Import Image"},
    {1001, "File:"},
    {1002, "(none)"},
    {1003, "Browse..."},
    {1004, "Source: {number}x{number2} pixels"},
    {1005, "Preview:"},
    {1006, "Scale Mode:"},
    {1007, "Fit selection"},
    {1008, "Fit playable"},
    {1009, "Output: {number} x {number2} tiles"},
    {1010, "Colors:"},
    {1011, "Re-detect"},
    {1012, "Color Mapping"},
    {1013, "Import"},

    /* Map editor stamp library */
    {1014, "Bundled Stamps"},
    {1015, "User Stamps"},
    {1016, "No user stamps yet."},
    {1017, "Use \"Save Clipboard...\" to create one."},
    {1018, "Delete"},
    {1019, "Save Clipboard..."},
    {1020, "Copy a selection first (Ctrl+C)"},
    {1021, "Import..."},
    {1022, "Untitled"},
    {1023, "Save Stamp"},
    {1024, "Enter a name for this stamp:"},
    {1025, "Name"},
    {1026, "Save"},

    /* Map editor export PNG */
    {1027, "Export as PNG"},
    {1028, "Preview"},
    {1029, "Mode:"},
    {1030, "Full resolution (4096x4096)"},
    {1031, "Preview size:"},
    {1032, "Options:"},
    {1033, "Show objects"},
    {1034, "Show mines"},
    {1035, "Show grid (full mode only)"},
    {1036, "Export..."},

    /* Map generator panel */
    {1037, "Locked: won't change on Randomize"},
    {1038, "Unlocked: will change on Randomize"},
    {1039, "Generator"},
    {1040, "Tournament"},
    {1041, "Natural"},
    {1042, "Maze"},
    {1043, "Fractal"},
    {1044, "Seed"},
    {1045, "Randomize"},
    {1046, "Symmetry"},
    {1047, "4-Corner Mirror"},
    {1048, "Mirror Horizontal"},
    {1049, "Mirror Vertical"},
    {1050, "Rotate 180\xC2\xB0"},
    {1051, "Rotate 90\xC2\xB0"},
    {1052, "Land Mass %"},
    {1053, "Roughness"},
    {1054, "Low"},
    {1055, "Medium"},
    {1056, "High"},
    {1057, "Include Roads"},
    {1058, "Map Style"},
    {1059, "Ocean"},
    {1060, "Continent"},
    {1061, "Islands"},
    {1062, "Archipelago"},
    {1063, "Inland"},
    {1064, "Terrain Mix (% land share):"},
    {1065, "Grass %"},
    {1066, "Forest %"},
    {1067, "Building %"},
    {1068, "Swamp %"},
    {1069, "River %"},
    {1070, "Boat %"},
    {1071, "Remaining grass: {number}%"},
    {1072, "Mine Density %"},
    {1073, "River Count"},
    {1074, "City Count"},
    {1075, "Maze Count"},
    {1076, "Wall Thickness"},
    {1077, "Corridor Width"},
    {1078, "City Rooms"},
    {1079, "Land Coverage %"},
    {1080, "Detail Passes"},
    {1081, "Coast Jaggedness"},
    {1082, "Terrain Layers"},
    {1083, "Rivers"},
    {1084, "Lakes"},

    /* Log viewer sim-replay messages */
    {1085, "{player} joined the game."},
    {1086, "{string1} has joined game"},
    {1087, "{player} just requested alliance with {other}"},
    {1088, "{player} just accepted alliance with {other}"},
    {1089, "{player} just left alliance"},
    {1090, "Message to all from {player}: {string1}"},
    {1091, "Message from {player} to {other}: {string1}"},
    {1092, "Server Message: {string1}"},
    {1093, "{player} has died"},
    {1094, "{player} just killed player {other}"},
    {1095, "{player} just rejoined game."},
    {1096, "{player} is leaving game."},
    {1097, "Lobby opened."},
    {1098, "Game started."},
    {1099, "{player} is ready."},
    {1100, "{player} is no longer ready."},
    {1101, "{player} left their team."},
    {1102, "{player} joined team {number}."},
    {1103, "Countdown started."},
    {1104, "Countdown cancelled."},
    {1105, "{player} voted to skip map."},
    {1106, "Map skipped. New map: {string1}"},
    {1107, "Team balance applied."},

    /* Log viewer main menu */
    {1108, "Open"},
    {1109, "Action"},
    {1110, "Play"},
    {1111, "Pause"},
    {1112, "Stop"},
    {1113, "Fast Forward"},
    {1114, "Rewind"},
    {1115, "Mode"},
    {1116, "Information"},
    {1117, "Select Team"},
    {1118, "Use Team Colours"},
    {1119, "Tank Centred"},
    {1120, "DNS Lookups"},
    {1121, "Team Colours"},
    {1122, "Windows"},
    {1123, "Controls"},
    {1124, "Events"},
    {1125, "Game Information"},
    {1126, "Item Information"},
    {1127, "Reset Window Positions"},

    /* Log viewer dialogs */
    {1128, "Assign colours to each player team:"},
    {1129, "Player {number}:"},
    {1130, "Neutral:"},
    {1131, "WinBolo Log Viewer"},
    {1132, "Version: {string1}"},
    {1133, "This program is free software; you can redistribute it\nand/or modify it under the terms of the GNU General\nPublic License as published by the Free Software Foundation."},
    {1134, "Website:"},

    /* Log viewer team colour names */
    {1135, "Grey"},
    {1136, "Khaki"},
    {1137, "Green"},
    {1138, "Pink"},
    {1139, "Yellow"},
    {1140, "Light Blue"},
    {1141, "Orange"},
    {1142, "Light Purple"},
    {1143, "Aqua"},
    {1144, "Light Green"},
    {1145, "Light Grey"},
    {1146, "Red"},
    {1147, "Blue"},
    {1148, "Brown"},
    {1149, "Light Pink"},
    {1150, "Pale Green"},
    {1151, "Purple"},

    /* Log viewer game info panel */
    {1152, "{number} second(s)"},
    {1153, "Map Name: {string1}"},
    {1154, "Game Type: {string1}"},
    {1155, "Hidden Mines: {string1}"},
    {1156, "Computer Tanks: {string1}"},
    {1157, "Time Limit: {string1}"},
    {1158, "Start Delay: {string1}"},
    {1159, "WBN Key:"},
    {1160, "Start Time: {string1}"},

    /* Log viewer item info panel */
    {1161, "{string1} ({number})"},
    {1162, "X: {number}, Y: {number2}"},
    {1163, "No item selected"},
    {1164, "Location: {string1}"},
    {1165, "Owner: {string1}"},
    {1166, "Armour: {number}"},
    {1167, "Shells: {number}"},
    {1168, "Mines: {number}"},
    {1169, "In Tank: {string1}"},
    {1170, "Center on Map"},

    /* Log viewer playback controls */
    {1171, "<< Rew"},
    {1172, "Play >"},
    {1173, "Fwd >>"},
    {1174, "Speed:"},
    {1175, "{string1} / {string2}"},
    {1176, "-{string1} remaining"},
    {1177, "No log loaded"},

    /* Log viewer events panel */
    {1178, "Copy"},
    {1179, "Copy All"},
    {1180, "Clear All"},
    {1181, "Auto Scroll"},

    /* Log viewer end-of-log marker */
    {1182, "End of Log File Reached"},

    /* iOS Settings panel */
    {1183, "Zoom"},
    {1184, "Performance"},
    {1185, "FPS: {number}"},

    /* iOS disconnect-to-menu */
    {1186, "You have lost your connection to the server.\nReturning to menu."},

    /* Language picker (Settings → Display) */
    {1187, "Language:"},
    {1188, "Existing message-log entries won't change language until they're regenerated."},

    /* Hosted multiplayer / NAT traversal — Phase 1-4 networking work */
    {1189, "Requires port forwarding for non-LAN players to join. (Coming: automatic NAT setup.)"},
    {1190, "Failed to load map from server"},
    {1191, "Error starting server"},
    {1192, "Checking server reachability..."},
    {1193, "Server accessible"},
    {1194, "Server unreachable"},
    {1195, "Trying to open a firewall port via UPnP / NAT-PMP and confirming the tracker can reach back through your network. This usually completes within 30 seconds."},
    {1196, "Port forwarded automatically via UPnP/NAT-PMP at {string1}:{number}. Joiners connect directly with no further steps required."},
    {1197, "Direct port forwarding could not be established, but NAT traversal is active. Joiners coordinate through the tracker to punch through your network's firewall. This works for most home networks; joiners on symmetric NAT or carrier-grade NAT may still fail to connect."},
    {1198, "Symmetric NAT detected — your network rewrites the source port for every destination, which prevents joiners from reaching you even via NAT traversal. To host successfully, manually forward UDP port {number} on your router to this machine."},
    {1199, "Could not open a firewall port automatically (UPnP/NAT-PMP refused or unavailable) and the tracker could not confirm bidirectional reachability. To host successfully, manually forward UDP port {number} on your router to this machine."},
    {1200, "Server Reachability"},
    {1201, "Test connectivity"},
    {1202, "Testing..."},
    {1203, "Reachable from internet"},
    {1204, "No reply from tracker"},
    {1205, "Network"},
    {1206, "Settings take effect on the next hosted game."},
    {1207, "Use UPnP / NAT-PMP for automatic port forwarding"},
    {1208, "Use NAT traversal (hole-punching) via tracker"},
    {1209, "Player name is empty."},
    {1210, "Player name contains disallowed characters."},
    {1211, "Player name mixes incompatible scripts."},
    {1212, "Player name cannot start with '*'."},
    {1213, "Player name cannot end with '-unverified'."},
    {1215, "{player} was renamed because {other} joined verified"},
    {1216, "That display name belongs to a verified player. Please pick another."},
    {1217, "That display name is in use by another verified player. Please pick another."},
    {1218, "Incorrect password"},
    {1219, "Game is locked"},
    {1220, "Server full"},
    {1221, "Server name pool exhausted"},
    {1222, "Invalid player name"},
    {1223, "WinBolo.net verification failed: {string1}"},
    {1224, "{player} has been server kicked."},

    /* Log viewer comments panel */
    {1225, "Comments"},
    {1226, "Sign in via WinBolo (in the main game) to post a comment"},
    {1227, "Open a log with a WinBolo.net key to view comments"},

    /* Log viewer File menu — open log from WinBolo.net */
    {1228, "Open from WinBolo.net..."},

    /* Log viewer zoom menu */
    {1229, "Zoom"},
    {1230, "Zoom In"},
    {1231, "Zoom Out"},

    /* System Info panel — server-side bot/sim telemetry labels */
    {1232, "Server"},
    {1233, "Bot pool"},
    {1234, "Tick"},
    {1235, "Brain"},
    {1236, "Simulation"},
    {1237, "Bot prep"},
    {1238, "Brain overruns"},
    {1467, "Controller"},
    {1468, "Press a button or pull a trigger..."},
    {1469, "Fire"},
    {1470, "Lay Mine"},
    {1471, "Execute Build"},
    {1472, "Cycle View"},
    {1473, "Decrease Gunsight"},
    {1474, "Increase Gunsight"},
    {1475, "Previous Build Type"},
    {1476, "Next Build Type"},
    {1477, "Toggle Build Cursor"},
    {1478, "Quick Chat"},
    {1479, "Quick menu"},
    {1480, "Status Overlay"},

    /* Controller Mode pref + connect prompt */
    {1481, "Controller Mode"},
    {1482, "Off"},
    {1483, "On"},
    {1484, "Auto"},
    {1485, "Ask when controller connected"},
    {1486, "Controller detected"},
    {1487, "A gamepad has been connected."},
    {1488, "Switch to Controller Mode?"},
    {1489, "(Hides the menu bar; Start opens a controller-friendly pause menu.\nChange later in Settings.)"},
    {1490, "Not now"},
    {1491, "Don't ask again"},
    {1492, "Keyboard"},
    {1493, "Gray letterbox bars"},
    {1494, "Reconciles: {number}/s, err avg {string1}px, max {string2}px, smoothing {string3}px"},
    {1495, "Map out of sync with the server \xE2\x80\x94 please rejoin."},
    {1496, "bot limit reached"},

    /* macOS native menu — App / File / Window menu items mirrored by
     * src/gui/sdl3/platform/mac_menubar.mm. Not referenced by the
     * in-window menu. */
    {1239, "About WinBolo"},
    {1240, "Preferences…"},
    {1241, "Services"},
    {1242, "Hide WinBolo"},
    {1243, "Hide Others"},
    {1244, "Show All"},
    {1245, "Quit WinBolo"},
    {1246, "Window"},
    {1247, "Minimize"},
    {1248, "Zoom"},
    {1249, "Enter Full Screen"},
    {1250, "Bring All to Front"},
    {1251, "Find Internet Game…"},
    {1252, "Find LAN Game…"},
    {1253, "Join by Address…"},
    {1254, "Open Map Editor"},
    {1255, "Open Log Viewer"},

    /* macOS Log Viewer app-menu items — mirrored by
     * src/logviewer/platform/mac_menubar.mm in the standalone Log
     * Viewer.app only. */
    {1256, "About Log Viewer"},
    {1257, "Hide Log Viewer"},
    {1258, "Quit Log Viewer"},

    /* macOS Map Editor app-menu items — surfaced only by the standalone
     * MapEditor.app via src/mapeditor/platform/mac_menubar.mm. */
    {1259, "Hide Map Editor"},
    {1260, "Quit Map Editor"},

    /* Settings → Network: WinBolo.net news auto-show toggle. */
    {1261, "Show news from winbolo.net on startup"},

    /* News popup + welcome News button strings. */
    {1262, "News"},
    {1263, "News"},
    {1264, "WinBolo News"},
    {1265, "WinBolo can show you new posts from winbolo.net when you launch the game. We will check WBN once at startup and only open this window when there is something new."},
    {1266, "You can change this anytime in Settings."},
    {1267, "Show news"},
    {1268, "Don't show"},
    {1269, "Loading…"},
    {1270, "Don't auto-show news in future"},
    {1271, "Comments: {number}"},

    /* Welcome screen full screen button. The label names where the button
     * takes you, so it is picked from the window's current state. */
    {1991, "Switch to Classic"},
    {1992, "Switch to Full Screen"},

    /* Touch (tablet/mobile) siblings of the tutorial strings whose
     * desktop wording assumes a keyboard or mouse. Picked at display
     * time by tutorialResolveSegments() when uiModeIsTablet() is true. */
    {468,  "Ahead of you is a short river leading inland. Push\nthe thumbstick forward to drive your boat to the end\nof the river.\n\nWhen you get there, keep the thumbstick pushed\nforward. The tank will disembark from the boat and the\nboat will be left moored at the end of the river"},
    {469,  "You are now on the grass. The tank moves quite\nquickly on grass.\n\n\nKeep the thumbstick pushed forward to move ahead\nto the forest."},
    {470,  "You are now in the forest. The tank moves more\nslowly in the forest than it does on grass.\n\n\nKeep the thumbstick pushed forward to move ahead\nto the swamp."},
    {471,  "You are now in the swamp. The tank moves very\nslowly in swamp. Fortunately there is a road ahead.\n\n\nKeep the thumbstick pushed forward to move ahead\nto the road."},
    {472,  "You are now on the road. The tank can move very\nquickly on road and pushing the thumbstick all the\nway forward will speed it up.\n\n\nIf you find yourself going too fast, pull the\nthumbstick back to slow down."},
    {473,  "There are some buildings ahead. Buildings are solid\nobstacles that you cannot drive through, so you will\nhave to find your way through the maze to the other\nside.\n\nPush the thumbstick to the left or right to turn the\ntank in that direction."},
    {474,  "There is one other simple way to escape pillboxes -\nspeed.\n\n\nMake sure you are correctly lined up with the centre\nof the road, and push the thumbstick fully forward to\ngo full speed past the pillboxes."},
    {475,  "If you drive your tank into the minefield it will be\ndestroyed. Fortunately you can clear a minefield by\nshooting. Tap the fire button to shoot.\n\nShoot a hole through the building, and then land a\nshell in the middle of the minefield to detonate it.\nWhen you blow up the mines they will make a big\ncrater in the ground, which you can drive through,\nalthough it is as slow as driving through swamp."},
    {476,  "WinBolo automatically scrolls the window to show you\nthe area around your tank, but you can drag the map\nwith a finger to manually override the view.\n\nDrag the map down now to bring the river ahead\nof you into view."},
    {477,  "This time we are going to be more subtle about\ncrossing the river. Instead of just driving through the\nwater, we are going to build a bridge.\n\n\nSelect tree farming mode by tapping the forest icon\n(top left of the window) and tap on the forest. Your\nbuilder will run out of the tank and harvest one tree\nfor building materials."},
    {478,  "After farming some tree, select the road/bridge tool\n(the second button from the top) and tap on the\nriver at the end of the road.\n\n\nYour builder will run out of the tank and build one\nsegment of bridge. Tap again to complete the bridge\nand then drive across to the other side."},
    {479,  "To cross this river you will have to build a boat. Boats\ntake five trees, so tap on the five trees to farm them.\nThen select the wall/boat tool (the third button from\nthe top) and tap on the water to build a boat.\n\nIf you tap on the land you will build a wall instead\nand you will have to farm some more trees to build a\nboat. Drive onto the boat and cross the river to the\nother side."},
    {480,  "Welcome to the WinBolo tutorial. This island introduces\nthe basic principles of WinBolo and leads you through\nthem one at a time. Each new principle will be\ndescribed in a window like this one.\n\nAfter reading each message, you can proceed by\ntapping the \"OK\" button."},
    {481,  "You are in control of a tank, which is currently on a\nboat at sea. Use the thumbstick on the left side of the\nscreen to drive: push it forward to accelerate, pull it\nback to slow down and stop.\n\nThe further you push the thumbstick, the faster the\ntank goes. Push the thumbstick forward now to make\nthe boat drive forwards towards the land.\n(Tap OK first to dismiss this window)"},
    {1830, "Ahead of you is a short river leading inland. Push\nthe left stick forward to drive your boat to the end\nof the river.\n\nWhen you get there, keep the stick pushed forward.\nThe tank will disembark from the boat and the boat will\nbe left moored at the end of the river"},
    {1831, "You are now on the grass. The tank moves quite\nquickly on grass.\n\n\nKeep the left stick pushed forward to move ahead\nto the forest."},
    {1832, "You are now in the forest. The tank moves more\nslowly in the forest than it does on grass.\n\n\nKeep the left stick pushed forward to move ahead\nto the swamp."},
    {1833, "You are now in the swamp. The tank moves very\nslowly in swamp. Fortunately there is a road ahead.\n\n\nKeep the left stick pushed forward to move ahead\nto the road."},
    {1834, "You are now on the road. The tank can move very\nquickly on road, and pushing the left stick all the\nway forward will speed it up.\n\n\nIf you find yourself going too fast, pull the left\nstick back to slow down."},
    {1835, "There are some buildings ahead. Buildings are solid\nobstacles that you cannot drive through, so you will\nhave to find your way through the maze to the other\nside.\n\nPush the left stick to the left or right to turn the\ntank in that direction."},
    {1836, "There is one other simple way to escape pillboxes -\nspeed.\n\n\nMake sure you are correctly lined up with the centre\nof the road, and push the left stick fully forward to\ngo full speed past the pillboxes."},
    {1837, "If you drive your tank into the minefield it will be\ndestroyed. Fortunately you can clear a minefield by\nshooting. Press {FIRE} to shoot.\n\nShoot a hole through the building, and then land a\nshell in the middle of the minefield to detonate it.\nWhen you blow up the mines they will make a big\ncrater in the ground, which you can drive through,\nalthough it is as slow as driving through swamp."},
    {1838, "WinBolo automatically scrolls the window to show you\nthe area around your tank, but you can push the right\nstick to manually override the view WinBolo chooses.\n\nPush the right stick up now to get a better view of\nthe river ahead of you."},
    {1839, "This time we are going to be more subtle about\ncrossing the river. Instead of just driving through the\nwater, we are going to build a bridge.\n\nPull {BUILD_MODE} to switch from scrolling into build\nmode - the right stick now moves a build cursor instead\nof the view. Press {BUILD_TOOL} to select tree-farming\nmode (top left of the window), aim the cursor at a tree\nand press {BUILD_PLACE}. Your builder will run out of\nthe tank and harvest one tree for building materials."},
    {1840, "After farming some tree, press {BUILD_TOOL} to select\nthe road/bridge tool (the second button from the top),\naim the build cursor at the river at the end of the\nroad and press {BUILD_PLACE}.\n\nYour builder will run out of the tank and build one\nsegment of bridge. Place again to complete the bridge,\nthen pull {BUILD_MODE} to leave build mode and drive\nacross to the other side."},
    {1841, "To cross this river you will have to build a boat. Boats\ntake five trees, so aim at five trees and press\n{BUILD_PLACE} on each to farm them. Then press\n{BUILD_TOOL} to select the wall/boat tool (the third\nbutton from the top) and place it on the water.\n\nIf you place on land you will build a wall instead, and\nyou will have to farm more trees to build a boat. Drive\nonto the boat and cross the river to the other side."},
    {1842, "You are in control of a tank, which is currently on a\nboat at sea. Push the left stick forward to make the\ntank (and boat) go forwards, and pull it back to make\nit slow down and stop.\n\nThe further you push the left stick, the faster the\ntank goes. Push the left stick forward now to drive\nthe boat towards the land.\n(Press {DISMISS} first to dismiss the window)"},
    {1843, "Your tank was destroyed - but you don't have to start\nthe tutorial over. You've been given a fresh tank just\nacross the water, where you came ashore. Get back into\nthe fight and finish your escape."},
    {1272, "You have been kicked from the server."},
    {1273, "Choose Map"},
    {1274, "Server Maps"},
    {1275, "Local Maps"},
    {1276, "Upload"},
    {1277, "Generate"},
    {1278, "Winbolo.net Maps"},
    {1279, "You've previewed a different map."},
    {1280, "What would you like to do?"},
    {1281, "Use This Map"},
    {1282, "Revert and Close"},
    {1283, "Keep picking"},
    {1284, "Cancel"},
    {1285, "Another upload is in flight."},
    {1286, "Uploads disabled on this server."},
    {1287, "Server map library is full."},
    {1288, "Too many requests \xe2\x80\x94 please wait a moment before retrying."},
    {1289, "Upload rejected by server."},
    {1290, "Download failed"},
    {1291, "Bad response from WinBolo.net"},
    {1292, "Network error fetching map"},
    {1293, "WBN returned HTTP {number}"},
    {1294, "Map download failed"},
    {1295, "Map exceeds 64KB upload cap"},
    {1296, "Out of memory queuing upload"},
    {1297, "Bad search response from WinBolo.net"},
    {1298, "Now"},
    {1299, "During game"},
    {1300, "Accept new join requests right now while the lobby is open."},
    {1301, "Keep accepting new players after the game has started."},
    {1302, "Ranked games lock new players out once the game starts."},
    {1303, "Ranked game"},
    {1304, "Locked by the server admin."},
    {1305, "Only the host or an admin can toggle Ranked game."},
    {1306, "Remove every bot from the lobby before flagging\nthe game as Ranked. Ranked matches are humans-only."},
    {1307, "Ranked games forbid bots and force the tournament\ngame type — no \"Open\" pre-armed mode. Toggling\nthis resets every player's ready state so the host\ncan confirm the new configuration."},
    {1308, "Team {number}"},
    {1309, "· 1 player"},
    {1310, "· {number} players"},
    {1311, "1 bot"},
    {1312, "bots"},
    {1313, "Join"},
    {1314, "Move yourself to Team {number}."},
    {1315, "Bot Naming:"},
    {1316, "Add Bot"},
    {1317, "Add a bot to Team {number}, named from the selected pool."},
    {1318, "Remove Team {number} (and its bots)."},
    {1319, "Drag onto a team to move"},
    {1320, "HOST"},
    {1321, "ADMIN"},
    {1322, "BOT"},
    {1323, "READY"},
    {1324, "NOT READY"},
    {1325, "Configure bot"},
    {1326, "Remove bot"},
    {1327, "Kick player"},
    {1328, "Add Team"},
    {1329, "Unassigned ({number}):"},
    {1330, "Kick \"{player}\"?"},
    {1331, "Name (override)"},
    {1332, "Reroll"},
    {1333, "Pick a fresh random name from the team's pool."},
    {1334, "Codebase"},
    {1335, "(none)"},
    {1336, "Difficulty"},
    {1337, "Easy"},
    {1338, "Normal"},
    {1339, "Hard"},
    {1340, "Personality"},
    {1341, "Aggressive"},
    {1342, "Defensive"},
    {1343, "Sniper"},
    {1344, "Done"},
    /* 2172: 2164 went to STR_DLGPLAYERS_SELECT on main — see lang.h. */
    {2172, "Medium"},

    /* Bot difficulty — tagline then description, per difficulty. The
       lobby colours everything up to the first full stop by difficulty,
       so start the tagline with the difficulty word in your language and
       end that word with a full stop. The words themselves are free. */
    {2165, "Easy. Captures pillboxes and bases, coordinates with allied bots, and plays in a steady, predictable way."},
    {2166, "Medium. Captures pillboxes and bases with its allied bots and presses an attack when it is already ahead."},
    {2167, "Hard. Aggressively captures pillboxes and bases and teams up with allied bots to overwhelm targets together."},
    {2168, "This bot captures enemy pillboxes and bases, defends its own, and shares its plans with allied bots so they avoid chasing the same target, though it plays cautiously and steers clear of big risks. Its steady, easy-to-read behaviour makes it a gentler challenge. Best for newer players, or anyone who wants a more relaxed game."},
    {2169, "This bot captures enemy pillboxes and bases, defends its own, and shares its plans with allied bots so they can strike a target together. It will join a group attack and press an advantage, but it picks its fights, keeps itself fuelled, and gives ground when the odds turn against it. Best for players who know the game and want a real opponent rather than a relentless one."},
    {2170, "This bot hunts down enemy pillboxes and bases, coordinating with its allied bots to strike key targets together while keeping its own pillboxes alive and refuelling when it runs low. It plays assertively, organising group assaults, committing to its attacks, and even rebuilding fallen pillboxes under fire when its team has the upper hand. Best for players who want a challenging, relentless opponent."},

    /* Bot AiConfig: the Mode dropdown's label. The mode names themselves
       come from the brain's own modes.txt, so they are data, not strings. */
    {2171, "Mode"},

    /* Title of the docs dialog a bot's announce line in team chat opens.
       {string1} is the brain's name, so "GoalHunter commands". */
    {2214, "{string1} commands"},
    /* A general Copy button: the docs dialog puts its whole body on the
       clipboard with it. */
    {2215, "Copy"},
    /* Balance from WBN */
    {1345, "Balance from WBN"},
    {1346, "Balance teams from WBN"},
    {1347, "This will clear every team in the lobby and replace them with two skill-balanced teams from winbolo.net. The new split takes effect immediately."},
    {1348, "There is currently 1 bot in the lobby. Choose whether the bot should take part in the balanced split or be removed for a humans-only matchup."},
    {1349, "There are currently {number} bots in the lobby. Choose whether the bots should take part in the balanced split or be removed for a humans-only matchup."},
    {1350, "Bots included"},
    {1351, "Humans only"},
    {1352, "Balance"},
    {1353, "Only the host or an admin can request a skill-\nbalanced team split from winbolo.net."},
    {1354, "At least two connected players are needed before\nthe lobby can be split into balanced teams."},
    {1355, "Ask winbolo.net to split the lobby into two skill-\nbalanced teams. The split is applied immediately\nonce WBN responds — existing teams are replaced."},
    {1356, "Asking WBN\xe2\x80\xa6"},
    {1357, "Teams balanced"},
    {1358, "No response from WBN"},
    /* Reject toast */
    {1359, "rejected"},
    {1360, "host-only action"},
    {1361, "setting locked by server"},
    {1362, "invalid request"},
    {1363, "\xe2\x9a\xa0 Lobby change rejected: {string1}"},
    /* Lock badge fallback */
    {1364, "[locked]"},
    /* Ranked shape tooltip */
    {1365, "Ranked games require exactly two teams with equal\nsizes: 1v1, 2v2, or 3v3 human players.\n(Currently {number} {string1}, sizes {number2} vs {number3}.)"},
    {1366, "team"},
    {1367, "teams"},
    {1368, "Game settings"},
    {1369, "The host has allowed all players to change game settings."},
    {1370, "Game Type"},
    {1371, "Computer Players"},
    {1372, "No computer tanks"},
    {1373, "Allow computer tanks"},
    {1374, "Allow with advantage"},
    {1375, "Allow with full advantage"},
    {1376, "Other"},
    {1377, "Allow all players to change settings"},
    {1378, "When on, every connected player can edit lobby\nsettings — including changing the map, adding\nor removing bots, and switching teams."},
    {1379, "min"},
    {1380, "Password"},
    {1381, "When on, new clients must supply the\npassword to join. Already-connected players\nare unaffected."},
    {1382, "Single player"},
    {1383, "Internet"},
    {1384, "Test in progress"},
    /* Lobby visibility block */
    {1994, "Visibility"},
    {1995, "Pill View"},
    {1996, "Base View"},
    {1997, "Allied Tank View"},
    {1998, "Always"},
    {1999, "Key"},
    {2000, "Decay"},
    {2001, "Off"},
    {2002, "secs"},
    {2008, "Classic mode"},
    {2009, "- Pill View goes to Key; Base View and Allied Tank View go to Off.\n- Allies in trees goes off, and so does line of sight.\n- The overview window goes to None, so there is no Map Overview and no Full Screen map.\n- All of those are held there while classic mode is on.\n- Turning it off leaves those values where it put them."},
    {2010, "The server has the map overview turned off."},
    {2011, "See allies in trees"},
    {2012, "Allied tanks won't be hidden by trees."},
    {2154, "Overview window"},
    {2155, "Classic: a map overview window option, or full-screen (a playable map overview).\nEven in overview/full screen, tanks have to scroll to see around them.\n\nExpanded: Like \"Classic with overview\" but able to see all 29x29 tiles centered on your tank"},
    {2156, "Line of sight"},
    {2157, "- Buildings hide the ground behind them.\n- Two squares of trees in a row hide what is behind them.\n- Trees within two squares of you never hide anything.\n- Off is the classic rule, and classic mode forces it off."},
    {2158, "Expanded"},
    {2159, "Classic"},
    {2173, "Classic"},
    {2174, "Standard bolo HUD view only.\nNo map overview, no full screen view, no extra bases/ally views."},
    {2175, "Classic with overview"},
    {2176, "Like classic with a map overview window option, or full-screen (a playable map overview).\nEven in overview/full screen, tanks have to scroll to see around them."},
    {2177, "Expanded"},
    {2178, "Like \"Classic with overview\" but able to see all 29x29 tiles centered on your tank.\nScrolling moves the entire map, and expanded with an always visible pill view."},
    {2179, "Max view"},
    {2180, "Like expanded, with additional visibility provided by bases and allies."},
    {2181, "Line of sight"},
    {2182, "Like max view, but only what your tank can actually see:\nBuildings, and two squares of trees in a row, hide what is behind them."},
    {2183, "Custom"},
    {2184, "Your own set. Changing anything here picks this row."},
    {2185, "Details"},
    {2186, "- Opens the visibility table.\n- Every preset with what it lets you see.\n- The last row is every setting on its own."},
    {2187, "- The server has locked one or more visibility settings.\n- The presets are unavailable while that lock is on."},
    {2188, "Pills"},
    {2189, "Bases"},
    {2190, "Allies"},
    {2191, "Overview"},
    {2192, "- Always: on the map overview all the time.\n- Key: only while you are watching that one through its view key.\n- Decay: for a while after one of your tanks was last near it.\n- Off: never."},
    {2193, "None"},
    {2194, "This server does not report its visibility settings."},
    {2195, "Expanded (full 29x29 view around tank)"},
    /* Lobby "Other" column — the host switch for smart pings */
    {2203, "Allow smart pings"},
    /* Settings > Display & Sound > Full Screen */
    {2013, "Full Screen"},
    {2014, "Newswire transparency"},
    {2015, "How much of the map shows through the newswire along\nthe bottom of the full screen view. 0% is solid, and\nthe slider stops at 90% so it cannot be made invisible."},
    {2016, "Auto-hide the newswire"},
    {2017, "Drop the newswire off the bottom of the screen after\nno new messages have been received for a while, and\nslide it back up on the next message. Off keeps it on\nscreen the whole game."},
    {2018, "Builder tools transparency"},
    {2019, "How much of the map shows through the builder tools\ndown the left of the full screen view. 0% is solid,\nand the slider stops at 90% so they cannot be made\ninvisible."},
    {2020, "Status panel transparency"},
    {2021, "How much of the map shows through the status panel\ndown the right of the full screen view - the LGM,\nkills and deaths, the base, pillbox and tank rows,\nand the stock bars. 0% is solid, and the slider stops\nat 90% so it cannot be made invisible."},
    {2022, "Fill the whole screen instead of running in a window.\nRemembered for next time."},

    /* Item view caption — the corner label and the full screen map's
     * caption both come through sdl3DrawGetItemViewLabel. */
    {2023, "Pillbox View"},
    {2024, "Base View"},
    {2025, "Allied Tank View"},
    {2026, "Allied Tank View \xE2\x80\x94 {player}"},

    /* Server browser visibility tag. Short by design: the tag packs the
     * whole rule set onto one line of the detail pane. */
    {2027, "Views:"},
    {2028, "Classic"},
    {2029, "Allies in trees"},

    /* Lobby team start side: the header's Start: selector, the row cell's
     * sea state and the start dropdown's two actions. 2113: {string1} =
     * side name, {number} = starts the side offers, {number2} = team
     * members, {number3} = members left to start at sea. 2115: {string1}
     * = side name. */
    {2102, "Start:"},
    {2103, "Any"},
    {2104, "North"},
    {2105, "East"},
    {2106, "South"},
    {2107, "West"},
    {2108, "Sea"},
    {2109, "Team side"},
    {2110, "Auto"},
    {2111, "Unassign"},
    {2112, "(off-side)"},
    {2113, "{string1}: {number} starts for {number2} players \xE2\x80\x94 {number3} will start at sea.\nChanging a side re-picks every player's start."},
    {2114, "Every start this team can use is taken, so this player\nwill start at sea beside a teammate's start."},
    {2115, "Off-side: this start is not on {string1}, the team's side.\nThe host chose it for this player."},

    /* Map-preview compass, shown to the host when exactly two teams have
     * members. Hovering an axis of the rose offers to put both teams on
     * it, or, when they are already on it, to clear both back to Any.
     * {string1} = the axis, "N/S" or "E/W"; {player} / {other} = the two
     * team names; {string2} / {string3} = the side each would take. */
    {2124, "Set the two teams to {string1}\n{player} starts {string2}, {other} starts {string3}"},
    {2125, "Teams set to {string1}. Click to go back to custom starts."},
    /* Map-preview start-picker tooltips over a start the viewer's own
     * team side rejects: 2116 for a player who cannot take it, 2117 for
     * the host, who still can. {number} = start; 2117: {string1} = the
     * host's team. */
    {2116, "Start #{number}\nNot on your team's side"},
    {2117, "Start #{number}\nOff-side for {string1}"},

    /* Map-preview start-picker tooltips over a start somebody else holds
     * that the viewer may join (LOBBY_SHARED_STARTS): 2127 for a player,
     * 2128 for the host, who can still drag and assign. {number} = start;
     * {player} = the holder, or every holder comma-joined. */
    {2127, "Start #{number} - {player}\nClick to start here too"},
    {2128, "Start #{number} - {player}\nClick to start here too; drag to move; right-click to assign to someone else"},
    {1388, "Choose Map"},

    /* Pre-flight version-mismatch error (client-side, surfaced by the
     * direct-connect / rejoin entry). */
    {1389, "Server is version {string1}, you have {string2} \xE2\x80\x94 please update."},

    /* In-game votes — back-to-lobby and surrender. */
    {1390, "Vote: Return to lobby"},
    {1391, "Vote: Surrender"},
    {1392, " (Draw)"},
    {1393, "Available once the game is running."},
    {1394, "Pick a team before voting to surrender."},
    {1395, "Surrender is only available when exactly two teams\nwith human players remain."},

    {1396, "{player} started a vote to return to lobby."},
    {1397, "{player} started a vote to surrender."},
    {1398, "{player} voted yes."},
    {1399, "{player} voted no."},
    {1400, "Vote passed."},
    {1401, "Vote failed."},

    {1402, "Volume"},
    {1403, "Mute"},
    {1404, "Returning to lobby"},

    /* Reject toast — BAD_STATE generic reason */
    {1405, "not allowed right now"},
    /* Balance status pill — WBN failure (server-side) */
    {1406, "WBN balance failed"},
    /* Net Info — inbound snapshot loss for the last 1-second window */
    {1407, "Packet loss: {number}% ({number2}/{number3} this sec, {number4} game total)"},

    /* WinBolo.net 1v1 ladder position in the account status block */
    {1408, "#{number} of {number2}"},
    {1409, "Unranked"},

    /* My Stats dialog — per-mode WinBolo.net play stats */
    {1410, "My Stats"},
    {1411, "WinBolo.net Stats"},
    {1412, "No stats available yet."},
    {1413, "Open"},
    {1414, "Tournament"},
    {1415, "Strict"},
    {1416, "Games:"},
    {1417, "Bases:"},
    {1418, "Pillboxes:"},
    {1419, "Tanks:"},
    {1420, "Score (ELO):"},
    {1421, "Wins:"},
    {1422, "Losses:"},
    {1423, "Rank:"},

    /* First-run online onboarding wizard */
    {1424, "Welcome to WinBolo Online"},
    {1425, "Account"},
    {1426, "Sign in to your WinBolo.net account, or continue without one."},
    {1427, "Player Name"},
    {1428, "Choose the name other players will see."},
    {1429, "Keys"},
    {1430, "Set up your controls. You can change these later in Settings."},
    {1431, "Next"},
    {1432, "Skip"},
    {1433, "Finish"},

    /* WinBolo.net browser signup link */
    {1434, "No account? Create one"},

    /* WinBolo.net Steam-auth status suffix */
    {1435, "via Steam"},

    /* In-client Steam signup form (login popup) */
    {1436, "Create a WinBolo.net account"},
    {1437, "optional"},
    {1438, "Create account with Steam"},
    {1439, "That username is already taken."},
    {1440, "That email address is already registered."},
    {1441, "This Steam account already has a WinBolo.net account. Use Sign in with Steam."},
    {1442, "Too many sign-up attempts. Please try again later."},
    {1443, "That username can't be used. Please choose another."},
    {1444, "That username is too long."},
    {1445, "Please enter a username."},
    {1446, "That email address isn't valid."},
    {1447, "Could not create your account. Please try again."},

    /* Column headers for the My Stats table */
    {1448, "Rank"},
    {1449, "Rating"},
    {1450, "Games"},
    {1451, "W/L"},
    {1452, "Bases"},
    {1453, "Pills"},
    {1454, "Tanks"},

    /* In-client Steam sign-in (existing account) + popup section labels */
    {1455, "Sign in with Steam"},
    {1456, "or sign in with a password"},
    {1457, "Have a password account? Link your Steam account at [winbolo.net](https://www.winbolo.net/playermodify)."},
    {1458, "No WinBolo.net account is linked to this Steam yet. Create one below."},
    {1459, "Create a standalone account in your browser"},
    {1460, "Email:"},
    {1461, "or"},
    {1462, "Steam couldn't verify your account. Please try again."},
    {1463, "Sign into WinBolo.net"},
    {1464, "Make host"},
    {1465, "Make \"{player}\" the host?"},
    {1466, "{player} is now the host"},

    /* BEGIN generated country names — tools/gen_countries.py; do not hand-edit. */
    {1532, "Andorra"},
    {1533, "United Arab Emirates"},
    {1534, "Afghanistan"},
    {1535, "Antigua and Barbuda"},
    {1536, "Anguilla"},
    {1537, "Albania"},
    {1538, "Armenia"},
    {1539, "Angola"},
    {1540, "Antarctica"},
    {1541, "Argentina"},
    {1542, "American Samoa"},
    {1543, "Austria"},
    {1544, "Australia"},
    {1545, "Aruba"},
    {1546, "Aland Islands"},
    {1547, "Azerbaijan"},
    {1548, "Bosnia and Herzegovina"},
    {1549, "Barbados"},
    {1550, "Bangladesh"},
    {1551, "Belgium"},
    {1552, "Burkina Faso"},
    {1553, "Bulgaria"},
    {1554, "Bahrain"},
    {1555, "Burundi"},
    {1556, "Benin"},
    {1557, "Saint Barthélemy"},
    {1558, "Bermuda"},
    {1559, "Brunei Darussalam"},
    {1560, "Bolivia"},
    {1561, "Bonaire, Sint Eustatius and Saba"},
    {1562, "Brazil"},
    {1563, "Bahamas"},
    {1564, "Bhutan"},
    {1565, "Bouvet Island"},
    {1566, "Botswana"},
    {1567, "Belarus"},
    {1568, "Belize"},
    {1569, "Canada"},
    {1570, "Cocos (Keeling) Islands"},
    {1571, "Democratic Republic of the Congo"},
    {1572, "Central African Republic"},
    {1573, "Republic of the Congo"},
    {1574, "Switzerland"},
    {1575, "Côte d'Ivoire"},
    {1576, "Cook Islands"},
    {1577, "Chile"},
    {1578, "Cameroon"},
    {1579, "China"},
    {1580, "Colombia"},
    {1581, "Clipperton Island"},
    {1582, "Costa Rica"},
    {1583, "Cuba"},
    {1584, "Cabo Verde"},
    {1585, "Curaçao"},
    {1586, "Christmas Island"},
    {1587, "Cyprus"},
    {1588, "Czech Republic"},
    {1589, "Germany"},
    {1590, "Diego Garcia"},
    {1591, "Djibouti"},
    {1592, "Denmark"},
    {1593, "Dominica"},
    {1594, "Dominican Republic"},
    {1595, "Algeria"},
    {1596, "Ecuador"},
    {1597, "Estonia"},
    {1598, "Egypt"},
    {1599, "Western Sahara"},
    {1600, "Eritrea"},
    {1601, "Spain"},
    {1602, "Ethiopia"},
    {1603, "Europe"},
    {1604, "Finland"},
    {1605, "Fiji"},
    {1606, "Falkland Islands"},
    {1607, "Federated States of Micronesia"},
    {1608, "Faroe Islands"},
    {1609, "France"},
    {1610, "Gabon"},
    {1611, "United Kingdom"},
    {1612, "Grenada"},
    {1613, "Georgia"},
    {1614, "French Guiana"},
    {1615, "Guernsey"},
    {1616, "Ghana"},
    {1617, "Gibraltar"},
    {1618, "Greenland"},
    {1619, "Gambia"},
    {1620, "Guinea"},
    {1621, "Guadeloupe"},
    {1622, "Equatorial Guinea"},
    {1623, "Greece"},
    {1624, "South Georgia and the South Sandwich Islands"},
    {1625, "Guatemala"},
    {1626, "Guam"},
    {1627, "Guinea-Bissau"},
    {1628, "Guyana"},
    {1629, "Hong Kong"},
    {1630, "Heard Island and McDonald Islands"},
    {1631, "Honduras"},
    {1632, "Croatia"},
    {1633, "Haiti"},
    {1634, "Hungary"},
    {1635, "Canary Islands"},
    {1636, "Indonesia"},
    {1637, "Ireland"},
    {1638, "Israel"},
    {1639, "Isle of Man"},
    {1640, "India"},
    {1641, "British Indian Ocean Territory"},
    {1642, "Iraq"},
    {1643, "Iran"},
    {1644, "Iceland"},
    {1645, "Italy"},
    {1646, "Jersey"},
    {1647, "Jamaica"},
    {1648, "Jordan"},
    {1649, "Japan"},
    {1650, "Kenya"},
    {1651, "Kyrgyzstan"},
    {1652, "Cambodia"},
    {1653, "Kiribati"},
    {1654, "Comoros"},
    {1655, "Saint Kitts and Nevis"},
    {1656, "North Korea"},
    {1657, "South Korea"},
    {1658, "Kuwait"},
    {1659, "Cayman Islands"},
    {1660, "Kazakhstan"},
    {1661, "Laos"},
    {1662, "Lebanon"},
    {1663, "Saint Lucia"},
    {1664, "Liechtenstein"},
    {1665, "Sri Lanka"},
    {1666, "Liberia"},
    {1667, "Lesotho"},
    {1668, "Lithuania"},
    {1669, "Luxembourg"},
    {1670, "Latvia"},
    {1671, "Libya"},
    {1672, "Morocco"},
    {1673, "Monaco"},
    {1674, "Moldova"},
    {1675, "Montenegro"},
    {1676, "Saint Martin"},
    {1677, "Madagascar"},
    {1678, "Marshall Islands"},
    {1679, "North Macedonia"},
    {1680, "Mali"},
    {1681, "Myanmar"},
    {1682, "Mongolia"},
    {1683, "Macau"},
    {1684, "Northern Mariana Islands"},
    {1685, "Martinique"},
    {1686, "Mauritania"},
    {1687, "Montserrat"},
    {1688, "Malta"},
    {1689, "Mauritius"},
    {1690, "Maldives"},
    {1691, "Malawi"},
    {1692, "Mexico"},
    {1693, "Malaysia"},
    {1694, "Mozambique"},
    {1695, "Namibia"},
    {1696, "New Caledonia"},
    {1697, "Niger"},
    {1698, "Norfolk Island"},
    {1699, "Nigeria"},
    {1700, "Nicaragua"},
    {1701, "Netherlands"},
    {1702, "Norway"},
    {1703, "Nepal"},
    {1704, "Nauru"},
    {1705, "Niue"},
    {1706, "New Zealand"},
    {1707, "Oman"},
    {1708, "Panama"},
    {1709, "Pacific Community"},
    {1710, "Peru"},
    {1711, "French Polynesia"},
    {1712, "Papua New Guinea"},
    {1713, "Philippines"},
    {1714, "Pakistan"},
    {1715, "Poland"},
    {1716, "Saint Pierre and Miquelon"},
    {1717, "Pitcairn"},
    {1718, "Puerto Rico"},
    {1719, "State of Palestine"},
    {1720, "Portugal"},
    {1721, "Palau"},
    {1722, "Paraguay"},
    {1723, "Qatar"},
    {1724, "Réunion"},
    {1725, "Romania"},
    {1726, "Serbia"},
    {1727, "Russia"},
    {1728, "Rwanda"},
    {1729, "Saudi Arabia"},
    {1730, "Solomon Islands"},
    {1731, "Seychelles"},
    {1732, "Sudan"},
    {1733, "Sweden"},
    {1734, "Singapore"},
    {1735, "Saint Helena, Ascension and Tristan da Cunha"},
    {1736, "Slovenia"},
    {1737, "Svalbard and Jan Mayen"},
    {1738, "Slovakia"},
    {1739, "Sierra Leone"},
    {1740, "San Marino"},
    {1741, "Senegal"},
    {1742, "Somalia"},
    {1743, "Suriname"},
    {1744, "South Sudan"},
    {1745, "Sao Tome and Principe"},
    {1746, "El Salvador"},
    {1747, "Sint Maarten"},
    {1748, "Syria"},
    {1749, "Eswatini"},
    {1750, "Turks and Caicos Islands"},
    {1751, "Chad"},
    {1752, "French Southern Territories"},
    {1753, "Togo"},
    {1754, "Thailand"},
    {1755, "Tajikistan"},
    {1756, "Tokelau"},
    {1757, "Timor-Leste"},
    {1758, "Turkmenistan"},
    {1759, "Tunisia"},
    {1760, "Tonga"},
    {1761, "Türkiye"},
    {1762, "Trinidad and Tobago"},
    {1763, "Tuvalu"},
    {1764, "Taiwan"},
    {1765, "Tanzania"},
    {1766, "Ukraine"},
    {1767, "Uganda"},
    {1768, "United States Minor Outlying Islands"},
    {1769, "United Nations"},
    {1770, "United States of America"},
    {1771, "Uruguay"},
    {1772, "Uzbekistan"},
    {1773, "Holy See"},
    {1774, "Saint Vincent and the Grenadines"},
    {1775, "Venezuela"},
    {1776, "Virgin Islands (British)"},
    {1777, "Virgin Islands (U.S.)"},
    {1778, "Vietnam"},
    {1779, "Vanuatu"},
    {1780, "Wallis and Futuna"},
    {1781, "Samoa"},
    {1782, "Kosovo"},
    {1783, "Unknown"},
    {1784, "Yemen"},
    {1785, "Mayotte"},
    {1786, "South Africa"},
    {1787, "Zambia"},
    {1788, "Zimbabwe"},
/* END generated country names */

    /* About modal — credits & links */
    {1789, "Additional 2.0 programming by Andrew Roth"},
    {1790, "Third Party Notices"},
    {1791, "Authors"},
    {1792, "Forums"},
    {1793, "In game"},
    {1794, "Ver"},
    {1795, "+AI"},
    {1796, "+Mines"},
    {1797, "No new players"},
    {1798, "Password required"},
    {1799, "Auto-locks on game start"},
    {1800, "Spectate"},
    {1801, "Auto-refresh"},
    {1802, "Lobby"},
    {1803, "In Game"},
    {1814, "Select a server"},
    {1815, "Preview unavailable"},
    {1816, "Click to enlarge"},
    {1817, "({number} AI players)"},
    {1818, "WinBolo.net players:"},
    {1819, "Locked/Full"},
    {1820, "No response"},
    {1821, "rnd"},
    {1822, "Tourn"},
    {1804, "Controller Disconnected"},
    {1805, "Your gamepad has been disconnected. You can reconnect it or switch to keyboard controls."},
    {1806, "Continue with keyboard and mouse"},
    {1807, "Respond in the Players menu"},
    {1808, "Respond in the Players menu"},
    {1809, "Select   B Backspace   LB Shift   RB ?123   Start Done"},
    {1810, "Bksp"},
    {1811, "Shift"},
    {1812, "Space"},
    {1813, "Enter"},
    {1823, "UI Scale"},
    {1824, "Auto"},
    {1825, "Small"},
    {1826, "Medium"},
    {1827, "Large"},
    {1828, "Alliances are disabled in ranked games."},
    {1829, "Enlarge"},
    {1844, "Display & Sound"},
    {1845, "Game/HUD"},
    {1846, "Session"},
    {1906, "Hosting"},
    {1907, "Port"},
    {1908, "Allow Spectators"},
    {1909, "Max Spectators"},
    {1910, "Map Uploads"},
    {1911, "Off"},
    {1912, "Allow"},
    {1913, "Persist (save to disk)"},
    {1914, "Upload Directory"},
    {1915, "Max Files"},
    {1916, "Max Storage (MB)"},
    {1917, "Enable Logging"},
    {1919, "Log Directory"},
    {1920, "Applies to the next hosted / New Internet game."},
    {1937, "Send Replays to Players"},
    /* Voice row of the Hosting tab: what the hosted server does with the
     * voice its clients send it. 2123 is the tooltip on the combo. */
    {2119, "Voice Chat"},
    {2120, "On"},
    {2121, "Off"},
    {2122, "Proximity"},
    {2123, "Off drops voice instead of carrying it to anyone. Proximity is not implemented yet and behaves the same as On."},
    /* Shown to a connected client whose server was started with voice off,
     * where the controls below it reach nothing. */
    {2126, "This server has voice turned off"},
    {1847, "Currently: {string1}"},
    {1848, "Spectators ({number}):"},
    {1866, "[Spectator] {string1}"},
    {1849, "Leave spectating?"},
    {1850, "Lobby"},
    {1851, "Live"},
    {1852, "Game Over"},
    {1853, "WinBolo - Spectating {string1}:{number} Map: {string2}{string3}"},
    {1854, "{player} joined as spectator"},
    {1855, "{player} left spectating"},
    {1856, "[spectator] {number}: {string1}"},
    {1857, "Spectating begins in {number}"},
    {1858, "Connecting..."},
    {1859, "Connection lost"},
    {1860, "Spectating begins now"},
    {1861, "WinBolo - Spectating {string1}:{number}{string3}"},

    /* Menu item to re-open a closed in-game vote widget. */
    {1862, "Show: {string1}"},

    /* Web client — join code / connect error dialogs */
    {1867, "You must be signed in to WinBolo.net to join this game."},
    {1868, "This game is not accepting new players."},
    {1869, "That game link is no longer valid."},
    {1870, "This game is full."},
    {1871, "Could not reach the server to get a join code."},
    {1872, "Could not connect to the server."},
    {1873, "Could not join the game (the server did not respond, or your invite link has already been used or expired)."},

    /* soundSetup: none of the sound effects could be loaded. */
    {2118, "Error loading sound effects"},

    /* Log viewer Options item: present the round on a game-relative clock. */
    {1921, "Hide Lobby"},

    /* Skin section of the Display & Sound settings tab */
    {1948, "Skin"},
    {1949, "Default"},
    {1950, "Built-in"},
    {1951, "User"},
    {1952, "Workshop"},
    {1953, "Downloading..."},
    {1954, "Open skins folder"},
    {1955, "Browse Workshop"},
    {1956, "Publish to Workshop..."},
    {1957, "Tile detail"},
    {1958, "Classic"},
    {1959, "Match to zoom"},
    {1960, "High detail"},
    {1961, "Animation smoothness"},
    {1962, "Classic"},
    {1963, "Match pixelation"},
    {1964, "Smooth"},
    {1965, "Smooth shells"},
    {1966, "Texture filter"},
    {1967, "Nearest"},
    {1968, "Linear"},
    {1969, "Pixel art"},
    {1970, "This skin supplies its art at one size only, so all three settings build the same tiles."},
    {1971, "Shows most on rotated sprites and on smooth sub-pixel motion. Textures drawn by the interface ignore it, which is why the tiles above do not change."},
    {1972, "Publish this skin to the Steam Workshop"},
    {1973, "Title"},
    {1974, "Description"},
    {1975, "Update the item this skin came from"},
    {1976, "Publish as a new item"},
    {1977, "Publish"},
    {1978, "Uploading..."},
    {1979, "Published to the Workshop."},
    {1980, "The publish failed. Check that Steam is running, then try again."},
    {1981, "Accept the Workshop legal agreement on the item's page, or nobody else can see the item."},
    {1982, "Open item page"},
    {1983, "Only a skin in your own skins folder can be published. The built-in art, Workshop skins and skins that did not load cannot be."},
    {1984, "This skin has finer art for only some sprites, so Match to zoom builds the same tiles as Classic. High detail uses the finer art where the skin has it."},
    {1985, "Recommended filter:"},
    {1986, "(recommended)"},

    /* Voice section of the Display/Sound settings tab */
    {2030, "Voice"},
    {2031, "Test microphone"},
    {2032, "Mic gain"},
    {2033, "Input level"},
    {2034, "Enable voice"},
    {2035, "Mode"},
    {2036, "Off"},
    {2037, "Push to talk"},
    {2038, "Open mic"},
    {2039, "Push-to-talk key"},
    {2040, "Transmitting"},
    {2041, "Not transmitting"},
    {2042, "Voice volume"},

    /* Key setup — push to talk binding */
    {2043, "Push to talk"},

    /* Players panel — microphone state icon */
    {2044, "Talking — click to mute"},
    {2045, "Has a microphone — click to mute"},
    {2046, "Their microphone is off — click to mute"},
    {2047, "No microphone — click to mute"},
    {2048, "Muted by you — click to unmute"},
    {2049, "Your microphone — click to mute"},
    {2050, "You have no microphone"},
    {2051, "Your microphone is off — click to unmute"},

    /* Voice settings — tank-label microphone icons */
    {2052, "Show microphone icons over tanks"},

    /* Voice settings — acoustic echo cancellation */
    {2053, "Echo cancellation"},

    /* Voice settings — microphone test progress */
    {2054, "Recording..."},
    {2055, "Playing back..."},

    /* Voice settings — echo canceller could not be created */
    {2056, "Unavailable on this system"},

    /* Voice settings — the platform cancels echo itself, so Speex does not */
    {2057, "Handled by the system"},

    /* Players menu — open the players panel */
    {2058, "Players Panel"},

    /* Key setup — self-mute binding */
    {2059, "Mute microphone"},

    /* Voice settings — which microphone and which speakers voice uses */
    {2060, "Microphone"},
    {2061, "Playback device"},
    {2062, "System default"},

    /* Players panel — per-player playback volume slider */
    {2090, "Voice chat volume"},

    /* Players panel — per-player smart-ping mute toggle */
    {2162, "Pings shown — click to hide this player's pings"},
    {2163, "Pings hidden — click to show this player's pings"},

    /* Players panel — label in front of the selection buttons */
    {2164, "Select:"},

    /* Lobby — the local player's voice sub-row */
    {2095, "Your microphone and voice settings"},
    {2096, "Voice is turned off"},

    /* Lobby chat — said once when somebody else is heard and voice is off here */
    {2153, "Someone is using voice chat. Yours is off — turn it on with the cog beside your name."},

    /* Players panel — a player muted here who is talking anyway */
    {2097, "Muted by you, and talking — click to unmute"},

    /* Sound settings — the three volumes, of which "Voice volume" is 2042 */
    {2100, "Master volume"},
    {2101, "Effects volume"},

    /* Smart ping: newswire lines, slice names, and the key-setup chord
       vocabulary. */
    {2129, "{player}: Ping!"},
    {2130, "{player}: Caution!"},
    {2131, "{player}: Assist me!"},
    {2132, "{player}: Attack!"},
    {2133, "{player}: On my way!"},
    {2134, "{player}: Bot command"},
    {2135, "Ping"},
    {2136, "Caution"},
    {2137, "Assist Me"},
    {2138, "Attack"},
    {2139, "On My Way"},
    {2140, "Bot Command"},
    {2141, "Smart Ping"},
    {2142, "Smart Ping Alternate Keys"},
    {2143, "Press a key or mouse button"},
    {2144, "Ctrl"},
    {2145, "Alt"},
    {2146, "Shift"},
    {2147, "Left Mouse"},
    {2148, "Middle Mouse"},
    {2149, "Right Mouse"},
    {2150, "Mouse 4"},
    {2151, "Mouse 5"},
    {2152, "Smart Ping Alternate Keys 2"},
    {2196, "Scripted"},
    {2197, "Run map scripts when hosting"},
    {2198, "Scenario:"},
    {2199, "Scripts off"},
    {2200, "Reload script"},
    {2201, "That setting is fixed by the map's scenario"},
    {2202, "Too soon; try again in a moment"},
    {2204, "Choose"},
    {2205, "Choose a mod"},
    {2206, "None — no mod. The map plays its own scenario if it has one."},
    {2207, "Asking the server..."},
    {2208, "This server offers no mods."},
    {2209, "Built for one map, so it cannot be played as a mod"},
    {2210, "Up to {number} players"},
    {2211, "{number} bot seats"},
    {2212, "Mod: {string2} (on {string1})"},
    {2213, "Run scripts in uploaded maps"},
    {2216, "Scenario Directory"},
    {2217, "unchanged"},
    {2218, "{string1}x faster"},
    {2219, "{string1}x slower"},
    {2220, "{string1}x as many"},
    {2221, "{string1}x fewer"},
    {2222, "twice as many"},
    {2223, "half as many"},
    {2224, "on"},
    {2225, "off"},
    {2226, "Scenario"},
    {2227, "Save the map first — a script is kept beside its map"},
    {2228, "Save Script"},
    {2229, "Reload"},
    {2230, "unsaved changes"},
    {2231, "Script loaded"},
    {2232, "No script beside this map yet"},
    {2233, "Script saved"},
    {2234, "The script could not be read"},
    {2235, "The script could not be written"},
    {2236, "The script is larger than 1 MB and was not opened"},
    {2237, "The script on disk was not opened, so it will not be overwritten"},
    {2238, "Script"},
    {2239, "Metadata"},
    {2240, "Lobby"},
    {2241, "Rules"},
    {2242, "Name"},
    {2243, "Description"},
    {2244, "Scenario API version"},
    {2245, "Game type"},
    {2246, "None — the round plays strict tournament"},
    {2247, "Built for this map"},
    {2248, "Untick it and the scenario is a mod: it plays over any map and keeps no tags or regions"},
    {2249, "Start pills and bases at the caps these rules leave in force"},
    {2250, "Max players"},
    {2251, "0 leaves the server its own cap"},
    {2252, "A host may add teams beyond these"},
    {2253, "Team"},
    {2254, "Team number"},
    {2255, "Bots"},
    {2256, "Max bots"},
    {2257, "Fielded"},
    {2258, "Brain"},
    {2259, "A brain's name, not a path: the server looks it up among its own brains"},
    {2260, "Init table"},
    {2261, "Key"},
    {2262, "Value"},
    {2263, "Add Pair"},
    {2264, "No room for another pair"},
    {2265, "Add Team"},
    {2266, "Remove Team"},
    {2267, "No room for another team"},
    {2268, "This template seats no teams"},
    {2269, "Remove"},
    {2270, "classic"},
    {2271, "No rules set, so the scenario plays the classic ones"},
    {2272, "Add Rule"},
    {2273, "Filter"},
    {2274, "No room for another rule"},
    {2275, "Validate"},
    {2276, "Issues"},
    {2277, "No problems found"},
    {2278, "Rules and tags are checked against the map when the round starts, not here"},
    {2279, "More problems were found than this list holds"},
    {2280, "Scenario Calls"},
    {2281, "Pack into Map"},
    {2282, "Save as Mod…"},
    {2283, "Save the map first — a packed scenario lives in the map file"},
    {2284, "Packed into the map — a loose script beside it still overrides the packed one when a server loads it"},
    {2285, "Mod saved"},
    {2286, "The script has problems, so nothing was written — the Script view lists them"},
    {2287, "The forms and the script's own table disagree, so nothing was written"},
    {2288, "The scenario was not written"},
    {2289, "Script read from the scenario packed into this map"},
    {2290, "The scenario packed into this map could not be read"},
    {2291, "The map's packed scenario was written back. Pack into Map saves the forms' changes."},
    {2292, "The map saved, but its packed scenario could not be written back"},
};

#define LANG_TABLE_SIZE ((int)(sizeof(langTable) / sizeof(langTable[0])))

static const char *lookupString(unsigned int id) {
    int i;
    for (i = 0; i < LANG_TABLE_SIZE; i++) {
        if (langTable[i].id == id) {
            return langTable[i].text;
        }
    }
    return "";
}

/* -------------------------------------------------------
 * Generated symbolic-name -> langid table.
 * Sorted alphabetically by name; resolved with bsearch().
 * Re-run tools/dump_lang_en.py after editing lang.h or this file.
 * ------------------------------------------------------- */
#include "lang_names.inc"

/* -------------------------------------------------------
 * Runtime override table for non-English translations.
 *
 * overrideTable is a sparse array indexed by langid. NULL means
 * "no override; fall back to the static langTable[]". Strings
 * are heap-allocated (strdup-style) and freed on reload/unload.
 * ------------------------------------------------------- */
static char        **overrideTable      = NULL;
static unsigned int  overrideTableSize  = 0;
static LangFileMeta  loadedMeta         = {{0}, {0}, {0}};
static bool          loadedMetaValid    = FALSE;

static int langNameCmp(const void *a, const void *b) {
    const char          *key   = (const char *)a;
    const LangNameEntry *entry = (const LangNameEntry *)b;
    return strcmp(key, entry->name);
}

static langid resolveName(const char *name) {
    /* Derive the element count from the array itself rather than the
     * generated K_LANG_NAME_TABLE_SIZE macro. When the macro drifted
     * above the real length (a hand-edit of the generated .inc), bsearch
     * read one element past the end of the table and faulted in strcmp on
     * the garbage slot's name pointer. tests/unit/test_lang_name_table.c
     * pins the macro against the array so the drift can't recur. */
    const LangNameEntry *hit = (const LangNameEntry *)bsearch(
        name, kLangNameTable,
        sizeof(kLangNameTable) / sizeof(kLangNameTable[0]),
        sizeof(kLangNameTable[0]), langNameCmp);
    return hit ? hit->id : 0;
}

static char *unescapeValue(const char *raw) {
    size_t len = strlen(raw);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    char *w = out;
    for (size_t i = 0; i < len; i++) {
        char c = raw[i];
        if (c == '\\' && i + 1 < len) {
            char nxt = raw[i + 1];
            switch (nxt) {
                case 'n': *w++ = '\n'; i++; continue;
                case 't': *w++ = '\t'; i++; continue;
                case 'r': *w++ = '\r'; i++; continue;
                case '\\': *w++ = '\\'; i++; continue;
                case '"': *w++ = '"';  i++; continue;
                default:  break;
            }
        }
        *w++ = c;
    }
    *w = '\0';
    return out;
}

static void clearOverrides(void) {
    if (overrideTable) {
        for (unsigned int i = 0; i < overrideTableSize; i++) {
            if (overrideTable[i]) {
                free(overrideTable[i]);
            }
        }
        free(overrideTable);
        overrideTable = NULL;
        overrideTableSize = 0;
    }
    memset(&loadedMeta, 0, sizeof(loadedMeta));
    loadedMetaValid = FALSE;
}

static bool ensureOverrideSlot(unsigned int id) {
    if (id < overrideTableSize) return TRUE;
    unsigned int newSize = overrideTableSize ? overrideTableSize : 64;
    while (newSize <= id) newSize *= 2;
    char **grown = (char **)realloc(overrideTable, newSize * sizeof(char *));
    if (!grown) return FALSE;
    for (unsigned int i = overrideTableSize; i < newSize; i++) {
        grown[i] = NULL;
    }
    overrideTable = grown;
    overrideTableSize = newSize;
    return TRUE;
}

static void setOverride(langid id, char *value) {
    if (!ensureOverrideSlot(id)) {
        free(value);
        return;
    }
    if (overrideTable[id]) {
        free(overrideTable[id]);
    }
    overrideTable[id] = value;
}

/* Strip leading and trailing horizontal whitespace (and CR/LF) in
 * place. Returns a pointer into the original buffer. Used for the
 * KEY portion of a `key=value` line; never call it on the value side
 * — translators must be able to encode trailing spaces in values
 * (e.g. "Map Name: ") just by typing them. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

static void copyMetaField(char *dst, size_t dstSize, const char *src) {
    size_t n = strlen(src);
    if (n >= dstSize) n = dstSize - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool langSetup(void) {
    langFileName[0] = '\0';
    return TRUE;
}

void langCleanup(void) {
    clearOverrides();
    langFileName[0] = '\0';
}

bool langLoadFile(const char *path) {
    if (!path || !*path) return FALSE;

    FILE *f = fopen(path, "rb");
    if (!f) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "langLoadFile: could not open '%s'", path);
        clearOverrides();
        return FALSE;
    }

    /* Replace any previously-loaded translation. */
    clearOverrides();

    /* Header parsing remains active until the first body line is seen. */
    bool inHeader = TRUE;
    bool firstLine = TRUE;
    char line[8192];

    while (fgets(line, sizeof(line), f)) {
        char *p = line;

        /* Strip optional UTF-8 BOM on the first line. */
        if (firstLine) {
            firstLine = FALSE;
            if ((unsigned char)p[0] == 0xEF &&
                (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
        }

        /* Strip ONLY CR/LF from the end. Spaces and tabs are
         * significant in the value side (some labels end in a space,
         * e.g. "Map Name: "). */
        size_t lineLen = strlen(p);
        while (lineLen > 0 && (p[lineLen - 1] == '\r' ||
                               p[lineLen - 1] == '\n')) {
            p[--lineLen] = '\0';
        }

        /* Skip leading horizontal whitespace before the blank/comment
         * check so that indented "# comment" lines still count. */
        char *t = p;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '\0' || *t == '#') continue;

        char *eq = strchr(t, '=');
        if (!eq) {
            WB_LOG_WARN(WB_LOG_CAT_ASSET, "langLoadFile: skipping malformed line: %s", t);
            continue;
        }
        *eq = '\0';
        char *key = trim(t);     /* whitespace not meaningful in keys */
        char *value = eq + 1;    /* value preserved verbatim, including
                                    any leading or trailing spaces */

        if (inHeader) {
            if (strcmp(key, "name") == 0) {
                copyMetaField(loadedMeta.name, sizeof(loadedMeta.name), value);
                loadedMetaValid = TRUE;
                continue;
            }
            if (strcmp(key, "author") == 0) {
                copyMetaField(loadedMeta.author, sizeof(loadedMeta.author), value);
                loadedMetaValid = TRUE;
                continue;
            }
            if (strcmp(key, "notes") == 0) {
                copyMetaField(loadedMeta.notes, sizeof(loadedMeta.notes), value);
                loadedMetaValid = TRUE;
                continue;
            }
            /* Anything that isn't a known header key starts the body. */
            inHeader = FALSE;
        }

        langid id = resolveName(key);
        if (id == 0) {
            WB_LOG_WARN(WB_LOG_CAT_ASSET, "langLoadFile: unknown ID '%s' — skipping", key);
            continue;
        }

        if (id < overrideTableSize && overrideTable && overrideTable[id]) {
            WB_LOG_WARN(WB_LOG_CAT_ASSET, "langLoadFile: duplicate ID '%s' — last one wins", key);
        }

        char *decoded = unescapeValue(value);
        if (!decoded) {
            WB_LOG_ERROR(WB_LOG_CAT_ASSET, "langLoadFile: out of memory decoding '%s'", key);
            continue;
        }
        setOverride(id, decoded);
    }

    fclose(f);

    /* Remember which file is loaded for the language picker. */
    size_t n = strlen(path);
    if (n >= sizeof(langFileName)) n = sizeof(langFileName) - 1;
    memcpy(langFileName, path, n);
    langFileName[n] = '\0';

    return TRUE;
}

void langUnloadFile(void) {
    clearOverrides();
    langFileName[0] = '\0';
}

const LangFileMeta *langGetLoadedMeta(void) {
    return loadedMetaValid ? &loadedMeta : NULL;
}

void langGetFileName(char *fileName) {
    strcpy(fileName, langFileName);
}

/* -------------------------------------------------------
 * Header-only parse of a data/lang/<code>.txt: read just the
 * header lines (name=/author=/notes=) and stop at the first
 * body line. Used by the language picker so it can show the
 * translation's name without loading hundreds of override
 * strings into memory. Does NOT touch the global override
 * table. Returns TRUE if at least one header field was read.
 * ------------------------------------------------------- */
static bool readHeaderOnly(const char *path, LangFileMeta *out) {
    if (!path || !out) return FALSE;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;

    bool firstLine = TRUE;
    bool gotAny    = FALSE;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        char *p = line;

        if (firstLine) {
            firstLine = FALSE;
            if ((unsigned char)p[0] == 0xEF &&
                (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
        }

        size_t lineLen = strlen(p);
        while (lineLen > 0 && (p[lineLen - 1] == '\r' ||
                               p[lineLen - 1] == '\n')) {
            p[--lineLen] = '\0';
        }

        char *t = p;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '\0' || *t == '#') continue;

        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(t);
        char *value = eq + 1;

        if (strcmp(key, "name") == 0) {
            copyMetaField(out->name, sizeof(out->name), value);
            gotAny = TRUE;
            continue;
        }
        if (strcmp(key, "author") == 0) {
            copyMetaField(out->author, sizeof(out->author), value);
            gotAny = TRUE;
            continue;
        }
        if (strcmp(key, "notes") == 0) {
            copyMetaField(out->notes, sizeof(out->notes), value);
            gotAny = TRUE;
            continue;
        }
        /* First non-header key — stop. */
        break;
    }

    fclose(f);
    return gotAny;
}

static void lowercaseAscii(char *s) {
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + ('a' - 'A'));
    }
}

/* Strip ".txt" (case-insensitive) off the end of `name`, in place. */
static void stripTxtExt(char *name) {
    size_t n = strlen(name);
    if (n >= 4) {
        char tail[5];
        tail[0] = (char)((name[n - 4] >= 'A' && name[n - 4] <= 'Z') ? name[n - 4] + 32 : name[n - 4]);
        tail[1] = (char)((name[n - 3] >= 'A' && name[n - 3] <= 'Z') ? name[n - 3] + 32 : name[n - 3]);
        tail[2] = (char)((name[n - 2] >= 'A' && name[n - 2] <= 'Z') ? name[n - 2] + 32 : name[n - 2]);
        tail[3] = (char)((name[n - 1] >= 'A' && name[n - 1] <= 'Z') ? name[n - 1] + 32 : name[n - 1]);
        tail[4] = '\0';
        if (strcmp(tail, ".txt") == 0) {
            name[n - 4] = '\0';
        }
    }
}

LangFileEntry *langPickerScan(int *outCount) {
    if (outCount) *outCount = 0;

    /* Always start with the synthetic "English (Default)" entry. The
     * static langTable[] is the English baseline, so selecting English
     * is equivalent to clearing any loaded override file. */
    int    cap   = 16;
    int    n     = 0;
    LangFileEntry *list = (LangFileEntry *)calloc((size_t)cap, sizeof(*list));
    if (!list) return NULL;

    strcpy(list[0].code, "en");
    list[0].path[0] = '\0';
    /* Use the IDs the dialog already exposes for the English defaults so
     * the picker stays consistent if a translator re-localizes them. */
    {
        const char *engName   = (const char *)lookupString(STR_DLGLANG_NAME);
        const char *engAuthor = (const char *)lookupString(STR_DLGLANG_AUTHOR);
        const char *engNotes  = (const char *)lookupString(STR_DLGLANG_NOTES);
        copyMetaField(list[0].meta.name,   sizeof(list[0].meta.name),   engName);
        copyMetaField(list[0].meta.author, sizeof(list[0].meta.author), engAuthor);
        copyMetaField(list[0].meta.notes,  sizeof(list[0].meta.notes),  engNotes);
    }
    n = 1;

    /* Per-platform directory scan. SDL_GlobDirectory doesn't see files
     * inside the iOS app bundle (verified via the bg_game pickRandomMap
     * path in gamefront.c), so iOS uses opendir/readdir directly.
     * Both branches feed the same per-entry processing below. */
    int   fileCount = 0;
    char **fileNames = NULL;
#if defined(__IPHONEOS__)
    {
        DIR *d = opendir("data/lang");
        if (d) {
            int   dcap = 16;
            char **arr = (char **)calloc((size_t)dcap, sizeof(*arr));
            if (arr) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    size_t nlen = strlen(ent->d_name);
                    if (nlen <= 4) continue;
                    if (strcasecmp(ent->d_name + nlen - 4, ".txt") != 0) continue;
                    if (fileCount >= dcap) {
                        int dnew = dcap * 2;
                        char **grown = (char **)realloc(arr,
                            (size_t)dnew * sizeof(*arr));
                        if (!grown) break;
                        arr = grown;
                        dcap = dnew;
                    }
                    arr[fileCount++] = SDL_strdup(ent->d_name);
                }
                fileNames = arr;
            }
            closedir(d);
        }
    }
#else
    fileNames = SDL_GlobDirectory("data/lang", "*.txt",
                                  SDL_GLOB_CASEINSENSITIVE, &fileCount);
#endif

    if (fileNames) {
        for (int i = 0; i < fileCount; i++) {
            const char *fname = fileNames[i];
            if (!fname || !*fname) continue;

            /* basename → code. Both SDL_GlobDirectory and our iOS readdir
             * pass return basenames only, so copy directly. */
            char code[32];
            size_t fnLen = strlen(fname);
            if (fnLen >= sizeof(code)) fnLen = sizeof(code) - 1;
            memcpy(code, fname, fnLen);
            code[fnLen] = '\0';
            stripTxtExt(code);
            lowercaseAscii(code);

            /* Skip the synthetic English baseline if a generated data/lang/en.txt
             * is present alongside it — they're functionally equivalent and
             * we don't want a duplicate entry in the dropdown. */
            if (strcmp(code, "en") == 0) continue;

            char path[FILENAME_MAX];
            snprintf(path, sizeof(path), "data/lang/%s", fname);

            LangFileMeta meta;
            if (!readHeaderOnly(path, &meta)) {
                /* Header missing or unreadable; still list the file with
                 * a fallback name derived from the code so the user has
                 * something to click on. */
                memset(&meta, 0, sizeof(meta));
                copyMetaField(meta.name, sizeof(meta.name), code);
            }

            if (n >= cap) {
                int newCap = cap * 2;
                LangFileEntry *grown = (LangFileEntry *)realloc(
                    list, (size_t)newCap * sizeof(*list));
                if (!grown) break;
                memset(grown + cap, 0,
                       (size_t)(newCap - cap) * sizeof(*list));
                list = grown;
                cap  = newCap;
            }

            strcpy(list[n].code, code);
            strcpy(list[n].path, path);
            list[n].meta = meta;
            n++;
        }
#if defined(__IPHONEOS__)
        for (int i = 0; i < fileCount; i++) SDL_free(fileNames[i]);
        free(fileNames);
#else
        SDL_free(fileNames);
#endif
    }

    if (outCount) *outCount = n;
    return list;
}

void langPickerFreeEntries(LangFileEntry *entries, int count) {
    (void)count;
    /* Entries are stored inline in one heap allocation; no per-entry
     * cleanup needed. */
    free(entries);
}

void langAutoDetect(char *outCode, int outSize) {
    if (outCode && outSize > 0) outCode[0] = '\0';

    int             count   = 0;
    LangFileEntry  *entries = langPickerScan(&count);
    if (!entries || count <= 1) {
        /* No translation files on disk; nothing to detect. */
        if (entries) langPickerFreeEntries(entries, count);
        return;
    }

    int             localeCount = 0;
    SDL_Locale    **locales     = SDL_GetPreferredLocales(&localeCount);
    if (!locales || localeCount <= 0) {
        if (locales) SDL_free(locales);
        langPickerFreeEntries(entries, count);
        return;
    }

    /* Walk preferred locales in order; for each, try language-COUNTRY
     * first ("pt-br"), then language alone ("pt"). First match wins. */
    for (int li = 0; li < localeCount; li++) {
        const SDL_Locale *loc = locales[li];
        if (!loc || !loc->language || !*loc->language) continue;

        char tag[64];
        if (loc->country && *loc->country) {
            snprintf(tag, sizeof(tag), "%s-%s", loc->language, loc->country);
        } else {
            snprintf(tag, sizeof(tag), "%s", loc->language);
        }
        lowercaseAscii(tag);

        /* Two passes: first the full BCP47-ish tag, then the language
         * portion alone. Skip index 0 (synthetic English baseline) on
         * matching — fresh installs whose locale is en-* fall through
         * with no override loaded, which is the correct behaviour. */
        for (int pass = 0; pass < 2; pass++) {
            const char *target = tag;
            if (pass == 1) {
                char *dash = strchr(tag, '-');
                if (!dash) continue; /* nothing to retry */
                *dash = '\0';
                target = tag;
            }

            for (int ei = 1; ei < count; ei++) {
                if (strcmp(target, entries[ei].code) == 0) {
                    if (langLoadFile(entries[ei].path)) {
                        if (outCode && outSize > 0) {
                            size_t cl = strlen(entries[ei].code);
                            if (cl >= (size_t)outSize) cl = (size_t)outSize - 1;
                            memcpy(outCode, entries[ei].code, cl);
                            outCode[cl] = '\0';
                        }
                        SDL_free(locales);
                        langPickerFreeEntries(entries, count);
                        return;
                    }
                }
            }
            /* Restore tag for pass 1 if pass 0 mutated it (it didn't,
             * so this is a no-op — but keep the structure clear). */
        }
    }

    SDL_free(locales);
    langPickerFreeEntries(entries, count);
}

char *langGetText(langid id) {
    if (overrideTable && id < overrideTableSize && overrideTable[id]) {
        return overrideTable[id];
    }
    return (char *)lookupString(id);
}

/* -------------------------------------------------------
 * langGetTextFmt — single-pass named-placeholder substitution.
 *
 * Substitutes the literal tokens {player}, {other}, {number} with
 * fields from `args`. Substitution is non-recursive: braces inside a
 * substituted value (e.g. a player name like "{ACCEL}lover") are not
 * rescanned. Output is written into one of a small ring of thread-local
 * buffers, so up to LANG_FMT_RING_BUFFERS overlapping calls (e.g.
 * `printf("%s vs %s", langGetTextFmt(...), langGetTextFmt(...))`) all
 * keep their pointers valid for the lifetime of the printf. Anything
 * past the cap is truncated cleanly.
 * ------------------------------------------------------- */

#define LANG_FMT_BUFFER_SIZE   1024
#define LANG_FMT_RING_BUFFERS  4

static THREAD_LOCAL char    g_fmtBuffers[LANG_FMT_RING_BUFFERS][LANG_FMT_BUFFER_SIZE];
static THREAD_LOCAL unsigned g_fmtBufferIdx = 0;

static void appendBounded(char *dst, size_t cap, size_t *used, const char *src,
                          size_t srcLen, bool *truncated) {
    if (*used >= cap - 1) {
        if (srcLen > 0 && truncated) *truncated = true;
        return;
    }
    size_t room = cap - 1 - *used;
    if (srcLen > room) {
        if (truncated) *truncated = true;
        srcLen = room;
    }
    memcpy(dst + *used, src, srcLen);
    *used += srcLen;
    dst[*used] = '\0';
}

const char *langGetTextFmt(langid id, const MessageArgs *args) {
    const char *src = langGetText(id);
    if (!args || !src) return src ? src : "";

    char *dst = g_fmtBuffers[g_fmtBufferIdx];
    g_fmtBufferIdx = (g_fmtBufferIdx + 1) % LANG_FMT_RING_BUFFERS;

    size_t used = 0;
    bool   truncated = false;
    dst[0] = '\0';

    char numberBuf[4][32];
    int  numberLen[4] = {-1, -1, -1, -1};

    const char *p = src;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p, '}');
            if (end) {
                size_t tokLen = (size_t)(end - p - 1);
                const char *replacement = NULL;
                size_t      replLen     = 0;

                if (tokLen == 6 && memcmp(p + 1, "player", 6) == 0) {
                    replacement = args->playerName;
                    replLen     = 0;
                    while (replLen < PLAYER_NAME_LEN &&
                           args->playerName[replLen] != '\0') replLen++;
                } else if (tokLen == 5 && memcmp(p + 1, "other", 5) == 0) {
                    replacement = args->otherName;
                    replLen     = 0;
                    while (replLen < PLAYER_NAME_LEN &&
                           args->otherName[replLen] != '\0') replLen++;
                } else if (tokLen >= 6 && tokLen <= 7 &&
                           memcmp(p + 1, "number", 6) == 0) {
                    int slot = -1;
                    if (tokLen == 6) {
                        slot = 0;
                    } else {
                        char d = p[7];
                        if (d == '2') slot = 1;
                        else if (d == '3') slot = 2;
                        else if (d == '4') slot = 3;
                    }
                    if (slot >= 0) {
                        if (numberLen[slot] < 0) {
                            int v = (slot == 0) ? args->number
                                  : (slot == 1) ? args->number2
                                  : (slot == 2) ? args->number3
                                                : args->number4;
                            numberLen[slot] = snprintf(numberBuf[slot],
                                                       sizeof(numberBuf[slot]),
                                                       "%d", v);
                            if (numberLen[slot] < 0) numberLen[slot] = 0;
                        }
                        replacement = numberBuf[slot];
                        replLen     = (size_t)numberLen[slot];
                    }
                } else if (tokLen == 7 && memcmp(p + 1, "string", 6) == 0) {
                    char d = p[7];
                    const char *s = NULL;
                    if (d == '1') s = args->string1;
                    else if (d == '2') s = args->string2;
                    else if (d == '3') s = args->string3;
                    if (s) {
                        replacement = s;
                        replLen     = 0;
                        while (replLen < LANG_MSGARG_STRING_LEN &&
                               s[replLen] != '\0') replLen++;
                    }
                }

                if (replacement) {
                    appendBounded(dst, LANG_FMT_BUFFER_SIZE, &used,
                                  replacement, replLen, &truncated);
                    p = end + 1;
                    continue;
                }
            }
        }
        appendBounded(dst, LANG_FMT_BUFFER_SIZE, &used, p, 1, &truncated);
        p++;
    }

    if (truncated) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET, "langGetTextFmt: id=%u rendered output exceeded "
                "LANG_FMT_BUFFER_SIZE=%d; result was clipped",
                (unsigned)id, LANG_FMT_BUFFER_SIZE);
    }

    return dst;
}
