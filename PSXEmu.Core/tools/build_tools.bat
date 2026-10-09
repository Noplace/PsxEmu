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

rem The vendored C libraries under PSXEmu.Core\lib that disc.cpp reads CHD
rem images through - libchdr, and the zlib, LZMA and (stub) zstd it decodes
rem with - compiled once into one library every harness links. Warnings off:
rem they are other people's code, kept as it came.
set THIRD=PSXEmu.Core\lib
if not exist Temp\tools\obj_thirdparty mkdir Temp\tools\obj_thirdparty
cl /nologo /c /O2 /MD /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DZ7_ST /W0 ^
   /I %THIRD%\libchdr\include /I %THIRD%\zlib /I %THIRD%\lzma /I %THIRD%\zstd_stub ^
   /Fo:Temp\tools\obj_thirdparty\ %THIRD%\libchdr\src\*.c %THIRD%\zlib\*.c ^
   %THIRD%\lzma\*.c %THIRD%\zstd_stub\*.c
if errorlevel 1 exit /b 1
lib /nologo /OUT:Temp\tools\thirdparty.lib Temp\tools\obj_thirdparty\*.obj
if errorlevel 1 exit /b 1

set CORE=PSXEmu.Core\psx\cpu.cpp PSXEmu.Core\psx\gte.cpp PSXEmu.Core\psx\gpu.cpp ^
 PSXEmu.Core\psx\software_raster.cpp ^
 PSXEmu.Core\psx\dma.cpp PSXEmu.Core\psx\io_interface.cpp PSXEmu.Core\psx\kernel.cpp ^
 PSXEmu.Core\psx\mc.cpp PSXEmu.Core\psx\mc_directory.cpp PSXEmu.Core\psx\spu.cpp PSXEmu.Core\psx\system.cpp ^
 PSXEmu.Core\psx\cdrom.cpp PSXEmu.Core\psx\disc.cpp PSXEmu.Core\psx\sio.cpp PSXEmu.Core\psx\sio1.cpp ^
 PSXEmu.Core\psx\iso9660.cpp PSXEmu.Core\psx\mdec.cpp PSXEmu.Core\psx\root_counter.cpp ^
 PSXEmu.Core\psx\state.cpp PSXEmu.Core\psx\debug_assist.cpp PSXEmu.Core\psx\debugger.cpp ^
 PSXEmu.Core\psx\bios_calls.cpp

set FLAGS=/nologo /std:c++20 /permissive- /EHsc /O2 /MD /DNDEBUG /D_CONSOLE ^
 /D_CRT_SECURE_NO_WARNINGS /I PSXEmu.Core /I PSXEmu.Core\lib\libchdr\include ^
 /I PSXEmu.Core\lib\zlib /I PSXEmu.Core\lib\lzma
set LIBS=/link /SUBSYSTEM:CONSOLE user32.lib psapi.lib Temp\tools\thirdparty.lib

rem The front end's hardware rasterisers, Direct3D 11's and 12's, for the tools that draw with
rem them (--hw-raster, and --d3d12 for the second).
set HWRASTER=PSXEmu.Win32\graphics\hw_raster\d3d11_raster.cpp ^
 PSXEmu.Win32\graphics\hw_raster\d3d12_raster.cpp ^
 PSXEmu.Win32\graphics\hw_raster\hardware_raster.cpp

rem boot_runner alone also has the front end's hardware rasteriser, for --hw-raster.
cl %FLAGS% /DPSXEMU_HW_RASTER /I PSXEmu.Win32 /Fo:Temp\tools\obj_boot\ ^
   /Fe:Temp\tools\boot_runner.exe PSXEmu.Core\tools\boot_runner.cpp ^
   %HWRASTER% %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_media mkdir Temp\tools\obj_media
cl %FLAGS% /Fo:Temp\tools\obj_media\ /Fe:Temp\tools\media_test.exe ^
   PSXEmu.Core\tools\media_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_cpu mkdir Temp\tools\obj_cpu
cl %FLAGS% /Fo:Temp\tools\obj_cpu\ /Fe:Temp\tools\cpu_test.exe ^
   PSXEmu.Core\tools\cpu_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_debug mkdir Temp\tools\obj_debug
cl %FLAGS% /Fo:Temp\tools\obj_debug\ /Fe:Temp\tools\debug_test.exe ^
   PSXEmu.Core\tools\debug_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_timing mkdir Temp\tools\obj_timing
cl %FLAGS% /Fo:Temp\tools\obj_timing\ /Fe:Temp\tools\timing_test.exe ^
   PSXEmu.Core\tools\timing_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_mc mkdir Temp\tools\obj_mc
cl %FLAGS% /Fo:Temp\tools\obj_mc\ /Fe:Temp\tools\mc_test.exe ^
   PSXEmu.Core\tools\mc_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_disc mkdir Temp\tools\obj_disc
cl %FLAGS% /Fo:Temp\tools\obj_disc\ /Fe:Temp\tools\make_test_disc.exe ^
   PSXEmu.Core\tools\make_test_disc.cpp %LIBS%
