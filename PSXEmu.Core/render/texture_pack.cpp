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
#include "render/texture_pack.h"

#include "emucore/image/png_file.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace emulation {
namespace psx {

namespace {

// Every image any pack makes gets its own number, so a rasteriser's cache of what it has on the
// card cannot mistake one for another - across packs opened one after another too.
std::atomic<uint64_t> next_image_id{ 1 };

std::string Trimmed(const std::string& text) {
  size_t first = 0, last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first])))
    ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])))
    --last;
  return text.substr(first, last - first);
}

std::string Lower(std::string text) {
  for (char& c : text)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// A name as pack.ini or a file gives it, without ".png".
std::string Bare(const std::string& name) {
  const std::string trimmed = Trimmed(name);
  if (trimmed.size() > 4 && Lower(trimmed.substr(trimmed.size() - 4)) == ".png")
    return trimmed.substr(0, trimmed.size() - 4);
  return trimmed;
}

}  // namespace

TexturePack::TexturePack(const TextureTracker* tracker) : tracker_(tracker) {}

TexturePack::~TexturePack() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

bool TexturePack::Open(const std::string& folder, std::string* error,
                       const std::string& ai_folder) {
  namespace fs = std::filesystem;
  std::error_code failed;
  if (!folder.empty() && !fs::is_directory(fs::path(folder), failed)) {
    *error = "there is no folder " + folder;
    return false;
  }
  // The images: every PNG named as a source is named, subfolders too - the first of a name found.
  std::unordered_map<uint64_t, Entry> entries;
  std::unordered_map<std::string, uint64_t> by_name;
  std::vector<std::string> warnings;
  auto scan = [&](const std::string& dir, bool ai) {
    for (fs::recursive_directory_iterator it(fs::path(dir), failed), end; !failed && it != end;
         it.increment(failed)) {
      if (!it->is_regular_file(failed))
        continue;
      const fs::path& path = it->path();
      if (Lower(path.extension().string()) != ".png")
        continue;
      const std::string name = path.stem().string();
      uint64_t image_hash, clut_hash;
      uint32_t mode;
      if (!TextureTracker::ParseName(name, &image_hash, &clut_hash, &mode))
        continue;
      const uint64_t key = TextureTracker::Key(image_hash, clut_hash, mode);
      if (entries.count(key) != 0)
        continue;
      Entry entry;
      entry.path = path.string();
      entry.name = name;
      entry.ai = ai;
      entries.emplace(key, entry);
      by_name.emplace(Lower(name), key);
    }
  };
  if (!folder.empty())
    scan(folder, false);
  if (failed) {
    *error = "the folder " + folder + " could not be read: " + failed.message();
    return false;
  }

  // pack.ini.
  bool bilinear = false;
  std::ifstream ini;
  if (!folder.empty())
    ini.open(fs::path(folder) / "pack.ini");
  std::string line, section;
  int number = 0;
  std::vector<std::pair<std::string, bool>> filters;          // name, bilinear
  std::vector<std::pair<std::string, std::string>> aliases;   // name, the image's name
  std::vector<std::string> never;
  while (std::getline(ini, line)) {
    ++number;
    const size_t comment = line.find_first_of(";#");
    line = Trimmed(comment == std::string::npos ? line : line.substr(0, comment));
    if (line.empty())
      continue;
    const std::string where = "pack.ini line " + std::to_string(number) + ": ";
    if (line.front() == '[' && line.back() == ']') {
      section = Lower(Trimmed(line.substr(1, line.size() - 2)));
      if (section != "pack" && section != "filter" && section != "alias" && section != "never")
        warnings.push_back(where + "no section [" + section + "]");
      continue;
    }
    const size_t equals = line.find('=');
    const std::string left = Trimmed(equals == std::string::npos ? line : line.substr(0, equals));
    const std::string right = equals == std::string::npos ? "" : Lower(Trimmed(line.substr(equals + 1)));
    if (section == "never") {
      never.push_back(Bare(left));
    } else if (section == "pack" || section == "filter") {
      if (right != "nearest" && right != "bilinear") {
        warnings.push_back(where + "a filter is nearest or bilinear");
        continue;
      }
      if (section == "pack" && Lower(left) == "filter")
        bilinear = right == "bilinear";
      else if (section == "filter")
        filters.emplace_back(Bare(left), right == "bilinear");
      else
        warnings.push_back(where + "[pack] has only filter");
    } else if (section == "alias") {
      if (equals == std::string::npos)
        warnings.push_back(where + "an alias is <name> = <another name>");
      else
        aliases.emplace_back(Bare(left), Bare(line.substr(equals + 1)));
    } else {
      warnings.push_back(where + "outside any section");
    }
  }
  for (auto& [key, entry] : entries)
    entry.bilinear = bilinear;
  // Aliases first, so a filter or never named for one applies to it.
  for (const auto& [name, target] : aliases) {
    uint64_t image_hash, clut_hash, key;
    uint32_t mode;
    const auto found = by_name.find(Lower(target));
    if (!TextureTracker::ParseName(name, &image_hash, &clut_hash, &mode)) {
      warnings.push_back("pack.ini: " + name + " is not a texture's name");
    } else if (found == by_name.end()) {
      warnings.push_back("pack.ini: no image " + target + " for " + name);
    } else if (entries.count(key = TextureTracker::Key(image_hash, clut_hash, mode)) == 0) {
      Entry alias = entries[found->second];
      alias.name = name;
      entries.emplace(key, alias);
      by_name.emplace(Lower(name), key);
    }
  }
  auto entry_named = [&](const std::string& name) -> Entry* {
    const auto found = by_name.find(Lower(name));
    return found == by_name.end() ? nullptr : &entries[found->second];
  };
  for (const auto& [name, linear] : filters) {
    if (Entry* entry = entry_named(name))
      entry->bilinear = linear;
    else
      warnings.push_back("pack.ini: no image " + name + " to filter");
  }
  // [never] needs no image: it can keep a texture the pack has none for from ever being one.
  for (const std::string& name : never) {
    if (Entry* entry = entry_named(name)) {
      entry->never = true;
      continue;
    }
    uint64_t image_hash, clut_hash;
    uint32_t mode;
    if (TextureTracker::ParseName(name, &image_hash, &clut_hash, &mode)) {
      Entry entry;
      entry.name = name;
      entry.never = true;
      entries.emplace(TextureTracker::Key(image_hash, clut_hash, mode), entry);
    } else {
      warnings.push_back("pack.ini: " + name + " is not a texture's name");
    }
  }
  // The upscaler's, under all of that: only for what the pack has nothing for, and not for what
  // it is never to replace. A folder not made yet has nothing in it.
  if (!ai_folder.empty() && fs::is_directory(fs::path(ai_folder), failed)) {
    scan(ai_folder, true);
    failed.clear();
    for (auto& [key, entry] : entries)
      if (entry.ai)
        entry.bilinear = bilinear;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  ++generation_;
  queue_.clear();
  entries_ = std::move(entries);
  warnings_ = std::move(warnings);
  counts_ = Counts();
  for (const auto& [key, entry] : entries_)
    if (!entry.never)
      ++counts_.files;
  return true;
}

bool TexturePack::HandMade(uint64_t key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(key);
  return found != entries_.end() && (!found->second.ai || found->second.never);
}

void TexturePack::Provide(uint64_t key, const std::string& name, const std::string& path,
                          std::shared_ptr<const ReplacementImage> image) {
  std::lock_guard<std::mutex> lock(mutex_);
  Entry& entry = entries_[key];
  if (!entry.path.empty() && !entry.ai)
    return;   // the pack's own wins
  if (entry.never)
    return;
  if (entry.path.empty())
    ++counts_.files;
  if (entry.image)
    counts_.bytes -= entry.image->pixels.size();
  entry.ai = true;
  entry.name = name;
  entry.path = path;
  entry.image = std::move(image);
  entry.width = entry.image->width;
  entry.height = entry.image->height;
  entry.state = State::kReady;
  counts_.bytes += entry.image->pixels.size();
  ++counts_.provided;
  Trim();
}

void TexturePack::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++generation_;
  queue_.clear();
  entries_.clear();
  counts_.bytes = 0;
  counts_.files = 0;
}

