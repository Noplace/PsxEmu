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

// Every controller binding the front end has: for each place a pad can be played (a port, or one
// of a multitap's players - kBindingSlots) and each device that can play it (the keyboard, or one
// of four XInput pads - kInputSourceChoices), which control presses which pad button.
//
// One map per slot *and* device, so Port 1 and Port 2 can both be on the keyboard with different
// keys - two players on one keyboard - and a pad can be laid out differently for each port it
// plays. The UI thread owns a ControllerBindings and edits it (controller_bindings_window.h); the
// machine's thread gets a copy whenever it changes, and maps the input thread's raw reading
// through it once a frame (App::ApplyInput).
//
// In psxemu.ini, Port 1 on the keyboard is the key_* settings it has always been. Every other map
// is one line, bind_<slot>_<device>, written only when it differs from the default, so the file
// grows by a line per map someone actually changed.

#include "framework.h"
#include "const.h"
#include "keyboard.h"
#include "win32_paths.h"   // Widen
#include "platform/input_bindings.h"

namespace psxemu {

    inline constexpr int kBindingDevices = static_cast<int>(std::size(kInputSourceChoices));
    inline constexpr int kKeyboardDevice = 0;   // kInputSourceChoices[0]

    struct ControllerBindings {
        KeyMap map[kBindingSlotCount][kBindingDevices];

        ControllerBindings() {
            for (int slot = 0; slot < kBindingSlotCount; ++slot) {
                for (int device = 0; device < kBindingDevices; ++device)
                    map[slot][device] = Default(device);
            }
        }

        static KeyMap Default(int device) {
            return device == kKeyboardDevice ? DefaultKeyMap() : DefaultPadMap();
        }
    };

    // ---------------------------------------------------------------------------------------------
    // Names
    // ---------------------------------------------------------------------------------------------

    // A button as a bind_ line names it: its key_* setting without the "key_".
    inline const char* const* BindingButtonNames() {
        static const std::array<const char*, kPadButtons> names = [] {
            std::array<const char*, kPadButtons> out = {};
            for (int i = 0; i < kPadButtons; ++i)
                out[i] = kKeyBindings[i].setting + 4;
            return out;
        }();
        return names.data();
    }

    inline std::string BindingCodeName(int device, int code) {
        return device == kKeyboardDevice ? KeyName(code) : utilities::PadInputKey(code);
    }

    inline int BindingCodeFromName(int device, const std::string& name) {
        return device == kKeyboardDevice ? KeyFromName(name) : utilities::PadInputFromKey(name);
    }

    // What the bindings window shows for a code.
    inline std::wstring BindingCodeLabel(int device, int code) {
        if (code == 0)
            return L"(none)";
        return Widen(device == kKeyboardDevice ? KeyName(code) : utilities::PadInputLabel(code));
    }

    inline std::string BindingSettingKey(int slot, int device) {
        return std::string("bind_") + kBindingSlots[slot].key + "_" + kInputSourceChoices[device].key;
    }

    // ---------------------------------------------------------------------------------------------
    // The settings file
    // ---------------------------------------------------------------------------------------------

    // A settings file written before bindings were per port has only key_*, which both ports read
    // when they were on the keyboard. Until bindings_version says otherwise, every slot's keyboard
    // starts from those keys, so someone who changed them and plays Port 2 on the keyboard keeps
    // what they had.
    inline constexpr int kBindingsVersion = 2;

    inline void LoadBindings(const emulation::psx::SettingsFile& settings,
                             ControllerBindings* bindings) {
        *bindings = ControllerBindings();
        // A button missing from the file keeps its default; one present but empty stays unbound -
        // clearing a key has to survive a restart.
        KeyMap& port1_keys = bindings->map[0][kKeyboardDevice];
        for (int i = 0; i < kPadButtons; ++i) {
            port1_keys[i] = KeyFromName(
                settings.GetString(kKeyBindings[i].setting, KeyName(kKeyBindings[i].key)));
        }
        const bool before_per_port = settings.GetInt("bindings_version", 0) < kBindingsVersion;
        for (int slot = 0; slot < kBindingSlotCount; ++slot) {
            for (int device = 0; device < kBindingDevices; ++device) {
                if (slot == 0 && device == kKeyboardDevice)
                    continue;
                KeyMap& map = bindings->map[slot][device];
                if (before_per_port && device == kKeyboardDevice)
                    map = port1_keys;
                const std::string line =
                    settings.GetString(BindingSettingKey(slot, device).c_str(), std::string());
                if (line.empty())
                    continue;
                utilities::ParseMap(line, BindingButtonNames(), kPadButtons,
                                    [device](const std::string& name) {
                                        return BindingCodeFromName(device, name);
                                    },
                                    map.data());
            }
        }
    }

