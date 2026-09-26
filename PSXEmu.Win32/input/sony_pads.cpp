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
#include "input/sony_pads.h"

#include "platform/sony_pad_reports.h"

#include <algorithm>
#include <cwctype>
#include <string>

#include <hidsdi.h>
#include <setupapi.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

namespace psxemu {

    using emulation::host::HostInput;
    using emulation::host::PadKind;
    using emulation::host::PadReading;

    namespace {

        const USHORT kSonyVendor = 0x054C;

        // Which pads, by product id. The DualShock 4's wireless adapter (0x0BA0) is left out: it
        // reports all the time, pad or no pad, and nothing in its report says reliably which.
        PadKind KindOf(USHORT product, bool* known) {
            *known = true;
            switch (product) {
                case 0x05C4:   // DualShock 4, first model
                case 0x09CC:   // DualShock 4, second model
                    return PadKind::kDualShock4;
                case 0x0CE6:   // DualSense
                case 0x0DF2:   // DualSense Edge
                    return PadKind::kDualSense;
                default:
                    *known = false;
                    return PadKind::kXInput;
            }
        }

    }   // namespace

    struct SonyPads::Pad {
        std::wstring path;
        HANDLE handle = INVALID_HANDLE_VALUE;
        PadKind kind = PadKind::kDualShock4;
        bool bluetooth = false;
        int slot = -1;

        std::vector<uint8_t> in;
        OVERLAPPED read = {};
        bool reading = false;

        std::vector<uint8_t> out;
        OVERLAPPED write = {};
        bool writing = false;
        uint8_t output_sequence = 0;   // the DualSense's, over Bluetooth

        PadReading state;
        uint8_t small_motor = 0, large_motor = 0;   // asked for
        bool rumble_dirty = false;
        bool failed = false;
    };

    SonyPads::SonyPads() = default;

    SonyPads::~SonyPads() {
        for (auto& pad : pads_)
            Close(*pad);
    }

    // ---------------------------------------------------------------------------------------------
    // Finding pads
    // ---------------------------------------------------------------------------------------------

