cmake_minimum_required(VERSION 3.20)
project(teez-worker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(TEEZ_WORKER_IS_TOPLEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(TEEZ_WORKER_IS_TOPLEVEL ON)
endif()

if(TEEZ_WORKER_IS_TOPLEVEL)
    enable_testing()
endif()

include(cmake/TeezResolveCore.cmake)
include(cmake/Dependencies.cmake)
include(cmake/CoditaryResolveNet.cmake)
include(cmake/Coverage.cmake)
include(cmake/EmbedRuntime.cmake)

coditary_resolve_net()

find_package(CURL REQUIRED)

set(TEEZ_EMBEDDED_RUNTIME_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_runtime.cpp")
teez_generate_embedded_runtime("${TEEZ_EMBEDDED_RUNTIME_CPP}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime")

add_library(teez_worker_lib STATIC
    src/harness.cpp
    src/http.cpp
    src/mock_binary.cpp
    src/mock_file.cpp
    src/mock_http.cpp
    src/runner.cpp
    src/runtime_config.cpp
    "${TEEZ_EMBEDDED_RUNTIME_CPP}"
)

target_include_directories(teez_worker_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(teez_worker_lib
    PUBLIC
        teez_core
        coditary_net
        coditary_lua_net
        CURL::libcurl
        httplib::httplib
        Threads::Threads
)

target_compile_definitions(teez_worker_lib
    PUBLIC
        TEEZ_WORKER_RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
)

if(NOT TARGET teez_core)
    target_compile_definitions(teez_worker_lib
        PUBLIC
            TEEZ_PLUGIN_DIR="${TEEZ_CORE_SOURCE_DIR}/plugins"
    )
endif()

add_executable(teez-worker
    src/main.cpp
)

target_link_libraries(teez-worker
    PRIVATE
        teez_worker_lib
)

if(TEEZ_WORKER_IS_TOPLEVEL)
# --- tests ---
add_executable(teez_worker_tests
    tests/test_http.cpp
    tests/test_main_integration.cpp
    tests/test_mock_binary.cpp
    tests/test_mock_file.cpp
    tests/test_mock_http.cpp
    tests/test_runner.cpp
)

target_link_libraries(teez_worker_tests
    PRIVATE
        teez_worker_lib
        Catch2::Catch2WithMain
)

target_compile_definitions(teez_worker_tests
    PRIVATE
        TEEZ_PLUGIN_DIR="${TEEZ_CORE_SOURCE_DIR}/plugins"
        TEEZ_WORKER_BIN="$<TARGET_FILE:teez-worker>"
)

add_dependencies(teez_worker_tests teez-worker)

catch_discover_tests(teez_worker_tests)
teez_apply_gcov_prebuild_cleanup()
teez_add_coverage_target(coverage "teez-worker")

include(${TEEZ_CORE_SOURCE_DIR}/cmake/Fuzz.cmake)
teez_add_fuzz_target(fuzz_worker
    CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus"
    SOURCES
        fuzz/fuzz_worker.cpp
    LINK_LIBRARIES
        teez_worker_lib
    COMPILE_DEFINITIONS
        TEEZ_WORKER_RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime"
)
if(TARGET fuzz_worker)
    target_include_directories(fuzz_worker
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/fuzz
    )
endif()
endif()
