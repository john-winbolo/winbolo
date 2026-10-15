/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          iOS Open URL
*Filename:      ios_open_url.m
*Purpose:
*  Makes a .wbv opened in place readable.
*
*  With LSSupportsOpeningDocumentsInPlace on, a file opened
*  from iCloud Drive or another file provider is not copied
*  into the app: the app gets a security-scoped URL to the
*  original, and may read it only between
*  startAccessingSecurityScopedResource and
*  stopAccessingSecurityScopedResource on that URL object.
*  SDL's scene delegate hands over only the URL's path, as
*  an SDL_EVENT_DROP_FILE, and the access cannot be started
*  again from a path: a URL rebuilt from it carries no
*  security scope.
*
*  So the access is started here, on the URL UIKit gave,
*  before SDL turns it into a path. WBSceneDelegate is SDL's
*  scene delegate with its two URL entry points (a file
*  opened while running, and one that launched the app)
*  wrapped: each file URL's access is started and the URL
*  kept, then SDL's own handling runs unchanged, winbolo://
*  links included. Once the file has been read,
*  iosOpenUrlRelease ends the access.
*
*  SDL picks its scene delegate class by asking
*  +[SDLUIKitSceneDelegate getSceneDelegateClassName] in
*  SDL_RunApp; SDL's own source says to override that
*  method with a category when subclassing, which the
*  category below does. SDL's class is declared here
*  rather than through its header, which is private to
*  SDL's source tree.
*********************************************************/

#import <UIKit/UIKit.h>
#include <string.h>

#include "ios_drops.h"

API_AVAILABLE(ios(13.0))
@interface SDLUIKitSceneDelegate : NSObject <UIApplicationDelegate, UIWindowSceneDelegate>
+ (NSString *)getSceneDelegateClassName;
@end

API_AVAILABLE(ios(13.0))
@interface WBSceneDelegate : SDLUIKitSceneDelegate
@end

/* File URLs whose access was started and is not yet ended. */
static NSMutableArray<NSURL *> *s_accessedUrls = nil;

/* Starts the access to a file URL that needs it and keeps the URL to end it
 * later. A URL inside the app's own sandbox answers NO and needs nothing. */
static void wbStartAccess(NSURL *url) {
    if (url == nil || !url.isFileURL) {
        return;
    }
    if ([url startAccessingSecurityScopedResource]) {
        if (s_accessedUrls == nil) {
            s_accessedUrls = [[NSMutableArray alloc] init];
        }
        [s_accessedUrls addObject:url];
    }
}

@implementation WBSceneDelegate

/* A file or link that launched the app. SDL keeps these and sends them as
 * drops on its first event pump, which is after this returns. */
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)connectionOptions {
    for (UIOpenURLContext *context in connectionOptions.URLContexts) {
        wbStartAccess(context.URL);
    }
    [super scene:scene willConnectToSession:session options:connectionOptions];
}

/* A file or link opened while the app runs. */
- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts {
    for (UIOpenURLContext *context in URLContexts) {
        wbStartAccess(context.URL);
    }
    [super scene:scene openURLContexts:URLContexts];
}

/* SDL's version names its own class as the scene's delegate; name this one,
 * so the wrapped entry points above are the ones UIKit calls. */
- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession
                                   options:(UISceneConnectionOptions *)options {
    UISceneConfiguration *config =
        [[UISceneConfiguration alloc] initWithName:@"SDLSceneConfiguration"
                                       sessionRole:connectingSceneSession.role];
    config.delegateClass = [WBSceneDelegate class];
    return config;
}

@end

/* Replacing SDL's own implementation is the point of this category, so the
 * compiler's warning that the class implements the same method is expected. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
@implementation SDLUIKitSceneDelegate (WinBolo)

+ (NSString *)getSceneDelegateClassName {
    return @"WBSceneDelegate";
}

@end
#pragma clang diagnostic pop

void iosOpenUrlRelease(const char *path) {
    if (path == NULL || s_accessedUrls == nil) {
        return;
    }
    for (NSUInteger i = s_accessedUrls.count; i > 0; i--) {
        NSURL *url = s_accessedUrls[i - 1];
        /* SDL sends url.filePathURL.path; compare the same form. */
        NSString *urlPath = url.filePathURL.path;
        if (urlPath != nil && strcmp(urlPath.UTF8String, path) == 0) {
            [url stopAccessingSecurityScopedResource];
            [s_accessedUrls removeObjectAtIndex:i - 1];
        }
    }
}
