# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

cmake_minimum_required(VERSION 3.16)
project(QtFileMgrInstalledConsumer LANGUAGES CXX)

find_package(QtFileMgr CONFIG REQUIRED)
add_executable(qtfilemgr_installed_smoke main.cpp)
target_link_libraries(qtfilemgr_installed_smoke PRIVATE QtFileMgr::QtFileMgr)
if(DEFINED QTFILEMGR_EXPECT_SSH)
    target_compile_definitions(qtfilemgr_installed_smoke PRIVATE
        QTFILEMGR_EXPECT_SSH=$<BOOL:${QTFILEMGR_EXPECT_SSH}>)
endif()