    void SonyPads::Scan() {
        GUID hid;
        HidD_GetHidGuid(&hid);
        HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE)
            return;
        SP_DEVICE_INTERFACE_DATA iface = { sizeof(iface) };
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
            if (size == 0)
                continue;
            std::vector<uint8_t> buffer(size);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr))
                continue;
            const std::wstring path = detail->DevicePath;
            // Sony's vendor id is in every one of its pads' paths - "vid_054c" over USB,
            // "vid&0002054c" over Bluetooth - so nothing else needs opening to rule it out.
            std::wstring lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (lower.find(L"054c") == std::wstring::npos)
                continue;
            const bool open = std::any_of(pads_.begin(), pads_.end(),
                                          [&path](const auto& pad) { return pad->path == path; });
            if (!open)
                Open(path);
        }
        SetupDiDestroyDeviceInfoList(set);
    }

    void SonyPads::Open(const std::wstring& path) {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return;   // gone already, or held exclusively by something else

        HIDD_ATTRIBUTES attributes = { sizeof(attributes) };
        bool known = false;
        PadKind kind = PadKind::kXInput;
        if (HidD_GetAttributes(handle, &attributes) && attributes.VendorID == kSonyVendor)
            kind = KindOf(attributes.ProductID, &known);
        HIDP_CAPS caps = {};
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (known && HidD_GetPreparsedData(handle, &preparsed)) {
            if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS)
                known = false;
            HidD_FreePreparsedData(preparsed);
        } else {
            known = false;
        }
        // The pad itself - a gamepad or joystick - not another of the device's collections.
        if (!known || caps.UsagePage != 0x01 || (caps.Usage != 0x05 && caps.Usage != 0x04) ||
            caps.InputReportByteLength < 10) {
            CloseHandle(handle);
            return;
        }

        auto pad = std::make_unique<Pad>();
        pad->path = path;
        pad->handle = handle;
        pad->kind = kind;
        // Over USB every report both pads send is 64 bytes; over Bluetooth the longest is more.
        // The path says so too: Bluetooth HID devices carry the HID service's UUID.
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        pad->bluetooth = caps.InputReportByteLength > 64 ||
                         lower.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos;
        pad->in.assign(caps.InputReportByteLength, 0);
        // Windows wants every output report written at exactly the length of the longest one. A
        // pad whose longest is too short for the report that sets its motors just never rumbles.
        const size_t needed =
            utilities::SonyRumbleReportLength(kind == PadKind::kDualSense, pad->bluetooth);
        if (caps.OutputReportByteLength >= needed)
            pad->out.assign(caps.OutputReportByteLength, 0);
        pad->read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pad->write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pad->state.connected = true;
        pad->state.kind = kind;
        pads_.push_back(std::move(pad));
    }

    void SonyPads::Close(Pad& pad) {
        if (pad.handle != INVALID_HANDLE_VALUE) {
            // Nothing may still be writing into the buffers once they are freed.
            if (pad.reading || pad.writing) {
                CancelIoEx(pad.handle, nullptr);
                DWORD ignored = 0;
                if (pad.reading)
                    GetOverlappedResult(pad.handle, &pad.read, &ignored, TRUE);
                if (pad.writing)
                    GetOverlappedResult(pad.handle, &pad.write, &ignored, TRUE);
            }
            CloseHandle(pad.handle);
            pad.handle = INVALID_HANDLE_VALUE;
        }
        pad.reading = pad.writing = false;
        if (pad.read.hEvent != nullptr)
            CloseHandle(pad.read.hEvent);
        if (pad.write.hEvent != nullptr)
            CloseHandle(pad.write.hEvent);
        pad.read.hEvent = pad.write.hEvent = nullptr;
    }

    // ---------------------------------------------------------------------------------------------
    // Reading
    // ---------------------------------------------------------------------------------------------

    void SonyPads::Poll(const std::array<bool, HostInput::kPads>& taken,
                        PadReading (&pads)[HostInput::kPads]) {
        if (rescan_) {
            rescan_ = false;
            Scan();
        }

        for (auto& pad : pads_) {
            Read(*pad);
            if (pad->writing) {
                DWORD written = 0;
                if (GetOverlappedResult(pad->handle, &pad->write, &written, FALSE) ||
                    GetLastError() != ERROR_IO_INCOMPLETE)
                    pad->writing = false;
            }
            if (!pad->failed && pad->rumble_dirty && !pad->writing && !pad->out.empty())
                WriteRumble(*pad);
        }
        // A pad that stopped answering has been unplugged, or has dropped off Bluetooth.
        for (auto it = pads_.begin(); it != pads_.end();) {
            if ((*it)->failed) {
                Close(**it);
                it = pads_.erase(it);
            } else {
                ++it;
            }
        }

        // Slots: an XInput pad's claim wins, then each pad keeps the one it has.
        std::array<bool, HostInput::kPads> used = taken;
        auto stop_motors = [](Pad& pad) {
            if (pad.small_motor != 0 || pad.large_motor != 0) {
                pad.small_motor = pad.large_motor = 0;
                pad.rumble_dirty = true;
            }
        };
        for (auto& pad : pads_) {
            if (pad->slot >= 0 && used[pad->slot]) {
                // An XInput pad has arrived in its slot. Whatever that slot's port was asking
                // for is the XInput pad's now.
                pad->slot = -1;
                stop_motors(*pad);
            }
            if (pad->slot >= 0)
                used[pad->slot] = true;
        }
        for (auto& pad : pads_) {
            if (pad->slot >= 0)
                continue;
            for (int i = 0; i < HostInput::kPads; ++i) {
                if (!used[i]) {
                    pad->slot = i;
                    used[i] = true;
                    stop_motors(*pad);
                    break;
                }
            }
        }
        for (auto& pad : pads_) {
            if (pad->slot >= 0)
                pads[pad->slot] = pad->state;
        }
    }

    void SonyPads::Read(Pad& pad) {
        // Every report that has arrived since the last poll, the last of them winning. A pad sends
        // one every few milliseconds, faster than this is called, so there is usually one waiting.
        for (int reports = 0; reports < 64; ++reports) {
            if (!pad.reading) {
                ResetEvent(pad.read.hEvent);
                DWORD length = 0;
                if (ReadFile(pad.handle, pad.in.data(), static_cast<DWORD>(pad.in.size()), &length,
                             &pad.read)) {
                    Parse(pad, pad.in.data(), length);
                    continue;
                }
                if (GetLastError() != ERROR_IO_PENDING) {
                    pad.failed = true;
                    return;
                }
                pad.reading = true;
            }
            DWORD length = 0;
            if (!GetOverlappedResult(pad.handle, &pad.read, &length, FALSE)) {
                if (GetLastError() != ERROR_IO_INCOMPLETE)
                    pad.failed = true;
                return;
            }
            pad.reading = false;
            Parse(pad, pad.in.data(), length);
        }
    }

    // One input report; ParseSonyPadReport knows the layouts.
    void SonyPads::Parse(Pad& pad, const uint8_t* report, size_t length) {
        utilities::SonyPadState state;
        if (!utilities::ParseSonyPadReport(pad.kind == PadKind::kDualSense, pad.bluetooth, report,
                                           length, &state))
            return;
        pad.state.inputs = state.inputs;
        pad.state.left_x = state.left_x;
        pad.state.left_y = state.left_y;
        pad.state.right_x = state.right_x;
        pad.state.right_y = state.right_y;
    }

    // ---------------------------------------------------------------------------------------------
    // Rumble
    // ---------------------------------------------------------------------------------------------

    void SonyPads::SetRumble(int slot, uint8_t small_motor, uint8_t large_motor) {
        for (auto& pad : pads_) {
            if (pad->slot != slot)
                continue;
            if (pad->small_motor != small_motor || pad->large_motor != large_motor) {
                pad->small_motor = small_motor;
                pad->large_motor = large_motor;
                pad->rumble_dirty = true;
            }
            return;
        }
    }

    // The motors only - see BuildSonyRumbleReport - at the length Windows wants.
    void SonyPads::WriteRumble(Pad& pad) {
        if (!utilities::BuildSonyRumbleReport(pad.kind == PadKind::kDualSense, pad.bluetooth,
                                              pad.output_sequence, pad.small_motor,
                                              pad.large_motor, pad.out.data(), pad.out.size()))
            return;
        if (pad.kind == PadKind::kDualSense && pad.bluetooth)
            pad.output_sequence = static_cast<uint8_t>(pad.output_sequence + 1);

        pad.rumble_dirty = false;
        ResetEvent(pad.write.hEvent);
        if (WriteFile(pad.handle, pad.out.data(), static_cast<DWORD>(pad.out.size()), nullptr,
                      &pad.write))
            return;
        if (GetLastError() == ERROR_IO_PENDING)
            pad.writing = true;
        // Any other failure is left to the next read to notice; the motors just stay as they were.
    }

}   // namespace psxemu
