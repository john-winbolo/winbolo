/* SPDX-License-Identifier: GPL-3.0-or-later */
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <SDL3/SDL.h>

#include "mac_menubar.h"
#include "../../lang.h"

/* PingBand only. Unlike ptype (matched numerically below to keep engine
 * headers out of this TU), the band names are the point: the whole reason
 * the band is plumbed through MacPlayerSlot is so this row stops deriving
 * its own thresholds and keeps the hysteresis. ping_display.h is a leaf —
 * stdbool/stdint and nothing else — and declares its own C linkage. */
#include "ping_display.h"

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
extern "C" bool sdl3ImguiIsSysInfoOpen(void);
extern "C" bool sdl3ImguiIsNetInfoOpen(void);
extern "C" bool sdl3ImguiIsGameInfoOpen(void);
extern "C" bool sdl3ImguiIsSendMsgOpen(void);

/* Map Overview: the desktop pop-out and its in-window twin. */
extern "C" void sdl3ImguiShowMapOverview(bool open);
extern "C" bool sdl3ImguiIsMapOverviewOpen(void);
extern "C" void sdl3ImguiShowOverviewInWindow(bool active);
extern "C" bool sdl3ImguiIsOverviewInWindowOpen(void);
/* Window > Enter Full Screen. The frontend decides which of the two toggles
 * that is — the full screen map in a game, the app full screen flag outside
 * one — so this item and Alt+Enter can never mean different things. */
extern "C" void sdl3ImguiToggleFullScreen(struct ClientSim *cs);

extern "C" void windowSmoothScrolling_toggle(void);
extern "C" void windowAutomaticScrolling_toggle(struct ClientSim *cs);
extern "C" void windowShowGunsight_toggle(struct ClientSim *cs);
extern "C" void windowLabelOwnTank_toggle(struct ClientSim *cs);
extern "C" void windowShowPillLabels_toggle(struct ClientSim *cs);
extern "C" void windowShowBaseLabels_toggle(struct ClientSim *cs);

extern "C" void sdl3ImguiSetFrameRate(int rate);
extern "C" void sdl3ImguiSetZoom(int zoom);
extern "C" void sdl3ImguiSetMessageLabelLen(struct ClientSim *cs, int len);
extern "C" void sdl3ImguiSetTankLabelLen(struct ClientSim *cs, int len);

extern "C" void windowMenuAllowNewPlayers_toggle(struct ClientSim *cs);
extern "C" void windowSoundEffects_toggle(void);
extern "C" void windowBackgroundSoundChange_toggle(void);
extern "C" void windowSoundKeepalive(void);
extern "C" void windowSetSoundVolume(int pct);
extern "C" void windowMenuNewswire_toggle(struct ClientSim *cs);
extern "C" void windowMenuAssistant_toggle(struct ClientSim *cs);
extern "C" void windowMenuAI_toggle(struct ClientSim *cs);
extern "C" void windowMenuNetwork_toggle(struct ClientSim *cs);
extern "C" void windowMenuNetworkDebug_toggle(struct ClientSim *cs);
extern "C" void clientSimRequestAllianceSelected(struct ClientSim *cs);
extern "C" void clientSimLeaveAllianceSelf(struct ClientSim *cs);
extern "C" void sdl3ImguiNoteAllianceRequested(void);
extern "C" void clientSimNetSendGameVoteToggle(struct ClientSim *cs,
                                               unsigned char kind,
                                               unsigned char toggleMode);
extern "C" void clientSimSetGameVoteWidgetVisible(struct ClientSim *cs,
                                                  unsigned char kind, bool visible);

extern "C" void sdl3ImguiShowKeySetup(void);
extern "C" void sdl3ImguiShowChangeName(void);

extern "C" void sdl3ImguiShowSendMsg(bool open);
extern "C" void sdl3ImguiShowPlayersPanel(bool open);
extern "C" void sdl3ImguiTogglePlayersPanel(void);
extern "C" void sdl3ImguiSendMsgShortcut(void);
extern "C" void clientSimCheckAllNonePlayers(struct ClientSim *cs, bool check);
extern "C" void clientSimCheckAlliedPlayers(struct ClientSim *cs);
extern "C" void clientSimCheckNearbyPlayers(struct ClientSim *cs);
extern "C" void clientSimTogglePlayerCheckState(struct ClientSim *cs, unsigned char playerNum);

extern "C" void sdl3ImguiStopBrain(void);
extern "C" void sdl3ImguiStartBrain(int idx, struct ClientSim *cs);
extern "C" void sdl3ImguiShowBrainSettings(void);

extern "C" {
#include "../../gamefront.h"
}

#define LANG_STR(id) ([NSString stringWithUTF8String:langGetText(id)])

@class WBMenuBridge;
static WBMenuBridge *g_bridge = nil;
static void *g_clientSim = NULL;

/* Cached menu / item pointers populated during mac_menubar_install() and
 * walked by mac_menubar_refresh() to mirror in-window state. Submenus
 * cache the NSMenu so the refresh can iterate items by tag rather than
 * caching one pointer per item. */
static NSMenu     *s_frameRateMenu           = nil;
static NSMenu     *s_windowSizeMenu          = nil;
static NSMenu     *s_messageLabelsMenu       = nil;
static NSMenu     *s_tankLabelsMenu          = nil;
static NSMenuItem *s_smoothScrollingItem     = nil;
static NSMenuItem *s_autoScrollingItem       = nil;
static NSMenuItem *s_showGunsightItem        = nil;
static NSMenuItem *s_pillLabelsItem          = nil;
static NSMenuItem *s_baseLabelsItem          = nil;
static NSMenuItem *s_noOwnLabelItem          = nil;
static NSMenuItem *s_allowNewPlayersItem     = nil;
static NSMenuItem *s_soundEffectsItem        = nil;
static NSMenuItem *s_backgroundSoundItem     = nil;
static NSMenu     *s_volumeMenu              = nil;
static NSMenuItem *s_soundKeepaliveItem      = nil;
static NSMenuItem *s_newswireMessagesItem    = nil;
static NSMenuItem *s_assistantMessagesItem   = nil;
static NSMenuItem *s_aiMessagesItem          = nil;
static NSMenuItem *s_networkStatusMessagesItem = nil;
static NSMenuItem *s_networkDebugMessagesItem  = nil;
static NSMenuItem *s_sysInfoItem             = nil;
static NSMenuItem *s_netInfoItem             = nil;
static NSMenuItem *s_gameInfoItem            = nil;
static NSMenuItem *s_sendMsgItem             = nil;
static NSMenuItem *s_playersPanelItem        = nil;
static NSMenuItem *s_mapOverviewItem         = nil;
static NSMenuItem *s_overviewInWindowItem    = nil;
static NSMenuItem *s_fullScreenItem          = nil;
static NSMenuItem *s_winboloRequestAllianceItem = nil;
static NSMenuItem *s_winboloLeaveAllianceItem   = nil;
static NSMenuItem *s_playersRequestAllianceItem = nil;
static NSMenuItem *s_playersLeaveAllianceItem   = nil;
static NSMenuItem *s_playersVoteBackToLobbyItem = nil;
static NSMenuItem *s_playersVoteSurrenderItem   = nil;

/* Brains menu — the parent item drives enable-gating on aiActive, and the
 * submenu is rebuilt whenever brainCount or brainSettingsShown changes.
 * The Manual item is permanent at index 0; the Settings item is alloced
 * once but only attached when a Lua brain is running. */
static NSMenuItem *s_brainsParentItem  = nil;
static NSMenu     *s_brainsMenu        = nil;
static NSMenuItem *s_brainManualItem   = nil;
static NSMenuItem *s_brainSettingsItem = nil;
static int  s_lastBrainCount           = -1;   /* force rebuild on first refresh */
static BOOL s_lastBrainSettingsShown   = NO;

/* Per-slot NSMenuItem + custom view caches for the rich player rows in
 * the Players menu. Items are created up-front in mac_menubar_install();
 * views are lazily allocated the first time a slot becomes occupied. */
@class WBPlayerSlotView;
static NSMenuItem      *s_playerSlotItems[16] = {nil};
static WBPlayerSlotView *s_playerSlotViews[16] = {nil};

