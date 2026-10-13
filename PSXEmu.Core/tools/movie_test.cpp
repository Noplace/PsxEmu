// movie_test - checks psx/movie.h: reading a BizHawk .bk2 and playing it into the ports.
//
// No movie made by BizHawk is kept here, so the logs are written out by hand in the two layouts
// its PlayStation cores use - Octoshock's and Nymashock's names for the same controls - and the
// archive is built by the test, the way .NET builds one: deflated, streamed, with the sizes in
// the central directory and zeroes where the entry's own header would have them.
//
// Every button is checked alone, for the exact bit it ends up as, and the sticks for which byte
// each lands in: a movie that plays with two buttons the wrong way round still plays, and looks
// like a desync. The last group goes all the way to the wire, polling the pad a movie has set
// through Sio the way the BIOS does.

#include "psx/psx.h"
#include "psx/mc_directory.h"
#include "psx/movie.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using emulation::psx::Movie;
using emulation::psx::MovieConsole;
using emulation::psx::MovieDevice;
using emulation::psx::MoviePad;
using emulation::psx::MoviePlayer;
using emulation::psx::Sio;
using emulation::psx::System;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckEqual(long long got, long long want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %lld (%llX) want %lld (%llX)\n", what, got, got, want, want);
  }
}

void Group(const char* name) { printf("%s\n", name); }

const char kHeader[] =
    "MovieVersion BizHawk v2.0.0\r\n"
    "Author somebody\r\n"
    "emuVersion Version 2.8\r\n"
    "Platform PSX\r\n"
    "GameName Some Game (USA)\r\n"
    "Core Octoshock\r\n"
    "rerecordCount 12345\r\n";

// Octoshock's key for one DualShock, in the order BizHawk writes it: axes before buttons in
// each group, the console's group first.
const char kOctoshockKey[] =
    "LogKey:#Disc Select|Open|Close|Reset|"
    "#P1 LStick X|P1 LStick Y|P1 RStick X|P1 RStick Y|"
    "P1 Up|P1 Down|P1 Left|P1 Right|P1 Select|P1 Start|P1 Square|P1 Triangle|P1 Circle|"
    "P1 Cross|P1 L1|P1 R1|P1 L2|P1 R2|P1 L3|P1 R3|P1 MODE|\r\n";

const char kOctoshockSync[] =
    "{\"o\":{\"$type\":\"BizHawk.Emulation.Cores.Sony.PSX.Octoshock+SyncSettings\","
    "\"EnableLEC\":false,\"FIOConfig\":{\"Multitaps\":[false,false],"
    "\"Memcards\":[true,false],\"Devices8\":[2,0,0,0,0,0,0,0]}}}";

std::string Log(const std::string& key, const std::vector<std::string>& frames) {
  std::string log = "[Input]\r\n" + key;
  for (const std::string& frame : frames)
    log += frame + "\r\n";
  return log + "[/Input]\r\n";
}

// A frame of the Octoshock key above: the disc, the console's three, the sticks, the 17 buttons.
std::string OctoFrame(int disc, const char* console, int lx, int ly, int rx, int ry,
                      const char* buttons) {
  char line[128];
  snprintf(line, sizeof(line), "|%5d,%s|%5d,%5d,%5d,%5d,%s|", disc, console, lx, ly, rx, ry,
           buttons);
  return line;
}

const char kNoButtons[] = ".................";

bool Parse(const std::string& header, const std::string& log, const std::string& sync,
           Movie* movie, std::string* error = nullptr) {
  std::string ignored;
  return emulation::psx::ParseBk2Text(header, log, sync, movie, error ? error : &ignored);
}

// ---------------------------------------------------------------------------------------------

void TestOctoshockLog() {
  Group("an Octoshock log: header, players, sticks and the console's controls");
  Movie movie;
  std::string error;
  const std::string log = Log(kOctoshockKey, {
      OctoFrame(1, "...", 128, 128, 128, 128, kNoButtons),
      OctoFrame(1, "...", 0, 255, 17, 200, "U........X......."),
      OctoFrame(2, "OCr", 128, 128, 128, 128, "................M"),
  });
  Check(Parse(kHeader, log, kOctoshockSync, &movie, &error), "it parses");
  if (!error.empty())
    printf("        %s\n", error.c_str());
  CheckEqual(movie.frames(), 3, "three frames");
  Check(movie.game == "Some Game (USA)", "the game's name, spaces and all");
  Check(movie.author == "somebody", "the author");
  Check(movie.core == "Octoshock", "the core");
  CheckEqual(static_cast<long long>(movie.rerecords), 12345, "the rerecord count");
  CheckEqual(static_cast<long long>(movie.players.size()), 1, "one player");
  if (movie.players.size() != 1 || movie.frames() != 3)
    return;
  CheckEqual(movie.players[0].port, 0, "in port 1");
  Check(movie.players[0].device == MovieDevice::kDualShock, "holding a DualShock");
  Check(movie.memory_card[0] && !movie.memory_card[1], "a card in slot 1 alone");
  Check(movie.ignored.empty(), "every column understood");

  CheckEqual(movie.pad(0, 0).buttons, 0, "frame 0: nothing held");
  CheckEqual(movie.pad(0, 0).left_x, 0x80, "frame 0: the stick at rest");
  CheckEqual(movie.pad(1, 0).buttons, Sio::kUp | Sio::kCross, "frame 1: Up and Cross");
  CheckEqual(movie.pad(1, 0).left_x, 0, "frame 1: left stick X");
  CheckEqual(movie.pad(1, 0).left_y, 255, "frame 1: left stick Y");
  CheckEqual(movie.pad(1, 0).right_x, 17, "frame 1: right stick X");
  CheckEqual(movie.pad(1, 0).right_y, 200, "frame 1: right stick Y");
  Check(!movie.pad(1, 0).analog, "frame 1: ANALOG up");
  Check(movie.pad(2, 0).analog, "frame 2: ANALOG down");
  CheckEqual(movie.pad(2, 0).buttons, 0, "frame 2: ANALOG is no button of the pad's");
  CheckEqual(movie.console[0].buttons, 0, "frame 0: nothing on the console");
  CheckEqual(movie.console[0].disc, 1, "frame 0: the first disc");
  CheckEqual(movie.console[2].buttons,
             MovieConsole::kOpen | MovieConsole::kClose | MovieConsole::kReset,
             "frame 2: Open, Close and Reset");
  CheckEqual(movie.console[2].disc, 2, "frame 2: the second disc");
  Check(movie.Summary() == "Some Game (USA) by somebody, 3 frames (Octoshock)", "the summary");
}

