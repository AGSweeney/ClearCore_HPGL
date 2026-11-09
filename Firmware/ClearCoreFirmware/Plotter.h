/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#ifndef __CLEARHPGL_PLOTTER_H__
#define __CLEARHPGL_PLOTTER_H__

/*
 * Turns interpreter output into machine motion.
 *
 * Pen-down and pen-up segments are queued as actions. Runs of segments stream
 * into CoordinatedMotionController (M0 = X, M1 = Y) with junction lookahead; the
 * planner always plans to a stop at its tail, so a host that falls behind costs
 * speed, not position. A pen change waits until XY has stopped, then drives the
 * pen output (or M2) and its settle time.
 */

#include <stddef.h>
#include <stdint.h>

#include "Hpgl.h"

typedef void (*PlotterReplyFn)(const char *text);

bool PlotterInit();
/* Re-reads g_settings (speeds always; mechanics and pen hardware while disabled). */
void PlotterApplySettings();
void PlotterPoll();

HpglSink *PlotterSink();
void PlotterSetReplyHandler(PlotterReplyFn fn);
uint32_t PlotterRoom();
/* No queued actions, planner stopped, pen settled. */
bool PlotterIdle();

const char *PlotterEnable();
const char *PlotterDisable();
bool PlotterEnabled();
/* Decelerate, drop queued actions, raise the pen. */
void PlotterAbort();
void PlotterPause(bool on);
bool PlotterPaused();
/* Returns true once after an estop trip or fault that the job source must discard. */
bool PlotterTakeJobAbort();
/* Current pen position becomes plotter (0,0). Idle only. */
const char *PlotterSetOrigin();
void PlotterClearFault();
const char *PlotterFault();

void PlotterStatus(char *buf, size_t len);
/* One-line position report for the console telemetry stream ("TEL ..."). */
void PlotterTelemetry(char *buf, size_t len);
/* Step-generation diagnostics (console "diag"): large single-sample step requests and
 * steps carried over the per-sample limit. */
void PlotterDiag(void (*out)(const char *line));
/* Clear the timing probe (console "diag reset"). */
void PlotterDiagReset();

/* Limits and homing (Switches.h). Axes: 0 X, 1 Y, as HP-GL sees them. */
/* Jog an axis at mmPerS (signed) for the next 0.5 s; call again within that to keep it going.
 * stopOn: inputs (bit per code) that stop it when they leave the level they read now. After a
 * stop on one of them, the jog is refused until PlotterJogStop(). */
const char *PlotterJog(int axis, double mmPerS, uint16_t stopOn);
void PlotterJogStop();
/* Home the axes in the mask (bit 0 X, bit 1 Y, in home_order); 0 = each axis with a home switch. */
const char *PlotterHome(uint8_t axes);
void PlotterHomeStop();
/* Bit per homed axis. */
uint8_t PlotterHomed();
/* Called before the interpreter is fed: true while jobs must wait (homing, or home_required and
 * not homed). jobWaiting: a job is ready to run, which starts the homing. */
bool PlotterHoldJobs(bool jobWaiting);
/* True once after the head's position was set (homing): the interpreter resyncs. */
bool PlotterTakePositionChanged();
/* Bench simulation (console "sim"): mode 0 off, 1 low, 2 high. */
void PlotterSimLevel(int code, int mode);
/* A switch on input code fixed to the machine, mm from here on side (+1 / -1) of axis. */
void PlotterSimAt(int code, int axis, int side, double mm);
void PlotterSimClear();
/* Console "swinfo": inputs, switches, the last stops, homing. */
void PlotterSwitchInfo(void (*out)(const char *line));

#endif
