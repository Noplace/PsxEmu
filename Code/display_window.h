#ifndef UISYSTEM_DISPLAY_WINDOW_H
#define UISYSTEM_DISPLAY_WINDOW_H

#include <WinCore/timer/timer2.h>
#include <WinCore/windows/windows.h>
#include <VisualEssence/Code/ve.h>
#include "../Resource/ui.h"
#include "emulation/psx/system.h"

namespace my_app {

/*
  Class Name  : DisplayWindow
  Description : this is the application's main window
*/
class DisplayWindow: public core::windows::Window {
  public:
    DisplayWindow();
    ~DisplayWindow();
    void Initialize();
    void Step();
   protected:
    int OnCreate(WPARAM wParam,LPARAM lParam);
    int OnDestroy(WPARAM wParam,LPARAM lParam);
    int OnCommand(WPARAM wParam,LPARAM lParam);
  private:
    emulation::psx::System psx_sys;
    graphics::ContextD3D9* gfx;
    utilities::Timer<double> timer;
    struct {
      uint64_t extra_cycles;
      uint64_t current_cycles;
      uint64_t prev_cycles;
      uint64_t total_cycles;
      uint32_t fps_counter;
      uint32_t ups_counter;
      uint32_t fps;
      uint32_t ups;
      double render_time_span;
      double fps_time_span;
      double span_accumulator;

    } timing;
};

}

#endif