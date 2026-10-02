# Hosting the web client (play.winbolo.net)

The browser (WASM) WinBolo client is served by Caddy on `play.winbolo.net`. The
page is the game build (`winbolo.html`, `.js`, `.wasm`, `.data`) plus its `js/`
and `img/` files, all served from one root. `/` opens the game on its menu;
there is no separate lobby page.

## What the page is made of

```
src/wasm/
├── shell.html          Page template for winbolo.html (loading screen, relay pick,
│                       API calls, link-preview tags)
└── web/                Copied next to winbolo.html on every build
    ├── js/relay.js     Relay latency check and selection (loaded by the shell)
    └── img/            Favicon, loading-screen logo, Open Graph image
```

## Build and deploy

Deploy from the web build directory (see `src/wasm/CMakeLists.txt` for the
configure step). Build from a clean checkout: `data/` is preloaded whole, so a
build from a working tree also packs any untracked files under `data/` into the
data file.

```bash
cmake --build <build-dir> --target wasm-dist
```

This makes `<build-dir>/wasm-dist.zip`, which holds:

| Path | Notes |
|------|-------|
| `winbolo.{html,js,wasm,data}` | each with `.br` and `.gz` copies |
| `js/relay.js` | with `.br` and `.gz` copies |
| `img/` | as they are (PNG and JPEG are already compressed) |

Unzip it into the site root. The paths in the zip are the paths the site
serves.

To try a build before deploying it, serve the build directory with
`python3 src/wasm/serve_isolated.py <build-dir>`, which sends the headers the
game needs (see Cross-origin isolation).

The data file is about **68 MB**. That figure needs Python `fonttools` at
configure time (`pip install fonttools brotli`) for the CJK font subset;
without it the build warns and preloads the full fonts, and the data file is
far larger. `wasm-dist` also needs `brotli` (`apt-get install brotli`).

## Backend requirements

The game shell calls these **same-origin** paths, which Caddy reverse-proxies to
the WinBolo.net backend (so there is no CORS to configure):

| Path | Method | Backend | Called by | Purpose |
|------|--------|---------|-----------|---------|
| `/api/v1/me` | GET | main app (8080) | game shell (`winbolo.html`) | Logged-in state for this browser |
| `/api/v1/games` | GET | wbn backend (8081) | game (finder) | Active game list |
| `/api/join` | POST | main app (8080) | game shell (`winbolo.html`) | Mint a single-use join code for a game key (requires login); minted per connect |
| `/api/v1/prefs` | GET / PUT | main app (8080) | game shell (`winbolo.html`) | Caller's full client-prefs document (cookie-authed, CAS on `updatedAt`) |
| `/logdownload` | GET | main app (8080) | game shell (`winbolo.html`) | Round log for the lobby's recap reel (see `wbRoundLogFetch` in `shell.html`) |

Session requirements in the backend `.env`:

- `SESSION_COOKIE_DOMAIN=.winbolo.net` — so the login cookie set on `winbolo.net`
  is also sent to `play.winbolo.net` (and through the proxy to the API).
- `SESSION_COOKIE_SAMESITE=Lax` — **not `Strict`**. Steam login returns via a
  cross-site redirect from `steamcommunity.com`; `Strict` withholds the session
  cookie on that return, so Steam logins silently fail to stick. `Lax` fixes it
  and still blocks cross-site subrequests.
- `WASM_CLIENT_ENABLED=true` — otherwise `/api/join` returns 404.

(Env changes require a container recreate: `docker compose up -d`.)

## Relays

Web play tunnels the game's UDP over a WebSocket relay. Relays are
interchangeable — any relay routes any join code — so the game shell picks the
**closest healthy** one at load time (`src/wasm/web/js/relay.js`, served as
`/js/relay.js`):

- Relays are `relay-{au,eu,us}.winbolo.net`. Each exposes `GET /ping` returning
  `{"status":"online"}` and **must** send `Access-Control-Allow-Origin` for
  `play.winbolo.net` — the check is cross-origin and reads the body.
- The shell warms each relay (a throwaway ping that pays the TLS handshake),
  takes a few timed samples, and connects to the lowest-latency responder:
  `wss://relay-<region>.winbolo.net/proxy?joinCode=<code>`. If none answer it
  falls back to a default (`us`).
- Add or remove a relay by editing the `WB_RELAYS` map in
  `src/wasm/web/js/relay.js`.

## Routes

The path routes are internal **rewrites** (not redirects), so the clean path
stays in the address bar. The shell reads its mode from
`window.location.pathname` and carries a `<base href="/">` so its assets still
resolve from the root.

| URL | Serves |
|-----|--------|
| `/` | `winbolo.html` (the game opens on its menu) |
| `/?finder=1` | `winbolo.html` (the game finder; the sign-in return link) |
| `/tutorial` | `winbolo.html` (shell sets `tutorial=1`) |
| `/practise` | `winbolo.html` (single-player practice) |
| `/join/<game_key>` | `winbolo.html` (shell reads `game_key` from the path, mints its own single-use join code, and picks the closest relay) |

