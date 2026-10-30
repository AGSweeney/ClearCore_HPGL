# ClearHPGL

Qt 6 desktop host for the ClearHPGL plotter firmware: open a DXF or HP-GL file, preview exactly
what the plotter will draw, repair DXF path gaps, and plot over Ethernet or a serial port.

`Firmware/` is ClearCore firmware that behaves like an HP-GL pen plotter (HP 7475A class plus
common HP-GL/2): send a `.plt` file over USB, an RS-232 COM port, or raw TCP 9100 and it plots.
See [Firmware/PROTOCOL.md](Firmware/PROTOCOL.md).

The app talks to the board with `src/PlotterLink` (console TCP 9300, jobs on TCP 9100, discovery
UDP 9302; or HP-GL straight down a serial port) and previews HP-GL with the firmware's own
interpreter (`Firmware/ClearCoreFirmware/Hpgl.cpp` is compiled into the app).

Extracted from `EnhancedClearCoreLibrary` (history preserved via `git subtree split`):

| Here | Source in EnhancedClearCoreLibrary |
|------|------------------------------------|
| repo root (Qt app) | `ClearHPGL/` |
| `Firmware/ClearCoreFirmware/` | forked from `ClearCNC_Controller/ClearCoreFirmware/`, now rewritten as the HP-GL plotter firmware |
| `Firmware/ClearCoreFirmware/Device_Startup/` | `Depreciated/MotionStreamingExample/Device_Startup/` (copied) |
| `libClearCore/`, `LwIP/`, `tools/` (flash + uf2 tools) | vendored copies at `292124c` |

## Layout

| Path | Contents |
|------|----------|
| `src/MainWindow.*` | Connection, plotter control, plot/jog tabs, DXF fixes |
| `src/PlotterLink.*` | Firmware protocol client (console, jobs, discovery, serial) |
| `src/HpglPreview.*` | HP-GL → strokes using the firmware interpreter |
| `src/DxfImport.*` | ASCII DXF loader |
| `src/PathGapBridge.*`, `src/PathVizIssues.*` | Path repair and issue markers |
| `src/PositionView3D.*` | 3D / plan view of the plot |
| `tests/test_plotterlink.cpp` | Headless protocol test (against `fake_plotter.py` or a board) |
| `resources/` | Qt resources and Windows app icon |
| `ProjectData/SampleDXF/` | Sample DXF files for testing |
| `tools/generate_windows_icon.py` | Regenerates the `.ico` from the SVG |
| `Firmware/ClearCoreFirmware/` | HP-GL plotter firmware (`ClearHPGL_Firmware.atsln`, Microchip Studio) |
| `Firmware/tests/` | Host tests for the portable HP-GL interpreter and channel code (no board) |
| `Firmware/host/clearhpgl_cli.py` | Discover, send a plot file, query, console commands |
| `Firmware/host/fake_plotter.py` | Network stand-in for a board (console, jobs, discovery) |
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

- **IDE:** open `Firmware/ClearCoreFirmware/ClearHPGL_Firmware.atsln` (includes the ClearCore
  and LwIP projects) and build.
- **Command line:** builds LwIP → libClearCore → firmware via the Studio-style Makefiles and
  produces `.bin` + `.uf2` in `Firmware/ClearCoreFirmware/<Config>/`. After adding or removing
  source files in the `.cppproj`, run `python Firmware/gen_makefiles.py`.

```powershell
./Firmware/build.ps1                  # Release
./Firmware/build.ps1 -Config Debug
./Firmware/build.ps1 -Flash           # build, then flash a USB-connected ClearCore
./Firmware/tests/run_tests.ps1        # host tests (MSVC via CMake), no board needed
```

## Using the plotter

```powershell
python Firmware/host/clearhpgl_cli.py discover
python Firmware/host/clearhpgl_cli.py --host <ip> console status
python Firmware/host/clearhpgl_cli.py --host <ip> send ProjectData/drawing.plt
copy /b drawing.plt COM5     # USB virtual COM port works the same way
```

First-time setup from a serial terminal on the USB port: type `menu`, press Enter, then
`config` / `set <key> <value>`. If DI-6 is not wired to an estop, `set estop_di6 0`.

## Testing without a board

```powershell
python Firmware/host/fake_plotter.py --bind 127.0.0.1      # in another terminal
./ProjectData/build/Release/test_plotterlink.exe 127.0.0.1  # or connect the app to 127.0.0.1
```
