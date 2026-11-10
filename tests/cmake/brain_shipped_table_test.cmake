# brain_shipped_table_test.cmake — a check of the build's brain discovery.
#
# Shows that a brain folder that a merge adds needs no other step: the
# table of shipped brain files (cmake/brain_shipped_table.cmake) and the
# copy step (cmake/copy_brains.cmake -DALL_BRAINS=ON) both find it on the
# next run, and leave out *.md files, dot files and a folder with no
# init.lua.
#
# Run: cmake -DSOURCE_DIR=<repo> -DWORK_DIR=<empty scratch folder>
#            -P tests/cmake/brain_shipped_table_test.cmake
# CTest runs it as brain_shipped_table_glob.

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED WORK_DIR)
    message(FATAL_ERROR "needs -DSOURCE_DIR=... -DWORK_DIR=...")
endif()

set(_brains "${WORK_DIR}/brains")
set(_out "${WORK_DIR}/brain_shipped_table.h")
set(_dest "${WORK_DIR}/install/Brains")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

file(WRITE "${_brains}/Alpha/init.lua" "return 'alpha'\n")
file(WRITE "${_brains}/Alpha/lib/util.lua" "return 1\n")
file(WRITE "${_brains}/Beta/init.lua" "return 'beta full'\n")
file(WRITE "${_brains}/Beta/opt/init.lua" "return 'beta opt'\n")
file(WRITE "${_brains}/Beta/notes.md" "plan notes\n")
file(WRITE "${_brains}/Beta/.hidden" "skip\n")
file(WRITE "${_brains}/NotABrain/readme.txt" "no init.lua here\n")

function(_run_table)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DBRAINS_DIR=${_brains} -DOUT=${_out}
                -P ${SOURCE_DIR}/cmake/brain_shipped_table.cmake
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "brain_shipped_table.cmake failed (${_rc})")
    endif()
endfunction()

function(_expect_row rel)
    file(READ "${_out}" _t)
    file(SHA256 "${_brains}/${rel}" _sha)
    string(TOLOWER "${_sha}" _sha)
    string(FIND "${_t}" "{ \"${rel}\", \"${_sha}\" }" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "the table has no row for ${rel} with its SHA-256")
    endif()
endfunction()

function(_expect_no rel)
    file(READ "${_out}" _t)
    string(FIND "${_t}" "\"${rel}\"" _at)
    if(NOT _at EQUAL -1)
        message(FATAL_ERROR "the table lists ${rel}, which does not ship")
    endif()
endfunction()

function(_expect_count n)
    file(READ "${_out}" _t)
    string(FIND "${_t}" "brainShippedCount = ${n};" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "the table does not hold ${n} files")
    endif()
endfunction()

# 1. The two brains, every file but the notes and the dot file.
_run_table()
_expect_row("Alpha/init.lua")
_expect_row("Alpha/lib/util.lua")
_expect_row("Beta/init.lua")
_expect_row("Beta/opt/init.lua")
_expect_no("Beta/notes.md")
_expect_no("Beta/.hidden")
_expect_no("NotABrain/readme.txt")
_expect_no("Gamma/init.lua")
_expect_count(4)

# 2. A merge adds the brain folder Gamma: the next run has it, with no list
#    to edit. Only its opt/ copy is there, which is a brain too.
file(WRITE "${_brains}/Gamma/opt/init.lua" "return 'gamma'\n")
file(WRITE "${_brains}/Gamma/modes.txt" "default\n")
_run_table()
_expect_row("Gamma/opt/init.lua")
_expect_row("Gamma/modes.txt")
_expect_count(6)

# 3. An edit changes that file's hash in the table.
file(WRITE "${_brains}/Alpha/init.lua" "return 'alpha, edited'\n")
_run_table()
_expect_row("Alpha/init.lua")

# 4. The copy step ships every brain folder, Gamma too, without the notes.
execute_process(
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${_brains} -DDEST_DIR=${_dest}
            -DALL_BRAINS=ON -P ${SOURCE_DIR}/cmake/copy_brains.cmake
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "copy_brains.cmake failed (${_rc})")
endif()
foreach(_f "Alpha/init.lua" "Alpha/lib/util.lua" "Beta/opt/init.lua"
           "Gamma/opt/init.lua" "Gamma/modes.txt")
    if(NOT EXISTS "${_dest}/${_f}")
        message(FATAL_ERROR "the copy step did not ship ${_f}")
    endif()
endforeach()
foreach(_f "Beta/notes.md" "NotABrain/readme.txt")
    if(EXISTS "${_dest}/${_f}")
        message(FATAL_ERROR "the copy step shipped ${_f}")
    endif()
endforeach()

message(STATUS "brain_shipped_table_glob: passed")
