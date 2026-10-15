# Building and checking the iOS client

The iOS client is the target `WinBoloIOS`, configured in the `iOS` branch
of `CMakeLists.txt`. Its driver is `src/gui/ios/winbolo_ios.m`; everything
else it draws comes from the shared SDL3 and ImGui sources in `src/gui/sdl3/`.
The rules every client follows, including this one, are in
`docs/ARCHITECTURE.md`. The history of how the iOS client was brought level
with the desktop is in `plans/ios.md`, which is kept outside git.

## Configure and build

Configure for the simulator into a build directory of its own:

```sh
cmake -B build-ios-sim -S . -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
```

For a device the sysroot is `iphoneos`, in another build directory:
sentry-cocoa is resolved to one slice (simulator or device) at configure
time, so one directory cannot serve both.

Build:

```sh
xcodebuild -project build-ios-sim/winbolo.xcodeproj -target WinBoloIOS \
  -configuration Debug -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO ARCHS=arm64 ONLY_ACTIVE_ARCH=YES
```

`-configuration Release` gives the optimised build. The app lands at
`build-ios-sim/WinBoloIOS.app`.

Building with `-target` does not regenerate the Xcode project. After any
change to `CMakeLists.txt` or `src/gui/ios/Info.plist`, run the configure
line again by hand before building, or the build uses the old project.

Crash reporting: `ENABLE_SENTRY` (on by default) downloads sentry-cocoa and
links it whether or not a DSN is given. The DSN reaches the target only as a
compile definition, from `-DSENTRY_DSN=...` on the configure line. A build
configured without it starts Sentry and reports nothing.

## Running it in a simulator

Everything below works without touching the simulator window, which is how
it can be scripted. Find a device's UDID with `xcrun simctl list devices`.

```sh
xcrun simctl boot <udid>
xcrun simctl install <udid> build-ios-sim/WinBoloIOS.app
SIMCTL_CHILD_WINBOLO_LOG='*=info' \
  xcrun simctl launch --console <udid> com.winbolo.ios
xcrun simctl io <udid> screenshot out.png
xcrun simctl terminate <udid> com.winbolo.ios
xcrun simctl shutdown <udid>
```

- `SIMCTL_CHILD_` passes the rest of the name into the app's environment.
  `WINBOLO_LOG` takes the same spec as on the desktop
  (`src/common/wb_log.c`): comma-separated `category=level` pairs applied
  left to right, `*` for every category. `net=debug` shows each WinBolo.net
  request and its HTTP status. With no `WINBOLO_LOG` the client logs
  nothing.
- A `winbolo://host:port` argument after the bundle id on the `launch`
  line opens the join dialog with that server filled in.
- `simctl` injects no touches. `xcrun simctl openurl` works too, but the
  system asks for a tap to confirm it.

A server to join: run a dedicated server on the Mac.

```sh
WinBoloDS -inbuilt -port <p> -gametype Open -nowinbolonet -notracker -mdns -quiet
```

`-mdns` advertises it on the LAN. `dns-sd -B _winbolo._udp local.` on the
Mac lists what the iOS Bonjour browser will see; `dns-sd -L <instance>
_winbolo._udp local.` shows the port and TXT record it will resolve.

## What the iOS target is

A client that plays single player against bots, joins Internet and LAN
games, records and replays rounds, and edits maps. It links:

- the sim and its runtime: `bolo_static`, `scenario_static`,
  `server_sim_static`, `server_round_log_static`, `bot_brains_static`,
  `threads_static`;
- WinBolo.net: `winbolonet_core` built with `src/winbolonet/http_nsurl.m`
  (NSURLSession) in place of `http.c`, since iOS has no libcurl, and
  `winbolonet_client`;
- voice: Opus and SpeexDSP, with the same `WINBOLO_VOICE` switches as the
  desktop;
- the log viewer's decoder, renderer and embedded reel host, the set the web
  client links (`IOS_LOGVIEWER_SOURCES` in `CMakeLists.txt`), and the map
  editor sources.

What it does not have, on purpose:

- no Steam (`src/steam/steam_wrapper_stub.c`);
- no hosting of network games: `server_static` and `winbolonet_server` are
  not linked, and `src/gui/ios/ios_stubs.c` gives the server-side calls the
  sim and `gamefront.c` still make bodies that do nothing. Its
  `serverInstanceStartup` and `serverInstanceTick` run the local
  single-player server through `serverSimLocalTick`;
- no NAT port mapping (`BOLO_PORTMAP` is off for iOS);
- no GeoIP country flags (the `geoLookup*` calls are stubbed).

Per-file T2 grants on `WinBoloIOS` (see "Per-file T2 grants" in
`docs/ARCHITECTURE.md`): `src/bolo/transport_udp_client.c`,
`src/gui/sdl3/bg_game.c`, `src/gui/ios/ios_stubs.c`, and the embedded map
editor's `mapeditor.c`, `mapeditor_export.c` and `mapeditor_validate.c`.

## Files on the phone

- The preferences directory (`SDL_GetPrefPath("WinBolo", "WinBolo")`) holds
  `WinBolo.json` and the recordings the client writes: `singleplayer.wbv`
  and the live `singleplayer-recording.wbv`.
- Documents shows in the Files app, and in Finder's file sharing, as the
  WinBolo folder. Saved maps go there, and the recordings screen
  (`src/gui/ios/ios_recordings.cpp`) copies a recording there with its
  Copy to Files button. It also lists `Documents/Inbox`, where iOS puts a
  file another app sends over.

## Info.plist keys and why each is there

- `NSMicrophoneUsageDescription`: voice chat opens the microphone.
- `NSLocalNetworkUsageDescription` and `NSBonjourServices`
  (`_winbolo._udp`): joining a LAN game and the Bonjour browser in
  `src/gui/ios/ios_discovery.m`. Without them iOS blocks local network
  traffic without saying so.
- `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace`: Documents
  appears in the Files app and in Finder.
- `CFBundleDocumentTypes` and `UTImportedTypeDeclarations` for
  `com.winbolo.wbv`: a `.wbv` opened from Files, Mail or another app
  launches WinBolo with it.
- `CFBundleURLTypes` (`winbolo`): `winbolo://` game links.
- `UIApplicationSupportsIndirectInputEvents`: a trackpad or mouse on an iPad
  reaches the app.

## Known limits on a device

- LAN search: the broadcast search in `src/bolo/discovery.c` sends to a
  broadcast address, which iOS allows only with Apple's multicast
  entitlement, which the app does not have. The Bonjour browser
  (`ios_discovery.m`), which `discovery_mdns.c` runs in place of its raw
  mDNS socket on iOS, needs no entitlement and finds every server started
  with `-mdns`. Servers that advertise only by broadcast are reachable
  through Manual Connect.
- App Transport Security: `Info.plist` has no exception, so only https
  works. The default WinBolo.net host is https; an `http://` host override
  or a plain-http image in a news post fails with a transport error.
- Bot brains run on PUC-Lua 5.4 (iOS forbids LuaJIT's compiler), several
  times slower per think than the desktop. The app bundles the `opt/` build
  of GoalHunter as the desktop does. In a Debug simulator build bots can
  still overrun their think budget; Release builds have shown none.
- The Metal API validation abort on the welcome screen is open. Running from
  Xcode needs the scheme's Metal API validation turned off.
