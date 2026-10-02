# wasm_dist.cmake — the wasm-dist packaging target, shared by the web game
# client (src/wasm) and the web log viewer (src/logviewer/wasm).
#
# bolo_add_wasm_dist(<target> COMPRESS <file>... [PLAIN <file>...])
#
# Adds a wasm-dist target that precompresses the web assets (for Caddy
# `file_server { precompressed br gzip }`) and packs everything a deploy needs
# into wasm-dist.zip beside <target>'s output:
#   COMPRESS  each file with its .br and .gz copies
#   PLAIN     files packed as they are (images that are already compressed)
#   and LICENSE, LICENSE-EXCEPTION.md and THIRD_PARTY_NOTICES.md, copied in
#   from REPO_ROOT. They are served beside the page as plain text, so they
#   are not precompressed.
# Paths are relative to <target>'s output directory, and the zip keeps them,
# so unzipping it into the site root is the whole deploy. See
# docs/web-hosting.md.
#
# Run explicitly — it is NOT part of the normal build, so day-to-day builds
# don't re-brotli the data file:
#     cmake --build <build-dir> --target wasm-dist
# Requires `brotli` (apt-get install brotli, brew install brotli); gzip is
# already standard. Without them the target is not created and configure
# warns.

function(bolo_add_wasm_dist target)
    cmake_parse_arguments(PARSE_ARGV 1 _wd "" "" "COMPRESS;PLAIN")

    find_program(BROTLI_EXE brotli)
    find_program(GZIP_EXE   gzip)
    if(NOT BROTLI_EXE OR NOT GZIP_EXE)
        message(WARNING
            "wasm-dist: brotli and/or gzip not found "
            "(apt-get install brotli, brew install brotli) — "
            "the wasm-dist target is unavailable")
        return()
    endif()

    # -k keeps the original (Caddy needs it as the fallback for clients that
    # accept neither encoding); -f overwrites stale variants.
    set(_cmds)
    set(_zip_files)
    foreach(_a IN LISTS _wd_COMPRESS)
        list(APPEND _cmds COMMAND ${BROTLI_EXE} -f -k -q 11 "${_a}")
        list(APPEND _cmds COMMAND ${GZIP_EXE} -f -k -9 "${_a}")
        list(APPEND _zip_files "${_a}" "${_a}.br" "${_a}.gz")
    endforeach()
    list(APPEND _zip_files ${_wd_PLAIN})

    foreach(_l IN ITEMS LICENSE LICENSE-EXCEPTION.md THIRD_PARTY_NOTICES.md)
        list(APPEND _cmds
             COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REPO_ROOT}/${_l}" "${_l}")
        list(APPEND _zip_files "${_l}")
    endforeach()

    add_custom_target(wasm-dist
        ${_cmds}
        COMMAND ${CMAKE_COMMAND} -E tar "cf" wasm-dist.zip --format=zip
                ${_zip_files}
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${target}>
        COMMENT "Precompressing web assets (.br + .gz) and packing wasm-dist.zip"
        VERBATIM
    )
    add_dependencies(wasm-dist ${target})
endfunction()