void TestEveryOctoshockButton() {
  Group("each Octoshock button alone is the pad's own bit");
  const uint16_t expected[16] = {
      Sio::kUp, Sio::kDown, Sio::kLeft, Sio::kRight, Sio::kSelect, Sio::kStart, Sio::kSquare,
      Sio::kTriangle, Sio::kCircle, Sio::kCross, Sio::kL1, Sio::kR1, Sio::kL2, Sio::kR2,
      Sio::kL3, Sio::kR3,
  };
  const char* names[16] = { "Up", "Down", "Left", "Right", "Select", "Start", "Square",
                            "Triangle", "Circle", "Cross", "L1", "R1", "L2", "R2", "L3", "R3" };
  std::vector<std::string> frames;
  for (int i = 0; i < 16; ++i) {
    std::string buttons = kNoButtons;
    buttons[static_cast<size_t>(i)] = '#';   // any character but '.' is down
    frames.push_back(OctoFrame(1, "...", 128, 128, 128, 128, buttons.c_str()));
  }
  Movie movie;
  Check(Parse(kHeader, Log(kOctoshockKey, frames), kOctoshockSync, &movie), "it parses");
  if (movie.frames() != 16)
    return;
  for (uint32_t i = 0; i < 16; ++i)
    CheckEqual(movie.pad(i, 0).buttons, expected[i], names[i]);
}

void TestNymashockNames() {
  Group("Nymashock's names for the same controls, symbols and all");
  // Mednafen's own names, a description after each symbol - with "left" and "right" in the
  // very names that are not directions.
  const std::string key =
      "LogKey:#Disk Index|Power|Reset|Open Tray|Close Tray|"
      "#P1 Left Stick Up / Down|P1 Left Stick Left / Right|"
      "P1 Right Stick Up / Down|P1 Right Stick Left / Right|"
      "P1 D-Pad Up|P1 D-Pad Down|P1 D-Pad Left|P1 D-Pad Right|P1 Select|P1 Start|"
      "P1 \xE2\x96\xB3 (upper)|P1 \xE2\x97\x8B (right)|P1 X (lower)|P1 \xE2\x96\xA1 (left)|"
      "P1 L1 (front left shoulder)|P1 R1 (front right shoulder)|"
      "P1 L2 (rear left shoulder)|P1 R2 (rear right shoulder)|"
      "P1 Left Stick, Button|P1 Right Stick, Button|P1 Analog|\r\n";
  const uint16_t expected[16] = {
      Sio::kUp, Sio::kDown, Sio::kLeft, Sio::kRight, Sio::kSelect, Sio::kStart, Sio::kTriangle,
      Sio::kCircle, Sio::kCross, Sio::kSquare, Sio::kL1, Sio::kR1, Sio::kL2, Sio::kR2,
      Sio::kL3, Sio::kR3,
  };
  const char* names[16] = { "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right", "Select",
                            "Start", "triangle", "circle", "X", "square", "L1", "R1", "L2", "R2",
                            "Left Stick, Button", "Right Stick, Button" };
  std::vector<std::string> frames;
  for (int i = 0; i < 16; ++i) {
    std::string buttons = kNoButtons;
    buttons[static_cast<size_t>(i)] = '!';
    frames.push_back("|    0,....|  128,  128,  128,  128," + buttons + "|");
  }
  frames.push_back("|    0,....|   10,   20,   30,   40,................A|");
  frames.push_back("|    1,PROC|  128,  128,  128,  128,.................|");
  frames.push_back("|   -1,....|  128,  128,  128,  128,.................|");

  std::string header = kHeader;
  header.replace(header.find("Octoshock"), 9, "Nymashock");
  const std::string sync =
      "{\"o\":{\"MednafenValues\":{},\"PortDevices\":{\"0\":\"dualshock\",\"1\":\"none\"}}}";
  Movie movie;
  std::string error;
  Check(Parse(header, Log(key, frames), sync, &movie, &error), "it parses");
  if (!error.empty())
    printf("        %s\n", error.c_str());
  if (movie.frames() != 19 || movie.players.size() != 1)
    return;
  Check(movie.ignored.empty(), "every column understood");
  for (uint32_t i = 0; i < 16; ++i)
    CheckEqual(movie.pad(i, 0).buttons, expected[i], names[i]);
  CheckEqual(movie.pad(16, 0).left_y, 10, "Left Stick Up / Down is the left stick's Y");
  CheckEqual(movie.pad(16, 0).left_x, 20, "Left Stick Left / Right is its X");
  CheckEqual(movie.pad(16, 0).right_y, 30, "Right Stick Up / Down is the right stick's Y");
  CheckEqual(movie.pad(16, 0).right_x, 40, "Right Stick Left / Right is its X");
  Check(movie.pad(16, 0).analog, "Analog is the ANALOG button");
  CheckEqual(movie.console[0].disc, 1, "Disk Index 0 is the first disc");
  CheckEqual(movie.console[17].disc, 2, "Disk Index 1 is the second");
  CheckEqual(movie.console[18].disc, 0, "Disk Index -1 is no disc");
  CheckEqual(movie.console[17].buttons,
             MovieConsole::kPower | MovieConsole::kReset | MovieConsole::kOpen |
                 MovieConsole::kClose,
             "Power, Reset, Open Tray and Close Tray");
  Check(movie.players[0].device == MovieDevice::kDualShock, "port 1 holds a DualShock");
}

void TestUnknownColumns() {
  Group("columns with names not known are read past, button or axis");
  const std::string key =
      "LogKey:#Insert Coin|Wobble|#P1 Up|P1 Twiddle|P1 Cross|P1 Sparkle|P1 Start|\r\n";
  Movie movie;
  std::string error;
  Check(Parse(kHeader, Log(key, { "|.   42,|U    7,X.S|", "|.    0,|.  -12,.!.|" }), "", &movie,
              &error),
        "it parses");
  if (!error.empty())
    printf("        %s\n", error.c_str());
  if (movie.frames() != 2)
    return;
  CheckEqual(movie.pad(0, 0).buttons, Sio::kUp | Sio::kCross | Sio::kStart,
             "the known buttons either side of them still land");
  CheckEqual(movie.pad(1, 0).buttons, 0, "and an unknown button held presses nothing");
  CheckEqual(static_cast<long long>(movie.ignored.size()), 4, "four columns ignored");
  Check(movie.players.size() == 1 && movie.players[0].device == MovieDevice::kDigital,
        "no sticks in the log: a digital pad");
}

