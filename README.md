# ClearHPGL

**Build your own HP-GL plotter from industrial motion parts, and plot to it from the CAD software
you already use.**

HP-GL is the language HP's pen plotters spoke, and it never went away: AutoCAD, CorelDRAW, sign and
vinyl software and countless CAM tools can still "print" a drawing as HP-GL. ClearHPGL is firmware
that makes a [Teknic ClearCore](https://teknic.com/products/io-motion-controller/) controller with
two ClearPath servos understand that language and behave like a real plotter. Point a plotter
driver at its USB port, serial port or network address and it draws: no plugin, no converter, no
special host software.

Alongside the firmware is **ClearHPGL**, a Windows desktop app for working with the machine: open a
DXF or HP-GL file, see exactly what will be drawn, send it, and watch the pen move.

The firmware drives two axes and a pen lift (a digital output or a third ClearPath), so the
natural build is a pen plotter of whatever size your mechanics allow; any machine that raises and
lowers a tool along 2D paths, such as a marking or scoring gantry, can be driven the same way.
(Tangential knife offsets for drag-knife cutting are not implemented.)

> **Status: experimental.** ClearHPGL runs on a bench machine (ClearCore, ClearPath-SD servos on X
> and Y, pen on an output) and plots real jobs, including direct from AutoCAD. It is not certified
> for production or safety-critical use. Always keep a physical E-stop in the motor power circuit.

## How it works

```
 AutoCAD / CAM / sign software            ClearHPGL app
 (HP-GL or HP-GL/2 plotter driver)        (DXF or .plt, preview, monitor)
              │                                     │
              └──── USB · RS-232 · TCP 9100 ────────┘
                               │
                   ┌───────────▼───────────────────────────────┐
                   │ ClearCore (ClearHPGL firmware)            │
                   │  input channel   flow control, ESC. codes,│
                   │                  driver wrappers stripped │
                   │  HP-GL interpreter  scaling, rotation,    │
                   │                     windows, arcs, PE ... │
                   │  job optimizer   joins strokes, cuts      │
                   │                  outlines last, shortest  │
                   │                  travel                   │
                   │  motion planner  coordinated X/Y,         │
                   │                  lookahead, accel limits  │
                   └───────────┬───────────────────────────────┘
                               │ step / direction / enable, HLFB
                    ClearPath X · ClearPath Y · pen or Z
```

1. **Any source can send a job.** The board listens on its USB virtual COM port, an optional
   RS-232/TTL serial port, and a raw TCP print port (9100), and handles the flow-control and
   device-control conventions real plotters used, so drivers that expect a plotter get one. One
   source plots at a time; the others wait their turn.
2. **The interpreter turns HP-GL into geometry** the way an HP plotter would: scaling and
   rotation, clipping windows, arcs and circles chorded smoothly, HP-GL/2 encoded polylines. It
   reports itself as a configurable model, so drivers for a 7475A or a DraftMaster both work (the
   DraftMaster's centred origin included).
3. **The job optimizer fixes the drawing order.** CAD drivers send paths in whatever order the
   drawing was made, often splitting one outline into many pieces. For jobs from CAD (USB and
   serial by default) the board buffers the whole job, joins touching strokes, draws everything
   inside a contour before the contour itself (essential for cutting) and always moves to the
   nearest next shape. On the AutoCAD parts tested it cut pen-up travel by 45-65%.
4. **The motion planner drives the servos** as one coordinated machine, looking ahead across
   segments so curves and polylines flow at speed instead of stopping at every vertex.
5. **Safety stays in the loop.** An E-stop input, ClearPath alerts and the servos' HLFB "ready"
   signal are checked continuously; a fault disables the motors, drops the job and latches until
   cleared.

## The desktop app

ClearHPGL (Qt 6, Windows) is optional but makes the machine pleasant to use:

- **Open** a DXF or an HP-GL file. DXF drawings are converted to HP-GL with the same cut ordering
  the firmware uses, and checked for open contours and small gaps, with one-click fixes.
- **Preview** with the firmware's own interpreter compiled into the app, so what you see is what
  the board will draw: position on the bed, rotation, scaling and clipping included.
- **Plot** over Ethernet or the serial port and follow it: a 3D view of the bed shows the pen live
  and paints everything drawn in green, the HP-GL line being drawn is highlighted, and a cycle
  timer records how long the run took. Jobs sent by other programs (AutoCAD on the USB port, say)
  are followed the same way.
- **Control and configure** the machine: enable, pause, abort, jog, set origin, pen up/down, and
  edit the board's settings.

## What you need

- A Teknic ClearCore controller.
- Two ClearPath servos in step-and-direction mode (ClearPath-SD, or MC in step/dir) on connectors
  M0 (X) and M1 (Y), with HLFB.
- A pen or tool lift: a solenoid or relay on a digital output (IO-0 by default), or a third
  ClearPath on M2.
- An E-stop wired to DI-6 (or set `estop_di6 0` if you have none on the board).
- To build: Microchip/Atmel Studio 7 for the firmware; Visual Studio 2022, CMake and Qt 6 (vcpkg)
  for the app.

## Getting started

1. **Flash the firmware** from a USB-connected ClearCore:

   ```powershell
   ./Firmware/build.ps1 -Flash
   ```

2. **Set up the machine** from a serial terminal on the USB port: type `menu`, then `config` to
   see the settings and `set <key> <value>` to change them. The essentials are steps per
   revolution and travel per revolution for each axis, the bed size, and speeds. The board uses
   DHCP by default; `set net_mode 1` with `ip`, `netmask` and `gateway`, then `restart`, gives it a
   static address.

3. **Plot.** Any of these work:

   ```powershell
   copy /b drawing.plt COM3                                       # USB or serial port (cmd.exe)
   python Firmware/host/clearhpgl_cli.py discover                 # find boards on the network
   python Firmware/host/clearhpgl_cli.py --host <ip> send drawing.plt
   ```

   or open the drawing in the ClearHPGL app and press **Plot**.

### Plotting straight from AutoCAD

Add a plotter with the **HP DraftMaster RX 7596B** (HP-GL/2) or **Draftmaster 2 (7596A)** (HP-GL)
model, point it at the board's COM port, give it a custom paper size matching your bed with no
margins, and set flow control to XON/XOFF. For the Draftmaster 2 driver, also `set ident 7596A`
on the board so it uses the DraftMaster's centred origin. The HP-GL/2 driver sends true arcs and
compact polylines and is the better choice. Because the board reorders every job, AutoCAD's own
path order does not matter, and `set job_speed 0` lets the board's pen speed setting win over the
speed AutoCAD sends.

## Documentation

- [Firmware/PROTOCOL.md](Firmware/PROTOCOL.md): connections and ports, supported HP-GL and
  HP-GL/2, device control, console commands, every setting with its default, and faults.
- `Firmware/host/clearhpgl_cli.py --help`: command-line client (discover, send, query, console).

Not supported yet: text (`LB`), area fills, line types and pen widths, homing and limit switches,
and automatic pen changes.

## Building and testing

**App:**

```powershell
./build.ps1             # Release build, deployed with its Qt runtime
./build.ps1 -Install    # also install to %LOCALAPPDATA%\Programs\ClearHPGL with a Start-menu shortcut
```

**Firmware:** `./Firmware/build.ps1` (or open `Firmware/ClearCoreFirmware/ClearHPGL_Firmware.atsln`
in Microchip Studio) builds LwIP, libClearCore and the firmware and writes `.bin` and `.uf2` files.

**Tests, no hardware needed:**

```powershell
./Firmware/tests/run_tests.ps1                         # interpreter, input channel, job optimizer, interpolator
python Firmware/host/fake_plotter.py --bind 127.0.0.1  # a network stand-in for a board, for the app
```

## Repository layout

| Path | What it is |
|------|------------|
| `Firmware/ClearCoreFirmware/` | The plotter firmware: HP-GL interpreter, input channels, job optimizer, motion and pen control, network and USB transport, console and settings |
| `Firmware/tests/`, `Firmware/host/` | Host tests; command-line client and fake board |
| `src/`, `resources/` | The desktop app |
| `tests/` | App-side test tools |
| `libClearCore/`, `LwIP/` | ClearCore library and TCP/IP stack the firmware builds on |
| `tools/` | Flashing and UF2 tools |
| `ProjectData/SampleDXF/` | Sample drawings |

## Background

ClearHPGL grew out of [EnhancedClearCoreLibrary](https://github.com/AGSweeney/EnhancedClearCoreLibrary),
which extends Teknic's ClearCore library with coordinated two-axis motion and unit conversion, and
was split into its own repository with its history. The `libClearCore` copy here is kept in step
with that library: problems found while building the plotter, such as step bursts on small arcs,
stalled TCP and USB transfers and faults on slow moves, were fixed here and backported there (see
its `MODIFICATIONS.md`).
