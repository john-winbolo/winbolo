# Script mode: copy the cached IPinfo Lite database into a build output's
# data/ directory. Run on every build rather than at configure time, so a
# copy downloaded after the tree was configured is still picked up.
#
#   -DSOURCE=<cached ipinfo_lite.mmdb>
#   -DDEST_DIR=<output data/ directory>
#   -DREQUIRED=ON        the dist target: fail when the database is missing
#                        or is not a MaxMind-format file
#   -DMAX_AGE_DAYS=<n>   with REQUIRED, warn when the database is this many
#                        days old or older

include("${CMAKE_CURRENT_LIST_DIR}/geoip_is_mmdb.cmake")

if(EXISTS "${SOURCE}")
    if(REQUIRED)
        _geoip_is_mmdb("${SOURCE}" _geoip_ok)
        if(NOT _geoip_ok)
            message(FATAL_ERROR "The GeoIP database ${SOURCE} is not a MaxMind-format file, "
                "and a release has to ship a working one. Delete it, set IPINFO_TOKEN and "
                "configure again to download it.")
        endif()
        if("${MAX_AGE_DAYS}" MATCHES "^[0-9]+$")
            _geoip_age_days("${SOURCE}" _geoip_days)
            if(_geoip_days GREATER_EQUAL MAX_AGE_DAYS)
                message(WARNING "The GeoIP database ${SOURCE} is ${_geoip_days} days old, "
                    "and this release will ship it. Set IPINFO_TOKEN and configure again "
                    "to download a new one.")
            endif()
        endif()
    endif()
    file(MAKE_DIRECTORY "${DEST_DIR}")
    file(COPY_FILE "${SOURCE}" "${DEST_DIR}/ipinfo_lite.mmdb" ONLY_IF_DIFFERENT)
elseif(REQUIRED)
    message(FATAL_ERROR "The GeoIP database ${SOURCE} is missing, and a release has to ship it. "
        "Set IPINFO_TOKEN and configure again to download it.")
endif()