if errorlevel 1 exit /b 1

rem Any image PSXEmu mounts, written out as a CHD - tools\chd_writer.h.
if not exist Temp\tools\obj_chd mkdir Temp\tools\obj_chd
cl %FLAGS% /Fo:Temp\tools\obj_chd\ /Fe:Temp\tools\make_chd.exe ^
   PSXEmu.Core\tools\make_chd.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_spu mkdir Temp\tools\obj_spu
cl %FLAGS% /Fo:Temp\tools\obj_spu\ /Fe:Temp\tools\spu_test.exe ^
   PSXEmu.Core\tools\spu_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_timer mkdir Temp\tools\obj_timer
cl %FLAGS% /Fo:Temp\tools\obj_timer\ /Fe:Temp\tools\timer_test.exe ^
   PSXEmu.Core\tools\timer_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_sio mkdir Temp\tools\obj_sio
cl %FLAGS% /Fo:Temp\tools\obj_sio\ /Fe:Temp\tools\sio_test.exe ^
   PSXEmu.Core\tools\sio_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_gpu mkdir Temp\tools\obj_gpu
rem With the hardware rasteriser too, for gpu_test --hw-raster.
cl %FLAGS% /DPSXEMU_HW_RASTER /I PSXEmu.Win32 /Fo:Temp\tools\obj_gpu\ ^
   /Fe:Temp\tools\gpu_test.exe PSXEmu.Core\tools\gpu_test.cpp ^
   %HWRASTER% %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_mdec mkdir Temp\tools\obj_mdec
cl %FLAGS% /Fo:Temp\tools\obj_mdec\ /Fe:Temp\tools\mdec_test.exe ^
   PSXEmu.Core\tools\mdec_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_gte mkdir Temp\tools\obj_gte
cl %FLAGS% /Fo:Temp\tools\obj_gte\ /Fe:Temp\tools\gte_test.exe ^
   PSXEmu.Core\tools\gte_test.cpp %CORE% %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_wav mkdir Temp\tools\obj_wav
cl %FLAGS% /Fo:Temp\tools\obj_wav\ /Fe:Temp\tools\wav_pitch.exe ^
   PSXEmu.Core\tools\wav_pitch.cpp %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_letterbox mkdir Temp\tools\obj_letterbox
cl %FLAGS% /Fo:Temp\tools\obj_letterbox\ /Fe:Temp\tools\letterbox_test.exe ^
   PSXEmu.Core\tools\letterbox_test.cpp %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_framelimit mkdir Temp\tools\obj_framelimit
cl %FLAGS% /Fo:Temp\tools\obj_framelimit\ /Fe:Temp\tools\frame_limiter_test.exe ^
   PSXEmu.Core\tools\frame_limiter_test.cpp %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_speedres mkdir Temp\tools\obj_speedres
cl %FLAGS% /Fo:Temp\tools\obj_speedres\ /Fe:Temp\tools\speed_resampler_test.exe ^
   PSXEmu.Core\tools\speed_resampler_test.cpp %LIBS%
if errorlevel 1 exit /b 1

if not exist Temp\tools\obj_mousescale mkdir Temp\tools\obj_mousescale
cl %FLAGS% /Fo:Temp\tools\obj_mousescale\ /Fe:Temp\tools\mouse_scaling_test.exe ^
   PSXEmu.Core\tools\mouse_scaling_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem Controller bindings: platform\input_bindings.h, and the front end's
rem controller_bindings.h on top of it, for its inline code only - nothing of
rem the front end is linked.
if not exist Temp\tools\obj_bindings mkdir Temp\tools\obj_bindings
cl %FLAGS% /I PSXEmu.Win32 /Fo:Temp\tools\obj_bindings\ /Fe:Temp\tools\bindings_test.exe ^
   PSXEmu.Core\tools\bindings_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem GameShark codes: psx\cheats.h is header-only, and runs against a RAM of the test's own.
if not exist Temp\tools\obj_cheats mkdir Temp\tools\obj_cheats
cl %FLAGS% /Fo:Temp\tools\obj_cheats\ /Fe:Temp\tools\cheats_test.exe ^
   PSXEmu.Core\tools\cheats_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem The recompiler, which is being built beside the core rather than into it -
rem nothing in psx\ includes any of this, and the emulator does not link it.
rem Only the emitter files rec_test actually reaches are compiled; the rest of
rem the vendored library is there for when more of it is needed.
if not exist Temp\tools\obj_rec mkdir Temp\tools\obj_rec
cl %FLAGS% /Fo:Temp\tools\obj_rec\ /Fe:Temp\tools\rec_test.exe ^
   PSXEmu.Core\tools\rec_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem The measurement step 6 of the plan is conditional on: compiled against
rem interpreted, and the register allocator on against off.
if not exist Temp\tools\obj_bench mkdir Temp\tools\obj_bench
cl %FLAGS% /Fo:Temp\tools\obj_bench\ /Fe:Temp\tools\rec_bench.exe ^
   PSXEmu.Core\tools\rec_bench.cpp %LIBS%
if errorlevel 1 exit /b 1

