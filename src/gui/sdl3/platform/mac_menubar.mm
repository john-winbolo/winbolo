#import <Cocoa/Cocoa.h>

#include <SDL3/SDL.h>

#include "mac_menubar.h"
#include "../../lang.h"

/* Declared in sdl3imgui.cpp. Avoid pulling that header in here so this
 * compilation unit stays narrow. */
extern "C" void windowSetQuitting(void);
extern "C" void sdl3ImguiShowAbout(void);
extern "C" void sdl3ImguiShowSettings(void);

struct ClientSim;
extern "C" void windowNewGame(void);
extern "C" void windowSaveMap(struct ClientSim *cs);
extern "C" void sdl3ImguiShowGameInfo(bool open);
extern "C" void sdl3ImguiShowSysInfo(bool open);
extern "C" void sdl3ImguiShowNetInfo(bool open);

extern "C" void windowSmoothScrolling_toggle(void);
extern "C" void windowAutomaticScrolling_toggle(struct ClientSim *cs);
extern "C" void windowShowGunsight_toggle(struct ClientSim *cs);
extern "C" void windowLabelOwnTank_toggle(struct ClientSim *cs);
extern "C" void windowShowPillLabels_toggle(struct ClientSim *cs);
extern "C" void windowShowBaseLabels_toggle(struct ClientSim *cs);
extern "C" void windowHideMainView_toggle(void);

extern "C" void sdl3ImguiSetFrameRate(int rate);
extern "C" void sdl3ImguiSetZoom(int zoom);
extern "C" void sdl3ImguiSetMessageLabelLen(struct ClientSim *cs, int len);
extern "C" void sdl3ImguiSetTankLabelLen(struct ClientSim *cs, int len);
extern "C" void sdl3ImguiCycleDevicePreset(void);

#define LANG_STR(id) ([NSString stringWithUTF8String:langGetText(id)])

@class WBMenuBridge;
static WBMenuBridge *g_bridge = nil;
static void *g_clientSim = NULL;

@interface WBMenuBridge : NSObject
- (void)onQuit:(id)sender;
- (void)onAbout:(id)sender;
- (void)onPreferences:(id)sender;
- (void)onNewGame:(id)sender;
- (void)onSaveMap:(id)sender;
- (void)onShowGameInfo:(id)sender;
- (void)onShowSysInfo:(id)sender;
- (void)onShowNetInfo:(id)sender;
- (void)onSmoothScrolling:(id)sender;
- (void)onAutoScrolling:(id)sender;
- (void)onShowGunsight:(id)sender;
- (void)onLabelOwnTank:(id)sender;
- (void)onPillboxLabels:(id)sender;
- (void)onBaseLabels:(id)sender;
- (void)onHideMainView:(id)sender;
- (void)onCycleDevice:(id)sender;
- (void)onSetFrameRate:(id)sender;
- (void)onSetZoom:(id)sender;
- (void)onSetMessageLabel:(id)sender;
- (void)onSetTankLabel:(id)sender;
@end

