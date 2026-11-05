@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem AGA-PISTORM: one-time SD card setup. Put the Amiga's SD card in this PC
rem and double-click this file. See README.txt.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0PC\SD-Setup.ps1" %*
echo.
pause
