#ifndef PLAYER_FLAGS_H
#define PLAYER_FLAGS_H

/* Standalone client-identity defines. Kept in a tiny header (no structs)
 * so it can be included from both bolo/players.h and gui/lang.h without
 * dragging in conflicting type definitions — logviewer/players.h declares
 * its own (different) `player` / `players` types, so any file that pulls
 * in lang.h via messages.h must not see bolo/players.h's structs. */

/* Self-reported by client (untrusted, but no in-game advantage from lying). */
typedef enum {
    CLIENT_TYPE_UNKNOWN   = 0,
    CLIENT_TYPE_WINDOWS   = 1,
    CLIENT_TYPE_LINUX     = 2,
    CLIENT_TYPE_MACOS     = 3,
    CLIENT_TYPE_IOS       = 4,
    CLIENT_TYPE_ANDROID   = 5,
    CLIENT_TYPE_STEAMDECK = 6,
    CLIENT_TYPE_WEB       = 7,
    CLIENT_TYPE_COUNT     = 8   /* sentinel - keep last */
} ClientType;

#define PLAYER_FLAG_WBN_VERIFIED     0x01  /* server-set, trusted */
#define PLAYER_FLAG_WBN_STEAM_LINKED 0x02  /* server-set, trusted */
#define PLAYER_FLAG_SUPPORTER        0x04  /* server OR client; trusted iff WBN_VERIFIED */
#define PLAYER_FLAG_STEAM_BUILD      0x08  /* client-set, untrusted */
/* bits 4-7 reserved */

#define PLAYER_CLIENT_HINT_MASK \
    (PLAYER_FLAG_SUPPORTER | PLAYER_FLAG_STEAM_BUILD)

#endif /* PLAYER_FLAGS_H */