@implementation WBMenuBridge
- (void)onQuit:(id)sender {
    (void)sender;
    windowSetQuitting();
}
- (void)onAbout:(id)sender {
    (void)sender;
    sdl3ImguiShowAbout();
}
- (void)onPreferences:(id)sender {
    (void)sender;
    sdl3ImguiShowSettings();
}
- (void)onNewGame:(id)sender {
    (void)sender;
    windowNewGame();
}
- (void)onSaveMap:(id)sender {
    (void)sender;
    if (g_clientSim) windowSaveMap((struct ClientSim *)g_clientSim);
}
- (void)onShowGameInfo:(id)sender {
    (void)sender;
    sdl3ImguiShowGameInfo(true);
}
- (void)onShowSysInfo:(id)sender {
    (void)sender;
    sdl3ImguiShowSysInfo(true);
}
- (void)onShowNetInfo:(id)sender {
    (void)sender;
    sdl3ImguiShowNetInfo(true);
}
- (void)onSmoothScrolling:(id)sender {
    (void)sender;
    windowSmoothScrolling_toggle();
}
- (void)onAutoScrolling:(id)sender {
    (void)sender;
    if (g_clientSim) windowAutomaticScrolling_toggle((struct ClientSim *)g_clientSim);
}
- (void)onShowGunsight:(id)sender {
    (void)sender;
    if (g_clientSim) windowShowGunsight_toggle((struct ClientSim *)g_clientSim);
}
- (void)onLabelOwnTank:(id)sender {
    (void)sender;
    if (g_clientSim) windowLabelOwnTank_toggle((struct ClientSim *)g_clientSim);
}
- (void)onPillboxLabels:(id)sender {
    (void)sender;
    if (g_clientSim) windowShowPillLabels_toggle((struct ClientSim *)g_clientSim);
}
- (void)onBaseLabels:(id)sender {
    (void)sender;
    if (g_clientSim) windowShowBaseLabels_toggle((struct ClientSim *)g_clientSim);
}
- (void)onHideMainView:(id)sender {
    (void)sender;
    windowHideMainView_toggle();
}
- (void)onCycleDevice:(id)sender {
    (void)sender;
    sdl3ImguiCycleDevicePreset();
}
- (void)onSetFrameRate:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    sdl3ImguiSetFrameRate((int)[item tag]);
}
- (void)onSetZoom:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    sdl3ImguiSetZoom((int)[item tag]);
}
- (void)onSetMessageLabel:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    if (g_clientSim) sdl3ImguiSetMessageLabelLen((struct ClientSim *)g_clientSim, (int)[item tag]);
}
- (void)onSetTankLabel:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    if (g_clientSim) sdl3ImguiSetTankLabelLen((struct ClientSim *)g_clientSim, (int)[item tag]);
}
@end

