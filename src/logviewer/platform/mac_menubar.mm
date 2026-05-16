#import <Cocoa/Cocoa.h>

#include <SDL3/SDL.h>

#include "mac_menubar.h"
#include "../../gui/lang.h"

extern "C" {
#include "../logviewer.h"
#include "../draw.h"
#include "../dns.h"
#include "../imgui/imgui_main_menu.h"
#include "../imgui/imgui_dialogs.h"
void lv_windowOpenFile(char *cmdLine);
void lv_windowSaveMap(void);
void lv_windowPlay(void);
void lv_windowPause(void);
void lv_windowStop(int corruptLog);
void lv_windowRewind(void);
void lv_windowFastForward(void);
}

#define LANG_STR(id) ([NSString stringWithUTF8String:langGetText(id)])

@class LVMenuBridge;
static LVMenuBridge       *s_lv_bridge          = nil;
static NSMenu             *s_lv_savedMainMenu   = nil;
static LogViewerState     *s_lv_state           = NULL;
/* When embedded, we detach the host's app-menu submenu (NSMenu can't be
 * the submenu of two NSMenuItems at once) and reattach it on uninstall.
 * Both pointers are non-nil iff a detach occurred. */
static NSMenuItem         *s_lv_hostAppItem     = nil;
static NSMenu             *s_lv_hostAppSubmenu  = nil;

/* Cached references walked by lv_mac_menubar_refresh(). Submenus cache
 * the NSMenu so refresh can iterate items by tag. */
static NSMenuItem *s_lv_saveMapItem    = nil;
static NSMenuItem *s_lv_returnItem     = nil;   /* embedded only: File > Return to Main Menu */
static NSMenuItem *s_lv_playItem       = nil;
static NSMenuItem *s_lv_pauseItem      = nil;
static NSMenuItem *s_lv_stopItem       = nil;
static NSMenuItem *s_lv_fastForwardItem = nil;
static NSMenuItem *s_lv_rewindItem     = nil;
static NSMenu     *s_lv_zoomMenu       = nil;
static NSMenuItem *s_lv_zoomInItem     = nil;
static NSMenuItem *s_lv_zoomOutItem    = nil;
static NSMenuItem *s_lv_modeInfoItem   = nil;
static NSMenuItem *s_lv_modeSelectItem = nil;
static NSMenuItem *s_lv_useTeamColoursItem = nil;
static NSMenuItem *s_lv_tankCentredItem    = nil;
static NSMenuItem *s_lv_soundEffectsItem   = nil;
static NSMenuItem *s_lv_dnsLookupsItem     = nil;
static NSMenuItem *s_lv_winControlsItem    = nil;
static NSMenuItem *s_lv_winEventsItem      = nil;
static NSMenuItem *s_lv_winGameInfoItem    = nil;
static NSMenuItem *s_lv_winItemInfoItem    = nil;
static NSMenuItem *s_lv_winCommentsItem    = nil;

static int s_lv_zoomStepsBuilt = -1;   /* Zoom submenu's per-step items rebuilt on count change. */

static void lv_push_sdl_quit(void) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&ev);
}

@interface LVMenuBridge : NSObject
- (void)onQuit:(id)sender;
- (void)onAbout:(id)sender;
- (void)onOpenWbn:(id)sender;
- (void)onOpenFile:(id)sender;
- (void)onSaveMap:(id)sender;
- (void)onReturn:(id)sender;
- (void)onPlay:(id)sender;
- (void)onPause:(id)sender;
- (void)onStop:(id)sender;
- (void)onFastForward:(id)sender;
- (void)onRewind:(id)sender;
- (void)onZoomStep:(id)sender;
- (void)onZoomIn:(id)sender;
- (void)onZoomOut:(id)sender;
- (void)onModeInfo:(id)sender;
- (void)onModeSelectTeam:(id)sender;
- (void)onToggleUseTeamColours:(id)sender;
- (void)onToggleTankCentred:(id)sender;
- (void)onToggleSoundEffects:(id)sender;
- (void)onToggleDnsLookups:(id)sender;
- (void)onTeamColoursDialog:(id)sender;
- (void)onToggleControls:(id)sender;
- (void)onToggleEvents:(id)sender;
- (void)onToggleGameInfo:(id)sender;
- (void)onToggleItemInfo:(id)sender;
- (void)onToggleComments:(id)sender;
- (void)onResetWindows:(id)sender;
- (void)onHelp:(id)sender;
@end

