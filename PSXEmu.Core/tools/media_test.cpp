// media_test - protocol-level tests for the disc layer and the CD-ROM
// controller. Takes no BIOS, no window and no disc of its own: it builds the
// images it needs in a temporary directory and deletes them afterwards.
//
//   media_test [work-directory]
//
// These are worth testing this way because none of it announces a fault. A
// sector reader that returns the wrong offset, a cue sheet whose second track
// lands in the wrong place, a controller that never answers a command - all of
// them look like "the game did not boot", which is the least useful symptom
// there is.

#include "psx/psx.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using emulation::psx::Cdrom;
using emulation::psx::Disc;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_keep_images = false;
std::string g_test;

void RemoveImage(const std::string& path) {
  if (!g_keep_images)
    remove(path.c_str());
}

void BeginTest(const std::string& name) { g_test = name; }

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    if (g_test.empty())
      printf("  FAIL  %s\n", what);
    else
      printf("  FAIL  %s / %s\n", g_test.c_str(), what);
  }
}

void CheckEqual(uint32_t actual, uint32_t expected, const char* what) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    printf("  FAIL  %s: got %u (0x%X), expected %u (0x%X)\n", what, actual,
           actual, expected, expected);
  }
}

// Writes an image whose every sector is filled with a byte derived from its
// own number, so a reader that returns the wrong sector is caught rather than
// merely returning plausible bytes.
bool WriteImage(const std::string& path, uint32_t sector_size,
                uint32_t sector_count) {
  FILE* fp = fopen(path.c_str(), "wb");
  if (fp == nullptr)
    return false;
  std::string sector(sector_size, '\0');
  for (uint32_t i = 0; i < sector_count; ++i) {
    memset(&sector[0], static_cast<int>(i & 0xFF), sector_size);
    // Stamp the sector number into the first four bytes of the user area so
    // the check does not depend on the fill alone.
    const uint32_t user = (sector_size == 2352) ? 24
                        : (sector_size == 2336) ? 8 : 0;
    memcpy(&sector[user], &i, sizeof(i));
    if (fwrite(sector.data(), 1, sector_size, fp) != sector_size) {
      fclose(fp);
      return false;
    }
  }
  fclose(fp);
  return true;
}

bool WriteText(const std::string& path, const char* text) {
  FILE* fp = fopen(path.c_str(), "wb");
  if (fp == nullptr)
    return false;
  fputs(text, fp);
  fclose(fp);
  return true;
}

uint32_t UserWordOf(const uint8_t* raw_sector) {
  uint32_t value = 0;
  memcpy(&value, raw_sector + 24, sizeof(value));
  return value;
}

void TestBcdAndMsf() {
  printf("msf conversion\n");

  CheckEqual(Disc::ToBcd(0), 0x00, "ToBcd(0)");
  CheckEqual(Disc::ToBcd(9), 0x09, "ToBcd(9)");
  CheckEqual(Disc::ToBcd(10), 0x10, "ToBcd(10)");
  CheckEqual(Disc::ToBcd(59), 0x59, "ToBcd(59)");
  CheckEqual(Disc::FromBcd(0x59), 59, "FromBcd(0x59)");

  // A round trip through MSF has to land back on the same sector, including
  // the awkward ones: exactly a second, exactly a minute, and one before each.
  const uint32_t kCases[] = { 0, 1, 74, 75, 76, 4499, 4500, 4501, 150, 123456 };
  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    uint8_t m, s, f;
    Disc::LbaToMsf(kCases[i], &m, &s, &f);
    CheckEqual(Disc::MsfToLba(m, s, f), kCases[i], "MSF round trip");
  }
}

