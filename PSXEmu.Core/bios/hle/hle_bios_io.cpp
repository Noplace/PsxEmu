// The HLE BIOS's devices: files on the CD-ROM ("cdrom:"), on memory cards ("bu00:", "bu10:") and
// the console ("tty:"), the kernel's CD-ROM functions, the memory card sector driver, and the
// pads read at every vblank.
//
// The kernel does all of these by driving the hardware, slowly; here the disc image, the card
// image and the pads are read directly, so a file loads in the time a trap takes. What software
// sees - the control blocks, the events, the error numbers, the order things are delivered in -
// follows psx-spx's "BIOS File Functions", "BIOS Memory Card Functions", "BIOS CDROM Functions"
// and "BIOS Joypad Functions". The one piece of timing kept is the memory card sector driver's:
// a sector read or written through B(4Eh)/B(4Fh) completes at a vblank, as on a console, because
// the libraries built on it (libcard) clear and wait for its events around the call.

#include "psx/psx.h"
#include "bios/hle/hle_bios.h"
#include "media/mc_directory.h"

namespace emulation {
namespace psx {
namespace hle {
namespace {

const uint32_t kIStat = 0x1F801070;
const uint32_t kIMask = 0x1F801074;
const uint32_t kEventClassCdrom = 0xF0000003;
const uint32_t kEventClassCard = 0xF0000011;
const uint32_t kEventClassCardHigh = 0xF4000001;

// File error numbers (B(54h) GetLastError).
const uint32_t kErrorNotFound = 0x02;
const uint32_t kErrorBadHandle = 0x09;
const uint32_t kErrorGeneral = 0x10;
const uint32_t kErrorExists = 0x11;
const uint32_t kErrorCrossDevice = 0x12;
const uint32_t kErrorUnknownDevice = 0x13;
const uint32_t kErrorAlignment = 0x16;
const uint32_t kErrorNoHandles = 0x18;
const uint32_t kErrorNoSpace = 0x1C;

const uint32_t kOpenCreate = 0x200;

// Device flags, DCB[04h]: CD-ROM and cards 14h, the dummy TTY 1.
const uint32_t kFlagsBlock = 0x14;
const uint32_t kFlagsTty = 0x01;

uint32_t FromBcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }

// A memory card filename against a pattern: "?" matches any one character, "*" the rest.
bool CardNameMatches(const std::string& name, const std::string& pattern) {
  size_t i = 0;
  for (; i < pattern.size(); ++i) {
    if (pattern[i] == '*')
      return true;
    if (i >= name.size())
      return false;
    if (pattern[i] != '?' && pattern[i] != name[i])
      return false;
  }
  return i == name.size();
}

// The same for a CD-ROM name, which is stored upper-case with a ";1" the pattern may leave off.
bool CdNameMatches(const std::string& stored, const std::string& pattern) {
  std::string name = stored;
  const size_t semicolon = name.find(';');
  std::string bare = semicolon == std::string::npos ? name : name.substr(0, semicolon);
  std::string want = pattern;
  for (char& c : want)
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  if (CardNameMatches(name, want))
    return true;
  const size_t want_semicolon = want.find(';');
  if (want_semicolon != std::string::npos)
    want = want.substr(0, want_semicolon);
  return CardNameMatches(bare, want);
}

}  // namespace

// ---- Paths and handles -------------------------------------------------------------------------

// "bu10:SAVE01" - the device's name, the port as the hex digits it ends with, and the name after
// the colon less any leading backslashes. psx-spx: the port is "a hexadecimal 32bit
// non-case-sensitive port number", which is why a device name may not end in a hex digit.
HleBios::Path HleBios::ParsePath(const std::string& text) {
  Path path;
  const size_t colon = text.find(':');
  if (colon == std::string::npos)
    return path;
  std::string device = text.substr(0, colon);
  size_t digits = device.size();
  while (digits > 0 && isxdigit(static_cast<unsigned char>(device[digits - 1])))
    --digits;
  path.port = digits < device.size() ? strtoul(device.substr(digits).c_str(), nullptr, 16) : 0;
  path.device = device.substr(0, digits);
  path.name = text.substr(colon + 1);
  while (!path.name.empty() && (path.name[0] == '\\' || path.name[0] == '/'))
    path.name.erase(0, 1);
  path.valid = path.device == "cdrom" || path.device == "bu" || path.device == "tty";
  return path;
}

HleBios::Fcb* HleBios::FileBlock(uint32_t fd) {
  if (fd >= static_cast<uint32_t>(kFcbCount))
    return nullptr;
  return At<Fcb>(kFcbs + fd * sizeof(Fcb));
}

int32_t HleBios::FreeFileHandle() {
  for (int fd = 0; fd < kFcbCount; ++fd)
    if (FileBlock(fd)->status == 0)
      return fd;
  return -1;
}

// C(12h) InstallDevices: the three devices' DCBs, and the two standard handles on the console.
void HleBios::InstallDevices() {
  struct Device {
    const char* name;
    const char* long_name;
    uint32_t flags;
    uint32_t sector;
  };
  static const Device kDevices[3] = {
    { "tty", "CONSOLE", kFlagsTty, 1 },
    { "cdrom", "CD-ROM", kFlagsBlock, 0x800 },
    { "bu", "MEMORY CARD", kFlagsBlock, 0x80 },
  };
  FillBlock(kDcbs, 0, kDcbCount * sizeof(Dcb));
  FillBlock(kFcbs, 0, kFcbCount * sizeof(Fcb));
  uint32_t strings = kStrings;
  for (int i = 0; i < 3; ++i) {
    Dcb* dcb = At<Dcb>(kDcbs + i * sizeof(Dcb));
    dcb->name = strings;
    WriteString(strings, kDevices[i].name);
    strings += 8;
    dcb->long_name = strings;
    WriteString(strings, kDevices[i].long_name);
    strings += 16;
    dcb->flags = kDevices[i].flags;
    dcb->sector_size = kDevices[i].sector;
    // The entry points are the A(5Bh-6Fh) functions that do the work on a console.
    const uint32_t base = i == 0 ? 0x5B : (i == 1 ? 0x5F : 0x65);
    dcb->init = i == 0 ? TrapAddress(kTableA + 0x5B) : 0;
    dcb->open = TrapAddress(kTableA + (i == 0 ? 0x5C : base));
    dcb->close = TrapAddress(kTableA + (i == 1 ? 0x61 : (i == 2 ? 0x68 : 0x5D)));
    dcb->read = TrapAddress(kTableA + (i == 1 ? 0x60 : (i == 2 ? 0x66 : 0x5D)));
  }
  Write32(kTableOfTables + 0x40, kFcbs);
  Write32(kTableOfTables + 0x44, kFcbCount * sizeof(Fcb));
  Write32(kTableOfTables + 0x50, kDcbs);
  Write32(kTableOfTables + 0x54, kDcbCount * sizeof(Dcb));

  for (uint32_t fd = 0; fd < 2; ++fd) {
    Fcb* f = FileBlock(fd);
    f->status = fd == 0 ? 1 : 2;
    f->dcb = kDcbs;
    f->device_flags = kFlagsTty;
    f->number = fd;
  }
}

// ---- The CD-ROM ----------------------------------------------------------------------------------

bool HleBios::OpenDisc() {
  Cdrom& cdrom = system_->cdrom();
  if (!cdrom.disc_loaded())
    return false;
  return system_->iso().Open(&cdrom.disc());
}

// A file by its path on the disc - relative to chdir's directory when it does not start at the
// root, as the kernel's paths are.
bool HleBios::CdFind(const std::string& path, uint32_t* lba, uint32_t* size) {
  if (!OpenDisc())
    return false;
  std::string full = path;
  if (!full.empty() && full[0] != '\\' && full[0] != '/') {
    std::string directory = vars().cd_directory;
    if (!directory.empty() && directory != "\\")
      full = directory + "\\" + full;
  }
  Iso9660::File file;
  if (!system_->iso().Find(full.c_str(), &file))
    return false;
  *lba = file.lba;
  *size = file.size;
  return true;
}

bool HleBios::CdRead(uint32_t lba, uint32_t offset, uint32_t dst, uint32_t bytes) {
  if (!OpenDisc())
    return false;
  uint8_t sector[Iso9660::kSectorSize];
  while (bytes > 0) {
    const uint32_t at = offset % Iso9660::kSectorSize;
    const uint32_t take = (Iso9660::kSectorSize - at) < bytes ? (Iso9660::kSectorSize - at) : bytes;
    if (!system_->iso().ReadLogicalSector(lba + offset / Iso9660::kSectorSize, sector))
      return false;
    WriteBlock(dst, sector + at, take);
    dst += take;
    offset += take;
    bytes -= take;
  }
  return true;
}

bool HleBios::LoadFileImage(const std::string& text, std::vector<uint8_t>* image) {
  const Path path = ParsePath(text.find(':') == std::string::npos ? "cdrom:" + text : text);
  if (!path.valid || path.device != "cdrom" || !OpenDisc())
    return false;
  std::string name = path.name;
  if (name.find(';') == std::string::npos)
    name += ";1";
  uint32_t lba = 0, size = 0;
  if (!CdFind(name, &lba, &size) && !CdFind(path.name, &lba, &size))
    return false;
  Iso9660::File file;
  file.lba = lba;
  file.size = size;
  file.directory = false;
  AddCost(kCdSeekCycles + (size + Iso9660::kSectorSize - 1) / Iso9660::kSectorSize * kCdSectorCycles);
  return system_->iso().Read(file, image);
}

// A(54h) CdInit: the kernel's five CD-ROM events and its interrupt handler, in priority 0.
void HleBios::CdInit() {
  KernelVars& v = vars();
  if (v.cd_events_open == 0) {
    static const uint32_t kSpecs[5] = { 0x10, 0x20, 0x40, 0x80, 0x8000 };
    for (int i = 0; i < 5; ++i) {
      v.cd_event[i] = OpenEvent(kEventClassCdrom, kSpecs[i], 0x2000, 0);
      if (EvCb* e = Event(v.cd_event[i]))
        if (v.cd_event[i] != 0xFFFFFFFF)
          e->status = 0x2000;
    }
    v.cd_events_open = 1;
  }
  EnqueueHandler(0, ChainElementFor(TrapAddress(kCdIoVerifier), 0));
  WriteIo32(kIMask, ReadIo32(kIMask) | 0x0C);

  // The drive's own interrupts on, and any it has pending acknowledged - libcd counts on the
  // kernel having done this, and never enables them itself (psx-spx "BIOS Boot State": the
  // interrupt enable register reads 1Fh, FFh with the unused bits, once a game is running).
  WriteIo8(0x1F801800, 1);
  WriteIo8(0x1F801802, 0x1F);
  WriteIo8(0x1F801803, 0x1F);
  WriteIo8(0x1F801800, 0);
}

// A(56h) CdRemove - which nearly every game calls first thing (libcd's _96_remove), to take the
// kernel's CD-ROM handling out of its way.
void HleBios::CdRemove() {
  KernelVars& v = vars();
  DequeueHandler(0, ChainElementFor(TrapAddress(kCdIoVerifier), 0));
  if (v.cd_events_open != 0) {
    for (uint32_t handle : v.cd_event)
      if (handle != 0xFFFFFFFF)
        if (EvCb* e = Event(handle))
          e->status = 0;
    v.cd_events_open = 0;
  }
}

// The kernel's CD-ROM interrupt handler, for as long as nothing has removed it. Nothing of the
// kernel's own waits on the drive here - its file functions read the image directly - so all it
// has to do is take the interrupt off the drive and the controller, and remember an error.
void HleBios::CdIoVerifier() {
  if ((ReadIo32(kIMask) & ReadIo32(kIStat) & 0x04) == 0) {
    Return(0);
    return;
  }
  const uint8_t index = system_->io().Read08(0x1F801800) & 3;
  WriteIo8(0x1F801800, 1);
  const uint8_t flags = system_->io().Read08(0x1F801803) & 7;
  if (flags == 5) {
    vars().cd_int5[0] = system_->io().Read08(0x1F801801);
    vars().cd_int5[1] = system_->io().Read08(0x1F801801);
  }
  WriteIo8(0x1F801803, 0x1F);
  WriteIo8(0x1F801800, index);
  WriteIo32(kIStat, ~0x04u);
  Return(0);
}

// ---- Memory cards --------------------------------------------------------------------------------

MC* HleBios::Card(uint32_t port) {
  const int slot = (port >> 4) & 1;
  const int index = port & 0x3;
  MC& card = index == 0 ? system_->mc(slot) : system_->mc(slot, index);
  return card.connected() ? &card : nullptr;
}

uint8_t* HleBios::CardImage(uint32_t port) {
  MC* card = Card(port);
  return card != nullptr ? card->data() : nullptr;
}

// The directory frame of the first block of the save called `name` - or of a deleted one.
int HleBios::CardFindFile(uint32_t port, const std::string& name, bool deleted) {
  const uint8_t* image = CardImage(port);
  if (image == nullptr)
    return -1;
  for (int block = 1; block < mcdir::kBlocks; ++block) {
    const uint8_t* frame = image + block * mcdir::kFrameSize;
    if (frame[0] != (deleted ? 0xA1 : 0x51))
      continue;
    const std::string stored(reinterpret_cast<const char*>(frame + 0x0A),
                             strnlen(reinterpret_cast<const char*>(frame + 0x0A), 20));
    if (CardNameMatches(stored, name))
      return block;
  }
  return -1;
}

// A directory frame rewritten through the card itself, checksum and all, so the card marks its
// file as changed and clears its new-card flag the way a write over the bus does.
void HleBios::CardWriteFrame(uint32_t port, int frame) {
  MC* card = Card(port);
  if (card == nullptr)
    return;
  uint8_t data[mcdir::kFrameSize];
  memcpy(data, card->data() + frame * mcdir::kFrameSize, sizeof(data));
  data[127] = mcdir::FrameChecksum(data);
  card->WriteSector(static_cast<uint16_t>(frame), data);
}

bool HleBios::CardCreate(uint32_t port, const std::string& name, uint32_t blocks, int* first) {
  MC* card = Card(port);
  if (card == nullptr)
    return false;
  if (blocks == 0)
    blocks = 1;
  uint8_t* image = card->data();
  std::vector<int> chosen;
  for (int block = 1; block < mcdir::kBlocks && chosen.size() < blocks; ++block) {
    const uint8_t state = image[block * mcdir::kFrameSize];
    if (state == 0xA0 || state == 0xA1 || state == 0xA2 || state == 0xA3)
      chosen.push_back(block);
  }
  if (chosen.size() < blocks)
    return false;
  for (size_t i = 0; i < chosen.size(); ++i) {
    uint8_t* frame = image + chosen[i] * mcdir::kFrameSize;
    memset(frame, 0, mcdir::kFrameSize);
    frame[0] = chosen.size() == 1 ? 0x51 : (i == 0 ? 0x51 : (i + 1 == chosen.size() ? 0x53 : 0x52));
    if (i == 0) {
      const uint32_t size = blocks * static_cast<uint32_t>(mcdir::kBlockSize);
      memcpy(frame + 4, &size, 4);
      strncpy(reinterpret_cast<char*>(frame + 0x0A), name.c_str(), 20);
    }
    const uint16_t next = i + 1 < chosen.size() ? static_cast<uint16_t>(chosen[i + 1] - 1) : 0xFFFF;
    memcpy(frame + 8, &next, 2);
    CardWriteFrame(port, chosen[i]);
  }
  *first = chosen[0];
  return true;
}

// Where byte `position` of the save starting at `first` is on the card.
bool HleBios::CardFileOffset(uint32_t port, int first, uint32_t position, uint32_t* offset) {
  const uint8_t* image = CardImage(port);
  if (image == nullptr)
    return false;
  int block = first;
  for (uint32_t skip = position / mcdir::kBlockSize; skip > 0; --skip) {
    uint16_t next;
    memcpy(&next, image + block * mcdir::kFrameSize + 8, 2);
    if (next == 0xFFFF || next + 1 >= mcdir::kBlocks)
      return false;
    block = next + 1;
  }
  *offset = block * static_cast<uint32_t>(mcdir::kBlockSize) + position % mcdir::kBlockSize;
  return true;
}

// B(4Eh)/B(4Fh)/A(ABh): a sector to write, read or ask about, left for the next vblank.
void HleBios::CardSectorOperation(uint32_t function, uint32_t port, uint32_t sector,
                                  uint32_t buffer) {
  KernelVars& v = vars();
  const uint32_t slot = (port >> 4) & 1;
  if (function != 0xAB && sector > 0x400) {   // 400h itself gets through, as on a console
    Return(0);
    return;
  }
  if ((v.card_status[slot] & 1) == 0) {
    Return(0);
    return;
  }
  v.card_op[slot] = function == 0x4F ? 1 : (function == 0x4E ? 2 : 3);
  v.card_sector[slot] = sector;
  v.card_buffer[slot] = buffer;
  v.card_status[slot] = function == 0x4F ? 2 : (function == 0x4E ? 4 : 8);
  Return(1);
}

// The pad and card handler's card half, at each vblank: the last operation's events taken back,
// then one slot's waiting sector done - slot 1 and slot 2 in turn, as the kernel alternates.
void HleBios::CardVblank(uint32_t return_to) {
  KernelVars& v = vars();
  static const uint32_t kSpecs[5] = { 0x0004, 0x8000, 0x0100, 0x0200, 0x2000 };
  for (uint32_t spec : kSpecs)
    UndeliverEvent(kEventClassCard, spec);

  // An operation takes two vblanks: one to start the transfer on the slot whose turn it is, and
  // the next to finish it - which is how long the kernel's driver takes to move a sector, a byte
  // per controller interrupt (_bu_init's two reads: 2.4-2.8 million cycles measured).
  const uint32_t kBegun = 0x100;
  uint32_t slot = 2;
  for (uint32_t s : { v.card_flip, v.card_flip ^ 1 })
    if (slot == 2 && (v.card_op[s] & kBegun) != 0)
      slot = s;
  if (slot == 2) {
    v.card_flip ^= 1;
    if (v.card_op[v.card_flip] != 0)
      v.card_op[v.card_flip] |= kBegun;
    cpu().pc = return_to;
    return;
  }

  const uint32_t op = v.card_op[slot] & ~kBegun;
  v.card_op[slot] = 0;
  v.card_last_port = slot << 4;
  MC* card = Card(slot << 4);
  uint32_t spec = 0x0004;
  if (card == nullptr) {
    v.card_status[slot] = 0x11;   // no answer
    spec = 0x0100;
  } else if ((card->flag() & 0x08) != 0 && v.card_allow_new == 0 && op != 2) {
    // A card that has been changed: nothing is read from it until software has acknowledged
    // that (allow_new_card, and a write).
    v.card_status[slot] = 1;
    spec = 0x2000;
  } else {
    v.card_allow_new = 0;
    uint8_t data[mcdir::kFrameSize];
    if (op == 1) {
      if (card->ReadSector(static_cast<uint16_t>(v.card_sector[slot]), data))
        WriteBlock(v.card_buffer[slot], data, sizeof(data));
      else
        spec = 0x8000;
    } else if (op == 2) {
      ReadBlock(v.card_buffer[slot], data, sizeof(data));
      if (!card->WriteSector(static_cast<uint16_t>(v.card_sector[slot]), data))
        spec = 0x8000;
    }
    v.card_status[slot] = spec == 0x0004 ? 1 : 0x21;
  }

  // The kernel's own callback first - bu_callback_okay, _err_busy, _err_eject or _err_write,
  // A(A7h)-A(AAh), each delivering its F4000001h event, which is how libcard learns what became of
  // a sector - and then the driver's event, F0000011h.
  Then then;
  then.kind = kThenCardEvents;
  then.a = return_to;
  then.b = spec;
  DeliverEvent(kEventClassCardHigh, spec, then);
}

// ---- Files ---------------------------------------------------------------------------------------

uint32_t HleBios::FileOpen(uint32_t name_address, uint32_t mode) {
  KernelVars& v = vars();
  const Path path = ParsePath(ReadString(name_address));
  if (!path.valid) {
    v.last_error = kErrorUnknownDevice;
    return 0xFFFFFFFF;
  }
  const int32_t fd = FreeFileHandle();
  if (fd < 0) {
    v.last_error = kErrorNoHandles;
    return 0xFFFFFFFF;
  }
  Fcb* f = FileBlock(fd);
  Fcb opened = {};
  opened.status = mode != 0 ? mode : 1;
  opened.number = static_cast<uint32_t>(fd);

  if (path.device == "cdrom") {
    uint32_t lba = 0, size = 0;
    if (!CdFind(path.name, &lba, &size)) {
      v.last_error = kErrorNotFound;
      return 0xFFFFFFFF;
    }
    opened.dcb = kDcbs + 1 * sizeof(Dcb);
    opened.device_flags = kFlagsBlock;
    opened.lba = lba;
    opened.size = size;
  } else if (path.device == "bu") {
    if (Card(path.port) == nullptr) {
      v.last_error = kErrorGeneral;
      return 0xFFFFFFFF;
    }
    int first = CardFindFile(path.port, path.name, false);
    if (mode & kOpenCreate) {
      if (first >= 0) {
        v.last_error = kErrorExists;
        return 0xFFFFFFFF;
      }
      if (!CardCreate(path.port, path.name, mode >> 16, &first)) {
        v.last_error = kErrorNoSpace;
        return 0xFFFFFFFF;
      }
    } else if (first < 0) {
      v.last_error = kErrorNotFound;
      return 0xFFFFFFFF;
    }
    uint32_t size;
    memcpy(&size, CardImage(path.port) + first * mcdir::kFrameSize + 4, 4);
    opened.dcb = kDcbs + 2 * sizeof(Dcb);
    opened.device_flags = kFlagsBlock;
    opened.port = path.port;
    opened.lba = static_cast<uint32_t>(first);
    opened.size = size;
    AddCost(kCardSectorCycles);   // the kernel reads sector 0 on every open (psx-spx)
  } else {
    opened.dcb = kDcbs;
    opened.device_flags = kFlagsTty;
  }
  *f = opened;
  return static_cast<uint32_t>(fd);
}

uint32_t HleBios::FileRead(uint32_t fd, uint32_t dst, uint32_t length) {
  KernelVars& v = vars();
  Fcb* f = FileBlock(fd);
  if (f == nullptr || f->status == 0) {
    v.last_error = kErrorBadHandle;
    return 0xFFFFFFFF;
  }
  if (f->dcb == kDcbs)
    return 0;   // the console: nothing is typed
  if (length == 0 || f->position >= f->size) {
    f->error = v.last_error = kErrorAlignment;
    return 0xFFFFFFFF;
  }
  if (f->dcb == kDcbs + sizeof(Dcb)) {
    // The CD-ROM reads in whole sectors, so what it hands back is `length` from the disc
    // whether or not the file ends sooner.
    if (!CdRead(f->lba, f->position, dst, length)) {
      f->error = v.last_error = kErrorGeneral;
      return 0xFFFFFFFF;
    }
    AddCost(kCdSeekCycles +
            (length + Iso9660::kSectorSize - 1) / Iso9660::kSectorSize * kCdSectorCycles);
    f->position += length;
    return length;
  }
  // A memory card: whole 80h-byte sectors, at a sector boundary.
  if ((length & 0x7F) != 0 || (f->position & 0x7F) != 0) {
    f->error = v.last_error = kErrorAlignment;
    return 0xFFFFFFFF;
  }
  const uint8_t* image = CardImage(f->port);
  if (image == nullptr) {
    f->error = v.last_error = kErrorGeneral;
    file_event_fd_ = fd;
    file_event_spec_ = 0x8000;
    return 0xFFFFFFFF;
  }
  if (f->position + length > f->size)
    length = f->size - f->position;
  AddCost(length / 0x80 * kCardSectorCycles);
  for (uint32_t done = 0; done < length; done += 0x80) {
    uint32_t offset;
    if (!CardFileOffset(f->port, static_cast<int>(f->lba), f->position + done, &offset))
      break;
    WriteBlock(dst + done, image + offset, 0x80);
  }
  f->position += length;
  file_event_fd_ = fd;
  file_event_spec_ = 0x0004;
  return length;
}

uint32_t HleBios::FileWrite(uint32_t fd, uint32_t src, uint32_t length) {
  KernelVars& v = vars();
  Fcb* f = FileBlock(fd);
  if (f == nullptr || f->status == 0) {
    v.last_error = kErrorBadHandle;
    return 0xFFFFFFFF;
  }
  if (f->dcb == kDcbs) {
    // The console: onto the BIOS console, as the TTY driver prints it.
    std::string text(length, '\0');
    if (length != 0)
      ReadBlock(src, &text[0], length);
    Console(text);
    return length;
  }
  if (f->dcb == kDcbs + sizeof(Dcb))
    return 0;   // the CD-ROM
  if ((length & 0x7F) != 0 || (f->position & 0x7F) != 0 || f->position >= f->size) {
    f->error = v.last_error = kErrorAlignment;
    return 0xFFFFFFFF;
  }
  MC* card = Card(f->port);
  if (card == nullptr) {
    f->error = v.last_error = kErrorGeneral;
    file_event_fd_ = fd;
    file_event_spec_ = 0x8000;
    return 0xFFFFFFFF;
  }
  if (f->position + length > f->size)
    length = f->size - f->position;
  AddCost(length / 0x80 * kCardSectorCycles);
  uint8_t data[0x80];
  for (uint32_t done = 0; done < length; done += 0x80) {
    uint32_t offset;
    if (!CardFileOffset(f->port, static_cast<int>(f->lba), f->position + done, &offset))
      break;
    ReadBlock(src + done, data, sizeof(data));
    card->WriteSector(static_cast<uint16_t>(offset / 0x80), data);
  }
  f->position += length;
  file_event_fd_ = fd;
  file_event_spec_ = 0x0004;
  return length;
}

uint32_t HleBios::FileClose(uint32_t fd) {
  Fcb* f = FileBlock(fd);
  if (f == nullptr || f->status == 0) {
    vars().last_error = kErrorBadHandle;
    return 0xFFFFFFFF;
  }
  f->status = 0;
  return fd;
}

uint32_t HleBios::FileSeek(uint32_t fd, int32_t offset, uint32_t whence) {
  Fcb* f = FileBlock(fd);
  if (f == nullptr || f->status == 0) {
    vars().last_error = kErrorBadHandle;
    return 0xFFFFFFFF;
  }
  switch (whence) {
    case 0: f->position = static_cast<uint32_t>(offset); break;
    case 1: f->position += static_cast<uint32_t>(offset); break;
    default:
      f->error = vars().last_error = kErrorAlignment;
      return 0xFFFFFFFF;
  }
  return f->position;
}

// A memory card file operation's events, after the call: the file's own class, then the
// device's, then back to the caller with the result.
void HleBios::FinishFile(uint32_t result) {
  if (file_event_fd_ == 0xFFFFFFFF) {
    Return(result);
    return;
  }
  const uint32_t fd = file_event_fd_;
  const uint32_t spec = file_event_spec_;
  file_event_fd_ = 0xFFFFFFFF;
  Then then;
  then.kind = kThenFileEvents;
  then.a = cpu().gp.ra;
  then.b = result;
  then.c = spec;
  DeliverEvent(fd, spec, then);
}

namespace {

// A direntry (psx-spx firstfile): name, attribute, size, the unused next pointer, first sector.
struct DirEntry {
  char name[20];
  uint32_t attribute;
  uint32_t size;
  uint32_t next;
  uint32_t sector;
  uint32_t reserved;
};
static_assert(sizeof(DirEntry) == 0x28, "direntry is 28h bytes");

}  // namespace

uint32_t HleBios::FirstFile(uint32_t name_address, uint32_t direntry) {
  KernelVars& v = vars();
  const std::string text = ReadString(name_address);
  const Path path = ParsePath(text);
  if (!path.valid || path.device == "tty") {
    v.search_device = 0;
    return 0;
  }
  v.search_device = path.device == "cdrom" ? 1 : 2;
  v.search_port = path.port;
  v.search_index = 0;
  memset(v.search_pattern, 0, sizeof(v.search_pattern));
  std::string pattern = path.name;
  if (v.search_device == 1) {
    // The directory part is where to look; the last part is the pattern.
    const size_t slash = pattern.find_last_of("\\/");
    std::string directory = slash == std::string::npos ? std::string(v.cd_directory)
                                                       : "\\" + pattern.substr(0, slash);
    if (slash != std::string::npos)
      pattern = pattern.substr(slash + 1);
    // The search directory rides in the pattern buffer's tail: "pattern\0directory".
    std::string packed = pattern + '\0' + directory;
    if (packed.size() >= sizeof(v.search_pattern))
      packed.resize(sizeof(v.search_pattern) - 1);
    memcpy(v.search_pattern, packed.data(), packed.size());
  } else {
    strncpy(v.search_pattern, pattern.c_str(), sizeof(v.search_pattern) - 1);
  }
  return NextFile(direntry);
}

uint32_t HleBios::NextFile(uint32_t direntry) {
  KernelVars& v = vars();
  DirEntry entry = {};
  if (v.search_device == 2) {
    const uint8_t* image = CardImage(v.search_port);
    if (image == nullptr)
      return 0;
    const bool deleted = v.card_find_mode != 0;
    while (v.search_index < static_cast<uint32_t>(mcdir::kBlocks) - 1) {
      const int block = static_cast<int>(++v.search_index);
      const uint8_t* frame = image + block * mcdir::kFrameSize;
      if (frame[0] != (deleted ? 0xA1 : 0x51))
        continue;
      const std::string stored(reinterpret_cast<const char*>(frame + 0x0A),
                               strnlen(reinterpret_cast<const char*>(frame + 0x0A), 20));
      if (!CardNameMatches(stored, v.search_pattern))
        continue;
      strncpy(entry.name, stored.c_str(), sizeof(entry.name) - 1);
      entry.attribute = frame[0] == 0x51 ? 0x50 : 0xA0;
      memcpy(&entry.size, frame + 4, 4);
      entry.sector = static_cast<uint32_t>(block) * 0x40;
      WriteBlock(direntry, &entry, sizeof(entry));
      return direntry;
    }
    return 0;
  }
  if (v.search_device == 1) {
    const std::string pattern(v.search_pattern);
    const std::string directory(v.search_pattern + pattern.size() + 1);
    std::vector<Iso9660::File> files;
    if (!OpenDisc() || !system_->iso().List(directory.c_str(), &files))
      return 0;
    while (v.search_index < files.size()) {
      const Iso9660::File& file = files[v.search_index++];
      if (file.name == "." || file.name == "..")
        continue;
      if (!CdNameMatches(file.name, pattern))
        continue;
      strncpy(entry.name, file.name.c_str(), sizeof(entry.name) - 1);
      entry.size = file.size;
      entry.sector = file.lba;
      WriteBlock(direntry, &entry, sizeof(entry));
      return direntry;
    }
    return 0;
  }
  return 0;
}

uint32_t HleBios::FileDelete(uint32_t name_address, bool undelete) {
  KernelVars& v = vars();
  const Path path = ParsePath(ReadString(name_address));
  if (!path.valid || path.device != "bu") {
    v.last_error = path.valid ? kErrorGeneral : kErrorUnknownDevice;
    return 0;
  }
  MC* card = Card(path.port);
  const int first = card != nullptr ? CardFindFile(path.port, path.name, undelete) : -1;
  if (first < 0) {
    v.last_error = kErrorNotFound;
    return 0;
  }
  std::vector<uint8_t> before(card->data(), card->data() + mcdir::kBlockSize);
  std::vector<uint8_t> image(card->data(), card->data() + mcdir::kCardSize);
  std::string error;
  const bool ok = undelete ? mcdir::Undelete(&image[0], first, &error)
                           : mcdir::Delete(&image[0], first, &error);
  if (!ok) {
    v.last_error = undelete ? kErrorExists : kErrorGeneral;
    return 0;
  }
  for (int frame = 1; frame < mcdir::kBlocks; ++frame) {
    const size_t at = frame * mcdir::kFrameSize;
    if (memcmp(&before[at], &image[at], mcdir::kFrameSize) != 0)
      card->WriteSector(static_cast<uint16_t>(frame), &image[at]);
  }
  return 1;
}

uint32_t HleBios::FileRename(uint32_t from_address, uint32_t to_address) {
  KernelVars& v = vars();
  const Path from = ParsePath(ReadString(from_address));
  const Path to = ParsePath(ReadString(to_address));
  if (!from.valid || !to.valid) {
    v.last_error = kErrorUnknownDevice;
    return 0;
  }
  if (from.device != to.device || from.port != to.port || from.device != "bu") {
    v.last_error = kErrorCrossDevice;
    return 0;
  }
  const int first = CardFindFile(from.port, from.name, false);
  if (first < 0) {
    v.last_error = kErrorNotFound;
    return 0;
  }
  if (CardFindFile(to.port, to.name, false) >= 0) {
    v.last_error = kErrorExists;
    return 0;
  }
  uint8_t* frame = CardImage(from.port) + first * mcdir::kFrameSize;
  memset(frame + 0x0A, 0, 21);
  strncpy(reinterpret_cast<char*>(frame + 0x0A), to.name.c_str(), 20);
  CardWriteFrame(from.port, first);
  return 1;
}

uint32_t HleBios::FormatDevice(uint32_t name_address) {
  KernelVars& v = vars();
  const Path path = ParsePath(ReadString(name_address));
  MC* card = path.valid && path.device == "bu" ? Card(path.port) : nullptr;
  if (card == nullptr) {
    v.last_error = path.valid ? kErrorGeneral : kErrorUnknownDevice;
    return 0;
  }
  std::vector<uint8_t> image(mcdir::kCardSize, 0);
  mcdir::Format(&image[0]);
  for (int frame = 0; frame < 64; ++frame)
    card->WriteSector(static_cast<uint16_t>(frame), &image[frame * mcdir::kFrameSize]);
  return 1;
}

uint32_t HleBios::ChangeDirectory(uint32_t name_address) {
  const Path path = ParsePath(ReadString(name_address));
  if (!path.valid || path.device != "cdrom") {
    vars().last_error = kErrorUnknownDevice;
    return 0;
  }
  std::string directory = "\\" + path.name;
  while (directory.size() > 1 && (directory.back() == '\\' || directory.back() == '/'))
    directory.pop_back();
  strncpy(vars().cd_directory, directory.c_str(), sizeof(vars().cd_directory) - 1);
  return 1;
}

// ---- Pads ----------------------------------------------------------------------------------------

// One pad into its InitPad buffer: FFh until it answers, then 00h, the ID, and as many halfwords
// as the ID's low nibble says (sixteen for a zero - a multitap's 22h bytes).
void HleBios::ReadPad(int port) {
  KernelVars& v = vars();
  const uint32_t buffer = v.pad_buf[port];
  if (buffer == 0)
    return;
  Write8(buffer, 0xFF);
  uint8_t out[35] = { 0x01, 0x42, 0x00 };
  if (v.pad_output[port] != 0)
    for (int i = 0; i < 32; ++i)
      out[3 + i] = Read8(v.pad_output[port] + i);
  uint8_t in[35] = {};
  const int got = system_->sio().ExchangeDirect(port, out, in, sizeof(out));
  if (trace_ != nullptr)
    fprintf(trace_, "            pad %d at %08X: %d bytes, %02X %02X %02X %02X %02X\n", port, buffer,
            got, in[0], in[1], in[2], in[3], in[4]);
  if (got < 3 || in[2] != 0x5A)
    return;
  const uint8_t id = in[1];
  const int halfwords = (id & 0x0F) != 0 ? (id & 0x0F) : 16;
  uint8_t reply[0x22] = {};
  reply[1] = id;
  const int bytes = halfwords * 2 < got - 3 ? halfwords * 2 : got - 3;
  memcpy(reply + 2, in + 3, bytes > 0 ? bytes : 0);
  WriteBlock(buffer + 1, reply + 1, static_cast<uint32_t>(1 + halfwords * 2));
  Write8(buffer, 0x00);
}

void HleBios::PadVblank() {
  KernelVars& v = vars();
  if (v.pad_started == 0 || v.pad_enable == 0)
    return;
  ReadPad(0);
  ReadPad(1);
  if (v.outdated_pad_dest != 0)
    Write32(v.outdated_pad_dest, OutdatedPadButtons());
}

// B(16h): both pads' buttons in one word, high byte first, pad 1 low - only for a digital pad
// (41h) or the "flying V" (23h); FFFFh for anything else.
uint32_t HleBios::OutdatedPadButtons() {
  KernelVars& v = vars();
  uint32_t result = 0xFFFFFFFF;
  for (int i = 0; i < 2; ++i) {
    const uint32_t buffer = v.pad_buf[i];
    if (buffer == 0 || Read8(buffer) != 0)
      continue;
    const uint8_t id = Read8(buffer + 1);
    if (id != 0x41 && id != 0x23)
      continue;
    uint32_t half = (static_cast<uint32_t>(Read8(buffer + 2)) << 8) | Read8(buffer + 3);
    if (id == 0x23) {
      half |= 0x7C7;
      if (Read8(buffer + 5) > 0x10) half &= ~0x40u;
      if (Read8(buffer + 6) > 0x10) half &= ~0x80u;
    }
    result = (result & ~(0xFFFFu << (16 * i))) | (half << (16 * i));
  }
  Write32(v.outdated_pad_dest, result);
  return result;
}

// ---- The functions -------------------------------------------------------------------------------

// B(n), or A(n) given as 100h + n.
bool HleBios::IoFunction(uint32_t n) {
  KernelVars& v = vars();
  if (n < 0x100) {
    switch (n) {
      case 0x4D:   // _card_info_subfunc(port)
        CardSectorOperation(0xAB, Arg(0), 0, 0);
        return true;
      case 0x4E:   // write_card_sector(port, sector, src)
      case 0x4F:   // read_card_sector(port, sector, dst)
        CardSectorOperation(n, Arg(0), Arg(1), Arg(2));
        return true;
      default:
        return false;
    }
  }

  const uint32_t a = n - 0x100;
  switch (a) {
    case 0x5B: case 0x5C: case 0x5D: case 0x5E:   // the TTY device's own entry points
    case 0x57: case 0x58: case 0x59: case 0x5A:
    case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x79: case 0x7A: case 0x7B: case 0x7D: case 0x7F: case 0x80:
    case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89:
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x9E: case 0xB0: case 0xB1: case 0xB3:
      Return(0);
      return true;
    case 0x95:     // CdInitSubFunc
      CdInit();
      Return(1);
      return true;
    case 0x78: {   // CdAsyncSeekL(src): MM SS FF in BCD
      const uint32_t src = Arg(0);
      const uint32_t m = FromBcd(Read8(src)), s = FromBcd(Read8(src + 1)), f = FromBcd(Read8(src + 2));
      const uint32_t sector = (m * 60 + s) * 75 + f;
      v.cd_seek_lba = sector >= 150 ? sector - 150 : 0;
      Then then;
      then.a = cpu().gp.ra;
      then.b = 1;
      DeliverEvent(kEventClassCdrom, 0x20, then);
      return true;
    }
    case 0x7C: {   // CdAsyncGetStatus(dst)
      Write8(Arg(0), 0x02);
      Then then;
      then.a = cpu().gp.ra;
      then.b = 1;
      DeliverEvent(kEventClassCdrom, 0x20, then);
      return true;
    }
    case 0x7E: {   // CdAsyncReadSector(count, dst, mode)
      const uint32_t count = Arg(0), dst = Arg(1), mode = Arg(2);
      if ((mode & 0x30) != 0)
        Message("CdAsyncReadSector of whole sectors reads their data only");
      const bool ok = CdRead(v.cd_seek_lba, 0, dst, count * Iso9660::kSectorSize);
      v.cd_seek_lba += count;
      Then then;
      then.a = cpu().gp.ra;
      then.b = ok ? 1 : 0;
      DeliverEvent(kEventClassCdrom, ok ? 0x20 : 0x8000, then);
      return true;
    }
    case 0x81: {   // CdAsyncSetMode(mode)
      Then then;
      then.a = cpu().gp.ra;
      then.b = 1;
      DeliverEvent(kEventClassCdrom, 0x20, then);
      return true;
    }
    case 0x94:     // CdromGetInt5errCode(dst1, dst2)
      Write8(Arg(0), static_cast<uint8_t>(v.cd_int5[0]));
      Write8(Arg(1), static_cast<uint8_t>(v.cd_int5[1]));
      Return(0);
      return true;
    case 0xA2:     // EnqueueCdIntr
      EnqueueHandler(0, ChainElementFor(TrapAddress(kCdIoVerifier), 0));
      Return(0);
      return true;
    case 0xA3:     // DequeueCdIntr
      DequeueHandler(0, ChainElementFor(TrapAddress(kCdIoVerifier), 0));
      Return(0);
      return true;
    case 0xA4: {   // CdGetLbn(filename)
      const Path path = ParsePath(ReadString(Arg(0)));
      uint32_t lba = 0, size = 0;
      Return(path.valid && CdFind(path.name, &lba, &size) ? lba : 0xFFFFFFFF);
      return true;
    }
    case 0xA5: {   // CdReadSector(count, sector, buffer)
      const uint32_t count = Arg(0), sector = Arg(1), buffer = Arg(2);
      AddCost(kCdSeekCycles + count * kCdSectorCycles);
      Return(CdRead(sector, 0, buffer, count * Iso9660::kSectorSize) ? count : 0xFFFFFFFF);
      return true;
    }
    case 0xA6:     // CdGetStatus
      Return(system_->cdrom().disc_loaded() ? 0x02 : 0x10);
      return true;
    case 0xA7:     // bu_callback_okay
    case 0xA8:     // bu_callback_err_write
    case 0xA9:     // bu_callback_err_busy
    case 0xAA:     // bu_callback_err_eject
    case 0xAE: {   // bu_callback_err_prev_write
      const uint32_t spec = a == 0xA7 ? 0x0004 : a == 0xA9 ? 0x0100 : a == 0xAA ? 0x2000 : 0x8000;
      Then then;
      then.a = cpu().gp.ra;
      DeliverEvent(kEventClassCardHigh, spec, then);
      return true;
    }
    case 0xAB:     // _card_info(port)
      CardSectorOperation(0xAB, Arg(0), 0, 0);
      return true;
    case 0xAC: {   // _card_async_load_directory(port): there is nothing to load
      Then then;
      then.a = cpu().gp.ra;
      then.b = 1;
      DeliverEvent(kEventClassCardHigh, 0x0004, then);
      return true;
    }
    case 0xAD:     // set_card_auto_format(flag)
      v.card_auto_format = Arg(0);
      Return(0);
      return true;
    case 0xAF: {   // card_write_test(port): acknowledge a new card with a write to sector 3Fh
      v.card_allow_new = 1;
      const uint32_t scratch = kCardDirectoryBuffer;
      CardSectorOperation(0x4E, Arg(0), 0x3F, scratch);
      return true;
    }
    default: {
      if (a >= 0x5F && a <= 0x6F) {
        char text[64];
        snprintf(text, sizeof(text), "A(%02Xh), a device's own entry point, is not implemented", a);
        Message(text);
        Return(0);
        return true;
      }
      char text[48];
      snprintf(text, sizeof(text), "A(%02Xh) is not implemented", a);
      Message(text);
      Return(0);
      return true;
    }
  }
}

}  // namespace hle
}  // namespace psx
}  // namespace emulation