@implementation LVMenuBridge
- (void)onQuit:(id)sender { (void)sender; lv_push_sdl_quit(); }
- (void)onAbout:(id)sender { (void)sender; lv_imgui_show_about_dialog(); }
- (void)onOpenWbn:(id)sender { (void)sender; lv_imgui_open_wbn_browser(); }
- (void)onOpenFile:(id)sender { (void)sender; lv_windowOpenFile(NULL); }
- (void)onSaveMap:(id)sender {
    (void)sender;
    if (s_lv_state && s_lv_state->isLoaded) lv_windowSaveMap();
}
- (void)onReturn:(id)sender { (void)sender; lv_push_sdl_quit(); }
- (void)onPlay:(id)sender {
    (void)sender;
    if (s_lv_state && s_lv_state->isLoaded && !s_lv_state->playIsPlaying) lv_windowPlay();
}
- (void)onPause:(id)sender {
    (void)sender;
    if (s_lv_state && s_lv_state->isLoaded && s_lv_state->playIsPlaying) lv_windowPause();
}
- (void)onStop:(id)sender {
    (void)sender;
    if (s_lv_state && s_lv_state->isLoaded) lv_windowStop(0);
}
- (void)onFastForward:(id)sender {
    (void)sender;
    if (s_lv_state && s_lv_state->isLoaded) lv_windowFastForward();
}
- (void)onRewind:(id)sender {
    (void)sender;
    if (s_lv_state && s_lv_state->isLoaded) lv_windowRewind();
}
- (void)onZoomStep:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    lv_imgui_zoom_at_center((int)[item tag]);
}
- (void)onZoomIn:(id)sender {
    (void)sender;
    lv_imgui_zoom_at_center(lv_drawGetZoomStepIndex() + 1);
}
- (void)onZoomOut:(id)sender {
    (void)sender;
    lv_imgui_zoom_at_center(lv_drawGetZoomStepIndex() - 1);
}
- (void)onModeInfo:(id)sender { (void)sender; lv_imgui_set_mode_information(1); }
- (void)onModeSelectTeam:(id)sender { (void)sender; lv_imgui_set_mode_information(0); }
- (void)onToggleUseTeamColours:(id)sender {
    (void)sender;
    if (s_lv_state && !s_lv_state->gameView) {
        s_lv_state->useTeamColours = s_lv_state->useTeamColours ? false : true;
    }
}
- (void)onToggleTankCentred:(id)sender { (void)sender; lv_imgui_toggle_tank_centred(); }
- (void)onToggleSoundEffects:(id)sender {
    (void)sender;
    if (s_lv_state) s_lv_state->isSoundsPlaying = s_lv_state->isSoundsPlaying ? false : true;
}
- (void)onToggleDnsLookups:(id)sender { (void)sender; lv_imgui_toggle_dns_lookups(); }
- (void)onTeamColoursDialog:(id)sender { (void)sender; lv_imgui_show_team_colours_dialog(); }
- (void)onToggleControls:(id)sender { (void)sender; lv_g_show_controls_window = !lv_g_show_controls_window; }
- (void)onToggleEvents:(id)sender { (void)sender; lv_g_show_events_window = !lv_g_show_events_window; }
- (void)onToggleGameInfo:(id)sender { (void)sender; lv_g_show_game_info_window = !lv_g_show_game_info_window; }
- (void)onToggleItemInfo:(id)sender { (void)sender; lv_g_show_item_info_window = !lv_g_show_item_info_window; }
- (void)onToggleComments:(id)sender { (void)sender; lv_g_show_comments_window = !lv_g_show_comments_window; }
- (void)onResetWindows:(id)sender { (void)sender; lv_g_reset_window_positions = true; }
- (void)onHelp:(id)sender { (void)sender; /* Phase 3 — Help is a placeholder */ }
@end

/* Build the standalone Log Viewer application menu. The first NSMenuItem
 * AppKit shows in the menu bar takes its title from the running app's
 * bundle name (Log Viewer.app), so an empty title here is correct. */
