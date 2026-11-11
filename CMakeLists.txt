cmake_minimum_required(VERSION 3.16)

project(lightweight-http-server
    VERSION 1.0.0
    DESCRIPTION "Minimal HTTP/1.1 REST server built on raw sockets"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# Core library: socket layer, parser, router and response writer.
add_library(lhs_core STATIC src/HttpServer.cpp)
target_include_directories(lhs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lhs_core PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(lhs_core PUBLIC ws2_32)
    # Windows 7+ API level is required for WSAPoll and inet_ntop/inet_pton.
    target_compile_definitions(lhs_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
endif()

if(MSVC)
    target_compile_options(lhs_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(lhs_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# CLI executable with the demo REST endpoints.
add_executable(lhs_server src/main.cpp)
target_link_libraries(lhs_server PRIVATE lhs_core)

if(MSVC)
    target_compile_options(lhs_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(lhs_server PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()
