# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

cmake_minimum_required(VERSION 3.25...3.31)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(cmake/validate_special_target.cmake)
include(cmake/version.cmake)
desktop_app_parse_version(Telegram/build/version)

set(FISHGRAM_REVISION "1" CACHE STRING "FishGram release revision (positive uint32).")
if (NOT FISHGRAM_REVISION MATCHES "^[1-9][0-9]*$" OR FISHGRAM_REVISION GREATER 4294967295)
    message(FATAL_ERROR "FISHGRAM_REVISION must be a positive uint32.")
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/Telegram/build/version" fishgram_version_line REGEX "^AppVersion ")
string(REGEX REPLACE "^AppVersion[ \t]+" "" fishgram_base_version "${fishgram_version_line}")
add_compile_definitions(FISHGRAM_REVISION=${FISHGRAM_REVISION} FISHGRAM_BASE_VERSION=${fishgram_base_version})

if (NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(configuration_types_init 1)
endif()

project(Telegram
    LANGUAGES C CXX
    VERSION ${desktop_app_version_cmake}
    DESCRIPTION "FishGram unofficial client based on Telegram API"
    HOMEPAGE_URL "https://github.com/lonefisher/fishgram"
)

if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

if (configuration_types_init
    AND CMAKE_CONFIGURATION_TYPES
    AND NOT MinSizeRel IN_LIST CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES};MinSizeRel" CACHE STRING "" FORCE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Telegram)

get_filename_component(third_party_loc "Telegram/ThirdParty" REALPATH)
get_filename_component(submodules_loc "Telegram" REALPATH)
get_filename_component(cmake_helpers_loc "cmake" REALPATH)

if (NOT DESKTOP_APP_USE_PACKAGED AND WIN32)
    set(Python3_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/python/Scripts/python)
endif()

include(cmake/variables.cmake)
include(cmake/nice_target_sources.cmake)
include(cmake/target_compile_options_if_exists.cmake)
include(cmake/target_link_frameworks.cmake)
include(cmake/target_link_options_if_exists.cmake)
include(cmake/init_target.cmake)
include(cmake/generate_target.cmake)
include(cmake/nuget.cmake)
include(cmake/validate_d3d_compiler.cmake)
include(cmake/target_prepare_qrc.cmake)

include(cmake/options.cmake)
include(cmake/external/qt/package.cmake)

set(desktop_app_skip_libs
    glibmm
    variant
)

add_subdirectory(cmake)
add_subdirectory(Telegram)
