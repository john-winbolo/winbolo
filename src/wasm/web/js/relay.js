/*
 * Relay selection for the WinBolo web client.
 *
 * Standalone module loaded by the game shell (winbolo.html) at /js/relay.js —
 * Caddy serves it from the play root, same origin as the game, exactly like the
 * shared /img assets. Picks the lowest-latency relay that reports healthy so the
 * game socket (wss://relay-<region>.winbolo.net/proxy) routes through the
 * closest one.
 *
 * The relays are interchangeable: any relay routes any join code, so the choice
 * is purely about latency. Each relay's /ping returns {"status":"online"} and
 * sends CORS for play.winbolo.net, so these cross-origin probes can read the
 * body and be timed.
 */
'use strict';

var WB_RELAYS = {
    au: 'relay-au.winbolo.net',
    eu: 'relay-eu.winbolo.net',
    us: 'relay-us.winbolo.net'
};

// Fallback when every probe fails (all relays down / offline / blocked).
var WB_DEFAULT = 'us';

// Friendly labels for the #netinfo chip.
var WB_RELAY_LABELS = { au: 'Australia', eu: 'Europe', us: 'United States' };

// Per-ping timeout. A relay that doesn't answer within this drops out fast, so a
// black-holed host can't stall the whole probe.
var WB_PING_TIMEOUT_MS = 2000;

// Measurement samples per relay, taken after a warm-up ping that pays the
// DNS/TCP/TLS handshake (so the samples time the round-trip, not the setup).
var WB_PING_SAMPLES = 4;

function wbRelayHost(region) {
    return WB_RELAYS[region] || WB_RELAYS[WB_DEFAULT];
}

function wbRelayLabel(region) {
    return WB_RELAY_LABELS[region] || String(region || '').toUpperCase();
}

function wbRelayWsUrl(region, joinCode) {
    return 'wss://' + wbRelayHost(region) + '/proxy?joinCode=' +
           encodeURIComponent(joinCode);
}

// One timed GET /ping. Resolves { ok, rtt }; ok is true only when the relay
// answers 200 with {"status":"online"}. Never rejects — a timeout or error
// resolves { ok:false, rtt:Infinity } so the caller just excludes it.
function wbPingRelay(region) {
    var host = WB_RELAYS[region];
    var ctrl = new AbortController();
    var timer = setTimeout(function () { ctrl.abort(); }, WB_PING_TIMEOUT_MS);
    var t0 = performance.now();
    return fetch('https://' + host + '/ping', {
        method: 'GET',
        cache: 'no-store',
        signal: ctrl.signal
    }).then(function (r) {
        return r.text().then(function (body) {
            clearTimeout(timer);
            var online = false;
            try { online = JSON.parse(body).status === 'online'; } catch (e) {}
            return { ok: r.ok && online, rtt: performance.now() - t0 };
        });
    }).catch(function () {
        clearTimeout(timer);
        return { ok: false, rtt: Infinity };
    });
}

// Probe one relay: a warm-up ping (discarded — it pays the handshake), then N
// samples on the warm connection. Returns the MIN sample rtt (the propagation
// floor; jitter only ever adds time) or null if it never reported healthy.
function wbProbeOne(region) {
    return wbPingRelay(region).then(function (warm) {
        if (!warm.ok) { return null; }        // down / offline -> excluded
        var best = Infinity;
        var i = 0;
        function next() {
            if (i >= WB_PING_SAMPLES) { return (best === Infinity ? null : best); }
            i++;
            return wbPingRelay(region).then(function (s) {
                if (s.ok && s.rtt < best) { best = s.rtt; }
                return next();
            });
        }
        return next();
    });
}

// Probe every relay in parallel; resolve the region with the lowest min-rtt that
// reported healthy, or null if none did (caller falls back to WB_DEFAULT).
function wbProbeRelays() {
    var regions = Object.keys(WB_RELAYS);
    return Promise.all(regions.map(function (region) {
        return wbProbeOne(region).then(function (rtt) {
            return { region: region, rtt: (rtt === null ? Infinity : rtt) };
        });
    })).then(function (results) {
        var best = null;
        results.forEach(function (r) {
            if (r.rtt !== Infinity && (best === null || r.rtt < best.rtt)) {
                best = r;
            }
        });
        return best ? best.region : null;
    });
}
