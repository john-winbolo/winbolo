#import <Cocoa/Cocoa.h>

#include <string.h>

#include "mac_menubar.h"
#include "../../gui/lang.h"

extern "C" {
#include "../mapeditor_imgui.h"
}
#include "../mapeditor_wbn_open.h"  /* meWbnOpenAvailable — empty unless MAPEDITOR_WBN_OPEN */

#define LANG_STR(id) ([NSString stringWithUTF8String:langGetText(id)])

@class MEMenuBridge;
static MEMenuBridge *s_me_bridge        = nil;
static NSMenu       *s_me_savedMainMenu = nil;
/* Embedded case: the host's app-menu submenu (NSMenu can't be the
 * submenu of two NSMenuItems at once). Both pointers are non-nil iff a
 * detach occurred and need to be reattached by uninstall. */
static NSMenuItem   *s_me_hostAppItem    = nil;
static NSMenu       *s_me_hostAppSubmenu = nil;
/* The app menu's Preferences item while the editor is up. Taken over from
 * the host: the item plus the target and action to hand back. Added by the
 * editor: the item alone, removed again by uninstall. */
static NSMenuItem   *s_me_prefsItem       = nil;
static id            s_me_prefsHostTarget = nil;
static SEL           s_me_prefsHostAction = NULL;
static BOOL          s_me_prefsAdded      = NO;
/* Standalone-only state used by the File menu's accelerator-resolution
 * step in refresh. */
static BOOL          s_me_embedded       = NO;

/* ----------------------------------------------------------------------------
 * Pending action buffer.
 *
 * NSMenu callbacks fire asynchronously w.r.t. the editor's main loop.
 * They can land at any point between two mapEditorRun frames, so they
 * write into this shared buffer and the main loop drains it once per
 * frame via me_mac_menubar_consume_actions().
 *
 * Booleans are saturating-ORed (multiple clicks of the same item across
 * a single frame collapse to one fire — correct for all of our flags).
 * The integer fields take last-write-wins semantics: if the user opens
 * two recent-files in the same frame, the latter wins. That mirrors
 * what would happen in the in-window menu (each click finalizes the
 * frame and would replace the previous pick).
 * ---------------------------------------------------------------------------- */
static MapEditorMenuAction s_me_pending;
static void me_pending_reset(void) {
    memset(&s_me_pending, 0, sizeof(s_me_pending));
    s_me_pending.openRecentIndex = -1;
    s_me_pending.zoomSetIndex    = 0;
}

/* ----------------------------------------------------------------------------
 * Cached NSMenuItem pointers that me_mac_menubar_refresh() walks each frame.
 * ---------------------------------------------------------------------------- */
static NSMenuItem *s_me_returnItem        = nil;   /* embedded only */
static NSMenuItem *s_me_undoItem          = nil;
static NSMenuItem *s_me_redoItem          = nil;
static NSMenuItem *s_me_cutItem           = nil;
static NSMenuItem *s_me_copyItem          = nil;
static NSMenuItem *s_me_mirrorHItem       = nil;
static NSMenuItem *s_me_mirrorVItem       = nil;
static NSMenuItem *s_me_rotate90Item      = nil;
static NSMenuItem *s_me_rotate180Item     = nil;
static NSMenuItem *s_me_showGridItem      = nil;
static NSMenuItem *s_me_showMinesItem     = nil;
static NSMenuItem *s_me_showPillRangesItem = nil;
static NSMenuItem *s_me_winTerrainItem    = nil;
static NSMenuItem *s_me_winToolsItem      = nil;
static NSMenuItem *s_me_winInspectorItem  = nil;
static NSMenuItem *s_me_winObjectsItem    = nil;
static NSMenuItem *s_me_winOverviewItem   = nil;
static NSMenuItem *s_me_winStatsItem      = nil;
static NSMenuItem *s_me_winStampLibItem   = nil;
static NSMenuItem *s_me_winScenarioItem   = nil;
static NSMenu     *s_me_recentMenu        = nil;
static NSMenuItem *s_me_recentRootItem    = nil;
static NSMenu     *s_me_zoomMenu          = nil;
static NSMenuItem *s_me_zoomInItem        = nil;
static NSMenuItem *s_me_zoomOutItem       = nil;

/* Track the last-rendered Recent and Zoom contents so the refresh
 * rebuilds only when the underlying list actually changes. The zoom
 * comparison is loose — count + index covers every change we care
 * about; the values array is borrowed from a const table that won't
 * actually mutate. */
static int s_me_lastRecentCount = -1;
static int s_me_lastZoomCount   = -1;
/* Track previous hasSelection so the Mirror/Rotate scope labels are
 * only rebuilt when the selection toggle changes (cheap, but skipping
 * the NSString allocation when the selection is stable is cheaper). */
static int s_me_lastScopeIsSelection = -1; /* -1 forces first refresh */