`/?finder=1` needs no rewrite of its own: it is `/` with a query string.

## Example Caddyfile

Caddy runs in its own container, so it sees the container filesystem and network:
mount the site root in as a volume, and proxy to the backend via
`host.docker.internal` (not `localhost`). Note Caddyfile blocks must span
multiple lines — `{` has to be the last token on its line.

```caddy
play.winbolo.net {
    encode zstd gzip

    # The unzipped wasm-dist: winbolo.*, js/ and img/
    root * /srv/play

    # The game uses threads, and the browser allows them only on a cross-origin
    # isolated page. No matcher, so every response carries these, including the
    # proxied ones and winbolo.js, which the game's thread workers load.
    header Cross-Origin-Opener-Policy "same-origin"
    header Cross-Origin-Embedder-Policy "require-corp"

    # Revalidates by ETag, so a join's page reload does not download the data file again.
    header /winbolo* Cache-Control "no-cache"

    # API → wbn container (same-origin, no CORS)
    handle /api/v1/games {
        reverse_proxy host.docker.internal:8081
    }
    handle /api/* {
        reverse_proxy host.docker.internal:8080
    }
    handle /logdownload* {
        reverse_proxy host.docker.internal:8080
    }

    # Clean URLs — internal rewrites keep the path in the address bar
    rewrite / /winbolo.html
    rewrite /tutorial /winbolo.html
    rewrite /practise /winbolo.html
    @join path_regexp join ^/join/(.+)$
    rewrite @join /winbolo.html

    handle {
        file_server {
            precompressed br gzip
            hide .*
        }
    }
}
```

Mount the site root into the Caddy container, e.g. in the Caddy
`docker-compose.yml`:

```yaml
    volumes:
      - <site-root>:/srv/play:ro
```

> Volume changes need a recreate (`docker compose up -d`), not just a reload.

## Log viewer (logviewer.winbolo.net)

The web log viewer (`src/logviewer/wasm/`) opens a WinBolo.net round log from
its key:

| URL | Opens |
|-----|-------|
| `/gamelog/<key>` | the log with that key |
| `/logviewer.html?key=<key>` | the same log |

The page fetches `/logdownload?key=<key>` from its own origin, so the server
passes `/logdownload` on to the WinBolo.net backend, and `/gamelog/*` is an
internal rewrite to `logviewer.html`. The page carries `<base href="/">`, so it
must be served from the site root.

```caddy
logviewer.winbolo.net {
    encode zstd gzip
    root * /srv/logviewer

    handle /logdownload* {
        reverse_proxy host.docker.internal:8080
    }

    rewrite / /logviewer.html
    rewrite /gamelog/* /logviewer.html

    handle {
        file_server {
            precompressed br gzip
            hide .*
        }
    }
}
```

The log viewer does not use threads, so it needs no cross-origin isolation
headers.

## Cross-origin isolation

The game runs threads, which need `SharedArrayBuffer`, and the browser gives
that only to a page served with `Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. Without both headers the page
shows a message that the browser can't run WinBolo and does not start the game.
The headers also mean:

- The relays must keep sending `Access-Control-Allow-Origin` on `/ping`. The
  latency check fetches it cross-origin in CORS mode (see Relays). The
  WebSocket to the relay is not affected.
- Nothing may be loaded from another origin unless it sends CORS headers or
  `Cross-Origin-Resource-Policy`. Everything else the page loads today is
  same-origin; sign-in and News open as navigations or new tabs, which the
  headers don't block.
- The game only runs as a top-level page: inside another site's iframe it is
  not cross-origin isolated, so it shows the message instead of starting.

To test a build locally with both headers, serve it with
`python3 src/wasm/serve_isolated.py <build-dir> [port]` (port 8000 by default).
`python3 -m http.server` does not send them, so the game won't start under it.

## Deploy order

Moving from the old static lobby to the game at `/` goes in this order:

1. **Deploy the new build to the site root**, with its `js/` and `img/`. It
   has to be in place first: the `/` rewrite needs the build with the in-game
   menu, and the build now carries the `js/relay.js` and `img/` files the old
   lobby used to supply.
2. **Switch Caddy's `/` to `winbolo.html` and change it to the single root**
   (the Caddyfile above). Switching `/` before step 1 would send `/` to an older
   build, which starts single player instead of opening the menu.
3. **Delete `play/` in the WinBolo.net repo.** Nothing is served from it once
   Caddy uses the single root, so it goes last.

Moving to the threaded build goes in this order:

1. **Add the two cross-origin isolation headers in Caddy** (the
   `Cross-Origin-*` lines in the Caddyfile above) and reload. The current build
   works under them, and the threaded build won't start without them, so they
   go on first.
2. **Deploy the threaded build to the site root**, as in Build and deploy.
