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

#include "emu/emuconfig.h"
#include "emucore/config/settings_file.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

// psxemu.ini and each game's own file (Emu.Core: emucore/config/settings_file.h), saying whose
// they are on their first line.
class SettingsFile : public config::SettingsFile {
 public:
  SettingsFile()
      : config::SettingsFile("# PSXEmu settings. Edit freely; unknown keys are preserved.\n\n") {}
};

// True if `value` is one of the strings in `valid` - shared by both settings
// below, since an unrecognised value is handled the same way either time:
// fall back to whatever was already there, the same as a missing key does.
template <size_t N>
inline bool IsValidChoice(const std::string& value,
                          const std::array<const char*, N>& valid) {
  for (const char* option : valid) {
    if (value == option)
      return true;
  }
  return false;
}

// filter_chain's stages, split at its commas. None for an empty chain; an empty stage for two
// commas together, or one at either end - which IsValidFilterChain refuses.
inline std::vector<std::string> SplitFilterChain(const std::string& chain) {
  std::vector<std::string> stages;
  if (chain.empty())
    return stages;
  size_t start = 0;
  for (;;) {
    const size_t comma = chain.find(',', start);
    if (comma == std::string::npos) {
      stages.push_back(chain.substr(start));
      return stages;
    }
    stages.push_back(chain.substr(start, comma - start));
    start = comma + 1;
  }
}

// Whether filter_chain can hold `chain`: up to kMaxFilterChain stages, each a filter.
inline bool IsValidFilterChain(const std::string& chain) {
  const std::vector<std::string> stages = SplitFilterChain(chain);
  if (stages.size() > EmuConfig::kMaxFilterChain)
    return false;
  for (const std::string& stage : stages) {
    if (stage.empty() || stage == "chain" ||
        !IsValidChoice(stage, EmuConfig::kValidVideoFilters))
      return false;
  }
  return true;
}