void TestPlayerPlacement() {
  Group("which socket each player is in");
  const std::string two_pads =
      "LogKey:#P1 Up|P1 Cross|#P2 Up|P2 Cross|\r\n";
  Movie movie;

  // Octoshock counts the sockets that hold something: a pad in port 2 alone is P1.
  const std::string one_pad = "LogKey:#P1 Up|P1 Cross|\r\n";
  const std::string port2 =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[false,false],\"Memcards\":[false,true],"
      "\"Devices8\":[0,0,0,0,1,0,0,0]}}}";
  Check(Parse(kHeader, Log(one_pad, { "|UX|" }), port2, &movie), "Octoshock, port 2 alone");
  if (movie.players.size() == 1) {
    CheckEqual(movie.players[0].port, 1, "P1 is the pad in port 2");
    Check(movie.players[0].device == MovieDevice::kDigital, "a digital pad");
    Check(!movie.memory_card[0] && movie.memory_card[1], "and the card is in slot 2");
  } else {
    Check(false, "one player");
  }

  // A multitap in port 1 with pads in A and C, and a pad in port 2: P1, P2, P3.
  const std::string three =
      "LogKey:#P1 Up|P1 Cross|#P2 Up|P2 Cross|#P3 Up|P3 Cross|\r\n";
  const std::string tap =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[true,false],\"Memcards\":[true,true],"
      "\"Devices8\":[1,0,2,0,3,0,0,0]}}}";
  Check(Parse(kHeader, Log(three, { "|UX|..|.X|" }), tap, &movie), "Octoshock, a multitap");
  if (movie.players.size() == 3) {
    Check(movie.multitap[0] && !movie.multitap[1], "the multitap is in port 1");
    Check(movie.players[0].port == 0 && movie.players[0].socket == 0, "P1 in socket A");
    Check(movie.players[1].port == 0 && movie.players[1].socket == 2, "P2 in socket C");
    Check(movie.players[1].device == MovieDevice::kDualShock, "which holds a DualShock");
    Check(movie.players[2].port == 1 && movie.players[2].socket == 0, "P3 in port 2");
    Check(movie.players[2].device == MovieDevice::kDualAnalog, "which holds a Dual Analog");
    CheckEqual(movie.pad(0, 2).buttons, Sio::kCross, "and P3's Cross is P3's");
  } else {
    Check(false, "three players");
  }

  // Nymashock's P<n> is virtual port n: with a multitap on port 2, virtual ports 2-5 are its
  // sockets.
  std::string header = kHeader;
  header.replace(header.find("Octoshock"), 9, "Nymashock");
  const std::string nyma =
      "{\"o\":{\"MednafenValues\":{\"psx.input.pport2.multitap\":\"1\","
      "\"psx.input.port2.memcard\":\"1\"},"
      "\"PortDevices\":{\"0\":\"gamepad\",\"1\":\"none\",\"2\":\"dualshock\"}}}";
  const std::string nyma_key = "LogKey:#P1 Up|P1 X|#P3 Up|P3 X|\r\n";
  Check(Parse(header, Log(nyma_key, { "|UX|.X|" }), nyma, &movie), "Nymashock, a multitap");
  if (movie.players.size() == 2) {
    Check(!movie.multitap[0] && movie.multitap[1], "the multitap is in port 2");
    Check(movie.players[0].port == 0 && movie.players[0].socket == 0, "P1 in port 1");
    Check(movie.players[1].number == 3 && movie.players[1].port == 1 &&
              movie.players[1].socket == 1,
          "P3 in port 2's socket B");
    Check(movie.memory_card[0] && movie.memory_card[1], "cards in both slots");
  } else {
    Check(false, "two players");
  }

  // No settings at all: the two players the log has, in the two ports.
  Check(Parse(kHeader, Log(two_pads, { "|UX|.X|" }), "", &movie), "no settings");
  if (movie.players.size() == 2) {
    Check(movie.players[0].port == 0 && movie.players[1].port == 1, "P1 and P2 in ports 1, 2");
    Check(!movie.multitap[0] && !movie.multitap[1], "and no multitap");
  } else {
    Check(false, "two players");
  }
}

void TestRefusals() {
  Group("what cannot be played is refused, and says why");
  Movie movie;
  std::string error;
  const std::string log = Log(kOctoshockKey, { OctoFrame(1, "...", 128, 128, 128, 128,
                                                         kNoButtons) });

  Check(!Parse(std::string(kHeader) + "StartsFromSavestate True\r\n", log, "", &movie, &error),
        "a movie that starts from a save state");
  Check(error.find("save state") != std::string::npos, "...saying so");

  std::string nes = kHeader;
  nes.replace(nes.find("PSX"), 3, "NES");
  Check(!Parse(nes, log, "", &movie, &error), "a movie for another machine");
  Check(error.find("NES") != std::string::npos, "...naming it");

  const std::string negcon =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[false,false],\"Memcards\":[true,false],"
      "\"Devices8\":[4,0,0,0,0,0,0,0]}}}";
  Check(!Parse(kHeader, log, negcon, &movie, &error), "a movie with a neGcon");
  Check(error.find("negcon") != std::string::npos, "...naming it");

  Check(!Parse(kHeader, "[Input]\r\n|..|\r\n[/Input]\r\n", "", &movie, &error),
        "a log with no key");
  Check(!Parse(kHeader, Log(kOctoshockKey, { "|    1,...|  128,  128,..|" }), "", &movie, &error),
        "a line shorter than its key");
  Check(error.find("frame 0") != std::string::npos, "...naming the frame");
  Check(!Parse(kHeader, Log(kOctoshockKey, {}), "", &movie, &error), "a log with no frames");
}

int Poll(System* system, int port, uint8_t* reply);