static void lv_buildStandaloneAppMenu(NSMenu *mainMenu) {
    NSMenuItem *appItem = [mainMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@""];
    [appItem setSubmenu:appMenu];

    NSMenuItem *aboutItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_ABOUT_LV)
        action:@selector(onAbout:)
        keyEquivalent:@""];
    [aboutItem setTarget:s_lv_bridge];
    [appMenu addItem:aboutItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_SERVICES)];
    NSMenuItem *servicesItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SERVICES) action:nil keyEquivalent:@""];
    [servicesItem setSubmenu:servicesMenu];
    [appMenu addItem:servicesItem];
    [NSApp setServicesMenu:servicesMenu];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *hideItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_HIDE_LV)
        action:@selector(hide:)
        keyEquivalent:@"h"];
    [appMenu addItem:hideItem];

    NSMenuItem *hideOthersItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_HIDE_OTHERS)
        action:@selector(hideOtherApplications:)
        keyEquivalent:@"h"];
    [hideOthersItem setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItem:hideOthersItem];

    NSMenuItem *showAllItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SHOW_ALL)
        action:@selector(unhideAllApplications:)
        keyEquivalent:@""];
    [appMenu addItem:showAllItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *quitItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_QUIT_LV)
        action:@selector(onQuit:)
        keyEquivalent:@"q"];
    [quitItem setTarget:s_lv_bridge];
    [appMenu addItem:quitItem];
}

/* Reuse the host's app menu in embedded mode by temporarily moving its
 * submenu into our new mainMenu. AppKit enforces a 1:1 NSMenuItem ↔
 * submenu binding (-setSubmenu: throws if the menu already has a
 * supermenu), so we detach from the host's first item first and stash
 * both pointers for restore. */