// EmuConfig <-> file. Everything in the struct is a genuine user setting, so
// all of it round-trips.
inline void StoreConfig(SettingsFile& f, const EmuConfig& c) {
  f.SetFloat("audio_volume", c.audio_volume);
  f.SetString("graphics_backend", c.graphics_backend);
  f.SetString("graphics_adapter", c.graphics_adapter);
  f.SetString("audio_backend", c.audio_backend);
  f.SetString("video_filter", c.video_filter);
  f.SetString("filter_chain", c.filter_chain);
  f.SetString("frame_pacing", c.frame_pacing);
  f.SetString("controller_type_port1", c.controller_type[0]);
  f.SetString("controller_type_port2", c.controller_type[1]);
  f.SetString("input_source_port1", c.input_source[0]);
  f.SetString("input_source_port2", c.input_source[1]);
  // Eight keys (2 ports x 4 players) - a loop rather than hand-unrolling,
  // unlike the pairs above, since eight near-identical lines would be
  // harder to see are actually identical than the loop that generates them.
  for (int port = 0; port < 2; ++port) {
    for (int player = 0; player < 4; ++player) {
      const std::string key = "multitap_port" + std::to_string(port + 1) +
                              "_player_" + std::string(1, char('a' + player)) +
                              "_source";
      f.SetString(key.c_str(), c.multitap_player_source[port][player]);
      const std::string type_key = "multitap_port" + std::to_string(port + 1) +
                                   "_player_" + std::string(1, char('a' + player)) +
                                   "_type";
      f.SetString(type_key.c_str(), c.multitap_player_type[port][player]);
    }
  }
  f.SetBool("frame_limiter", c.frame_limiter);
  f.SetBool("gpu_thread", c.gpu_thread);
  f.SetString("gpu_rasteriser", c.gpu_rasteriser);
  f.SetInt("resolution_scale", c.resolution_scale);
  f.SetBool("true_color", c.true_color);
  f.SetBool("pgxp_vertices", c.pgxp_vertices);
  f.SetBool("pgxp_textures", c.pgxp_textures);
  f.SetBool("pgxp_culling", c.pgxp_culling);
  f.SetBool("texture_dump", c.texture_dump);
  f.SetBool("texture_replacement", c.texture_replacement);
  f.SetInt("texture_budget_mb", c.texture_budget_mb);
  f.SetString("texture_filter", c.texture_filter);
  f.SetInt("texture_ai_scale", c.texture_ai_scale);
  f.SetString("texture_ai_model", c.texture_ai_model);
  f.SetInt("texture_ai_share", c.texture_ai_share);
  f.SetString("texture_ai_card", c.texture_ai_card);
  f.SetString("dlss_mode", c.dlss_mode);
  f.SetString("dlss_preset", c.dlss_preset);
  f.SetString("dlss_frame_generation", c.dlss_frame_generation);
  f.SetString("fsr_mode", c.fsr_mode);
  f.SetString("fsr_version", c.fsr_version);
  f.SetString("fsr_sharpness", c.fsr_sharpness);
  f.SetBool("fsr_frame_generation", c.fsr_frame_generation);
  f.SetBool("gpu_transfer_timing", c.gpu_transfer_timing);
  f.SetBool("icache_timing", c.icache_timing);
  f.SetBool("exact_event_timing", c.exact_event_timing);
  f.SetBool("dma_stops_cpu", c.dma_stops_cpu);
  f.SetBool("measured_bus_timing", c.measured_bus_timing);
  f.SetBool("write_queue_timing", c.write_queue_timing);
  f.SetBool("cdrom_mechanical_timing", c.cdrom_mechanical_timing);
  f.SetBool("skip_bios_intro", c.skip_bios_intro);
  f.SetBool("recompiler", c.recompiler);
  f.SetBool("gte_x64", c.gte_x64);
  f.SetInt("run_ahead", c.run_ahead);
  f.SetInt("rewind_interval", c.rewind_interval);
  f.SetInt("rewind_budget_mb", c.rewind_budget_mb);
  f.SetString("bios_file", c.bios_file);
  f.SetBool("hle_bios", c.hle_bios);
  f.SetFloat("emulation_speed", c.emulation_speed);
  f.SetBool("pause_in_menus", c.pause_in_menus);
  f.SetBool("show_timings", c.show_timings);
  f.SetBool("show_bios_console", c.show_bios_console);
  f.SetBool("sio1_to_console", c.sio1_to_console);
  f.SetString("mouse_motion", c.mouse_motion);
  f.SetInt("mouse_dpi", c.mouse_dpi);
}

