# ClearHPGL

Qt 6 desktop host for ClearCore/ClearCNC machines: DXF import, DXF→HPGL preview/export,
and path-gap / issue repair.

App-only — it is not linked to `libClearCore`; it talks to the controller over the
ClearCNC line protocol (serial or network).

Extracted from `EnhancedClearCoreLibrary/ClearHPGL` (history preserved via `git subtree split`).

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

## Building (Windows)

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
