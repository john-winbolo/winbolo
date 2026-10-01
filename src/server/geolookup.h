/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          GeoLookup
 *Filename:      geolookup.h
 *Purpose:
 *  IP-to-country lookup using libmaxminddb and DB-IP Lite.
 *  Returns 2-letter ISO 3166-1 alpha-2 country codes.
 *  IP geolocation by DB-IP (https://db-ip.com).
 *********************************************************/

#ifndef GEOLOOKUP_H
#define GEOLOOKUP_H

#include <stdbool.h>

/*********************************************************
 *NAME:          geoLookupCreate
 *PURPOSE:
 *  Open the MaxMind DB (.mmdb) file for IP-to-country lookups.
 *
 *ARGUMENTS:
 *  mmdbPath - Path to the .mmdb database file (e.g., "data/dbip-country-lite.mmdb")
 *RETURNS:
 *  true on success, false if the file could not be opened.
 *  If this returns false, all lookups will return "XX".
 *********************************************************/
bool geoLookupCreate(const char *mmdbPath);

/*********************************************************
 *NAME:          geoLookupDestroy
 *PURPOSE:
 *  Close the database and free resources.
 *********************************************************/
void geoLookupDestroy(void);

/*********************************************************
 *NAME:          geoLookupCountry
 *PURPOSE:
 *  Look up a 2-letter country code for a given IP address string.
 *
 *ARGUMENTS:
 *  ipStr       - IP address as a string (e.g., "8.8.8.8" or "2001:4860:4860::8888")
 *  countryCode - Output buffer, at least 3 bytes. Will be null-terminated.
 *                Set to "XX" if lookup fails or database is not loaded.
 *RETURNS:
 *  true if a country code was found, false otherwise.
 *********************************************************/
bool geoLookupCountry(const char *ipStr, char countryCode[3]);

/*********************************************************
 *NAME:          geoLookupIsLoaded
 *PURPOSE:
 *  Check whether a database is currently loaded.
 *
 *RETURNS:
 *  true if a database is open and ready for lookups.
 *********************************************************/
bool geoLookupIsLoaded(void);

#endif /* GEOLOOKUP_H */
