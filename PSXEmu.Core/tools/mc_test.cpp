// mc_test - the memory card: its on-card format, the editor's operations, and the file
// underneath, checked against the format rather than against this implementation.
//
// The standards document's rule for anything written, wiped and read back applies twice here.
// For the editor, "delete then undelete" must give back the card byte for byte and "export
// everything, format, import it all" must give back the same saves - a directory that records
// nothing would pass anything weaker. For the file, the card goes out of the slot and comes back
// in from disk, so a flush that writes nothing cannot look like one that works.
//
// Checksums are worked by hand where a formula would only restate the code: a formatted
// header is 'M' ^ 'C' = 0Eh, a free directory frame A0h ^ FFh ^ FFh = A0h.

#include "psx/psx.h"
#include "psx/mc_directory.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using emulation::psx::MC;
namespace mcdir = emulation::psx::mcdir;

namespace {

int g_checks = 0;
int g_failures = 0;

void Group(const char* name) { printf("%s\n", name); }

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckEqual(uint32_t got, uint32_t want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %u (0x%X) want %u (0x%X)\n", what, got, got, want, want);
  }
}

typedef std::vector<uint8_t> Card;

Card FormattedCard() {
  Card card(mcdir::kCardSize);
  mcdir::Format(card.data());
  return card;
}

const uint8_t* DirFrame(const Card& card, int block) { return &card[block * mcdir::kFrameSize]; }

bool AllChecksumsValid(const Card& card, int first_frame, int last_frame) {
  for (int f = first_frame; f <= last_frame; ++f) {
    const uint8_t* frame = &card[f * mcdir::kFrameSize];
    if (frame[127] != mcdir::FrameChecksum(frame))
      return false;
  }
  return true;
}

// A .mcs built by hand: a directory frame naming the save, then `blocks` blocks whose first
// frame is a title frame - "ＴＥＳＴ" in full-width Shift-JIS, one icon frame, palette entry 1
// pure red - and whose remaining bytes carry `fill` so each save's data is recognisable.
std::vector<uint8_t> MakeSave(const char* name, int blocks, uint8_t fill) {
  std::vector<uint8_t> mcs(mcdir::kFrameSize + blocks * mcdir::kBlockSize, fill);
  uint8_t* dir = mcs.data();
  memset(dir, 0, mcdir::kFrameSize);
  dir[0] = 0x51;
  const uint32_t size = static_cast<uint32_t>(blocks * mcdir::kBlockSize);
  memcpy(dir + 4, &size, 4);
  dir[8] = 0xFF;
  dir[9] = 0xFF;
  strncpy(reinterpret_cast<char*>(dir + 0x0A), name, 20);
  dir[127] = mcdir::FrameChecksum(dir);

  uint8_t* title = mcs.data() + mcdir::kFrameSize;
  memset(title, 0, mcdir::kFrameSize);
  title[0] = 'S';
  title[1] = 'C';
  title[2] = 0x11;
  title[3] = static_cast<uint8_t>(blocks);
  const uint8_t sjis_test[] = { 0x82, 0x73, 0x82, 0x64, 0x82, 0x72, 0x82, 0x73 };   // ＴＥＳＴ
  memcpy(title + 4, sjis_test, sizeof(sjis_test));
  title[0x60 + 2] = 0x1F;   // palette entry 1: red 31, green 0, blue 0
  uint8_t* icon = title + mcdir::kFrameSize;
  memset(icon, 0, mcdir::kFrameSize);
  icon[0] = 0x10;           // pixel 0 is entry 0 (transparent), pixel 1 is entry 1
  return mcs;
}

void TestFormat() {
  Group("format");
  const Card card = FormattedCard();
  Check(mcdir::IsFormatted(card.data()), "the header says MC");
  CheckEqual(card[127], 0x0E, "header checksum is 'M' ^ 'C'");
  CheckEqual(DirFrame(card, 1)[0], 0xA0, "directory frames start free");
  CheckEqual(DirFrame(card, 1)[8] | (DirFrame(card, 1)[9] << 8), 0xFFFF, "with no next block");
  CheckEqual(DirFrame(card, 1)[127], 0xA0, "free frame checksum is A0h ^ FFh ^ FFh");
  Check(AllChecksumsValid(card, 0, 35), "header, directory and broken-sector frames all sum");
  Check(memcmp(&card[63 * mcdir::kFrameSize], &card[0], mcdir::kFrameSize) == 0,
        "the write-test frame copies the header");
  CheckEqual(mcdir::FreeBlocks(card.data()), 15, "fifteen free blocks");
  CheckEqual(static_cast<uint32_t>(mcdir::List(card.data(), true).size()), 0, "nothing listed");
}