@interface MEMenuBridge : NSObject
- (void)onQuit:(id)sender;
- (void)onNew:(id)sender;
- (void)onOpen:(id)sender;
#ifdef MAPEDITOR_WBN_OPEN
- (void)onOpenWbn:(id)sender;
#endif
- (void)onSave:(id)sender;
- (void)onSaveAs:(id)sender;
- (void)onExportPNG:(id)sender;
- (void)onExit:(id)sender;
- (void)onOpenRecent:(id)sender;
- (void)onUndo:(id)sender;
- (void)onRedo:(id)sender;
- (void)onCut:(id)sender;
- (void)onCopy:(id)sender;
- (void)onPaste:(id)sender;
- (void)onGenerate:(id)sender;
- (void)onText:(id)sender;
- (void)onImageImport:(id)sender;
- (void)onValidate:(id)sender;
- (void)onMirrorH:(id)sender;
- (void)onMirrorV:(id)sender;
- (void)onRotate90:(id)sender;
- (void)onRotate180:(id)sender;
- (void)onZoomStep:(id)sender;
- (void)onZoomIn:(id)sender;
- (void)onZoomOut:(id)sender;
- (void)onCenter:(id)sender;
- (void)onPointStarts:(id)sender;
- (void)onToggleGrid:(id)sender;
- (void)onToggleMines:(id)sender;
- (void)onTogglePillRanges:(id)sender;
- (void)onToggleTerrain:(id)sender;
- (void)onToggleTools:(id)sender;
- (void)onToggleInspector:(id)sender;
- (void)onToggleObjects:(id)sender;
- (void)onToggleOverview:(id)sender;
- (void)onToggleStats:(id)sender;
- (void)onToggleStampLibrary:(id)sender;
- (void)onToggleScenario:(id)sender;
- (void)onSettings:(id)sender;
@end

@implementation MEMenuBridge
- (void)onQuit:(id)sender {
    (void)sender;
    /* Quit and Return to Menu both leave the editor; only this one goes on to
     * end the application. */
    s_me_pending.wantExit = true;
    s_me_pending.wantQuitApp = true;
}
- (void)onNew:(id)sender { (void)sender; s_me_pending.wantNew = true; }
- (void)onOpen:(id)sender { (void)sender; s_me_pending.wantOpen = true; }
#ifdef MAPEDITOR_WBN_OPEN
- (void)onOpenWbn:(id)sender { (void)sender; s_me_pending.wantOpenWbn = true; }
#endif
- (void)onSave:(id)sender { (void)sender; s_me_pending.wantSave = true; }
- (void)onSaveAs:(id)sender { (void)sender; s_me_pending.wantSaveAs = true; }
- (void)onExportPNG:(id)sender { (void)sender; s_me_pending.wantExportPNG = true; }
- (void)onExit:(id)sender { (void)sender; s_me_pending.wantExit = true; }
- (void)onOpenRecent:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    s_me_pending.openRecentIndex = (int)[item tag];
}
- (void)onUndo:(id)sender { (void)sender; s_me_pending.wantUndo = true; }
- (void)onRedo:(id)sender { (void)sender; s_me_pending.wantRedo = true; }
- (void)onCut:(id)sender { (void)sender; s_me_pending.wantCut = true; }
- (void)onCopy:(id)sender { (void)sender; s_me_pending.wantCopy = true; }
- (void)onPaste:(id)sender { (void)sender; s_me_pending.wantPaste = true; }
- (void)onGenerate:(id)sender { (void)sender; s_me_pending.wantGenerate = true; }
- (void)onText:(id)sender { (void)sender; s_me_pending.wantText = true; }
- (void)onImageImport:(id)sender { (void)sender; s_me_pending.wantImageImport = true; }
- (void)onValidate:(id)sender { (void)sender; s_me_pending.wantValidate = true; }
- (void)onMirrorH:(id)sender { (void)sender; s_me_pending.wantSymMirrorH = true; }
- (void)onMirrorV:(id)sender { (void)sender; s_me_pending.wantSymMirrorV = true; }
- (void)onRotate90:(id)sender { (void)sender; s_me_pending.wantSymRotate90 = true; }
- (void)onRotate180:(id)sender { (void)sender; s_me_pending.wantSymRotate180 = true; }
- (void)onZoomStep:(id)sender {
    NSMenuItem *item = (NSMenuItem *)sender;
    s_me_pending.wantZoomSet  = true;
    s_me_pending.zoomSetIndex = (int)[item tag];
}
- (void)onZoomIn:(id)sender { (void)sender; s_me_pending.wantZoomIn = true; }
- (void)onZoomOut:(id)sender { (void)sender; s_me_pending.wantZoomOut = true; }
- (void)onCenter:(id)sender { (void)sender; s_me_pending.wantCenter = true; }
- (void)onPointStarts:(id)sender { (void)sender; s_me_pending.wantPointStarts = true; }
- (void)onToggleGrid:(id)sender { (void)sender; s_me_pending.toggleGrid = true; }
- (void)onToggleMines:(id)sender { (void)sender; s_me_pending.toggleMines = true; }
- (void)onTogglePillRanges:(id)sender { (void)sender; s_me_pending.togglePillRanges = true; }
- (void)onToggleTerrain:(id)sender { (void)sender; s_me_pending.wantToggleTerrain = true; }
- (void)onToggleTools:(id)sender { (void)sender; s_me_pending.wantToggleTools = true; }
- (void)onToggleInspector:(id)sender { (void)sender; s_me_pending.wantToggleInspector = true; }
- (void)onToggleObjects:(id)sender { (void)sender; s_me_pending.wantToggleObjects = true; }
- (void)onToggleOverview:(id)sender { (void)sender; s_me_pending.wantToggleOverview = true; }
- (void)onToggleStats:(id)sender { (void)sender; s_me_pending.wantToggleStats = true; }
- (void)onToggleStampLibrary:(id)sender { (void)sender; s_me_pending.wantToggleStampLibrary = true; }
- (void)onToggleScenario:(id)sender { (void)sender; s_me_pending.wantToggleScenario = true; }
- (void)onSettings:(id)sender { (void)sender; s_me_pending.wantSettings = true; }
@end

/* ----------------------------------------------------------------------------
 * App-menu builders.
 *
 * Standalone owns its own app menu (About is omitted — Map Editor has
 * no in-app About dialog; if one is added later, wire it through a new
 * `onAbout:` trampoline + STR_MENU_ABOUT_ME). Embedded reuses WinBolo's
 * app menu via the host-detach trick lifted from the Log Viewer shim.
 * ---------------------------------------------------------------------------- */
