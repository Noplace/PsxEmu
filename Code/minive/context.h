#include <Windows.h>

namespace minive {

class Context {
 public:
  Context() {}
  ~Context() {}
  virtual int Initialize(int width, int height, bool vsync, HWND hwnd, bool fullscreen, float depth, float near) = 0;
  virtual int Deinitialize() = 0;
  virtual int Clear() = 0;
  virtual int Present() = 0;
};

}
