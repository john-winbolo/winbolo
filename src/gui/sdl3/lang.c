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
*Filename:      lang.c
*Author:        John Morrison
*Purpose:
*  Cross-platform string table (English built-in) plus an
*  optional runtime override table loaded from lang/<code>.txt.
*  The static langTable[] below remains the source of truth
*  for English; non-English builds layer overrides on top.
*********************************************************/

#include "../lang.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char langFileName[FILENAME_MAX];
static char langBuff[16 * 1024];
static char langBuff2[16 * 1024];

typedef struct { unsigned int id; const char *text; } LangEntry;

static const LangEntry langTable[] = {
    {151,  "WinBolo Game Selection"},
    {152,  "Welcome to WinBolo, the multiplayer tank game.\nPlease choose a game type from the list below:"},
    {153,  "Practice"},
    {154,  "TCP/IP"},
    {155,  "Internet"},
    {156,  "Skip this Dialog Next Time"},
    {157,  "OK"},
    {158,  "Quit"},
    {159,  "Language Select"},
    {160,  "Name:"},
    {161,  "English (Default)"},
    {162,  "Author: "},
    {163,  "John Morrison"},
    {164,  "Local Network"},
    {165,  "Tutorial"},
    {166,  "Notes: "},
    {167,  "None."},
    {168,  "Note: For incomplete translations the missing items default back to English."},
    {169,  "About WinBolo"},
    {170,  "The WinBolo package may be freely distributed provided the neither the program nor any of the accompanying files are omitted or modified in any way.\n\nWinBolo is a shareware program. Shareware software is not free. It costs money, just like other software you buy, except that with shareware you get to try it out first to decide if you like it.\n\nWinBolo costs $25(US) If you decide to keep WinBolo you can pay for it via the following methods:\n\nRun the program register.exe in the WinBolo directory, it allow you to pay via cheque, money order, cash or credit card via email, fax or postal mediums.\n\nTo register via credit card online visit: http://order.kagi.com/?XYV and fill out the form.\n\nRead the text file Readme(Shareware) included in the distribution for more information."},
    {171,  "Alliance Request"},
    {172,  "Accept"},
    {173,  "Reject"},
    {174,  "{player} requests alliance. Accept?"},
    {175,  "Try hitting refresh first."},
    {176,  "Message of the Day"},
    {177,  "The game you tried to join is a different version of WinBolo. Try joining a different game"},
    {178,  "No games in progress (Start one!)"},
    {179,  "Yes"},
    {180,  "No"},
    {181,  "Yes (Adv)"},
    {182,  "Open"},
    {183,  "Tournament"},
    {184,  "Strict"},
    {185,  "Game Info"},
    {186,  "Map: "},
    {187,  "Players: {number}"},
    {188,  "Game Type: "},
    {189,  "Hidden Mines: "},
    {190,  "Computer Tanks Allowed: "},
    {191,  "Time Limit:"},
    {192,  "Please select the options you want for your new game:"},
    {193,  "Cancel"},
    {194,  "Game Setup"},
    {195,  "Choose map"},
    {196,  "Selected Map: "},
    {197,  "Selected Map: Everard Island (Inbuilt)"},
    {198,  "Open Game (pre-armed)"},
    {199,  "Tournament (free ammo early)"},
    {200,  "Strict Tournament (no free ammo)"},
    {201,  "Allow Hidden Mines"},
    {202,  "Allow Computer Tanks"},
    {203,  "and give them an advantage"},
    {204,  "Game Password"},
    {205,  "minutes"},
    {206,  "seconds"},
    {207,  "Game start delay"},
    {208,  "Game time limit"},
    {209,  "Error Opening Map."},
    {210,  "About {number} minute(s)"},
    {211,  "Open"},
    {212,  "Tournament"},
    {213,  "Strict Tournament"},
    {214,  "Yes (Advantage)"},
    {215,  "Me"},
    {216,  "Key Setup"},
    {217,  "Click on the option you wish to change then the next key pressed will be assigned to it. The key ALT, PRINT SCREEN and PAUSE can not be assigned to anything."},
    {218,  "Drive Tank"},
    {219,  "Rotate Tank:"},
    {220,  "Gun Range"},
    {221,  "Weapons"},
    {222,  "Views"},
    {223,  "Scroll"},
    {224,  "(left)"},
    {225,  "(right)"},
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
    {238,  "Up"},
    {240,  "Down"},
    {241,  "Left"},
    {242,  "Right"},
    {243,  "Press new key for "},
    {244,  "forward"},
    {245,  "backward"},
    {246,  "rotate left"},
    {247,  "rotate right"},
    {248,  "increase range"},
    {249,  "decrease range"},
    {250,  "shoot"},
    {251,  "lay mine"},
    {252,  "tank view"},
    {253,  "pillbox view"},
    {254,  "scroll up"},
    {255,  "scroll down"},
    {256,  "scroll left"},
    {257,  "scroll right"},
    {258,  "Send"},
    {259,  "All Players"},
    {260,  "All Allies"},
    {261,  "All Nearby"},
    {262,  "Selected Players"},
    {263,  "Sending message to {number} player"},
    {264,  "Sending message to {number} players"},
    {265,  "Network Info"},
    {266,  "Server address:"},
    {267,  "This game address:"},
    {268,  "Server ping: {number} ms"},
    {269,  "Packets per second (per player):"},
    {270,  "Status:"},
    {271,  "Net errors: {number}"},
    {272,  "Password Required"},
    {273,  "This game requires a password:"},
    {274,  "Enter Player Name"},
    {275,  "Enter the new player name for your tank:"},
    {276,  "Tracker Config"},
    {277,  "Use Tracker"},
    {278,  "Tracker Address:"},
    {279,  "Tracker Port:"},
    {280,  "System Info"},
    {281,  "CPU Usage:"},
    {282,  "Sim Modeling:"},
    {283,  "Com Processing:"},
    {284,  "Graphics display:"},
    {285,  "AI tank control processing:"},
    {286,  "Total:"},
    {287,  "Graphics frames per second"},
    {288,  "UDP (Internet) Setup"},
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
    {304,  "Can't set the window subclass for copying!"},
    {305,  "Join &By Address"},
    {306,  "&Refresh"},
    {307,  "Set &Player Name"},
    {308,  "&Message of the Day"},
    {309,  "Status: "},
    {310,  "Selected Game Information"},
    {311,  "Brains:"},
    {312,  "Password:"},
    {313,  "No of free Pillboxes:"},
    {314,  "No of free Bases:"},
    {315,  "Hidden Mines:"},
    {316,  "No of Players:"},
    {317,  "Version:"},
    {318,  "Game Type:"},
    {319,  "Server Port:"},
    {320,  "Server Address:"},
    {321,  "Brains Directory does not exist.\nNot going to load brains!"},
    {322,  "Error Launching Brain"},
    {323,  "Error Launching Brain - Brain Does not contain \"BrainMain\""},
    {324,  "Error initing brain"},
    {325,  "Could not execute brain"},
    {326,  "Creating DD Object Failed"},
    {327,  "Creating DD co-op level Failed"},
    {328,  "Creating DD Primary Surface Failed"},
    {329,  "Getting DD Pixel Format Failed"},
    {330,  "Getting Surface Description Failed"},
    {331,  "This Version of WinBolo does not run in palette mode.\n Please change your colour depth to greater than 256 colours"},
    {332,  "Creating DD buffer Failed"},
    {333,  "DD Get DC failed"},
    {334,  "Creating DD Clipper Failed"},
    {335,  "Error Creating Drawing Brush"},
    {336,  "Error Creating Drawing Pen"},
    {337,  "Game Starts in "},
    {338,  "Error Releasing DC for drawing tank label (Thats bad)"},
    {339,  "Pillbox View"},
    {340,  "Cant load font Courier New"},
    {341,  "An error occurred in reading preferences. The defaults will be used"},
    {342,  "Error Creating Window"},
    {343,  "Error Setting up Direct Draw"},
    {344,  "Error Setting up Direct Sound"},
    {345,  "Error Setting up Direct Input"},
    {346,  "Error Loading Cursor"},
    {347,  "Error Loading Fonts"},
    {348,  "LAN Game Finder"},
    {349,  "Tracker Game Finder"},
    {350,  "An error occurred trying to join the game"},
    {351,  "An error occurred trying to spawn the server process.\n Is the program \"WinBoloDS\" in the same directory as WinBolo?"},
    {352,  "Something really bizarre happened in the networking subsystems which is strange since you selected a single player game. Lucky you..."},
    {353,  "Launching dedicated server. Type \"quit\" in its window to quit."},
    {354,  "Creation of Direct Input Keyboard Device Failed"},
    {355,  "Setting the Data Format for the Direct Input Keyboard Device Failed"},
    {356,  "Setting the Co-operative Level for the Direct Input Keyboard Device Failed"},
    {357,  "Error Starting Winsock"},
    {358,  "Error Creating UDP Socket"},
    {359,  "Error Creating TCP Socket"},
    {360,  "Error Binding UDP Socket\n Some other program has this port assigned already. Try choosing another.\n\nNOTE: If you started a network game and it launched successfully then it is still running.\n You will not need to start it again"},
    {361,  "Error adding network Events to messaging chain"},
    {362,  "Unsupported Tracker Version"},
    {363,  "Error finding tracker (DNS lookup failure)"},
    {364,  "Error Connecting to tracker"},
    {365,  "Error setting socket to be nonblocking"},
    {366,  "Receiving game info..."},
    {367,  "Processing tracker data..."},
    {368,  "Error: Tracker sent no data"},
    {369,  "Connecting: "},
    {370,  "Idle"},
    {371,  "Error binding socket"},
    {372,  "Error sending broadcast"},
    {373,  "Receiving responses..."},
    {374,  "Can't Load WinBolo Sounds - Continuing without Sound"},
    {375,  "Error Setting Up Direct Sound - Hardware is in use by another application.\nContinuing without sound."},
    {376,  "Creating DS Object Failed\nContinuing without sound."},
    {377,  "Creating DS Co-Op Level Failed"},
    {378,  "Creating DS Primary Buffer Failed"},
    {379,  "Loading of one or more sound files failed"},
    {380,  "Error Creating Mutex. (Thats bad)"},
    {381,  "Error Loading Brain List"},
    {382,  "Error Setting up Key Setup Class"},
    {383,  "Timelimit has expired. The Game has ended."},
    {384,  "Error Saving Map (Disk full?)"},
    {385,  "Ahead of you is a short river leading inland. Hold\ndown the {ACCEL} key to drive your boat to the end of\nthe river.\n\nWhen you get there, keep holding {ACCEL}. The tank will\ndisembark from the boat and the boat will be left\nmoored at the end of the river"},
    {386,  "You are now on the grass. The tank moves quite\nquickly on grass.\n\n\nKeep pressing {ACCEL} to move ahead to the forest."},
    {387,  "You are now in the forest. The tank moves more\nslowly in the forest than it does on grass.\n\n\nKeep pressing {ACCEL} to move ahead to the swamp."},
    {388,  "You are now in the swamp. The tank moves very\nslowly in swamp. Fortunately there is a road ahead.\n\n\nKeep pressing {ACCEL} to move ahead to the road."},
    {389,  "You are now on the road. The tank can move very\nquickly on road and if you press {ACCEL} the tank will\nspeed up.\n\n\nIf you find yourself going too fast you can press {BRAKE}"},
    {390,  "There are some buildings ahead. Buildings are solid\nobstacles that you cannot drive through, so you will\nhave to find your way through the maze to the other\nside.\n\nTo make the tank turn left press {LEFT}\nTo make the tank turn right press {RIGHT}"},
    {391,  "These round objects with the red guns poking out are\nautomatic pillboxes. They will shoot at any tank\ncomes with within range.\n\n\nFortunately the buildings provide protection from the\nshots, so the pillboxes will not be able to hit you\nunless you wait so long that they manage to\ncompletely shoot their way through the buildings."},
    {392,  "Forest also provides protection from pillboxes, but in\na different way. As well as slowing down the tank a\nlot, driving through forest also limits visibility.\n\n\nWhen you are inside forest you can still clearly see\neverything outside the forest, but pillboxes (and\nother players) cannot see your tank unless you get\nvery close to them."},
    {393,  "Line up your tank with the middle of the strip of\nforest and drive due North. If you go straight\nand don't stray too close to the edges of the forest,\nthe pillboxes will not see you and they will not shoot.\n\n\nDon't drive outside the forest of you'll get blown to pieces.\n"},
    {394,  "There is one other simple way to escape pillboxes -\nspeed.\n\n\nMake sure you are correctly lined up with the centre\nof the road, and hold {ACCEL} to go full speed past the\npillboxes."},
    {395,  "When they notice you they will start shooting, but if\nyou keep going straight ahead and don't lose your\nnerve you will be out of range before the shots can\nhit you.\n\n\nDon't run off the road into the marsh or you'll be a\nsitting duck target for the pillboxes, and you don't be\nable to move quickly enough to escape."},
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
    {408,  "After you manage to defeat this pillbox, your final\nchallenge of the tutorial is to take out the two\npillboxes guarding the exit, build a boat, and escape.\n\n\nTake special care to watch your tank's armour level -\nif you get your tank destroyed you'll have to start\nwith a new tank on a new boat out at sea again."},
    {409,  "Congratulations. You have completed the WinBolo\ntutorial.\n\n\n\nNow organise some friends to play with and find out\nwhat it is like to compete against intelligent human\nopponents instead of stationary unthinking targets.\n\nIf you'd like to replay the tutorial later, you can\nlaunch it again from the Settings menu."},
    {410,  "Welcome to the WinBolo tutorial. This island introduces\nthe basic principles of WinBolo and leads you through\nthem one at a time. Each new principle will be\ndescribed in a window like this one.\n\nAfter reading each message, you can proceed by\nclicking the \"OK\" button on the mouse or simply\nby pressing the <Return> key on the keyboard"},
    {411,  "WinBolo is the only authorized clone of Stuart Cheshire's classic Macintosh network game, Bolo.\nYou can find strategy hints and other information from the official website:\n\nhttps://www.winbolo.com\n\nOr come join us on Reddit and Discord\n"},
    {412,  "One quick note about Internet play.\n\nIf you join a online game and are a lot of network errors\nthen leave the game straight away before\nyou ruin the game for the other players."},
    {413,  "You are in control of a tank, which is currently on a\nboat at sea. Press {ACCEL} to make the tank (and boat) go\nforwards, and press {BRAKE} to make it slow down and\nstop.\n\nDon't tap the keys as if you are typing a letter -\npress and hold them until the tank is going at the speed\nyou want and then let go. Press {ACCEL} now to make the\nboat drive forwards towards the land.\n(Press <Return> first to dismiss the window)"},
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
    {460,  "Languages"},
    {461,  "Help"},
    {462,  "Error opening help file."},
    {463,  "Skin Selection"},
    {464,  "If not all skin items are present then the default built in skin/sounds are used."},
    {465,  "The man cannot build under your boat"},
    {466,  "Sorry, but your player name is blank, you must have a player name"},
    {467,  "The server was unable to prepare the map data. Please try again"},

    /* System Info panel additions */
    {482,  "Frame Rate:"},
    {483,  "Graphics:"},
    {484,  "AI Tanks:"},

    /* Network Info panel additions */
    {485,  "Server:"},
    {486,  "This game:"},
    {487,  "Ping: min %d / avg %d / max %d ms"},
    {488,  "KB/s In:"},
    {489,  "KB/s Out:"},
    {490,  "Packets/sec: %d in / %d out"},
    {491,  "KB/sec: %.1f in / %.1f out"},

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
    {542,  "Exit"},
    {543,  "Edit"},
    {544,  "Frame Rate"},
    {545,  "Window Size"},
    {546,  "Normal"},
    {547,  "Double"},
    {548,  "Triple"},
    {549,  "Quad"},
    {550,  "Custom (Resizable)"},
    {551,  "Requires %dx%d - exceeds display"},
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
    {606,  "Map preview unavailable"},
    {607,  "Pillboxes:"},
    {608,  "Bases:"},
    {609,  "Starts:"},
    {610,  "Skip Map"},
    {611,  "Cancel Skip"},
    {612,  "%d/%d votes to skip"},
    {613,  "Chat"},
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
    {641,  "%s:%u | %s | %s Mines | AI: %s | %s"},
    {642,  "%dh %02dm %02ds"},
    {643,  "%dm %02ds"},
    {644,  "{number}s"},
    {645,  "Map:"},
    {646,  "WinBolo - Game Setup"},
    {647,  "Select a map:"},
    {648,  "Back"},
    {649,  "Bases: %d  Starts: %d"},
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
    {675,  "WinBolo - %s"},
    {676,  "Refresh"},
    {677,  "Loading games list%s"},
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
    {698,  "Status: %s  |  %d servers  |  Pinging %d..."},
    {699,  "Status: %s  |  %d servers"},
    {700,  "New Game"},
    {701,  "Player Name"},
    {702,  "Manual"},
    {703,  "Server is running a different version of WinBolo."},
    {704,  "You must set a player name first."},
    {705,  "Set Player Name"},
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
    {721,  "Error: %s"},
    {722,  "Map"},
    {723,  "Type"},
    {724,  "Players"},
    {725,  "Rating"},
    {726,  "Size"},
    {727,  "Date"},
    {728,  "Players: "},
    {729,  "{number} player(s)"},
    {730,  "Duration: %s"},
    {731,  "Rating: %.1f/10 (%d)"},
    {732,  "Downloads: {number}"},
    {733,  "Size: %s"},
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
    {746,  "Page %d of %d  (%d total)"},
    {747,  "Open File..."},
    {748,  "Failed to parse response"},
    {749,  "Network error"},
    {750,  "Failed to load details"},
    {751,  "Download failed"},
    {752,  "Could not determine save path"},
    {753,  "Failed to post comment"},
    {754,  "Fetch failed"},
    {755,  "Unknown error"},
    {756,  "WinBolo Log Files"},
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
    {776,  "(expires %s)"},
    {777,  "Sign out of WBN"},
    {778,  "Not signed in"},
    {779,  "Sign in to WBN..."},
    {780,  "Sign in to WinBolo.net"},
    {781,  "Sign in with your WinBolo.net username and password. A token will be saved so you don't need to enter your password again."},
    {782,  "Username:"},
    {783,  "Password:"},
    {784,  "Signing in..."},
    {785,  "Sign in"},
    {786,  "Please enter your username and password."},
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
    {800,  "Pillboxes: %d  Bases: %d  Starts: %d"},
    {801,  "Map Files"},
    {802,  "All Files"},
    {803,  "Status"},
    {804,  "Kills: %d  Deaths: %d"},
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

    /* Touch (tablet/mobile) siblings of the tutorial strings whose
     * desktop wording assumes a keyboard or mouse. Picked at display
     * time by tutorialResolveText() when uiModeIsTablet() is true. */
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
    const LangNameEntry *hit = (const LangNameEntry *)bsearch(
        name, kLangNameTable, K_LANG_NAME_TABLE_SIZE,
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
        SDL_Log("langLoadFile: could not open '%s'", path);
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
            SDL_Log("langLoadFile: skipping malformed line: %s", t);
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
            SDL_Log("langLoadFile: unknown ID '%s' — skipping", key);
            continue;
        }

        if (id < overrideTableSize && overrideTable && overrideTable[id]) {
            SDL_Log("langLoadFile: duplicate ID '%s' — last one wins", key);
        }

        char *decoded = unescapeValue(value);
        if (!decoded) {
            SDL_Log("langLoadFile: out of memory decoding '%s'", key);
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

char *langGetText(langid id) {
    if (overrideTable && id < overrideTableSize && overrideTable[id]) {
        return overrideTable[id];
    }
    return (char *)lookupString(id);
}

char *langGetText2(langid id) {
    return langGetText(id);
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

#define LANG_FMT_BUFFER_SIZE   512
#define LANG_FMT_RING_BUFFERS  4

static THREAD_LOCAL char    g_fmtBuffers[LANG_FMT_RING_BUFFERS][LANG_FMT_BUFFER_SIZE];
static THREAD_LOCAL unsigned g_fmtBufferIdx = 0;

static void appendBounded(char *dst, size_t cap, size_t *used, const char *src,
                          size_t srcLen) {
    if (*used >= cap - 1) return;
    size_t room = cap - 1 - *used;
    if (srcLen > room) srcLen = room;
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
    dst[0] = '\0';

    char numberBuf[32];
    int numberLen = -1;  /* lazily formatted on first use */

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
                } else if (tokLen == 6 && memcmp(p + 1, "number", 6) == 0) {
                    if (numberLen < 0) {
                        numberLen = snprintf(numberBuf, sizeof(numberBuf),
                                             "%d", args->number);
                        if (numberLen < 0) numberLen = 0;
                    }
                    replacement = numberBuf;
                    replLen     = (size_t)numberLen;
                }

                if (replacement) {
                    appendBounded(dst, LANG_FMT_BUFFER_SIZE, &used,
                                  replacement, replLen);
                    p = end + 1;
                    continue;
                }
            }
        }
        appendBounded(dst, LANG_FMT_BUFFER_SIZE, &used, p, 1);
        p++;
    }

    return dst;
}