void TestIsoImage(const std::string& directory) {
  printf("iso image, 2048-byte sectors\n");

  const std::string path = directory + "media_test.iso";
  if (!WriteImage(path, 2048, 100)) {
    printf("  FAIL  could not write %s\n", path.c_str());
    ++g_failures;
    return;
  }

  Disc disc;
  Check(disc.Open(path.c_str()), "open the image");
  CheckEqual(disc.track_count(), 1, "track count");
  CheckEqual(disc.total_sectors(), Disc::kLeadInSectors + 100, "total sectors");
  CheckEqual(disc.track(0).start_lba, Disc::kLeadInSectors, "track 1 start");

  uint8_t sector[Disc::kRawSectorSize];

  // The first sector of the disc is at the lead-in, not at zero. Getting this
  // off by 150 is the classic disc-image mistake and reads perfectly valid
  // data from the wrong place.
  Check(disc.ReadSector(Disc::kLeadInSectors, sector), "read the first sector");
  CheckEqual(UserWordOf(sector), 0, "first sector is sector 0");

  Check(disc.ReadSector(Disc::kLeadInSectors + 42, sector), "read sector 42");
  CheckEqual(UserWordOf(sector), 42, "sector 42 is sector 42");

  // A cooked image has no sync pattern of its own, so the reader has to build
  // one. Without it nothing that inspects the sector header works.
  Check(sector[0] == 0x00 && sector[1] == 0xFF && sector[11] == 0x00,
        "sync pattern was synthesised");
  CheckEqual(sector[15], 0x02, "mode 2 in the synthesised header");
  CheckEqual(Disc::MsfToLba(sector[12], sector[13], sector[14]),
             Disc::kLeadInSectors + 42, "synthesised header holds the address");

  // Off the end of the disc must fail rather than return stale bytes.
  Check(!disc.ReadSector(Disc::kLeadInSectors + 100, sector),
        "reading past the end fails");
  Check(!disc.ReadSector(0, sector), "reading inside the lead-in fails");

  disc.Close();
  Check(!disc.loaded(), "closing ejects");
  remove(path.c_str());
}

void TestRawImageAndCue(const std::string& directory) {
  printf("cue sheet with two tracks\n");

  const std::string bin = directory + "media_test.bin";
  const std::string cue = directory + "media_test.cue";
  if (!WriteImage(bin, 2352, 100)) {
    printf("  FAIL  could not write %s\n", bin.c_str());
    ++g_failures;
    return;
  }
  if (!WriteText(cue,
                 "FILE \"media_test.bin\" BINARY\r\n"
                 "  TRACK 01 MODE2/2352\r\n"
                 "    INDEX 01 00:00:00\r\n"
                 "  TRACK 02 AUDIO\r\n"
                 "    INDEX 01 00:00:60\r\n")) {
    printf("  FAIL  could not write %s\n", cue.c_str());
    ++g_failures;
    return;
  }

  Disc disc;
  Check(disc.Open(cue.c_str()), "open the cue sheet");
  CheckEqual(disc.track_count(), 2, "track count");

  CheckEqual(disc.track(0).start_lba, Disc::kLeadInSectors, "track 1 start");
  CheckEqual(disc.track(0).length, 60, "track 1 length");
  Check(disc.track(0).type == Disc::kTrackData, "track 1 is data");

  CheckEqual(disc.track(1).start_lba, Disc::kLeadInSectors + 60,
             "track 2 start");
  CheckEqual(disc.track(1).length, 40, "track 2 length");
  Check(disc.track(1).type == Disc::kTrackAudio, "track 2 is audio");

  CheckEqual(disc.total_sectors(), Disc::kLeadInSectors + 100, "total sectors");

  // A sector inside track 2 has to resolve through that track's own offset. If
  // the track table is ignored it still returns *a* sector, just the wrong one.
  uint8_t sector[Disc::kRawSectorSize];
  Check(disc.ReadSector(Disc::kLeadInSectors + 65, sector),
        "read a sector in track 2");
  CheckEqual(UserWordOf(sector), 65, "track 2 sector resolves to the right one");

  disc.Close();
  remove(cue.c_str());
  remove(bin.c_str());
}

// Drives the controller the way software does: write parameters, write the
// command, then step time until the interrupt appears and read the response.
class ControllerHarness {
 public:
  explicit ControllerHarness(emulation::psx::System* system)
      : system_(system), cdrom_(&system->cdrom()) {}

  void Command(uint8_t command, const uint8_t* parameters, int count) {
    cdrom_->Write(0x1F801800, 0);            // index 0
    for (int i = 0; i < count; ++i)
      cdrom_->Write(0x1F801802, parameters[i]);
    cdrom_->Write(0x1F801801, command);
  }