static void lv_reuseHostAppMenu(NSMenu *mainMenu, NSMenu *hostMain) {
    NSMenuItem *appItem = [mainMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenuItem *hostApp = (hostMain && [hostMain numberOfItems] > 0) ? [hostMain itemAtIndex:0] : nil;
    NSMenu *hostAppSub = [hostApp submenu];
    if (hostApp && hostAppSub) {
        [hostApp setSubmenu:nil];
        [appItem setSubmenu:hostAppSub];
        s_lv_hostAppItem    = hostApp;
        s_lv_hostAppSubmenu = hostAppSub;
    } else {
        /* Defensive: host had no app menu (shouldn't happen since AppKit
         * always reserves slot 0). Build an empty submenu so AppKit has
         * something to attach the bundle title to. */
        [appItem setSubmenu:[[NSMenu alloc] initWithTitle:@""]];
    }
}

void lv_mac_menubar_install(struct SDL_Window *win, struct LogViewerState *lvState) {
    (void)win;
    s_lv_state = lvState;

    if (s_lv_bridge == nil) {
        s_lv_bridge = [[LVMenuBridge alloc] init];
    }

    NSMenu *previousMain = [NSApp mainMenu];
    s_lv_savedMainMenu = previousMain;

    /* Embedded path is "host process running, host main menu installed".
     * Standalone path is "no host menu yet" (fresh app launch). */
    BOOL embedded = (previousMain != nil && [previousMain numberOfItems] > 0);

    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];

    if (embedded) {
        lv_reuseHostAppMenu(mainMenu, previousMain);
    } else {
        lv_buildStandaloneAppMenu(mainMenu);
    }

    /* File menu */
    NSMenuItem *fileItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_FILE) action:nil keyEquivalent:@""];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_FILE)];
    [fileItem setSubmenu:fileMenu];
    [fileMenu setAutoenablesItems:NO];

    NSMenuItem *openWbnItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_MENU_OPEN_WBN)
        action:@selector(onOpenWbn:)
        keyEquivalent:@""];
    [openWbnItem setTarget:s_lv_bridge];
    [fileMenu addItem:openWbnItem];

    NSMenuItem *openItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_MENU_OPEN)
        action:@selector(onOpenFile:)
        keyEquivalent:@"o"];
    [openItem setTarget:s_lv_bridge];
    [fileMenu addItem:openItem];

    NSMenuItem *saveMapItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SAVE_MAP)
        action:@selector(onSaveMap:)
        keyEquivalent:@"s"];
    [saveMapItem setTarget:s_lv_bridge];
    [fileMenu addItem:saveMapItem];
    s_lv_saveMapItem = saveMapItem;

    /* Embedded gets a "Return to Main Menu" entry in the File menu;
     * standalone gets no File-menu quit (Quit lives in the app menu). */
    if (embedded) {
        [fileMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *returnItem = [[NSMenuItem alloc]
            initWithTitle:LANG_STR(STR_MAPEDIT_MENU_RETURN)
            action:@selector(onReturn:)
            keyEquivalent:@""];
        [returnItem setTarget:s_lv_bridge];
        [fileMenu addItem:returnItem];
        s_lv_returnItem = returnItem;
    } else {
        s_lv_returnItem = nil;
    }

    /* Action menu */
    NSMenuItem *actionItem = [mainMenu addItemWithTitle:LANG_STR(STR_LV_MENU_ACTION) action:nil keyEquivalent:@""];
    NSMenu *actionMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_LV_MENU_ACTION)];
    [actionItem setSubmenu:actionMenu];
    [actionMenu setAutoenablesItems:NO];

    NSMenuItem *playItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_PLAY)
        action:@selector(onPlay:)
        keyEquivalent:@"p"];
    [playItem setTarget:s_lv_bridge];
    [actionMenu addItem:playItem];
    s_lv_playItem = playItem;

    NSMenuItem *pauseItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_PAUSE)
        action:@selector(onPause:)
        keyEquivalent:@"u"];
    [pauseItem setTarget:s_lv_bridge];
    [actionMenu addItem:pauseItem];
    s_lv_pauseItem = pauseItem;

    /* ⌘S is already bound to Save Map on Mac (the Mac convention).
     * Use ⌘. for Stop — the Mac convention for "stop the current
     * operation". The in-window ImGui menu keeps "Ctrl+S" for non-Mac. */
    NSMenuItem *stopItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_STOP)
        action:@selector(onStop:)
        keyEquivalent:@"."];
    [stopItem setTarget:s_lv_bridge];
    [actionMenu addItem:stopItem];
    s_lv_stopItem = stopItem;

    [actionMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *ffItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_FAST_FORWARD)
        action:@selector(onFastForward:)
        keyEquivalent:@"f"];
    [ffItem setTarget:s_lv_bridge];
    [actionMenu addItem:ffItem];
    s_lv_fastForwardItem = ffItem;

    NSMenuItem *rwItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_REWIND)
        action:@selector(onRewind:)
        keyEquivalent:@"r"];
    [rwItem setTarget:s_lv_bridge];
    [actionMenu addItem:rwItem];
    s_lv_rewindItem = rwItem;

    /* Options menu */
    NSMenuItem *optionsItem = [mainMenu addItemWithTitle:LANG_STR(STR_MAPEDIT_MENU_OPTIONS) action:nil keyEquivalent:@""];
    NSMenu *optionsMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MAPEDIT_MENU_OPTIONS)];
    [optionsItem setSubmenu:optionsMenu];
    [optionsMenu setAutoenablesItems:NO];

    /* Zoom submenu — per-step items are rebuilt on refresh when the step
     * count changes (it depends on the loaded log's preferences). */
    NSMenuItem *zoomRoot = [optionsMenu addItemWithTitle:LANG_STR(STR_LV_ZOOM) action:nil keyEquivalent:@""];
    NSMenu *zoomMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_LV_ZOOM)];
    [zoomRoot setSubmenu:zoomMenu];
    [zoomMenu setAutoenablesItems:NO];
    s_lv_zoomMenu = zoomMenu;

    NSMenuItem *zoomInItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_ZOOM_IN)
        action:@selector(onZoomIn:)
        keyEquivalent:@"+"];
    [zoomInItem setTarget:s_lv_bridge];
    s_lv_zoomInItem = zoomInItem;

    NSMenuItem *zoomOutItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_ZOOM_OUT)
        action:@selector(onZoomOut:)
        keyEquivalent:@"-"];
    [zoomOutItem setTarget:s_lv_bridge];
    s_lv_zoomOutItem = zoomOutItem;
    /* Items are added into the submenu by refresh after the step rebuild. */

    [optionsMenu addItem:[NSMenuItem separatorItem]];

    /* Mode submenu */
    NSMenuItem *modeRoot = [optionsMenu addItemWithTitle:LANG_STR(STR_LV_MODE) action:nil keyEquivalent:@""];
    NSMenu *modeMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_LV_MODE)];
    [modeRoot setSubmenu:modeMenu];
    [modeMenu setAutoenablesItems:NO];

    NSMenuItem *modeInfo = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_MODE_INFO)
        action:@selector(onModeInfo:)
        keyEquivalent:@"i"];
    [modeInfo setTarget:s_lv_bridge];
    [modeMenu addItem:modeInfo];
    s_lv_modeInfoItem = modeInfo;

    NSMenuItem *modeSelect = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_SELECT_TEAM)
        action:@selector(onModeSelectTeam:)
        keyEquivalent:@"c"];
    [modeSelect setTarget:s_lv_bridge];
    [modeMenu addItem:modeSelect];
    s_lv_modeSelectItem = modeSelect;

    NSMenuItem *useTeamColours = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_USE_TEAM_COLOURS)
        action:@selector(onToggleUseTeamColours:)
        keyEquivalent:@""];
    [useTeamColours setTarget:s_lv_bridge];
    [optionsMenu addItem:useTeamColours];
    s_lv_useTeamColoursItem = useTeamColours;

    NSMenuItem *tankCentred = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_TANK_CENTRED)
        action:@selector(onToggleTankCentred:)
        keyEquivalent:@"t"];
    [tankCentred setTarget:s_lv_bridge];
    [optionsMenu addItem:tankCentred];
    s_lv_tankCentredItem = tankCentred;

    NSMenuItem *soundEffects = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SOUND_EFFECTS)
        action:@selector(onToggleSoundEffects:)
        keyEquivalent:@""];
    [soundEffects setTarget:s_lv_bridge];
    [optionsMenu addItem:soundEffects];
    s_lv_soundEffectsItem = soundEffects;

    NSMenuItem *dnsLookups = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_DNS_LOOKUPS)
        action:@selector(onToggleDnsLookups:)
        keyEquivalent:@""];
    [dnsLookups setTarget:s_lv_bridge];
    [optionsMenu addItem:dnsLookups];
    s_lv_dnsLookupsItem = dnsLookups;

    [optionsMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *teamColoursDialog = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_TEAM_COLOURS)
        action:@selector(onTeamColoursDialog:)
        keyEquivalent:@""];
    [teamColoursDialog setTarget:s_lv_bridge];
    [optionsMenu addItem:teamColoursDialog];

    /* Windows menu — Log Viewer's panel toggles. Not [NSApp setWindowsMenu:]
     * since this isn't the standard Mac Window menu. */
    NSMenuItem *winRoot = [mainMenu addItemWithTitle:LANG_STR(STR_LV_MENU_WINDOWS) action:nil keyEquivalent:@""];
    NSMenu *winMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_LV_MENU_WINDOWS)];
    [winRoot setSubmenu:winMenu];
    [winMenu setAutoenablesItems:NO];

    NSMenuItem *winControls = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_WIN_CONTROLS)
        action:@selector(onToggleControls:)
        keyEquivalent:@"1"];
    [winControls setTarget:s_lv_bridge];
    [winMenu addItem:winControls];
    s_lv_winControlsItem = winControls;

    NSMenuItem *winEvents = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_WIN_EVENTS)
        action:@selector(onToggleEvents:)
        keyEquivalent:@"2"];
    [winEvents setTarget:s_lv_bridge];
    [winMenu addItem:winEvents];
    s_lv_winEventsItem = winEvents;

    NSMenuItem *winGameInfo = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_WIN_GAMEINFO)
        action:@selector(onToggleGameInfo:)
        keyEquivalent:@"3"];
    [winGameInfo setTarget:s_lv_bridge];
    [winMenu addItem:winGameInfo];
    s_lv_winGameInfoItem = winGameInfo;

    NSMenuItem *winItemInfo = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_WIN_ITEMINFO)
        action:@selector(onToggleItemInfo:)
        keyEquivalent:@"4"];
    [winItemInfo setTarget:s_lv_bridge];
    [winMenu addItem:winItemInfo];
    s_lv_winItemInfoItem = winItemInfo;

    NSMenuItem *winComments = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_WIN_COMMENTS)
        action:@selector(onToggleComments:)
        keyEquivalent:@"5"];
    [winComments setTarget:s_lv_bridge];
    [winMenu addItem:winComments];
    s_lv_winCommentsItem = winComments;

    [winMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *resetWindows = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_RESET_WINDOWS)
        action:@selector(onResetWindows:)
        keyEquivalent:@""];
    [resetWindows setTarget:s_lv_bridge];
    [winMenu addItem:resetWindows];

    /* Help menu — kept symmetric with the in-window ImGui menu so the
     * Win/Linux/Web rendering of this menu doesn't diverge. About is
     * also present in the standalone app menu via STR_MENU_ABOUT_LV. */
    NSMenuItem *helpRoot = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_HELP) action:nil keyEquivalent:@""];
    NSMenu *helpMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_HELP)];
    [helpRoot setSubmenu:helpMenu];
    [helpMenu setAutoenablesItems:NO];

    NSMenuItem *helpItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_HELP)
        action:@selector(onHelp:)
        keyEquivalent:@""];
    [helpItem setTarget:s_lv_bridge];
    [helpMenu addItem:helpItem];

    NSMenuItem *aboutItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_ABOUT)
        action:@selector(onAbout:)
        keyEquivalent:@""];
    [aboutItem setTarget:s_lv_bridge];
    [helpMenu addItem:aboutItem];

    /* Force the zoom submenu to be rebuilt on first refresh. */
    s_lv_zoomStepsBuilt = -1;

    [NSApp setMainMenu:mainMenu];
}

