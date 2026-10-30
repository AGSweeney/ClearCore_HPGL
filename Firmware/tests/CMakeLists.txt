cmake_minimum_required(VERSION 3.21)
project(ClearHPGL_FirmwareTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ClearCoreFirmware")

add_executable(test_hpgl
    test_hpgl.cpp
    ${FW_DIR}/Hpgl.cpp
    ${FW_DIR}/HpglChannel.cpp
)
target_include_directories(test_hpgl PRIVATE ${FW_DIR})
if (MSVC)
    target_compile_options(test_hpgl PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_hpgl PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME test_hpgl COMMAND test_hpgl)
