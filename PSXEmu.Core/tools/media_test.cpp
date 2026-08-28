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

using emulation::psx::Cdrom;
using emulation::psx::Disc;

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

}  // namespace

int main(int argc, char** argv) {
  std::string directory = (argc > 1) ? argv[1] : ".";
  if (!directory.empty() && directory[directory.size() - 1] != '\\' &&
      directory[directory.size() - 1] != '/')
    directory += "\\";

  printf("media_test - disc images and the CD-ROM controller\n\n");

  TestBcdAndMsf();
  TestIsoImage(directory);
  TestRawImageAndCue(directory);

  // The controller needs a System around it for its interrupt line, but no
  // BIOS: nothing here executes a single instruction.
  emulation::psx::System* system = new emulation::psx::System();
  system->InitializeWithoutBios();
  TestControllerWithoutDisc(system);
  TestControllerWithDisc(system, directory);
  system->Deinitialize();
  delete system;

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