/* Lazily-populated cache of NSImages loaded from data/<subdir>/<name>.svg
 * (NSImage on macOS 11+ reads SVG natively). Base entries are keyed
 * "<subdir>/<name>"; tinted-ui variants pin an extra "#R,G,B,A" suffix so
 * a mode flip (labelColor changes from black-ish to white-ish) lands in
 * a different cache slot and the previous entry is GC'd by NSCache. */
static NSCache<NSString *, NSImage *> *g_iconCache = nil;

static NSImage *macMenubarLoadSvg(NSString *subdir, NSString *basename) {
    if (!subdir || !basename) return nil;
    if (!g_iconCache) g_iconCache = [[NSCache alloc] init];
    NSString *key = [NSString stringWithFormat:@"%@/%@", subdir, basename];
    NSImage *cached = [g_iconCache objectForKey:key];
    if (cached) return cached;
    NSString *resPath = [[NSBundle mainBundle] resourcePath];
    if (!resPath) return nil;
    NSString *path = [NSString stringWithFormat:@"%@/data/%@/%@.svg", resPath, subdir, basename];
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:path];
    if (img) [g_iconCache setObject:img forKey:key];
    return img;
}

static NSImage *macMenubarFlagIcon(const char *countryCode) {
    NSString *base = nil;
    if (countryCode && countryCode[0] != '\0') {
        char buf[3] = { countryCode[0], (countryCode[1] ? countryCode[1] : '\0'), '\0' };
        base = [[NSString stringWithUTF8String:buf] lowercaseString];
    }
    NSImage *img = base ? macMenubarLoadSvg(@"flags", base) : nil;
    if (!img) img = macMenubarLoadSvg(@"flags", @"xx");
    return img;
}

static NSString *macMenubarPlatformBasename(int clientType) {
    switch (clientType) {
        case 1: return @"windows";    /* CLIENT_TYPE_WINDOWS */
        case 2: return @"linux";      /* CLIENT_TYPE_LINUX */
        case 3: return @"mac";        /* CLIENT_TYPE_MACOS */
        case 4: return @"ios";        /* CLIENT_TYPE_IOS */
        case 5: return @"android";    /* CLIENT_TYPE_ANDROID */
        case 6: return @"steam-deck"; /* CLIENT_TYPE_STEAMDECK */
        case 7: return @"globe";      /* CLIENT_TYPE_WEB → globe */
        default: return @"globe";     /* CLIENT_TYPE_UNKNOWN */
    }
}

/* Return a copy of data/ui/<basename>.svg with every non-transparent
 * pixel replaced by `tint` (alpha preserved). Mirrors the SDL path's
 * imguiLoadSvgIconWhite trick: the SVG's authored fill is discarded and
 * only its alpha mask survives, so monochrome AND multi-colour icons
 * resolve to a clean coloured silhouette. Required because most ui SVGs
 * are authored black-on-transparent and would render near-invisible
 * against a dark menu background. Result is cached per (basename, RGBA);
 * a system appearance flip produces a different RGBA key and rebuilds. */
static NSImage *macMenubarTintedUiIcon(NSString *basename, NSColor *tint) {
    if (!basename || !tint) return nil;
    NSColor *rgb = [tint colorUsingColorSpace:[NSColorSpace deviceRGBColorSpace]];
    if (!rgb) return macMenubarLoadSvg(@"ui", basename);
    if (!g_iconCache) g_iconCache = [[NSCache alloc] init];
    NSString *key = [NSString stringWithFormat:@"ui-tinted/%@#%.3f,%.3f,%.3f,%.3f",
                     basename, rgb.redComponent, rgb.greenComponent,
                     rgb.blueComponent, rgb.alphaComponent];
    NSImage *cached = [g_iconCache objectForKey:key];
    if (cached) return cached;
    NSImage *base = macMenubarLoadSvg(@"ui", basename);
    if (!base) return nil;
    NSImage *out = [[NSImage alloc] initWithSize:base.size];
    [out lockFocus];
    [rgb set];
    NSRect r = NSMakeRect(0, 0, base.size.width, base.size.height);
    NSRectFill(r);
    [base drawInRect:r fromRect:NSZeroRect
           operation:NSCompositingOperationDestinationIn fraction:1.0];
    [out unlockFocus];
    [g_iconCache setObject:out forKey:key];
    return out;
}

@interface WBMenuBridge : NSObject
- (void)onQuit:(id)sender;
- (void)onAbout:(id)sender;
- (void)onPreferences:(id)sender;
- (void)onNewGame:(id)sender;
- (void)onSaveMap:(id)sender;
- (void)onShowGameInfo:(id)sender;
- (void)onShowSysInfo:(id)sender;
- (void)onShowNetInfo:(id)sender;
- (void)onShowMapOverview:(id)sender;
- (void)onToggleOverviewInWindow:(id)sender;
- (void)onToggleFullScreen:(id)sender;
- (void)onSmoothScrolling:(id)sender;
- (void)onAutoScrolling:(id)sender;
- (void)onShowGunsight:(id)sender;
- (void)onLabelOwnTank:(id)sender;
- (void)onPillboxLabels:(id)sender;
- (void)onBaseLabels:(id)sender;
- (void)onSetFrameRate:(id)sender;
- (void)onSetZoom:(id)sender;
- (void)onSetMessageLabel:(id)sender;
- (void)onSetTankLabel:(id)sender;
- (void)onAllowNewPlayers:(id)sender;
- (void)onShowKeySetup:(id)sender;
- (void)onShowChangeName:(id)sender;
- (void)onSoundEffects:(id)sender;
- (void)onBackgroundSound:(id)sender;
- (void)onSoundKeepalive:(id)sender;
- (void)onSetSoundVolume:(id)sender;
- (void)onNewswireMessages:(id)sender;
- (void)onAssistantMessages:(id)sender;
- (void)onAIMessages:(id)sender;
- (void)onNetworkStatusMessages:(id)sender;
- (void)onNetworkDebugMessages:(id)sender;
- (void)onRequestAlliance:(id)sender;
- (void)onLeaveAlliance:(id)sender;
- (void)onVoteReturnToLobby:(id)sender;
- (void)onVoteSurrender:(id)sender;
- (void)onShowSendMsg:(id)sender;
- (void)onShowPlayersPanel:(id)sender;
- (void)onSelectAllPlayers:(id)sender;
- (void)onSelectNonePlayers:(id)sender;
- (void)onSelectAllies:(id)sender;
- (void)onSelectNearby:(id)sender;
- (void)onBrainManual:(id)sender;
- (void)onBrainItem:(id)sender;
- (void)onBrainSettings:(id)sender;
- (void)onDockSinglePlayer:(id)sender;
- (void)onDockFindInternet:(id)sender;
- (void)onDockFindLan:(id)sender;
- (void)onDockJoinByAddress:(id)sender;
- (void)onDockOpenMapEditor:(id)sender;
- (void)onDockOpenLogViewer:(id)sender;
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
    sdl3ImguiShowGameInfo(!sdl3ImguiIsGameInfoOpen());
}
- (void)onShowSysInfo:(id)sender {
    (void)sender;
    sdl3ImguiShowSysInfo(!sdl3ImguiIsSysInfoOpen());
}
- (void)onShowNetInfo:(id)sender {
    (void)sender;
    sdl3ImguiShowNetInfo(!sdl3ImguiIsNetInfoOpen());
}
- (void)onShowMapOverview:(id)sender {
    (void)sender;
    sdl3ImguiShowMapOverview(!sdl3ImguiIsMapOverviewOpen());
}
- (void)onToggleOverviewInWindow:(id)sender {
    (void)sender;
    sdl3ImguiShowOverviewInWindow(!sdl3ImguiIsOverviewInWindowOpen());
}
- (void)onToggleFullScreen:(id)sender {
    (void)sender;
    sdl3ImguiToggleFullScreen((struct ClientSim *)g_clientSim);
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
- (void)onAllowNewPlayers:(id)sender {
    (void)sender;
    if (g_clientSim) windowMenuAllowNewPlayers_toggle((struct ClientSim *)g_clientSim);
}
- (void)onShowKeySetup:(id)sender {
    (void)sender;
    sdl3ImguiShowKeySetup();
}
- (void)onShowChangeName:(id)sender {
    (void)sender;
    sdl3ImguiShowChangeName();
}
- (void)onSoundEffects:(id)sender {
    (void)sender;
    windowSoundEffects_toggle();
}
- (void)onBackgroundSound:(id)sender {
    (void)sender;
    windowBackgroundSoundChange_toggle();
}
- (void)onSoundKeepalive:(id)sender {
    (void)sender;
    windowSoundKeepalive();
}
- (void)onSetSoundVolume:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    windowSetSoundVolume((int)[item tag]);
}
- (void)onNewswireMessages:(id)sender {
    (void)sender;
    if (g_clientSim) windowMenuNewswire_toggle((struct ClientSim *)g_clientSim);
}
- (void)onAssistantMessages:(id)sender {
    (void)sender;
    if (g_clientSim) windowMenuAssistant_toggle((struct ClientSim *)g_clientSim);
}
- (void)onAIMessages:(id)sender {
    (void)sender;
    if (g_clientSim) windowMenuAI_toggle((struct ClientSim *)g_clientSim);
}
- (void)onNetworkStatusMessages:(id)sender {
    (void)sender;
    if (g_clientSim) windowMenuNetwork_toggle((struct ClientSim *)g_clientSim);
}
- (void)onNetworkDebugMessages:(id)sender {
    (void)sender;
    if (g_clientSim) windowMenuNetworkDebug_toggle((struct ClientSim *)g_clientSim);
}
- (void)onRequestAlliance:(id)sender {
    (void)sender;
    /* Set the cooldown before firing — mirrors the in-window menu's
     * order at sdl3imgui.cpp:~2272-2274 so a request that fails (no
     * eligible targets, network drop) still triggers the cooldown. */
    sdl3ImguiNoteAllianceRequested();
    if (g_clientSim) clientSimRequestAllianceSelected((struct ClientSim *)g_clientSim);
}
- (void)onLeaveAlliance:(id)sender {
    (void)sender;
    if (g_clientSim) clientSimLeaveAllianceSelf((struct ClientSim *)g_clientSim);
}
/* Vote handlers — mirror the in-window Players menu vote items in
 * sdl3imgui.cpp. The send wrapper routes through clientSimSubmitCommand
 * (chat/alliance pattern), so the same call covers UDP and SP-host.
 * The widget-visible setter opens the local-side vote panel; once the
 * server publishes CTRL_GAME_VOTE_STATE the widget populates. The
 * numeric literals match the GAME_VOTE_KIND_* / GAME_VOTE_TOGGLE_*
 * constants in client_sim.h — kept inline to avoid pulling a T1 header
 * into a platform .mm. */
