# Embed the Wirebone coordinator (POSIX; needs OpenSSL, nghttp2, libzstd).
#
# Source lookup, first match wins:
#   1. QUACKSCALE_WIREBONE_DIR (cache/path)
#   2. ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wirebone
#   3. ${CMAKE_CURRENT_SOURCE_DIR}/../wirebone.cpp  (sibling checkout)

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

if(NOT QUACKSCALE_WIREBONE_SOURCE)
    message(STATUS "QuackScale: Wirebone sources not found; coordinator SQL disabled "
                   "(set QUACKSCALE_WIREBONE_DIR or checkout wirebone.cpp next to this repo)")
    set(QUACKSCALE_WITH_WIREBONE OFF CACHE BOOL "Embed Wirebone coordinator" FORCE)
    return()
endif()

if(WIN32)
    message(STATUS "QuackScale: Wirebone is POSIX-only; coordinator SQL disabled on Windows")
    set(QUACKSCALE_WITH_WIREBONE OFF CACHE BOOL "Embed Wirebone coordinator" FORCE)
    return()
endif()

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
    message(STATUS "QuackScale: pkg-config missing; Wirebone coordinator SQL disabled")
    set(QUACKSCALE_WITH_WIREBONE OFF CACHE BOOL "Embed Wirebone coordinator" FORCE)
    return()
endif()

pkg_check_modules(_QS_WB_CRYPTO QUIET libcrypto)
pkg_check_modules(_QS_WB_NGHTTP2 QUIET libnghttp2)
pkg_check_modules(_QS_WB_ZSTD QUIET libzstd)
if(NOT _QS_WB_CRYPTO_FOUND OR NOT _QS_WB_NGHTTP2_FOUND OR NOT _QS_WB_ZSTD_FOUND)
    message(STATUS "QuackScale: OpenSSL/nghttp2/zstd not found; Wirebone coordinator SQL disabled")
    set(QUACKSCALE_WITH_WIREBONE OFF CACHE BOOL "Embed Wirebone coordinator" FORCE)
    return()
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
