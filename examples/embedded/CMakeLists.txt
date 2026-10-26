# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(QtFileMgrEmbeddedExample LANGUAGES CXX)
    find_package(QtFileMgr 0.1 CONFIG REQUIRED)
endif()

add_executable(qtfilemgr_example_embedded main.cpp)
target_link_libraries(qtfilemgr_example_embedded PRIVATE QtFileMgr::QtFileMgr)
