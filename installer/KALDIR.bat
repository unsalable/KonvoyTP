@echo off
chcp 65001 >nul
title KonvoyTP kaldir
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dosyalar\kur.ps1" -Kaldir
echo.
pause
