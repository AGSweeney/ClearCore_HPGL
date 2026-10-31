# Third-party notices

ClearHPGL's own source code is released under the [MIT License](LICENSE), Copyright (c) 2026
Adam G. Sweeney. This repository also contains code and tools by others, which remain under their
own copyrights and licences. Changes made to them in this project stay under the licence of the
file they were made in.

| Component | Path | Copyright | Licence |
|-----------|------|-----------|---------|
| ClearCore library | `libClearCore/` (files with a Teknic header) | Teknic, Inc. | MIT: [`libClearCore/license.txt`](libClearCore/license.txt) |
| ClearCore library additions | `libClearCore/` (files with an Adam G. Sweeney header, e.g. `CoordinatedMotionController`, `LinearInterpolator`) | Adam G. Sweeney | MIT |
| Microchip Advanced Software Framework (ASF) | `libClearCore/hal/`, `hpl/`, `hri/`, `usb/` | Microchip Technology Inc. and its subsidiaries | Microchip ASF licence, in each file's header (use with Microchip products) |
| Microchip device startup and linker scripts | `Firmware/ClearCoreFirmware/Device_Startup/` | Microchip Technology Inc. | Apache-2.0 (`SPDX-License-Identifier` in each file) |
| lwIP TCP/IP stack | `LwIP/` | Swedish Institute of Computer Science and contributors | BSD-style, in each file's header |
| ClearCore flash scripts | `tools/flash_clearcore.cmd`, `tools/flash_clearcore_loop.cmd` | Teknic, Inc. (authors Zachary Lebold, Nick Dawson) | Distributed with Teknic's ClearCore library (MIT) |
| BOSSA flash utility | `tools/bossac/bossac.exe` | BOSSA authors, <https://github.com/shumatech/BOSSA> | Upstream licence applies |
| UF2 conversion tool | `tools/uf2-builder/` | Distributed with Teknic's ClearCore library; UF2 format by Microsoft, <https://github.com/microsoft/uf2> | Upstream licence applies |

## Changes made here to third-party files

The ClearCore library modifications and additions are documented in
[EnhancedClearCoreLibrary](https://github.com/AGSweeney/EnhancedClearCoreLibrary)'s
`MODIFICATIONS.md` and `NEW_FILES.md`, and the `libClearCore/` copy here matches it. They include
changes to Teknic files (USB serial, TCP, motor driver) and one change to a Microchip ASF file
(`libClearCore/hpl/usb/hpl_usb.c`, endpoint direction), which remains under Microchip's licence.