void TestImportAndList() {
  Group("import and list");
  Card card = FormattedCard();
  std::string error;
  Check(mcdir::Import(card.data(), MakeSave("BASLUS-01013LOM", 3, 0x5A), &error), "imports");
  const auto saves = mcdir::List(card.data(), false);
  CheckEqual(static_cast<uint32_t>(saves.size()), 1, "one save");
  if (saves.size() != 1)
    return;
  const auto& save = saves[0];
  Check(save.filename == "BASLUS-01013LOM", "the name survives");
  CheckEqual(save.blocks, 3, "three blocks");
  CheckEqual(save.size, 3 * 8192, "and the size says so");
  Check(!save.deleted, "live");
  Check(save.title == L"TEST", "full-width Shift-JIS title narrowed to TEST");
  CheckEqual(save.icon_frames, 1, "one icon frame");
  CheckEqual(save.icons.size() >= 2 ? save.icons[0] : 1, 0, "palette 0 is transparent");
  CheckEqual(save.icons.size() >= 2 ? save.icons[1] : 0, 0xFFFF0000, "palette 1 is opaque red");
  CheckEqual(mcdir::FreeBlocks(card.data()), 12, "twelve blocks left");
  CheckEqual(DirFrame(card, 1)[0], 0x51, "block 1 starts it");
  CheckEqual(DirFrame(card, 2)[0], 0x52, "block 2 continues it");
  CheckEqual(DirFrame(card, 3)[0], 0x53, "block 3 ends it");
  CheckEqual(DirFrame(card, 1)[8], 1, "block 1 links to block 2 (stored as 2 - 1)");
  CheckEqual(DirFrame(card, 3)[8] | (DirFrame(card, 3)[9] << 8), 0xFFFF, "block 3 ends the chain");
  Check(AllChecksumsValid(card, 0, 15), "every directory frame it touched still sums");
  CheckEqual(card[2 * mcdir::kBlockSize + 200], 0x5A, "the save's data landed in block 2");

  Check(!mcdir::Import(card.data(), MakeSave("BASLUS-01013LOM", 1, 0), &error),
        "a second save of the same name is refused");
  Check(!mcdir::Import(card.data(), MakeSave("BIG", 13, 0), &error),
        "a save bigger than the free space is refused");
  CheckEqual(mcdir::FreeBlocks(card.data()), 12, "and the refusals changed nothing");
  std::vector<uint8_t> truncated = MakeSave("SHORT", 1, 0);
  truncated.resize(truncated.size() - 1);
  Check(!mcdir::Import(card.data(), truncated, &error), "a file that is not frame + blocks is refused");
}

void TestExport() {
  Group("export");
  Card card = FormattedCard();
  std::string error;
  const std::vector<uint8_t> original = MakeSave("BESCES-00867", 2, 0x33);
  mcdir::Import(card.data(), original, &error);
  std::vector<uint8_t> exported;
  Check(mcdir::Export(card.data(), 1, &exported, &error), "exports");
  CheckEqual(static_cast<uint32_t>(exported.size()), static_cast<uint32_t>(original.size()),
             "a directory frame and two blocks");
  Check(memcmp(exported.data() + 128, original.data() + 128, original.size() - 128) == 0,
        "the blocks are the save's own");
  Check(memcmp(exported.data() + 0x0A, "BESCES-00867", 12) == 0, "and the name comes with them");
  CheckEqual(exported[127], mcdir::FrameChecksum(exported.data()), "its directory frame sums");
  Check(!mcdir::Export(card.data(), 2, &exported, &error), "a middle block is not a save");
}

void TestDeleteUndelete() {
  Group("delete and undelete");
  Card card = FormattedCard();
  std::string error;
  mcdir::Import(card.data(), MakeSave("FIRST", 3, 0x11), &error);
  const Card before = card;

  Check(mcdir::Delete(card.data(), 1, &error), "deletes");
  CheckEqual(static_cast<uint32_t>(mcdir::List(card.data(), false).size()), 0, "gone from the list");
  const auto deleted = mcdir::List(card.data(), true);
  Check(deleted.size() == 1 && deleted[0].deleted && deleted[0].blocks == 3,
        "still there as a deleted, three-block save");
  CheckEqual(mcdir::FreeBlocks(card.data()), 15, "its blocks count as free");
  CheckEqual(DirFrame(card, 1)[0], 0xA1, "first block A1h");
  CheckEqual(DirFrame(card, 2)[0], 0xA2, "middle block A2h");
  CheckEqual(DirFrame(card, 3)[0], 0xA3, "last block A3h");

  Check(mcdir::Undelete(card.data(), 1, &error), "undeletes");
  Check(card == before, "and the card is byte for byte what it was");

  // Now let a new save take one of its blocks. Free blocks go first, so it takes the twelve
  // free ones and then the deleted save's first: undelete has to see that and refuse.
  mcdir::Delete(card.data(), 1, &error);
  Check(mcdir::Import(card.data(), MakeSave("SECOND", 13, 0x22), &error),
        "a thirteen-block save fits in twelve free blocks and one deleted one");
  Check(!mcdir::Undelete(card.data(), 1, &error), "undelete refuses once a block is reused");
  const auto live = mcdir::List(card.data(), false);
  Check(live.size() == 1 && live[0].filename == "SECOND", "and SECOND is intact");
}

