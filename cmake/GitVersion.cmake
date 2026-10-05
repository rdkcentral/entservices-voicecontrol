###
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
###

# Derives the plugin version info at configure time so PluginVersion.h never
# needs hand-editing. See "Versioning" in README.md.
#
# Sets:
#   PLUGIN_VERSION_MAJOR / _MINOR / _PATCH  - from PLUGIN_VERSION (the recipe's ${PV}),
#                                             else the nearest x.y.z tag, else BUILD_REFERENCE
#   PLUGIN_VERSION_STRING                   - the full version (e.g. 1.0.5), with "++" appended if the tree is modified
#   PLUGIN_GIT_BRANCH                       - exact tag, current branch, or a branch containing HEAD
#   PLUGIN_GIT_HASH                         - full commit hash, or "unknown"
#   PLUGIN_BUILD_REFERENCE                  - value for Thunder's BUILD_REFERENCE define

set(PLUGIN_VERSION "" CACHE STRING "Plugin version x.y.z (Yocto recipes pass \${PV})")

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)

set(_git_tag "")
set(_git_dirty FALSE)
set(PLUGIN_GIT_HASH "")
set(PLUGIN_GIT_BRANCH "")

find_package(Git QUIET)
if(GIT_FOUND)
    # Only trust git if this repo is the top level, not a parent repo it was copied into.
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --show-toplevel
        WORKING_DIRECTORY ${_repo_root}
        OUTPUT_VARIABLE _git_toplevel
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_git_toplevel)
        get_filename_component(_git_toplevel "${_git_toplevel}" REALPATH)
    endif()

    if(_git_toplevel STREQUAL _repo_root)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            WORKING_DIRECTORY ${_repo_root}
            OUTPUT_VARIABLE PLUGIN_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        # --match skips non-version tags such as IMPORT_INITIAL_develop; suffixed tags (1.0.5.1, 1.0.2-RDK7.1) are accepted below
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "[0-9]*.[0-9]*.[0-9]*"
            WORKING_DIRECTORY ${_repo_root}
            OUTPUT_VARIABLE _git_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND ${GIT_EXECUTABLE} diff --quiet HEAD
            WORKING_DIRECTORY ${_repo_root}
            RESULT_VARIABLE _git_diff_result
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _git_diff_result EQUAL 0)
            set(_git_dirty TRUE)
        endif()

        # Branch: exact tag, else the checked-out branch, else the first branch containing
        # HEAD (Yocto checks out a detached SRCREV, so this is usually e.g. origin/develop).
        execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
            WORKING_DIRECTORY ${_repo_root}
            OUTPUT_VARIABLE PLUGIN_GIT_BRANCH
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT PLUGIN_GIT_BRANCH)
            execute_process(COMMAND ${GIT_EXECUTABLE} symbolic-ref -q --short HEAD
                WORKING_DIRECTORY ${_repo_root}
                OUTPUT_VARIABLE PLUGIN_GIT_BRANCH
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
        if(NOT PLUGIN_GIT_BRANCH)
            execute_process(COMMAND ${GIT_EXECUTABLE} branch -a --contains HEAD "--format=%(refname:short)"
                WORKING_DIRECTORY ${_repo_root}
                OUTPUT_VARIABLE _git_branches
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            string(REPLACE "\n" ";" _git_branches "${_git_branches}")
            foreach(_branch IN LISTS _git_branches)
                # Skip "(HEAD detached at ...)"
                if(NOT _branch MATCHES "^\\(")
                    set(PLUGIN_GIT_BRANCH "${_branch}")
                    break()
                endif()
            endforeach()
        endif()
    endif()
endif()

if(PLUGIN_VERSION)
    set(_version_source "${PLUGIN_VERSION}")
elseif(_git_tag)
    set(_version_source "${_git_tag}")
else()
    set(_version_source "${BUILD_REFERENCE}")
endif()

# x.y.z with an optional ".n" or "-text" suffix; no leading zeros since the parts become C++ integer literals
if(_version_source MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)([.-].*)?$")
    set(PLUGIN_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(PLUGIN_VERSION_MINOR ${CMAKE_MATCH_2})
    set(PLUGIN_VERSION_PATCH ${CMAKE_MATCH_3})
    set(PLUGIN_VERSION_STRING "${_version_source}")
elseif(PLUGIN_VERSION)
    message(FATAL_ERROR "PLUGIN_VERSION '${PLUGIN_VERSION}' must start with x.y.z (no leading zeros)")
else()
    # Keep API major 1 so getApiVersionNumber and Thunder registration don't change in untagged builds
    message(WARNING "No PLUGIN_VERSION or x.y.z version tag found (shallow clone or no git?); using version 1.0.0")
    set(PLUGIN_VERSION_MAJOR 1)
    set(PLUGIN_VERSION_MINOR 0)
    set(PLUGIN_VERSION_PATCH 0)
    set(PLUGIN_VERSION_STRING "1.0.0")
endif()

# Thunder stores plugin versions as uint8_t
foreach(_part MAJOR MINOR PATCH)
    if(PLUGIN_VERSION_${_part} GREATER 255)
        message(FATAL_ERROR "Version ${_version_source}: each component must be <= 255 (Thunder stores them as uint8_t)")
    endif()
endforeach()

if(_git_dirty)
    # set() rather than string(APPEND), which needs CMake 3.4
    set(PLUGIN_VERSION_STRING "${PLUGIN_VERSION_STRING}++")
endif()

if(NOT PLUGIN_GIT_BRANCH)
    set(PLUGIN_GIT_BRANCH "unknown")
endif()

if(PLUGIN_GIT_HASH)
    set(PLUGIN_BUILD_REFERENCE "${PLUGIN_GIT_HASH}")
else()
    set(PLUGIN_GIT_HASH "unknown")
    set(PLUGIN_BUILD_REFERENCE "${BUILD_REFERENCE}")
endif()
