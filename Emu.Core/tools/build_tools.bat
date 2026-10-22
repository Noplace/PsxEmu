@echo off
REM Builds Emu.Core's own headless harnesses - the ones that test nothing but Emu.Core - into
REM Temp\tools under this library's repository root, or into the folder given (relative to where
REM it is run from). Run from anywhere. PSXEmu.Core\tools\build_tools.bat calls this first, with
REM its own Temp\tools, so it still builds every harness.
REM
REM Independent of the solution, as PSXEmu's is: each harness compiles the sources it tests.
setlocal

set OUT=Temp\tools
if not "%~1"=="" set OUT=%~f1

if defined VCINSTALLDIR goto have_vcvars
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo Could not find vcvars64.bat at:
  echo   %VCVARS%
  echo Edit build_tools.bat if your Visual Studio install differs.
  exit /b 1
)
call "%VCVARS%" >nul
:have_vcvars

cd /d "%~dp0..\.."

set FLAGS=/nologo /std:c++20 /permissive- /EHsc /O2 /MD /DNDEBUG /D_CONSOLE ^
 /D_CRT_SECURE_NO_WARNINGS /I Emu.Core
set LIBS=/link /SUBSYSTEM:CONSOLE user32.lib psapi.lib

rem The frame limiter's rate and spacing, and matching the display: emucore\timing.
if not exist %OUT%\obj_framelimit mkdir %OUT%\obj_framelimit
cl %FLAGS% /Fo:%OUT%\obj_framelimit\ /Fe:%OUT%\frame_limiter_test.exe ^
   Emu.Core\tools\frame_limiter_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem The audio arithmetic behind 50-300% speed: emucore\timing\speed_resampler.h.
if not exist %OUT%\obj_speedres mkdir %OUT%\obj_speedres
cl %FLAGS% /Fo:%OUT%\obj_speedres\ /Fe:%OUT%\speed_resampler_test.exe ^
   Emu.Core\tools\speed_resampler_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem What a host mouse's movement is worth to a console's: emucore\input\mouse_scaling.h.
if not exist %OUT%\obj_mousescale mkdir %OUT%\obj_mousescale
cl %FLAGS% /Fo:%OUT%\obj_mousescale\ /Fe:%OUT%\mouse_scaling_test.exe ^
   Emu.Core\tools\mouse_scaling_test.cpp %LIBS%
if errorlevel 1 exit /b 1

echo Built %OUT%\frame_limiter_test.exe
echo Built %OUT%\speed_resampler_test.exe
echo Built %OUT%\mouse_scaling_test.exe
