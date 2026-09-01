// make_test_disc - writes a bootable PlayStation disc image that draws.
//
//   make_test_disc <out.iso>
//
// The image carries a real ISO9660 filesystem, a SYSTEM.CNF naming its
// executable, and a PS-EXE that sets up the GPU and fills the screen with a
// solid colour, then loops forever.
//
// It exists because "my game does not boot" is untestable without a game. This
// gives the whole chain - filesystem, SYSTEM.CNF, executable load, CPU
// execution, GPU output - something to prove itself against, using nothing but
// a BIOS dump. If this disc shows a green screen and a real game does not, the
// fault is in something the real game uses and this does not.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---- MIPS assembly ---------------------------------------------------------

enum { zero = 0, t0 = 8, t1 = 9, t2 = 10 };

uint32_t LUI(int rt, uint32_t imm) {
  return (0x0Fu << 26) | (rt << 16) | (imm & 0xFFFF);
}
uint32_t ORI(int rt, int rs, uint32_t imm) {
  return (0x0Du << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF);
}
uint32_t SW(int rt, int off, int rs) {
  return (0x2Bu << 26) | (rs << 21) | (rt << 16) | (off & 0xFFFF);
}
uint32_t J(uint32_t target) {
  return (0x02u << 26) | ((target >> 2) & 0x03FFFFFF);
}
uint32_t NOP() { return 0; }

// Loads a 32-bit constant into a register.
void LoadImmediate(std::vector<uint32_t>* code, int reg, uint32_t value) {
  code->push_back(LUI(reg, value >> 16));
  if ((value & 0xFFFF) != 0)
    code->push_back(ORI(reg, reg, value & 0xFFFF));
}

// Writes a 32-bit constant to the address already in `port`.
void WritePort(std::vector<uint32_t>* code, int port, uint32_t value) {
  if (value == 0) {
    code->push_back(SW(zero, 0, port));
    return;
  }
  LoadImmediate(code, t2, value);
  code->push_back(SW(t2, 0, port));
}

// The program: bring the display up, fill the framebuffer, then spin.
std::vector<uint32_t> BuildProgram(uint32_t load_address, uint32_t colour) {
  std::vector<uint32_t> code;

  // t0 = GP0 (0x1F801810), t1 = GP1 (0x1F801814)
  code.push_back(LUI(t0, 0x1F80));
  code.push_back(ORI(t1, t0, 0x1814));
  code.push_back(ORI(t0, t0, 0x1810));

  WritePort(&code, t1, 0x00000000);   // GP1(00) reset
  WritePort(&code, t1, 0x03000000);   // GP1(03) display enabled
  WritePort(&code, t1, 0x05000000);   // GP1(05) display area at VRAM 0,0
  WritePort(&code, t1, 0x06C60260);   // GP1(06) horizontal range
  WritePort(&code, t1, 0x07040010);   // GP1(07) vertical range
  WritePort(&code, t1, 0x08000001);   // GP1(08) 320x240, NTSC, 15-bit

  WritePort(&code, t0, 0xE1000000);   // GP0(E1) draw mode
  WritePort(&code, t0, 0xE3000000);   // GP0(E3) draw area top-left 0,0
  WritePort(&code, t0, 0xE403BD3F);   // GP0(E4) draw area 319,239
  WritePort(&code, t0, 0xE5000000);   // GP0(E5) draw offset 0,0

  // GP0(02) fill rectangle: command plus colour, then position, then size.
  WritePort(&code, t0, 0x02000000 | (colour & 0x00FFFFFF));
  WritePort(&code, t0, 0x00000000);            // at 0,0
  WritePort(&code, t0, (240u << 16) | 320u);   // 320x240

  // Spin here forever. The address has to be absolute, so it depends on how
  // many instructions came before.
  const uint32_t here = load_address + static_cast<uint32_t>(code.size()) * 4;
  code.push_back(J(here));
  code.push_back(NOP());
  return code;
}

// ---- PS-EXE ----------------------------------------------------------------