    inline void StoreBindings(emulation::psx::SettingsFile* settings,
                              const ControllerBindings& bindings) {
        const KeyMap& port1_keys = bindings.map[0][kKeyboardDevice];
        for (int i = 0; i < kPadButtons; ++i)
            settings->SetString(kKeyBindings[i].setting, KeyName(port1_keys[i]));
        for (int slot = 0; slot < kBindingSlotCount; ++slot) {
            for (int device = 0; device < kBindingDevices; ++device) {
                if (slot == 0 && device == kKeyboardDevice)
                    continue;
                const KeyMap& map = bindings.map[slot][device];
                const std::string key = BindingSettingKey(slot, device);
                if (map == ControllerBindings::Default(device)) {
                    settings->Remove(key.c_str());
                    continue;
                }
                settings->SetString(key.c_str(),
                                    utilities::SerialiseMap(map.data(), BindingButtonNames(),
                                                            kPadButtons, [device](int code) {
                                                                return BindingCodeName(device, code);
                                                            }));
            }
        }
        settings->SetInt("bindings_version", kBindingsVersion);
    }

    // ---------------------------------------------------------------------------------------------
    // Mapping, on the machine's thread
    // ---------------------------------------------------------------------------------------------

    inline const uint32_t* BindingButtonBits() {
        static const std::array<uint32_t, kPadButtons> bits = [] {
            std::array<uint32_t, kPadButtons> out = {};
            for (int i = 0; i < kPadButtons; ++i)
                out[i] = kKeyBindings[i].button;
            return out;
        }();
        return bits.data();
    }

    // The pad buttons, and kAnalogKey above them, that the keys held press through `map`.
    inline uint32_t MapKeyboard(const KeyMap& map, const uint32_t keys[8]) {
        return utilities::MapKeys(map.data(), BindingButtonBits(), kPadButtons, keys);
    }

    // The same for a pad. The left stick also works as the d-pad, as it always has - except in a
    // direction some button is bound to, where it presses only that button.
    inline uint32_t MapGamepad(const KeyMap& map, uint32_t inputs) {
        using namespace utilities;
        uint32_t buttons = MapPad(map.data(), BindingButtonBits(), kPadButtons, inputs);
        uint32_t bound = 0;
        for (int i = 0; i < kPadButtons; ++i)
            bound |= PadInputBit(map[i]);
        // clang-format off
        static constexpr struct { int input; uint32_t button; } kStickAsDpad[] = {
            { kPadLStickUp,    emulation::psx::Sio::kUp },
            { kPadLStickDown,  emulation::psx::Sio::kDown },
            { kPadLStickLeft,  emulation::psx::Sio::kLeft },
            { kPadLStickRight, emulation::psx::Sio::kRight },
        };
        // clang-format on
        for (const auto& direction : kStickAsDpad) {
            const uint32_t bit = PadInputBit(direction.input);
            if ((inputs & bit) != 0 && (bound & bit) == 0)
                buttons |= direction.button;
        }
        return buttons;
    }

    // Every key some keyboard map uses: the ones the input thread has to read.
    inline std::array<bool, 256> KeysInUse(const ControllerBindings& bindings) {
        std::array<bool, 256> used = {};
        for (int slot = 0; slot < kBindingSlotCount; ++slot) {
            for (const int key : bindings.map[slot][kKeyboardDevice]) {
                if (key > 0 && key < 256)
                    used[key] = true;
            }
        }
        return used;
    }

}   // namespace psxemu
