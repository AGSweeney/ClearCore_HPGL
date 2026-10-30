# ClearHPGL

Qt 6 desktop host for ClearCore/ClearCNC machines: DXF import, DXF→HPGL preview/export,
and path-gap / issue repair.

The desktop app talks to the ClearCore over the ClearCNC line protocol (USB serial, or
Ethernet: control TCP 8888, telemetry TCP 8889, discovery UDP 10040). The matching controller
firmware lives in `Firmware/`.

Extracted from `EnhancedClearCoreLibrary` (history preserved via `git subtree split`):

| Here | Source in EnhancedClearCoreLibrary |
|------|------------------------------------|
| repo root (Qt app) | `ClearHPGL/` |
| `Firmware/ClearCoreFirmware/` | `ClearCNC_Controller/ClearCoreFirmware/` (forked; diverges from ClearCNC from here on) |
| `Firmware/ClearCoreFirmware/Device_Startup/` | `Depreciated/MotionStreamingExample/Device_Startup/` (copied) |
| `libClearCore/`, `LwIP/`, `tools/` (flash + uf2 tools) | vendored copies at `292124c` |

## Layout

| Path | Contents |
|------|----------|
| `src/MainWindow.*` | Transport, machine, program and DXF UI |
| `src/DxfImport.*` | ASCII DXF loader |
| `src/PathGapBridge.*`, `src/PathVizIssues.*` | Path repair and issue markers |
| `src/GCodeProgramKinematics.*`, `src/PositionView3D.*` | Preview kinematics / 3D view (visualization only) |
| `resources/` | Qt resources and Windows app icon |
| `ProjectData/SampleDXF/` | Sample DXF files for testing |
| `tools/generate_windows_icon.py` | Regenerates the `.ico` from the SVG |
| `Firmware/ClearCoreFirmware/` | ClearCore controller firmware (Microchip/Atmel Studio project) |
| `libClearCore/`, `LwIP/` | Vendored ClearCore library and LwIP stack used by the firmware |
| `tools/` | `bossac`, `uf2-builder`, `flash_clearcore.cmd` |

## Building the app (Windows)

Requirements: Visual Studio 2022 (Desktop C++), CMake ≥ 3.21, and Qt 6 via vcpkg
(`OpenGL`, `OpenGLWidgets`, `Widgets`, `SerialPort`, `Network`, `Svg`).

```powershell
./build.ps1                 # Release, VS generator, runs windeployqt
./build.ps1 -Config Debug
./build.ps1 -Generator Ninja -SkipDeploy
```

Output goes to `ProjectData/build/<Config>/ClearHPGL.exe`. The script auto-detects
`VsDevCmd.bat`, CMake, and `%USERPROFILE%\vcpkg`; override with `-VsDevCmd`, `-CmakeExe`,
`-VcpkgToolchain`, `-WinDeployQtExe`.

## Building the firmware

Requires Microchip/Atmel Studio 7 (ARM GCC toolchain + SAME53 DFP pack).

- **IDE:** open `Firmware/ClearCoreFirmware/ClearCNC_Firmware.atsln` (includes the ClearCore and
  LwIP projects) and build.
- **Command line:** builds LwIP → libClearCore → firmware via the Studio-generated Makefiles and
  produces `.bin` + `.uf2` in `Firmware/ClearCoreFirmware/<Config>/`.

```powershell
./Firmware/build.ps1                  # Release
./Firmware/build.ps1 -Config Debug
./Firmware/build.ps1 -Flash           # build, then flash a USB-connected ClearCore
```
