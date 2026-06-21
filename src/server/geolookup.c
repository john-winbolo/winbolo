/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          GeoLookup
 *Filename:      geolookup.c
 *Purpose:
 *  IP-to-country lookup using libmaxminddb and DB-IP Lite.
 *  IP geolocation by DB-IP (https://db-ip.com).
 *********************************************************/

#include <string.h>
#include "geolookup.h"

#ifdef HAVE_MAXMINDDB
#include <maxminddb.h>

static MMDB_s geoDb;
static bool geoDbLoaded = false;

bool geoLookupCreate(const char *mmdbPath) {
  int status;

  if (mmdbPath == NULL) {
    return false;
  }

  status = MMDB_open(mmdbPath, MMDB_MODE_MMAP, &geoDb);
  if (status != MMDB_SUCCESS) {
    geoDbLoaded = false;
    return false;
  }

  geoDbLoaded = true;
  return true;
}

void geoLookupDestroy(void) {
  if (geoDbLoaded) {
    MMDB_close(&geoDb);
    geoDbLoaded = false;
  }
}

bool geoLookupCountry(const char *ipStr, char countryCode[3]) {
  int gaiError;
  int mmdbError;
  MMDB_lookup_result_s result;
  MMDB_entry_data_s entryData;
  int status;

  countryCode[0] = 'X';
  countryCode[1] = 'X';
  countryCode[2] = '\0';

  if (!geoDbLoaded || ipStr == NULL || ipStr[0] == '\0') {
    return false;
  }

  result = MMDB_lookup_string(&geoDb, ipStr, &gaiError, &mmdbError);

  if (gaiError != 0 || mmdbError != MMDB_SUCCESS) {
    return false;
  }

  if (!result.found_entry) {
    return false;
  }

  status = MMDB_get_value(&result.entry, &entryData, "country", "iso_code", NULL);
  if (status != MMDB_SUCCESS) {
    return false;
  }

  if (!entryData.has_data || entryData.type != MMDB_DATA_TYPE_UTF8_STRING) {
    return false;
  }

  if (entryData.data_size >= 2) {
    countryCode[0] = entryData.utf8_string[0];
    countryCode[1] = entryData.utf8_string[1];
    countryCode[2] = '\0';
    return true;
  }

  return false;
}

bool geoLookupIsLoaded(void) {
  return geoDbLoaded;
}

#else /* !HAVE_MAXMINDDB — stub implementation */

bool geoLookupCreate(const char *mmdbPath) {
  (void)mmdbPath;
  return false;
}

void geoLookupDestroy(void) {
}

bool geoLookupCountry(const char *ipStr, char countryCode[3]) {
  (void)ipStr;
  countryCode[0] = 'X';
  countryCode[1] = 'X';
  countryCode[2] = '\0';
  return false;
}

bool geoLookupIsLoaded(void) {
  return false;
}

#endif /* HAVE_MAXMINDDB */
