# ---------------------------------------------------------------------------
# libepoc: transport + decode for the Emotiv EPOC dongle
# ---------------------------------------------------------------------------

include(GetHIDAPI)

add_library(epoc STATIC
    src/aes128.cpp
    src/protocol.cpp
    src/device.cpp
    src/dsp.cpp
    src/osc.cpp)

target_include_directories(epoc
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# hidapi is an implementation detail: it appears in no public header, so it
# stays off the interface of this target.
target_link_libraries(epoc
    PRIVATE hidapi::hidapi xavier_warnings)

# Platform libraries. hidapi's own targets already carry most of these, but
# they are named explicitly so a system hidapi found via pkg-config (which may
# not propagate them) still links.
if(APPLE)
    target_link_libraries(epoc PRIVATE
        "-framework IOKit" "-framework CoreFoundation" "-framework AppKit")
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(epoc PRIVATE Threads::Threads)
endif()

if(WIN32)
    # Winsock, for the OSC UDP sender. BSD sockets need no extra library on
    # Linux and macOS.
    target_link_libraries(epoc PRIVATE ws2_32)
endif()

set_target_properties(epoc PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME epoc)

# ---------------------------------------------------------------------------
# epoc CLI
# ---------------------------------------------------------------------------

add_executable(epoc_cli
    cli/main.cpp
    cli/terminal.cpp)

# The library target already owns the name "epoc"; give the executable that
# name on disk without colliding in CMake's target namespace.
set_target_properties(epoc_cli PROPERTIES OUTPUT_NAME epoc)

target_link_libraries(epoc_cli PRIVATE epoc xavier_warnings)

# Single source of truth for the version string: the project() call.
target_compile_definitions(epoc_cli PRIVATE
    XAVIER_EPOC_VERSION="${PROJECT_VERSION}")

install(TARGETS epoc_cli RUNTIME DESTINATION bin)
install(TARGETS epoc ARCHIVE DESTINATION lib)
install(DIRECTORY include/epoc DESTINATION include)

# ---------------------------------------------------------------------------
# Tests (no hardware required)
# ---------------------------------------------------------------------------

if(XAVIER_BUILD_TESTS)
    # Protocol tests reach into src/ to exercise the AES implementation
    # directly, so they get the source root on the include path.
    add_executable(epoc_tests test/test_epoc.cpp)
    target_include_directories(epoc_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(epoc_tests PRIVATE epoc xavier_warnings)
    add_test(NAME epoc_protocol COMMAND epoc_tests)

    add_executable(epoc_dsp_tests test/test_dsp.cpp)
    target_link_libraries(epoc_dsp_tests PRIVATE epoc xavier_warnings)
    add_test(NAME epoc_dsp COMMAND epoc_dsp_tests)

    add_executable(epoc_osc_tests test/test_osc.cpp)
    target_link_libraries(epoc_osc_tests PRIVATE epoc xavier_warnings)
    add_test(NAME epoc_osc COMMAND epoc_osc_tests)
endif()
