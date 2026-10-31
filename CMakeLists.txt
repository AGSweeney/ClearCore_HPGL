cmake_minimum_required(VERSION 3.21)
project(ClearHPGL LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS OpenGL OpenGLWidgets Widgets SerialPort Network Svg)

qt_add_executable(ClearHPGL WIN32
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/PositionView3D.h
    src/PositionView3D.cpp
    src/PlotterLink.h
    src/PlotterLink.cpp
    src/HpglPreview.h
    src/HpglPreview.cpp
    src/PlotterSettingsDialog.h
    src/PlotterSettingsDialog.cpp
    src/DxfImport.h
    src/DxfImport.cpp
    src/PathGapBridge.h
    src/PathGapBridge.cpp
    src/PathVizIssues.h
    src/PathVizIssues.cpp
    resources/app_resources.qrc
    # The firmware's HP-GL interpreter (portable C++), so the preview matches the plotter.
    Firmware/ClearCoreFirmware/Hpgl.h
    Firmware/ClearCoreFirmware/Hpgl.cpp
    # ...and its input channel, which strips PCL/PJL escapes and handles ESC. device control.
    Firmware/ClearCoreFirmware/HpglChannel.h
    Firmware/ClearCoreFirmware/HpglChannel.cpp
)

target_include_directories(ClearHPGL PRIVATE Firmware/ClearCoreFirmware)

target_link_libraries(ClearHPGL PRIVATE
    Qt6::OpenGLWidgets
    Qt6::OpenGL
    Qt6::Widgets
    Qt6::Svg
    Qt6::SerialPort
    Qt6::Network
)

# Windows: embed the .ico via a real .rc (Explorer + pinned taskbar; RC_ICONS alone is flaky on MSVC).
if (WIN32)
    set(_win_rc "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/clearcnc_app_icon.rc")
    if (EXISTS "${_win_rc}")
        target_sources(ClearHPGL PRIVATE "${_win_rc}")
        set_source_files_properties("${_win_rc}" PROPERTIES
            OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/clearcnc_app_icon.ico"
        )
    endif()
    target_link_libraries(ClearHPGL PRIVATE shell32)
endif()

# Headless protocol test: run Firmware/host/fake_plotter.py (or use a real board), then
# ProjectData/build/<Config>/test_plotterlink.exe [host]
option(CLEARHPGL_BUILD_TESTS "Build test_plotterlink" ON)
if (CLEARHPGL_BUILD_TESTS)
    qt_add_executable(test_plotterlink
        tests/test_plotterlink.cpp
        src/PlotterLink.h
        src/PlotterLink.cpp
        src/HpglPreview.h
        src/HpglPreview.cpp
        Firmware/ClearCoreFirmware/Hpgl.cpp
        Firmware/ClearCoreFirmware/HpglChannel.cpp
    )
    target_include_directories(test_plotterlink PRIVATE src Firmware/ClearCoreFirmware)
    target_link_libraries(test_plotterlink PRIVATE Qt6::Core Qt6::Network Qt6::SerialPort Qt6::Gui)
endif()
if (CLEARHPGL_BUILD_TESTS)
    # Visual check of the settings dialog: render_settings_dialog <config.txt> <out.png>
    qt_add_executable(render_settings_dialog
        tests/render_settings_dialog.cpp
        src/MainWindow.cpp src/MainWindow.h
        src/PlotterSettingsDialog.cpp src/PlotterSettingsDialog.h
        src/PositionView3D.cpp src/PositionView3D.h
        src/PlotterLink.cpp src/PlotterLink.h
        src/HpglPreview.cpp src/HpglPreview.h
        src/DxfImport.cpp src/DxfImport.h
        src/PathGapBridge.cpp src/PathGapBridge.h
        src/PathVizIssues.cpp src/PathVizIssues.h
        resources/app_resources.qrc
        Firmware/ClearCoreFirmware/Hpgl.cpp
        Firmware/ClearCoreFirmware/HpglChannel.cpp
    )
    target_include_directories(render_settings_dialog PRIVATE src Firmware/ClearCoreFirmware)
    target_link_libraries(render_settings_dialog PRIVATE Qt6::OpenGLWidgets Qt6::OpenGL Qt6::Widgets Qt6::Svg
                          Qt6::SerialPort Qt6::Network)
endif()
if (CLEARHPGL_BUILD_TESTS)
    # dxf_probe <file.dxf>: strips, vertices, extent and drawn length from the app's importer
    qt_add_executable(dxf_probe tests/dxf_probe.cpp src/DxfImport.cpp src/DxfImport.h)
    target_include_directories(dxf_probe PRIVATE src)
    target_link_libraries(dxf_probe PRIVATE Qt6::Core Qt6::Gui)
endif()
