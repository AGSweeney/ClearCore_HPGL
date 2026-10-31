# ClearHPGL_Firmware protocol

**Experimental.** Not certified for production or safety-critical use. Keep a physical estop in the circuit.

The board behaves like an HP-GL pen plotter (HP 7475A class) with the HP-GL/2 commands that drivers commonly emit. A host sends a `.plt` / `.hpgl` file over any of the links below and it plots. No host software is required.

Firmware `0.1.0`. Host-tested, and run on one bench setup (ClearCore, ClearPath-SD on X and Y, pen on an output).

## Links

| Link | Role |
|------|------|
| USB CDC (115200, any baud) | HP-GL stream. `menu` + Enter on its own line opens the setup console. |
| COM-0 / COM-1 | HP-GL stream, RS-232 or TTL. Off by default (`com_port`). XON/XOFF by default, or RTS/CTS. |
| TCP **9100** | HP-GL stream, raw print port. One connection = one job. |
| TCP **9300** | Setup / status console (same commands as USB `menu`). One client. |
| UDP **9302** | `CLEARHPGL_DISCOVER?` → `CLEARHPGL ClearHPGL_Firmware IP=… HPGL=9100 CONSOLE=9300 FW=… IDENT=…` |

These ports stay clear of ClearCNC (8888, 8889, 10040) and ClearCoreROS (9200–9203). Port 9100 is shared with ClearAI's range, but only one firmware runs on a board.

DHCP is the default; if it fails the address falls back to `192.168.0.110`. `set net_mode 1` plus `ip`, `netmask`, `gateway` give a static address after `restart`.

Examples:

```
copy /b drawing.plt COM5                      (Windows, USB or COM port)
nc 192.168.0.110 9100 < drawing.plt           (Linux / macOS)
python Firmware/host/clearhpgl_cli.py --host 192.168.0.110 send drawing.plt
```

### Job arbitration

One source owns the plotter at a time. It keeps it until its buffer is empty and it has either closed (TCP) or been quiet for 2 s (USB, COM). Then the next source with data takes over; other sources wait (TCP and USB by back-pressure, COM by XON/XOFF). Replies to output instructions go to the owning source. When a source finishes, an unterminated last instruction (`PU0,0` with no `;`) is ended as if `;` had arrived.

### Reordering (`optimize`)

Jobs from the sources set in `optimize` (default USB and COM: CAD drivers such as AutoCAD) are buffered whole and plotted in a better order once they end: strokes whose ends meet are joined into paths, everything a closed contour encloses is drawn before it (outlines last, so a cutter never frees a part before its interior is cut), and among the paths that are ready the nearest is drawn next, entered at its nearest vertex or end. Pen or speed changes split a job into groups that keep their order. While buffering, `status` reports `state=buffering`; plotting starts when the job ends (2 s of quiet on USB/COM, connection closed on TCP) or at a page break (`PG`). A page holds up to 8000 points and 500 strokes; a larger job is plotted as received and the rest streams through. `diag` prints the last job's strokes, paths and pen-up travel before and after. The ClearHPGL app orders its own jobs, so network jobs are not reordered by default.

### Flow control

| Link | Mechanism |
|------|-----------|
| TCP 9100 | Back-pressure: the board stops reading when its 8 KB buffer is full. |
| USB | Back-pressure: the board stops reading when its 4 KB buffer is full. |
| COM | `com_handshake` 1 = XON/XOFF (XOFF below 128 bytes free, XON above half), 2 = RTS/CTS, 0 = none. ENQ/ACK (`ESC.H` / `ESC.I`) and `ESC.B` block mode work on every link. The COM buffer is 1024 bytes, which is what `ESC.L` reports. |

## Units and coordinates

- Plotter units: 40 per mm (0.025 mm). Hard-clip area = `width_mm` × `height_mm`, origin at plotter (0,0).
- Plotter (0,0) is where the pen was at power-up, or wherever `origin` was run. There is no homing yet.
- Pen-up moves outside the hard clip are clamped to it. Pen-down lines are clipped to `IW` ∩ hard clip; the pen lifts where a line leaves the window and drops where it comes back.
- `RO 90/180/270` rotates the coordinate frame and swaps the logical width and height (`OH` reports the rotated size).
- Arcs and circles are drawn as chords: 5° by default, set by the chord parameter of `AA`/`AR`/`CI`/`EW`/`WG`, or as a deviation after `CT1`. The minimum is 0.5°.

## HP-GL instructions

Mnemonics are case-insensitive. Parameters are separated by commas or spaces. An instruction ends with `;` or with the next mnemonic (`PA10,10PD20,20`). Decimals and signs are accepted everywhere.

