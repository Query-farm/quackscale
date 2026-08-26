cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME quackscale)

option(QUACKSCALE_WITH_TAILSCALE "Embed libtailscale (requires Go)" ON)
option(QUACKSCALE_WITH_WIREBONE "Embed Wirebone coordinator (required on POSIX unless OFF)" ON)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/quackscale_extension.cpp src/attach_ducklake.cpp src/tailscale_bridge.cpp src/tailscale_forwarder.cpp src/tailscale_log_capture.cpp src/tailscale_http.cpp src/wirebone_bridge.cpp src/wirebone_functions.cpp src/wirebone_catalog.cpp)

if(QUACKSCALE_WITH_TAILSCALE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Libtailscale.cmake)
endif()

# Code-quality CI (`make tidy-check`) configures without vcpkg, so Wirebone's
# required OpenSSL/nghttp2/zstd lookup would FATAL_ERROR. Real builds still
# fail closed; clang-tidy only needs compile_commands for src/.
if(QUACKSCALE_WITH_WIREBONE AND NOT CLANG_TIDY)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Wirebone.cmake)
elseif(CLANG_TIDY AND QUACKSCALE_WITH_WIREBONE)
    message(STATUS "QuackScale: skipping Wirebone configure for clang-tidy")
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

function(quackscale_link_tailscale target_name)
    if(NOT QUACKSCALE_WITH_TAILSCALE)
        target_compile_definitions(${target_name} PRIVATE QUACKSCALE_WITH_TAILSCALE=0)
        return()
    endif()

    target_compile_definitions(${target_name} PRIVATE QUACKSCALE_WITH_TAILSCALE=1)
    target_include_directories(${target_name} PRIVATE ${QUACKSCALE_LIBTAILSCALE_INCLUDE})
    add_dependencies(${target_name} libtailscale_archive)
    target_link_libraries(${target_name} ${QUACKSCALE_LIBTAILSCALE_ARCHIVE})

    if(APPLE)
        target_link_libraries(${target_name} "-framework CoreFoundation" "-framework Security"
                                "-framework SystemConfiguration" "-framework IOKit")
    elseif(UNIX)
        target_link_libraries(${target_name} resolv pthread)
    endif()
endfunction()

quackscale_link_tailscale(${EXTENSION_NAME})
quackscale_link_tailscale(${LOADABLE_EXTENSION_NAME})

function(quackscale_link_wirebone_or_stub target_name)
    if(QUACKSCALE_WITH_WIREBONE AND COMMAND quackscale_link_wirebone)
        quackscale_link_wirebone(${target_name})
    else()
        target_compile_definitions(${target_name} PRIVATE QUACKSCALE_WITH_WIREBONE=0)
    endif()
endfunction()

quackscale_link_wirebone_or_stub(${EXTENSION_NAME})
quackscale_link_wirebone_or_stub(${LOADABLE_EXTENSION_NAME})

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
