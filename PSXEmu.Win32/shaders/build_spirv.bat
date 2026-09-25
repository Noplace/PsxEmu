@echo off
REM Rebuilds spirv_filters.h - the Vulkan engine's shaders as SPIR-V - from glsl_filters.h.
REM
REM     build_spirv.bat <path to glslang.exe>
REM
REM Needed only after changing a filter in glsl_filters.h; the build stops with a message saying
REM so if the two disagree. glslang is Khronos's GLSL compiler (github.com/KhronosGroup/glslang,
REM releases, the windows-x86_64-release zip, bin\glslang.exe); nothing else in the project needs it.
setlocal
if "%~1"=="" (
  echo usage: build_spirv.bat ^<path to glslang.exe^>
  exit /b 2
)

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo Could not find vcvars64.bat at:
  echo   %VCVARS%
  exit /b 1
)
call "%VCVARS%" >nul

cd /d "%~dp0..\.."
if not exist Temp\tools\obj_spirv mkdir Temp\tools\obj_spirv
cl /nologo /std:c++20 /EHsc /O2 /MD /D_CRT_SECURE_NO_WARNINGS /Fo:Temp\tools\obj_spirv\ ^
   /Fe:Temp\tools\make_spirv.exe PSXEmu.Win32\shaders\make_spirv.cpp
if errorlevel 1 exit /b 1

cd /d Temp\tools\obj_spirv
..\make_spirv.exe "%~f1" "%~dp0spirv_filters.h"
