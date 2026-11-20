# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

# The bundle project's uninstall target (cmake -P): removes the files that the
# last cmake --install listed in MANIFEST (install_manifest.txt), then the
# directories that are left empty, up to share/kemu. DESTDIR is honoured.

if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR "Nothing to remove: ${MANIFEST} does not exist (run cmake --install first)")
endif()

file(STRINGS "${MANIFEST}" files)
set(dirs)
foreach(file IN LISTS files)
    set(path "$ENV{DESTDIR}${file}")
    if(EXISTS "${path}" OR IS_SYMLINK "${path}")
        message(STATUS "Removing: ${path}")
        file(REMOVE "${path}")
    endif()
    # Every directory between the file and share/kemu.
    get_filename_component(dir "${path}" DIRECTORY)
    while(dir MATCHES "/kemu/agent(/|$)")
        list(APPEND dirs "${dir}")
        get_filename_component(dir "${dir}" DIRECTORY)
    endwhile()
    list(APPEND dirs "${dir}") # share/kemu
endforeach()
list(REMOVE_DUPLICATES dirs)

# Deepest first, so that a directory is empty once its subdirectories are gone.
set(keyed)
foreach(dir IN LISTS dirs)
    string(LENGTH "${dir}" length)
    math(EXPR key "100000 + ${length}")
    list(APPEND keyed "${key}|${dir}")
endforeach()
list(SORT keyed COMPARE NATURAL ORDER DESCENDING)
foreach(entry IN LISTS keyed)
    string(REGEX REPLACE "^[0-9]+\\|" "" dir "${entry}")
    if(IS_DIRECTORY "${dir}")
        file(GLOB left LIST_DIRECTORIES true "${dir}/*" "${dir}/.*")
        if(NOT left)
            message(STATUS "Removing: ${dir}")
            file(REMOVE_RECURSE "${dir}") # empty, as just checked
        endif()
    endif()
endforeach()
