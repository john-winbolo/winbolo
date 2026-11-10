# brain_folders.cmake — the brain folders under a brains/ folder.
#
# A brain is every folder that holds an init.lua or an opt/init.lua. No list
# names them: a brain folder that a merge adds ships with the next build and
# is in its table of shipped files (brain_shipped_table.cmake). Included by
# copy_brains.cmake, brain_shipped_table.cmake and the top CMakeLists.txt.

function(winbolo_brain_folders out dir)
    file(GLOB _inits RELATIVE "${dir}" "${dir}/*/init.lua" "${dir}/*/opt/init.lua")
    set(_brains "")
    foreach(_i IN LISTS _inits)
        string(REGEX REPLACE "/.*$" "" _b "${_i}")
        if(NOT _b MATCHES "^\\.")
            list(APPEND _brains "${_b}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _brains)
    list(SORT _brains)
    set(${out} "${_brains}" PARENT_SCOPE)
endfunction()