- (void)onVoteReturnToLobby:(id)sender {
    (void)sender;
    if (!g_clientSim) return;
    clientSimNetSendGameVoteToggle((struct ClientSim *)g_clientSim, 1, 2);
    clientSimSetGameVoteWidgetVisible((struct ClientSim *)g_clientSim, 1, true);
}
- (void)onVoteSurrender:(id)sender {
    (void)sender;
    if (!g_clientSim) return;
    clientSimNetSendGameVoteToggle((struct ClientSim *)g_clientSim, 2, 2);
    clientSimSetGameVoteWidgetVisible((struct ClientSim *)g_clientSim, 2, true);
}
- (void)onShowSendMsg:(id)sender {
    (void)sender;
    /* Show-and-raise against the pop-out, which is usually sitting behind the
       game window where hiding it is never what the player meant; a toggle
       against the in-window panel the full screen map draws instead. See
       sdl3ImguiSendMsgShortcut in sdl3imgui.cpp. */
    sdl3ImguiSendMsgShortcut();
}
- (void)onShowPlayersPanel:(id)sender {
    (void)sender;
    /* Two behaviours behind one action, the same two the in-window bar has.
       A click on the menu item toggles: the tick this item carries has to be
       able to come off again, and clicking a ticked item is an explicit "put
       it away". The ⇧⌘P key equivalent AppKit hangs off this same item shows
       and raises instead, for the reason the in-window shortcut does — that
       key press lands on the main window, so a toggle would close the pop-out
       the player was asking to bring forward. AppKit sends both through this
       one selector; the event that triggered it tells them apart, a key-down
       for the equivalent and a mouse event for the click. */
    NSEvent *trigger = [NSApp currentEvent];
    if (trigger && [trigger type] == NSEventTypeKeyDown) {
        sdl3ImguiShowPlayersPanel(true);
    } else {
        sdl3ImguiTogglePlayersPanel();
    }
}
- (void)onSelectAllPlayers:(id)sender {
    (void)sender;
    if (g_clientSim) clientSimCheckAllNonePlayers((struct ClientSim *)g_clientSim, true);
}
- (void)onSelectNonePlayers:(id)sender {
    (void)sender;
    if (g_clientSim) clientSimCheckAllNonePlayers((struct ClientSim *)g_clientSim, false);
}
- (void)onSelectAllies:(id)sender {
    (void)sender;
    if (g_clientSim) clientSimCheckAlliedPlayers((struct ClientSim *)g_clientSim);
}
- (void)onSelectNearby:(id)sender {
    (void)sender;
    if (g_clientSim) clientSimCheckNearbyPlayers((struct ClientSim *)g_clientSim);
}
- (void)onBrainManual:(id)sender {
    (void)sender;
    sdl3ImguiStopBrain();
}
- (void)onBrainItem:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    sdl3ImguiStartBrain((int)item.tag, (struct ClientSim *)g_clientSim);
}
- (void)onBrainSettings:(id)sender {
    (void)sender;
    sdl3ImguiShowBrainSettings();
}
- (void)onDockSinglePlayer:(id)sender {
    (void)sender;
    gameFrontRequestTransition(openSetup);
}
- (void)onDockFindInternet:(id)sender {
    (void)sender;
    gameFrontRequestTransition(openInternet);
}
- (void)onDockFindLan:(id)sender {
    (void)sender;
    gameFrontRequestTransition(openLan);
}
- (void)onDockJoinByAddress:(id)sender {
    (void)sender;
    gameFrontRequestTransition(openUdp);
}
- (void)onDockOpenMapEditor:(id)sender {
    (void)sender;
    gameFrontRequestTransition(openMapEditor);
}
- (void)onDockOpenLogViewer:(id)sender {
    (void)sender;
    gameFrontRequestTransition(openLogViewer);
}
@end

/* Custom view used as the .view of each occupied Players-menu slot row.
 * Draws checkmark, flag, platform icon, optional WBN/Steam badges, name,
 * and a colour-coded ping. -mouseUp: toggles the slot's selection state
 * and dismisses the menu, matching the in-window Players panel's
 * click-to-toggle UX. */
@interface WBPlayerSlotView : NSView {
    NSImage  *_flagImage;          /* full-colour, drawn as-is */
    NSString *_platformBasename;   /* ui-icon name; tinted at draw time */
    BOOL      _platformGold;       /* verified && supporter → gold tint */
    BOOL      _hasWbn;             /* draw shield.svg tinted */
    BOOL      _hasSteam;           /* draw steam.svg tinted */
    NSString *_name;
    NSString *_pingText;
    NSColor  *_pingColor;
    BOOL      _checked;
}
@property (nonatomic) NSInteger slotIndex;
- (void)setFromSlot:(const struct MacPlayerSlot *)slot;
@end

@implementation WBPlayerSlotView

