# Script mode: copy the cached IPinfo Lite database into a build output's
# data/ directory. Run at build time rather than configure time, so a copy
# downloaded after the tree was configured is still picked up.
#
#   -DSOURCE=<cached ipinfo_lite.mmdb>
#   -DDEST_DIR=<output data/ directory>
#   -DREQUIRED=ON   fail when the database is missing (the dist target)

if(EXISTS "${SOURCE}")
    file(MAKE_DIRECTORY "${DEST_DIR}")
    file(COPY_FILE "${SOURCE}" "${DEST_DIR}/ipinfo_lite.mmdb" ONLY_IF_DIFFERENT)
elseif(REQUIRED)
    message(FATAL_ERROR "The GeoIP database ${SOURCE} is missing, and a release has to ship it. "
        "Set IPINFO_TOKEN and configure again to download it.")
endif()