void lv_mac_menubar_uninstall(void) {
    /* Return the host's app submenu to its original NSMenuItem before we
     * tear down our mainMenu — AppKit needs the binding back to a single
     * owner. The detach-from-our-side happens implicitly when NSApp's
     * mainMenu changes (and our NSMenu is released), but doing it
     * explicitly avoids relying on that ordering. */
    if (s_lv_hostAppItem && s_lv_hostAppSubmenu) {
        NSMenu *lvMain = [NSApp mainMenu];
        if (lvMain && [lvMain numberOfItems] > 0) {
            [[lvMain itemAtIndex:0] setSubmenu:nil];
        }
        [s_lv_hostAppItem setSubmenu:s_lv_hostAppSubmenu];
        s_lv_hostAppItem    = nil;
        s_lv_hostAppSubmenu = nil;
    }

    if (s_lv_savedMainMenu) {
        [NSApp setMainMenu:s_lv_savedMainMenu];
    } else {
        /* Standalone exits the process right after this; an empty menu
         * stub keeps NSApp happy in case anything queries it. */
        [NSApp setMainMenu:[[NSMenu alloc] initWithTitle:@""]];
    }
    s_lv_savedMainMenu = nil;
    s_lv_state = NULL;
    s_lv_saveMapItem = nil;
    s_lv_returnItem = nil;
    s_lv_playItem = nil;
    s_lv_pauseItem = nil;
    s_lv_stopItem = nil;
    s_lv_fastForwardItem = nil;
    s_lv_rewindItem = nil;
    s_lv_zoomMenu = nil;
    s_lv_zoomInItem = nil;
    s_lv_zoomOutItem = nil;
    s_lv_modeInfoItem = nil;
    s_lv_modeSelectItem = nil;
    s_lv_useTeamColoursItem = nil;
    s_lv_tankCentredItem = nil;
    s_lv_soundEffectsItem = nil;
    s_lv_dnsLookupsItem = nil;
    s_lv_winControlsItem = nil;
    s_lv_winEventsItem = nil;
    s_lv_winGameInfoItem = nil;
    s_lv_winItemInfoItem = nil;
    s_lv_winCommentsItem = nil;
    s_lv_zoomStepsBuilt = -1;
}

