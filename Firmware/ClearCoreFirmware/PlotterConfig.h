/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */

#ifndef __CLEARHPGL_PLOTTER_CONFIG_H__
#define __CLEARHPGL_PLOTTER_CONFIG_H__

#define CHPGL_FIRMWARE_NAME "ClearHPGL_Firmware"
#define CHPGL_FIRMWARE_VERSION "0.1.0"

#define CHPGL_USB_BAUD 115200

/* 9100 is the raw-print (JetDirect) port plotter drivers and spoolers expect.
 * Console and discovery stay clear of ClearCNC (8888/8889/10040) and ClearCoreROS (9200-9203). */
#define CHPGL_TCP_HPGL_PORT 9100
#define CHPGL_TCP_CONSOLE_PORT 9300
#define CHPGL_UDP_DISCOVERY_PORT 9302
#define CHPGL_DISCOVERY_REQUEST "CLEARHPGL_DISCOVER?"
/* Used when DHCP fails. */
#define CHPGL_FALLBACK_IP 192, 168, 0, 110

/* Receive buffers. The COM size is what ESC.L / ESC.B report on RS-232 (7475A: 1024). */
#define CHPGL_RING_USB 4096
#define CHPGL_RING_COM 1024
#define CHPGL_RING_TCP 8192

/* Pen and line actions between the interpreter and the planner. */
#define CHPGL_ACTION_QUEUE 1024
/* A serial source keeps the plotter this long after its last byte. */
#define CHPGL_JOB_IDLE_MS 2000u
/* Wait this long for more segments before starting a short run. */
#define CHPGL_START_WAIT_MS 60u
#define CHPGL_ENABLE_HLFB_WAIT_MS 1000u
#define CHPGL_WAIT_USB_MS 3000u
#define CHPGL_MAX_LINE 160
/* Bytes handed to the interpreter per main-loop pass. */
#define CHPGL_FEED_BURST 512

#endif