// What a real movie taught (Metal Gear Solid: Integral, Octoshock, BizHawk 1.11.4): cards saved
// in the movie, a BIOS named by its SHA-1, and Dual Analog pads that are analog from power-on.
void TestSavedCardsAndBios() {
  Group("a movie that starts from saved cards, and names its BIOS");
  const std::string log = Log(kOctoshockKey, { OctoFrame(1, "...", 128, 128, 128, 128,
                                                         kNoButtons) });
  const std::string header = std::string(kHeader) + "StartsFromSaveRam True\r\n" +
                             "PSX_Firmware_J 343883A7B555646DA8CEE54AADD2795B6E7DD070\r\n";
  const std::string both_cards =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[false,false],\"Memcards\":[true,true],"
      "\"Devices8\":[2,0,0,0,0,0,0,0]}}}";
  const std::string second_card =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[false,false],\"Memcards\":[false,true],"
      "\"Devices8\":[2,0,0,0,0,0,0,0]}}}";
  std::vector<uint8_t> two(2 * Movie::kCardSize, 0);
  two[0] = 'M';
  two[1] = 'C';
  two[Movie::kCardSize] = 0x5A;        // the second card's first byte
  two[2 * Movie::kCardSize - 1] = 0xA5;   // and its last

  Movie movie;
  std::string error;
  Check(emulation::psx::ParseBk2Text(header, log, both_cards, &movie, &error, &two),
        "two cards, 256 KB: it parses");
  CheckEqual(static_cast<long long>(movie.card[0].size()), Movie::kCardSize, "slot 1 has a card");
  CheckEqual(static_cast<long long>(movie.card[1].size()), Movie::kCardSize, "slot 2 has a card");
  if (movie.card[0].size() == Movie::kCardSize && movie.card[1].size() == Movie::kCardSize) {
    Check(movie.card[0][0] == 'M' && movie.card[0][1] == 'C', "slot 1's is the first 128 KB");
    Check(movie.card[1][0] == 0x5A && movie.card[1][Movie::kCardSize - 1] == 0xA5,
          "slot 2's is the second");
  }
  Check(movie.bios_sha1 == "343883a7b555646da8cee54aadd2795b6e7dd070",
        "the BIOS's SHA-1, in lower case");

  std::vector<uint8_t> one(Movie::kCardSize, 0x77);
  Check(emulation::psx::ParseBk2Text(header, log, second_card, &movie, &error, &one),
        "a card in slot 2 alone, 128 KB: it parses");
  Check(movie.card[0].empty() && movie.card[1].size() == Movie::kCardSize,
        "and the one card is slot 2's");

  Check(!emulation::psx::ParseBk2Text(header, log, both_cards, &movie, &error, &one),
        "128 KB for two cards is refused");
  Check(error.find("131072 bytes for 2 cards") != std::string::npos, "...with the sizes");
  Check(!emulation::psx::ParseBk2Text(header, log, both_cards, &movie, &error, nullptr),
        "and so is no MovieSaveRam.bin at all");
  Check(emulation::psx::ParseBk2Text(kHeader, log, both_cards, &movie, &error, &one),
        "a movie that does not start from cards ignores one it is handed");
  Check(movie.card[0].empty() && movie.card[1].empty() && movie.bios_sha1.empty(),
        "and has no cards and no BIOS named");

  Group("SHA-1, which is how a movie names its BIOS");
  Check(emulation::psx::Sha1Hex(nullptr, 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709",
        "of nothing");
  Check(emulation::psx::Sha1Hex(reinterpret_cast<const uint8_t*>("abc"), 3) ==
            "a9993e364706816aba3e25717850c26c9cd0d89d",
        "of \"abc\"");
  // 56 bytes: the length no longer fits the block the data ends in.
  const char* fips = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  Check(emulation::psx::Sha1Hex(reinterpret_cast<const uint8_t*>(fips), 56) ==
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        "of the standard's 56-byte message");
  const std::vector<uint8_t> million(1000000, 'a');
  Check(emulation::psx::Sha1Hex(million.data(), million.size()) ==
            "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
        "of a million a's");
}

void TestDiscCheck() {
  Group("whether a movie was made on the disc that is in");
  const std::string log = Log(kOctoshockKey, { OctoFrame(1, "...", 128, 128, 128, 128,
                                                         kNoButtons) });
  Movie movie;
  // No disc is mounted, which is still a disc as far as its id goes: no tracks, and 26 sectors
  // that read as nothing.
  emulation::psx::Disc disc;
  char none[16];
  snprintf(none, sizeof(none), "%08x", emulation::psx::BizHawkDiscId(disc));

  Check(Parse(kHeader, log, kOctoshockSync, &movie), "a header with no hash");
  Check(movie.disc_hash.empty(), "names no disc");
  emulation::psx::MovieDiscCheck check = emulation::psx::CheckMovieDisc(movie, disc);
  Check(check.by_hash == emulation::psx::MovieDiscCheck::kCannotTell, "so the hash says nothing");
  Check(check.by_serial == emulation::psx::MovieDiscCheck::kCannotTell, "nor, with no cards, the serial");
  Check(check.disc_id == none, "and the disc's own id is given all the same");

  Check(Parse(std::string(kHeader) + "SHA1 0DEFACED\r\n", log, kOctoshockSync, &movie),
        "a header with an eight-digit hash");
  Check(movie.disc_hash == "0defaced", "names its disc, in lower case");
  check = emulation::psx::CheckMovieDisc(movie, disc);
  Check(check.by_hash == emulation::psx::MovieDiscCheck::kDifferent, "which this disc is not");
  Check(check.Describe(movie).find("NOT the same disc") != std::string::npos, "...said plainly");

  std::string upper = none;
  for (char& c : upper)
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  Check(Parse(std::string(kHeader) + "CRC32 CRC32:" + upper + "\r\n", log, kOctoshockSync, &movie),
        "the disc's own id, under another key and with its kind in front");
  check = emulation::psx::CheckMovieDisc(movie, disc);
  Check(check.by_hash == emulation::psx::MovieDiscCheck::kSame, "is the same disc");

  Check(Parse(std::string(kHeader) + "SHA1 343883A7B555646DA8CEE54AADD2795B6E7DD070\r\n", log,
              kOctoshockSync, &movie),
        "a forty-digit hash");
  check = emulation::psx::CheckMovieDisc(movie, disc);
  Check(check.by_hash == emulation::psx::MovieDiscCheck::kCannotTell,
        "is not a disc's id, and says nothing either way");

  // A movie that starts from a card with a save on it: the save's name has the serial of the
  // disc that made it.
  std::vector<uint8_t> card(Movie::kCardSize, 0);
  emulation::psx::mcdir::Format(card.data());
  uint8_t* entry = card.data() + 128;   // the directory's first frame
  memset(entry, 0, 128);
  entry[0] = 0x51;                      // in use, the first block of a save
  entry[5] = 0x20;                      // 8,192 bytes
  entry[8] = entry[9] = 0xFF;           // and its last
  memcpy(entry + 10, "BISLPM-86247G:E0pBAA", 20);
  entry[127] = emulation::psx::mcdir::FrameChecksum(entry);
  const std::string one_card =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[false,false],\"Memcards\":[true,false],"
      "\"Devices8\":[2,0,0,0,0,0,0,0]}}}";
  std::string error;
  Check(emulation::psx::ParseBk2Text(std::string(kHeader) + "StartsFromSaveRam True\r\n", log,
                                     one_card, &movie, &error, &card),
        "a movie that starts from a card with a save on it");
  check = emulation::psx::CheckMovieDisc(movie, disc);
  Check(check.card_serials.size() == 1 && check.card_serials[0] == "SLPM-86247",
        "the save was made under SLPM-86247");
  Check(check.by_serial == emulation::psx::MovieDiscCheck::kCannotTell,
        "which a disc with no serial neither is nor is not");
}

void TestCardsInTheMachine() {
  Group("the machine: the cards a movie starts with");
  char temp[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, temp);
  std::string directory = temp;
  while (!directory.empty() && (directory.back() == '\\' || directory.back() == '/'))
    directory.pop_back();
  System* system = new System();
  system->InitializeWithoutBios();

  const std::string log = Log(kOctoshockKey, { OctoFrame(1, "...", 128, 128, 128, 128,
                                                         kNoButtons) });
  const std::string both_cards =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[false,false],\"Memcards\":[true,true],"
      "\"Devices8\":[2,0,0,0,0,0,0,0]}}}";
  std::string error;
  Movie movie;

  // Octoshock from power-on: a new card is all zeroes, and slot 2 has none by default.
  Parse(kHeader, log, kOctoshockSync, &movie);
  Check(emulation::psx::InsertMovieCards(*system, movie, directory, &error), "Octoshock, new");
  Check(system->mc(0).connected() && !system->mc(1).connected(), "a card in slot 1 alone");
  if (system->mc(0).connected()) {
    bool zero = true;
    for (size_t i = 0; i < Movie::kCardSize; ++i)
      zero = zero && system->mc(0).data()[i] == 0;
    Check(zero, "and it is all zeroes: never formatted");
  }

  // Nymashock from power-on: formatted.
  std::string nyma = kHeader;
  nyma.replace(nyma.find("Octoshock"), 9, "Nymashock");
  Parse(nyma, log, "", &movie);
  Check(emulation::psx::InsertMovieCards(*system, movie, directory, &error), "Nymashock, new");
  Check(system->mc(0).connected() && system->mc(0).data()[0] == 'M' &&
            system->mc(0).data()[1] == 'C',
        "its new card is formatted");

  // From saved cards: each slot's own, byte for byte.
  std::vector<uint8_t> two(2 * Movie::kCardSize);
  for (size_t i = 0; i < two.size(); ++i)
    two[i] = static_cast<uint8_t>(i * 7 + (i >> 9));
  const std::string header = std::string(kHeader) + "StartsFromSaveRam True\r\n";
  Check(emulation::psx::ParseBk2Text(header, log, both_cards, &movie, &error, &two), "saved");
  Check(emulation::psx::InsertMovieCards(*system, movie, directory, &error), "...and inserted");
  Check(system->mc(0).connected() && system->mc(1).connected(), "a card in each slot");
  if (system->mc(0).connected() && system->mc(1).connected()) {
    Check(memcmp(system->mc(0).data(), two.data(), Movie::kCardSize) == 0,
          "slot 1 holds the movie's first card");
    Check(memcmp(system->mc(1).data(), two.data() + Movie::kCardSize, Movie::kCardSize) == 0,
          "slot 2 holds its second");
    // And on disk, so a reset the movie makes brings the same card back.
    const std::string path = system->mc(1).filename();
    FILE* file = fopen(path.c_str(), "rb");
    std::vector<uint8_t> written(Movie::kCardSize);
    const bool read = file != nullptr &&
                      fread(written.data(), 1, written.size(), file) == written.size();
    if (file != nullptr)
      fclose(file);
    Check(read && memcmp(written.data(), two.data() + Movie::kCardSize, Movie::kCardSize) == 0,
          "and its file is that card");
  }
  system->mc(0).Eject();
  system->mc(1).Eject();
  DeleteFileA((directory + "\\movie_card1.mcr").c_str());
  DeleteFileA((directory + "\\movie_card2.mcr").c_str());
  delete system;
}

void TestDualAnalogIsAlwaysAnalog() {
  Group("the machine: Mednafen's Dual Analog is analog from power-on, MODE or no MODE");
  System* system = new System();
  system->InitializeWithoutBios();
  const std::string dual_analog =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[false,false],\"Memcards\":[true,true],"
      "\"Devices8\":[3,0,0,0,3,0,0,0]}}}";
  std::string key = kOctoshockKey;
  key.erase(key.find("\r\n"));
  std::string second = key.substr(key.find("#P1"));
  for (size_t at = second.find("P1"); at != std::string::npos; at = second.find("P1", at))
    second[at + 1] = '2';
  key += second + "\r\n";
  auto frame = [](const char* p1, int lx, const char* p2) {
    char line[200];
    snprintf(line, sizeof(line), "|    1,...|%5d,  128,  128,  128,%s|  128,  128,  128,  128,%s|",
             lx, p1, p2);
    return std::string(line);
  };
  Movie movie;
  std::string error;
  Check(Parse(kHeader,
              Log(key, { frame(kNoButtons, 128, kNoButtons),
                         frame("................M", 9, ".........X......."),
                         frame(kNoButtons, 200, kNoButtons) }),
              dual_analog, &movie, &error),
        "two Dual Analog pads: it parses");
  if (!error.empty())
    printf("        %s\n", error.c_str());
  if (movie.players.size() != 2) {
    Check(false, "two players");
    delete system;
    return;
  }
  Check(movie.players[0].device == MovieDevice::kDualAnalog && movie.players[0].always_analog,
        "P1 is a Dual Analog, always analog");
  Check(movie.players[1].port == 1 && movie.players[1].always_analog, "and so is P2, in port 2");

  MoviePlayer player;
  player.Start(movie);
  uint8_t reply[9] = {};
  player.Next(0);
  ApplyMoviePads(*system, player);
  CheckEqual(Poll(system, 0, reply), 9, "the first poll of all has the sticks in it");
  CheckEqual(reply[1], 0x73, "and the analog id, with MODE never pressed");
  Poll(system, 1, reply);
  CheckEqual(reply[1], 0x73, "port 2's the same");

  player.Next(1);   // MODE held on P1, Cross on P2
  ApplyMoviePads(*system, player);
  Poll(system, 0, reply);
  CheckEqual(reply[1], 0x73, "MODE does nothing to it");
  CheckEqual(reply[7], 9, "and its left stick reads");
  Poll(system, 1, reply);
  CheckEqual(reply[4], 0xFF & ~0x40, "P2's Cross reaches port 2");

  player.Next(2);
  ApplyMoviePads(*system, player);
  Poll(system, 0, reply);
  CheckEqual(reply[1], 0x73, "nor does letting go of it");
  CheckEqual(reply[7], 200, "the stick still reads");
  delete system;
}

// ---------------------------------------------------------------------------------------------
// The archive
// ---------------------------------------------------------------------------------------------

void Put16(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value));
  out->push_back(static_cast<uint8_t>(value >> 8));
}
void Put32(std::vector<uint8_t>* out, uint32_t value) {
  Put16(out, value & 0xFFFF);
  Put16(out, value >> 16);
}