inline void LoadConfig(const SettingsFile& f, EmuConfig& c) {
  c.audio_volume = f.GetFloat("audio_volume", c.audio_volume);
  if (c.audio_volume < EmuConfig::kMinAudioVolume)
    c.audio_volume = EmuConfig::kMinAudioVolume;
  if (c.audio_volume > EmuConfig::kMaxAudioVolume)
    c.audio_volume = EmuConfig::kMaxAudioVolume;

  const std::string backend =
      f.GetString("graphics_backend", c.graphics_backend);
  if (IsValidChoice(backend, EmuConfig::kValidGraphicsBackends))
    c.graphics_backend = backend;

  // Any name is allowed - it is looked up among the cards the machine has - but not one longer
  // than a card's name can be, or holding a line break.
  const std::string adapter = f.GetString("graphics_adapter", c.graphics_adapter);
  if (adapter.size() <= EmuConfig::kMaxGraphicsAdapterLength &&
      adapter.find_first_of("\r\n") == std::string::npos)
    c.graphics_adapter = adapter;

  const std::string audio_backend =
      f.GetString("audio_backend", c.audio_backend);
  if (IsValidChoice(audio_backend, EmuConfig::kValidAudioBackends))
    c.audio_backend = audio_backend;

  const std::string filter = f.GetString("video_filter", c.video_filter);
  if (IsValidChoice(filter, EmuConfig::kValidVideoFilters))
    c.video_filter = filter;

  // Every stage a filter, not None and not the chain itself, and no more of them than the
  // window shows - or the whole of it is refused, like any other value that is not a choice.
  const std::string chain = f.GetString("filter_chain", c.filter_chain);
  if (IsValidFilterChain(chain))
    c.filter_chain = chain;

  const std::string pacing = f.GetString("frame_pacing", c.frame_pacing);
  if (IsValidChoice(pacing, EmuConfig::kValidFramePacings))
    c.frame_pacing = pacing;

  const std::string type1 =
      f.GetString("controller_type_port1", c.controller_type[0]);
  if (IsValidChoice(type1, EmuConfig::kValidControllerTypes))
    c.controller_type[0] = type1;
  const std::string type2 =
      f.GetString("controller_type_port2", c.controller_type[1]);
  if (IsValidChoice(type2, EmuConfig::kValidControllerTypes))
    c.controller_type[1] = type2;

  const std::string source1 =
      f.GetString("input_source_port1", c.input_source[0]);
  if (IsValidChoice(source1, EmuConfig::kValidInputSources))
    c.input_source[0] = source1;
  const std::string source2 =
      f.GetString("input_source_port2", c.input_source[1]);
  if (IsValidChoice(source2, EmuConfig::kValidInputSources))
    c.input_source[1] = source2;

  for (int port = 0; port < 2; ++port) {
    for (int player = 0; player < 4; ++player) {
      const std::string key = "multitap_port" + std::to_string(port + 1) +
                              "_player_" + std::string(1, char('a' + player)) +
                              "_source";
      const std::string source =
          f.GetString(key.c_str(), c.multitap_player_source[port][player]);
      if (IsValidChoice(source, EmuConfig::kValidInputSources))
        c.multitap_player_source[port][player] = source;
      const std::string type_key = "multitap_port" + std::to_string(port + 1) +
                                   "_player_" + std::string(1, char('a' + player)) +
                                   "_type";
      const std::string type =
          f.GetString(type_key.c_str(), c.multitap_player_type[port][player]);
      if (IsValidChoice(type, EmuConfig::kValidMultitapPlayerTypes))
        c.multitap_player_type[port][player] = type;
    }
  }

  c.frame_limiter = f.GetBool("frame_limiter", c.frame_limiter);
  c.gpu_thread = f.GetBool("gpu_thread", c.gpu_thread);
  const std::string rasteriser = f.GetString("gpu_rasteriser", c.gpu_rasteriser);
  if (IsValidChoice(rasteriser, EmuConfig::kValidGpuRasterisers))
    c.gpu_rasteriser = rasteriser;
  const int scale = f.GetInt("resolution_scale", c.resolution_scale);
  for (int valid : EmuConfig::kValidResolutionScales)
    if (scale == valid)
      c.resolution_scale = scale;
  c.true_color = f.GetBool("true_color", c.true_color);
  c.pgxp_vertices = f.GetBool("pgxp_vertices", c.pgxp_vertices);
  c.pgxp_textures = f.GetBool("pgxp_textures", c.pgxp_textures);
  c.pgxp_culling = f.GetBool("pgxp_culling", c.pgxp_culling);
  c.texture_dump = f.GetBool("texture_dump", c.texture_dump);
  c.texture_replacement = f.GetBool("texture_replacement", c.texture_replacement);
  const std::string texture_filter = f.GetString("texture_filter", c.texture_filter);
  if (IsValidChoice(texture_filter, EmuConfig::kValidTextureFilters))
    c.texture_filter = texture_filter;
  const int budget = f.GetInt("texture_budget_mb", c.texture_budget_mb);
  if (budget >= 64 && budget <= 32768)
    c.texture_budget_mb = budget;
  const int ai_scale = f.GetInt("texture_ai_scale", c.texture_ai_scale);
  if (ai_scale == 0 || ai_scale == 2 || ai_scale == 4)
    c.texture_ai_scale = ai_scale;
  const std::string ai_model = f.GetString("texture_ai_model", c.texture_ai_model);
  if (IsValidChoice(ai_model, EmuConfig::kValidTextureAiModels))
    c.texture_ai_model = ai_model;
  const int ai_share = f.GetInt("texture_ai_share", c.texture_ai_share);
  if (ai_share >= 5 && ai_share <= 100)
    c.texture_ai_share = ai_share;
  c.texture_ai_card = f.GetString("texture_ai_card", c.texture_ai_card);
  const std::string dlss_mode = f.GetString("dlss_mode", c.dlss_mode);
  if (IsValidChoice(dlss_mode, EmuConfig::kValidDlssModes))
    c.dlss_mode = dlss_mode;
  const std::string dlss_preset = f.GetString("dlss_preset", c.dlss_preset);
  if (IsValidChoice(dlss_preset, EmuConfig::kValidDlssPresets))
    c.dlss_preset = dlss_preset;
  const std::string generation = f.GetString("dlss_frame_generation", c.dlss_frame_generation);
  if (IsValidChoice(generation, EmuConfig::kValidDlssFrameGenerations))
    c.dlss_frame_generation = generation;
  const std::string fsr_mode = f.GetString("fsr_mode", c.fsr_mode);
  if (IsValidChoice(fsr_mode, EmuConfig::kValidFsrModes))
    c.fsr_mode = fsr_mode;
  const std::string fsr_version = f.GetString("fsr_version", c.fsr_version);
  if (IsValidChoice(fsr_version, EmuConfig::kValidFsrVersions))
    c.fsr_version = fsr_version;
  const std::string fsr_sharpness = f.GetString("fsr_sharpness", c.fsr_sharpness);
  if (IsValidChoice(fsr_sharpness, EmuConfig::kValidFsrSharpness))
    c.fsr_sharpness = fsr_sharpness;
  c.fsr_frame_generation = f.GetBool("fsr_frame_generation", c.fsr_frame_generation);
  // DLSS and FSR are never both on: a file edited by hand to have both keeps DLSS.
  if (c.dlss_mode != "off" && c.fsr_mode != "off")
    c.fsr_mode = "off";
  c.gpu_transfer_timing = f.GetBool("gpu_transfer_timing", c.gpu_transfer_timing);
  c.icache_timing = f.GetBool("icache_timing", c.icache_timing);
  c.exact_event_timing = f.GetBool("exact_event_timing", c.exact_event_timing);
  c.dma_stops_cpu = f.GetBool("dma_stops_cpu", c.dma_stops_cpu);
  c.measured_bus_timing = f.GetBool("measured_bus_timing", c.measured_bus_timing);
  c.write_queue_timing = f.GetBool("write_queue_timing", c.write_queue_timing);
  c.cdrom_mechanical_timing =
      f.GetBool("cdrom_mechanical_timing", c.cdrom_mechanical_timing);
  c.skip_bios_intro = f.GetBool("skip_bios_intro", c.skip_bios_intro);
  c.recompiler = f.GetBool("recompiler", c.recompiler);
  c.gte_x64 = f.GetBool("gte_x64", c.gte_x64);
  const int run_ahead = f.GetInt("run_ahead", c.run_ahead);
  if (run_ahead >= 0 && run_ahead <= EmuConfig::kMaxRunAhead)
    c.run_ahead = run_ahead;
  const int rewind_interval = f.GetInt("rewind_interval", c.rewind_interval);
  for (int valid : EmuConfig::kValidRewindIntervals)
    if (rewind_interval == valid)
      c.rewind_interval = rewind_interval;
  const int rewind_budget = f.GetInt("rewind_budget_mb", c.rewind_budget_mb);
  for (int valid : EmuConfig::kValidRewindBudgets)
    if (rewind_budget == valid)
      c.rewind_budget_mb = rewind_budget;
  // No list to validate against - what is valid is whatever is in the folder,
  // which only the front end can see. It checks before using it.
  c.bios_file = f.GetString("bios_file", c.bios_file);
  c.hle_bios = f.GetBool("hle_bios", c.hle_bios);

  // Snapped to one of the offered speeds rather than clamped to a range: the
  // menu has six entries and can only tick one of them, so a hand-edited 1.23
  // would leave nothing ticked and no way to tell what was set.
  const float speed = f.GetFloat("emulation_speed", c.emulation_speed);
  float nearest = EmuConfig::kValidSpeeds[0];
  for (float candidate : EmuConfig::kValidSpeeds) {
    if (std::fabs(candidate - speed) < std::fabs(nearest - speed))
      nearest = candidate;
  }
  c.emulation_speed = nearest;

  c.pause_in_menus = f.GetBool("pause_in_menus", c.pause_in_menus);
  c.show_timings = f.GetBool("show_timings", c.show_timings);
  c.show_bios_console = f.GetBool("show_bios_console", c.show_bios_console);
  c.sio1_to_console = f.GetBool("sio1_to_console", c.sio1_to_console);

  const std::string mouse_motion = f.GetString("mouse_motion", c.mouse_motion);
  if (IsValidChoice(mouse_motion, EmuConfig::kValidMouseMotions))
    c.mouse_motion = mouse_motion;

  // Snapped to one the menu offers, the same as the speeds above and for the
  // same reason - a hand-edited 1234 would leave nothing ticked.
  const int dpi = f.GetInt("mouse_dpi", c.mouse_dpi);
  int nearest_dpi = EmuConfig::kValidMouseDpis[0];
  for (int candidate : EmuConfig::kValidMouseDpis) {
    if (std::abs(candidate - dpi) < std::abs(nearest_dpi - dpi))
      nearest_dpi = candidate;
  }
  c.mouse_dpi = nearest_dpi;
}

