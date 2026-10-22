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

// The letterbox math a presenter uses to fit a frame of some target aspect
// ratio into a window of a possibly different one - centred, with bars on
// whichever axis has room left over. No graphics API, no window handle,
// nothing Windows-specific: this is here so it can be checked without a
// device, the same reason boot_runner's own helpers live in this directory.

struct LetterboxRect {
  float x, y, width, height;
};

inline LetterboxRect ComputeLetterboxRect(int back_buffer_width,
                                          int back_buffer_height,
                                          float target_aspect) {
  float view_width = static_cast<float>(back_buffer_width);
  float view_height = view_width / target_aspect;
  if (view_height > back_buffer_height) {
    view_height = static_cast<float>(back_buffer_height);
    view_width = view_height * target_aspect;
  }
  LetterboxRect rect;
  rect.x = (back_buffer_width - view_width) * 0.5f;
  rect.y = (back_buffer_height - view_height) * 0.5f;
  rect.width = view_width;
  rect.height = view_height;
  return rect;
}