struct ZipEntry {
  std::string name;
  std::string text;
  bool deflated;
};

// A zip as .NET's streaming writer leaves it: bit 3 set, the entry's own sizes zero, a data
// descriptor after the data, and the real sizes in the central directory alone.
std::vector<uint8_t> BuildZip(const std::vector<ZipEntry>& entries) {
  std::vector<uint8_t> zip;
  std::vector<uint8_t> directory;
  for (const ZipEntry& entry : entries) {
    std::vector<uint8_t> packed(entry.text.begin(), entry.text.end());
    if (entry.deflated) {
      z_stream stream = {};
      deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                   Z_DEFAULT_STRATEGY);
      packed.assign(deflateBound(&stream, static_cast<uLong>(entry.text.size())) + 16, 0);
      stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(entry.text.data()));
      stream.avail_in = static_cast<uInt>(entry.text.size());
      stream.next_out = packed.data();
      stream.avail_out = static_cast<uInt>(packed.size());
      deflate(&stream, Z_FINISH);
      packed.resize(stream.total_out);
      deflateEnd(&stream);
    }
    const uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(entry.text.data()),
                               static_cast<uInt>(entry.text.size()));
    const uint32_t offset = static_cast<uint32_t>(zip.size());
    const uint16_t method = entry.deflated ? 8 : 0;

    Put32(&zip, 0x04034B50);
    Put16(&zip, 20);
    Put16(&zip, 0x0008);   // sizes follow the data
    Put16(&zip, method);
    Put32(&zip, 0);        // time and date
    Put32(&zip, 0);        // crc, and the two sizes: not known yet
    Put32(&zip, 0);
    Put32(&zip, 0);
    Put16(&zip, static_cast<uint32_t>(entry.name.size()));
    Put16(&zip, 0);
    zip.insert(zip.end(), entry.name.begin(), entry.name.end());
    zip.insert(zip.end(), packed.begin(), packed.end());
    Put32(&zip, 0x08074B50);
    Put32(&zip, crc);
    Put32(&zip, static_cast<uint32_t>(packed.size()));
    Put32(&zip, static_cast<uint32_t>(entry.text.size()));

    Put32(&directory, 0x02014B50);
    Put16(&directory, 20);
    Put16(&directory, 20);
    Put16(&directory, 0x0008);
    Put16(&directory, method);
    Put32(&directory, 0);
    Put32(&directory, crc);
    Put32(&directory, static_cast<uint32_t>(packed.size()));
    Put32(&directory, static_cast<uint32_t>(entry.text.size()));
    Put16(&directory, static_cast<uint32_t>(entry.name.size()));
    Put16(&directory, 0);
    Put16(&directory, 0);
    Put16(&directory, 0);
    Put16(&directory, 0);
    Put32(&directory, 0);
    Put32(&directory, offset);
    directory.insert(directory.end(), entry.name.begin(), entry.name.end());
  }
  const uint32_t directory_offset = static_cast<uint32_t>(zip.size());
  zip.insert(zip.end(), directory.begin(), directory.end());
  Put32(&zip, 0x06054B50);
  Put16(&zip, 0);
  Put16(&zip, 0);
  Put16(&zip, static_cast<uint32_t>(entries.size()));
  Put16(&zip, static_cast<uint32_t>(entries.size()));
  Put32(&zip, static_cast<uint32_t>(directory.size()));
  Put32(&zip, directory_offset);
  Put16(&zip, 0);
  return zip;
}

