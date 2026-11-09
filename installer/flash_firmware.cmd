@echo off
rem Flash ClearHPGL_Firmware.bin (next to this file) to a ClearCore on USB.
rem The plotter settings are kept: they are in the board's NVM, which flashing does not erase.
rem Plotter > Update board firmware in the app does the same and checks the version afterwards.
echo ClearHPGL: flash the ClearCore firmware over USB.
echo Connect the ClearCore by USB and close the ClearHPGL app (it holds the port).
echo The plotter settings stay on the board.
echo.
call "%~dp0flash_clearcore.cmd" "%~dp0ClearHPGL_Firmware.bin"
echo.
pause
