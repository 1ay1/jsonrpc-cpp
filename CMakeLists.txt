cmake_minimum_required(VERSION 3.20)
project(jsonrpc-cpp VERSION 0.1.0 LANGUAGES CXX)

# jsonrpc-cpp: JSON-RPC 2.0 as a passive library. Header-only, C++23, no
# dependency but nlohmann/json, and no runtime of any kind (no threads,
# locks, clocks, futures or I/O). The host drives it.
#
# As a subproject (agentty adds it before mcp-cpp and acp-cpp), the parent
# supplies nlohmann_json; built on its own, it fetches it.

option(JSONRPC_BUILD_TESTS "Build jsonrpc-cpp's tests" ${PROJECT_IS_TOP_LEVEL})

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11 QUIET)
    if(NOT nlohmann_json_FOUND)
        include(FetchContent)
        FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

add_library(jsonrpc INTERFACE)
add_library(jsonrpc::jsonrpc ALIAS jsonrpc)
target_include_directories(jsonrpc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(jsonrpc INTERFACE nlohmann_json::nlohmann_json)
target_compile_features(jsonrpc INTERFACE cxx_std_23)

if(JSONRPC_BUILD_TESTS)
    enable_testing()
    # Deliberately NOT linked with Threads::Threads and built without
    # -pthread: if the library ever needs a thread, its tests fail to link.
    add_executable(jsonrpc_test tests/jsonrpc_test.cpp)
    target_link_libraries(jsonrpc_test PRIVATE jsonrpc::jsonrpc)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(jsonrpc_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME jsonrpc_test COMMAND jsonrpc_test)
endif()