void TestRoundTrip() {
  Group("export everything, format, import it back");
  Card card = FormattedCard();
  std::string error;
  mcdir::Import(card.data(), MakeSave("ALPHA", 1, 0xA1), &error);
  mcdir::Import(card.data(), MakeSave("BRAVO", 4, 0xB2), &error);
  mcdir::Import(card.data(), MakeSave("CHARLIE", 2, 0xC3), &error);
  mcdir::Delete(card.data(), 2, &error);                         // BRAVO: leaves a hole
  mcdir::Import(card.data(), MakeSave("DELTA", 3, 0xD4), &error);

  const auto saves = mcdir::List(card.data(), false);
  std::vector<std::vector<uint8_t>> exported;
  for (const auto& save : saves) {
    exported.emplace_back();
    mcdir::Export(card.data(), save.first_block, &exported.back(), &error);
  }
  mcdir::Format(card.data());
  CheckEqual(static_cast<uint32_t>(mcdir::List(card.data(), true).size()), 0, "the format wiped it");
  for (const auto& mcs : exported)
    mcdir::Import(card.data(), mcs, &error);

  const auto again = mcdir::List(card.data(), false);
  CheckEqual(static_cast<uint32_t>(again.size()), static_cast<uint32_t>(saves.size()),
             "the same number of saves");
  bool same = again.size() == saves.size();
  for (size_t i = 0; same && i < saves.size(); ++i) {
    same = again[i].filename == saves[i].filename && again[i].blocks == saves[i].blocks &&
           again[i].size == saves[i].size && again[i].title == saves[i].title;
  }
  Check(same, "with the same names, sizes and titles");
  std::vector<uint8_t> first_again;
  mcdir::Export(card.data(), again.empty() ? 1 : again[0].first_block, &first_again, &error);
  Check(!exported.empty() && first_again == exported[0], "and the same bytes");
}

std::string TempCardPath() {
  char dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  return std::string(dir) + "mc_test_card.mcr";
}

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
  std::vector<uint8_t> bytes;
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp)
    return bytes;
  fseek(fp, 0, SEEK_END);
  bytes.resize(static_cast<size_t>(ftell(fp)));
  fseek(fp, 0, SEEK_SET);
  bytes.resize(fread(bytes.data(), 1, bytes.size(), fp));
  fclose(fp);
  return bytes;
}

