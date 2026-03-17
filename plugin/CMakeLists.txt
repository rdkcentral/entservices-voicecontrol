###
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2024 RDK Management
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

find_package(${NAMESPACE}Plugins REQUIRED)
find_package(${NAMESPACE}Definitions REQUIRED)
find_package(${NAMESPACE}Core REQUIRED)

if (USE_THUNDER_R4)
    find_package(${NAMESPACE}COM REQUIRED)
    find_package(${NAMESPACE}WebSocket REQUIRED)
else()
    find_package(${NAMESPACE}Protocols REQUIRED)
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
target_link_libraries(${MODULE_NAME} PRIVATE
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        ${NAMESPACE}Definitions::${NAMESPACE}Definitions)

install(TARGETS ${MODULE_NAME}
        DESTINATION lib/${STORAGE_DIRECTORY}/plugins)

add_library(${PLUGIN_IMPLEMENTATION} SHARED
        VoiceControlImplementation.cpp
        Module.cpp)

set_target_properties(${PLUGIN_IMPLEMENTATION} PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED YES)

target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ../helpers)

find_package(CTRLM)
if (CTRLM_FOUND)
    find_package(IARMBus)
    add_definitions(-DCTRLM_FOUND)
    target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${IARMBUS_INCLUDE_DIRS})
    target_include_directories(${PLUGIN_IMPLEMENTATION} PRIVATE ${CTRLM_INCLUDE_DIRS})
    target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE
            ${NAMESPACE}Plugins::${NAMESPACE}Plugins
            ${NAMESPACE}Definitions::${NAMESPACE}Definitions
            ${IARMBUS_LIBRARIES})
else (CTRLM_FOUND)
    target_link_libraries(${PLUGIN_IMPLEMENTATION} PRIVATE
            ${NAMESPACE}Plugins::${NAMESPACE}Plugins
            ${NAMESPACE}Definitions::${NAMESPACE}Definitions)
endif(CTRLM_FOUND)

install(TARGETS ${PLUGIN_IMPLEMENTATION}
DESTINATION lib/${STORAGE_DIRECTORY}/plugins)

write_config(${PLUGIN_NAME})