rem host\, the one part of Core that knows about threads: the channels between
rem the front end's threads, stress-tested with a thread on each side, and the
rem machine, audio and video threads run for real against a BIOS.
set HOST=PSXEmu.Core\host\machine.cpp PSXEmu.Core\host\audio_output.cpp ^
 PSXEmu.Core\host\video_output.cpp
if not exist Temp\tools\obj_host mkdir Temp\tools\obj_host
cl %FLAGS% /Fo:Temp\tools\obj_host\ /Fe:Temp\tools\host_test.exe ^
   PSXEmu.Core\tools\host_test.cpp %HOST% %CORE% %LIBS%
if errorlevel 1 exit /b 1

rem The hardware rasteriser against the software one, scene by scene, on WARP.
if not exist Temp\tools\obj_hwr mkdir Temp\tools\obj_hwr
cl %FLAGS% /I PSXEmu.Win32 /Fo:Temp\tools\obj_hwr\ /Fe:Temp\tools\hw_raster_test.exe ^
   PSXEmu.Core\tools\hw_raster_test.cpp %HWRASTER% ^
   %CORE% %LIBS%
if errorlevel 1 exit /b 1

rem Two of boot_runner's pictures compared, for the hardware rasteriser against the software one.
if not exist Temp\tools\obj_ppm mkdir Temp\tools\obj_ppm
cl %FLAGS% /Fo:Temp\tools\obj_ppm\ /Fe:Temp\tools\ppm_diff.exe ^
   PSXEmu.Core\tools\ppm_diff.cpp
if errorlevel 1 exit /b 1

rem The arithmetic of sizes around DLSS: graphics\dlss\dlss_choice.h, header-only.
if not exist Temp\tools\obj_dlss mkdir Temp\tools\obj_dlss
cl %FLAGS% /I PSXEmu.Win32 /Fo:Temp\tools\obj_dlss\ /Fe:Temp\tools\dlss_choice_test.exe ^
   PSXEmu.Core\tools\dlss_choice_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem What NVIDIA Streamline says about each card - Docs/DLSS-Plan.md, phase 0. Needs
rem PSXEmu.Win32\graphics\dlss\fetch_streamline.ps1 run first to find anything, but builds without.
if not exist Temp\tools\obj_slprobe mkdir Temp\tools\obj_slprobe
cl %FLAGS% /I PSXEmu.Win32 /Fo:Temp\tools\obj_slprobe\ /Fe:Temp\tools\sl_probe.exe ^
   PSXEmu.Core\tools\sl_probe.cpp PSXEmu.Win32\graphics\dlss\streamline.cpp %LIBS%
if errorlevel 1 exit /b 1

rem The arithmetic of sizes around AMD FSR: graphics\fsr\fsr_choice.h, header-only.
if not exist Temp\tools\obj_fsr mkdir Temp\tools\obj_fsr
cl %FLAGS% /I PSXEmu.Win32 /Fo:Temp\tools\obj_fsr\ /Fe:Temp\tools\fsr_choice_test.exe ^
   PSXEmu.Core\tools\fsr_choice_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem The custom filter chain's passes: graphics\filter_chain.h and shader_pass.h, header-only, and
rem the settings that hold the chain.
if not exist Temp\tools\obj_filterchain mkdir Temp\tools\obj_filterchain
cl %FLAGS% /I PSXEmu.Win32 /Fo:Temp\tools\obj_filterchain\ /Fe:Temp\tools\filter_chain_test.exe ^
   PSXEmu.Core\tools\filter_chain_test.cpp %LIBS%
if errorlevel 1 exit /b 1

rem What AMD's FidelityFX runtime says about each card - Docs/FSR-Plan.md, phase 0. Needs
rem PSXEmu.Win32\graphics\fsr\fetch_fidelityfx.ps1 run first to find anything, but builds without;
rem AMD's loader finds its effects only beside the executable, so they are copied here.
if not exist Temp\tools\obj_ffxprobe mkdir Temp\tools\obj_ffxprobe
cl %FLAGS% /I PSXEmu.Win32 /Fo:Temp\tools\obj_ffxprobe\ /Fe:Temp\tools\ffx_probe.exe ^
   PSXEmu.Core\tools\ffx_probe.cpp PSXEmu.Win32\graphics\fsr\fidelityfx.cpp ^
   PSXEmu.Win32\graphics\dlss\streamline.cpp %LIBS%
if errorlevel 1 exit /b 1
if exist Temp\fidelityfx\v2.3.0\amd_fidelityfx_loader_dx12.dll ^
   xcopy /y /d /q Temp\fidelityfx\v2.3.0\*.dll Temp\tools\ >nul

echo.
echo Built Temp\tools\boot_runner.exe
echo Built Temp\tools\media_test.exe
echo Built Temp\tools\cpu_test.exe

echo Built Temp\tools\gte_test.exe
echo Built Temp\tools\gpu_test.exe
echo Built Temp\tools\spu_test.exe
echo Built Temp\tools\host_test.exe
echo Built Temp\tools\hw_raster_test.exe
echo Built Temp\tools\ppm_diff.exe
