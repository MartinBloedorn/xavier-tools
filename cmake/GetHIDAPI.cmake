# Provides the imported target hidapi::hidapi.
#
# Prefers a hidapi already installed on the system (distro package, Homebrew,
# vcpkg) and otherwise downloads a pinned release and builds it statically.
# This keeps a bare `cmake -B build` working on a fresh Windows machine, where
# there is no system package manager to lean on, while still letting Linux and
# macOS builds use the platform hidapi.
#
# Backends selected by hidapi itself:
#   Windows  hidapi_winapi  (hid.dll / setupapi, no extra dependencies)
#   Linux    hidapi_hidraw  (requires libudev headers: libudev-dev)
#   macOS    hidapi_darwin  (IOKit + CoreFoundation frameworks)

include_guard(GLOBAL)

set(XAVIER_HIDAPI_VERSION "0.14.0" CACHE STRING
    "hidapi release tag to download when no system hidapi is used")
option(XAVIER_USE_SYSTEM_HIDAPI
    "Use a system-installed hidapi when one can be found" ON)

if(TARGET hidapi::hidapi)
    return()
endif()

set(_xavier_hidapi_found FALSE)

if(XAVIER_USE_SYSTEM_HIDAPI)
    # hidapi ships a CMake package config in recent versions.
    find_package(hidapi QUIET CONFIG)
    if(hidapi_FOUND AND TARGET hidapi::hidapi)
        set(_xavier_hidapi_found TRUE)
        set(XAVIER_HIDAPI_ORIGIN "system (CMake package)" CACHE INTERNAL "")
    endif()

    # Older installs only ship a pkg-config file.
    if(NOT _xavier_hidapi_found)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            # hidraw is the preferred backend on Linux; libusb is the fallback.
            pkg_check_modules(_HIDAPI QUIET IMPORTED_TARGET hidapi-hidraw)
            if(NOT _HIDAPI_FOUND)
                pkg_check_modules(_HIDAPI QUIET IMPORTED_TARGET hidapi)
            endif()
            if(NOT _HIDAPI_FOUND)
                pkg_check_modules(_HIDAPI QUIET IMPORTED_TARGET hidapi-libusb)
            endif()
            if(_HIDAPI_FOUND)
                # A pkg-config imported target is not GLOBAL, so it cannot be
                # ALIASed directly; wrap it in an interface library instead.
                add_library(xavier_hidapi_pc INTERFACE)
                target_link_libraries(xavier_hidapi_pc INTERFACE PkgConfig::_HIDAPI)
                add_library(hidapi::hidapi ALIAS xavier_hidapi_pc)
                set(_xavier_hidapi_found TRUE)
                set(XAVIER_HIDAPI_ORIGIN "system (pkg-config)" CACHE INTERNAL "")
            endif()
        endif()
    endif()
endif()

if(NOT _xavier_hidapi_found)
    include(FetchContent)

    # hidapi's own build knobs. Set before the subproject is added.
    set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
    set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(HIDAPI_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
    # Static, so the CLI is a single self-contained binary.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(hidapi
        GIT_REPOSITORY https://github.com/libusb/hidapi.git
        GIT_TAG        "hidapi-${XAVIER_HIDAPI_VERSION}"
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)

    message(STATUS "hidapi: no system install found, fetching ${XAVIER_HIDAPI_VERSION}")
    FetchContent_MakeAvailable(hidapi)

    if(NOT TARGET hidapi::hidapi)
        message(FATAL_ERROR
            "hidapi was fetched but did not define hidapi::hidapi. On Linux this "
            "usually means the libudev headers are missing; install libudev-dev "
            "(Debian/Ubuntu) or systemd-devel (Fedora), or configure with "
            "-DXAVIER_USE_SYSTEM_HIDAPI=ON against an installed hidapi.")
    endif()

    set(XAVIER_HIDAPI_ORIGIN "fetched ${XAVIER_HIDAPI_VERSION}" CACHE INTERNAL "")
endif()