TexturePack::Counts TexturePack::counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}

std::vector<std::string> TexturePack::warnings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return warnings_;
}

bool TexturePack::Replace(const DrawJob& job, TextureReplacement* replacement) {
  (void)job;   // the tracker has already said what it names
  const TextureTracker::Named& named = tracker_->current();
  if (!named.named)
    return false;
  std::unique_lock<std::mutex> lock(mutex_);
  auto found = entries_.find(named.key);
  if (found == entries_.end()) {
    ++counts_.jobs_unmatched;
    return false;
  }
  Entry* entry = &found->second;
  if (entry->never) {
    ++counts_.jobs_never;
    return false;
  }
  entry->used = ++serial_;
  if (entry->state == State::kUnloaded) {
    entry->state = State::kLoading;
    entry->width = named.width;
    entry->height = named.height;
    if (synchronous_) {
      const uint64_t generation = generation_;
      lock.unlock();
      Load(named.key, generation);
      lock.lock();
      found = entries_.find(named.key);
      if (found == entries_.end())
        return false;
      entry = &found->second;
    } else {
      queue_.emplace_back(named.key, generation_);
      if (!thread_.joinable())
        thread_ = std::thread(&TexturePack::Loader, this);
      wake_.notify_one();
    }
  }
  if (entry->state == State::kLoading) {
    ++counts_.jobs_waiting;
    return false;
  }
  if (entry->state != State::kReady || entry->image->width != named.width ||
      entry->image->height != named.height)
    return false;   // unreadable, or for a source of another size that hashed the same
  replacement->image = entry->image;
  replacement->page_x = named.page_x;
  replacement->page_y = named.page_y;
  ++counts_.jobs_replaced;
  if (entry->ai)
    ++counts_.jobs_ai;
  return true;
}