- (void)setFromSlot:(const struct MacPlayerSlot *)slot {
    if (!slot) return;
    BOOL verified  = (slot->pflags & 0x01) != 0;  /* PLAYER_FLAG_WBN_VERIFIED */
    BOOL linked    = (slot->pflags & 0x02) != 0;  /* PLAYER_FLAG_WBN_STEAM_LINKED */
    BOOL supporter = (slot->pflags & 0x04) != 0;  /* PLAYER_FLAG_SUPPORTER */
    BOOL isBot     = (slot->pflags & 0x20) != 0;  /* PLAYER_FLAG_BOT */
    /* Bot rows: no country flag, brain icon in the platform-icon slot,
     * no WBN/Steam badges (a bot can never be either). */
    _flagImage        = isBot ? nil : macMenubarFlagIcon(slot->country);
    _platformBasename = isBot ? @"brain" : macMenubarPlatformBasename(slot->ptype);
    _platformGold     = (!isBot && verified && supporter) ? YES : NO;
    _hasWbn           = (!isBot && verified) ? YES : NO;
    _hasSteam         = (!isBot && linked)   ? YES : NO;
    _name             = slot->name[0] ? [NSString stringWithUTF8String:slot->name] : @"";
    int ping = slot->ping;
    if (ping <= 0) {
        _pingText  = @"---";
        _pingColor = [NSColor colorWithCalibratedWhite:0.5 alpha:1.0];
    } else {
        _pingText = [NSString stringWithFormat:@"%d", ping];
        /* Band is decided upstream (clientSimGetPlayerPingBand) and rides
         * in the slot: re-deriving it from the number here would drop the
         * hysteresis and strobe this row while the ImGui ones held. */
        switch ((PingBand)slot->pingBand) {
            case PING_BAND_GOOD:
                _pingColor = [NSColor colorWithCalibratedRed:0.0 green:0.9 blue:0.0 alpha:1.0];
                break;
            case PING_BAND_FAIR:
                _pingColor = [NSColor colorWithCalibratedRed:0.9 green:0.9 blue:0.0 alpha:1.0];
                break;
            case PING_BAND_POOR:
                _pingColor = [NSColor colorWithCalibratedRed:0.9 green:0.0 blue:0.0 alpha:1.0];
                break;
            case PING_BAND_NONE:
            default:
                _pingColor = [NSColor colorWithCalibratedWhite:0.5 alpha:1.0];
                break;
        }
    }
    _checked = slot->checked ? YES : NO;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect bounds = self.bounds;
    BOOL highlighted = self.enclosingMenuItem.isHighlighted;

    if (highlighted) {
        [[NSColor selectedMenuItemColor] set];
        NSRectFill(bounds);
    }

    NSColor *textColor = highlighted ? [NSColor selectedMenuItemTextColor] : [NSColor labelColor];
    /* Icons authored in data/ui/*.svg are mostly black on transparent;
     * rendering them as-is washes out against the menu background (and
     * goes near-invisible in dark mode). Force them to the menu's text
     * colour so they read with the same contrast as the row's text. */
    NSColor *iconTint  = textColor;
    NSColor *goldTint  = [NSColor colorWithCalibratedRed:1.00 green:0.84 blue:0.20 alpha:1.0];
    NSFont  *font      = [NSFont menuFontOfSize:0];
    CGFloat  lineH     = font.pointSize + 4.0;
    CGFloat  midY      = NSMidY(bounds);

    CGFloat x = 6.0;

    /* Checkmark column (fixed ~14pt — keeps following icons aligned). */
    if (_checked) {
        NSDictionary *attrs = @{NSFontAttributeName: font,
                                NSForegroundColorAttributeName: textColor};
        NSString *check = @"✓";
        NSSize sz = [check sizeWithAttributes:attrs];
        [check drawAtPoint:NSMakePoint(x, midY - sz.height * 0.5) withAttributes:attrs];
    }
    x += 14.0;

    /* Flag 16x11. */
    if (_flagImage) {
        [_flagImage drawInRect:NSMakeRect(x, midY - 5.5, 16.0, 11.0)
                     fromRect:NSZeroRect
                    operation:NSCompositingOperationSourceOver
                     fraction:1.0];
    }
    x += 16.0 + 4.0;

    /* Platform icon 14x14. Gold-tinted for verified supporters (held
     * constant across highlight so the supporter cue stays distinct);
     * otherwise tracks the row's text colour. */
    if (_platformBasename) {
        NSImage *platform = macMenubarTintedUiIcon(_platformBasename,
                                                   _platformGold ? goldTint : iconTint);
        if (platform) {
            [platform drawInRect:NSMakeRect(x, midY - 7.0, 14.0, 14.0)
                       fromRect:NSZeroRect
                      operation:NSCompositingOperationSourceOver
                       fraction:1.0];
        }
    }
    x += 14.0 + 4.0;

    /* WBN-verified shield — only when WBN_VERIFIED. */
    if (_hasWbn) {
        NSImage *wbn = macMenubarTintedUiIcon(@"shield", iconTint);
        if (wbn) {
            [wbn drawInRect:NSMakeRect(x, midY - 7.0, 14.0, 14.0)
                  fromRect:NSZeroRect
                 operation:NSCompositingOperationSourceOver
                  fraction:1.0];
        }
        x += 14.0 + 4.0;
    }

    /* Steam logo — only when WBN_STEAM_LINKED. */
    if (_hasSteam) {
        NSImage *steam = macMenubarTintedUiIcon(@"steam", iconTint);
        if (steam) {
            [steam drawInRect:NSMakeRect(x, midY - 7.0, 14.0, 14.0)
                    fromRect:NSZeroRect
                   operation:NSCompositingOperationSourceOver
                    fraction:1.0];
        }
        x += 14.0 + 4.0;
    }

    CGFloat rightReserve = 50.0;
    CGFloat rightPad     = 6.0;

    /* Name — flexible width, truncated tail if too long. */
    if (_name.length > 0) {
        NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
        ps.lineBreakMode = NSLineBreakByTruncatingTail;
        NSDictionary *attrs = @{NSFontAttributeName: font,
                                NSForegroundColorAttributeName: textColor,
                                NSParagraphStyleAttributeName: ps};
        CGFloat nameW = bounds.size.width - rightReserve - rightPad - x;
        if (nameW < 0) nameW = 0;
        NSRect r = NSMakeRect(x, midY - lineH * 0.5, nameW, lineH);
        [_name drawInRect:r withAttributes:attrs];
    }

    /* Ping — right-aligned, colour-coded (overridden to the menu's
     * selection text colour when the row is highlighted, so the colour
     * cue stays legible). */
    if (_pingText.length > 0) {
        NSColor *pColor = highlighted ? [NSColor selectedMenuItemTextColor] : _pingColor;
        NSMutableParagraphStyle *ps = [[NSMutableParagraphStyle alloc] init];
        ps.alignment = NSTextAlignmentRight;
        NSDictionary *attrs = @{NSFontAttributeName: font,
                                NSForegroundColorAttributeName: pColor,
                                NSParagraphStyleAttributeName: ps};
        NSRect r = NSMakeRect(bounds.size.width - rightReserve - rightPad,
                              midY - lineH * 0.5,
                              rightReserve,
                              lineH);
        [_pingText drawInRect:r withAttributes:attrs];
    }
}

- (void)mouseUp:(NSEvent *)event {
    (void)event;
    if (g_clientSim) {
        clientSimTogglePlayerCheckState((struct ClientSim *)g_clientSim,
                                        (unsigned char)self.slotIndex);
    }
    [self.enclosingMenuItem.menu cancelTracking];
}

@end

/* Dock-menu items are welcome-screen state transitions. Outside the
 * welcome screen they would silently no-op (the pending channel is only
 * consumed by the welcome loop), so render them dimmed instead. The
 * menu is rebuilt on every right-click of the Dock icon, so the
 * enabled bits track gameFrontIsAtWelcome() with zero refresh cost. */
static NSMenu *macMenubarBuildDockMenu(void) {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@""];
    [menu setAutoenablesItems:NO];
    BOOL enabled = gameFrontIsAtWelcome() ? YES : NO;

    NSMenuItem *(^addItem)(langid, SEL) = ^NSMenuItem *(langid str, SEL sel) {
        NSMenuItem *it = [[NSMenuItem alloc]
            initWithTitle:LANG_STR(str)
                   action:sel
            keyEquivalent:@""];
        [it setTarget:g_bridge];
        [it setEnabled:enabled];
        [menu addItem:it];
        return it;
    };

    addItem(STR_DLGWELCOME_SINGLE,        @selector(onDockSinglePlayer:));
    addItem(STR_MENU_FIND_INTERNET_GAME,  @selector(onDockFindInternet:));
    addItem(STR_MENU_FIND_LAN_GAME,       @selector(onDockFindLan:));
    addItem(STR_MENU_JOIN_BY_ADDRESS,     @selector(onDockJoinByAddress:));
    [menu addItem:[NSMenuItem separatorItem]];
    addItem(STR_MENU_OPEN_MAP_EDITOR,     @selector(onDockOpenMapEditor:));
    addItem(STR_MENU_OPEN_LOG_VIEWER,     @selector(onDockOpenLogViewer:));

    return menu;
}

/* SDL3 installs its own NSApp delegate (SDL3AppDelegate) and reuses
 * the static across re-init paths, so swapping NSApp.delegate to a
 * proxy is fragile — SDL3 holds no strong ref to the original and the
 * weak NSApp.delegate slot then races our proxy's lifetime. Inject
 * the dock-menu hook directly onto SDL3's existing delegate class via
 * the Objective-C runtime instead: one new method, zero replacements,
 * no impact on SDL3's own lifecycle handlers. */