| Group | Instructions | Notes |
|-------|--------------|-------|
| Setup | `IN` `DF` `IP` `IR` `SC` (types 0, 1, 2) `IW` `RO` | `IN` raises the pen, resets P1/P2 to the hard clip, clears errors and sets `OS` bit 8. |
| Pen / motion | `PU` `PD` `PA` `PR` `PE` | `PE` (HP-GL/2 polyline encoded): base-64 and 7-bit (`7`), fractional bits (`>`), absolute (`=`), pen-up (`<`) and pen select (`:`) flags. |
| Pen / speed | `SP` `VS` | Single pen. `SP0` raises the pen. `VS` is in cm/s and is clamped to `max_mm_s`; `VS` alone restores `draw_mm_s`. |
| Arcs / shapes | `AA` `AR` `CI` `CT` `EA` `ER` `EW` `RA` `RR` `WG` `XT` `YT` `TL` | `CI` and `EW` return to the centre with the pen in its previous state. **`RA`, `RR` and `WG` draw outlines; there is no fill.** |
| Page | `PG` `NR` `AF` `AH` `FR` | Raise the pen and count a page. |
| Output | `OI` `OS` `OE` `OA` `OC` `OP` `OH` `OW` `OF` `OO` `OT` `OD` `OG` `OL` | `OI` replies with `ident` (default `7475A`). `OS` clears bits 8 and 2 after reading; `OE` clears the error. |
| Text | `LB` `DT` | **Labels are not drawn yet.** Text is consumed up to the terminator and counted (`labels_skipped` in `status`). |
| Accepted, no effect | `LT` `PT` `FT` `LA` `PW` `WU` `NP` `PC` `BP` `PS` `QL` `RP` `MC` `CO` `MG` `SI` `SR` `DI` `DR` `CS` `CA` `SS` `SA` `ES` `LO` `CP` `SM` … | Line types are solid. Font and label setup are ignored. Quoted strings (`BP1,"name"`, `CO"…"`) are skipped. |

Anything else sets error 1 (`OE`) and status bit 32 (`OS`), and is skipped. `@PJL …` lines and PCL/PJL escapes (`ESC%-12345X`, `ESC%#A`/`ESC%#B`, `ESC E`, `ESC&…`, `ESC*…`) are dropped, so HP-GL/2 output from Windows drivers plots.

### Errors (`OE`)

| Code | Meaning |
|------|---------|
| 0 | none |
| 1 | unrecognised instruction |
| 2 | wrong number of parameters |
| 3 | bad parameter (e.g. `RO45`, `SC` with zero range) |
| 6 | position overflow |

### Status (`OS`)

| Bit | Value | Meaning |
|-----|-------|---------|
| 0 | 1 | pen down |
| 1 | 2 | P1/P2 changed (cleared by `OS` or `OP`) |
| 3 | 8 | initialised (set by `IN`, cleared by `OS`) |
| 4 | 16 | ready |
| 5 | 32 | error (cleared by `OE`) |

Power-up `OS` is `24`.

## RS-232 device control (`ESC .`)

These are answered as soon as they arrive, ahead of the buffer, the way a 7475A answers them. They work on every link. The default reply terminator is CR; `ESC.M` changes it.

| Sequence | Effect / reply |
|----------|----------------|
| `ESC.(` `ESC.Y` | plotter on |
| `ESC.)` `ESC.Z` | plotter off: data is discarded until on |
| `ESC.B` | free buffer bytes |
| `ESC.L` | buffer size |
| `ESC.O` | `8` buffer empty, else `0` |
| `ESC.E` | extended error (11 bad byte after `ESC.`, 12 bad parameter, 14 too many parameters, 16 buffer overflow), then cleared |
| `ESC.K` | abort graphics: drop buffered data, stop motion, raise the pen |
| `ESC.J` | abort device control (cancel a pending ACK) |
| `ESC.R` | reset handshake and output settings |
| `ESC.H` / `ESC.I` `threshold;enq;ack…:` | ENQ/ACK: on `enq`, reply `ack` once `threshold` bytes are free |
| `ESC.N` `delay;xoff:` | enable XON/XOFF with that XOFF character |
| `ESC.M` `delay;trigger;echo;term1;term2;init:` | output terminator (`term1`, optional `term2`) |
| `ESC.@` `ESC.S` `ESC.T` `ESC.U` `ESC.P` | accepted, no effect |

## Machine