void TexturePack::Loader() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
    if (stopping_)
      return;
    const std::pair<uint64_t, uint64_t> next = queue_.front();
    queue_.pop_front();
    lock.unlock();
    Load(next.first, next.second);
    lock.lock();
  }
}

void TexturePack::Load(uint64_t key, uint64_t generation) {
  std::string path, name;
  uint32_t width = 0, height = 0;
  bool bilinear = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(key);
    if (generation != generation_ || found == entries_.end())
      return;
    path = found->second.path;
    name = found->second.name;
    width = found->second.width;
    height = found->second.height;
    bilinear = found->second.bilinear;
  }
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  auto image = std::make_shared<ReplacementImage>();
  std::string why;
  bool read = image::ReadPngFile(path, &w, &h, &rgba);
  const int times = test_scale_;
  if (read && times > 1) {
    std::vector<uint8_t> larger(static_cast<size_t>(w) * times * h * times * 4);
    for (int y = 0; y < h * times; ++y)
      for (int x = 0; x < w * times; ++x)
        memcpy(&larger[(static_cast<size_t>(y) * w * times + x) * 4],
               &rgba[(static_cast<size_t>(y / times) * w + x / times) * 4], 4);
    rgba.swap(larger);
    w *= times;
    h *= times;
  }
  const bool made = read && MakeImage(rgba, w, h, width, height, bilinear, image.get(), &why);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = entries_.find(key);
  if (generation != generation_ || found == entries_.end())
    return;   // the pack was closed or opened again meanwhile
  Entry& entry = found->second;
  if (entry.state != State::kLoading)
    return;   // the upscaler handed over a newer one meanwhile
  if (!made) {
    entry.state = State::kBad;
    ++counts_.bad;
    warnings_.push_back(name + ": " + (read ? why : std::string("not a PNG that can be read")));
    return;
  }
  entry.image = image;
  entry.state = State::kReady;
  ++counts_.loaded;
  counts_.bytes += image->pixels.size();
  Trim();
}

void TexturePack::Trim() {
  while (counts_.bytes > budget_) {
    Entry* oldest = nullptr;
    for (auto& [key, entry] : entries_)
      if (entry.state == State::kReady && (oldest == nullptr || entry.used < oldest->used))
        oldest = &entry;
    // The one just read is the newest, and stays even alone over the budget.
    if (oldest == nullptr || oldest->used == serial_)
      return;
    counts_.bytes -= oldest->image->pixels.size();
    oldest->image.reset();
    oldest->state = State::kUnloaded;
    ++counts_.dropped;
  }
}

uint64_t TexturePack::NewImageId() {
  return next_image_id++;
}

bool TexturePack::MakeImage(const std::vector<uint8_t>& rgba, int w, int h, uint32_t width,
                            uint32_t height, bool bilinear, ReplacementImage* image,
                            std::string* error) {
  if (width == 0 || height == 0 || w <= 0 || h <= 0 ||
      static_cast<uint32_t>(w) % width != 0 || static_cast<uint32_t>(h) % height != 0 ||
      static_cast<uint32_t>(w) / width != static_cast<uint32_t>(h) / height ||
      static_cast<uint32_t>(w) / width > 8) {
    *error = std::to_string(w) + "x" + std::to_string(h) + " is not 1x to 8x its texture's " +
             std::to_string(width) + "x" + std::to_string(height);
    return false;
  }
  image->id = next_image_id++;
  image->width = width;
  image->height = height;
  image->scale = static_cast<uint32_t>(w) / width;
  image->bilinear = bilinear;
  image->blocks.clear();
  image->pixels.clear();
  const uint32_t side = image->block_pixels();
  std::vector<uint8_t> block(static_cast<size_t>(side) * side * 4);
  for (uint32_t by = 0; by < image->blocks_y(); ++by)
    for (uint32_t bx = 0; bx < image->blocks_x(); ++bx) {
      // Past the image's edge a block is clear; nothing reads there.
      block.assign(block.size(), 0);
      bool any = false;
      for (uint32_t row = 0; row < side; ++row) {
        const uint32_t y = by * side + row;
        if (y >= static_cast<uint32_t>(h))
          break;
        for (uint32_t col = 0; col < side; ++col) {
          const uint32_t x = bx * side + col;
          if (x >= static_cast<uint32_t>(w))
            break;
          const uint8_t* from = &rgba[(static_cast<size_t>(y) * w + x) * 4];
          uint8_t* to = &block[(static_cast<size_t>(row) * side + col) * 4];
          // Alpha to the nearest of the three the console has; a clear pixel is all zeros.
          const uint8_t a = from[3] < 64 ? 0 : from[3] < 192 ? 128 : 255;
          if (a == 0)
            continue;
          to[0] = from[0];
          to[1] = from[1];
          to[2] = from[2];
          to[3] = a;
          any = true;
        }
      }
      if (!any)
        continue;   // drawn with the game's own texels
      image->blocks.push_back(bx + by * image->blocks_x());
      image->pixels.insert(image->pixels.end(), block.begin(), block.end());
    }
  return true;
}

}  // namespace psx
}  // namespace emulation
