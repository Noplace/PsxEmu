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
#include "display_window.h"



namespace my_app {

//const double M_PI = 3.14159265358979323846;

DisplayWindow::DisplayWindow() : core::windows::Window() {
  
}

DisplayWindow::~DisplayWindow() {

}


void DisplayWindow::Initialize() {
  PrepareClass("Test");
  this->Create();
  SetClientSize(640,480);
  Center();
  SetMenu(LoadMenu(NULL,MAKEINTRESOURCE(IDR_MAINMENU)));
  memset(&timing,0,sizeof(timing));
  timer.Calibrate();
  timing.prev_cycles = timer.GetCurrentCycles();
  
  gfx = new graphics::ContextD3D9();
  gfx->Initialize();
  gfx->CreateDisplay(this);
  psx_sys.gpu().set_gfx(gfx);
  psx_sys.Initialize();
  Show();
}

void DisplayWindow::Step() {

  const double cpu_freq_hz = 33868800.0;  //33.8688 Mhz
  const double dt =  1000.0 / cpu_freq_hz;//options.cpu_freq(); 0.00058f;//16.667f;
  timing.current_cycles = timer.GetCurrentCycles();
  double time_span =  (timing.current_cycles - timing.prev_cycles) * timer.resolution();
  if (time_span > 250.0) //clamping time
    time_span = 250.0;

  timing.span_accumulator += time_span;
  //while (timing.span_accumulator >= dt) {
    psx_sys.Step();
    timing.span_accumulator -= dt;
  //}

  timing.total_cycles += timing.current_cycles-timing.prev_cycles;
  timing.prev_cycles = timing.current_cycles;
  


  timing.render_time_span += time_span;
  if (timing.render_time_span >= 16.667) {
    gfx->ClearTarget();
    gfx->Begin();

    gfx->End();
    gfx->Render();
    ++timing.fps_counter;
    timing.render_time_span = 0;
  }
  
  timing.fps_time_span += time_span;
  if (timing.fps_time_span >= 1000.0) {
    timing.fps = timing.fps_counter;
    timing.fps_counter = 0;
    timing.fps_time_span = 0;

    char caption[256];
    //sprintf(caption,"Freq : %0.2f MHz",nes.frequency_mhz());
    //sprintf(caption,"CPS: %llu ",nes.cycles_per_second());
    
    sprintf_s(caption,"FPS: %02d",timing.fps);
    SetWindowText(handle(),caption);
 
  }
  

}

int DisplayWindow::OnCreate(WPARAM wParam,LPARAM lParam) {
  return 0;
}

int DisplayWindow::OnDestroy(WPARAM wParam,LPARAM lParam) {
  psx_sys.Deinitialize();
  gfx->Deinitialize();
  delete gfx;
  PostQuitMessage(0);
  return 0;
}

int DisplayWindow::OnCommand(WPARAM wParam,LPARAM lParam) {
  return 0;
}

}