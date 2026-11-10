@echo off
rem KonvoyTP derleme betigi (Visual Studio 2022/2026 + CMake + Ninja)
rem   build\out\KonvoyTP\KonvoyTP.dll      oyun eklentisi
rem   build\out\app\KonvoyTP_Kurulum.exe   kurulum / guncelleme uygulamasi
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
if exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" goto havevs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VS=%%i"
:havevs
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
cd /d "%~dp0"
"%CMAKE%" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA%" || exit /b 1
"%CMAKE%" --build build || exit /b 1
echo.
echo DERLEME TAMAM: build\out\KonvoyTP\KonvoyTP.dll + build\out\app\KonvoyTP_Kurulum.exe