void lv_mac_menubar_refresh(const struct LvMenuState *s) {
    if (!s) return;

    if (s_lv_saveMapItem) [s_lv_saveMapItem setEnabled:(s->isLoaded ? YES : NO)];

    if (s_lv_playItem)        [s_lv_playItem        setEnabled:((s->isLoaded && !s->playIsPlaying) ? YES : NO)];
    if (s_lv_pauseItem)       [s_lv_pauseItem       setEnabled:((s->isLoaded &&  s->playIsPlaying) ? YES : NO)];
    if (s_lv_stopItem)        [s_lv_stopItem        setEnabled:(s->isLoaded ? YES : NO)];
    if (s_lv_fastForwardItem) [s_lv_fastForwardItem setEnabled:(s->isLoaded ? YES : NO)];
    if (s_lv_rewindItem)      [s_lv_rewindItem      setEnabled:(s->isLoaded ? YES : NO)];

    /* Mode submenu checkmarks. */
    if (s_lv_modeInfoItem)   [s_lv_modeInfoItem   setState:(s->modeInformation ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_lv_modeSelectItem) [s_lv_modeSelectItem setState:(s->modeInformation ? NSControlStateValueOff : NSControlStateValueOn)];

    /* Use Team Colours is disabled while in gameView. */
    if (s_lv_useTeamColoursItem) {
        [s_lv_useTeamColoursItem setState:(s->useTeamColours ? NSControlStateValueOn : NSControlStateValueOff)];
        [s_lv_useTeamColoursItem setEnabled:(s->gameViewActive ? NO : YES)];
    }
    if (s_lv_tankCentredItem)  [s_lv_tankCentredItem  setState:(s->tankCentred  ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_lv_soundEffectsItem) [s_lv_soundEffectsItem setState:(s->soundEffects ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_lv_dnsLookupsItem)   [s_lv_dnsLookupsItem   setState:(s->dnsLookups   ? NSControlStateValueOn : NSControlStateValueOff)];

    if (s_lv_winControlsItem) [s_lv_winControlsItem setState:(s->showControls ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_lv_winEventsItem)   [s_lv_winEventsItem   setState:(s->showEvents   ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_lv_winGameInfoItem) [s_lv_winGameInfoItem setState:(s->showGameInfo ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_lv_winItemInfoItem) [s_lv_winItemInfoItem setState:(s->showItemInfo ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_lv_winCommentsItem) [s_lv_winCommentsItem setState:(s->showComments ? NSControlStateValueOn : NSControlStateValueOff)];

    /* Zoom submenu — rebuild per-step items when the step count changes,
     * then mirror checkmarks every frame so wheel-zoom updates land. */
    if (s_lv_zoomMenu && s->zoomStepCount > 0) {
        if (s_lv_zoomStepsBuilt != s->zoomStepCount) {
            [s_lv_zoomMenu removeAllItems];
            for (int i = 0; i < s->zoomStepCount; i++) {
                float val = lv_drawGetZoomStepValue(i);
                NSString *label;
                if (val == (float)(int)val) {
                    label = [NSString stringWithFormat:@"%dx", (int)val];
                } else {
                    label = [NSString stringWithFormat:@"%.1fx", val];
                }
                NSMenuItem *it = [[NSMenuItem alloc]
                    initWithTitle:label
                    action:@selector(onZoomStep:)
                    keyEquivalent:@""];
                [it setTarget:s_lv_bridge];
                [it setTag:i];
                [s_lv_zoomMenu addItem:it];
            }
            [s_lv_zoomMenu addItem:[NSMenuItem separatorItem]];
            if (s_lv_zoomInItem)  [s_lv_zoomMenu addItem:s_lv_zoomInItem];
            if (s_lv_zoomOutItem) [s_lv_zoomMenu addItem:s_lv_zoomOutItem];
            s_lv_zoomStepsBuilt = s->zoomStepCount;
        }
        for (NSMenuItem *it in [s_lv_zoomMenu itemArray]) {
            if (it.action != @selector(onZoomStep:)) continue;
            [it setState:((it.tag == s->zoomStepIndex) ? NSControlStateValueOn : NSControlStateValueOff)];
        }
        if (s_lv_zoomInItem)  [s_lv_zoomInItem  setEnabled:((s->zoomStepIndex < s->zoomStepCount - 1) ? YES : NO)];
        if (s_lv_zoomOutItem) [s_lv_zoomOutItem setEnabled:((s->zoomStepIndex > 0) ? YES : NO)];
    }
}
