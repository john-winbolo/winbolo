#import <Cocoa/Cocoa.h>

#include <SDL3/SDL.h>

#include "mac_menubar.h"

/* Declared in sdl3imgui.cpp. Avoid pulling that header in here so this
 * compilation unit stays narrow. */
extern "C" void windowSetQuitting(void);

@interface WBMenuBridge : NSObject
- (void)onQuit:(id)sender;
@end

@implementation WBMenuBridge
- (void)onQuit:(id)sender {
    (void)sender;
    windowSetQuitting();
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

    NSMenuItem *quitItem = [[NSMenuItem alloc]
        initWithTitle:@"Quit WinBolo"
        action:@selector(onQuit:)
        keyEquivalent:@"q"];
    [quitItem setTarget:g_bridge];
    [appMenu addItem:quitItem];

    [NSApp setMainMenu:mainMenu];
}