static NSMenu *macMenubarDockMenuIMP(id self, SEL _cmd, NSApplication *sender) {
    (void)self;
    (void)_cmd;
    (void)sender;
    return macMenubarBuildDockMenu();
}

static void macMenubarInstallDockMenu(void) {
    id delegate = [NSApp delegate];
    if (!delegate) return;
    Class cls = object_getClass(delegate);
    if (!cls) return;
    /* Use class_replaceMethod so a future SDL3 update that ships its
     * own applicationDockMenu: doesn't silently win. The signature
     * "@@:@" matches -(NSMenu *) applicationDockMenu:(NSApplication *). */
    class_replaceMethod(cls,
                        @selector(applicationDockMenu:),
                        (IMP)macMenubarDockMenuIMP,
                        "@@:@");
}

void mac_menubar_install_dock_menu(void) {
    if (g_bridge == nil) {
        g_bridge = [[WBMenuBridge alloc] init];
    }
    macMenubarInstallDockMenu();
}

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
        initWithTitle:LANG_STR(STR_MENU_ABOUT_APP)
        action:@selector(onAbout:)
        keyEquivalent:@""];
    [aboutItem setTarget:g_bridge];
    [appMenu addItem:aboutItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *prefsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_PREFERENCES)
        action:@selector(onPreferences:)
        keyEquivalent:@","];
    [prefsItem setTarget:g_bridge];
    [appMenu addItem:prefsItem];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_SERVICES)];
    NSMenuItem *servicesItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SERVICES) action:nil keyEquivalent:@""];
    [servicesItem setSubmenu:servicesMenu];
    [appMenu addItem:servicesItem];
    [NSApp setServicesMenu:servicesMenu];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *hideItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_HIDE_APP)
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
        initWithTitle:LANG_STR(STR_MENU_QUIT_APP)
        action:@selector(onQuit:)
        keyEquivalent:@"q"];
    [quitItem setTarget:g_bridge];
    [appMenu addItem:quitItem];

    /* File menu — game lifecycle and pop-out info windows. */
    NSMenuItem *fileItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_FILE) action:nil keyEquivalent:@""];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_FILE)];
    [fileItem setSubmenu:fileMenu];
    /* Map Overview's enabled state comes from game state via
     * mac_menubar_refresh(). AppKit's automatic enabling would recompute it
     * back to enabled on every menu update and key-equivalent lookup, since
     * the item has a live target that responds to its action. Every other
     * item in this menu is always selectable, which is what an NSMenuItem
     * defaults to, so turning the automatic mode off changes nothing for
     * them. */
    [fileMenu setAutoenablesItems:NO];

    NSMenuItem *newGameItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_NEW)
        action:@selector(onNewGame:)
        keyEquivalent:@""];
    [newGameItem setTarget:g_bridge];
    [fileMenu addItem:newGameItem];

    NSMenuItem *saveMapItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SAVE_MAP)
        action:@selector(onSaveMap:)
        keyEquivalent:@"s"];
    [saveMapItem setTarget:g_bridge];
    [fileMenu addItem:saveMapItem];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *gameInfoItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_DLGGAMEINFO_TITLE)
        action:@selector(onShowGameInfo:)
        keyEquivalent:@""];
    [gameInfoItem setTarget:g_bridge];
    [fileMenu addItem:gameInfoItem];
    s_gameInfoItem = gameInfoItem;

    NSMenuItem *sysInfoItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_DLGSYSINFO_TITLE)
        action:@selector(onShowSysInfo:)
        keyEquivalent:@""];
    [sysInfoItem setTarget:g_bridge];
    [fileMenu addItem:sysInfoItem];
    s_sysInfoItem = sysInfoItem;

    NSMenuItem *netInfoItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_DLGNETINFO_TITLE)
        action:@selector(onShowNetInfo:)
        keyEquivalent:@""];
    [netInfoItem setTarget:g_bridge];
    [fileMenu addItem:netInfoItem];
    s_netInfoItem = netInfoItem;

    [fileMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *mapOverviewItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_MAP_OVERVIEW)
        action:@selector(onShowMapOverview:)
        keyEquivalent:@"o"];
    [mapOverviewItem setTarget:g_bridge];
    [fileMenu addItem:mapOverviewItem];
    s_mapOverviewItem = mapOverviewItem;

    NSMenuItem *overviewInWindowItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_OVERVIEW_IN_WINDOW)
        action:@selector(onToggleOverviewInWindow:)
        keyEquivalent:@""];
    [overviewInWindowItem setTarget:g_bridge];
    [fileMenu addItem:overviewInWindowItem];
    s_overviewInWindowItem = overviewInWindowItem;

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

    s_frameRateMenu = frameRateMenu;

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

    /* Window Size is the only submenu with explicit per-item .enabled
     * gating (based on fit1x..fit4x). Turn off AppKit auto-enable so our
     * flags actually take effect; other submenus keep the default. */
    [windowSizeMenu setAutoenablesItems:NO];
    s_windowSizeMenu = windowSizeMenu;

    /* Smooth Scrolling — single toggle, no shortcut. */
    NSMenuItem *smoothScrollItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SMOOTH_SCROLLING)
        action:@selector(onSmoothScrolling:)
        keyEquivalent:@""];
    [smoothScrollItem setTarget:g_bridge];
    [editMenu addItem:smoothScrollItem];
    s_smoothScrollingItem = smoothScrollItem;

    [editMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *autoScrollItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_AUTO_SCROLLING)
        action:@selector(onAutoScrolling:)
        keyEquivalent:@"a"];
    [autoScrollItem setTarget:g_bridge];
    [editMenu addItem:autoScrollItem];
    s_autoScrollingItem = autoScrollItem;

    NSMenuItem *showGunsightItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SHOW_GUNSIGHT)
        action:@selector(onShowGunsight:)
        keyEquivalent:@"g"];
    [showGunsightItem setTarget:g_bridge];
    [editMenu addItem:showGunsightItem];
    s_showGunsightItem = showGunsightItem;

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

    s_messageLabelsMenu = msgLabelsMenu;

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

    /* Sentinel tag so the tank-labels tag-walk in mac_menubar_refresh()
     * cannot match this sibling toggle (real labelTank values are 0/1/2). */
    NSMenuItem *noOwnLabel = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_NO_OWN_LABEL)
        action:@selector(onLabelOwnTank:)
        keyEquivalent:@""];
    [noOwnLabel setTarget:g_bridge]; [noOwnLabel setTag:-1];
    [tankLabelsMenu addItem:noOwnLabel];
    s_noOwnLabelItem = noOwnLabel;

    s_tankLabelsMenu = tankLabelsMenu;

    NSMenuItem *pillLabelsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_PILLBOX_LABELS)
        action:@selector(onPillboxLabels:)
        keyEquivalent:@"p"];
    [pillLabelsItem setTarget:g_bridge];
    [editMenu addItem:pillLabelsItem];
    s_pillLabelsItem = pillLabelsItem;

    NSMenuItem *baseLabelsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_BASE_LABELS)
        action:@selector(onBaseLabels:)
        keyEquivalent:@"b"];
    [baseLabelsItem setTarget:g_bridge];
    [editMenu addItem:baseLabelsItem];
    s_baseLabelsItem = baseLabelsItem;

    /* WinBolo menu — game-state toggles and player commands. Mirrors the
     * ImGui WinBolo menu in renderMenuBar(). The in-window Settings entry
     * is intentionally dropped here — App > Preferences (⌘,) opens the
     * same panel. State sync (checkmarks for the toggles) lands in a
     * later phase; for now items render unchecked regardless of state. */
    NSMenuItem *winBoloItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_WINBOLO) action:nil keyEquivalent:@""];
    NSMenu *winBoloMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_WINBOLO)];
    [winBoloItem setSubmenu:winBoloMenu];
    /* Honour our explicit .enabled flags on Request / Leave Alliance. */
    [winBoloMenu setAutoenablesItems:NO];

    NSMenuItem *allowNewPlayersItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_ALLOW_NEW_PLAYERS)
        action:@selector(onAllowNewPlayers:)
        keyEquivalent:@""];
    [allowNewPlayersItem setTarget:g_bridge];
    [winBoloMenu addItem:allowNewPlayersItem];
    s_allowNewPlayersItem = allowNewPlayersItem;

    NSMenuItem *setKeysItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SETKEYS)
        action:@selector(onShowKeySetup:)
        keyEquivalent:@"k"];
    [setKeysItem setTarget:g_bridge];
    [winBoloMenu addItem:setKeysItem];

    NSMenuItem *changeNameItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_DLGCHANGENAME_TITLE)
        action:@selector(onShowChangeName:)
        keyEquivalent:@""];
    [changeNameItem setTarget:g_bridge];
    [winBoloMenu addItem:changeNameItem];

    [winBoloMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *soundEffectsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SOUND_EFFECTS)
        action:@selector(onSoundEffects:)
        keyEquivalent:@""];
    [soundEffectsItem setTarget:g_bridge];
    [winBoloMenu addItem:soundEffectsItem];
    s_soundEffectsItem = soundEffectsItem;

    NSMenuItem *backgroundSoundItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_BACKGROUND_SOUND)
        action:@selector(onBackgroundSound:)
        keyEquivalent:@""];
    [backgroundSoundItem setTarget:g_bridge];
    [winBoloMenu addItem:backgroundSoundItem];
    s_backgroundSoundItem = backgroundSoundItem;

    NSMenuItem *soundKeepaliveItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SOUND_KEEPALIVE)
        action:@selector(onSoundKeepalive:)
        keyEquivalent:@""];
    [soundKeepaliveItem setTarget:g_bridge];
    [winBoloMenu addItem:soundKeepaliveItem];
    s_soundKeepaliveItem = soundKeepaliveItem;

    /* Volume submenu — fixed presets (Mute, 25%, 50%, 75%, 100%).
     * Tags carry the percentage; the active preset gets a checkmark in
     * mac_menubar_refresh(). */
    NSMenuItem *volumeRoot = [winBoloMenu addItemWithTitle:LANG_STR(STR_MENU_VOLUME)
                                                    action:nil
                                             keyEquivalent:@""];
    NSMenu *volumeMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_VOLUME)];
    [volumeRoot setSubmenu:volumeMenu];

    const int volumePresets[] = { 0, 25, 50, 75, 100 };
    for (size_t i = 0; i < sizeof(volumePresets) / sizeof(volumePresets[0]); i++) {
        int pct = volumePresets[i];
        NSString *title = (pct == 0)
            ? LANG_STR(STR_VOLUME_MUTE)
            : [NSString stringWithFormat:@"%d%%", pct];
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:title
            action:@selector(onSetSoundVolume:)
            keyEquivalent:@""];
        [item setTarget:g_bridge];
        [item setTag:pct];
        [volumeMenu addItem:item];
    }
    s_volumeMenu = volumeMenu;

    [winBoloMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *newswireMsgsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_NEWSWIRE_MSGS)
        action:@selector(onNewswireMessages:)
        keyEquivalent:@""];
    [newswireMsgsItem setTarget:g_bridge];
    [winBoloMenu addItem:newswireMsgsItem];
    s_newswireMessagesItem = newswireMsgsItem;

    NSMenuItem *assistantMsgsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_ASSISTANT_MSGS)
        action:@selector(onAssistantMessages:)
        keyEquivalent:@""];
    [assistantMsgsItem setTarget:g_bridge];
    [winBoloMenu addItem:assistantMsgsItem];
    s_assistantMessagesItem = assistantMsgsItem;

    NSMenuItem *aiMsgsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_AI_MSGS)
        action:@selector(onAIMessages:)
        keyEquivalent:@""];
    [aiMsgsItem setTarget:g_bridge];
    [winBoloMenu addItem:aiMsgsItem];
    s_aiMessagesItem = aiMsgsItem;

    NSMenuItem *netStatusMsgsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_NETSTATUS_MSGS)
        action:@selector(onNetworkStatusMessages:)
        keyEquivalent:@""];
    [netStatusMsgsItem setTarget:g_bridge];
    [winBoloMenu addItem:netStatusMsgsItem];
    s_networkStatusMessagesItem = netStatusMsgsItem;

    NSMenuItem *netDebugMsgsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_NETDEBUG_MSGS)
        action:@selector(onNetworkDebugMessages:)
        keyEquivalent:@""];
    [netDebugMsgsItem setTarget:g_bridge];
    [winBoloMenu addItem:netDebugMsgsItem];
    s_networkDebugMessagesItem = netDebugMsgsItem;

    [winBoloMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *requestAllianceItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_REQUEST_ALLIANCE)
        action:@selector(onRequestAlliance:)
        keyEquivalent:@"r"];
    [requestAllianceItem setTarget:g_bridge];
    [winBoloMenu addItem:requestAllianceItem];
    s_winboloRequestAllianceItem = requestAllianceItem;

    NSMenuItem *leaveAllianceItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LEAVE_ALLIANCE)
        action:@selector(onLeaveAlliance:)
        keyEquivalent:@""];
    [leaveAllianceItem setTarget:g_bridge];
    [winBoloMenu addItem:leaveAllianceItem];
    s_winboloLeaveAllianceItem = leaveAllianceItem;

    /* Players menu — only the static items from the ImGui Players menu in
     * renderMenuBar(). The dynamic per-player list (alliance indicator,
     * flag, ping, name) stays in the in-window Players Panel; we don't
     * replicate it natively. Send Message routes through the wrapper so
     * macOS opens a real native popout (the wrapper handles desktop vs.
     * tablet/mobile). */
    NSMenuItem *playersItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_PLAYERS) action:nil keyEquivalent:@""];
    NSMenu *playersMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_PLAYERS)];
    [playersItem setSubmenu:playersMenu];
    /* Honour our explicit .enabled flags on Request / Leave Alliance. */
    [playersMenu setAutoenablesItems:NO];

    /* Send Message — ⇧⌘M (Cmd+M is Window > Minimize per the earlier
     * collision-resolution decision). */
    NSMenuItem *sendMsgItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SEND_MESSAGE)
        action:@selector(onShowSendMsg:)
        keyEquivalent:@"m"];
    [sendMsgItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [sendMsgItem setTarget:g_bridge];
    [playersMenu addItem:sendMsgItem];
    s_sendMsgItem = sendMsgItem;

    /* Players Panel — ⇧⌘P. A checked toggle like its in-window twin, so the
     * item pointer is kept for the per-frame refresh to tick. */
    NSMenuItem *playersPanelItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_PLAYERS_PANEL)
        action:@selector(onShowPlayersPanel:)
        keyEquivalent:@"p"];
    [playersPanelItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [playersPanelItem setTarget:g_bridge];
    [playersMenu addItem:playersPanelItem];
    s_playersPanelItem = playersPanelItem;

    [playersMenu addItem:[NSMenuItem separatorItem]];

    /* The ImGui menu uses ImGuiSelectableFlags_DontClosePopups to allow
     * chaining several Select actions in one open; NSMenu has no
     * equivalent, so clicking dismisses the menu. Users can re-open it. */
    NSMenuItem *selectAllItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SELECT_ALL)
        action:@selector(onSelectAllPlayers:)
        keyEquivalent:@""];
    [selectAllItem setTarget:g_bridge];
    [playersMenu addItem:selectAllItem];

    NSMenuItem *selectNoneItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SELECT_NONE)
        action:@selector(onSelectNonePlayers:)
        keyEquivalent:@""];
    [selectNoneItem setTarget:g_bridge];
    [playersMenu addItem:selectNoneItem];

    NSMenuItem *selectAlliesItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SELECT_ALLIES)
        action:@selector(onSelectAllies:)
        keyEquivalent:@""];
    [selectAlliesItem setTarget:g_bridge];
    [playersMenu addItem:selectAlliesItem];

    NSMenuItem *selectNearbyItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SELECT_NEARBY)
        action:@selector(onSelectNearby:)
        keyEquivalent:@""];
    [selectNearbyItem setTarget:g_bridge];
    [playersMenu addItem:selectNearbyItem];

    [playersMenu addItem:[NSMenuItem separatorItem]];

    /* 16 player slots — each frame mac_menubar_refresh() decides whether
     * to attach a rich WBPlayerSlotView (occupied slot) or restore the
     * disabled numeric placeholder (empty slot). The placeholders keep
     * the menu's vertical footprint stable while empty. */
    for (int i = 0; i < 16; i++) {
        NSMenuItem *slot = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%d", i + 1]
            action:nil
            keyEquivalent:@""];
        [slot setEnabled:NO];
        [playersMenu addItem:slot];
        s_playerSlotItems[i] = slot;
        s_playerSlotViews[i] = nil;
    }

    [playersMenu addItem:[NSMenuItem separatorItem]];

    /* No accelerator — WinBolo > Request Alliance owns Cmd+R. The two
     * Request items share the same selector and trampoline; AppKit
     * dispatches by selector so firing either one routes identically. */
    NSMenuItem *playersRequestAllianceItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_REQUEST_ALLIANCE)
        action:@selector(onRequestAlliance:)
        keyEquivalent:@""];
    [playersRequestAllianceItem setTarget:g_bridge];
    [playersMenu addItem:playersRequestAllianceItem];
    s_playersRequestAllianceItem = playersRequestAllianceItem;

    NSMenuItem *playersLeaveAllianceItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LEAVE_ALLIANCE)
        action:@selector(onLeaveAlliance:)
        keyEquivalent:@""];
    [playersLeaveAllianceItem setTarget:g_bridge];
    [playersMenu addItem:playersLeaveAllianceItem];
    s_playersLeaveAllianceItem = playersLeaveAllianceItem;

    /* In-game votes — back-to-lobby and surrender. Placement mirrors the
     * in-window Players menu in renderMenuBar(): after the alliance
     * actions, separated by a divider, enabled only while the game is
     * running. The Surrender item additionally requires exactly two
     * active human teams and that we are not on team 0 — gated in
     * mac_menubar_refresh() from MacMenuState. */
    [playersMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *playersVoteBackToLobbyItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_VOTE_BACK_TO_LOBBY)
        action:@selector(onVoteReturnToLobby:)
        keyEquivalent:@""];
    [playersVoteBackToLobbyItem setTarget:g_bridge];
    [playersMenu addItem:playersVoteBackToLobbyItem];
    s_playersVoteBackToLobbyItem = playersVoteBackToLobbyItem;

    NSMenuItem *playersVoteSurrenderItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_VOTE_SURRENDER)
        action:@selector(onVoteSurrender:)
        keyEquivalent:@""];
    [playersVoteSurrenderItem setTarget:g_bridge];
    [playersMenu addItem:playersVoteSurrenderItem];
    s_playersVoteSurrenderItem = playersVoteSurrenderItem;

    /* Brains menu — top-level, between Players and Window. Mirrors the
     * in-window Brains menu in renderMenuBar(). The submenu is built with
     * just the permanent Manual item; mac_menubar_refresh() rebuilds the
     * dynamic brain list and adds/removes the Settings entry. The parent
     * is enabled-gated on aiActive (refresh decides). */
    NSMenuItem *brainsItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_BRAINS) action:nil keyEquivalent:@""];
    NSMenu *brainsMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_BRAINS)];
    [brainsItem setSubmenu:brainsMenu];
    /* Brains menu items are explicitly gated; AppKit auto-enable would
     * second-guess the Manual checkmark and rebuilt brain rows. */
    [brainsMenu setAutoenablesItems:NO];

    NSMenuItem *brainManualItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_MANUAL)
        action:@selector(onBrainManual:)
        keyEquivalent:@""];
    [brainManualItem setTarget:g_bridge];
    [brainsMenu addItem:brainManualItem];

    /* Pre-allocated Settings item — added/removed by refresh based on
     * brainSettingsShown so the brain Settings entry only surfaces while
     * a Lua brain is running. */
    NSMenuItem *brainSettingsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SETTINGS)
        action:@selector(onBrainSettings:)
        keyEquivalent:@""];
    [brainSettingsItem setTarget:g_bridge];

    s_brainsParentItem  = brainsItem;
    s_brainsMenu        = brainsMenu;
    s_brainManualItem   = brainManualItem;
    s_brainSettingsItem = brainSettingsItem;

    /* Window menu — items dispatched through the responder chain to the
     * key NSWindow; no explicit targets. */
    NSMenuItem *windowItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_WINDOW) action:nil keyEquivalent:@""];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_WINDOW)];
    [windowItem setSubmenu:windowMenu];

    NSMenuItem *minimizeItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_MINIMIZE)
        action:@selector(performMiniaturize:)
        keyEquivalent:@"m"];
    [windowMenu addItem:minimizeItem];

    NSMenuItem *zoomItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_ZOOM)
        action:@selector(performZoom:)
        keyEquivalent:@""];
    [windowMenu addItem:zoomItem];

    [windowMenu addItem:[NSMenuItem separatorItem]];

    /* Drives the app's own full screen command rather than AppKit's
       toggleFullScreen: — two items in one menu bar taking the window full
       screen by different routes would leave the flag disagreeing with the
       window. Which of the two toggles it is belongs to the frontend, not to
       the menu item: this is the same call Alt+Enter makes, so in a game it
       is the full screen map and outside one the plain app full screen flag,
       exactly as the in-window bar's File > Overview in Window behaves.
       The title stays put and the state shows as a checkmark, the way every
       other toggle in this bar reports itself. */
    NSMenuItem *fullScreenItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_ENTER_FULL_SCREEN)
        action:@selector(onToggleFullScreen:)
        keyEquivalent:@"f"];
    [fullScreenItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagControl];
    [fullScreenItem setTarget:g_bridge];
    [windowMenu addItem:fullScreenItem];
    s_fullScreenItem = fullScreenItem;

    [windowMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *bringAllToFrontItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_BRING_ALL_TO_FRONT)
        action:@selector(arrangeInFront:)
        keyEquivalent:@""];
    [windowMenu addItem:bringAllToFrontItem];

    [NSApp setWindowsMenu:windowMenu];

    [NSApp setMainMenu:mainMenu];

    macMenubarInstallDockMenu();
}

