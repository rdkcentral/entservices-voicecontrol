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
    find_package(${NAMESPACE}Protocols QUIET)
    if (NOT ${NAMESPACE}Protocols_FOUND)
        message(WARNING "${NAMESPACE}Protocols package not found, falling back to ${NAMESPACE}COM/${NAMESPACE}WebSocket")
        find_package(${NAMESPACE}COM REQUIRED)
        find_package(${NAMESPACE}WebSocket REQUIRED)
    endif()
endif()

if (BUILD_TESTS)
    add_subdirectory(test)
endif(BUILD_TESTS)

## Plugin library (loaded by Thunder framework — thin JSON-RPC + COM-RPC shell)
add_library(${MODULE_NAME} SHARED
        VoiceControl.cpp
        Module.cpp)

set_target_properties(${MODULE_NAME} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

target_include_directories(${MODULE_NAME} PRIVATE ../helpers)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Thunder 5.3 generated JSON enum conversion handlers rely on namespace aliases.
        # GCC/Clang reject this without permissive mode.
        target_compile_options(${MODULE_NAME} PRIVATE -fpermissive)
endif()
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
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${PLUGIN_IMPLEMENTATION} PRIVATE -fpermissive)
endif()

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