void Put32(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

std::string MakePsExe(uint32_t load_address, const std::vector<uint32_t>& code) {
  // The text section is padded to a whole sector, as a real executable is.
  const uint32_t text_size =
      static_cast<uint32_t>((code.size() * 4 + 2047) / 2048 * 2048);
  std::string exe(0x800 + text_size, '\0');
  uint8_t* base = reinterpret_cast<uint8_t*>(&exe[0]);

  memcpy(base, "PS-X EXE", 8);
  Put32(base + 0x10, load_address);        // pc0
  Put32(base + 0x14, 0);                   // gp0
  Put32(base + 0x18, load_address);        // t_addr
  Put32(base + 0x1C, text_size);           // t_size
  Put32(base + 0x30, 0x801FFF00);          // s_addr, the initial stack
  Put32(base + 0x34, 0);                   // s_size

  for (size_t i = 0; i < code.size(); ++i)
    Put32(base + 0x800 + i * 4, code[i]);
  return exe;
}

// ---- ISO9660 ---------------------------------------------------------------

void PutU32Both(uint8_t* p, uint32_t value) {
  Put32(p, value);
  p[4] = static_cast<uint8_t>(value >> 24);
  p[5] = static_cast<uint8_t>(value >> 16);
  p[6] = static_cast<uint8_t>(value >> 8);
  p[7] = static_cast<uint8_t>(value);
}

uint32_t WriteDirectoryRecord(uint8_t* out, const char* name, uint32_t lba,
                              uint32_t size, bool directory) {
  const bool special = (name[0] == '\0' || name[0] == '\1');
  const uint32_t name_length =
      special ? 1 : static_cast<uint32_t>(strlen(name));
  uint32_t length = 33 + name_length;
  if (length & 1)
    ++length;

  memset(out, 0, length);
  out[0] = static_cast<uint8_t>(length);
  PutU32Both(out + 2, lba);
  PutU32Both(out + 10, size);
  out[25] = directory ? 0x02 : 0x00;
  out[32] = static_cast<uint8_t>(name_length);
  if (special)
    out[33] = static_cast<uint8_t>(name[0]);
  else
    memcpy(out + 33, name, name_length);
  return length;
}

struct Entry {
  const char* name;
  std::string contents;
};

// The BIOS reads sectors 4 to 11 before it will boot anything and checks them
// for this exact 64-byte string - it is the region lock. A disc without it is
// refused, and the refusal looks like "the logo screen appears and then
// nothing happens", with no error anywhere.
//
// The string here is the one in SCPH1001. Other BIOS revisions expect the
// region suffix to differ ("Amer  ica", "Euro pe"), so an image made for one
// region will not boot on another's BIOS.
const char kLicenceString[] =
    "          Licensed  by          Sony Computer Entertainment Inc.";

bool WriteIso(const std::string& path, const std::vector<Entry>& entries,
              const char* volume_id) {
  const uint32_t kSector = 2048;
  const uint32_t kDescriptor = 16;
  const uint32_t kRoot = 17;
  const uint32_t kFirstFile = 18;
  const uint32_t kLicenceFirst = 4;
  const uint32_t kLicenceLast = 11;

  std::vector<uint32_t> lba(entries.size());
  uint32_t next = kFirstFile;
  for (size_t i = 0; i < entries.size(); ++i) {
    lba[i] = next;
    uint32_t sectors =
        static_cast<uint32_t>((entries[i].contents.size() + kSector - 1) / kSector);
    if (sectors == 0)
      sectors = 1;
    next += sectors;
  }

  std::vector<uint8_t> image(static_cast<size_t>(next) * kSector, 0);

  for (uint32_t s = kLicenceFirst; s <= kLicenceLast; ++s) {
    memcpy(&image[s * kSector], kLicenceString, sizeof(kLicenceString) - 1);
  }

  uint8_t* pvd = &image[kDescriptor * kSector];
  pvd[0] = 0x01;
  memcpy(pvd + 1, "CD001", 5);
  pvd[6] = 0x01;
  memset(pvd + 40, ' ', 32);
  memcpy(pvd + 40, volume_id, strlen(volume_id));
  PutU32Both(pvd + 80, next);
  WriteDirectoryRecord(pvd + 156, "\0", kRoot, kSector, true);

  uint8_t* root = &image[kRoot * kSector];
  uint32_t offset = 0;
  offset += WriteDirectoryRecord(root + offset, "\0", kRoot, kSector, true);
  offset += WriteDirectoryRecord(root + offset, "\1", kRoot, kSector, true);
  for (size_t i = 0; i < entries.size(); ++i) {
    offset += WriteDirectoryRecord(
        root + offset, entries[i].name, lba[i],
        static_cast<uint32_t>(entries[i].contents.size()), false);
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    if (!entries[i].contents.empty()) {
      memcpy(&image[lba[i] * kSector], entries[i].contents.data(),
             entries[i].contents.size());
    }
  }

  FILE* fp = fopen(path.c_str(), "wb");
  if (fp == nullptr)
    return false;
  const size_t written = fwrite(&image[0], 1, image.size(), fp);
  fclose(fp);
  return written == image.size();
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "test_disc.iso";
  const uint32_t load_address = 0x80010000;
  const uint32_t colour = 0x0000FF00;      // bright green, as BBGGRR

  const std::vector<uint32_t> code = BuildProgram(load_address, colour);

  std::vector<Entry> entries;
  Entry cnf = { "SYSTEM.CNF;1",
                "BOOT = cdrom:\\PSX_TEST.EXE;1\r\n"
                "TCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n" };
  Entry exe = { "PSX_TEST.EXE;1", MakePsExe(load_address, code) };
  entries.push_back(cnf);
  entries.push_back(exe);

  if (!WriteIso(path, entries, "PSX TEST DISC")) {
    fprintf(stderr, "could not write %s\n", path);
    return 1;
  }

  printf("wrote %s\n", path);
  printf("  volume    PSX TEST DISC\n");
  printf("  boot      cdrom:\\PSX_TEST.EXE;1\n");
  printf("  loads at  %08X, %u instructions\n", load_address,
         static_cast<unsigned>(code.size()));
  printf("  draws     a 320x240 fill in colour %06X\n", colour);
  return 0;
}
