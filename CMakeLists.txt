cmake_minimum_required(VERSION 3.16)

project(xavier-tools
    VERSION 0.1.0
    DESCRIPTION "Tools for the Emotiv EPOC EEG headset"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Project-wide settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep binaries in one predictable place regardless of generator. Multi-config
# generators (Visual Studio, Xcode) append the config name themselves.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Default to a Release build for single-config generators; an EEG read loop
# built -O0 is needlessly slow.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# Helps clangd/VS Code find headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(XAVIER_BUILD_TESTS "Build the hardware-free test suite" ON)
option(XAVIER_WERROR "Treat compiler warnings as errors" OFF)

# ---------------------------------------------------------------------------
# Warning flags, applied via an interface target rather than globally so
# fetched third-party code (hidapi) is not held to our warning level.
# ---------------------------------------------------------------------------

add_library(xavier_warnings INTERFACE)

if(MSVC)
    target_compile_options(xavier_warnings INTERFACE /W4 /permissive- /utf-8)
    # Silence the CRT "use the _s variants" advice: the standard functions we
    # use are portable and used correctly.
    target_compile_definitions(xavier_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(XAVIER_WERROR)
        target_compile_options(xavier_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(xavier_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(XAVIER_WERROR)
        target_compile_options(xavier_warnings INTERFACE -Werror)
    endif()
endif()

if(XAVIER_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(epoc)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "")
message(STATUS "xavier-tools ${PROJECT_VERSION}")
message(STATUS "  system      : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  compiler    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type  : ${CMAKE_BUILD_TYPE}${CMAKE_CONFIGURATION_TYPES}")
message(STATUS "  hidapi      : ${XAVIER_HIDAPI_ORIGIN}")
message(STATUS "  tests       : ${XAVIER_BUILD_TESTS}")
message(STATUS "")