static void me_buildStandaloneAppMenu(NSMenu *mainMenu) {
    /* AppKit replaces the first menu item's title with the running
     * bundle's name (CFBundleName), so an empty title here is correct
     * and "Map Editor" is what users see. */
    NSMenuItem *appItem = [mainMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@""];
    [appItem setSubmenu:appMenu];

    /* TODO: Map Editor has no About dialog yet. When one is added,
     * insert an "About Map Editor" item here wired to a new
     * -[MEMenuBridge onAbout:] + STR_MENU_ABOUT_ME. */

    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_SERVICES)];
    NSMenuItem *servicesItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_SERVICES) action:nil keyEquivalent:@""];
    [servicesItem setSubmenu:servicesMenu];
    [appMenu addItem:servicesItem];
    [NSApp setServicesMenu:servicesMenu];

    [appMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *hideItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_HIDE_ME)
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

    /* Quit ⌘Q routes through the editor's wantExit flow (which honours
     * the unsaved-changes modal) rather than AppKit's -terminate:. */
    NSMenuItem *quitItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_QUIT_ME)
        action:@selector(onQuit:)
        keyEquivalent:@"q"];
    [quitItem setTarget:s_me_bridge];
    [appMenu addItem:quitItem];
}

static void me_reuseHostAppMenu(NSMenu *mainMenu, NSMenu *hostMain) {
    NSMenuItem *appItem = [mainMenu addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenuItem *hostApp = (hostMain && [hostMain numberOfItems] > 0) ? [hostMain itemAtIndex:0] : nil;
    NSMenu *hostAppSub = [hostApp submenu];
    if (hostApp && hostAppSub) {
        [hostApp setSubmenu:nil];
        [appItem setSubmenu:hostAppSub];
        s_me_hostAppItem    = hostApp;
        s_me_hostAppSubmenu = hostAppSub;
    } else {
        [appItem setSubmenu:[[NSMenu alloc] initWithTitle:@""]];
    }
}

/* Point the app menu's Preferences item at the editor. WinBolo's own item
 * (onPreferences:) opens the in-game settings overlay, which the editor does
 * not draw, so it is taken over for the editor's run. Before a game has
 * started WinBolo's menu bar is not installed yet and the app menu is SDL's,
 * which has no such item, so one is added. */
static void me_attachSettingsItem(NSMenu *appMenu) {
    if (appMenu == nil) return;
    for (NSMenuItem *it in [appMenu itemArray]) {
        if ([it action] == NSSelectorFromString(@"onPreferences:")) {
            s_me_prefsItem       = it;
            s_me_prefsHostTarget = [it target];
            s_me_prefsHostAction = [it action];
            s_me_prefsAdded      = NO;
            [it setTarget:s_me_bridge];
            [it setAction:@selector(onSettings:)];
            return;
        }
    }
    NSMenuItem *prefsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_PREFERENCES)
        action:@selector(onSettings:)
        keyEquivalent:@","];
    [prefsItem setTarget:s_me_bridge];
    /* After About when there is one, as in WinBolo's own app menu. */
    NSInteger at = ([appMenu numberOfItems] > 0) ? 1 : 0;
    [appMenu insertItem:prefsItem atIndex:at];
    s_me_prefsItem  = prefsItem;
    s_me_prefsAdded = YES;
}

static void me_detachSettingsItem(void) {
    if (s_me_prefsItem == nil) return;
    if (s_me_prefsAdded) {
        [[s_me_prefsItem menu] removeItem:s_me_prefsItem];
    } else {
        [s_me_prefsItem setTarget:s_me_prefsHostTarget];
        [s_me_prefsItem setAction:s_me_prefsHostAction];
    }
    s_me_prefsItem       = nil;
    s_me_prefsHostTarget = nil;
    s_me_prefsHostAction = NULL;
    s_me_prefsAdded      = NO;
}

/* ----------------------------------------------------------------------------
 * me_mac_menubar_install — build and install.
 * ---------------------------------------------------------------------------- */
void me_mac_menubar_install(struct SDL_Window *win, bool hasSettings) {
    (void)win;

    if (s_me_bridge == nil) {
        s_me_bridge = [[MEMenuBridge alloc] init];
    }

    me_pending_reset();

    NSMenu *previousMain = [NSApp mainMenu];
    s_me_savedMainMenu = previousMain;
    BOOL embedded = (previousMain != nil && [previousMain numberOfItems] > 0);
    s_me_embedded = embedded;

    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];

    if (embedded) {
        me_reuseHostAppMenu(mainMenu, previousMain);
    } else {
        me_buildStandaloneAppMenu(mainMenu);
    }
    if (hasSettings) {
        me_attachSettingsItem([[mainMenu itemAtIndex:0] submenu]);
    }

    /* ------------------------------------------------------------------
     * File menu — New / Open / Recent / Save / Save As / Export PNG /
     * Return-or-Exit. The Recent submenu is populated by refresh; the
     * empty placeholder here just reserves the slot. Export PNG is
     * gated to desktop only, matching the in-window menu (this shim is
     * already gated off iOS at the CMake level, so we don't re-gate
     * for __EMSCRIPTEN__ — that's a different build entirely).
     * ------------------------------------------------------------------ */
    NSMenuItem *fileItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_FILE) action:nil keyEquivalent:@""];
    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_FILE)];
    [fileItem setSubmenu:fileMenu];
    [fileMenu setAutoenablesItems:NO];

    NSMenuItem *newItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MENU_NEW)
        action:@selector(onNew:)
        keyEquivalent:@"n"];
    [newItem setTarget:s_me_bridge];
    [fileMenu addItem:newItem];

    NSMenuItem *openItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_OPEN)
        action:@selector(onOpen:)
        keyEquivalent:@"o"];
    [openItem setTarget:s_me_bridge];
    [fileMenu addItem:openItem];