void mac_menubar_install(struct SDL_Window *win, void *clientSim) {
    (void)win;
    g_clientSim = clientSim;

    if (g_bridge == nil) {
        g_bridge = [[WBMenuBridge alloc] init];
    }

    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];

    /* Application menu — AppKit treats the first submenu specially and
     * draws its title in bold using the running app's name regardless of
     * what we set here. */
    NSMenuItem *appItem = [mainMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@""];
    [appItem setSubmenu:appMenu];

    NSMenuItem *aboutItem = [[NSMenuItem alloc]
        initWithTitle:@"About WinBolo"
        action:@selector(onAbout:)
        keyEquivalent:@""];
    [aboutItem setTarget:g_bridge];
    [appMenu addItem:aboutItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *prefsItem = [[NSMenuItem alloc]
        initWithTitle:@"Preferences…"
        action:@selector(onPreferences:)
        keyEquivalent:@","];
    [prefsItem setTarget:g_bridge];
    [appMenu addItem:prefsItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    NSMenuItem *servicesItem = [[NSMenuItem alloc]
        initWithTitle:@"Services" action:nil keyEquivalent:@""];
    [servicesItem setSubmenu:servicesMenu];
    [appMenu addItem:servicesItem];
    [NSApp setServicesMenu:servicesMenu];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *hideItem = [[NSMenuItem alloc]
        initWithTitle:@"Hide WinBolo"
        action:@selector(hide:)
        keyEquivalent:@"h"];
    [appMenu addItem:hideItem];

    NSMenuItem *hideOthersItem = [[NSMenuItem alloc]
        initWithTitle:@"Hide Others"
        action:@selector(hideOtherApplications:)
        keyEquivalent:@"h"];
    [hideOthersItem setKeyEquivalentModifierMask:NSEventModifierFlagOption | NSEventModifierFlagCommand];
    [appMenu addItem:hideOthersItem];

    NSMenuItem *showAllItem = [[NSMenuItem alloc]
        initWithTitle:@"Show All"
        action:@selector(unhideAllApplications:)
        keyEquivalent:@""];
    [appMenu addItem:showAllItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *quitItem = [[NSMenuItem alloc]
        initWithTitle:@"Quit WinBolo"
        action:@selector(onQuit:)
        keyEquivalent:@"q"];
    [quitItem setTarget:g_bridge];
    [appMenu addItem:quitItem];

    /* File menu — game lifecycle and pop-out info windows. */
    NSMenuItem *fileItem = [mainMenu addItemWithTitle:@"File" action:nil keyEquivalent:@""];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    [fileItem setSubmenu:fileMenu];

    NSMenuItem *newGameItem = [[NSMenuItem alloc]
        initWithTitle:@"New Game"
        action:@selector(onNewGame:)
        keyEquivalent:@""];
    [newGameItem setTarget:g_bridge];
    [fileMenu addItem:newGameItem];

    NSMenuItem *saveMapItem = [[NSMenuItem alloc]
        initWithTitle:@"Save Map"
        action:@selector(onSaveMap:)
        keyEquivalent:@"s"];
    [saveMapItem setTarget:g_bridge];
    [fileMenu addItem:saveMapItem];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *gameInfoItem = [[NSMenuItem alloc]
        initWithTitle:@"Game Info"
        action:@selector(onShowGameInfo:)
        keyEquivalent:@""];
    [gameInfoItem setTarget:g_bridge];
    [fileMenu addItem:gameInfoItem];

    NSMenuItem *sysInfoItem = [[NSMenuItem alloc]
        initWithTitle:@"System Info"
        action:@selector(onShowSysInfo:)
        keyEquivalent:@""];
    [sysInfoItem setTarget:g_bridge];
    [fileMenu addItem:sysInfoItem];

    NSMenuItem *netInfoItem = [[NSMenuItem alloc]
        initWithTitle:@"Network Info"
        action:@selector(onShowNetInfo:)
        keyEquivalent:@""];
    [netInfoItem setTarget:g_bridge];
    [fileMenu addItem:netInfoItem];

    /* Edit menu — mirrors the ImGui Edit menu (renderMenuBar() in
     * sdl3imgui.cpp). Titles are localized via langGetText so the native
     * menu tracks the in-window menu across all 17 supported languages.
     * State sync (checkmarks / disabled state) is intentionally not done
     * here — that lands in a later phase. */
    NSMenuItem *editItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_EDIT) action:nil keyEquivalent:@""];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_EDIT)];
    [editItem setSubmenu:editMenu];

    /* Frame Rate submenu — tag values are the FRAME_RATE_* macro values
     * from src/gui/winbolo.h (timer-tick periods, not literal Hz). */
    NSMenuItem *frameRateRoot = [editMenu addItemWithTitle:LANG_STR(STR_MENU_FRAME_RATE) action:nil keyEquivalent:@""];
    NSMenu *frameRateMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_FRAME_RATE)];
    [frameRateRoot setSubmenu:frameRateMenu];

    NSMenuItem *fr60 = [[NSMenuItem alloc] initWithTitle:@"60" action:@selector(onSetFrameRate:) keyEquivalent:@""];
    [fr60 setTarget:g_bridge]; [fr60 setTag:111]; /* FRAME_RATE_60 */
    [frameRateMenu addItem:fr60];

    NSMenuItem *fr50 = [[NSMenuItem alloc] initWithTitle:@"50" action:@selector(onSetFrameRate:) keyEquivalent:@""];
    [fr50 setTarget:g_bridge]; [fr50 setTag:82]; /* FRAME_RATE_50 */
    [frameRateMenu addItem:fr50];

    NSMenuItem *fr30 = [[NSMenuItem alloc] initWithTitle:@"30" action:@selector(onSetFrameRate:) keyEquivalent:@""];
    [fr30 setTarget:g_bridge]; [fr30 setTag:55]; /* FRAME_RATE_30 */
    [frameRateMenu addItem:fr30];

    NSMenuItem *fr20 = [[NSMenuItem alloc] initWithTitle:@"20" action:@selector(onSetFrameRate:) keyEquivalent:@""];
    [fr20 setTarget:g_bridge]; [fr20 setTag:37]; /* FRAME_RATE_20 */
    [frameRateMenu addItem:fr20];

    NSMenuItem *fr15 = [[NSMenuItem alloc] initWithTitle:@"15" action:@selector(onSetFrameRate:) keyEquivalent:@""];
    [fr15 setTarget:g_bridge]; [fr15 setTag:28]; /* FRAME_RATE_15 */
    [frameRateMenu addItem:fr15];

    NSMenuItem *fr12 = [[NSMenuItem alloc] initWithTitle:@"12" action:@selector(onSetFrameRate:) keyEquivalent:@""];
    [fr12 setTarget:g_bridge]; [fr12 setTag:23]; /* FRAME_RATE_12 */
    [frameRateMenu addItem:fr12];

    NSMenuItem *fr10 = [[NSMenuItem alloc] initWithTitle:@"10" action:@selector(onSetFrameRate:) keyEquivalent:@""];
    [fr10 setTarget:g_bridge]; [fr10 setTag:19]; /* FRAME_RATE_10 */
    [frameRateMenu addItem:fr10];

    /* Window Size submenu — tag values are ZOOM_FACTOR_* macros. */
    NSMenuItem *windowSizeRoot = [editMenu addItemWithTitle:LANG_STR(STR_MENU_WINDOW_SIZE) action:nil keyEquivalent:@""];
    NSMenu *windowSizeMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_WINDOW_SIZE)];
    [windowSizeRoot setSubmenu:windowSizeMenu];

    NSMenuItem *zoomNormal = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_MENU_NORMAL) action:@selector(onSetZoom:) keyEquivalent:@""];
    [zoomNormal setTarget:g_bridge]; [zoomNormal setTag:1]; /* ZOOM_FACTOR_NORMAL */
    [windowSizeMenu addItem:zoomNormal];

    NSMenuItem *zoomDouble = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_MENU_DOUBLE) action:@selector(onSetZoom:) keyEquivalent:@""];
    [zoomDouble setTarget:g_bridge]; [zoomDouble setTag:2]; /* ZOOM_FACTOR_DOUBLE */
    [windowSizeMenu addItem:zoomDouble];

    NSMenuItem *zoomTriple = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_MENU_TRIPLE) action:@selector(onSetZoom:) keyEquivalent:@""];
    [zoomTriple setTarget:g_bridge]; [zoomTriple setTag:3]; /* ZOOM_FACTOR_TRIPLE */
    [windowSizeMenu addItem:zoomTriple];

    NSMenuItem *zoomQuad = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_MENU_QUAD) action:@selector(onSetZoom:) keyEquivalent:@""];
    [zoomQuad setTarget:g_bridge]; [zoomQuad setTag:4]; /* ZOOM_FACTOR_QUAD */
    [windowSizeMenu addItem:zoomQuad];

    [windowSizeMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *zoomCustom = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_MENU_CUSTOM_RESIZABLE) action:@selector(onSetZoom:) keyEquivalent:@""];
    [zoomCustom setTarget:g_bridge]; [zoomCustom setTag:0]; /* ZOOM_FACTOR_CUSTOM */
    [windowSizeMenu addItem:zoomCustom];

    /* Smooth Scrolling — single toggle, no shortcut. */
    NSMenuItem *smoothScrollItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SMOOTH_SCROLLING)
        action:@selector(onSmoothScrolling:)
        keyEquivalent:@""];
    [smoothScrollItem setTarget:g_bridge];
    [editMenu addItem:smoothScrollItem];

    [editMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *autoScrollItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_AUTO_SCROLLING)
        action:@selector(onAutoScrolling:)
        keyEquivalent:@"a"];
    [autoScrollItem setTarget:g_bridge];
    [editMenu addItem:autoScrollItem];

    NSMenuItem *showGunsightItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SHOW_GUNSIGHT)
        action:@selector(onShowGunsight:)
        keyEquivalent:@"g"];
    [showGunsightItem setTarget:g_bridge];
    [editMenu addItem:showGunsightItem];

    /* Message Labels submenu — tag values are labelLen enum members. */
    NSMenuItem *msgLabelsRoot = [editMenu addItemWithTitle:LANG_STR(STR_MENU_MSG_NAMES_SUB) action:nil keyEquivalent:@""];
    NSMenu *msgLabelsMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_MSG_NAMES_SUB)];
    [msgLabelsRoot setSubmenu:msgLabelsMenu];

    NSMenuItem *msgShort = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_SHORT) action:@selector(onSetMessageLabel:) keyEquivalent:@""];
    [msgShort setTarget:g_bridge]; [msgShort setTag:1]; /* lblShort */
    [msgLabelsMenu addItem:msgShort];

    NSMenuItem *msgLong = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_LONG) action:@selector(onSetMessageLabel:) keyEquivalent:@""];
    [msgLong setTarget:g_bridge]; [msgLong setTag:2]; /* lblLong */
    [msgLabelsMenu addItem:msgLong];

    /* Tank Labels submenu — Cmd+1/2/3 select length; trailing item toggles
     * own-tank label visibility (different action, no tag). */
    NSMenuItem *tankLabelsRoot = [editMenu addItemWithTitle:LANG_STR(STR_MENU_TANK_LABELS_SUB) action:nil keyEquivalent:@""];
    NSMenu *tankLabelsMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_TANK_LABELS_SUB)];
    [tankLabelsRoot setSubmenu:tankLabelsMenu];

    NSMenuItem *tankNone = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_NONE) action:@selector(onSetTankLabel:) keyEquivalent:@"1"];
    [tankNone setTarget:g_bridge]; [tankNone setTag:0]; /* lblNone */
    [tankLabelsMenu addItem:tankNone];

    NSMenuItem *tankShort = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_SHORT) action:@selector(onSetTankLabel:) keyEquivalent:@"2"];
    [tankShort setTarget:g_bridge]; [tankShort setTag:1]; /* lblShort */
    [tankLabelsMenu addItem:tankShort];

    NSMenuItem *tankLong = [[NSMenuItem alloc] initWithTitle:LANG_STR(STR_LONG) action:@selector(onSetTankLabel:) keyEquivalent:@"3"];
    [tankLong setTarget:g_bridge]; [tankLong setTag:2]; /* lblLong */
    [tankLabelsMenu addItem:tankLong];

    [tankLabelsMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *noOwnLabel = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_NO_OWN_LABEL)
        action:@selector(onLabelOwnTank:)
        keyEquivalent:@""];
    [noOwnLabel setTarget:g_bridge];
    [tankLabelsMenu addItem:noOwnLabel];

    NSMenuItem *pillLabelsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_PILLBOX_LABELS)
        action:@selector(onPillboxLabels:)
        keyEquivalent:@"p"];
    [pillLabelsItem setTarget:g_bridge];
    [editMenu addItem:pillLabelsItem];

    NSMenuItem *baseLabelsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_BASE_LABELS)
        action:@selector(onBaseLabels:)
        keyEquivalent:@"b"];
    [baseLabelsItem setTarget:g_bridge];
    [editMenu addItem:baseLabelsItem];

    [editMenu addItem:[NSMenuItem separatorItem]];

    /* Hide Main View — no keyEquivalent on macOS; Cmd+H is owned by
     * App > Hide WinBolo per the earlier collision-resolution decision.
     * The SDL_SCANCODE_H handler in sdl3imgui.cpp still serves
     * Windows/Linux/Web. */
    NSMenuItem *hideMainViewItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_HIDE_MAIN)
        action:@selector(onHideMainView:)
        keyEquivalent:@""];
    [hideMainViewItem setTarget:g_bridge];
    [editMenu addItem:hideMainViewItem];

    [editMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *deviceItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_DEVICE)
        action:@selector(onCycleDevice:)
        keyEquivalent:@"t"];
    [deviceItem setTarget:g_bridge];
    [editMenu addItem:deviceItem];

    /* Window menu — items dispatched through the responder chain to the
     * key NSWindow; no explicit targets. */
    NSMenuItem *windowItem = [mainMenu addItemWithTitle:@"Window" action:nil keyEquivalent:@""];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowItem setSubmenu:windowMenu];

    NSMenuItem *minimizeItem = [[NSMenuItem alloc]
        initWithTitle:@"Minimize"
        action:@selector(performMiniaturize:)
        keyEquivalent:@"m"];
    [windowMenu addItem:minimizeItem];

    NSMenuItem *zoomItem = [[NSMenuItem alloc]
        initWithTitle:@"Zoom"
        action:@selector(performZoom:)
        keyEquivalent:@""];
    [windowMenu addItem:zoomItem];

    [windowMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *fullScreenItem = [[NSMenuItem alloc]
        initWithTitle:@"Enter Full Screen"
        action:@selector(toggleFullScreen:)
        keyEquivalent:@"f"];
    [fullScreenItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagControl];
    [windowMenu addItem:fullScreenItem];

    [windowMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *bringAllToFrontItem = [[NSMenuItem alloc]
        initWithTitle:@"Bring All to Front"
        action:@selector(arrangeInFront:)
        keyEquivalent:@""];
    [windowMenu addItem:bringAllToFrontItem];

    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:mainMenu];
}

void mac_menubar_set_clientsim(void *clientSim) {
    g_clientSim = clientSim;
}
