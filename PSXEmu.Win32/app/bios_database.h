#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace psxemu {

    // ---------------------------------------------------------------------------------------------
    // BIOS Database Entry
    // ---------------------------------------------------------------------------------------------
    // You can easily add more BIOS dumps to the dictionary (kBiosDatabase) below.
    //
    // Fields:
    //   md5         - 32-character lowercase hex MD5 checksum (or nullptr/empty if unknown)
    //   name        - Model or identifier (e.g. "SCPH-1001", "SCPH-5502")
    //   version     - Version string (e.g. "v2.2", "v3.0", "v1.0")
    //   date        - Release / build date (e.g. "1995-12-04")
    //   region      - Region (e.g. "North America", "Europe", "Japan", "Asia")
    //   description - Optional notes / hardware info
    // ---------------------------------------------------------------------------------------------
    struct BiosDatabaseEntry {
        const char* md5;
        const char* name;
        const char* version;
        const char* date;
        const char* region;
        const char* description;
    };

    // ---------------------------------------------------------------------------------------------
    // The BIOS Database Dictionary
    // ---------------------------------------------------------------------------------------------
    // Fill this array with more BIOS information. If a file's MD5 matches an entry here,
    // its metadata will be retrieved directly. If an MD5 is not listed, the emulator
    // will fall back to reading the internal System ROM version and kernel headers.
    // ---------------------------------------------------------------------------------------------
    inline constexpr BiosDatabaseEntry kBiosDatabase[] = {
        // Japan (NTSC-J)
        { "239665b1a3dade1b5a52c06338011044", "SCPH-1000", "v1.0", "1994-09-22", "Japan", "Launch NTSC-J, PU-7 board" },
        { "6be57bf701831415fb5f8650a3ecad6e", "SCPH-3000", "v1.1", "1995-01-22", "Japan", "NTSC-J, PU-8 board" },
        { "ad89895c1a84f3eb81a17935ae8a4b5d", "SCPH-3500", "v2.1", "1995-07-17", "Japan", "NTSC-J, PU-8 board" },
        { "026be15d398939b4b0453d10ee9cf91d", "SCPH-5000", "v2.2", "1995-12-04", "Japan", "NTSC-J, PU-8 board" },
        { "8dd7d5296a650fac731f2203c0c5980a", "SCPH-5500", "v3.0", "1996-09-09", "Japan", "NTSC-J, PU-18 board" },
        { "e383187212003c2004ae55eb84860b73", "SCPH-7000", "v4.0", "1997-08-18", "Japan", "NTSC-J DualShock, PU-20 board" },
        { "c53ca5908936d412331790f4426c664a", "SCPH-100",  "v4.3", "2000-03-11", "Japan", "PSone NTSC-J, PM-41 board" },

        // North America (NTSC-U/C)
        { "0266da2b8b9a7102e35a1a1eb7591c0e", "SCPH-1001", "v2.1", "1995-07-17", "North America", "NTSC-U, PU-8 board" },
        { "924e392ed05558ffdb115408c263dccf", "SCPH-1001", "v2.2", "1995-12-04", "North America", "NTSC-U, PU-8 board" },
        { "490f666e1afb15b7362b406ed1cea246", "SCPH-5501", "v3.0", "1996-11-18", "North America", "NTSC-U, PU-18 board" },
        { "1e68c23d3d2635cb6c2341e177368031", "SCPH-7001", "v4.1", "1997-12-16", "North America", "NTSC-U DualShock (also 7501/9001)" },
        { "6e3791a45ae78553da5441551a92e10a", "SCPH-101",  "v4.4", "2000-03-24", "North America", "PSone NTSC-U (early), PM-41 board" },
        { "dc2b9f521f81bf8d442d1c0892b1e4c1", "SCPH-101",  "v4.5", "2000-05-25", "North America", "PSone NTSC-U (late), PM-41 board" },

        // Europe (PAL)
        { "b57917cf7b5a864cc12ee649ff521b7d", "SCPH-1002", "v2.0", "1995-05-10", "Europe", "PAL, PU-8 board" },
        { "96dbf9ee2743818e6e5bdcf94c391ff7", "SCPH-1002", "v2.1", "1995-07-17", "Europe", "PAL, PU-8 board" },
        { "310ff94b150c7cd768a4b6480e0fa7a1", "SCPH-1002", "v2.2", "1995-12-04", "Europe", "PAL, PU-8 board" },
        { "32736f17079d0b2b7024407c39bd3050", "SCPH-5502", "v3.0", "1997-01-06", "Europe", "PAL, PU-18 board (also 5552)" },
        { "26be72cfb6962d85340eb930510fe208", "SCPH-7002", "v4.1", "1997-12-16", "Europe", "PAL DualShock (also 7502/9002)" },
        { "f0ebbf60a9f5d166946654a938c218dd", "SCPH-102",  "v4.4", "2000-03-24", "Europe", "PSone PAL (early), PM-41 board" },
        { "51a02100868f044dbbf1f531aa778943", "SCPH-102",  "v4.5", "2000-05-25", "Europe", "PSone PAL (late), PM-41 board" },

        // Asia (NTSC)
        { "490f666e1afb15b7362b406ed1cea246", "SCPH-7003", "v3.0", "1996-11-18", "Asia", "NTSC English, PU-20 board (also 5003/7503)" },

        // Development / Debugging (DTL-H)
        { "8b50f7ff19904996453fa8ec0cf82dd7", "DTL-H1001", "v2.0", "1995-05-07", "North America", "DTL-H1001 Dev" },
        { "c379bf4a9235cbcc4399e0fffc4c0ca9", "DTL-H1100", "v2.2", "1996-03-06", "Debug", "DTL-H1100 Debug green console" },
        { "026be15d398939b4b0453d10ee9cf91d", "DTL-H1200", "v2.2", "1995-12-04", "Japan", "DTL-H1200 Dev" },
        { "924e392ed05558ffdb115408c263dccf", "DTL-H1201", "v2.2", "1995-12-04", "North America", "DTL-H1201 / DTL-H3001 Dev" },
        { "310ff94b150c7cd768a4b6480e0fa7a1", "DTL-H1202", "v2.2", "1995-12-04", "Europe", "DTL-H1202 / DTL-H3002 Dev" },
    };

    // ---------------------------------------------------------------------------------------------
    // Resolved BIOS Information
    // ---------------------------------------------------------------------------------------------
    struct BiosInfo {
        std::string filename;
        std::string full_path;
        std::string md5;
        std::string name;
        std::string version;
        std::string date;
        std::string region;
        std::string description;
        std::string rom_string;
        bool matched_by_md5 = false;
        bool matched_by_rom = false;

        std::wstring FormatMenuLabel() const;
    };

    // ---------------------------------------------------------------------------------------------
    // MD5 Implementation (RFC 1321)
    // ---------------------------------------------------------------------------------------------
    namespace detail {

        inline uint32_t Rotl(uint32_t x, uint32_t n) {
            return (x << n) | (x >> (32 - n));
        }

        inline std::string ComputeMd5(const uint8_t* data, size_t len) {
            uint32_t h0 = 0x67452301;
            uint32_t h1 = 0xefcdab89;
            uint32_t h2 = 0x98badcfe;
            uint32_t h3 = 0x10325476;

            static constexpr uint32_t k[64] = {
                0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
                0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
                0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
                0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
                0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
                0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
                0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
                0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
                0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
                0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
                0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
                0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
                0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
                0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
                0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
                0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
            };

            static constexpr uint32_t s[64] = {
                7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
                5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
                4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
                6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
            };

            const size_t padded_len = ((len + 8) / 64 + 1) * 64;
            std::vector<uint8_t> msg(padded_len, 0);
            if (len > 0 && data != nullptr)
                std::memcpy(msg.data(), data, len);
            msg[len] = 0x80;
            const uint64_t bit_len = static_cast<uint64_t>(len) * 8;
            for (int i = 0; i < 8; ++i) {
                msg[padded_len - 8 + i] = static_cast<uint8_t>(bit_len >> (i * 8));
            }

            for (size_t offset = 0; offset < padded_len; offset += 64) {
                uint32_t w[16];
                for (int i = 0; i < 16; ++i) {
                    w[i] = static_cast<uint32_t>(msg[offset + i * 4]) |
                           (static_cast<uint32_t>(msg[offset + i * 4 + 1]) << 8) |
                           (static_cast<uint32_t>(msg[offset + i * 4 + 2]) << 16) |
                           (static_cast<uint32_t>(msg[offset + i * 4 + 3]) << 24);
                }

                uint32_t a = h0, b = h1, c = h2, d = h3;
                for (int i = 0; i < 64; ++i) {
                    uint32_t f, g;
                    if (i < 16) {
                        f = (b & c) | ((~b) & d);
                        g = i;
                    } else if (i < 32) {
                        f = (d & b) | ((~d) & c);
                        g = (5 * i + 1) % 16;
                    } else if (i < 48) {
                        f = b ^ c ^ d;
                        g = (3 * i + 5) % 16;
                    } else {
                        f = c ^ (b | (~d));
                        g = (7 * i) % 16;
                    }
                    const uint32_t temp = d;
                    d = c;
                    c = b;
                    b = b + Rotl(a + f + k[i] + w[g], s[i]);
                    a = temp;
                }
                h0 += a;
                h1 += b;
                h2 += c;
                h3 += d;
            }

            char hex[33] = { 0 };
            const uint32_t h[4] = { h0, h1, h2, h3 };
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) {
                    const uint8_t byte = static_cast<uint8_t>((h[i] >> (j * 8)) & 0xFF);
                    static const char kHexChars[] = "0123456789abcdef";
                    hex[(i * 4 + j) * 2] = kHexChars[(byte >> 4) & 0x0F];
                    hex[(i * 4 + j) * 2 + 1] = kHexChars[byte & 0x0F];
                }
            }
            return std::string(hex, 32);
        }

        inline std::vector<uint8_t> ReadFileBytes(const std::string& path, size_t max_bytes = 0x80000) {
            std::vector<uint8_t> buffer;
            FILE* fp = fopen(path.c_str(), "rb");
            if (!fp)
                return buffer;
            fseek(fp, 0, SEEK_END);
            const long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (size > 0) {
                size_t to_read = static_cast<size_t>(size);
                if (max_bytes > 0 && to_read > max_bytes)
                    to_read = max_bytes;
                buffer.resize(to_read);
                const size_t read_bytes = fread(buffer.data(), 1, to_read, fp);
                buffer.resize(read_bytes);
            }
            fclose(fp);
            return buffer;
        }

        struct ParsedRomMetadata {
            bool has_rom_string = false;
            std::string raw_string;
            std::string version;
            std::string date;
            std::string region;
        };

        inline ParsedRomMetadata ParseRomMetadata(const uint8_t* data, size_t size) {
            ParsedRomMetadata result;
            if (data == nullptr || size < 0x80000)
                return result;

            // 1. Check System ROM Version string at offset 0x7FF32
            static const size_t kRomStringOffset = 0x7FF32;
            static const char kPrefix[] = "System ROM Version ";
            static const size_t kPrefixLen = 19;

            if (size >= kRomStringOffset + kPrefixLen &&
                memcmp(data + kRomStringOffset, kPrefix, kPrefixLen) == 0) {

                result.has_rom_string = true;
                const size_t max_len = std::min(static_cast<size_t>(64), size - kRomStringOffset);
                size_t len = 0;
                while (len < max_len && data[kRomStringOffset + len] != 0 &&
                       data[kRomStringOffset + len] != '\r' && data[kRomStringOffset + len] != '\n') {
                    len++;
                }
                result.raw_string =
                    std::string(reinterpret_cast<const char*>(data + kRomStringOffset), len);

                // Format: "System ROM Version <version> <date> <region>"
                // Example: "System ROM Version 2.2 12/04/95 A"
                const std::string remainder = result.raw_string.substr(kPrefixLen);
                std::istringstream iss(remainder);
                std::string ver, dt, reg;
                if (iss >> ver) {
                    result.version = "v" + ver;
                }
                if (iss >> dt) {
                    result.date = dt;
                }
                if (iss >> reg && !reg.empty()) {
                    const char r = static_cast<char>(toupper(reg[0]));
                    if (r == 'A')
                        result.region = "North America";
                    else if (r == 'E')
                        result.region = "Europe";
                    else if (r == 'J')
                        result.region = "Japan";
                    else if (r == 'D')
                        result.region = "Debug";
                    else if (r == 'T')
                        result.region = "PlayStation 2";
                    else if (r == 'W')
                        result.region = "Region-Free";
                    else
                        result.region = std::string(1, r);
                }
            } else if (size >= 0x140) {
                // 2. Fallback: check Kernel Header at 0x100 and 0x108
                // For SCPH-1000 / v1.0 launch:
                const uint8_t d0 = data[0x100], d1 = data[0x101], d2 = data[0x102], d3 = data[0x103];
                char bcd_date[12] = { 0 };
                snprintf(bcd_date, sizeof(bcd_date), "%02X%02X-%02X-%02X", d3, d2, d1, d0);

                const std::string header_str(reinterpret_cast<const char*>(data + 0x108), 0x38);
                if (header_str.find("CEX-1000") != std::string::npos) {
                    result.has_rom_string = true;
                    result.raw_string = "CEX-1000 KT-3 (v1.0 1994-09-22)";
                    result.version = "v1.0";
                    result.date = "1994-09-22";
                    result.region = "Japan";
                }
            }

            return result;
        }

    } // namespace detail

    // ---------------------------------------------------------------------------------------------
    // Identify BIOS by MD5 and ROM Version
    // ---------------------------------------------------------------------------------------------
    inline BiosInfo IdentifyBios(const std::string& full_path, const std::string& filename) {
        BiosInfo info;
        info.filename = filename;
        info.full_path = full_path;

        const std::vector<uint8_t> data = detail::ReadFileBytes(full_path, 0x80000);
        if (data.size() == 0x80000) {
            info.md5 = detail::ComputeMd5(data.data(), data.size());
        }

        // 1. Look up in kBiosDatabase by MD5 checksum
        if (!info.md5.empty()) {
            for (const auto& entry : kBiosDatabase) {
                if (entry.md5 != nullptr && entry.md5[0] != '\0' &&
                    _stricmp(entry.md5, info.md5.c_str()) == 0) {
                    info.name = entry.name ? entry.name : "";
                    info.version = entry.version ? entry.version : "";
                    info.date = entry.date ? entry.date : "";
                    info.region = entry.region ? entry.region : "";
                    info.description = entry.description ? entry.description : "";
                    info.matched_by_md5 = true;

                    // If multiple entries share the same hash (e.g. SCPH-5501 vs SCPH-7003),
                    // prioritize the one whose name or number is mentioned in the filename.
                    if (!filename.empty() && !info.name.empty() &&
                        filename.find(info.name) != std::string::npos) {
                        break;
                    }
                }
            }
        }

        // 2. Parse internal System ROM version / date / region from ROM binary
        const detail::ParsedRomMetadata rom = detail::ParseRomMetadata(data.data(), data.size());
        if (rom.has_rom_string) {
            info.rom_string = rom.raw_string;
            info.matched_by_rom = true;

            // If not matched by MD5, populate fields directly from ROM headers
            if (!info.matched_by_md5) {
                if (!rom.version.empty())
                    info.version = rom.version;
                if (!rom.date.empty())
                    info.date = rom.date;
                if (!rom.region.empty())
                    info.region = rom.region;

                // Look up in database by version + region to suggest the model name
                for (const auto& entry : kBiosDatabase) {
                    if (entry.version != nullptr && entry.region != nullptr &&
                        _stricmp(entry.version, info.version.c_str()) == 0 &&
                        _stricmp(entry.region, info.region.c_str()) == 0) {
                        info.name = entry.name ? entry.name : "";
                        if (info.description.empty() && entry.description)
                            info.description = entry.description;
                        break;
                    }
                }
            }
        }

        // 3. Fallback heuristics: Sony SCPH model numbering conventions
        if (info.region.empty()) {
            std::string upper_fn = filename;
            for (char& c : upper_fn)
                c = static_cast<char>(toupper(c));

            const size_t scph_pos = upper_fn.find("SCPH");
            if (scph_pos != std::string::npos) {
                size_t num_start = scph_pos + 4;
                while (num_start < upper_fn.size() &&
                       (upper_fn[num_start] == '-' || upper_fn[num_start] == '_')) {
                    num_start++;
                }
                size_t num_end = num_start;
                while (num_end < upper_fn.size() && isdigit(upper_fn[num_end])) {
                    num_end++;
                }
                if (num_end > num_start) {
                    const char last_digit = upper_fn[num_end - 1];
                    if (last_digit == '0')
                        info.region = "Japan";
                    else if (last_digit == '1')
                        info.region = "North America";
                    else if (last_digit == '2')
                        info.region = "Europe";
                    else if (last_digit == '3')
                        info.region = "Asia";

                    if (info.name.empty())
                        info.name = "SCPH-" + upper_fn.substr(num_start, num_end - num_start);
                }
            }
        }

        return info;
    }

    // ---------------------------------------------------------------------------------------------
    // Format Menu Label
    // ---------------------------------------------------------------------------------------------
    inline std::wstring BiosInfo::FormatMenuLabel() const {
        std::wstring label;
        if (filename.empty())
            return label;

        // Filename
        const int fn_size = MultiByteToWideChar(CP_ACP, 0, filename.c_str(), -1, nullptr, 0);
        if (fn_size > 1) {
            label.resize(static_cast<size_t>(fn_size - 1));
            MultiByteToWideChar(CP_ACP, 0, filename.c_str(), -1, &label[0], fn_size);
        }

        // Build metadata tags: [Model · Region · Version · Date]
        std::vector<std::string> tags;
        if (!name.empty()) {
            tags.push_back(name);
        }
        if (!region.empty()) {
            tags.push_back(region);
        }
        if (!version.empty()) {
            tags.push_back(version);
        }
        if (!date.empty()) {
            tags.push_back(date);
        }

        if (!tags.empty()) {
            label += L"  [";
            for (size_t i = 0; i < tags.size(); ++i) {
                if (i > 0)
                    label += L" \x00B7 "; // middle dot (·)
                const int tag_size =
                    MultiByteToWideChar(CP_ACP, 0, tags[i].c_str(), -1, nullptr, 0);
                if (tag_size > 1) {
                    std::wstring wide_tag(static_cast<size_t>(tag_size - 1), L'\0');
                    MultiByteToWideChar(CP_ACP, 0, tags[i].c_str(), -1, &wide_tag[0], tag_size);
                    label += wide_tag;
                }
            }
            label += L"]";
        }

        return label;
    }

} // namespace psxemu
