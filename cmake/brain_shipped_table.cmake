# brain_shipped_table.cmake — run at build time with cmake -P.
#
# Writes the table of the brain files this build ships, with the SHA-256 of
# each, as a C header (brain_shipped_table.h). A client built with
# WINBOLO_BOTHOST_OFFICIAL_ONLY checks its installed brain files against it
# before its Bot Host runs a bot (brainSyncShippedCheck in brain_sync.c), so
# a brain file that a person changed on disk is not run.
#
# A brain is every folder under BRAINS_DIR that holds an init.lua or an
# opt/init.lua: no list names them, so a brain folder that a merge adds is
# in the table of the next build. The files are the ones the copy step
# ships (cmake/copy_brains.cmake): every file but *.md. A name with a part
# that starts with '.' is left out too, as the Bot Host's own file walk
# leaves it out. The table holds every such file; which of them a brain
# runs (all of them, or opt/ and the top-level files opt/ does not replace)
# is decided at run time, by the same rule as brainSyncCollectBrain.
#
# A text file (.lua, .txt, .json, any case) is hashed as brainSyncLoadFile
# reads it: every run of CRs that a LF comes after is taken out, so a
# Windows checkout with CRLF line endings gives the same table as a Linux
# one. A lone CR stays. Any other file (a .map, a .bin) is hashed as it is.
#
# Pass -DBRAINS_DIR=<source brains folder> -DOUT=<header> and
# -DSTAMP=<stamp file>. The header is rewritten only when its text changes,
# so a brain edit that leaves every hash the same compiles nothing; the
# stamp is touched on each run.

if(NOT DEFINED BRAINS_DIR OR NOT DEFINED OUT)
    message(FATAL_ERROR
        "brain_shipped_table.cmake requires -DBRAINS_DIR=... -DOUT=...")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/brain_folders.cmake")
winbolo_brain_folders(_brains "${BRAINS_DIR}")

# The lower-case SHA-256 of the brain file at path into out. A text file
# (.lua, .txt, .json) is hashed with its line endings made LF, as
# brainSyncNormalizeText in src/bolo/brain_sync.c makes them: each run of
# CRs that a LF comes after is taken out, and a lone CR stays.
#
# file(READ) without HEX is not exact (it drops a CR at the end of each line
# and at the end of the file, and on Windows reads in text mode), so it is
# used only when its result can be checked against the file's real bytes:
#   1. No 0d in the hex: there is no CR, and the file is hashed as it is.
#   2. A plain read with no CR left in it, whose hex is the file's hex with
#      each 0d0a made 0a: the file is plain CRLF (or mixed CRLF and LF)
#      text, and the read is its LF form.
#   3. Else (a lone CR, CR CR LF, a CR at the end, a NUL or a Ctrl-Z) the
#      hex is cut into bytes, the CRs before each LF are taken out, and the
#      bytes are put back into a string. This is exact but slow, about three
#      seconds a megabyte, and no shipped brain file needs it.
function(winbolo_brain_file_sha256 out path)
    if(NOT path MATCHES "\\.([Ll][Uu][Aa]|[Tt][Xx][Tt]|[Jj][Ss][Oo][Nn])$")
        file(SHA256 "${path}" _sha)
        string(TOLOWER "${_sha}" _sha)
        set(${out} "${_sha}" PARENT_SCOPE)
        return()
    endif()
    file(READ "${path}" _hex HEX)
    string(TOLOWER "${_hex}" _hex)
    string(FIND "${_hex}" "0d" _at)
    if(_at EQUAL -1)
        file(SHA256 "${path}" _sha)                                # 1.
    else()
        file(READ "${path}" _text)
        string(FIND "${_text}" "\r" _cr)
        string(HEX "${_text}" _textHex)
        string(REPLACE "0d0a" "0a" _lfHex "${_hex}")
        if(_cr EQUAL -1 AND _textHex STREQUAL _lfHex)
            string(SHA256 _sha "${_text}")                         # 2.
        else()
            string(REGEX REPLACE "(..)" "h\\1;" _bytes "${_hex}")  # 3.
            string(REGEX REPLACE "(h0d;)+h0a;" "h0a;" _bytes "${_bytes}")
            string(FIND "${_bytes}" "h00;" _nul)
            if(NOT _nul EQUAL -1)
                message(FATAL_ERROR "brain file ${path} holds a NUL byte: "
                                    "it cannot be hashed as a text file")
            endif()
            set(_digits 0 1 2 3 4 5 6 7 8 9 a b c d e f)
            set(_n 0)
            foreach(_hi IN LISTS _digits)
                foreach(_lo IN LISTS _digits)
                    string(REPLACE "h${_hi}${_lo};" "${_n};" _bytes "${_bytes}")
                    math(EXPR _n "${_n} + 1")
                endforeach()
            endforeach()
            string(REGEX REPLACE ";$" "" _bytes "${_bytes}")
            set(_text "")
            if(NOT _bytes STREQUAL "")
                string(ASCII ${_bytes} _text)
            endif()
            string(SHA256 _sha "${_text}")
        endif()
    endif()
    string(TOLOWER "${_sha}" _sha)
    set(${out} "${_sha}" PARENT_SCOPE)
endfunction()

set(_rows "")
set(_count 0)
foreach(_b IN LISTS _brains)
    file(GLOB_RECURSE _files RELATIVE "${BRAINS_DIR}/${_b}"
        "${BRAINS_DIR}/${_b}/*")
    list(SORT _files)
    foreach(_f IN LISTS _files)
        if(_f MATCHES "\\.md$" OR _f MATCHES "(^|/)\\.")
            continue()
        endif()
        if(IS_DIRECTORY "${BRAINS_DIR}/${_b}/${_f}")
            continue()
        endif()
        winbolo_brain_file_sha256(_sha "${BRAINS_DIR}/${_b}/${_f}")
        set(_p "${_b}/${_f}")
        string(REPLACE "\\" "\\\\" _p "${_p}")
        string(REPLACE "\"" "\\\"" _p "${_p}")
        string(APPEND _rows "    { \"${_p}\", \"${_sha}\" },\n")
        math(EXPR _count "${_count} + 1")
    endforeach()
endforeach()

set(_text "/* Generated by cmake/brain_shipped_table.cmake at build time from\n")
string(APPEND _text " * the brains/ folder. Do not edit: the build writes it again. */\n")
string(APPEND _text "#ifndef BRAIN_SHIPPED_TABLE_H\n#define BRAIN_SHIPPED_TABLE_H\n\n")
string(APPEND _text "#include \"brain_sync.h\"   /* BrainSyncShippedFile */\n\n")
string(APPEND _text "/* ${_count} files of the brains: ${_brains} */\n")
string(APPEND _text "static const BrainSyncShippedFile brainShippedTable[] = {\n")
string(APPEND _text "${_rows}")
string(APPEND _text "    { NULL, NULL }\n};\n")
string(APPEND _text "static const int brainShippedCount = ${_count};\n\n")
string(APPEND _text "#endif\n")

file(WRITE "${OUT}.tmp" "${_text}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
if(DEFINED STAMP)
    file(TOUCH "${STAMP}")
endif()
