cmake_minimum_required(VERSION 3.20)

project(file_system 
    VERSION 0.1.0
    DESCRIPTION "A simple file system implementation"
    LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DDEBUG)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")
    message(STATUS "Building in Debug mode")
elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    message(STATUS "Building in Release mode")
else()
    message(FATAL_ERROR "Unsupported build type: ${CMAKE_BUILD_TYPE}")
endif()

option(ENABLE_MULTITHREAD "Enable multithread" OFF)

if (ENABLE_MULTITHREAD)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

include(FetchContent)

include_directories(${CMAKE_SOURCE_DIR}/include)

option(ENABLE_BASE_TEST "Enable base test" ON)

if (ENABLE_BASE_TEST)
    enable_testing()
    add_subdirectory(src)
    add_subdirectory(tests)
else()
    message(STATUS "Base test is disabled")
    add_subdirectory(src)
endif()


