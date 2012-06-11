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

template<typename T,size_t initial_size=10>
class Array {
 public:
  Array() : count_(0),renew_count(0),new_size_mul(1.2) {
    size_ = initial_size;
    array_ = new T[size_];
  }
  virtual ~Array() {
    delete [] array_;
  }
  void Add(T item) {
    if (count_ == size_) {
      ++renew_count;
      double freq = renew_count / (double)count_;
      if (freq>0.1)
        new_size_mul*=1.3;
      size_t new_size = size_t(size_ * new_size_mul);
      T* new_array = new T[new_size];
      memcpy(new_array,array_,sizeof(T)*size_);
      delete [] array_;
      array_ = new_array;
      size_ = new_size;
    }
    array_[count_++] = item;
  }
  size_t size() { return size_; }
 private:
  T* array_;
  size_t count_,size_;
  size_t renew_count;
  double new_size_mul;
};

int __stdcall WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  unsigned old_fp_state;
  _controlfp_s(&old_fp_state, _PC_53, _MCW_PC);
  /*Array<int> test1;
  std::vector<int> test2;
  
  LARGE_INTEGER pc1,pc2;
  QueryPerformanceCounter(&pc1);
  for (int i=0;i<9999999;++i)
    test1.Add(i);
  QueryPerformanceCounter(&pc2);

  auto d1 = pc2.QuadPart - pc1.QuadPart;
  QueryPerformanceCounter(&pc1);
  for (int i=0;i<9999999;++i)
    test2.push_back(i);
  QueryPerformanceCounter(&pc2);

  auto d2 = pc2.QuadPart - pc1.QuadPart;
  char debug_str[256];
 
  sprintf(debug_str,"speed1:%llu - speed2:%llu\n",d1,d2);
  OutputDebugString(debug_str);
  sprintf(debug_str,"size1:%d - size2:%d\n",test1.size(), test2.capacity());
  OutputDebugString(debug_str);*/
  my_app::DisplayWindow display_window;
  //void read_iso_cd();
  //read_iso_cd();
  display_window.Initialize();

  MSG msg;
  do {
    if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    } else {
      display_window.Step();
    }
  } while(msg.message!=WM_QUIT);
 
   //Return the exit code to the system. 
   return static_cast<int>(msg.wParam);
}