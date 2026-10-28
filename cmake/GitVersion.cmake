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

# component_version(<target> [VERSION <x.y.z>] [DEFINE_PREFIX <prefix>])
#
# Reads the component's version and git info for its startup log. Sets COMPONENT_VERSION_STRING,
# COMPONENT_GIT_BRANCH and COMPONENT_GIT_HASH in the caller's scope, and adds them to <target> as the
# <prefix>_VERSION_STRING, <prefix>_GIT_BRANCH and <prefix>_GIT_HASH compile definitions.
# VERSION defaults to PROJECT_VERSION and DEFINE_PREFIX to COMPONENT. Fails if the commit hash can't be read.

include(CMakeParseArguments) # cmake_parse_arguments is built in only from CMake 3.5

function(component_version target)
    cmake_parse_arguments(ARG "" "VERSION;DEFINE_PREFIX" "" ${ARGN})
    if(NOT ARG_VERSION)
        set(ARG_VERSION ${PROJECT_VERSION})
    endif()
    if(NOT ARG_DEFINE_PREFIX)
        set(ARG_DEFINE_PREFIX COMPONENT)
    endif()
    set(_dir ${CMAKE_CURRENT_SOURCE_DIR})

    # Branch: exact tag, else the checked-out branch, else (detached, e.g. Yocto) the first real branch containing HEAD
    execute_process(COMMAND git describe --tags --exact-match WORKING_DIRECTORY ${_dir} OUTPUT_VARIABLE _branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _branch)
        execute_process(COMMAND git symbolic-ref -q --short HEAD WORKING_DIRECTORY ${_dir} OUTPUT_VARIABLE _branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT _branch)
        execute_process(COMMAND git branch -a "--format=%(refname:short)" --contains HEAD COMMAND sed -n 2p WORKING_DIRECTORY ${_dir} OUTPUT_VARIABLE _branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REGEX REPLACE "^origin/" "" _branch "${_branch}")
    endif()

    execute_process(COMMAND git log -1 --format=%H WORKING_DIRECTORY ${_dir} OUTPUT_VARIABLE _hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT _hash)
        message(FATAL_ERROR "Could not read the git commit hash from ${_dir}; a git checkout is required")
    endif()

    # "++" marks a build with uncommitted (staged or unstaged) changes
    execute_process(COMMAND git diff --quiet HEAD WORKING_DIRECTORY ${_dir} RESULT_VARIABLE _dirty ERROR_QUIET)
    set(_version ${ARG_VERSION})
    if(_dirty EQUAL 1)
        set(_version "${ARG_VERSION}++")
    endif()

    target_compile_definitions(${target} PRIVATE
        ${ARG_DEFINE_PREFIX}_VERSION_STRING="${_version}"
        ${ARG_DEFINE_PREFIX}_GIT_BRANCH="${_branch}"
        ${ARG_DEFINE_PREFIX}_GIT_HASH="${_hash}")

    set(COMPONENT_VERSION_STRING ${_version} PARENT_SCOPE)
    set(COMPONENT_GIT_BRANCH ${_branch} PARENT_SCOPE)
    set(COMPONENT_GIT_HASH ${_hash} PARENT_SCOPE)
endfunction()
