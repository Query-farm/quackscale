# Embed the Wirebone coordinator (POSIX; needs OpenSSL, nghttp2, libzstd).
#
# Source lookup, first match wins:
#   1. QUACKSCALE_WIREBONE_DIR (cache/path)
#   2. ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wirebone
#   3. ${CMAKE_CURRENT_SOURCE_DIR}/../wirebone.cpp  (sibling checkout)
#   4. FetchContent from github.com/lmangani/wirebone.cpp
#
# DuckDB extension CI installs those libraries via vcpkg and sets
# OPENSSL_ROOT_DIR. Prepend that prefix to PKG_CONFIG_PATH so this file
# does not silently disable the hub (which made wirebone.test fail).

function(_quackscale_prepend_pkgconfig prefix)
    if(prefix AND IS_DIRECTORY "${prefix}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_PATH} "${prefix}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
    if(prefix AND IS_DIRECTORY "${prefix}/lib64/pkgconfig")
        set(ENV{PKG_CONFIG_PATH} "${prefix}/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
    if(prefix AND EXISTS "${prefix}")
        list(APPEND CMAKE_PREFIX_PATH "${prefix}")
        set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)
    endif()
endfunction()

_quackscale_prepend_pkgconfig("$ENV{OPENSSL_ROOT_DIR}")
_quackscale_prepend_pkgconfig("$ENV{OPENSSL_DIR}")
if(DEFINED ENV{VCPKG_TARGET_TRIPLET} AND NOT "$ENV{VCPKG_TARGET_TRIPLET}" STREQUAL "")
    _quackscale_prepend_pkgconfig("${CMAKE_BINARY_DIR}/vcpkg_installed/$ENV{VCPKG_TARGET_TRIPLET}")
endif()
if(VCPKG_TARGET_TRIPLET)
    _quackscale_prepend_pkgconfig("${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}")
endif()

set(_qs_wirebone_candidates "")
if(QUACKSCALE_WIREBONE_DIR)
    list(APPEND _qs_wirebone_candidates "${QUACKSCALE_WIREBONE_DIR}")
endif()
list(APPEND _qs_wirebone_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wirebone"
    "${CMAKE_CURRENT_SOURCE_DIR}/../wirebone.cpp")

set(QUACKSCALE_WIREBONE_SOURCE "")
foreach(_qs_wb_dir IN LISTS _qs_wirebone_candidates)
    if(EXISTS "${_qs_wb_dir}/CMakeLists.txt" AND EXISTS "${_qs_wb_dir}/include/wirebone/wirebone.h")
        get_filename_component(QUACKSCALE_WIREBONE_SOURCE "${_qs_wb_dir}" ABSOLUTE)
        break()
    endif()
endforeach()

if(NOT QUACKSCALE_WIREBONE_SOURCE AND NOT WIN32)
    include(FetchContent)
    FetchContent_Declare(
        wirebone_src
        GIT_REPOSITORY https://github.com/lmangani/wirebone.cpp.git
        GIT_TAG 6cf6aaabe704f51f8a4a62fc9f87d4265a8d8446
    )
    message(STATUS "QuackScale: fetching Wirebone from GitHub")
    FetchContent_GetProperties(wirebone_src)
    if(NOT wirebone_src_POPULATED)
        FetchContent_Populate(wirebone_src)
    endif()
    if(EXISTS "${wirebone_src_SOURCE_DIR}/CMakeLists.txt"
       AND EXISTS "${wirebone_src_SOURCE_DIR}/include/wirebone/wirebone.h")
        set(QUACKSCALE_WIREBONE_SOURCE "${wirebone_src_SOURCE_DIR}")
    endif()
endif()

if(NOT QUACKSCALE_WIREBONE_SOURCE)
    if(WIN32)
        message(STATUS "QuackScale: Wirebone is POSIX-only; coordinator SQL disabled on Windows")
        set(QUACKSCALE_WITH_WIREBONE OFF CACHE BOOL "Embed Wirebone coordinator" FORCE)
        return()
    endif()
    message(FATAL_ERROR
        "QuackScale: Wirebone sources are required (in-process hub). "
        "Init the submodule (git submodule update --init third_party/wirebone), "
        "check out wirebone.cpp next to this repo, set QUACKSCALE_WIREBONE_DIR, "
        "or pass -DQUACKSCALE_WITH_WIREBONE=OFF for a stub build.")
endif()

if(WIN32)
    message(STATUS "QuackScale: Wirebone is POSIX-only; coordinator SQL disabled on Windows")
    set(QUACKSCALE_WITH_WIREBONE OFF CACHE BOOL "Embed Wirebone coordinator" FORCE)
    return()
endif()

find_package(nlohmann_json CONFIG QUIET)

find_package(PkgConfig)
if(NOT PkgConfig_FOUND)
    message(FATAL_ERROR
        "QuackScale: pkg-config is required to link Wirebone (OpenSSL, nghttp2, zstd). "
        "Install pkg-config or pass -DQUACKSCALE_WITH_WIREBONE=OFF.")
endif()

pkg_check_modules(_QS_WB_CRYPTO libcrypto)
pkg_check_modules(_QS_WB_NGHTTP2 libnghttp2)
pkg_check_modules(_QS_WB_ZSTD libzstd)
if(NOT _QS_WB_CRYPTO_FOUND OR NOT _QS_WB_NGHTTP2_FOUND OR NOT _QS_WB_ZSTD_FOUND)
    message(FATAL_ERROR
        "QuackScale: Wirebone needs OpenSSL (libcrypto), nghttp2, and libzstd. "
        "Install them (or let vcpkg.json provide openssl/nghttp2/zstd), "
        "or pass -DQUACKSCALE_WITH_WIREBONE=OFF.\n"
        "  libcrypto=${_QS_WB_CRYPTO_FOUND} nghttp2=${_QS_WB_NGHTTP2_FOUND} zstd=${_QS_WB_ZSTD_FOUND}\n"
        "  PKG_CONFIG_PATH=$ENV{PKG_CONFIG_PATH}")
endif()

set(QUACKSCALE_WITH_WIREBONE ON CACHE BOOL "Embed Wirebone coordinator")
set(WIREBONE_BUILD_CLI OFF CACHE BOOL "Build the wirebone CLI" FORCE)
set(WIREBONE_BUILD_TESTS OFF CACHE BOOL "Build wirebone unit tests" FORCE)

if(NOT TARGET wirebone)
    add_subdirectory("${QUACKSCALE_WIREBONE_SOURCE}" "${CMAKE_BINARY_DIR}/third_party/wirebone")
endif()
target_compile_definitions(wirebone PRIVATE WIREBONE_DUCKDB_ZSTD=1)

message(STATUS "QuackScale: Wirebone coordinator from ${QUACKSCALE_WIREBONE_SOURCE}")

function(quackscale_link_wirebone target_name)
    target_compile_definitions(${target_name} PRIVATE QUACKSCALE_WITH_WIREBONE=1)
    target_include_directories(${target_name} PRIVATE "${QUACKSCALE_WIREBONE_SOURCE}/include")
    if(TARGET nlohmann_json)
        get_target_property(_qs_nlohmann_inc nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
        if(_qs_nlohmann_inc)
            target_include_directories(${target_name} PRIVATE ${_qs_nlohmann_inc})
        endif()
    endif()
    add_dependencies(${target_name} wirebone)
    # Archive + absolute dylibs so DuckDB's custom extension link line sees them
    # (imported CMake targets do not propagate through the generated loader).
    target_link_libraries(${target_name} "$<TARGET_FILE:wirebone>"
        ${_QS_WB_CRYPTO_LINK_LIBRARIES}
        ${_QS_WB_NGHTTP2_LINK_LIBRARIES}
        ${_QS_WB_ZSTD_LINK_LIBRARIES})
    if(UNIX AND NOT APPLE)
        target_link_libraries(${target_name} pthread)
    endif()
endfunction()
