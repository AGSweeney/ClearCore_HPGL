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

#endif