#ifdef MAPEDITOR_WBN_OPEN
    /* Open from WinBolo.net — no key equivalent. */
    NSMenuItem *openWbnItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_MENU_OPEN_WBN)
        action:@selector(onOpenWbn:)
        keyEquivalent:@""];
    [openWbnItem setTarget:s_me_bridge];
    /* Availability is fixed before mapEditorRun installs the menu (the
     * standalone editor sets it from httpCreate at startup), so
     * install-time is enough; fileMenu has autoenablesItems off. */
    [openWbnItem setEnabled:meWbnOpenAvailable()];
    [fileMenu addItem:openWbnItem];
#endif

    /* Recent Files submenu — content rebuilt by refresh. */
    NSMenuItem *recentRoot = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_RECENT)
        action:nil keyEquivalent:@""];
    NSMenu *recentMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MAPEDIT_MENU_RECENT)];
    [recentMenu setAutoenablesItems:NO];
    [recentRoot setSubmenu:recentMenu];
    [recentRoot setEnabled:NO];   /* enabled by refresh iff numRecent > 0 */
    [fileMenu addItem:recentRoot];
    s_me_recentMenu     = recentMenu;
    s_me_recentRootItem = recentRoot;

    [fileMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *saveItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_SAVE)
        action:@selector(onSave:)
        keyEquivalent:@"s"];
    [saveItem setTarget:s_me_bridge];
    [fileMenu addItem:saveItem];

    NSMenuItem *saveAsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_SAVEAS)
        action:@selector(onSaveAs:)
        keyEquivalent:@"s"];
    [saveAsItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [saveAsItem setTarget:s_me_bridge];
    [fileMenu addItem:saveAsItem];

    [fileMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *exportItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_EXPORT)
        action:@selector(onExportPNG:)
        keyEquivalent:@""];
    [exportItem setTarget:s_me_bridge];
    [fileMenu addItem:exportItem];

    /* Embedded gets "Return to Menu" in the File menu; standalone gets
     * no File-menu exit (Quit lives in the app menu at ⌘Q). */
    if (embedded) {
        [fileMenu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *returnItem = [[NSMenuItem alloc]
            initWithTitle:LANG_STR(STR_MAPEDIT_MENU_RETURN)
            action:@selector(onExit:)
            keyEquivalent:@""];
        [returnItem setTarget:s_me_bridge];
        [fileMenu addItem:returnItem];
        s_me_returnItem = returnItem;
    } else {
        s_me_returnItem = nil;
    }

    /* ------------------------------------------------------------------
     * Edit menu — Undo / Redo / Cut / Copy / Paste. Undo/Redo and
     * Cut/Copy are enable-gated by refresh.
     * ------------------------------------------------------------------ */
    NSMenuItem *editItem = [mainMenu addItemWithTitle:LANG_STR(STR_MENU_EDIT) action:nil keyEquivalent:@""];
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MENU_EDIT)];
    [editItem setSubmenu:editMenu];
    [editMenu setAutoenablesItems:NO];

    NSMenuItem *undoItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_UNDO)
        action:@selector(onUndo:)
        keyEquivalent:@"z"];
    [undoItem setTarget:s_me_bridge];
    [editMenu addItem:undoItem];
    s_me_undoItem = undoItem;

    /* Mac convention: Redo is ⌘⇧Z. The in-window non-Mac menu shows
     * "Ctrl+Y" — that label is untouched, this is the Mac sibling. */
    NSMenuItem *redoItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_REDO)
        action:@selector(onRedo:)
        keyEquivalent:@"z"];
    [redoItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [redoItem setTarget:s_me_bridge];
    [editMenu addItem:redoItem];
    s_me_redoItem = redoItem;

    [editMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *cutItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_CUT)
        action:@selector(onCut:)
        keyEquivalent:@"x"];
    [cutItem setTarget:s_me_bridge];
    [editMenu addItem:cutItem];
    s_me_cutItem = cutItem;

    NSMenuItem *copyItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_COPY)
        action:@selector(onCopy:)
        keyEquivalent:@"c"];
    [copyItem setTarget:s_me_bridge];
    [editMenu addItem:copyItem];
    s_me_copyItem = copyItem;

    NSMenuItem *pasteItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_PASTE)
        action:@selector(onPaste:)
        keyEquivalent:@"v"];
    [pasteItem setTarget:s_me_bridge];
    [editMenu addItem:pasteItem];

    /* ------------------------------------------------------------------
     * Map menu — Random Map / Text / Import Image / Validate /
     * Mirror H,V / Rotate 90,180. The mirror/rotate items have a
     * {scope} placeholder in their title that refresh fills in.
     * ------------------------------------------------------------------ */
    NSMenuItem *mapItem = [mainMenu addItemWithTitle:LANG_STR(STR_MAPEDIT_MENU_MAP) action:nil keyEquivalent:@""];
    NSMenu *mapMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MAPEDIT_MENU_MAP)];
    [mapItem setSubmenu:mapMenu];
    [mapMenu setAutoenablesItems:NO];

    NSMenuItem *genItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_RANDOMMAP)
        action:@selector(onGenerate:)
        keyEquivalent:@""];
    [genItem setTarget:s_me_bridge];
    [mapMenu addItem:genItem];

    NSMenuItem *textItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_TEXT)
        action:@selector(onText:)
        keyEquivalent:@""];
    [textItem setTarget:s_me_bridge];
    [mapMenu addItem:textItem];

    NSMenuItem *imgItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_IMPORTIMG)
        action:@selector(onImageImport:)
        keyEquivalent:@""];
    [imgItem setTarget:s_me_bridge];
    [mapMenu addItem:imgItem];

    NSMenuItem *validateItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_VALIDATE)
        action:@selector(onValidate:)
        keyEquivalent:@"v"];
    [validateItem setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [validateItem setTarget:s_me_bridge];
    [mapMenu addItem:validateItem];

    [mapMenu addItem:[NSMenuItem separatorItem]];

    /* Initial titles are placeholders — refresh overwrites them with
     * the localized "Mirror Horizontal (selection)" / "(full map)"
     * forms once it sees the first MeMenuState. */
    NSMenuItem *mirrorHItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_MIRROR_H)
        action:@selector(onMirrorH:) keyEquivalent:@""];
    [mirrorHItem setTarget:s_me_bridge];
    [mapMenu addItem:mirrorHItem];
    s_me_mirrorHItem = mirrorHItem;

    NSMenuItem *mirrorVItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_MIRROR_V)
        action:@selector(onMirrorV:) keyEquivalent:@""];
    [mirrorVItem setTarget:s_me_bridge];
    [mapMenu addItem:mirrorVItem];
    s_me_mirrorVItem = mirrorVItem;

    NSMenuItem *rot90Item = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_ROTATE_90)
        action:@selector(onRotate90:) keyEquivalent:@""];
    [rot90Item setTarget:s_me_bridge];
    [mapMenu addItem:rot90Item];
    s_me_rotate90Item = rot90Item;

    NSMenuItem *rot180Item = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_ROTATE_180)
        action:@selector(onRotate180:) keyEquivalent:@""];
    [rot180Item setTarget:s_me_bridge];
    [mapMenu addItem:rot180Item];
    s_me_rotate180Item = rot180Item;

    /* ------------------------------------------------------------------
     * Options menu — Zoom submenu / Center Map / Point Start Points /
     * Show Grid / Mines / Pill Ranges. The Show* toggles take ⌘G and
     * ⌘M intentionally — binding the bare "G" / "M" the in-window
     * menu uses would hijack typing in any text field.
     * ------------------------------------------------------------------ */
    NSMenuItem *optionsItem = [mainMenu addItemWithTitle:LANG_STR(STR_MAPEDIT_MENU_OPTIONS) action:nil keyEquivalent:@""];
    NSMenu *optionsMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MAPEDIT_MENU_OPTIONS)];
    [optionsItem setSubmenu:optionsMenu];
    [optionsMenu setAutoenablesItems:NO];

    /* Zoom submenu — per-step items rebuilt by refresh on count change. */
    NSMenuItem *zoomRoot = [optionsMenu addItemWithTitle:LANG_STR(STR_LV_ZOOM) action:nil keyEquivalent:@""];
    NSMenu *zoomMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_LV_ZOOM)];
    [zoomRoot setSubmenu:zoomMenu];
    [zoomMenu setAutoenablesItems:NO];
    s_me_zoomMenu = zoomMenu;

    NSMenuItem *zoomInItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_ZOOM_IN)
        action:@selector(onZoomIn:)
        keyEquivalent:@"+"];
    [zoomInItem setTarget:s_me_bridge];
    s_me_zoomInItem = zoomInItem;

    NSMenuItem *zoomOutItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_LV_ZOOM_OUT)
        action:@selector(onZoomOut:)
        keyEquivalent:@"-"];
    [zoomOutItem setTarget:s_me_bridge];
    s_me_zoomOutItem = zoomOutItem;
    /* Zoom In/Out items are attached to the submenu by the first refresh. */

    [optionsMenu addItem:[NSMenuItem separatorItem]];

    /* Center Map — keep Home key (no Cmd). NSMenu doesn't surface
     * the Home key shortcut directly; we leave the key handling to
     * the existing SDL_EVENT_KEY_DOWN path in mapeditor.c and only
     * provide the menu item. */
    NSMenuItem *centerItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_CENTER)
        action:@selector(onCenter:)
        keyEquivalent:@""];
    [centerItem setTarget:s_me_bridge];
    [optionsMenu addItem:centerItem];

    NSMenuItem *pointStartsItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_POINT_STARTS)
        action:@selector(onPointStarts:)
        keyEquivalent:@""];
    [pointStartsItem setTarget:s_me_bridge];
    [optionsMenu addItem:pointStartsItem];

    [optionsMenu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *showGridItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_SHOWGRID)
        action:@selector(onToggleGrid:)
        keyEquivalent:@"g"];
    [showGridItem setTarget:s_me_bridge];
    [optionsMenu addItem:showGridItem];
    s_me_showGridItem = showGridItem;

    NSMenuItem *showMinesItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_SHOWMINES)
        action:@selector(onToggleMines:)
        keyEquivalent:@"m"];
    [showMinesItem setTarget:s_me_bridge];
    [optionsMenu addItem:showMinesItem];
    s_me_showMinesItem = showMinesItem;

    NSMenuItem *showPillRangesItem = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_MENU_SHOWPILLRANGES)
        action:@selector(onTogglePillRanges:)
        keyEquivalent:@""];
    [showPillRangesItem setTarget:s_me_bridge];
    [optionsMenu addItem:showPillRangesItem];
    s_me_showPillRangesItem = showPillRangesItem;

    /* ------------------------------------------------------------------
     * Window menu — panel toggles. Bindings are ⌘1..⌘7 matching the
     * in-window labels, but note the order: the in-window list places
     * Stats before Stamp Library, yet Stamp Library takes ⌘6 and Stats
     * takes ⌘7. We preserve that binding map even though it looks
     * out-of-order in the menu UI.
     * ------------------------------------------------------------------ */
    NSMenuItem *windowItem = [mainMenu addItemWithTitle:LANG_STR(STR_MAPEDIT_MENU_WINDOW) action:nil keyEquivalent:@""];
    NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:LANG_STR(STR_MAPEDIT_MENU_WINDOW)];
    [windowItem setSubmenu:windowMenu];
    [windowMenu setAutoenablesItems:NO];

    NSMenuItem *winTerrain = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_WIN_TERRAIN)
        action:@selector(onToggleTerrain:)
        keyEquivalent:@"1"];
    [winTerrain setTarget:s_me_bridge];
    [windowMenu addItem:winTerrain];
    s_me_winTerrainItem = winTerrain;

    NSMenuItem *winTools = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_WIN_TOOLS)
        action:@selector(onToggleTools:)
        keyEquivalent:@"2"];
    [winTools setTarget:s_me_bridge];
    [windowMenu addItem:winTools];
    s_me_winToolsItem = winTools;

    NSMenuItem *winInspector = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_WIN_INSPECTOR)
        action:@selector(onToggleInspector:)
        keyEquivalent:@"3"];
    [winInspector setTarget:s_me_bridge];
    [windowMenu addItem:winInspector];
    s_me_winInspectorItem = winInspector;

    NSMenuItem *winObjects = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_WIN_OBJECTS)
        action:@selector(onToggleObjects:)
        keyEquivalent:@"4"];
    [winObjects setTarget:s_me_bridge];
    [windowMenu addItem:winObjects];
    s_me_winObjectsItem = winObjects;

    NSMenuItem *winOverview = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_WIN_OVERVIEW)
        action:@selector(onToggleOverview:)
        keyEquivalent:@"5"];
    [winOverview setTarget:s_me_bridge];
    [windowMenu addItem:winOverview];
    s_me_winOverviewItem = winOverview;

    NSMenuItem *winStampLib = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_WIN_STAMP_LIB)
        action:@selector(onToggleStampLibrary:)
        keyEquivalent:@"6"];
    [winStampLib setTarget:s_me_bridge];
    [windowMenu addItem:winStampLib];
    s_me_winStampLibItem = winStampLib;

    NSMenuItem *winStats = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_WIN_STATS)
        action:@selector(onToggleStats:)
        keyEquivalent:@"7"];
    [winStats setTarget:s_me_bridge];
    [windowMenu addItem:winStats];
    s_me_winStatsItem = winStats;

    NSMenuItem *winScenario = [[NSMenuItem alloc]
        initWithTitle:LANG_STR(STR_MAPEDIT_SCENARIO_TITLE)
        action:@selector(onToggleScenario:)
        keyEquivalent:@"8"];
    [winScenario setTarget:s_me_bridge];
    [windowMenu addItem:winScenario];
    s_me_winScenarioItem = winScenario;

    /* Force first-refresh rebuilds for the dynamic submenus. */
    s_me_lastRecentCount      = -1;
    s_me_lastZoomCount        = -1;
    s_me_lastScopeIsSelection = -1;

    [NSApp setMainMenu:mainMenu];
}