void TestArchive() {
  Group("the archive: deflated and stored entries, sizes from the central directory");
  // Long enough that deflate has something to do, and that a wrong size would show.
  std::vector<std::string> frames;
  for (int i = 0; i < 5000; ++i) {
    std::string buttons = kNoButtons;
    if (i % 7 == 0)
      buttons[9] = 'X';
    frames.push_back(OctoFrame(1, "...", 128, i % 256, 128, 128, buttons.c_str()));
  }
  const std::string log = Log(kOctoshockKey, frames);
  const std::vector<uint8_t> zip = BuildZip({
      { "BizVersion.txt", "2.8", false },
      { "Header.txt", kHeader, false },
      { "Comments.txt", "", true },
      { "Input Log.txt", log, true },
      { "SyncSettings.json", kOctoshockSync, true },
  });
  Check(zip.size() < log.size() / 4, "the log really was deflated");

  Movie movie;
  std::string error;
  Check(emulation::psx::ParseBk2(zip.data(), zip.size(), &movie, &error), "it parses");
  if (!error.empty())
    printf("        %s\n", error.c_str());
  CheckEqual(movie.frames(), 5000, "every frame is there");
  if (movie.frames() == 5000) {
    CheckEqual(movie.pad(4998, 0).left_y, 4998 % 256, "the last but one frame's stick");
    CheckEqual(movie.pad(4998, 0).buttons, Sio::kCross, "and its Cross (4998 is 7 x 714)");
    CheckEqual(movie.pad(4999, 0).buttons, 0, "and none on the last");
  }
  Check(movie.game == "Some Game (USA)", "the header came out of its stored entry");

  // Cut short, and an archive of something else.
  Check(!emulation::psx::ParseBk2(zip.data(), zip.size() / 2, &movie, &error),
        "half an archive is refused");
  const std::vector<uint8_t> other = BuildZip({ { "readme.txt", "hello", false } });
  Check(!emulation::psx::ParseBk2(other.data(), other.size(), &movie, &error),
        "a zip with no input log is refused");
  Check(error.find("Input Log.txt") != std::string::npos, "...saying what is missing");
  const uint8_t junk[64] = { 'P', 'S', '-', 'X', ' ', 'E', 'X', 'E' };
  Check(!emulation::psx::ParseBk2(junk, sizeof(junk), &movie, &error),
        "a file that is no zip is refused");
}

