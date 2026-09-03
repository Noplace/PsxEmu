@echo off
REM Builds the headless test harnesses into Temp\tools. Run from anywhere.
REM
REM Deliberately independent of the solution: the harnesses compile the core
REM sources directly, so they keep working regardless of what the MSBuild
REM configuration is doing, and they are the fastest way to get a compile
REM error out of the core.
setlocal

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo Could not find vcvars64.bat at:
  echo   %VCVARS%
  echo Edit build_tools.bat if your Visual Studio install differs.
  exit /b 1
)
call "%VCVARS%" >nul

cd /d "%~dp0..\.."
if not exist Temp\tools\obj_boot mkdir Temp\tools\obj_boot

set CORE=PSXEmu.Core\psx\cpu.cpp PSXEmu.Core\psx\gte.cpp PSXEmu.Core\psx\gpu.cpp ^
 PSXEmu.Core\psx\dma.cpp PSXEmu.Core\psx\io_interface.cpp PSXEmu.Core\psx\kernel.cpp ^
 PSXEmu.Core\psx\mc.cpp PSXEmu.Core\psx\spu.cpp PSXEmu.Core\psx\system.cpp ^
 PSXEmu.Core\psx\cdrom.cpp PSXEmu.Core\psx\disc.cpp PSXEmu.Core\psx\sio.cpp ^
 PSXEmu.Core\psx\iso9660.cpp PSXEmu.Core\psx\mdec.cpp ^
 PSXEmu.Core\psx\debug_assist.cpp

set FLAGS=/nologo /std:c++20 /permissive- /EHsc /O2 /MD /DNDEBUG /D_CONSOLE ^
 /D_CRT_SECURE_NO_WARNINGS /I PSXEmu.Core
set LIBS=/link /SUBSYSTEM:CONSOLE user32.lib

cl %FLAGS% /Fo:Temp\tools\obj_boot\ /Fe:Temp\tools\boot_runner.exe ^
   PSXEmu.Core\tools\boot_runner.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_media mkdir Temp\tools\obj_media
cl %FLAGS% /Fo:Temp\tools\obj_media\ /Fe:Temp\tools\media_test.exe ^
   PSXEmu.Core\tools\media_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_cpu mkdir Temp\tools\obj_cpu
cl %FLAGS% /Fo:Temp\tools\obj_cpu\ /Fe:Temp\tools\cpu_test.exe ^
   PSXEmu.Core\tools\cpu_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_disc mkdir Temp\tools\obj_disc
cl %FLAGS% /Fo:Temp\tools\obj_disc\ /Fe:Temp\tools\make_test_disc.exe ^
   PSXEmu.Core\tools\make_test_disc.cpp %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_spu mkdir Temp\tools\obj_spu
cl %FLAGS% /Fo:Temp\tools\obj_spu\ /Fe:Temp\tools\spu_test.exe ^
   PSXEmu.Core\tools\spu_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_sio mkdir Temp\tools\obj_sio
cl %FLAGS% /Fo:Temp\tools\obj_sio\ /Fe:Temp\tools\sio_test.exe ^
   PSXEmu.Core\tools\sio_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_mdec mkdir Temp\tools\obj_mdec
cl %FLAGS% /Fo:Temp\tools\obj_mdec\ /Fe:Temp\tools\mdec_test.exe ^
   PSXEmu.Core\tools\mdec_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_gte mkdir Temp\tools\obj_gte
cl %FLAGS% /Fo:Temp\tools\obj_gte\ /Fe:Temp\tools\gte_test.exe ^
   PSXEmu.Core\tools\gte_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

echo.
echo Built Temp\tools\boot_runner.exe
echo Built Temp\tools\media_test.exe
echo Built Temp\tools\cpu_test.exe

echo Built Temp\tools\gte_test.exe
echo Built Temp	ools\spu_test.exe
