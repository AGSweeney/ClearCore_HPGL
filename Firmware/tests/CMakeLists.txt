# MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
cmake_minimum_required(VERSION 3.21)
project(ClearHPGL_FirmwareTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ClearCoreFirmware")

add_executable(test_hpgl
    test_hpgl.cpp
    ${FW_DIR}/Hpgl.cpp
    ${FW_DIR}/HpglChannel.cpp
    ${FW_DIR}/JobOptimizer.cpp
)
target_include_directories(test_hpgl PRIVATE ${FW_DIR})
if (MSVC)
    target_compile_options(test_hpgl PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_hpgl PRIVATE -Wall -Wextra)
endif()

# libClearCore's LinearInterpolator (no ClearCore dependencies)
add_executable(test_interp
    test_interp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libClearCore/src/LinearInterpolator.cpp
)
target_include_directories(test_interp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../libClearCore/inc)

# libClearCore's ArcInterpolator: exact end points, speed, no bursts (same test as
# libClearCore/tests/host, built here so run_tests.ps1 covers it)
set(LCC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../libClearCore")
add_executable(test_arc
    ${LCC_DIR}/tests/host/arc_interpolator_test.cpp
    ${LCC_DIR}/src/ArcInterpolator.cpp
    ${LCC_DIR}/src/TrigLUT.cpp
)
target_include_directories(test_arc PRIVATE ${LCC_DIR}/tests/host/stub ${LCC_DIR}/inc ${LCC_DIR}/src)
if (MSVC)
    target_compile_definitions(test_arc PRIVATE _USE_MATH_DEFINES)
endif()

# libClearCore's CoreXY kinematics (CoordinatedKinematics.h) through the interpolators
add_executable(test_corexy
    ${LCC_DIR}/tests/host/corexy_kinematics_test.cpp
    ${LCC_DIR}/src/ArcInterpolator.cpp
    ${LCC_DIR}/src/LinearInterpolator.cpp
    ${LCC_DIR}/src/TrigLUT.cpp
)
target_include_directories(test_corexy PRIVATE ${LCC_DIR}/tests/host/stub ${LCC_DIR}/inc ${LCC_DIR}/src)
if (MSVC)
    target_compile_definitions(test_corexy PRIVATE _USE_MATH_DEFINES)
endif()

# Limit and home switches and the homing sequence (Switches.cpp), against simulated motion
add_executable(test_switches
    test_switches.cpp
    ${FW_DIR}/Switches.cpp
)
target_include_directories(test_switches PRIVATE ${FW_DIR})
if (MSVC)
    target_compile_options(test_switches PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()
add_test(NAME test_hpgl COMMAND test_hpgl)
add_test(NAME test_interp COMMAND test_interp)
add_test(NAME test_arc COMMAND test_arc)
add_test(NAME test_corexy COMMAND test_corexy)
add_test(NAME test_switches COMMAND test_switches)