/* ----------------------------------------------------------------------------
 * me_mac_menubar_uninstall — restore.
 * ---------------------------------------------------------------------------- */
void me_mac_menubar_uninstall(void) {
    /* Before the app menu goes back to the host, while the item is still
     * where it was found or put. */
    me_detachSettingsItem();

    /* Reattach the host's app submenu to its original NSMenuItem so
     * the host gets its app menu back as soon as NSApp swaps mainMenu. */
    if (s_me_hostAppItem && s_me_hostAppSubmenu) {
        NSMenu *meMain = [NSApp mainMenu];
        if (meMain && [meMain numberOfItems] > 0) {
            [[meMain itemAtIndex:0] setSubmenu:nil];
        }
        [s_me_hostAppItem setSubmenu:s_me_hostAppSubmenu];
        s_me_hostAppItem    = nil;
        s_me_hostAppSubmenu = nil;
    }

    if (s_me_savedMainMenu) {
        [NSApp setMainMenu:s_me_savedMainMenu];
    } else {
        /* Standalone exits right after this; an empty stub avoids
         * AppKit seeing a stale pointer if anything queries mainMenu
         * between teardown and process exit. */
        [NSApp setMainMenu:[[NSMenu alloc] initWithTitle:@""]];
    }
    s_me_savedMainMenu = nil;

    s_me_returnItem        = nil;
    s_me_undoItem          = nil;
    s_me_redoItem          = nil;
    s_me_cutItem           = nil;
    s_me_copyItem          = nil;
    s_me_mirrorHItem       = nil;
    s_me_mirrorVItem       = nil;
    s_me_rotate90Item      = nil;
    s_me_rotate180Item     = nil;
    s_me_showGridItem      = nil;
    s_me_showMinesItem     = nil;
    s_me_showPillRangesItem = nil;
    s_me_winTerrainItem    = nil;
    s_me_winToolsItem      = nil;
    s_me_winInspectorItem  = nil;
    s_me_winObjectsItem    = nil;
    s_me_winOverviewItem   = nil;
    s_me_winStatsItem      = nil;
    s_me_winStampLibItem   = nil;
    s_me_winScenarioItem   = nil;
    s_me_recentMenu        = nil;
    s_me_recentRootItem    = nil;
    s_me_zoomMenu          = nil;
    s_me_zoomInItem        = nil;
    s_me_zoomOutItem       = nil;

    s_me_lastRecentCount      = -1;
    s_me_lastZoomCount        = -1;
    s_me_lastScopeIsSelection = -1;
    s_me_embedded             = NO;
}

