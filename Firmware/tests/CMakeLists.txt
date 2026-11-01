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

enable_testing()
add_test(NAME test_hpgl COMMAND test_hpgl)
add_test(NAME test_interp COMMAND test_interp)
add_test(NAME test_arc COMMAND test_arc)