void TestTheFile() {
  Group("the card file");
  const std::string path = TempCardPath();
  DeleteFileA(path.c_str());

  MC mc;
  mc.Initialize();
  CheckEqual(mc.CreateFile(path.c_str()), S_OK, "creates");
  Check(mc.connected(), "and inserts it");
  const std::vector<uint8_t> created = ReadFileBytes(path);
  CheckEqual(static_cast<uint32_t>(created.size()), 0x20000, "a 128 KB file");
  Check(created.size() == 0x20000 && mcdir::IsFormatted(created.data()), "already formatted");
  CheckEqual(mc.flag() & 0x08, 0x08, "flagged as a new card");

  uint8_t sector[128];
  memset(sector, 0x77, sizeof(sector));
  Check(mc.WriteSector(200, sector), "a game writes a sector");
  CheckEqual(mc.flag() & 0x08, 0, "which clears the new-card flag");
  Check(mc.dirty(), "the card is dirty");
  Check(ReadFileBytes(path)[200 * 128] != 0x77, "and the file is not touched yet");

  for (int i = 0; i < MC::kFlushAfterIdleFrames - 1; ++i)
    mc.OnFrame();
  Check(mc.dirty(), "still waiting one frame short of a second");
  mc.OnFrame();
  Check(!mc.dirty(), "flushed after a second with no writes");
  CheckEqual(ReadFileBytes(path)[200 * 128], 0x77, "the file has the sector");
  Check(GetFileAttributesA((path + ".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
        "and no temporary file is left behind");

  memset(sector, 0x99, sizeof(sector));
  mc.WriteSector(300, sector);
  mc.Eject();
  Check(!mc.connected(), "ejected");
  CheckEqual(ReadFileBytes(path)[300 * 128], 0x99, "ejecting saved the unflushed write");

  // The wipe: in from disk again, and what was written has to be there.
  CheckEqual(mc.LoadFile(path.c_str()), S_OK, "reinserts");
  uint8_t back[128];
  mc.ReadSector(200, back);
  CheckEqual(back[0], 0x77, "the flushed sector survived the round trip");
  mc.ReadSector(300, back);
  CheckEqual(back[0], 0x99, "and so did the one saved by the eject");

  mc.data()[5 * 128] = 0x42;
  mc.Modified();
  CheckEqual(mc.flag() & 0x08, 0x08, "an editor's change flags the card as swapped");
  Check(mc.Flush() && ReadFileBytes(path)[5 * 128] == 0x42, "and is saved");

  // Inserting another card over this one must not lose a pending write to it.
  const std::string other = path + ".other";
  DeleteFileA(other.c_str());
  {
    MC maker;
    maker.Initialize();
    maker.CreateFile(other.c_str());
  }
  memset(sector, 0xAB, sizeof(sector));
  mc.WriteSector(400, sector);
  CheckEqual(mc.LoadFile(other.c_str()), S_OK, "a second card goes in over the first");
  CheckEqual(ReadFileBytes(path)[400 * 128], 0xAB, "and the first was saved on its way out");

  std::vector<uint8_t> junk(1000, 0);
  FILE* fp = fopen(other.c_str(), "wb");
  fwrite(junk.data(), 1, junk.size(), fp);
  fclose(fp);
  CheckEqual(mc.LoadFile(other.c_str()), S_FALSE, "a file that is not 128 KB is refused");
  Check(mc.connected() && mc.filename() == other, "leaving the card that was in, in");

  mc.Deinitialize();
  DeleteFileA(path.c_str());
  DeleteFileA(other.c_str());
}

}  // namespace

// mc_test <card.mcr>: lists a card instead of testing - read only, the file is never written.
int ListCard(const char* path) {
  const std::vector<uint8_t> card = ReadFileBytes(path);
  if (card.size() != mcdir::kCardSize) {
    printf("%s: not a 128 KB card (%zu bytes)\n", path, card.size());
    return 1;
  }
  printf("%s\n  %s, %d of 15 blocks free\n", path,
         mcdir::IsFormatted(card.data()) ? "formatted" : "NOT formatted",
         mcdir::FreeBlocks(card.data()));
  for (const auto& save : mcdir::List(card.data(), true)) {
    char title[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, save.title.c_str(), -1, title, sizeof(title), nullptr,
                        nullptr);
    int opaque = 0;
    for (uint32_t pixel : save.icons)
      opaque += (pixel >> 24) != 0;
    printf("  block %2d  %-20s  %2d block%s  %6u bytes  icon %d frame%s (%3d opaque px)%s  %s\n",
           save.first_block, save.filename.c_str(), save.blocks, save.blocks == 1 ? " " : "s",
           save.size, save.icon_frames, save.icon_frames == 1 ? " " : "s", opaque,
           save.deleted ? "  DELETED" : "", title);
  }
  return 0;
}

// Other tools' files (mcdir::CardFromFile, SaveFromFile, ImportCard): the same card and the
// same saves have to come out of every wrapper they arrive in.
void TestOtherFormats() {
  Group("other tools' cards and saves");
  Card source = FormattedCard();
  std::string error;
  mcdir::Import(source.data(), MakeSave("BASLUS-01013LOM", 3, 0x5A), &error);
  mcdir::Import(source.data(), MakeSave("BESCES-00867RR", 1, 0x33), &error);

  // The same card in each wrapper.
  struct Wrapper { const char* name; std::vector<uint8_t> file; };
  std::vector<Wrapper> wrappers;
  wrappers.push_back({ "raw", source });
  {
    std::vector<uint8_t> gme(0xF40, 0);
    memcpy(gme.data(), "123-456-STD", 11);
    gme.insert(gme.end(), source.begin(), source.end());
    wrappers.push_back({ "DexDrive .gme", gme });
  }
  {
    std::vector<uint8_t> vgs(64, 0);
    memcpy(vgs.data(), "VgsM", 4);
    vgs.insert(vgs.end(), source.begin(), source.end());
    wrappers.push_back({ "VGS .mem", vgs });
  }
  {
    std::vector<uint8_t> psx(256, 0);
    memcpy(psx.data(), "PSV", 3);
    psx.insert(psx.end(), source.begin(), source.end());
    wrappers.push_back({ ".psx card", psx });
  }
  for (const Wrapper& wrapper : wrappers) {
    std::vector<uint8_t> card;
    std::string format;
    const bool ok = mcdir::CardFromFile(wrapper.file, &card, &format, &error);
    char what[96];
    snprintf(what, sizeof(what), "a %s card unwraps to the card inside it", wrapper.name);
    Check(ok && card == source, what);
  }
  {
    // Some .gme files stop short; what is there is kept and the rest is blank.
    std::vector<uint8_t> gme(0xF40, 0);
    memcpy(gme.data(), "123-456-STD", 11);
    gme.insert(gme.end(), source.begin(), source.begin() + 4 * mcdir::kBlockSize);
    std::vector<uint8_t> card;
    Check(mcdir::CardFromFile(gme, &card, nullptr, &error) && card.size() == mcdir::kCardSize &&
              memcmp(card.data(), source.data(), 4 * mcdir::kBlockSize) == 0,
          "a short .gme keeps what it has");
  }
  {
    std::vector<uint8_t> card;
    Check(!mcdir::CardFromFile(std::vector<uint8_t>(1000, 0), &card, nullptr, &error),
          "a file that is no card is refused");
    Check(!mcdir::CardFromFile(std::vector<uint8_t>(mcdir::kCardSize, 0), &card, nullptr, &error),
          "and so is an unformatted one, which holds no saves");
  }

  // A whole card imported onto another: every save that fits, and an account of the rest.
  Card target = FormattedCard();
  mcdir::Import(target.data(), MakeSave("BESCES-00867RR", 1, 0x77), &error);   // already there
  mcdir::Import(target.data(), MakeSave("FILLER", 11, 0x11), &error);          // leaves 3 free
  std::string report;
  Check(mcdir::ImportCard(target.data(), source.data(), &report), "a card's saves import");
  const auto saves = mcdir::List(target.data(), false);
  bool lom = false;
  for (const auto& save : saves)
    lom = lom || save.filename == "BASLUS-01013LOM";
  Check(lom, "the one that fits is on the card");
  CheckEqual(mcdir::FreeBlocks(target.data()), 0, "in the three free blocks");
  Check(report.find("Copied 1 of 2") != std::string::npos &&
            report.find("BESCES-00867RR") != std::string::npos,
        "and the report names the one already there");
  {
    Card full = FormattedCard();
    mcdir::Import(full.data(), MakeSave("FULL", 15, 0x22), &error);
    const Card before = full;
    Check(!mcdir::ImportCard(full.data(), source.data(), &report), "nothing fits: refused");
    Check(full == before, "and the card is exactly as it was");
  }

  // A raw save: its blocks alone, named after its file.
  const std::vector<uint8_t> mcs = MakeSave("BASLUS-00000", 2, 0x44);
  const std::vector<uint8_t> raw(mcs.begin() + mcdir::kFrameSize, mcs.end());
  std::vector<uint8_t> converted;
  Check(mcdir::SaveFromFile(raw, "BASLUS-01251FF9-SAVE01EXTRA", &converted, &error),
        "a raw save converts");
  Card card = FormattedCard();
  Check(mcdir::Import(card.data(), converted, &error), "and imports");
  const auto raw_saves = mcdir::List(card.data(), false);
  Check(raw_saves.size() == 1 && raw_saves[0].filename == "BASLUS-01251FF9-SAVE",
        "named after its file, cut to 20 characters");
  Check(raw_saves.size() == 1 && raw_saves[0].blocks == 2 && raw_saves[0].title == L"TEST",
        "two blocks, and its title read from the data");
  Check(memcmp(&card[raw_saves.empty() ? 0 : raw_saves[0].first_block * mcdir::kBlockSize],
               raw.data(), mcdir::kBlockSize) == 0,
        "the data exactly as it was");
  Check(mcdir::SaveFromFile(mcs, "ignored", &converted, &error) && converted == mcs,
        "a .mcs passes through untouched");
  std::vector<uint8_t> not_a_save(mcdir::kBlockSize, 0);
  Check(!mcdir::SaveFromFile(not_a_save, "X", &converted, &error),
        "a block that does not start with a title is not taken for a save");
}

int main(int argc, char** argv) {
  if (argc > 1)
    return ListCard(argv[1]);
  printf("mc_test - memory card format, editor operations and file\n\n");
  TestFormat();
  TestImportAndList();
  TestExport();
  TestDeleteUndelete();
  TestRoundTrip();
  TestTheFile();
  TestOtherFormats();
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
