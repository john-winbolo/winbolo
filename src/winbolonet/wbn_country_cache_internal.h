/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          WinBolo.net country-code cache (internal)
 * Filename:      wbn_country_cache_internal.h
 * Purpose:
 *   Test-only reset hook for the in-memory country-code
 *   cache implemented in wbn_country_cache.c. Lives in a
 *   header separate from winbolonet_core.h so that
 *   production callers can't see it.
 *
 *   Used by tests/unit/test_wbn_country_cache.c to put the
 *   cache back into its uninitialised state between cases.
 *********************************************************/

#ifndef __WBN_COUNTRY_CACHE_INTERNAL_H
#define __WBN_COUNTRY_CACHE_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

void winbolonetCountryCacheResetForTesting(void);

#ifdef __cplusplus
}
#endif

#endif /* __WBN_COUNTRY_CACHE_INTERNAL_H */