void mac_menubar_set_clientsim(void *clientSim) {
    g_clientSim = clientSim;
}

void mac_menubar_refresh(const struct MacMenuState *s) {
    if (!s) return;

    if (s_smoothScrollingItem) {
        [s_smoothScrollingItem setState:(s->smoothScrolling ? NSControlStateValueOn : NSControlStateValueOff)];
    }

    if (s_frameRateMenu) {
        for (NSMenuItem *item in [s_frameRateMenu itemArray]) {
            [item setState:(([item tag] == s->frameRate) ? NSControlStateValueOn : NSControlStateValueOff)];
        }
    }

    if (s_windowSizeMenu) {
        for (NSMenuItem *item in [s_windowSizeMenu itemArray]) {
            NSInteger tag = [item tag];
            [item setState:((tag == s->zoomFactor) ? NSControlStateValueOn : NSControlStateValueOff)];
            BOOL enabled = YES;
            switch (tag) {
                case 1: enabled = s->fit1x ? YES : NO; break;
                case 2: enabled = s->fit2x ? YES : NO; break;
                case 3: enabled = s->fit3x ? YES : NO; break;
                case 4: enabled = s->fit4x ? YES : NO; break;
                default: break; /* tag 0 (Custom) and separators stay enabled */
            }
            [item setEnabled:enabled];
        }
    }

    if (s_autoScrollingItem)         [s_autoScrollingItem         setState:(s->autoScrolling         ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_showGunsightItem)          [s_showGunsightItem          setState:(s->showGunsight          ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_pillLabelsItem)            [s_pillLabelsItem            setState:(s->showPillLabels        ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_baseLabelsItem)            [s_baseLabelsItem            setState:(s->showBaseLabels        ? NSControlStateValueOn : NSControlStateValueOff)];

    if (s_allowNewPlayersItem)       [s_allowNewPlayersItem       setState:(s->allowNewPlayers       ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_soundEffectsItem)          [s_soundEffectsItem          setState:(s->soundEffects          ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_backgroundSoundItem)       [s_backgroundSoundItem       setState:(s->backgroundSound       ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_soundKeepaliveItem)        [s_soundKeepaliveItem        setState:(s->useSoundKeepalive     ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_volumeMenu) {
        for (NSMenuItem *item in [s_volumeMenu itemArray]) {
            [item setState:(([item tag] == s->soundVolume) ? NSControlStateValueOn : NSControlStateValueOff)];
        }
    }
    if (s_newswireMessagesItem)      [s_newswireMessagesItem      setState:(s->newswireMessages      ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_assistantMessagesItem)     [s_assistantMessagesItem     setState:(s->assistantMessages     ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_aiMessagesItem)            [s_aiMessagesItem            setState:(s->aiMessages            ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_networkStatusMessagesItem) [s_networkStatusMessagesItem setState:(s->networkStatusMessages ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_networkDebugMessagesItem)  [s_networkDebugMessagesItem  setState:(s->networkDebugMessages  ? NSControlStateValueOn : NSControlStateValueOff)];

    if (s_sysInfoItem)               [s_sysInfoItem               setState:(s->sysInfoOpen           ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_netInfoItem)               [s_netInfoItem               setState:(s->netInfoOpen           ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_gameInfoItem)              [s_gameInfoItem              setState:(s->gameInfoOpen          ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_mapOverviewItem)           [s_mapOverviewItem           setState:(s->mapOverviewOpen       ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_overviewInWindowItem)      [s_overviewInWindowItem      setState:(s->overviewInWindow      ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_fullScreenItem)            [s_fullScreenItem            setState:(s->fullScreenOn          ? NSControlStateValueOn : NSControlStateValueOff)];

    if (s_sendMsgItem)               [s_sendMsgItem               setState:(s->sendMsgOpen           ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_playersPanelItem)          [s_playersPanelItem          setState:(s->playersPanelShown     ? NSControlStateValueOn : NSControlStateValueOff)];

    if (s_messageLabelsMenu) {
        for (NSMenuItem *item in [s_messageLabelsMenu itemArray]) {
            [item setState:(([item tag] == s->labelMsg) ? NSControlStateValueOn : NSControlStateValueOff)];
        }
    }

    if (s_tankLabelsMenu) {
        for (NSMenuItem *item in [s_tankLabelsMenu itemArray]) {
            [item setState:(([item tag] == s->labelTank) ? NSControlStateValueOn : NSControlStateValueOff)];
        }
    }

    /* Override after the tank-labels tag-walk: No Own Label is a sibling
     * toggle inside the same submenu (sentinel tag -1) and tracks its own
     * source-of-truth (!labelSelf), not labelTank. */
    if (s_noOwnLabelItem) {
        [s_noOwnLabelItem setState:(s->noOwnLabel ? NSControlStateValueOn : NSControlStateValueOff)];
    }

    /* Alliance items — both answers arrive decided from
     * populateMacMenuState, which asks the same allianceActionState the
     * in-window bars ask. Nothing is re-derived here: combining the flags
     * locally is exactly how this bar came to grey Request whenever you had
     * an ally, and to ignore ranked games, while the in-window bar did
     * neither. Same predicate on both Request items (WinBolo and Players
     * menus) so those two enable and disable together as well. */
    BOOL canRequest = s->canRequest ? YES : NO;
    BOOL canLeave   = s->canLeave   ? YES : NO;
    if (s_winboloRequestAllianceItem) [s_winboloRequestAllianceItem setEnabled:canRequest];
    if (s_winboloLeaveAllianceItem)   [s_winboloLeaveAllianceItem   setEnabled:canLeave];
    if (s_playersRequestAllianceItem) [s_playersRequestAllianceItem setEnabled:canRequest];
    if (s_playersLeaveAllianceItem)   [s_playersLeaveAllianceItem   setEnabled:canLeave];

    /* Vote gating — Return-to-lobby tracks `voteRunning`; Surrender
     * additionally needs exactly two active human teams and that we
     * are on a real team. Both predicates come pre-computed from the
     * MacMenuState producer (populateMacMenuState in sdl3imgui.cpp). */
    if (s_playersVoteBackToLobbyItem) [s_playersVoteBackToLobbyItem setEnabled:(s->voteRunning ? YES : NO)];
    if (s_playersVoteSurrenderItem)   [s_playersVoteSurrenderItem   setEnabled:(s->voteCanSurrender ? YES : NO)];

    /* Map Overview gating — the overview draws the map this game has
     * revealed, so it is selectable only while a game is running. Disabling
     * the item also makes its Cmd+O key equivalent inert. */
    if (s_mapOverviewItem)            [s_mapOverviewItem            setEnabled:(s->mapOverviewEnabled ? YES : NO)];
    if (s_overviewInWindowItem)       [s_overviewInWindowItem       setEnabled:(s->overviewInWindowEnabled ? YES : NO)];

    /* Per-slot view swap. Occupied slots get a WBPlayerSlotView assigned
     * (lazily allocated on first use); empty slots have their view torn
     * down so the numeric placeholder title renders again. The view
     * itself is responsible for redrawing on data changes — setFromSlot:
     * marks it dirty. */
    for (int i = 0; i < 16; i++) {
        NSMenuItem *item = s_playerSlotItems[i];
        if (!item) continue;
        const struct MacPlayerSlot *slot = &s->players[i];
        if (slot->enabled) {
            if (!s_playerSlotViews[i]) {
                WBPlayerSlotView *v = [[WBPlayerSlotView alloc] initWithFrame:NSMakeRect(0, 0, 280, 22)];
                v.slotIndex = i;
                s_playerSlotViews[i] = v;
            }
            [s_playerSlotViews[i] setFromSlot:slot];
            if (item.view != s_playerSlotViews[i]) {
                [item setView:s_playerSlotViews[i]];
            }
            [item setEnabled:YES];
        } else {
            if (item.view != nil) {
                [item setView:nil];
                [item setTitle:[NSString stringWithFormat:@"%d", i + 1]];
            }
            [item setEnabled:NO];
        }
    }

    /* Brains submenu — parent enable, Manual checkmark, and a delta-driven
     * rebuild of the dynamic brain list + Settings entry. Per-frame work
     * is one int compare, one BOOL compare, and a title-equality check
     * per brain item; the heavy rebuild only fires when the brain count
     * or Settings visibility actually changes. */
    if (s_brainsParentItem) {
        [s_brainsParentItem setEnabled:(s->aiActive ? YES : NO)];
    }
    if (s_brainManualItem) {
        [s_brainManualItem setState:(!s->brainRunning ? NSControlStateValueOn : NSControlStateValueOff)];
    }

    BOOL countChanged    = (s_lastBrainCount != s->brainCount);
    BOOL settingsChanged = (s_lastBrainSettingsShown != (s->brainSettingsShown ? YES : NO));

    if (s_brainsMenu && (countChanged || settingsChanged)) {
        /* Drop everything after Manual (index 0) — brain rows, separators,
         * and the Settings entry. The Settings item itself is retained by
         * s_brainSettingsItem, so removal is safe. */
        while ([s_brainsMenu numberOfItems] > 1) {
            [s_brainsMenu removeItemAtIndex:1];
        }

        if (s->brainCount > 0) {
            [s_brainsMenu addItem:[NSMenuItem separatorItem]];
            for (int i = 0; i < s->brainCount; i++) {
                NSMenuItem *bi = [[NSMenuItem alloc]
                    initWithTitle:[NSString stringWithUTF8String:s->brainNames[i]]
                    action:@selector(onBrainItem:)
                    keyEquivalent:@""];
                [bi setTag:i];
                [bi setTarget:g_bridge];
                [s_brainsMenu addItem:bi];
            }
        }

        if (s->brainSettingsShown) {
            [s_brainsMenu addItem:[NSMenuItem separatorItem]];
            [s_brainsMenu addItem:s_brainSettingsItem];
        }

        s_lastBrainCount         = s->brainCount;
        s_lastBrainSettingsShown = s->brainSettingsShown ? YES : NO;
    }

    /* Per-frame: refresh titles + checkmarks on existing brain rows. Names
     * can change if luaBrainLoadBrains rescans, and the running-brain
     * checkmark mirrors brainRunIdx. */
    if (s_brainsMenu && s->brainCount > 0) {
        for (NSMenuItem *it in [s_brainsMenu itemArray]) {
            if (it.action != @selector(onBrainItem:)) continue;
            NSInteger tag = it.tag;
            if (tag < 0 || tag >= s->brainCount) continue;
            NSString *currentTitle = [NSString stringWithUTF8String:s->brainNames[(int)tag]];
            if (![it.title isEqualToString:currentTitle]) {
                [it setTitle:currentTitle];
            }
            BOOL active = s->brainRunning && (tag == s->brainRunIdx);
            [it setState:(active ? NSControlStateValueOn : NSControlStateValueOff)];
        }
    }
}
