@echo off
rem KonvoyTP offline logic tests (no game needed)
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++20 /utf-8 /EHa /W3 /O1 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /D_AMD64_ /I..\src /I..\SPF_API test_logic.cpp /Fo:out\ /Fe:out\test_logic.exe advapi32.lib user32.lib >out\build.log 2>&1
if errorlevel 1 (type out\build.log & exit /b 1)
out\test_logic.exe