// The keys a game can keep its own values for - Settings > Emulation's switches
// that are about how a game runs, and what is plugged into each port - which is
// what a game's settings file starts with when separate settings are turned on.
// Not the front end's own preferences (pause_in_menus), nor which device plays
// each port, which follows the person's hardware rather than the game.
//
// A key in a game's file is that game's, whether or not it is listed here, so a
// hand-added `audio_volume` there works too; this is only what the window writes.
inline std::vector<std::string> GameSettingKeys() {
  std::vector<std::string> keys = {
      "recompiler",         "gte_x64",             "icache_timing",       "exact_event_timing",
      // How much of a game's lag run-ahead takes away is the game's (Docs/Rewind-and-Run-Ahead-
      // Plan.md).
      "run_ahead",
      "dma_stops_cpu",      "measured_bus_timing", "write_queue_timing",
      "gpu_thread",         "gpu_transfer_timing", "cdrom_mechanical_timing",
      "skip_bios_intro",    "controller_type_port1", "controller_type_port2",
      // A game the HLE BIOS cannot run yet can keep the real one (Docs/HLE-BIOS.md).
      "hle_bios",
      // A game the hardware rasteriser gets wrong can be kept on software, or at its own
      // resolution (Docs/Hardware-Renderer-Plan.md).
      "gpu_rasteriser",     "resolution_scale",    "true_color",
      "pgxp_vertices",      "pgxp_textures",       "pgxp_culling",
      // ...and textures dumped and replaced, which are a game's own (Docs/Texture-Upscaling-
      // Plan.md).
      "texture_dump",       "texture_replacement", "texture_ai_scale",   "texture_ai_model",
      "texture_filter",
      // ...and DLSS off, or in another mode, for a game it does badly or well
      // (Docs/DLSS-Plan.md).
      "dlss_mode",          "dlss_frame_generation",
      // ...and FSR the same way (Docs/FSR-Plan.md).
      "fsr_mode",           "fsr_frame_generation",
  };
  for (int port = 0; port < 2; ++port) {
    for (int player = 0; player < 4; ++player)
      keys.push_back("multitap_port" + std::to_string(port + 1) + "_player_" +
                     std::string(1, char('a' + player)) + "_type");
  }
  return keys;
}

}
}
