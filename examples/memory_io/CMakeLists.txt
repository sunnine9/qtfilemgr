# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(QtFileMgrMemoryIOExample LANGUAGES CXX)
    find_package(QtFileMgr 0.1 CONFIG REQUIRED)
endif()
set(CMAKE_AUTOMOC ON)

add_library(qtfilemgr_example_memory_backend STATIC MemoryIO.cpp MemoryIO.h)
target_link_libraries(qtfilemgr_example_memory_backend PUBLIC QtFileMgr::QtFileMgr)
target_include_directories(qtfilemgr_example_memory_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(qtfilemgr_example_memory_io main.cpp)
target_link_libraries(qtfilemgr_example_memory_io PRIVATE qtfilemgr_example_memory_backend)
