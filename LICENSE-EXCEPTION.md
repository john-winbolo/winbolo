# Additional permissions for platform SDKs and app stores

WinBolo is licensed under the GNU General Public License, version 2 or (at
your option) any later version, as set out in [LICENSE](LICENSE). This file
adds three permissions to that licence. They exist so that WinBolo can be
released on Steam, on app stores and on other platforms whose libraries or
terms the GPL does not allow for on its own. They do not allow WinBolo, or any
part of it, to be made closed source.

## Definitions

- **The Steamworks SDK** means the Steamworks SDK published by Valve
  Corporation, including the `steam_api` runtime library from it.
- **A platform SDK** means a library that is not licensed under terms
  compatible with the GPL, that is provided by the operator of a platform
  (such as a game store, an app store, a games console or an online service),
  and that is needed to run WinBolo on that platform or to use that platform's
  services. The Steamworks SDK is a platform SDK.
- **An app store** means a service that distributes software to users, such as
  the Apple App Store, TestFlight or Google Play, whose terms of distribution or
  use place restrictions on users that the GPL does not allow.
- **The WinBolo source code** means the complete source code of the version of
  WinBolo being distributed, as the GPL defines it, not including any platform
  SDK.

## Permission 1: the Steamworks SDK

As a special exception, the copyright holders of WinBolo give permission to
link WinBolo with the Steamworks SDK, and to distribute the linked combination
together with the Steamworks SDK's runtime library.

## Permission 2: other platform SDKs

As a special exception, the copyright holders of WinBolo give permission to
link WinBolo with any other platform SDK, and to distribute the linked
combination together with those parts of the platform SDK that its owner
allows to be distributed.

## Permission 3: app stores

As a special exception, the copyright holders of WinBolo give permission to
distribute WinBolo, alone or linked with a platform SDK under Permission 1 or
2, through an app store, even though that app store's terms place restrictions
on users that the GPL does not allow.

## Conditions

Each permission above applies only while both of these are true:

1. The WinBolo source code for the version distributed is available to the
   public under the GNU General Public License, free of charge.
2. You obey the GNU General Public License in all respects for all of the code
   used other than the platform SDK.

## Modified versions

If you modify WinBolo, you may extend these permissions to your version, but
you are not required to. If you do not wish to do so, delete this file and any
reference to it from your version.

## Platform SDKs are not part of this source code

- No platform SDK is included in this repository or in any WinBolo source
  release, and none is licensed under the GPL. Each is owned by its publisher
  and is available only from that publisher, under that publisher's terms.
- A platform SDK is not part of the WinBolo source code for the purposes of the
  GPL. Anyone distributing WinBolo is not required to provide a platform SDK's
  source code.
- WinBolo builds and runs without any platform SDK. Without the Steamworks SDK
  the build links `src/steam/steam_wrapper_stub.c` in its place, and the Steam
  features report themselves as unavailable. See
  [docs/BUILDING.md](docs/BUILDING.md#steam-integration).
- Anyone who builds WinBolo with a platform SDK must get it from its publisher
  and follow its publisher's terms, as well as the GPL for the rest of
  WinBolo.

The Steamworks SDK is available from Valve under the
[Steamworks SDK Access Agreement](https://partner.steamgames.com/documentation/sdk_access_agreement).