  // Runs the clock until an interrupt is raised, then returns its kind and
  // drains the response FIFO. Returns 0 if nothing arrived.
  uint8_t WaitForInterrupt(uint8_t* response, int* length, int max_bytes) {
    for (int step = 0; step < 4000; ++step) {
      cdrom_->Tick(1000);
      cdrom_->Write(0x1F801800, 1);          // index 1
      const uint8_t flags = cdrom_->Read(0x1F801803) & 0x07;
      if (flags == 0)
        continue;

      cdrom_->Write(0x1F801800, 0);
      int count = 0;
      while (count < max_bytes && (cdrom_->Read(0x1F801800) & 0x20))
        response[count++] = cdrom_->Read(0x1F801801);
      *length = count;

      // Acknowledge, so the next queued response can be delivered.
      cdrom_->Write(0x1F801800, 1);
      cdrom_->Write(0x1F801803, 0x07);
      cdrom_->Write(0x1F801800, 0);
      return flags;
    }
    *length = 0;
    return 0;
  }

 private:
  emulation::psx::System* system_;
  Cdrom* cdrom_;
};

void TestControllerWithoutDisc(emulation::psx::System* system) {
  printf("cd-rom controller, empty tray\n");

  ControllerHarness harness(system);
  uint8_t response[16];
  int length = 0;

  // Getstat must answer even with nothing in the drive.
  harness.Command(0x01, nullptr, 0);
  CheckEqual(harness.WaitForInterrupt(response, &length, 16),
             Cdrom::kIntAcknowledge, "Getstat acknowledges");
  Check(length >= 1, "Getstat returns a status byte");

  // GetID is the one that matters: an empty tray has to come back as an error
  // with the "no disc" code, not silence. Silence is what leaves the BIOS
  // shell spinning in its timeout forever.
  harness.Command(0x1A, nullptr, 0);
  CheckEqual(harness.WaitForInterrupt(response, &length, 16),
             Cdrom::kIntAcknowledge, "GetID acknowledges first");
  const uint8_t second = harness.WaitForInterrupt(response, &length, 16);
  CheckEqual(second, Cdrom::kIntError, "GetID reports an error with no disc");
  Check(length >= 2 && response[1] == 0x40, "GetID reports 'no disc'");

  // An unknown command still has to answer. A controller that ignores what it
  // does not understand hangs whatever sent it.
  harness.Command(0xFE, nullptr, 0);
  CheckEqual(harness.WaitForInterrupt(response, &length, 16), Cdrom::kIntError,
             "an unknown command still answers");
}

void TestControllerWithDisc(emulation::psx::System* system,
                            const std::string& directory) {
  printf("cd-rom controller, disc in the tray\n");

  const std::string path = directory + "media_test_ctl.iso";
  if (!WriteImage(path, 2048, 200)) {
    printf("  FAIL  could not write %s\n", path.c_str());
    ++g_failures;
    return;
  }
  Check(system->LoadDisc(path.c_str()), "mount the disc");

  ControllerHarness harness(system);
  uint8_t response[16];
  int length = 0;

  harness.Command(0x1A, nullptr, 0);
  harness.WaitForInterrupt(response, &length, 16);
  CheckEqual(harness.WaitForInterrupt(response, &length, 16),
             Cdrom::kIntComplete, "GetID succeeds with a disc");
  Check(length >= 8 && response[4] == 'S' && response[7] == 'A',
        "GetID reports a licensed region");

  harness.Command(0x13, nullptr, 0);        // GetTN
  CheckEqual(harness.WaitForInterrupt(response, &length, 16),
             Cdrom::kIntAcknowledge, "GetTN answers");
  Check(length >= 3 && response[1] == 0x01 && response[2] == 0x01,
        "GetTN reports one track");

  // Seek to sector 30 of the disc (MSF counts from the lead-in) and read it.
  uint8_t m, s, f;
  Disc::LbaToMsf(Disc::kLeadInSectors + 30, &m, &s, &f);
  const uint8_t location[3] = { m, s, f };
  harness.Command(0x02, location, 3);       // Setloc
  harness.WaitForInterrupt(response, &length, 16);

  harness.Command(0x06, nullptr, 0);        // ReadN
  CheckEqual(harness.WaitForInterrupt(response, &length, 16),
             Cdrom::kIntAcknowledge, "ReadN acknowledges");
  CheckEqual(harness.WaitForInterrupt(response, &length, 16),
             Cdrom::kIntDataReady, "a sector arrives");

  Check(system->cdrom().data_available(), "the sector is readable");
  const uint32_t first_word = system->cdrom().ReadDataWord();
  CheckEqual(first_word, 30, "the sector delivered is the one asked for");

  system->EjectDisc();
  Check(!system->cdrom().disc_loaded(), "ejecting unmounts");
  remove(path.c_str());
}

