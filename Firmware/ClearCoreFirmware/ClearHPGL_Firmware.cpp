/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/*
 * ClearHPGL_Firmware
 *
 * ClearCore firmware that behaves like an HP-GL pen plotter (HP 7475A class,
 * plus the HP-GL/2 commands drivers commonly emit). Send a .plt / .hpgl file and
 * it plots:
 *
 *   USB CDC          copy /b drawing.plt COMn   ("menu" + Enter opens setup)
 *   COM-0 / COM-1    RS-232 or TTL with XON/XOFF or RTS/CTS (com_port setting)
 *   TCP 9100         raw print port: one connection per job
 *   TCP 9300         setup / status console
 *   UDP 9302         "CLEARHPGL_DISCOVER?"
 *
 * M0 = X, M1 = Y (coordinated, ClearPath step/dir, HLFB 482 Hz). The pen is an
 * IO output or a ClearPath Z axis on M2. DI-6 is an optional estop.
 * See Firmware/PROTOCOL.md.
 */

#include "ClearCore.h"
#include "Plotter.h"
#include "PlotterConfig.h"
#include "Settings.h"
#include "Transport.h"

int main(void) {
    Delay_ms(300);
    TransportInitUsb();
    SettingsLoad();
    const bool motionOk = PlotterInit();
    TransportInit();
    TransportUsbSendLine(motionOk ? CHPGL_FIRMWARE_NAME " " CHPGL_FIRMWARE_VERSION " ready"
                                  : "error: motion controller init failed");
    TransportInitEthernet();
    TransportUsbSendLine("Send HP-GL, or type 'menu' and press Enter for setup.");

    while (true) {
        TransportPoll();
        PlotterPoll();
    }
}
