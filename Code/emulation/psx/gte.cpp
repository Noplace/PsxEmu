/*****************************************************************************************************************
* Copyright (c) 2015 Khalid Ali Al-Kooheji                                                                       *
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
#include "global.h"

namespace emulation {
namespace psx {


int GTE::Initialize() {
  memset(&context_,0,sizeof(context_));
  return S_OK;
}

int GTE::Deinitialize() {
  return S_OK;
}

void GTE::ExecuteCommand(uint32_t code) {
  uint8_t command = code & 0x3F;
  sf = (uint8_t)BIT(code,19);

  switch (command) {
    case 0x01:
      RTPS();
    break;
  }
}


void GTE::RTPS() {
  context_.IR1 = context_.MAC1 = (context_.TR.X*0x1000 + context_.RT._11*context_.V0.X + context_.RT._12*context_.V0.Y + context_.RT._13*context_.V0.Z);// SAR (sf*12)
//  context_.IR2 = context_.MAC2 = (context_.TR.Y*0x1000 + RT21*VX0 + RT22*VY0 + RT23*VZ0) SAR (sf*12)
//  context_.IR3 = context_.MAC3 = (context_.TR.Z*0x1000 + RT31*VX0 + RT32*VY0 + RT33*VZ0) SAR (sf*12)
}

}
}
