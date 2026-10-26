# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_executable(qtfilemgr_localio_safety_test test_localio_safety.cpp)
target_link_libraries(qtfilemgr_localio_safety_test PRIVATE QtFileMgr::QtFileMgr)
add_test(NAME qtfilemgr.localio_safety
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_isolated.py"
            --binary "$<TARGET_FILE:qtfilemgr_localio_safety_test>")
set_tests_properties(qtfilemgr.localio_safety PROPERTIES TIMEOUT 300 LABELS "local;regression")

foreach(safety IN ITEMS undo_guard remote_overwrite widget_workspace_lifetime
                        undo_history directory_lifetime provider_replacement serial_transfer)
    add_executable(qtfilemgr_${safety}_test test_${safety}.cpp)
    target_link_libraries(qtfilemgr_${safety}_test PRIVATE QtFileMgr::QtFileMgr)
    add_test(NAME qtfilemgr.${safety}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_isolated.py"
                --binary "$<TARGET_FILE:qtfilemgr_${safety}_test>")
    set_tests_properties(qtfilemgr.${safety} PROPERTIES TIMEOUT 120 LABELS "safety;regression")
endforeach()

if(QTFILEMGR_WITH_SSH)
    add_executable(qtfilemgr_ssh_provider_lifetime_test test_ssh_provider_lifetime.cpp)
    target_link_libraries(qtfilemgr_ssh_provider_lifetime_test PRIVATE QtFileMgr::QtFileMgr)
    add_test(NAME qtfilemgr.ssh_provider_lifetime
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_isolated.py"
                --binary "$<TARGET_FILE:qtfilemgr_ssh_provider_lifetime_test>")
    set_tests_properties(qtfilemgr.ssh_provider_lifetime PROPERTIES
        TIMEOUT 30 LABELS "ssh;lifetime;regression")
endif()

if(QTFILEMGR_BUILD_EXAMPLES)
    add_executable(qtfilemgr_memory_io_test ../examples/memory_io/test_memory_io.cpp)
    target_link_libraries(qtfilemgr_memory_io_test PRIVATE qtfilemgr_example_memory_backend)
    add_test(NAME qtfilemgr.memory_io
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_isolated.py"
                --binary "$<TARGET_FILE:qtfilemgr_memory_io_test>")
    foreach(example IN ITEMS embedded memory_io)
        add_test(NAME qtfilemgr.example_${example}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_isolated.py"
                    --binary "$<TARGET_FILE:qtfilemgr_example_${example}>" -- --smoke-test)
        set_tests_properties(qtfilemgr.example_${example} PROPERTIES
            TIMEOUT 30 LABELS "example;integration")
    endforeach()
    set_tests_properties(qtfilemgr.memory_io PROPERTIES TIMEOUT 30 LABELS "example;backend")
endif()

option(QTFILEMGR_BUILD_VISUAL_TOOLS "Build the opt-in offscreen screenshot tool" OFF)
if(QTFILEMGR_BUILD_VISUAL_TOOLS)
    add_executable(qtfilemgr_grab_shot grab_shot.cpp)
    target_link_libraries(qtfilemgr_grab_shot PRIVATE QtFileMgr::QtFileMgr)
endif()

option(QTFILEMGR_TEST_SSH_E2E "Run isolated real SSH/SFTP integration tests" OFF)
if(QTFILEMGR_TEST_SSH_E2E)
    if(NOT QTFILEMGR_WITH_SSH)
        message(FATAL_ERROR "QTFILEMGR_TEST_SSH_E2E requires QTFILEMGR_WITH_SSH=ON")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "The isolated non-root SSH fixture currently requires Linux")
    endif()
    add_executable(qtfilemgr_ssh_e2e_test test_ssh_e2e.cpp)
    target_link_libraries(qtfilemgr_ssh_e2e_test PRIVATE QtFileMgr::QtFileMgr)
    add_test(NAME qtfilemgr.ssh_e2e
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_ssh_e2e.py"
                --run --binary "$<TARGET_FILE:qtfilemgr_ssh_e2e_test>")
    set_tests_properties(qtfilemgr.ssh_e2e PROPERTIES TIMEOUT 240 LABELS "ssh;integration")
endif()
