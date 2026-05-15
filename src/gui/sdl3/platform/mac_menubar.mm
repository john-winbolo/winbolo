#import <Cocoa/Cocoa.h>

#include <SDL3/SDL.h>

#include "mac_menubar.h"

/* Declared in sdl3imgui.cpp. Avoid pulling that header in here so this
 * compilation unit stays narrow. */
extern "C" void windowSetQuitting(void);
extern "C" void sdl3ImguiShowAbout(void);
extern "C" void sdl3ImguiShowSettings(void);

@interface WBMenuBridge : NSObject
- (void)onQuit:(id)sender;
- (void)onAbout:(id)sender;
- (void)onPreferences:(id)sender;
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
@end

static WBMenuBridge *g_bridge = nil;
static void *g_clientSim = NULL;

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

    [NSApp setMainMenu:mainMenu];
}