/* ----------------------------------------------------------------------------
 * me_mac_menubar_refresh — push state into the menu each frame.
 * ---------------------------------------------------------------------------- */
static NSString *me_basename(const char *path) {
    if (!path) return @"";
    const char *slash = strrchr(path, '/');
    if (!slash) slash = strrchr(path, '\\');
    const char *name = slash ? slash + 1 : path;
    return [NSString stringWithUTF8String:name];
}

void me_mac_menubar_refresh(const struct MeMenuState *s) {
    if (!s) return;

    /* Edit menu enable gates. */
    if (s_me_undoItem) [s_me_undoItem setEnabled:(s->canUndo ? YES : NO)];
    if (s_me_redoItem) [s_me_redoItem setEnabled:(s->canRedo ? YES : NO)];
    if (s_me_cutItem)  [s_me_cutItem  setEnabled:(s->hasSelection ? YES : NO)];
    if (s_me_copyItem) [s_me_copyItem setEnabled:(s->hasSelection ? YES : NO)];

    /* Map > Mirror / Rotate scope labels. langGetTextFmt() returns a
     * pointer into a small thread-local ring, so we re-fetch each
     * item's localized title before copying it into the NSMenuItem
     * (subsequent calls would overwrite earlier ring slots if we
     * read all four into raw const char* and dereferenced lazily). */
    int scopeIsSel = s->hasSelection ? 1 : 0;
    if (scopeIsSel != s_me_lastScopeIsSelection) {
        MessageArgs scopeArgs;
        memset(&scopeArgs, 0, sizeof(scopeArgs));
        const char *scope = langGetText(scopeIsSel
                                        ? STR_MAPEDIT_SCOPE_SELECTION
                                        : STR_MAPEDIT_SCOPE_FULLMAP);
        snprintf(scopeArgs.string1, sizeof(scopeArgs.string1), "%s", scope);
        if (s_me_mirrorHItem) {
            NSString *t = [NSString stringWithUTF8String:langGetTextFmt(STR_MAPEDIT_MENU_MIRROR_H, &scopeArgs)];
            [s_me_mirrorHItem setTitle:t];
        }
        if (s_me_mirrorVItem) {
            NSString *t = [NSString stringWithUTF8String:langGetTextFmt(STR_MAPEDIT_MENU_MIRROR_V, &scopeArgs)];
            [s_me_mirrorVItem setTitle:t];
        }
        if (s_me_rotate90Item) {
            NSString *t = [NSString stringWithUTF8String:langGetTextFmt(STR_MAPEDIT_MENU_ROTATE_90, &scopeArgs)];
            [s_me_rotate90Item setTitle:t];
        }
        if (s_me_rotate180Item) {
            NSString *t = [NSString stringWithUTF8String:langGetTextFmt(STR_MAPEDIT_MENU_ROTATE_180, &scopeArgs)];
            [s_me_rotate180Item setTitle:t];
        }
        s_me_lastScopeIsSelection = scopeIsSel;
    }

    /* Options checkmarks. */
    if (s_me_showGridItem)
        [s_me_showGridItem setState:(s->showGrid ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_showMinesItem)
        [s_me_showMinesItem setState:(s->showMines ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_showPillRangesItem)
        [s_me_showPillRangesItem setState:(s->showPillRanges ? NSControlStateValueOn : NSControlStateValueOff)];

    /* Window menu checkmarks. */
    if (s_me_winTerrainItem)
        [s_me_winTerrainItem setState:(s->showTerrain ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_winToolsItem)
        [s_me_winToolsItem setState:(s->showTools ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_winInspectorItem)
        [s_me_winInspectorItem setState:(s->showInspector ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_winObjectsItem)
        [s_me_winObjectsItem setState:(s->showObjects ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_winOverviewItem)
        [s_me_winOverviewItem setState:(s->showOverview ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_winStatsItem)
        [s_me_winStatsItem setState:(s->showStats ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_winStampLibItem)
        [s_me_winStampLibItem setState:(s->showStampLibrary ? NSControlStateValueOn : NSControlStateValueOff)];
    if (s_me_winScenarioItem)
        [s_me_winScenarioItem setState:(s->showScenario ? NSControlStateValueOn : NSControlStateValueOff)];

    /* Recent Files — rebuild whenever the count changes. The slot's
     * title shows just the basename; the full path goes into the
     * tooltip (NSMenuItem.toolTip), matching the in-window menu's
     * SetTooltip behaviour. We also rebuild if any pointer differs,
     * which catches the same-count-different-list edge case (e.g.
     * touch one of N recents — it moves to the head). */
    if (s_me_recentMenu && s_me_recentRootItem) {
        BOOL needRebuild = NO;
        if (s->numRecent != s_me_lastRecentCount) {
            needRebuild = YES;
        } else if (s->numRecent > 0) {
            NSArray *items = [s_me_recentMenu itemArray];
            if ((int)[items count] != s->numRecent) {
                needRebuild = YES;
            } else {
                for (int i = 0; i < s->numRecent; i++) {
                    NSString *cached = [(NSMenuItem *)items[i] toolTip];
                    NSString *cur = s->recentFiles[i] ? [NSString stringWithUTF8String:s->recentFiles[i]] : @"";
                    if (![cached isEqualToString:cur]) { needRebuild = YES; break; }
                }
            }
        }
        if (needRebuild) {
            [s_me_recentMenu removeAllItems];
            for (int i = 0; i < s->numRecent; i++) {
                const char *path = s->recentFiles[i];
                NSMenuItem *it = [[NSMenuItem alloc]
                    initWithTitle:me_basename(path)
                    action:@selector(onOpenRecent:)
                    keyEquivalent:@""];
                [it setTarget:s_me_bridge];
                [it setTag:i];
                if (path) [it setToolTip:[NSString stringWithUTF8String:path]];
                [s_me_recentMenu addItem:it];
            }
            s_me_lastRecentCount = s->numRecent;
        }
        [s_me_recentRootItem setEnabled:(s->numRecent > 0 ? YES : NO)];
    }

    /* Zoom submenu — rebuild on count change, then mirror checkmarks
     * every frame. The Zoom In / Out items below the per-step list are
     * cached and re-added during the rebuild, then enable-gated each
     * frame from the current index. */
    if (s_me_zoomMenu && s->zoomStepCount > 0 && s->zoomStepValues) {
        if (s_me_lastZoomCount != s->zoomStepCount) {
            [s_me_zoomMenu removeAllItems];
            for (int i = 0; i < s->zoomStepCount; i++) {
                float val = s->zoomStepValues[i];
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
                [it setTarget:s_me_bridge];
                [it setTag:i];
                [s_me_zoomMenu addItem:it];
            }
            [s_me_zoomMenu addItem:[NSMenuItem separatorItem]];
            if (s_me_zoomInItem)  [s_me_zoomMenu addItem:s_me_zoomInItem];
            if (s_me_zoomOutItem) [s_me_zoomMenu addItem:s_me_zoomOutItem];
            s_me_lastZoomCount = s->zoomStepCount;
        }
        for (NSMenuItem *it in [s_me_zoomMenu itemArray]) {
            if (it.action != @selector(onZoomStep:)) continue;
            [it setState:((it.tag == s->zoomStepIndex) ? NSControlStateValueOn : NSControlStateValueOff)];
        }
        if (s_me_zoomInItem)
            [s_me_zoomInItem  setEnabled:((s->zoomStepIndex < s->zoomStepCount - 1) ? YES : NO)];
        if (s_me_zoomOutItem)
            [s_me_zoomOutItem setEnabled:((s->zoomStepIndex > 0) ? YES : NO)];
    }
}

/* ----------------------------------------------------------------------------
 * me_mac_menubar_consume_actions — drain pending into `action`.
 *
 * Mirrors the start of mapEditorImguiMenuBar() on non-Apple: zero the
 * struct, set openRecentIndex to -1, then overlay any pending fields.
 * The pending buffer is reset so each click fires exactly once.
 * ---------------------------------------------------------------------------- */
void me_mac_menubar_consume_actions(MapEditorMenuAction *action) {
    if (!action) return;
    memset(action, 0, sizeof(*action));
    action->openRecentIndex = -1;

    /* Boolean flags — straight copies (pending was reset to false). */
    action->wantExit            = s_me_pending.wantExit;
    action->wantQuitApp         = s_me_pending.wantQuitApp;
    action->wantNew             = s_me_pending.wantNew;
    action->wantOpen            = s_me_pending.wantOpen;
    action->wantOpenWbn         = s_me_pending.wantOpenWbn;
    action->wantSave            = s_me_pending.wantSave;
    action->wantSaveAs          = s_me_pending.wantSaveAs;
    action->wantCenter          = s_me_pending.wantCenter;
    action->wantPointStarts     = s_me_pending.wantPointStarts;
    action->toggleGrid          = s_me_pending.toggleGrid;
    action->toggleMines         = s_me_pending.toggleMines;
    action->togglePillRanges    = s_me_pending.togglePillRanges;
    action->wantCut             = s_me_pending.wantCut;
    action->wantCopy            = s_me_pending.wantCopy;
    action->wantPaste           = s_me_pending.wantPaste;
    action->wantUndo            = s_me_pending.wantUndo;
    action->wantRedo            = s_me_pending.wantRedo;
    action->wantValidate        = s_me_pending.wantValidate;
    action->wantSymMirrorH      = s_me_pending.wantSymMirrorH;
    action->wantSymMirrorV      = s_me_pending.wantSymMirrorV;
    action->wantSymRotate90     = s_me_pending.wantSymRotate90;
    action->wantSymRotate180    = s_me_pending.wantSymRotate180;
    action->wantGenerate        = s_me_pending.wantGenerate;
    action->wantText            = s_me_pending.wantText;
    action->wantImageImport     = s_me_pending.wantImageImport;
    action->wantExportPNG       = s_me_pending.wantExportPNG;
    action->wantZoomIn          = s_me_pending.wantZoomIn;
    action->wantZoomOut         = s_me_pending.wantZoomOut;
    action->wantZoomSet         = s_me_pending.wantZoomSet;
    action->wantToggleTerrain   = s_me_pending.wantToggleTerrain;
    action->wantToggleTools     = s_me_pending.wantToggleTools;
    action->wantToggleInspector = s_me_pending.wantToggleInspector;
    action->wantToggleObjects   = s_me_pending.wantToggleObjects;
    action->wantToggleOverview  = s_me_pending.wantToggleOverview;
    action->wantToggleStats     = s_me_pending.wantToggleStats;
    action->wantToggleStampLibrary = s_me_pending.wantToggleStampLibrary;
    action->wantToggleScenario  = s_me_pending.wantToggleScenario;
    action->wantSettings        = s_me_pending.wantSettings;

    if (s_me_pending.wantZoomSet) {
        action->zoomSetIndex = s_me_pending.zoomSetIndex;
    }
    if (s_me_pending.openRecentIndex >= 0) {
        action->openRecentIndex = s_me_pending.openRecentIndex;
    }

    me_pending_reset();
}

void me_mac_menubar_drop_settings_request(void) {
    s_me_pending.wantSettings = false;
}