// ---------------------------------------------------------------------------------------------
// Playing
// ---------------------------------------------------------------------------------------------

Movie SmallMovie(const std::vector<std::string>& frames) {
  Movie movie;
  Parse(kHeader, Log(kOctoshockKey, frames), kOctoshockSync, &movie);
  return movie;
}

void TestPlayer() {
  Group("the player: a frame a call, in order, then finished");
  const std::string idle = OctoFrame(1, "...", 128, 128, 128, 128, kNoButtons);
  const std::string cross = OctoFrame(1, "...", 128, 128, 128, 128, ".........X.......");
  const std::string start = OctoFrame(1, "...", 128, 128, 128, 128, ".....S...........");
  MoviePlayer player;
  player.Start(SmallMovie({ idle, cross, start }));
  Check(player.playing(), "playing once started");
  CheckEqual(player.initial_disc(), 1, "it boots with the first disc");

  uint64_t count = 100;   // the GPU's count: where it starts does not matter
  MoviePlayer::Step step = player.Next(count++);
  CheckEqual(player.frame(), 0, "the first call is frame 0");
  CheckEqual(player.pads()[0].buttons, 0, "with nothing held");
  step = player.Next(count);
  CheckEqual(player.pads()[0].buttons, Sio::kCross, "frame 1: Cross");
  // The debugger halted mid-frame: the frame is asked for again with the count unmoved.
  step = player.Next(count);
  CheckEqual(player.frame(), 1, "asked again before the frame ran, it is still frame 1");
  CheckEqual(player.pads()[0].buttons, Sio::kCross, "with the same pads");
  ++count;
  step = player.Next(count++);
  CheckEqual(player.pads()[0].buttons, Sio::kStart, "frame 2: Start");
  Check(!step.finished && player.playing(), "not finished on its last frame");
  step = player.Next(count++);
  Check(step.finished && !player.playing(), "finished on the call after");
  CheckEqual(player.pads()[0].buttons, 0, "with the pad let go");
  CheckEqual(player.frame(), 3, "and the frame is the movie's length");
  step = player.Next(count++);
  Check(!step.finished, "and it only says so once");

  Group("the player: an offset waits, or leaves frames out");
  player.Start(SmallMovie({ cross, start, idle }));
  player.set_offset(2);
  player.Next(0);
  CheckEqual(player.pads()[0].buttons, 0, "+2: nothing on the first call");
  player.Next(1);
  CheckEqual(player.pads()[0].buttons, 0, "+2: nor the second");
  player.Next(2);
  CheckEqual(player.pads()[0].buttons, Sio::kCross, "+2: the movie's first frame on the third");
  player.Start(SmallMovie({ cross, start, idle }));
  player.set_offset(-1);
  player.Next(0);
  CheckEqual(player.pads()[0].buttons, Sio::kStart, "-1: the first call is the movie's second");

  Group("the player: ANALOG is pressed once however long it is held");
  const std::string mode = OctoFrame(1, "...", 128, 128, 128, 128, "................M");
  player.Start(SmallMovie({ idle, mode, mode, idle, mode }));
  const bool expected[5] = { false, true, false, false, true };
  for (int i = 0; i < 5; ++i) {
    player.Next(static_cast<uint64_t>(i));
    CheckEqual(player.analog_pressed()[0] ? 1 : 0, expected[i] ? 1 : 0,
               i == 1   ? "down on frame 1: a press"
               : i == 2 ? "still down on frame 2: not another"
               : i == 4 ? "down again on frame 4: a press"
                        : "up: no press");
  }
  player.Start(SmallMovie({ idle, mode }));
  player.Next(0);
  player.Next(1);
  player.Next(1);
  Check(!player.analog_pressed()[0], "and a frame asked for twice does not press it twice");
}

void TestLidAndReset() {
  Group("the player: the lid, the disc in it, and a reset");
  auto frame = [](int disc, const char* console) {
    return OctoFrame(disc, console, 128, 128, 128, 128, kNoButtons);
  };
  MoviePlayer player;
  //   0 boots on disc 1; 1 opens; 2 picks disc 2 with the lid up; 3 closes; 4 resets;
  //   5 picks disc 1 with the lid down, which is not looked at; 6 opens and closes at once.
  player.Start(SmallMovie({ frame(1, "..."), frame(1, "O.."), frame(2, "..."), frame(2, ".C."),
                            frame(2, "..r"), frame(1, "..."), frame(1, "OC.") }));
  CheckEqual(player.initial_disc(), 1, "boots on disc 1");
  MoviePlayer::Step step = player.Next(10);
  Check(!step.eject && step.insert == 0 && !step.reset, "frame 0: nothing to do");
  step = player.Next(11);
  Check(step.eject && step.insert == 0, "frame 1: the lid opens");
  step = player.Next(12);
  Check(!step.eject && step.insert == 0, "frame 2: a disc chosen, nothing done yet");
  step = player.Next(13);
  CheckEqual(step.insert, 2, "frame 3: the lid closes on disc 2");
  Check(!step.eject, "...without opening again");
  step = player.Next(14);
  Check(step.reset, "frame 4: reset");
  // The machine was made afresh, so the GPU's count is back where a frame-4 call left it.
  step = player.Next(1);
  CheckEqual(player.frame(), 5, "the call after a reset is the next frame whatever the count");
  Check(!step.eject && step.insert == 0, "frame 5: the disc column is not read with the lid down");
  step = player.Next(2);
  Check(step.eject, "frame 6: open...");
  CheckEqual(step.insert, 1, "...and closed on disc 1 in the one frame");

  player.Start(SmallMovie({ frame(2, "..."), frame(2, "...") }));
  CheckEqual(player.initial_disc(), 2, "a movie whose first frame has disc 2 boots on it");
  player.Start(SmallMovie({ frame(1, "O.."), frame(1, ".C.") }));
  CheckEqual(player.initial_disc(), 0, "one that starts with the lid up boots on an empty tray");
  player.Next(0);
  step = player.Next(1);
  CheckEqual(step.insert, 1, "and puts disc 1 in when it closes");
}

