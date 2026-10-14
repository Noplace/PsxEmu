/*****************************************************************************************************************
* Copyright (c) 2026 Khalid Ali Al-Kooheji                                                                       *
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

// Input movies: a recording of what every pad held on every frame from power-on, played back
// into the ports in place of the host's devices. The format read is BizHawk's .bk2, which is
// what PlayStation tool-assisted runs are published in.
//
// A .bk2 is a zip archive of text files:
//
//   Header.txt         "Key value" lines: Platform, Core (Octoshock or Nymashock), GameName,
//                      Author, rerecordCount, StartsFromSavestate, PAL...
//   Input Log.txt      [Input], a "LogKey:" line naming every column, one "|...|" line a frame
//   SyncSettings.json  what was plugged in where, as the core that recorded it wrote it down
//
// The log key is "#" before each group and "|" after each name:
//
//   LogKey:#Disc Select|Open|Close|Reset|#P1 LStick X|P1 LStick Y|...|P1 Up|P1 Down|...|
//   |    1,...|  128,  128,  128,  128,.................|
//
// and in a frame's line a button is one character - '.' for up, anything else for down - while
// an axis is a number padded to five places with a comma after it. Groups are separated by '|'.
// Nothing in a line says which a column is: BizHawk knows from its own core, so here it is told
// by the column's name, and for a name not known by looking at what the first line holds there.
//
// The two cores name the same controls differently ("P1 Cross" and "P1 X", "P1 LStick X" and
// "P1 Left Stick Left / Right", "Disc Select" from 1 and "Disk Index" from 0) and number their
// players differently behind a multitap. All of that is settled when the file is read: a Movie
// holds pads in Sio's own vocabulary, on the port and multitap socket each one plays.
//
// What a movie cannot carry is the machine it was recorded on. It is input by frame number and
// nothing else, so it stays in step only for as long as this emulator reaches every pad read on
// the same frame BizHawk's core did - the same BIOS, the same disc, and every load taking the
// same number of frames. A frame's difference anywhere and everything after it lands on the
// wrong frame. MoviePlayer::set_offset moves the whole movie, which is as much as can be done
// about a boot that takes a different number of frames; it does nothing for a load that does.
//
// Nothing in the first half of this file knows what a System is, so a movie is read and stepped
// through with no machine at all (tools/movie_test). ApplyMoviePads and the two helpers under it
// are the only part that touches one.

#include <cstdint>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

class System;

// What a movie has in a port, or in one of a multitap's sockets.
enum class MovieDevice : uint8_t {
  kNone,
  kDigital,
  kDualAnalog,
  kDualShock,
  // Something this cannot play back: a mouse, a neGcon, a light gun. A movie with one is refused.
  kUnsupported,
};

// One pad on one frame, as Sio takes it: its buttons, and the sticks as the pad's own bytes.
struct MoviePad {
  uint16_t buttons = 0;   // Sio::Button bits
  uint8_t left_x = 0x80;
  uint8_t left_y = 0x80;
  uint8_t right_x = 0x80;
  uint8_t right_y = 0x80;
  bool analog = false;    // the ANALOG button is held
};

// The console's own controls on one frame.
struct MovieConsole {
  enum : uint8_t { kReset = 1 << 0, kPower = 1 << 1, kOpen = 1 << 2, kClose = 1 << 3 };
  uint8_t buttons = 0;
  // Which disc of the game's set the tray holds: 1 the first, 0 none, -1 whichever it held
  // already. Read only while the lid is open, the way both cores read it.
  int8_t disc = 1;
};

struct Movie {
  static const int kPorts = 2;
  static const int kSockets = 4;   // behind a multitap: A-D

  // One "P<n>" group of the log: which socket it plays, and what is plugged in there.
  struct Player {
    int number = 0;   // n
    int port = 0;
    int socket = 0;   // 0 without a multitap
    MovieDevice device = MovieDevice::kNone;
    std::string device_name;   // as the movie called it, for saying what is not supported
    // Mednafen's Dual Analog, which both of BizHawk's cores are built on, has no digital mode:
    // it answers as an analog pad (73h) from power-on, and its MODE button does nothing. A
    // movie made with one has sticks in it and never a press of MODE, so here the pad is kept
    // in analog mode for it and the button is not passed on.
    bool always_analog = false;
  };

  // Header.txt.
  std::string platform;
  std::string core;
  std::string game;
  std::string author;
  std::string emulator;
  uint64_t rerecords = 0;
  bool pal = false;
  bool from_savestate = false;
  bool from_saveram = false;

  // The disc it was recorded on, as the header names it (SHA1, SHA256, MD5 or CRC32 - whichever
  // is there), in lower case. For a PlayStation disc BizHawk writes its own eight-digit id,
  // BizHawkDiscId. Empty if the header does not say: BizHawk 1.x wrote none.
  std::string disc_hash;
  // The BIOS it was recorded on: the SHA-1 of the image, in lower-case hex (the header's
  // PSX_Firmware_J, _U or _E). Empty if the header does not say. A movie is in step only on
  // that BIOS - another takes a different number of frames to reach the game.
  std::string bios_sha1;

  std::vector<Player> players;
  bool multitap[kPorts] = { false, false };
  bool memory_card[kPorts] = { true, false };
  // A movie that starts from saved cards (StartsFromSaveRam, MovieSaveRam.bin): each slot's
  // 128 KB image, empty for a slot that starts with a new card.
  static const size_t kCardSize = 128 * 1024;
  std::vector<uint8_t> card[kPorts];
  // What a new card is. Octoshock's is all zeroes, which a game sees as a card that has never
  // been formatted - the second card of a real Octoshock movie's MovieSaveRam.bin is exactly
  // that. Nymashock's is taken to be formatted, as Mednafen's own are; not seen in a file.
  bool new_cards_formatted = true;
  // Columns that were read past and not used - a name this does not know.
  std::vector<std::string> ignored;

  uint32_t frames() const { return static_cast<uint32_t>(console.size()); }
  const MoviePad& pad(uint32_t frame, size_t player) const {
    return pads[static_cast<size_t>(frame) * players.size() + player];
  }

  // frames() of each: `pads` holds players.size() of them a frame, in players' order.
  std::vector<MoviePad> pads;
  std::vector<MovieConsole> console;

  // "Crash Bandicoot by someone, 123,456 frames (Octoshock)" - for a notification or a log.
  std::string Summary() const;
};

// Reads a .bk2. False with the reason in `error`: not an archive, not a PlayStation movie, one
// that starts from a save state, a device that cannot be played back, a line that does not parse.
bool LoadBk2(const std::string& path, Movie* movie, std::string* error);
// The same from the archive's bytes.
bool ParseBk2(const uint8_t* data, size_t size, Movie* movie, std::string* error);
// ...and from the files inside it. `sync_settings` may be empty: the players are then worked
// out from the log's columns alone. `save_ram` is MovieSaveRam.bin, for a movie whose header
// says it starts from saved cards: 128 KB for each slot that has a card, in the slots' order.
bool ParseBk2Text(const std::string& header, const std::string& input_log,
                  const std::string& sync_settings, Movie* movie, std::string* error,
                  const std::vector<uint8_t>* save_ram = nullptr);

// The SHA-1 of some bytes, in lower-case hex: what a movie's header names its BIOS by.
std::string Sha1Hex(const uint8_t* data, size_t size);

// How BizHawk names a PlayStation disc (its DiscHasher's Calculate_PSX_BizIDHash): a CRC-32 over
// the table of contents - the session's format, the first and last track numbers, then control,
// presence and position for each of a hundred entries, the last of them the lead-out - and the
// first 26 sectors whole. Its game database is keyed on this, and a movie that names its disc
// names it by this. Checked against that database on discs here: Wild Arms 15102200, Metal Gear
// Solid - VR Missions 2E8CE37E, Air Combat 484EA15F.
//
// It takes the table of contents as the image gives it, so two descriptors of one disc that
// disagree about where a music track starts give two ids: Air Combat's .ccd gives the
// database's and its .cue does not. A disc of one track has nothing to disagree about.
uint32_t BizHawkDiscId(const class Disc& disc);

// Whether a movie was recorded on the disc that is mounted, as far as the two can say.
struct MovieDiscCheck {
  enum Verdict { kCannotTell, kSame, kDifferent };
  // By BizHawk's id, when the movie names its disc by one.
  Verdict by_hash = kCannotTell;
  std::string disc_id;       // the mounted disc's, eight hex digits
  // By serial, when the movie starts from saved cards: a save's file name carries the serial of
  // the disc that made it ("BISLPM-86247..."), and a run that starts from one was made on that
  // game. It tells one game and region from another, not two pressings of one disc.
  Verdict by_serial = kCannotTell;
  std::string disc_serial;                 // "SLPM-86247", empty if the disc has none
  std::vector<std::string> card_serials;   // the serials the movie's saves were made under
  // One line saying all of that, for a log or a message.
  std::string Describe(const Movie& movie) const;
};
MovieDiscCheck CheckMovieDisc(const Movie& movie, class Disc& disc);

// A movie being played: where it has got to, and what the lid is doing.
//
// Before each frame the host calls Next and does what the step says to the machine - the things a
// player cannot do itself, since they need the BIOS and the disc images - and then ApplyMoviePads
// with pads(), which is the frame's input.
class MoviePlayer {
 public:
  // What to do to the machine before this frame runs, in this order.
  struct Step {
    bool reset = false;     // power the console off and on, the disc and the cards left in
    bool eject = false;     // the lid opened
    int insert = 0;         // the lid closed on this disc of the set, 1 the first; 0 nothing
    bool finished = false;  // the movie ran out: this frame is the first without it
  };

  void Start(Movie movie);
  void Stop();
  bool playing() const { return playing_; }
  const Movie& movie() const { return movie_; }

  // Frames to wait before the movie's first, when this machine reaches the game's first pad read
  // later than the one it was recorded on; negative leaves that many of the movie's frames out.
  // Set after Start, which puts it back to none, and before the first Next.
  void set_offset(int frames) { offset_ = frames; }

  // The disc to boot with, 1 the first of the set, or 0 for an empty tray: what the movie's first
  // frame leaves in it.
  int initial_disc() const;

  // The next frame. `frame_count` is the GPU's (Gpu::frame_count): a second call before it has
  // moved is the same frame asked for again - the debugger halted in the middle of it - and
  // changes nothing.
  Step Next(uint64_t frame_count);

  // The frame Next last moved to: each player's pad, in movie().players' order. Neutral before
  // the movie's first frame, and whenever nothing is playing.
  const std::vector<MoviePad>& pads() const { return current_; }
  // ...and whether each player's ANALOG button went down on it.
  const std::vector<bool>& analog_pressed() const { return analog_pressed_; }

  // The movie's frame the last Next gave, from 0 - and frames() once it has finished.
  uint32_t frame() const { return frame_; }

 private:
  Movie movie_;
  bool playing_ = false;
  int offset_ = 0;
  int64_t position_ = 0;   // frames asked for since Start
  uint32_t frame_ = 0;
  bool have_last_ = false;
  uint64_t last_frame_count_ = 0;
  bool lid_open_ = false;
  int selected_disc_ = 1;
  std::vector<MoviePad> current_;
  std::vector<bool> analog_held_;
  std::vector<bool> analog_pressed_;
};

// The timing a movie is played under, laid over whatever the settings say for as long as it
// plays: the drive timed as Mednafen's (EmuConfig::cdrom_mednafen_timing), since every load has
// to take the frames it took in BizHawk, and the interpreter with none of the optional timing
// models, since each of them moves the frame the BIOS reaches the game on - the instruction
// cache alone puts the first logo 112 frames later. Nothing here is stored.
//
// It is the nearest this machine gets to BizHawk's, not the same: measured against the
// recording of a real movie, each event of the first minute lands 30 to 45 frames early
// (Docs/Bugs-Found.md, bug 151).
void ApplyMovieTiming(struct EmuConfig& config);

// Plugs in what the movie has in each port and sets every pad for the frame the player is on.
// Returns every pad's buttons ORed together, for cheats that test them.
uint16_t ApplyMoviePads(System& system, const MoviePlayer& player);

// MoviePlayer::Step::reset: the machine made afresh on `bios_path`, with the disc and the memory
// cards that were in it put back. False if the machine could not be initialised.
bool ResetForMovie(System& system, const char* bios_path);

// The memory cards a movie starts with, in each slot the movie has one in: the card saved in the
// movie if it has one for the slot, and otherwise a new one as the core that recorded it makes
// them (Movie::new_cards_formatted). Written to `directory` as movie_card1.mcr and
// movie_card2.mcr over whatever was there, with nothing in a slot the movie has no card in. A
// movie's saves never touch the player's own cards.
bool InsertMovieCards(System& system, const Movie& movie, const std::string& directory,
                      std::string* error);

}  // namespace psx
}  // namespace emulation
