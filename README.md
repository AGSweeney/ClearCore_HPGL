# ClearHPGL

ClearHPGL turns a [Teknic ClearCore](https://teknic.com/products/io-motion-controller/) with two
ClearPath step-and-direction servos into an HP-GL pen plotter, and comes with a Windows desktop app
to drive it.

- **Firmware** (`Firmware/`): the board behaves like an HP 7475A-class plotter that also accepts
  the HP-GL/2 commands drivers commonly send. Anything that can write a `.plt` file to a port can
  plot: AutoCAD's HP-GL and HP-GL/2 plotter drivers, `copy /b drawing.plt COM3`, or a raw TCP
  print port. No host software is needed.
- **App** (repo root, Qt 6): opens DXF or HP-GL, previews exactly what the board will draw (it
  compiles in the firmware's own interpreter), orders DXF paths for cutting, sends jobs, and shows
  the pen live in a 3D view.

**Status: experimental.** It runs on one bench setup (ClearCore, ClearPath-SD on X and Y, pen on
an output) but is not certified for production or safety-critical use. Keep a physical E-stop in
the circuit.

## Firmware

| Area | What it does |
|------|--------------|
| Inputs | USB virtual COM port, COM-0 / COM-1 (RS-232 or TTL, off by default), raw TCP 9100 (one connection per job). Setup console on USB (`menu`) and TCP 9300; UDP 9302 discovery. One source plots at a time; the others wait. |
| HP-GL | Absolute/relative moves, `IP` `SC` `IW` `RO` `IR`, arcs and circles (`AA` `AR` `CI`), rectangles and wedges (outlines), encoded polylines (`PE`), output queries (`OI` `OH` `OA` ...), `ESC.` device control, ENQ/ACK and XON/XOFF. PCL/PJL wrappers from Windows drivers are ignored. |
| Motion | Coordinated X/Y with junction lookahead, curvature speed limits, configurable speeds and acceleration. Pen on a digital output or a third ClearPath (M2). |
| Job reordering | Jobs from USB and COM (by default) are buffered whole and plotted in a better order: touching strokes joined, everything inside a closed contour drawn before it (outlines last, as a cutter needs), nearest shape next. Up to 8000 points / 500 strokes per page; larger jobs plot as received. |
| CAD compatibility | `ident` sets the model the board reports; DraftMaster / 758x idents put plotter 0,0 at the bed centre as those plotters do. `job_speed 0` ignores pen speeds sent by the job and always draws at the board's pen-down speed. |
| Safety | DI-6 E-stop, ClearPath alert and HLFB checks: motors disable, the job is dropped, the fault latches until cleared. |
| Settings | `config` / `set <key> <value>` on the console, stored in NVM. Network settings are imported from ClearCoreROS or ClearAI firmware if found. |

Full protocol, settings and defaults: [Firmware/PROTOCOL.md](Firmware/PROTOCOL.md).

Default machine: X on M0, Y on M1, 800 steps/rev, 5 mm/rev, 400 × 300 mm bed, pen on IO-0,
E-stop on DI-6 (active low; `set estop_di6 0` if not wired).

Not implemented: `LB` text, area fills, line types and pen widths, homing / limit switches, and
multi-pen carousels (`SP` n > 1 does not pause for a pen change).

## App

- **Open** ASCII DXF (`LINE`, `LWPOLYLINE`, `POLYLINE`, `ARC`, `CIRCLE`; `$INSUNITS` honoured) or
  HP-GL files. DXF paths are joined and ordered for cutting (holes before outlines, nearest next).
  Path checks and quick fixes for open contours and gaps, with **Save DXF geometry** and
  **Export HP-GL**.
- **Preview** through the firmware's interpreter and input channel, using the board's model
  (so DraftMaster centre-origin files land where the board will put them).
- **Plot** over Ethernet or the USB/serial port, with progress, a cycle timer and the HP-GL line
  being drawn highlighted.
- **3D view** of the bed: the planned path, the pen live (about 40 updates/s), a green trace of
  everything drawn with the pen down, follow-pen and top-down modes.
- **Follow** jobs sent to the board by other programs (for example AutoCAD on the USB port) while
  connected over Ethernet.
- **Control**: enable/disable, pause, abort, set origin, jog, go to, pen up/down, and a settings
  dialog for the board's configuration.

## Plotting from AutoCAD

Two profiles work, plotting straight to the board's COM port:

- **Draftmaster 2 (7596A)** (HP-GL driver): set the board's `ident` to `7596A` so its origin is
  the bed centre. AutoCAD sends arcs as short line segments.
- **DraftMaster RX 7596B** (HP-GL/2 driver): sends real arcs (`AA`), circles and encoded
  polylines; places the plot itself, independent of `ident`.

Give either profile a custom 400 × 300 mm paper size with no margins, and in Configure Port set
flow control to XON/XOFF. The board reorders the job before plotting (`optimize`), so AutoCAD's own
path order does not matter much.

## Getting started

1. Build and flash the firmware (below), or flash a `.bin` with
   `tools\flash_clearcore.cmd Firmware\ClearCoreFirmware\Release\ClearHPGL_Firmware.bin`.
2. Open the USB port in a serial terminal, type `menu`, then `config` and `set <key> <value>` for
   your mechanics (`steps_rev_x`, `mm_rev_x`, `width_mm`, speeds, ...). DHCP is the default; a
   static address is `set net_mode 1` plus `ip`, `netmask`, `gateway`, then `restart`.
3. Plot:

```powershell
copy /b drawing.plt COM3                                        # USB or COM port, from cmd.exe
python Firmware/host/clearhpgl_cli.py discover
python Firmware/host/clearhpgl_cli.py --host <ip> send drawing.plt
python Firmware/host/clearhpgl_cli.py --host <ip> console status
```

Or connect the app (Ethernet or serial), open a drawing and press Plot.

## Building

### App (Windows)

Requirements: Visual Studio 2022 (Desktop C++), CMake ≥ 3.21, Qt 6 via vcpkg (`OpenGL`,
`OpenGLWidgets`, `Widgets`, `SerialPort`, `Network`, `Svg`).

```powershell
./build.ps1                     # Release, Visual Studio generator, runs windeployqt
./build.ps1 -Install            # ...and copy to %LOCALAPPDATA%\Programs\ClearHPGL + Start-menu shortcut
./build.ps1 -Config Debug
./build.ps1 -Generator Ninja -SkipDeploy
```

Output: `ProjectData/build/<Config>/ClearHPGL.exe`. The script finds `VsDevCmd.bat`, CMake and
`%USERPROFILE%\vcpkg`; override with `-VsDevCmd`, `-CmakeExe`, `-VcpkgToolchain`,
`-WinDeployQtExe`. Pin the installed copy (`-Install`) to the taskbar: the build folder is
rewritten on every build.

### Firmware

Requires Microchip/Atmel Studio 7 (ARM GCC toolchain and the SAME53 DFP pack).

- **IDE:** open `Firmware/ClearCoreFirmware/ClearHPGL_Firmware.atsln` (includes the libClearCore
  and LwIP projects).
- **Command line:** builds LwIP → libClearCore → firmware with the Studio makefiles and writes
  `.bin` and `.uf2` to `Firmware/ClearCoreFirmware/<Config>/`. After adding or removing sources in
  the `.cppproj`, run `python Firmware/gen_makefiles.py`.

```powershell
./Firmware/build.ps1                  # Release
./Firmware/build.ps1 -Config Debug
./Firmware/build.ps1 -Flash           # build, then flash a USB-connected ClearCore
```

### Tests (no board needed)

```powershell
./Firmware/tests/run_tests.ps1        # HP-GL interpreter, input channel, job optimizer,
                                      # libClearCore LinearInterpolator (MSVC via CMake)
python Firmware/host/fake_plotter.py --bind 127.0.0.1             # network stand-in for a board
./ProjectData/build/Release/test_plotterlink.exe 127.0.0.1        # app protocol client against it
```

`Firmware/tests/build/Release/test_hpgl.exe <file.plt> [ident]` runs any plot file through the
interpreter and job optimizer and reports strokes, paths and pen-up travel before and after.

## Layout

| Path | Contents |
|------|----------|
| `src/MainWindow.*` | Main window: connection, plotter control, plot and jog tabs, preview, DXF fixes |
| `src/PlotterLink.*` | Protocol client: console, jobs, telemetry, discovery, serial |
| `src/HpglPreview.*` | HP-GL → strokes through the firmware's channel and interpreter |
| `src/DxfImport.*` | ASCII DXF import, path joining and cut ordering, HP-GL generation |
| `src/PathGapBridge.*`, `src/PathVizIssues.*` | Path checks and gap repair |
| `src/PositionView3D.*` | 3D / top-down view of the bed, path, progress and pen trace |
| `src/PlotterSettingsDialog.*` | Board settings dialog |
| `tests/` | `test_plotterlink` (protocol client), `dxf_probe` (DXF import and cut order), `render_settings_dialog` (UI renders, `--hpgl-extent` placement check) |
| `Firmware/ClearCoreFirmware/` | Plotter firmware: `Hpgl` (interpreter), `HpglChannel` (input handling), `JobOptimizer`, `Plotter` (motion and pen), `Transport` (USB, COM, TCP, UDP), `Console`, `Settings` |
| `Firmware/tests/` | Host tests for the portable firmware code and the interpolator |
| `Firmware/host/` | `clearhpgl_cli.py` (discover, send, query, console), `fake_plotter.py` |
| `Firmware/PROTOCOL.md` | Links, HP-GL coverage, console commands and settings |
| `libClearCore/`, `LwIP/` | ClearCore library and LwIP stack used by the firmware (see below) |
| `tools/` | `bossac`, `uf2-builder`, `flash_clearcore.cmd`, icon generator |
| `ProjectData/SampleDXF/` | Sample drawings |

## Origin and the ClearCore library

ClearHPGL was split out of
[EnhancedClearCoreLibrary](https://github.com/AGSweeney/EnhancedClearCoreLibrary) with history
(`git subtree split`). The app came from its `ClearHPGL/` folder; the firmware started as a fork of
`ClearCNC_Controller/ClearCoreFirmware/` and was rewritten as the plotter firmware.

`libClearCore/` and `LwIP/` are vendored copies. Fixes made here are backported to
EnhancedClearCoreLibrary and documented in its `MODIFICATIONS.md`; the `libClearCore` sources
currently match its `main`. They include coordinated-motion step-burst and slow-move fixes, TCP
receive flow control, and USB serial receive fixes (writing to the board's USB port with
`copy /b` depends on them).
