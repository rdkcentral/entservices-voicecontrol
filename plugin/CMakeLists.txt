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

set(PLUGIN_NAME VoiceControl)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})
set(PLUGIN_IMPLEMENTATION "${MODULE_NAME}Implementation" CACHE STRING "Library with VoiceControl implementation")
set(PLUGIN_VOICECONTROL_MODE "Local" CACHE STRING "Controls if the plugin should run in its own process, in process or remote.")

find_package(${NAMESPACE}Plugins REQUIRED)
find_package(${NAMESPACE}Definitions REQUIRED)
find_package(${NAMESPACE}Core REQUIRED)
find_package(CTRLM REQUIRED)
find_package(IARMBus)

if (USE_THUNDER_R4)
    find_package(${NAMESPACE}COM REQUIRED)
    find_package(${NAMESPACE}WebSocket REQUIRED)
else()
    find_package(${NAMESPACE}Protocols REQUIRED)
endif()

## Version: API numbers come from the project version; the startup log header is generated like ctrlm and xr-voice-sdk
add_definitions(-DAPI_VERSION_NUMBER_MAJOR=${PROJECT_VERSION_MAJOR} -DAPI_VERSION_NUMBER_MINOR=${PROJECT_VERSION_MINOR} -DAPI_VERSION_NUMBER_PATCH=${PROJECT_VERSION_PATCH})

add_custom_command(OUTPUT PluginVersion.h
   COMMAND echo -n "#define PLUGIN_GIT_BRANCH     \"" > PluginVersion.h
   COMMAND sh -c "src=\"${CMAKE_CURRENT_SOURCE_DIR}\"; ref=$(git -C \"$src\" describe --tags --exact-match 2>/dev/null || git -C \"$src\" branch -a --format='%(refname:short)' --contains HEAD 2>/dev/null | sed -n '2{p;q}'); echo -n \"$ref\" >> PluginVersion.h"
   COMMAND echo "\"" >> PluginVersion.h
   COMMAND echo -n "#define PLUGIN_GIT_HASH       " >> PluginVersion.h
   COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} log --format=\"%H\" -n 1 >> PluginVersion.h
   COMMAND echo -n "#define PLUGIN_VERSION_STRING \"${PROJECT_VERSION}" >> PluginVersion.h
   COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} diff --quiet || echo -n "++" >> PluginVersion.h
   COMMAND echo "\"" >> PluginVersion.h
   VERBATIM
)

if (BUILD_TESTS)
    add_subdirectory(test)
endif(BUILD_TESTS)

## Plugin library (loaded by Thunder framework — thin JSON-RPC + COM-RPC shell)
add_library(${MODULE_NAME} SHARED
        VoiceControl.cpp
        Module.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/PluginVersion.h)

set_target_properties(${MODULE_NAME} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

target_include_directories(${MODULE_NAME} PRIVATE ../helpers ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(${MODULE_NAME} PRIVATE
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        ${NAMESPACE}Definitions::${NAMESPACE}Definitions)

add_library(${PLUGIN_IMPLEMENTATION} SHARED
        VoiceControlImplementation.cpp
        Module.cpp)

set_target_properties(${PLUGIN_IMPLEMENTATION} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ../helpers)

target_compile_definitions(${PLUGIN_IMPLEMENTATION} PRIVATE CTRLM_FOUND)
target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${CTRLM_INCLUDE_DIRS})
target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        ${NAMESPACE}Definitions::${NAMESPACE}Definitions)

target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${IARMBUS_INCLUDE_DIRS})
target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE ${IARMBUS_LIBRARIES})

install(TARGETS ${MODULE_NAME} ${PLUGIN_IMPLEMENTATION}
        DESTINATION lib/${STORAGE_DIRECTORY}/plugins)

write_config(PLUGINS ${PLUGIN_NAME} CLASSNAME ${PLUGIN_NAME})