| Item | Default |
|------|---------|
| X / Y | M0 / M1, ClearPath step and direction, HLFB bipolar PWM 482 Hz, coordinated by `CoordinatedMotionController` with junction lookahead |
| Mechanics | 800 steps/rev, 5 mm/rev (160 steps/mm), 400 × 300 mm |
| Speeds | draw 100 mm/s, travel 200 mm/s, `VS` cap 250 mm/s, accel 2000 mm/s², corner Δv 20 mm/s |
| Pen, `pen_mode 0` | IO-0 output, on = down, 150 ms down / 100 ms up settle |
| Pen, `pen_mode 1` | ClearPath on M2: `z_up_steps` 0, `z_down_steps` 800. M2 must be at pen-up at power-up. |
| Estop | DI-6 active-low (`estop_di6 1`). Set `estop_di6 0` if it is not wired. |
| Enable | Automatic when a job arrives (`auto_enable 1`), with an HLFB check (`hlfb_check 1`) |

Pen changes happen only with XY stopped. Segments between pen changes stream through the planner with lookahead. The planner always plans to a stop at its last queued segment, so a host that falls behind costs speed, not position. Path speed and acceleration use the X scale, so keep X and Y at the same steps/mm.

**Faults:**
- **DI-6 trip, motor alert or HLFB failure:** the motors disable and the rest of the job is dropped. A TCP job is closed; a serial job is discarded until its source is quiet for 2 s. The fault latches until `clear` or `enable`.
- **`pause`:** holds at the end of the queued segments and raises the pen. `resume` lowers it and continues.

## Console (USB `menu`, TCP 9300)

Every command ends with a line starting `ok` or `error:`.

| Command | Effect |
|---------|--------|
| `status` | `state=idle\|buffering\|plotting\|paused\|disabled\|waiting_enable\|fault`, position in mm, pen, queue depth, counters, link stats |
| `config` | every setting with its current value |
| `set <key> <value>` | validate, apply, save to NVM. `[disabled]` keys need motors disabled; `[restart]` keys apply after `restart`. |
| `defaults` | factory settings (motors disabled) |
| `enable` / `disable` | motor enable |
| `abort` | stop, drop the current job, raise the pen |
| `pause` / `resume` | see above |
| `origin` | current position becomes plotter 0,0 |
| `goto <x_mm> <y_mm>` | pen-up move (no job running) |
| `pen up` / `pen down` | pen test (no job running) |
| `clear` | clear a latched fault |
| `tel <ms>` | TCP console only: push `TEL ms= x= y= pen= state= queue=` every `<ms>` (min 10, `0` off). Sent only when the TCP send buffer has room, so a slow client cannot stall motion. The app uses 25 ms. |
| `restart` | reboot |
| `version`, `help`, `exit` | |

### Settings

Stored in the NVM user page with magic `CHPG`, so blobs left by ClearAI (`CAIC`) or ClearCoreROS (`CROS`) are ignored. NVM writes need a supply above the ClearCore undervoltage lockout.

| Key | Default | Applies |
|-----|---------|---------|
| `steps_rev_x`, `steps_rev_y` | 800 | disabled |
| `mm_rev_x`, `mm_rev_y` | 5.0 | disabled |
| `dir_x`, `dir_y` | 1 (or -1) | disabled |
| `width_mm`, `height_mm` | 400, 300 | disabled |
| `draw_mm_s`, `travel_mm_s`, `max_mm_s` | 100, 200, 250 | live |
| `accel_mm_s2`, `corner_mm_s` | 2000, 20 | live |
| `pen_mode` | 0 (IO) / 1 (Z on M2) | disabled |
| `pen_io`, `pen_active_high` | 0, 1 | disabled |
| `pen_down_ms`, `pen_up_ms` | 150, 100 | live |
| `z_up_steps`, `z_down_steps`, `z_vel_steps`, `z_accel_steps` | 0, 800, 20000, 400000 | disabled |
| `estop_di6`, `auto_enable`, `hlfb_check` | 1, 1, 1 | live |
| `com_port`, `com_baud`, `com_handshake`, `com_rs232` | 0, 9600, 1, 1 | restart |
| `net_mode`, `ip`, `netmask`, `gateway` | DHCP | restart |
| `ident` | `7475A` | live. `758x` / `759x` (DraftMaster, e.g. AutoCAD's "Draftmaster 2 (7596A)" driver): plotter 0,0 at the bed centre, `OH` reports `-w/2,-h/2,w/2,h/2` |
| `optimize` | 3 (USB + COM) | live. Sources to buffer and reorder: 1 USB, 2 COM, 4 network |
| `job_speed` | 1 | live. 1: a job's `VS` sets the pen-down speed (cm/s, capped at `max_mm_s`). 0: ignore `VS`, always `draw_mm_s` |

## Not implemented yet

- `LB` text (needs a stroke font), fills for `RA` / `RR` / `WG` / `FP`, line types (`LT`), `PW` pen width.
- Homing to limit switches; soft limits beyond the hard clip.
- Multi-pen carousel (`SP` n > 1 is accepted but does not pause for a pen change).
