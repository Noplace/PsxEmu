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

// Which discs belong together. A multi-disc game has a serial per disc - Final Fantasy VII is
// SCUS-94163, -94164 and -94165 - so nothing on the disc says the three are one game. Their file
// names do: "(Disc 1)", "CD2", "Disc 3 of 3". DiscSetTitle takes that marker off, and what is
// left is the same for every disc of the set.

#include <cctype>
#include <cstdlib>
#include <string>

namespace psxemu {

    // The title a disc image shares with the other discs of its set - "Final Fantasy VII" for
    // "Final Fantasy VII (Disc 2) [SCUS-94164].cue" - or empty if its name marks no disc number.
    // A marker is "disc", "disk" or "cd", then an optional space, dash or underscore, then a digit,
    // at the start of a word; "of N" after it goes too. Anything in brackets or parentheses goes,
    // as TitleFromPath drops it, and so do the spaces and dashes left at the ends.
    // `disc`, if given, gets which disc of the set it is - 0 when it is none.
    inline std::string DiscSetTitle(const std::string& path, int* disc = nullptr) {
        if (disc != nullptr)
            *disc = 0;
        const size_t slash = path.find_last_of("/\\");
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0)
            name.erase(dot);

        auto lower = [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); };
        auto word_start = [&name](size_t i) {
            return i == 0 || !isalnum(static_cast<unsigned char>(name[i - 1]));
        };
        bool found = false;
        for (size_t i = 0; i < name.size() && !found; ++i) {
            if (!word_start(i))
                continue;
            size_t j = i;
            for (const char* word : { "disc", "disk", "cd" }) {
                size_t k = 0;
                while (word[k] != '\0' && i + k < name.size() && lower(name[i + k]) == word[k])
                    ++k;
                if (word[k] == '\0') {
                    j = i + k;
                    break;
                }
            }
            if (j == i)
                continue;
            if (j < name.size() && (name[j] == ' ' || name[j] == '-' || name[j] == '_'))
                ++j;
            if (j >= name.size() || !isdigit(static_cast<unsigned char>(name[j])))
                continue;
            size_t end = j;
            while (end < name.size() && isdigit(static_cast<unsigned char>(name[end])))
                ++end;
            // Not the start of something longer - "CD1" is a marker, "CD1X" is not.
            if (end < name.size() && isalpha(static_cast<unsigned char>(name[end])))
                continue;
            // " of 3"
            size_t of = end;
            while (of < name.size() && name[of] == ' ')
                ++of;
            if (of + 2 < name.size() && lower(name[of]) == 'o' && lower(name[of + 1]) == 'f') {
                size_t n = of + 2;
                while (n < name.size() && name[n] == ' ')
                    ++n;
                if (n < name.size() && isdigit(static_cast<unsigned char>(name[n]))) {
                    while (n < name.size() && isdigit(static_cast<unsigned char>(name[n])))
                        ++n;
                    end = n;
                }
            }
            if (disc != nullptr)
                *disc = atoi(name.substr(j, end - j).c_str());
            name.erase(i, end - i);
            found = true;
        }
        if (!found)
            return std::string();

        // Brackets and what is in them, underscores as spaces, and the ends tidied - which is
        // also what "()" left behind by "(Disc 1)" goes with.
        std::string out;
        int depth = 0;
        for (char c : name) {
            if (c == '[' || c == '(')
                ++depth;
            else if ((c == ']' || c == ')') && depth > 0)
                --depth;
            else if (depth == 0)
                out += (c == '_') ? ' ' : c;
        }
        auto separator = [](char c) { return c == ' ' || c == '-' || c == '.' || c == ','; };
        while (!out.empty() && separator(out.back()))
            out.pop_back();
        while (!out.empty() && separator(out.front()))
            out.erase(out.begin());
        // Runs of spaces left where the marker was, down to one.
        std::string tidy;
        for (char c : out) {
            if (!(c == ' ' && !tidy.empty() && tidy.back() == ' '))
                tidy += c;
        }
        return tidy;
    }

}   // namespace psxemu
