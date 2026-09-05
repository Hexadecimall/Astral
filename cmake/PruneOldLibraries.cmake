# Removes versioned library files left behind by earlier versions.
#
# The shared library carries its version in its file name, so every bump
# writes a new file and the build tree keeps the old ones for ever. Nothing
# links them once the bump lands; they are dead weight, and a stale one is
# easy to load by accident.
#
# Expects DIR, PATTERN and KEEP.

file(GLOB _found "${DIR}/${PATTERN}")
foreach(_file ${_found})
    get_filename_component(_name "${_file}" NAME)
    if(NOT _name STREQUAL "${KEEP}" AND NOT IS_SYMLINK "${_file}")
        file(REMOVE "${_file}")
        message(STATUS "removed the library left by an earlier version: ${_name}")
    endif()
endforeach()
