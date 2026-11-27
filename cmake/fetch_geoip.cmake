# IPinfo Lite country database (ipinfo_lite.mmdb), downloaded at configure
# time into a per-user cache so every build tree and worktree shares one copy.
#
# The download needs an IPinfo token, read from the IPINFO_TOKEN environment
# variable. The token is never stored: it is not a cache variable, it is not
# written into the generated build files, and the download URL is never
# printed. Without a token the build carries on, using the cached copy if one
# exists and otherwise building without the database (country codes show as
# unknown). The dist target refuses to package without it.
#
# Sets WINBOLO_GEOIP_DB to the cached file's path, whether or not it exists
# yet; the copy steps check for it at build time (cmake/copy_geoip.cmake).
#
#   WINBOLO_GEOIP_CACHE_DIR    where the cached copy lives
#   WINBOLO_GEOIP_MAX_AGE_DAYS download again once the copy is this old
#   WINBOLO_GEOIP_REFRESH      download on this configure whatever its age;
#                              resets itself to OFF afterwards

if(WIN32 AND DEFINED ENV{LOCALAPPDATA})
    set(_geoip_default_dir "$ENV{LOCALAPPDATA}/winbolo")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_geoip_default_dir "$ENV{XDG_CACHE_HOME}/winbolo")
elseif(DEFINED ENV{HOME})
    set(_geoip_default_dir "$ENV{HOME}/.cache/winbolo")
else()
    set(_geoip_default_dir "${CMAKE_BINARY_DIR}/geoip-cache")
endif()
file(TO_CMAKE_PATH "${_geoip_default_dir}" _geoip_default_dir)

set(WINBOLO_GEOIP_CACHE_DIR "${_geoip_default_dir}" CACHE PATH
    "Directory holding the cached IPinfo Lite database (ipinfo_lite.mmdb)")
set(WINBOLO_GEOIP_MAX_AGE_DAYS 30 CACHE STRING
    "Download the IPinfo Lite database again once the cached copy is this many days old")
option(WINBOLO_GEOIP_REFRESH
    "Download the IPinfo Lite database on the next configure, whatever its age" OFF)

set(WINBOLO_GEOIP_DB "${WINBOLO_GEOIP_CACHE_DIR}/ipinfo_lite.mmdb")

include("${CMAKE_CURRENT_LIST_DIR}/geoip_is_mmdb.cmake")

# A value that is not a whole number would make the age test below false on
# every configure, so the database would never be downloaded again.
set(_geoip_max_age_days "${WINBOLO_GEOIP_MAX_AGE_DAYS}")
if(NOT _geoip_max_age_days MATCHES "^[0-9]+$")
    message(WARNING "GeoIP: WINBOLO_GEOIP_MAX_AGE_DAYS is \"${WINBOLO_GEOIP_MAX_AGE_DAYS}\", "
        "which is not a whole number of days; using 30")
    set(_geoip_max_age_days 30)
endif()

set(_geoip_want_download FALSE)
if(WINBOLO_GEOIP_REFRESH OR NOT EXISTS "${WINBOLO_GEOIP_DB}")
    set(_geoip_want_download TRUE)
else()
    _geoip_age_days("${WINBOLO_GEOIP_DB}" _geoip_days)
    if(_geoip_days GREATER_EQUAL _geoip_max_age_days)
        set(_geoip_want_download TRUE)
    endif()
endif()

if(_geoip_want_download)
    if(NOT DEFINED ENV{IPINFO_TOKEN} OR "$ENV{IPINFO_TOKEN}" STREQUAL "")
        if(EXISTS "${WINBOLO_GEOIP_DB}")
            message(STATUS "GeoIP: ${WINBOLO_GEOIP_DB} is out of date, but IPINFO_TOKEN is not set; using it as it is")
        else()
            message(WARNING "GeoIP: IPINFO_TOKEN is not set and there is no cached database; "
                "building without it, so players' countries will show as unknown. "
                "Set IPINFO_TOKEN and configure again to download ${WINBOLO_GEOIP_DB}.")
        endif()
    else()
        file(MAKE_DIRECTORY "${WINBOLO_GEOIP_CACHE_DIR}")
        # Each configure downloads to a file name of its own, so two
        # configures running at once (two worktrees share this cache) cannot
        # delete or overwrite each other's download. Whichever finishes last
        # renames its complete file over the other's.
        string(RANDOM LENGTH 8 _geoip_suffix)
        set(_geoip_part "${WINBOLO_GEOIP_DB}.${_geoip_suffix}.part")
        # A configure that was killed leaves its download behind. Remove any
        # that are a day old; a newer one may belong to a configure still
        # running.
        file(GLOB _geoip_old_parts "${WINBOLO_GEOIP_DB}.*.part" "${WINBOLO_GEOIP_DB}.part")
        foreach(_geoip_old_part IN LISTS _geoip_old_parts)
            _geoip_age_days("${_geoip_old_part}" _geoip_part_days)
            if(_geoip_part_days GREATER_EQUAL 1)
                file(REMOVE "${_geoip_old_part}")
            endif()
        endforeach()
        message(STATUS "GeoIP: downloading the IPinfo Lite database to ${WINBOLO_GEOIP_DB}")
        # No LOG or SHOW_PROGRESS: either could put the URL, and so the token,
        # in the output.
        file(DOWNLOAD "https://ipinfo.io/data/ipinfo_lite.mmdb?token=$ENV{IPINFO_TOKEN}"
            "${_geoip_part}"
            STATUS _geoip_status
            TLS_VERIFY ON
            INACTIVITY_TIMEOUT 60)
        list(GET _geoip_status 0 _geoip_code)
        _geoip_is_mmdb("${_geoip_part}" _geoip_ok)
        if(_geoip_code EQUAL 0 AND _geoip_ok)
            file(RENAME "${_geoip_part}" "${WINBOLO_GEOIP_DB}")
            message(STATUS "GeoIP: database downloaded")
        else()
            file(REMOVE "${_geoip_part}")
            if(_geoip_code EQUAL 0)
                set(_geoip_why "the file downloaded is not a MaxMind database")
            elseif(_geoip_code EQUAL 22)
                set(_geoip_why "IPinfo refused the request; check IPINFO_TOKEN")
            else()
                set(_geoip_why "download error ${_geoip_code}")
            endif()
            if(EXISTS "${WINBOLO_GEOIP_DB}")
                message(WARNING "GeoIP: could not refresh the database (${_geoip_why}); keeping the cached copy")
            else()
                message(WARNING "GeoIP: could not download the database (${_geoip_why}); "
                    "building without it, so players' countries will show as unknown")
            endif()
        endif()
    endif()
endif()

if(WINBOLO_GEOIP_REFRESH)
    set(WINBOLO_GEOIP_REFRESH OFF CACHE BOOL
        "Download the IPinfo Lite database on the next configure, whatever its age" FORCE)
endif()
