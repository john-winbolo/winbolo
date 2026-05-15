#import <Cocoa/Cocoa.h>

#include <SDL3/SDL.h>

#include "mac_menubar.h"

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
