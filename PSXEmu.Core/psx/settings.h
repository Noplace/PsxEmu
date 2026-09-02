/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

#include "psx/emuconfig.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace emulation {
namespace psx {

// A flat `key = value` settings file, following the same design as GBAEmu's.
//
// Deliberately plain text and hand-editable, because there is no settings UI
// beyond a couple of menu items yet, so the file is how anything else gets
// changed. Unknown keys are kept and written back out, so a file written by a
// newer build is not quietly stripped by an older one, and every getter takes
// the current value as its default, so a missing key leaves whatever was
// already there.
//
// Backed by an ordered map, which keeps the file stable between saves rather
// than reshuffling it on every write.
class SettingsFile {
 public:
  // A missing file is not a failure - it just means nothing is stored yet.
  bool Load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
      return false;
    std::string line;
    while (std::getline(f, line)) {
      const size_t hash = line.find('#');
      if (hash != std::string::npos)
        line.erase(hash);
      const size_t eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      const std::string key = Trim(line.substr(0, eq));
      const std::string value = Trim(line.substr(eq + 1));
      if (!key.empty())
        values_[key] = value;
    }
    return true;
  }

  // The exact text Save() would write. Exposed so a caller can tell whether
  // anything actually changed without touching the disk, which is what lets
  // settings be written as they are edited rather than only at exit - a kill
  // or a crash would otherwise lose them.
  std::string Serialise() const {
    std::ostringstream s;
    s << "# PSXEmu settings. Edit freely; unknown keys are preserved.\n\n";
    for (std::map<std::string, std::string>::const_iterator it =
             values_.begin();
         it != values_.end(); ++it) {
      s << it->first << " = " << it->second << "\n";
    }
    return s.str();
  }

  bool Save(const std::string& path) const {
    std::ofstream f(path.c_str(), std::ios::trunc);
    if (!f.is_open())
      return false;
    f << Serialise();
    return true;
  }

  bool GetBool(const char* key, bool fallback) const {
    const std::string* v = Find(key);
    if (v == nullptr)
      return fallback;
    return *v == "1" || *v == "true" || *v == "yes";
  }
  int GetInt(const char* key, int fallback) const {
    const std::string* v = Find(key);
    if (v == nullptr)
      return fallback;
    try { return std::stoi(*v); } catch (...) { return fallback; }
  }
  float GetFloat(const char* key, float fallback) const {
    const std::string* v = Find(key);
    if (v == nullptr)
      return fallback;
    try { return std::stof(*v); } catch (...) { return fallback; }
  }
  std::string GetString(const char* key, const std::string& fallback) const {
    const std::string* v = Find(key);
    return (v != nullptr) ? *v : fallback;
  }

  void SetBool(const char* key, bool v) { values_[key] = v ? "1" : "0"; }
  void SetInt(const char* key, int v) { values_[key] = std::to_string(v); }
  void SetFloat(const char* key, float v) {
    std::ostringstream s;
    s << v;
    values_[key] = s.str();
  }
  void SetString(const char* key, const std::string& v) { values_[key] = v; }

 private:
  const std::string* Find(const char* key) const {
    std::map<std::string, std::string>::const_iterator it = values_.find(key);
    return (it == values_.end()) ? nullptr : &it->second;
  }
  static std::string Trim(std::string s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
      return std::string();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
  }

  std::map<std::string, std::string> values_;
};

// EmuConfig <-> file. Everything in the struct is a genuine user setting, so
// all of it round-trips.
inline void StoreConfig(SettingsFile& f, const EmuConfig& c) {
  f.SetFloat("audio_volume", c.audio_volume);
}

inline void LoadConfig(const SettingsFile& f, EmuConfig& c) {
  c.audio_volume = f.GetFloat("audio_volume", c.audio_volume);
  if (c.audio_volume < EmuConfig::kMinAudioVolume)
    c.audio_volume = EmuConfig::kMinAudioVolume;
  if (c.audio_volume > EmuConfig::kMaxAudioVolume)
    c.audio_volume = EmuConfig::kMaxAudioVolume;
}

}
}
