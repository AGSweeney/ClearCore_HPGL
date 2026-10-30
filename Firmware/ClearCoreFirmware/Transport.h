/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#ifndef __CLEARHPGL_TRANSPORT_H__
#define __CLEARHPGL_TRANSPORT_H__

/*
 * Host links and job arbitration.
 *
 *   USB CDC          HP-GL stream, or the setup console after "menu" + Enter
 *   COM-0 / COM-1    HP-GL stream (RS-232 or TTL), optional; com_port setting
 *   TCP 9100         HP-GL stream, raw print port, one connection = one job
 *   TCP 9300         setup / status console, one client
 *   UDP 9302         "CLEARHPGL_DISCOVER?"
 *
 * One source owns the plotter at a time. It keeps it until its buffer is empty
 * and it has gone quiet (serial) or closed (TCP); then the next source with
 * data takes over. Replies to output instructions go to the owning source.
 */

#include <stddef.h>
#include <stdint.h>

#include "Hpgl.h"

void TransportInitUsb();
void TransportInit();
void TransportInitEthernet();
void TransportPoll();

/* Push g_settings into the interpreter, plotter and serial channel. */
void TransportApplySettings();
HpglInterpreter &TransportInterpreter();

/* Console actions. Each returns nullptr on success or an error message. */
void TransportAbortJob();
const char *TransportGoto(double xMm, double yMm);
const char *TransportPen(bool down);
const char *TransportOrigin();
bool TransportJobActive();
void TransportStats(char *buf, size_t len);
const char *TransportIpString();
void TransportRestart();

void TransportUsbSendLine(const char *line);

#endif
