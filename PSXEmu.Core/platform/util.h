// Replaces the SafeDelete / SafeRelease helpers that came from WinCore.
#pragma once

namespace platform {

template <typename T>
inline void SafeDeleteImpl(T** p) {
  if (p != nullptr && *p != nullptr) {
    delete *p;
    *p = nullptr;
  }
}

template <typename T>
inline void SafeDeleteArrayImpl(T** p) {
  if (p != nullptr && *p != nullptr) {
    delete[] *p;
    *p = nullptr;
  }
}

template <typename T>
inline void SafeReleaseImpl(T** p) {
  if (p != nullptr && *p != nullptr) {
    (*p)->Release();
    *p = nullptr;
  }
}

}  // namespace platform

#define SafeDelete(p)      ::platform::SafeDeleteImpl(p)
#define SafeDeleteArray(p) ::platform::SafeDeleteArrayImpl(p)
#define SafeRelease(p)     ::platform::SafeReleaseImpl(p)