// ---------------------------------------------------------------------------
// ISO9660, SYSTEM.CNF and the disc boot
// ---------------------------------------------------------------------------

void PutU32Both(uint8_t* p, uint32_t value) {
  // ISO9660 stores 32-bit numbers twice: little-endian, then big-endian.
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
  p[4] = static_cast<uint8_t>(value >> 24);
  p[5] = static_cast<uint8_t>(value >> 16);
  p[6] = static_cast<uint8_t>(value >> 8);
  p[7] = static_cast<uint8_t>(value);
}

// Writes one directory record and returns how many bytes it took.
uint32_t WriteDirectoryRecord(uint8_t* out, const char* name, uint32_t lba,
                              uint32_t size, bool directory) {
  const bool special = (name[0] == '\0' || name[0] == '\1');
  const uint32_t name_length =
      special ? 1 : static_cast<uint32_t>(strlen(name));
  uint32_t length = 33 + name_length;
  if (length & 1)
    ++length;                       // records are padded to an even length

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

struct SyntheticEntry {
  const char* name;
  std::string contents;
  bool directory;
};

// Builds a minimal but real ISO9660 image: a primary volume descriptor at
// sector 16, a root directory at 17, and the files from 18 onward.
//
// Writing the image rather than shipping one keeps the test self-contained,
// and makes the expected values obvious - the layout is right here.
bool WriteIsoImage(const std::string& path,
                   const std::vector<SyntheticEntry>& entries,
                   const char* volume_id) {
  const uint32_t kSector = 2048;
  const uint32_t kDescriptorSector = 16;
  const uint32_t kRootSector = 17;
  const uint32_t kFirstFileSector = 18;

  std::vector<uint32_t> lba(entries.size());
  uint32_t next = kFirstFileSector;
  for (size_t i = 0; i < entries.size(); ++i) {
    lba[i] = next;
    uint32_t sectors =
        static_cast<uint32_t>((entries[i].contents.size() + kSector - 1) / kSector);
    if (sectors == 0)
      sectors = 1;
    next += sectors;
  }
  const uint32_t total_sectors = next;

  std::vector<uint8_t> image(static_cast<size_t>(total_sectors) * kSector, 0);

  uint8_t* pvd = &image[kDescriptorSector * kSector];
  pvd[0] = 0x01;
  memcpy(pvd + 1, "CD001", 5);
  pvd[6] = 0x01;
  memset(pvd + 40, ' ', 32);
  memcpy(pvd + 40, volume_id, strlen(volume_id));
  PutU32Both(pvd + 80, total_sectors);
  WriteDirectoryRecord(pvd + 156, "\0", kRootSector, kSector, true);

  uint8_t* root = &image[kRootSector * kSector];
  uint32_t offset = 0;
  offset += WriteDirectoryRecord(root + offset, "\0", kRootSector, kSector, true);
  offset += WriteDirectoryRecord(root + offset, "\1", kRootSector, kSector, true);
  for (size_t i = 0; i < entries.size(); ++i) {
    offset += WriteDirectoryRecord(
        root + offset, entries[i].name, lba[i],
        static_cast<uint32_t>(entries[i].contents.size()),
        entries[i].directory);
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

// A PS-EXE that does nothing, carrying a recognisable payload so a test can
// prove the bytes reached the load address.
std::string MakePsExe(uint32_t entry_point, uint32_t load_address,
                      uint32_t text_size) {
  std::string exe(0x800 + text_size, '\0');
  memcpy(&exe[0], "PS-X EXE", 8);
  uint8_t* header = reinterpret_cast<uint8_t*>(&exe[0]);
  struct Put {
    uint8_t* base;
    void operator()(uint32_t at, uint32_t value) const {
      base[at + 0] = static_cast<uint8_t>(value);
      base[at + 1] = static_cast<uint8_t>(value >> 8);
      base[at + 2] = static_cast<uint8_t>(value >> 16);
      base[at + 3] = static_cast<uint8_t>(value >> 24);
    }
  };
  const Put put = { header };
  put(0x10, entry_point);       // pc0
  put(0x14, 0);                 // gp0
  put(0x18, load_address);      // t_addr
  put(0x1C, text_size);         // t_size
  for (uint32_t i = 0; i < text_size; ++i)
    exe[0x800 + i] = static_cast<char>(0xA0 + (i & 0x0F));
  return exe;
}

void TestIso9660(const std::string& directory) {
  printf("iso9660 filesystem\n");

  const std::string path = directory + "media_test_fs.iso";
  std::vector<SyntheticEntry> entries;
  SyntheticEntry cnf = { "SYSTEM.CNF;1",
                         "BOOT = cdrom:\\SLUS_007.55;1\r\n"
                         "TCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n",
                         false };
  SyntheticEntry exe = { "SLUS_007.55;1",
                         MakePsExe(0x80010000, 0x80010000, 64), false };
  SyntheticEntry other = { "README.TXT;1", "hello", false };
  entries.push_back(cnf);
  entries.push_back(exe);
  entries.push_back(other);

  if (!WriteIsoImage(path, entries, "TEST VOLUME")) {
    printf("  FAIL  could not write %s\n", path.c_str());
    ++g_failures;
    return;
  }

  Disc disc;
  BeginTest("opening the filesystem");
  Check(disc.Open(path.c_str()), "mount the image");

  emulation::psx::Iso9660 iso;
  Check(iso.Open(&disc), "open the filesystem");
  Check(iso.volume_id() == "TEST VOLUME", "the volume identifier");
  // "." and ".." plus the three files.
  CheckEqual(static_cast<uint32_t>(iso.root().size()), 5u, "root entry count");

  emulation::psx::Iso9660::File found;

  BeginTest("finding a file by the forms software actually writes");
  Check(iso.Find("SYSTEM.CNF", &found), "a bare name");
  Check(!found.directory, "SYSTEM.CNF is a file");
  CheckEqual(found.size, static_cast<uint32_t>(cnf.contents.size()),
             "SYSTEM.CNF size");
  Check(iso.Find("\\SYSTEM.CNF", &found), "a leading backslash");
  Check(iso.Find("/SYSTEM.CNF", &found), "a leading slash");
  Check(iso.Find("SYSTEM.CNF;1", &found), "the version suffix");
  Check(iso.Find("system.cnf", &found), "the wrong case");
  Check(iso.Find("cdrom:\\SYSTEM.CNF;1", &found), "a device prefix");

  BeginTest("a name that is not there is not found");
  Check(!iso.Find("NOTHERE.TXT", &found), "a missing file");
  Check(!iso.Find("", &found), "an empty path");
  Check(!iso.Find("SYSTEM.CNF\\CHILD.TXT", &found),
        "walking into a file rather than a directory");

  BeginTest("reading a file back");
  Check(iso.Find("README.TXT", &found), "find README.TXT");
  std::vector<uint8_t> contents;
  Check(iso.Read(found, &contents), "read it");
  CheckEqual(static_cast<uint32_t>(contents.size()), 5u,
             "the size is exact, not rounded up to a sector");
  Check(contents.size() == 5 && memcmp(&contents[0], "hello", 5) == 0,
        "the contents match");

  BeginTest("an image with no filesystem is rejected");
  disc.Close();
  const std::string audio = directory + "media_test_audio.bin";
  if (WriteImage(audio, 2352, 40)) {
    Disc raw;
    Check(raw.Open(audio.c_str()), "mount a data-less image");
    emulation::psx::Iso9660 empty;
    Check(!empty.Open(&raw), "no CD001 signature, so no filesystem");
    raw.Close();
    remove(audio.c_str());
  }

  remove(path.c_str());
}
// The settings file. It lives here rather than with the SPU because what is
// being checked is the file, not the volume: that a value survives a round
// trip, that an unknown key written by a newer build is not thrown away by an
// older one, and that a nonsense value is clamped rather than passed through
// to the mixer.
void TestSettingsFile(const std::string& directory) {
  printf("settings file\n");

  using emulation::psx::EmuConfig;
  using emulation::psx::SettingsFile;

  const std::string path = directory + "/settings_test.ini";
  remove(path.c_str());

  // A missing file is not a failure, and leaves the defaults alone.
  {
    SettingsFile file;
    Check(!file.Load(path), "a missing settings file reports not-loaded");
    EmuConfig config;
    const float before = config.audio_volume;
    emulation::psx::LoadConfig(file, config);
    CheckEqual(static_cast<int>(config.audio_volume * 100),
               static_cast<int>(before * 100),
               "a missing file leaves the default in place");
  }

  // A value round-trips through the file.
  {
    EmuConfig config;
    config.audio_volume = 3.5f;
    SettingsFile out;
    emulation::psx::StoreConfig(out, config);
    Check(out.Save(path), "settings were written");

    SettingsFile in;
    Check(in.Load(path), "settings were read back");
    EmuConfig loaded;
    emulation::psx::LoadConfig(in, loaded);
    CheckEqual(static_cast<int>(loaded.audio_volume * 100), 350,
               "the volume survived the round trip");
  }

  // A key this build does not know about is preserved rather than dropped, so
  // a file written by a newer build survives being loaded and saved by an
  // older one.
  {
    FILE* fp = fopen(path.c_str(), "w");
    Check(fp != nullptr, "a settings file could be written by hand");
    if (fp != nullptr) {
      fprintf(fp, "# a comment\naudio_volume = 1.5\nsomething_new = 42\n");
      fclose(fp);
    }
    SettingsFile file;
    Check(file.Load(path), "a hand-written file loads");
    CheckEqual(file.GetInt("something_new", 0), 42, "an unknown key is read");
    EmuConfig config;
    emulation::psx::LoadConfig(file, config);
    CheckEqual(static_cast<int>(config.audio_volume * 100), 150,
               "and the known key still applies");

    SettingsFile out = file;
    emulation::psx::StoreConfig(out, config);
    Check(out.Serialise().find("something_new") != std::string::npos,
          "the unknown key is written back out");
  }

  // A value outside the range a menu offers is clamped on the way in, so a
  // hand-edited file cannot hand the mixer a gain that wraps.
  {
    FILE* fp = fopen(path.c_str(), "w");
    if (fp != nullptr) {
      fprintf(fp, "audio_volume = 1000\n");
      fclose(fp);
    }
    SettingsFile file;
    file.Load(path);
    EmuConfig config;
    emulation::psx::LoadConfig(file, config);
    CheckEqual(static_cast<int>(config.audio_volume),
               static_cast<int>(EmuConfig::kMaxAudioVolume),
               "an absurd volume is clamped to the maximum");

    fp = fopen(path.c_str(), "w");
    if (fp != nullptr) {
      fprintf(fp, "audio_volume = -5\n");
      fclose(fp);
    }
    SettingsFile negative;
    negative.Load(path);
    EmuConfig low;
    emulation::psx::LoadConfig(negative, low);
    CheckEqual(static_cast<int>(low.audio_volume),
               static_cast<int>(EmuConfig::kMinAudioVolume),
               "a negative volume is clamped to the minimum");
  }

  remove(path.c_str());
}


void TestSystemCnf() {
  printf("system.cnf parsing\n");

  std::string boot;
  struct Parse {
    static bool Run(const char* text, std::string* out) {
      std::vector<uint8_t> bytes(text, text + strlen(text));
      return emulation::psx::System::ParseSystemCnf(bytes, out);
    }
  };

  BeginTest("the ordinary form");
  Check(Parse::Run("BOOT = cdrom:\\SLUS_007.55;1\r\nTCB = 4\r\n", &boot),
        "parsed");
  Check(boot == "cdrom:\\SLUS_007.55;1", "the value is taken verbatim");

  BeginTest("spacing and line endings vary by publisher");
  Check(Parse::Run("TCB=4\nBOOT=cdrom:\\SCUS_944.26;1\n", &boot), "no spaces");
  Check(boot == "cdrom:\\SCUS_944.26;1", "value");
  Check(Parse::Run("BOOT   =   cdrom:\\A.EXE;1   \r\n", &boot), "extra spaces");
  Check(boot == "cdrom:\\A.EXE;1", "the value is trimmed");
  Check(Parse::Run("boot = cdrom:\\B.EXE;1\n", &boot), "a lower-case key");
  Check(boot == "cdrom:\\B.EXE;1", "value");

  BeginTest("BOOT need not be the first line");
  Check(Parse::Run("TCB = 4\r\nEVENT = 10\r\nBOOT = cdrom:\\C.EXE;1\r\n", &boot),
        "found further down");
  Check(boot == "cdrom:\\C.EXE;1", "value");

  BeginTest("a file with no BOOT line fails rather than guessing");
  Check(!Parse::Run("TCB = 4\r\nEVENT = 10\r\n", &boot), "no BOOT line");
  Check(!Parse::Run("", &boot), "an empty file");
  Check(!Parse::Run("BOOTSTRAP = 4\r\n", &boot),
        "a key that merely starts with BOOT");
}

void TestDiscBoot(emulation::psx::System* system, const std::string& directory) {
  printf("booting a disc\n");

  const std::string path = directory + "media_test_boot.iso";
  std::vector<SyntheticEntry> entries;
  SyntheticEntry cnf = { "SYSTEM.CNF;1", "BOOT = cdrom:\\SLUS_123.45;1\r\n",
                         false };
  SyntheticEntry exe = { "SLUS_123.45;1",
                         MakePsExe(0x80010000, 0x80010000, 64), false };
  entries.push_back(cnf);
  entries.push_back(exe);

  if (!WriteIsoImage(path, entries, "BOOT TEST")) {
    printf("  FAIL  could not write %s\n", path.c_str());
    ++g_failures;
    return;
  }

  BeginTest("the boot succeeds and reports what it found");
  Check(system->LoadDisc(path.c_str()), "mount the disc");
  emulation::psx::System::DiscBootInfo info;
  const bool booted = system->BootDisc(&info);
  if (!booted && info.error != nullptr)
    printf("        BootDisc said: %s\n", info.error);
  Check(booted, "BootDisc returned true");
  Check(info.error == nullptr, "no error was recorded");
  Check(info.volume_id == "BOOT TEST", "the volume identifier");
  Check(info.boot_path == "cdrom:\\SLUS_123.45;1", "the BOOT line");
  Check(info.executable == "SLUS_123.45;1", "the executable it resolved to");

  BeginTest("the executable is in RAM and the pc points at its entry");
  CheckEqual(system->cpu().context()->pc, 0x80010000u, "pc is the entry point");
  const uint8_t* ram = system->ram();
  Check(ram[0x10000] == 0xA0 && ram[0x10001] == 0xA1 && ram[0x1000F] == 0xAF,
        "the payload landed at the load address");

  // A failure has to say which step failed, not just "no". That is the whole
  // difference between a diagnosable boot and "the game did not start".
  BeginTest("a disc with no filesystem fails with a reason");
  const std::string audio = directory + "media_test_noiso.bin";
  if (WriteImage(audio, 2352, 40)) {
    Check(system->LoadDisc(audio.c_str()), "mount it");
    emulation::psx::System::DiscBootInfo bad;
    Check(!system->BootDisc(&bad), "BootDisc returned false");
    Check(bad.error != nullptr, "and said why");
    remove(audio.c_str());
  }

  BeginTest("an empty drive fails with a reason");
  system->EjectDisc();
  emulation::psx::System::DiscBootInfo empty;
  Check(!system->BootDisc(&empty), "BootDisc returned false");
  Check(empty.error != nullptr, "and said why");

  RemoveImage(path);
  if (g_keep_images)
    printf("  kept %s\n", path.c_str());
}
}  // namespace

int main(int argc, char** argv) {
  // A second argument of "keep" leaves the generated images behind, which is
  // how boot_runner --boot-disc gets something to point at without a game.
  std::string directory = (argc > 1) ? argv[1] : ".";
  g_keep_images = (argc > 2 && strcmp(argv[2], "keep") == 0);
  if (!directory.empty() && directory[directory.size() - 1] != '\\' &&
      directory[directory.size() - 1] != '/')
    directory += "\\";

  printf("media_test - disc images and the CD-ROM controller\n\n");

  TestBcdAndMsf();
  TestIsoImage(directory);
  TestRawImageAndCue(directory);
  TestIso9660(directory);
  TestSystemCnf();
  TestSettingsFile(directory);

  // The controller needs a System around it for its interrupt line, but no
  // BIOS: nothing here executes a single instruction.
  emulation::psx::System* system = new emulation::psx::System();
  system->InitializeWithoutBios();
  TestControllerWithoutDisc(system);
  TestControllerWithDisc(system, directory);
  TestDiscBoot(system, directory);
  system->Deinitialize();
  delete system;

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
