# Build libtailscale (Go c-archive) when QUACKSCALE_WITH_TAILSCALE is enabled.

if(NOT QUACKSCALE_WITH_TAILSCALE)
    return()
endif()

find_program(GO_EXECUTABLE go)
if(NOT GO_EXECUTABLE)
    message(FATAL_ERROR "QUACKSCALE_WITH_TAILSCALE=ON but Go (go) was not found in PATH")
endif()

set(LIBTAILSCALE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libtailscale")
if(NOT EXISTS "${LIBTAILSCALE_SOURCE_DIR}/tailscale.go")
    message(FATAL_ERROR "libtailscale sources not found at ${LIBTAILSCALE_SOURCE_DIR}. "
                        "Run: git submodule update --init --recursive")
endif()

set(LIBTAILSCALE_BUILD_DIR "${CMAKE_BINARY_DIR}/third_party/libtailscale")
set(LIBTAILSCALE_ARCHIVE "${LIBTAILSCALE_BUILD_DIR}/libtailscale.a")

file(MAKE_DIRECTORY "${LIBTAILSCALE_BUILD_DIR}")

set(_libtailscale_go_env "CGO_ENABLED=1")
if(APPLE)
    set(_libtailscale_go_env "${_libtailscale_go_env}" "MACOSX_DEPLOYMENT_TARGET=11.0")
endif()

add_custom_command(
    OUTPUT "${LIBTAILSCALE_ARCHIVE}"
    COMMAND ${CMAKE_COMMAND} -E env ${_libtailscale_go_env}
            ${GO_EXECUTABLE} build -buildmode=c-archive -o "${LIBTAILSCALE_ARCHIVE}"
    WORKING_DIRECTORY "${LIBTAILSCALE_SOURCE_DIR}"
    DEPENDS
        "${LIBTAILSCALE_SOURCE_DIR}/tailscale.go"
        "${LIBTAILSCALE_SOURCE_DIR}/tailscale.c"
        "${LIBTAILSCALE_SOURCE_DIR}/go.mod"
    COMMENT "Building libtailscale.a with Go"
    VERBATIM
)

add_custom_target(libtailscale_archive DEPENDS "${LIBTAILSCALE_ARCHIVE}")

set(QUACKSCALE_LIBTAILSCALE_ARCHIVE "${LIBTAILSCALE_ARCHIVE}")
set(QUACKSCALE_LIBTAILSCALE_INCLUDE "${LIBTAILSCALE_SOURCE_DIR}")