// One poll of the pad in `port`, as the BIOS makes it: 01 42 00 and zeroes for the rest.
int Poll(System* system, int port, uint8_t* reply) {
  const uint8_t out[9] = { 0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  return system->sio().ExchangeDirect(port, out, reply, 9);
}

void TestOnTheWire() {
  Group("the machine: what a game reads from the pad a movie is playing");
  System* system = new System();
  system->InitializeWithoutBios();
  // As a front end leaves them: something else in each port.
  system->sio().set_controller_type(0, Sio::kDigital);
  system->sio().set_controller_type(1, Sio::kDualShock);
  system->sio().set_connected(1, true);

  const std::string idle = OctoFrame(1, "...", 128, 128, 128, 128, kNoButtons);
  MoviePlayer player;
  player.Start(SmallMovie({
      OctoFrame(1, "...", 128, 128, 128, 128, "U........X......."),
      OctoFrame(1, "...", 128, 128, 128, 128, "................M"),
      OctoFrame(1, "...", 1, 2, 3, 4, "..L...Q.........M"),
      idle,
  }));

  uint8_t reply[9] = {};
  player.Next(0);
  const uint16_t held = ApplyMoviePads(*system, player);
  CheckEqual(held, Sio::kUp | Sio::kCross, "ApplyMoviePads returns what is held");
  CheckEqual(system->sio().controller_type(0), Sio::kDualShock,
             "port 1 is the movie's DualShock");
  CheckEqual(system->sio().controller_type(1), Sio::kNone, "port 2 is empty, as in the movie");
  int n = Poll(system, 0, reply);
  CheckEqual(n, 5, "a digital poll: five bytes");
  CheckEqual(reply[1], 0x41, "the pad starts digital, as BizHawk's does");
  // Active low: Up is bit 4 of the first byte, Cross bit 6 of the second.
  CheckEqual(reply[3], 0xFF & ~0x10, "Up, on the wire");
  CheckEqual(reply[4], 0xFF & ~0x40, "Cross, on the wire");
  CheckEqual(Poll(system, 1, reply), 1, "nothing answers in port 2");

  player.Next(1);
  ApplyMoviePads(*system, player);
  n = Poll(system, 0, reply);
  CheckEqual(reply[1], 0x73, "ANALOG pressed: the pad polls as analog");
  CheckEqual(n, 9, "with the sticks in its reply");

  player.Next(2);   // ANALOG still held: no second press
  ApplyMoviePads(*system, player);
  n = Poll(system, 0, reply);
  CheckEqual(reply[1], 0x73, "ANALOG held another frame: still analog");
  CheckEqual(reply[3], 0xFF & ~0x80, "Left, on the wire");
  CheckEqual(reply[4], 0xFF & ~0x80, "Square, on the wire");
  CheckEqual(reply[5], 3, "right stick X is the reply's fifth byte");
  CheckEqual(reply[6], 4, "right stick Y the sixth");
  CheckEqual(reply[7], 1, "left stick X the seventh");
  CheckEqual(reply[8], 2, "left stick Y the eighth");

  player.Next(3);
  ApplyMoviePads(*system, player);
  Poll(system, 0, reply);
  CheckEqual(reply[3], 0xFF, "let go: nothing held");
  CheckEqual(reply[1], 0x73, "and plugging the same pad in every frame does not reset it");

  Group("the machine: a multitap's players");
  const std::string key = "LogKey:#P1 Up|P1 Cross|#P2 Up|P2 Cross|#P3 Up|P3 Cross|\r\n";
  const std::string tap =
      "{\"o\":{\"FIOConfig\":{\"Multitaps\":[true,false],\"Memcards\":[true,true],"
      "\"Devices8\":[1,0,2,0,1,0,0,0]}}}";
  Movie movie;
  Parse(kHeader, Log(key, { "|U.|.X|UX|" }), tap, &movie);
  player.Start(movie);
  player.Next(0);
  ApplyMoviePads(*system, player);
  CheckEqual(system->sio().controller_type(0), Sio::kMultitap, "port 1 is a multitap");
  CheckEqual(system->sio().multitap_player_type(0, 0), Sio::kDigital, "socket A a digital pad");
  CheckEqual(system->sio().multitap_player_type(0, 1), Sio::kNone, "socket B empty");
  CheckEqual(system->sio().multitap_player_type(0, 2), Sio::kDualShock, "socket C a DualShock");
  CheckEqual(system->sio().controller_type(1), Sio::kDigital, "port 2 a digital pad");
  // A multitap's players are reached by address: 01h-04h for A-D.
  auto poll_socket = [&](int socket, uint8_t* into) {
    const uint8_t out[5] = { static_cast<uint8_t>(0x01 + socket), 0x42, 0x00, 0x00, 0x00 };
    return system->sio().ExchangeDirect(0, out, into, 5);
  };
  CheckEqual(poll_socket(0, reply), 5, "socket A answers");
  CheckEqual(reply[3], 0xFF & ~0x10, "with P1's Up");
  CheckEqual(reply[4], 0xFF, "and no Cross");
  CheckEqual(poll_socket(2, reply), 5, "socket C answers");
  CheckEqual(reply[3], 0xFF, "with no Up");
  CheckEqual(reply[4], 0xFF & ~0x40, "and P2's Cross");
  CheckEqual(poll_socket(1, reply), 1, "socket B does not");
  Poll(system, 1, reply);
  CheckEqual(reply[3], 0xFF & ~0x10, "port 2 has P3's Up");
  CheckEqual(reply[4], 0xFF & ~0x40, "and P3's Cross");

  delete system;
}

}  // namespace

int main() {
  TestOctoshockLog();
  TestEveryOctoshockButton();
  TestNymashockNames();
  TestUnknownColumns();
  TestPlayerPlacement();
  TestRefusals();
  TestSavedCardsAndBios();
  TestDiscCheck();
  TestCardsInTheMachine();
  TestDualAnalogIsAlwaysAnalog();
  TestArchive();
  TestPlayer();
  TestLidAndReset();
  TestOnTheWire();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
