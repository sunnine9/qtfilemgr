# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

cmake_minimum_required(VERSION 3.16)
project(QtFileMgr VERSION 0.1.0
    DESCRIPTION "Embeddable Qt Widgets file manager by Spike"
    HOMEPAGE_URL "https://github.com/sunnine9/qtfilemgr"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(BUILD_SHARED_LIBS "Build QtFileMgr as a shared library" ON)
option(QTFILEMGR_WITH_SSH "Build the libssh SSH/SFTP backend and connection UI" ON)
option(QTFILEMGR_BUILD_APP "Build the standalone file manager application" ON)
option(QTFILEMGR_BUILD_EXAMPLES "Build the embedded host and read-only memory IO examples" OFF)
set(QTFILEMGR_QT_MAJOR "" CACHE STRING "Qt major version: empty for automatic, 5, or 6")
set_property(CACHE QTFILEMGR_QT_MAJOR PROPERTY STRINGS "" 5 6)

if(QTFILEMGR_QT_MAJOR STREQUAL "")
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
    set(QTFILEMGR_QT_MAJOR_FOUND "${QT_VERSION_MAJOR}")
elseif(QTFILEMGR_QT_MAJOR MATCHES "^[56]$")
    set(QTFILEMGR_QT_MAJOR_FOUND "${QTFILEMGR_QT_MAJOR}")
else()
    message(FATAL_ERROR "QTFILEMGR_QT_MAJOR must be empty, 5, or 6")
endif()
set(QT_VERSION_MAJOR "${QTFILEMGR_QT_MAJOR_FOUND}")
if(QTFILEMGR_QT_MAJOR_FOUND STREQUAL "5")
    set(QTFILEMGR_QT_MIN_VERSION 5.12)
else()
    set(QTFILEMGR_QT_MIN_VERSION 6.0)
endif()
find_package(Qt${QTFILEMGR_QT_MAJOR_FOUND} ${QTFILEMGR_QT_MIN_VERSION}
    REQUIRED COMPONENTS Core Gui Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/forms")

set(qtfilemgr_sources
    src/QtFileMgrPlacesPanel.cpp
    src/QtFileMgrBreadcrumbBar.cpp
    src/QtFileMgrFileView.cpp
    src/QtFileMgrWidget.cpp
    src/QtFileMgrItemDelegate.cpp
    src/QtFileMgrFlatModel.cpp
    src/QtFileMgrFilterBar.cpp
    src/QtFileMgrIO.cpp
    src/QtFileMgrLocalIO.cpp
    src/QtFileMgrFileOperationJob.cpp
    src/QtFileMgrHamburgerMenu.cpp
    src/QtFileMgrPathPicker.cpp
    src/QtFileMgrSettingsDialog.cpp
    src/QtFileMgrConfigStore.cpp
    src/QtFileMgrPropertiesDialog.cpp
    src/QtFileMgrPreviewPanel.cpp
    src/QtFileMgrSearchDialog.cpp
    src/QtFileMgrWorkspace.cpp
    src/QtFileMgrTrashDialog.cpp
    src/QtFileMgrDeviceProvider.cpp
    src/QtFileMgrVersionsDialog.cpp
    src/QtFileMgrStyle.h
    forms/QtFileMgrWidget.ui
    qtfilemgr.qrc)
file(GLOB qtfilemgr_public_headers CONFIGURE_DEPENDS "include/*.h")
if(QTFILEMGR_WITH_SSH)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh)
    list(APPEND qtfilemgr_sources
        src/QtFileMgrSSHIO.cpp
        src/QtFileMgrSSHDialog.cpp
        src/QtFileMgrProtocolProvider.cpp)
else()
    list(REMOVE_ITEM qtfilemgr_public_headers
        "${CMAKE_CURRENT_SOURCE_DIR}/include/QtFileMgrSSHIO.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/QtFileMgrSSHDialog.h")
endif()

add_library(QtFileMgr ${qtfilemgr_sources} ${qtfilemgr_public_headers})
add_library(QtFileMgr::QtFileMgr ALIAS QtFileMgr)
target_compile_features(QtFileMgr PUBLIC cxx_std_17)
target_compile_definitions(QtFileMgr PUBLIC
    QTFILEMGR_STANDALONE=1
    QTFILEMGR_WITH_SSH=$<BOOL:${QTFILEMGR_WITH_SSH}>)
target_include_directories(QtFileMgr
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/QtFileMgr>
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(QtFileMgr PUBLIC
    Qt${QTFILEMGR_QT_MAJOR_FOUND}::Core
    Qt${QTFILEMGR_QT_MAJOR_FOUND}::Gui
    Qt${QTFILEMGR_QT_MAJOR_FOUND}::Widgets)
if(QTFILEMGR_WITH_SSH)
    target_link_libraries(QtFileMgr PRIVATE PkgConfig::LIBSSH)
endif()
set_target_properties(QtFileMgr PROPERTIES
    OUTPUT_NAME qtfilemgr
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")

if(QTFILEMGR_BUILD_APP)
    add_executable(qtfilemgr_app app/main.cpp)
    target_link_libraries(qtfilemgr_app PRIVATE QtFileMgr::QtFileMgr)
    if(UNIX AND NOT APPLE)
        set_target_properties(qtfilemgr_app PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
    install(TARGETS qtfilemgr_app RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(TARGETS QtFileMgr EXPORT QtFileMgrTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES ${qtfilemgr_public_headers}
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/QtFileMgr")
set(qtfilemgr_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/QtFileMgr")
configure_package_config_file(cmake/QtFileMgrConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/QtFileMgrConfig.cmake"
    INSTALL_DESTINATION "${qtfilemgr_cmake_dir}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/QtFileMgrConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(EXPORT QtFileMgrTargets NAMESPACE QtFileMgr::
    DESTINATION "${qtfilemgr_cmake_dir}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/QtFileMgrConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/QtFileMgrConfigVersion.cmake"
    DESTINATION "${qtfilemgr_cmake_dir}")
install(FILES LICENSE NOTICE.md AUTHORS.md README.md README.zh-CN.md CHANGELOG.md
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/QtFileMgr")
install(DIRECTORY docs/
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/QtFileMgr/docs"
    FILES_MATCHING PATTERN "*.md" PATTERN "*.png")
install(DIRECTORY examples/
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/QtFileMgr/examples"
    FILES_MATCHING PATTERN "*.cpp" PATTERN "*.h" PATTERN "CMakeLists.txt" PATTERN "*.md")

if(QTFILEMGR_BUILD_EXAMPLES)
    add_subdirectory(examples/embedded)
    add_subdirectory(examples/memory_io)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
