@echo off
chcp 65001 >nul
title KonvoyTP kurulum
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dosyalar\kur.ps1" %*
echo.
pause
