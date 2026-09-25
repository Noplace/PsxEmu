/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

// The include set every translation unit in this project needs, so each one opens with a single
// include rather than the same block of a dozen. The same thing psx/psx.h is for the core, and the
// same name GBAEmu uses for it - see Docs/Emulator-Project-Standards.md section 3.
//
// Deliberately the *common* set and nothing more. The narrow headers - commdlg.h, shlobj.h,
// shellapi.h, xinput.h, and the two Direct3D backends - stay in the single file that needs each,
// along with the #pragma comment(lib) that goes with them, so what a file actually depends on stays
// readable from the top of that file.

#include "psx/psx.h"   // also brings in windows.h, with WIN32_LEAN_AND_MEAN and NOMINMAX

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
