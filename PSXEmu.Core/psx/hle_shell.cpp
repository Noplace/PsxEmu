// The HLE BIOS's shell: what the machine shows with no game to boot. PSXEmu's own, not a copy of
// Sony's - an intro, a menu, a memory card manager and a CD player.
//
// It is host code, like the rest of the HLE BIOS, but it works the machine the way a program
// would: it draws by building a display list in RAM and sending it to the GPU by DMA, reads the
// pads over the controller port, plays audio CDs by sending the CD-ROM controller its commands
// and taking its interrupts, and changes memory cards through the cards themselves. What it
// remembers between frames is in RAM (ShellVars), so a save state taken in it resumes in it.
//
// It runs with interrupts off, once a vblank: the kShell trap looks at the vblank bit, and does a
// frame when it is set. A PlayStation disc put in while it runs boots, as on a console.

#include "psx/psx.h"
#include "psx/hle_bios.h"
#include "psx/mc_directory.h"

namespace emulation {
namespace psx {

namespace {

// ---- The font ------------------------------------------------------------------------------------
// 5x7 glyphs, a row a byte, bit 4 the leftmost pixel, for 20h-7Eh and then the shell's own symbols
// from 80h: the four face buttons, a cursor, pause, stop and a note. Drawn in 8x8 cells.
const uint8_t kFont[][7] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x04,0x04,0x04,0x04,0x04,0x00,0x04},  // space !
  {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},  // " #
  {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, {0x18,0x19,0x02,0x04,0x08,0x13,0x03},  // $ %
  {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, {0x0C,0x04,0x08,0x00,0x00,0x00,0x00},  // & '
  {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, {0x08,0x04,0x02,0x02,0x02,0x04,0x08},  // ( )
  {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},  // * +
  {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},  // , -
  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {0x00,0x01,0x02,0x04,0x08,0x10,0x00},  // . /
  {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},  // 0 1
  {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},  // 2 3
  {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},  // 4 5
  {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},  // 6 7
  {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},  // 8 9
  {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},  // : ;
  {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},  // < =
  {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},  // > ?
  {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11},  // @ A
  {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},  // B C
  {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},  // D E
  {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},  // F G
  {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},  // H I
  {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11},  // J K
  {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},  // L M
  {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},  // N O
  {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},  // P Q
  {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},  // R S
  {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},  // T U
  {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},  // V W
  {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},  // X Y
  {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},  // Z [
  {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},  // \ ]
  {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},  // ^ _
  {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},  // ` a
  {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}, {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E},  // b c
  {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}, {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},  // d e
  {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E},  // f g
  {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},  // h i
  {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, {0x10,0x10,0x12,0x14,0x18,0x14,0x12},  // j k
  {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x00,0x00,0x1A,0x15,0x15,0x11,0x11},  // l m
  {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},  // n o
  {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01},  // p q
  {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E},  // r s
  {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},  // t u
  {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},  // v w
  {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},  // x y
  {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, {0x02,0x04,0x04,0x08,0x04,0x04,0x02},  // z {
  {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, {0x08,0x04,0x04,0x02,0x04,0x04,0x08},  // | }
  {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00},  // ~ (7Fh)
  {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}, {0x00,0x0E,0x11,0x11,0x11,0x0E,0x00},  // 80h cross, circle
  {0x00,0x1F,0x11,0x11,0x11,0x1F,0x00}, {0x00,0x04,0x0A,0x11,0x11,0x1F,0x00},  // 82h square, triangle
  {0x08,0x0C,0x0E,0x0F,0x0E,0x0C,0x08}, {0x00,0x1B,0x1B,0x1B,0x1B,0x1B,0x00},  // 84h cursor, pause
  {0x00,0x1F,0x1F,0x1F,0x1F,0x1F,0x00}, {0x0F,0x09,0x09,0x09,0x1B,0x1B,0x00},  // 86h stop, note
};
const int kGlyphCount = static_cast<int>(sizeof(kFont) / sizeof(kFont[0]));

// The symbols, as characters in a string.
#define CROSS "\x80"
#define CIRCLE "\x81"
#define SQUARE "\x82"
#define TRIANGLE "\x83"
#define CURSOR "\x84"
#define PAUSE_MARK "\x85"
#define STOP_MARK "\x86"
#define NOTE "\x87"

// ---- Where things are ----------------------------------------------------------------------------
const uint32_t kShellList = 0x00100000;   // the display list, in RAM nothing else is using
const int kWidth = 320;
const int kHeight = 240;
// The font and the save icons share a 15-bit texture page at VRAM x=640: the font at u 0-127,
// v 0-63, the icons at u 128-255 - eight across, a row of eight per 16 lines, a 64-line band per
// animation frame.
const uint32_t kTexturePage = 0x10A;      // x=640, 15-bit
const int kIconBase = 128;

// Buttons, as the pad sends them (inverted here: set means held).
const uint32_t kSelect = 0x0001, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020, kDown = 0x0040,
               kLeft = 0x0080, kL1 = 0x0400, kR1 = 0x0800, kTriangle = 0x1000, kCircle = 0x2000,
               kCross = 0x4000, kSquare = 0x8000;

enum Screen : uint32_t { kIntro, kMenu, kCards, kPlayer };
enum Dialog : uint32_t { kNoDialog, kActions, kConfirmDelete, kConfirmFormat, kConfirmCopy };

const int kVisibleSaves = 7;
const int kVisibleTracks = 11;

// What the shell remembers from frame to frame, in RAM at HleBios::kShellVars.
struct ShellVars {
  uint32_t screen, frame, screen_frame;
  uint32_t held, pressed, repeat;
  uint32_t menu_cursor;
  uint32_t card_side, card_cursor[2], card_scroll[2];
  uint32_t dialog, dialog_cursor;
  uint32_t card_hash[2];
  uint32_t font_loaded, back_buffer, list_sent;
  uint32_t disc_timer;
  uint32_t message_timer;
  char message[48];
  // The CD player and its driver.
  uint32_t track_cursor, track_scroll;
  uint32_t playing, paused, demuted;
  uint32_t cd_busy, cd_second, cd_poll;
  uint32_t queue_count;
  uint32_t queue[8][3];   // command, parameter count, parameter
  uint8_t location[8];    // the last GetlocP
};

uint32_t Rgb(int r, int g, int b) { return (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16); }
uint32_t Xy(int x, int y) { return (static_cast<uint32_t>(y & 0xFFFF) << 16) | (x & 0xFFFF); }
uint8_t ToBcd(int value) { return static_cast<uint8_t>(((value / 10) << 4) | (value % 10)); }
int FromBcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }

// A save's title in the shell's font: the card's Shift-JIS, decoded by mcdir and narrowed, is
// ASCII for most Western saves; a Japanese title would be mostly question marks, so the save's
// filename stands in for one.
std::string Printable(const mcdir::Save& save) {
  std::string text;
  int unknown = 0;
  for (wchar_t c : save.title) {
    if (c >= 0x20 && c < 0x7F) {
      text.push_back(static_cast<char>(c));
    } else if (c == 0x3000) {
      text.push_back(' ');
    } else {
      text.push_back('?');
      ++unknown;
    }
  }
  while (!text.empty() && text.back() == ' ')
    text.pop_back();
  if (text.empty() || unknown * 2 > static_cast<int>(text.size()))
    return save.filename;
  return text;
}

}  // namespace

// ---- The shell -----------------------------------------------------------------------------------

class HleShell {
 public:
  explicit HleShell(HleBios& bios) : b_(bios), v_(*bios.At<ShellVars>(HleBios::kShellVars)) {}

  void Start();
  void Frame();

 private:
  // The display list: packets of primitives, written to RAM and sent by DMA.
  void Prim(std::initializer_list<uint32_t> words) {
    if (packet_.size() + words.size() > 240)
      FlushPacket();
    packet_.insert(packet_.end(), words.begin(), words.end());
  }
  void FlushPacket() {
    if (!packet_.empty())
      packets_.push_back(std::move(packet_));
    packet_.clear();
  }
  void SendList();
  bool ListDone() { return (b_.ReadIo32(0x1F8010A8) & 0x01000000) == 0; }

  // Drawing.
  void Environment(int buffer);
  void Gradient(uint32_t top, uint32_t bottom);
  void Panel(int x, int y, int w, int h, uint32_t color);
  void Box(int x, int y, int w, int h, uint32_t color);
  void Text(int x, int y, const std::string& text, uint32_t color, bool shadow = true);
  void BigText(int x, int y, const std::string& text, int scale, uint32_t color);
  void Centered(int y, const std::string& text, uint32_t color) {
    Text((kWidth - static_cast<int>(text.size()) * 8) / 2, y, text, color);
  }
  void UploadFont();
  void UploadIcons(int card, const std::vector<mcdir::Save>& saves);
  void Icon(int x, int y, int slot, int frames);

  // The screens.
  void Intro();
  void Menu();
  void Cards();
  void Player();
  void DrawDialog(const std::vector<std::string>& lines, const std::vector<std::string>& choices);
  void Message(const std::string& text);
  void DiscStatus(int y);

  // Input and devices.
  uint32_t ReadButtons();
  bool Pressed(uint32_t mask) const { return (v_.pressed & mask) != 0; }
  MC* Card(int side) { return b_.Card(static_cast<uint32_t>(side) << 4); }
  std::vector<mcdir::Save> Saves(int side);
  void WriteCard(MC* card, const std::vector<uint8_t>& image);
  bool CardChanged(int side);
  void CdQueue(uint8_t command, int count = 0, uint8_t parameter = 0);
  void CdPoll();
  std::vector<int> AudioTracks();
  bool BootableDisc();

  HleBios& b_;
  ShellVars& v_;
  std::vector<uint32_t> packet_;
  std::vector<std::vector<uint32_t>> packets_;
};

void HleBios::ShellStart() { HleShell(*this).Start(); }
void HleBios::ShellFrame() { HleShell(*this).Frame(); }

void HleShell::Start() {
  memset(&v_, 0, sizeof(v_));
  static_assert(sizeof(ShellVars) <= 0x1000, "ShellVars outgrew its page");
  CpuContext& c = b_.cpu();
  c.ctrl.SR.raw = 0x40000000;   // kernel mode, interrupts off: the shell polls for its vblank
  b_.WriteIo32(0x1F801074, 0);
  b_.WriteIo32(0x1F801070, 0);
  // The display: 320x240, both buffers cleared.
  b_.WriteIo32(0x1F801814, 0x00000000);
  b_.WriteIo32(0x1F801814, 0x06C60260);
  b_.WriteIo32(0x1F801814, 0x07040010);
  b_.WriteIo32(0x1F801814, 0x08000001);
  b_.WriteIo32(0x1F801814, 0x05000000);
  b_.WriteIo32(0x1F801814, 0x03000000);
  // CD audio into the SPU at full volume, for the player.
  b_.WriteIo16(0x1F801DAA, static_cast<uint16_t>(b_.ReadIo16(0x1F801DAA) | 0xC001));
  b_.WriteIo16(0x1F801DB0, 0x7FFF);
  b_.WriteIo16(0x1F801DB2, 0x7FFF);
  v_.screen = kIntro;
  b_.cpu().pc = HleBios::TrapAddress(HleBios::kShell);
}

void HleShell::Frame() {
  // One frame per vblank, taken off the interrupt controller by hand.
  if ((b_.ReadIo32(0x1F801070) & 1) == 0) {
    b_.cost_ = 2000;
    return;
  }
  b_.WriteIo32(0x1F801070, ~1u);
  b_.cost_ = 20000;   // a frame's worth of shell is about this much CPU on a console's terms
  ++v_.frame;
  ++v_.screen_frame;

  // Input: an edge when a button goes down, and up and down repeating while held.
  const uint32_t held = ReadButtons();
  v_.pressed = held & ~v_.held;
  if (held & (kUp | kDown | kLeft | kRight)) {
    if (++v_.repeat > 18 && (v_.repeat % 4) == 0)
      v_.pressed |= held & (kUp | kDown | kLeft | kRight);
  } else {
    v_.repeat = 0;
  }
  v_.held = held;

  CdPoll();

  // A PlayStation disc put in boots, a second after it is seen, as a console's would.
  if (v_.screen != kIntro && ++v_.disc_timer >= 60) {
    v_.disc_timer = 0;
    if (BootableDisc()) {
      b_.Reset();
      return;
    }
  }

  // The last frame's list on screen once the GPU has it all; then this frame's.
  if (v_.list_sent != 0 && !ListDone())
    return;
  if (v_.list_sent != 0) {
    b_.WriteIo32(0x1F801814, 0x05000000 | ((v_.back_buffer * 256u) << 10));
    v_.back_buffer ^= 1;
  }
  packets_.clear();
  packet_.clear();
  if (v_.font_loaded == 0) {
    UploadFont();
    v_.font_loaded = 1;
  }
  Environment(static_cast<int>(v_.back_buffer));
  switch (v_.screen) {
    case kIntro: Intro(); break;
    case kMenu: Menu(); break;
    case kCards: Cards(); break;
    case kPlayer: Player(); break;
    default: v_.screen = kMenu; break;
  }
  if (v_.message_timer > 0) {
    --v_.message_timer;
    Panel(20, 206, 280, 18, Rgb(0x60, 0x10, 0x10));
    Centered(211, v_.message, Rgb(0xFF, 0xE0, 0xA0));
  }
  SendList();
}

// ---- The display list ----------------------------------------------------------------------------

void HleShell::SendList() {
  FlushPacket();
  uint32_t at = kShellList;
  for (size_t i = 0; i < packets_.size(); ++i) {
    const std::vector<uint32_t>& p = packets_[i];
    const uint32_t next = at + 4 + static_cast<uint32_t>(p.size()) * 4;
    const uint32_t header = (static_cast<uint32_t>(p.size()) << 24) |
                            (i + 1 < packets_.size() ? (next & 0xFFFFFF) : 0xFFFFFF);
    b_.Write32(at, header);
    b_.WriteBlock(at + 4, p.data(), static_cast<uint32_t>(p.size() * 4));
    at = next;
  }
  if (packets_.empty())
    return;
  b_.WriteIo32(0x1F801814, 0x04000002);   // DMA from the CPU to GP0
  b_.WriteIo32(0x1F8010F0, b_.ReadIo32(0x1F8010F0) | 0x800);
  b_.WriteIo32(0x1F8010A0, kShellList);
  b_.WriteIo32(0x1F8010A4, 0);
  b_.WriteIo32(0x1F8010A8, 0x01000401);   // linked list, from RAM, start
  v_.list_sent = 1;
}

void HleShell::Environment(int buffer) {
  const uint32_t y0 = static_cast<uint32_t>(buffer) * 256;
  Prim({ 0xE1000000 | kTexturePage | 0x200 | 0x400,   // dither, may draw where it shows
         0xE2000000,
         0xE3000000 | (y0 << 10),
         0xE4000000 | ((y0 + kHeight - 1) << 10) | (kWidth - 1),
         0xE5000000 | ((y0 & 0x7FF) << 11),
         0xE6000000 });
}

void HleShell::Gradient(uint32_t top, uint32_t bottom) {
  Prim({ 0x38000000 | top, Xy(0, 0), top, Xy(kWidth, 0), bottom, Xy(0, kHeight), bottom,
         Xy(kWidth, kHeight) });
}

void HleShell::Panel(int x, int y, int w, int h, uint32_t color) {
  Prim({ 0x62000000 | color, Xy(x, y), Xy(w, h) });   // half-transparent
}

void HleShell::Box(int x, int y, int w, int h, uint32_t color) {
  Prim({ 0x60000000 | color, Xy(x, y), Xy(w, h) });
}

void HleShell::Text(int x, int y, const std::string& text, uint32_t color, bool shadow) {
  for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
    const uint32_t tint = pass == 0 ? Rgb(0x10, 0x10, 0x18) : color;
    int cx = x + (pass == 0 ? 1 : 0);
    const int cy = y + (pass == 0 ? 1 : 0);
    for (unsigned char ch : text) {
      const int glyph = ch - 0x20;
      if (glyph > 0 && glyph < kGlyphCount) {
        const uint32_t u = (glyph % 16) * 8, v = (glyph / 16) * 8;
        Prim({ 0x74000000 | tint, Xy(cx, cy), (v << 8) | u });
      }
      cx += 8;
    }
  }
}

void HleShell::BigText(int x, int y, const std::string& text, int scale, uint32_t color) {
  const int size = 8 * scale;
  for (unsigned char ch : text) {
    const int glyph = ch - 0x20;
    if (glyph > 0 && glyph < kGlyphCount) {
      const uint32_t u = (glyph % 16) * 8, v = (glyph / 16) * 8;
      Prim({ 0x2C000000 | color, Xy(x, y), (v << 8) | u,
             Xy(x + size, y), (kTexturePage << 16) | (v << 8) | (u + 8),
             Xy(x, y + size), ((v + 8) << 8) | u,
             Xy(x + size, y + size), ((v + 8) << 8) | (u + 8) });
    }
    x += size;
  }
}

void HleShell::UploadFont() {
  // White where the glyph is, 0000h - transparent - everywhere else, three rows a packet.
  std::vector<uint16_t> pixels(128 * 64, 0);
  for (int g = 0; g < kGlyphCount; ++g)
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        if (kFont[g][row] & (0x10 >> col))
          pixels[((g / 16) * 8 + row) * 128 + (g % 16) * 8 + 1 + col] = 0x7FFF;
  FlushPacket();
  for (int y = 0; y < 64; y += 3) {
    const int rows = y + 3 <= 64 ? 3 : 64 - y;
    Prim({ 0xA0000000, Xy(640, y), Xy(128, rows) });
    for (int i = 0; i < rows * 128; i += 2)
      packet_.push_back(pixels[y * 128 + i] | (static_cast<uint32_t>(pixels[y * 128 + i + 1]) << 16));
    FlushPacket();
  }
}

void HleShell::UploadIcons(int card, const std::vector<mcdir::Save>& saves) {
  FlushPacket();
  for (size_t s = 0; s < saves.size() && s < 15; ++s) {
    const int slot = card * 15 + static_cast<int>(s);
    for (int f = 0; f < saves[s].icon_frames && f < 3; ++f) {
      const int x = 640 + kIconBase + (slot % 8) * 16;
      const int y = f * 64 + (slot / 8) * 16;
      packet_.push_back(0xA0000000);
      packet_.push_back(Xy(x, y));
      packet_.push_back(Xy(16, 16));
      for (int i = 0; i < 256; i += 2) {
        uint32_t two = 0;
        for (int k = 0; k < 2; ++k) {
          const uint32_t argb = saves[s].icons[f * 256 + i + k];
          uint16_t p = 0;
          if ((argb >> 24) != 0) {
            p = static_cast<uint16_t>(((argb >> 19) & 0x1F) | (((argb >> 11) & 0x1F) << 5) |
                                      (((argb >> 3) & 0x1F) << 10));
            if (p == 0)
              p = 0x8000;   // black, but not see-through
          }
          two |= static_cast<uint32_t>(p) << (16 * k);
        }
        packet_.push_back(two);
      }
      FlushPacket();
    }
  }
}

void HleShell::Icon(int x, int y, int slot, int frames) {
  const int frame = frames > 1 ? static_cast<int>((v_.frame / 16) % frames) : 0;
  const uint32_t u = kIconBase + (slot % 8) * 16;
  const uint32_t v = frame * 64 + (slot / 8) * 16;
  Prim({ 0x7C808080, Xy(x, y), (v << 8) | u });
}

// ---- Screens -------------------------------------------------------------------------------------

void HleShell::Intro() {
  // Black, the name fading in on a sweep of light, held, and out: about three seconds. Any button
  // skips it.
  const int t = static_cast<int>(v_.screen_frame);
  int level = t < 60 ? t * 128 / 60 : (t < 150 ? 128 : (t < 180 ? (180 - t) * 128 / 30 : 0));
  Gradient(Rgb(0, 0, 0), Rgb(level / 8, level / 10, level / 4));
  const int sweep = (t * 6) % 480 - 80;
  Prim({ 0x38000000 | Rgb(0, 0, 0), Xy(sweep - 60, 100), Rgb(level / 2, level / 2, level),
         Xy(sweep, 100), Rgb(0, 0, 0), Xy(sweep - 60, 140), Rgb(level / 2, level / 2, level),
         Xy(sweep, 140) });
  const uint32_t name = Rgb(level, level, level + level / 4 > 255 ? 255 : level + level / 4);
  BigText((kWidth - 6 * 32) / 2, 88, "PSXEmu", 4, name);
  if (t > 40)
    Centered(136, "H L E   B I O S", Rgb(level * 3 / 4, level * 3 / 4, level));
  if (t >= 180 || (t > 10 && v_.pressed != 0)) {
    v_.screen = kMenu;
    v_.screen_frame = 0;
  }
}

void HleShell::DiscStatus(int y) {
  std::string text = "NO DISC";
  Cdrom& cdrom = b_.system_->cdrom();
  if (cdrom.disc_loaded()) {
    const std::vector<int> tracks = AudioTracks();
    if (!tracks.empty())
      text = NOTE " AUDIO CD - " + std::to_string(tracks.size()) + " TRACKS";
    else if (b_.OpenDisc())
      text = "DISC: " + b_.system_->iso().volume_id() + " (NOT BOOTABLE)";
    else
      text = "UNKNOWN DISC";
  }
  Centered(y, text, Rgb(0x90, 0xA0, 0xC0));
}

void HleShell::Menu() {
  Gradient(Rgb(0x10, 0x18, 0x40), Rgb(0x50, 0x10, 0x48));
  BigText((kWidth - 6 * 24) / 2, 28, "PSXEmu", 3, Rgb(0xC0, 0xC8, 0xFF));
  Centered(58, "HLE BIOS", Rgb(0x80, 0x88, 0xC0));
  static const char* const kItems[2] = { "MEMORY CARD", "CD PLAYER" };
  if (Pressed(kUp | kDown))
    v_.menu_cursor ^= 1;
  for (uint32_t i = 0; i < 2; ++i) {
    const int y = 100 + static_cast<int>(i) * 34;
    const bool on = v_.menu_cursor == i;
    const int pulse = on ? 0x30 + static_cast<int>((v_.frame * 4) % 64) : 0;
    Panel(70, y - 8, 180, 24, on ? Rgb(0x40 + pulse, 0x50 + pulse, 0x90) : Rgb(0x10, 0x10, 0x30));
    if (on)
      Text(80, y, CURSOR, Rgb(0xFF, 0xFF, 0x80));
    Text(100, y, kItems[i], on ? Rgb(0xFF, 0xFF, 0xFF) : Rgb(0x90, 0x90, 0xB0));
  }
  DiscStatus(186);
  Centered(222, CROSS " SELECT", Rgb(0xA0, 0xA0, 0xC0));
  if (Pressed(kCross | kStart)) {
    v_.screen = v_.menu_cursor == 0 ? kCards : kPlayer;
    v_.screen_frame = 0;
    v_.dialog = kNoDialog;
    v_.card_hash[0] = v_.card_hash[1] = 0;
  }
}

// ---- The memory card manager ---------------------------------------------------------------------

std::vector<mcdir::Save> HleShell::Saves(int side) {
  MC* card = Card(side);
  if (card == nullptr || !mcdir::IsFormatted(card->data()))
    return {};
  return mcdir::List(card->data(), false);
}

// Whether the card's directory is not what the icons in VRAM were made from: a hash of block 0's
// directory frames, and whether it is there at all.
bool HleShell::CardChanged(int side) {
  MC* card = Card(side);
  uint32_t hash = 2166136261u;
  if (card != nullptr) {
    const uint8_t* data = card->data();
    for (size_t i = 0; i < mcdir::kFrameSize * 16; ++i)
      hash = (hash ^ data[i]) * 16777619u;
  } else {
    hash = 1;
  }
  if (hash == v_.card_hash[side])
    return false;
  v_.card_hash[side] = hash;
  return true;
}

void HleShell::WriteCard(MC* card, const std::vector<uint8_t>& image) {
  for (int frame = 0; frame < 1024; ++frame) {
    const size_t at = static_cast<size_t>(frame) * mcdir::kFrameSize;
    if (memcmp(card->data() + at, &image[at], mcdir::kFrameSize) != 0)
      card->WriteSector(static_cast<uint16_t>(frame), &image[at]);
  }
}

void HleShell::Message(const std::string& text) {
  strncpy(v_.message, text.c_str(), sizeof(v_.message) - 1);
  v_.message[sizeof(v_.message) - 1] = 0;
  v_.message_timer = 150;
}

void HleShell::DrawDialog(const std::vector<std::string>& lines,
                          const std::vector<std::string>& choices) {
  const int h = 24 + static_cast<int>(lines.size()) * 12 + static_cast<int>(choices.size()) * 14;
  const int y = (kHeight - h) / 2;
  Box(40, y, 240, h, Rgb(0x18, 0x18, 0x38));
  Panel(36, y - 4, 248, h + 8, Rgb(0x60, 0x60, 0xC0));
  int cy = y + 10;
  for (const std::string& line : lines) {
    Centered(cy, line, Rgb(0xFF, 0xFF, 0xFF));
    cy += 12;
  }
  cy += 4;
  for (size_t i = 0; i < choices.size(); ++i) {
    const bool on = v_.dialog_cursor == i;
    if (on)
      Text(96, cy, CURSOR, Rgb(0xFF, 0xFF, 0x80));
    Text(112, cy, choices[i], on ? Rgb(0xFF, 0xFF, 0xFF) : Rgb(0x90, 0x90, 0xB0));
    cy += 14;
  }
}

void HleShell::Cards() {
  Gradient(Rgb(0x08, 0x20, 0x30), Rgb(0x10, 0x08, 0x30));
  Centered(8, "MEMORY CARD", Rgb(0xC0, 0xE0, 0xFF));

  std::vector<mcdir::Save> saves[2] = { Saves(0), Saves(1) };
  for (int side = 0; side < 2; ++side) {
    if (CardChanged(side))
      UploadIcons(side, saves[side]);
    const int count = static_cast<int>(saves[side].size());
    if (static_cast<int>(v_.card_cursor[side]) >= count)
      v_.card_cursor[side] = count > 0 ? count - 1 : 0;
  }

  // Input, unless a dialog has it.
  const int side = static_cast<int>(v_.card_side);
  const int count = static_cast<int>(saves[side].size());
  MC* card = Card(side);
  if (v_.dialog == kNoDialog) {
    if (Pressed(kLeft | kRight | kL1 | kR1))
      v_.card_side ^= 1;
    if (Pressed(kUp) && v_.card_cursor[side] > 0)
      --v_.card_cursor[side];
    if (Pressed(kDown) && static_cast<int>(v_.card_cursor[side]) + 1 < count)
      ++v_.card_cursor[side];
    if (Pressed(kCross) && count > 0) {
      v_.dialog = kActions;
      v_.dialog_cursor = 0;
    } else if (Pressed(kTriangle) && card != nullptr) {
      v_.dialog = kConfirmFormat;
      v_.dialog_cursor = 1;
    }
    if (v_.dialog != kNoDialog)
      v_.pressed = 0;   // the press that opened it is not also its answer
    if (Pressed(kCircle)) {
      v_.screen = kMenu;
      v_.screen_frame = 0;
      return;
    }
  }
  int& cursor = reinterpret_cast<int&>(v_.card_cursor[side]);
  int& scroll = reinterpret_cast<int&>(v_.card_scroll[side]);
  if (cursor < scroll)
    scroll = cursor;
  if (cursor >= scroll + kVisibleSaves)
    scroll = cursor - kVisibleSaves + 1;

  // The two cards.
  for (int s = 0; s < 2; ++s) {
    const int x = s == 0 ? 8 : 164;
    const bool active = s == side;
    Panel(x, 22, 148, 152, active ? Rgb(0x30, 0x50, 0x80) : Rgb(0x18, 0x20, 0x38));
    MC* c = Card(s);
    std::string head = "CARD " + std::to_string(s + 1);
    Text(x + 6, 28, head, active ? Rgb(0xFF, 0xFF, 0xFF) : Rgb(0x90, 0x90, 0xB0));
    if (c == nullptr) {
      Text(x + 30, 90, "NO CARD", Rgb(0xA0, 0xA0, 0xA0));
      continue;
    }
    if (!mcdir::IsFormatted(c->data())) {
      Text(x + 14, 84, "NOT FORMATTED", Rgb(0xFF, 0xC0, 0x80));
      Text(x + 14, 100, TRIANGLE " FORMAT", Rgb(0xA0, 0xA0, 0xC0));
      continue;
    }
    Text(x + 72, 28, std::to_string(mcdir::FreeBlocks(c->data())) + " FREE",
         Rgb(0x80, 0xC0, 0x80));
    const std::vector<mcdir::Save>& list = saves[s];
    if (list.empty())
      Text(x + 38, 90, "NO SAVES", Rgb(0xA0, 0xA0, 0xA0));
    const int first = static_cast<int>(v_.card_scroll[s]);
    for (int i = first; i < static_cast<int>(list.size()) && i < first + kVisibleSaves; ++i) {
      const int y = 42 + (i - first) * 18;
      const bool on = active && i == static_cast<int>(v_.card_cursor[s]);
      if (on)
        Box(x + 2, y - 1, 144, 18, Rgb(0x50 + static_cast<int>((v_.frame * 3) % 48), 0x70, 0xB0));
      if (list[i].icon_frames > 0)
        Icon(x + 4, y, s * 15 + i, list[i].icon_frames);
      std::string name = Printable(list[i]);
      if (name.size() > 15)
        name = name.substr(0, 15);
      Text(x + 24, y + 5, name, on ? Rgb(0xFF, 0xFF, 0xFF) : Rgb(0xB0, 0xB0, 0xC8), !on);
    }
    if (first > 0)
      Text(x + 136, 42, "^", Rgb(0xFF, 0xFF, 0x80));
    if (first + kVisibleSaves < static_cast<int>(list.size()))
      Text(x + 136, 160, "v", Rgb(0xFF, 0xFF, 0x80));
  }

  // What the cursor is on.
  if (count > 0) {
    const mcdir::Save& save = saves[side][v_.card_cursor[side]];
    std::string title = Printable(save);
    Text(8, 180, title.substr(0, 38), Rgb(0xFF, 0xFF, 0xFF));
    Text(8, 192, save.filename + "  " + std::to_string(save.blocks) +
                     (save.blocks == 1 ? " BLOCK" : " BLOCKS"),
         Rgb(0x90, 0xB0, 0xD0));
  }
  Centered(226, CROSS " ACTIONS  " TRIANGLE " FORMAT  L/R CARD  " CIRCLE " BACK",
           Rgb(0xA0, 0xA0, 0xC0));

  // The dialogs.
  if (v_.dialog == kNoDialog)
    return;
  const int other = side ^ 1;
  std::vector<std::string> choices;
  std::vector<std::string> lines;
  switch (v_.dialog) {
    case kActions:
      lines = { Printable(saves[side][v_.card_cursor[side]]).substr(0, 28) };
      choices = { "COPY TO CARD " + std::to_string(other + 1), "DELETE", "CANCEL" };
      break;
    case kConfirmDelete:
      lines = { "DELETE THIS SAVE?" };
      choices = { "DELETE", "CANCEL" };
      break;
    case kConfirmFormat:
      lines = { "FORMAT CARD " + std::to_string(side + 1) + "?", "EVERY SAVE ON IT IS LOST" };
      choices = { "FORMAT", "CANCEL" };
      break;
    case kConfirmCopy:
      lines = { "COPY TO CARD " + std::to_string(other + 1) + "?" };
      choices = { "COPY", "CANCEL" };
      break;
  }
  if (Pressed(kUp) && v_.dialog_cursor > 0)
    --v_.dialog_cursor;
  if (Pressed(kDown) && v_.dialog_cursor + 1 < choices.size())
    ++v_.dialog_cursor;
  const bool chose = Pressed(kCross);
  const uint32_t choice = v_.dialog_cursor;
  if (Pressed(kCircle) || (chose && choice + 1 == choices.size())) {
    v_.dialog = kNoDialog;
    return;
  }
  DrawDialog(lines, choices);
  if (!chose)
    return;
  std::string error;
  switch (v_.dialog) {
    case kActions:
      v_.dialog = choice == 0 ? kConfirmCopy : kConfirmDelete;
      v_.dialog_cursor = 0;
      return;
    case kConfirmDelete: {
      std::vector<uint8_t> image(card->data(), card->data() + mcdir::kCardSize);
      if (mcdir::Delete(&image[0], saves[side][v_.card_cursor[side]].first_block, &error)) {
        WriteCard(card, image);
        Message("DELETED");
      } else {
        Message(error.substr(0, 38));
      }
      break;
    }
    case kConfirmFormat: {
      std::vector<uint8_t> image(mcdir::kCardSize, 0);
      mcdir::Format(&image[0]);
      WriteCard(card, image);
      Message("FORMATTED");
      break;
    }
    case kConfirmCopy: {
      MC* destination = Card(other);
      std::vector<uint8_t> mcs;
      if (destination == nullptr || !mcdir::IsFormatted(destination->data())) {
        Message("NO FORMATTED CARD IN SLOT " + std::to_string(other + 1));
      } else if (mcdir::Export(card->data(), saves[side][v_.card_cursor[side]].first_block,
                               &mcs, &error)) {
        std::vector<uint8_t> image(destination->data(), destination->data() + mcdir::kCardSize);
        if (mcdir::Import(&image[0], mcs, &error)) {
          WriteCard(destination, image);
          Message("COPIED");
        } else {
          Message(error.substr(0, 38));
        }
      } else {
        Message(error.substr(0, 38));
      }
      break;
    }
  }
  v_.dialog = kNoDialog;
}

// ---- The CD player -------------------------------------------------------------------------------

std::vector<int> HleShell::AudioTracks() {
  std::vector<int> tracks;
  Cdrom& cdrom = b_.system_->cdrom();
  if (!cdrom.disc_loaded())
    return tracks;
  const Disc& disc = cdrom.disc();
  for (int i = 0; i < disc.track_count(); ++i)
    if (disc.track(i).type == Disc::kTrackAudio)
      tracks.push_back(i);
  return tracks;
}

void HleShell::CdQueue(uint8_t command, int count, uint8_t parameter) {
  if (v_.queue_count >= 8)
    return;
  v_.queue[v_.queue_count][0] = command;
  v_.queue[v_.queue_count][1] = static_cast<uint32_t>(count);
  v_.queue[v_.queue_count][2] = parameter;
  ++v_.queue_count;
}

// The CD-ROM controller, by polling: take whatever it has answered, acknowledge it, and send the
// next command once the last one has been answered - and, for the ones that answer twice, done.
void HleShell::CdPoll() {
  for (int guard = 0; guard < 4; ++guard) {
    b_.WriteIo8(0x1F801800, 1);
    const uint8_t type = b_.system_->io().Read08(0x1F801803) & 7;
    if (type == 0)
      break;
    uint8_t response[16] = {};
    for (int i = 0; i < 16 && (b_.system_->io().Read08(0x1F801800) & 0x20) != 0; ++i)
      response[i] = b_.system_->io().Read08(0x1F801801);
    b_.WriteIo8(0x1F801800, 1);
    b_.WriteIo8(0x1F801803, 0x1F);
    b_.WriteIo32(0x1F801070, ~4u);
    if (type == 3) {                       // acknowledged: done, unless a second answer follows
      if (v_.cd_busy == 0x11)
        memcpy(v_.location, response, 8);
      if (v_.cd_second == 0)
        v_.cd_busy = 0;
    } else if (type == 2 || type == 5) {   // complete, or an error
      v_.cd_busy = 0;
      v_.cd_second = 0;
    } else if (type == 4) {                // the end of the disc
      v_.playing = 0;
      v_.paused = 0;
    }
  }
  b_.WriteIo8(0x1F801800, 0);
  if (v_.cd_busy != 0 || v_.queue_count == 0)
    return;
  const uint8_t command = static_cast<uint8_t>(v_.queue[0][0]);
  const int count = static_cast<int>(v_.queue[0][1]);
  const uint8_t parameter = static_cast<uint8_t>(v_.queue[0][2]);
  for (uint32_t i = 1; i < v_.queue_count; ++i)
    memcpy(v_.queue[i - 1], v_.queue[i], sizeof(v_.queue[0]));
  --v_.queue_count;
  b_.WriteIo8(0x1F801800, 0);
  if (count > 0)
    b_.WriteIo8(0x1F801802, parameter);
  b_.WriteIo8(0x1F801801, command);
  v_.cd_busy = command;
  v_.cd_second = (command == 0x08 || command == 0x09) ? 1 : 0;   // Stop and Pause answer twice
}

void HleShell::Player() {
  Gradient(Rgb(0x20, 0x08, 0x30), Rgb(0x04, 0x10, 0x28));
  Centered(8, NOTE " CD PLAYER " NOTE, Rgb(0xFF, 0xD0, 0xFF));
  const std::vector<int> tracks = AudioTracks();
  const Disc* disc = b_.system_->cdrom().disc_loaded() ? &b_.system_->cdrom().disc() : nullptr;

  if (Pressed(kCircle)) {
    v_.screen = kMenu;
    v_.screen_frame = 0;
    return;
  }
  if (tracks.empty()) {
    Centered(110, "NO AUDIO CD", Rgb(0xC0, 0xC0, 0xC0));
    Centered(226, CIRCLE " BACK", Rgb(0xA0, 0xA0, 0xC0));
    return;
  }
  const int count = static_cast<int>(tracks.size());
  int& cursor = reinterpret_cast<int&>(v_.track_cursor);
  if (cursor >= count)
    cursor = count - 1;
  if (Pressed(kUp) && cursor > 0)
    --cursor;
  if (Pressed(kDown) && cursor + 1 < count)
    ++cursor;
  if (v_.demuted == 0) {
    CdQueue(0x0C);   // Demute
    v_.demuted = 1;
  }
  auto play = [&](int index) {
    CdQueue(0x03, 1, ToBcd(disc->track(tracks[index]).number));
    v_.playing = 1;
    v_.paused = 0;
    v_.track_cursor = static_cast<uint32_t>(index);
  };
  if (Pressed(kCross | kStart))
    play(cursor);
  if (Pressed(kSquare) && v_.playing) {
    if (v_.paused) {
      CdQueue(0x03);   // carry on from where it is
      v_.paused = 0;
    } else {
      CdQueue(0x09);
      v_.paused = 1;
    }
  }
  if (Pressed(kTriangle)) {
    CdQueue(0x08);
    v_.playing = 0;
    v_.paused = 0;
    memset(v_.location, 0, sizeof(v_.location));
  }
  if (Pressed(kR1) && cursor + 1 < count)
    play(cursor + 1);
  if (Pressed(kL1) && cursor > 0)
    play(cursor - 1);
  // Where the head is, a few times a second.
  if (v_.playing && !v_.paused && ++v_.cd_poll >= 15 && v_.queue_count == 0) {
    v_.cd_poll = 0;
    CdQueue(0x11);   // GetlocP
  }

  // The track list.
  int& scroll = reinterpret_cast<int&>(v_.track_scroll);
  if (cursor < scroll)
    scroll = cursor;
  if (cursor >= scroll + kVisibleTracks)
    scroll = cursor - kVisibleTracks + 1;
  Panel(16, 24, 132, 180, Rgb(0x30, 0x18, 0x50));
  const int playing_track = v_.location[0] != 0 ? FromBcd(v_.location[0]) : 0;
  for (int i = scroll; i < count && i < scroll + kVisibleTracks; ++i) {
    const int y = 30 + (i - scroll) * 16;
    const Disc::Track& t = disc->track(tracks[i]);
    const uint32_t seconds = t.length / 75;
    char line[32];
    snprintf(line, sizeof(line), "%02d   %2u:%02u", t.number, seconds / 60, seconds % 60);
    const bool on = i == cursor;
    if (on)
      Box(18, y - 3, 128, 14, Rgb(0x60, 0x40, 0x90));
    if (v_.playing && t.number == playing_track)
      Text(22, y, NOTE, Rgb(0xFF, 0xD0, 0x60));
    Text(34, y, line, on ? Rgb(0xFF, 0xFF, 0xFF) : Rgb(0xB0, 0xA0, 0xC8), !on);
  }

  // What is playing.
  Panel(160, 24, 144, 180, Rgb(0x18, 0x18, 0x40));
  Text(172, 36, std::to_string(count) + " TRACKS", Rgb(0xA0, 0xA0, 0xD0));
  const char* state = !v_.playing ? STOP_MARK " STOPPED" : (v_.paused ? PAUSE_MARK " PAUSED"
                                                                     : CURSOR " PLAYING");
  Text(172, 60, state, Rgb(0xFF, 0xFF, 0xFF));
  if (v_.playing) {
    char text[16];
    snprintf(text, sizeof(text), "%02d", playing_track);
    Text(172, 84, "TRACK", Rgb(0xA0, 0xA0, 0xD0));
    BigText(172, 96, text, 3, Rgb(0xFF, 0xD0, 0xFF));
    snprintf(text, sizeof(text), "%d:%02d", FromBcd(v_.location[2]), FromBcd(v_.location[3]));
    BigText(172, 136, text, 2, Rgb(0xD0, 0xD0, 0xFF));
  }
  Centered(214, CROSS " PLAY  " SQUARE " PAUSE  " TRIANGLE " STOP", Rgb(0xA0, 0xA0, 0xC0));
  Centered(226, "L1/R1 TRACK  " CIRCLE " BACK", Rgb(0xA0, 0xA0, 0xC0));
}

// ---- Devices -------------------------------------------------------------------------------------

uint32_t HleShell::ReadButtons() {
  uint32_t held = 0;
  for (int port = 0; port < 2; ++port) {
    uint8_t out[5] = { 0x01, 0x42, 0x00, 0x00, 0x00 };
    uint8_t in[5] = {};
    if (b_.system_->sio().ExchangeDirect(port, out, in, 5) >= 5 && in[2] == 0x5A)
      held |= ~(static_cast<uint32_t>(in[3]) | (static_cast<uint32_t>(in[4]) << 8)) & 0xFFFF;
  }
  return held;
}

bool HleShell::BootableDisc() {
  if (!b_.system_->cdrom().disc_loaded() || !b_.OpenDisc())
    return false;
  Iso9660::File file;
  return b_.system_->iso().Find("SYSTEM.CNF", &file) || b_.system_->iso().Find("PSX.EXE", &file);
}

}  // namespace psx
}  // namespace emulation
