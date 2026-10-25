// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A synthetic OG project folder (OS-007e), as the files of one folder with their relative paths, built from the byte maps of og_fixture.h (which
// shares no code with the readers): the tests put them on a host directory (DirStorage) and on a FAT32 image (FatStorage), load them through
// app::OgFolderLoader and compare with the same bytes read straight through the formats readers. Header-only.
//
//   project.mt                     version 16, "Synthetic Project", playlist 1 2 2 3 12
//   patterns/pattern_01.mtp, 02, 03, 12     12-track version 5, a different seed each
//   patterns/pattern_07.mtp        cut to 100 bytes: fails (truncated), the load goes on
//   patterns/patternsMetadata      a valid PAMD file: not read by this slice, counted as ignored
//   patterns/._pattern_01.mtp      what macOS leaves beside a file: ignored
//   patterns/old/pattern_09.mtp    a subdirectory: ignored, never entered
//   instruments/instrument_01.mti  slot 0, a whole .mti
//   instruments/02 Snare.pti       slot 1, 1000 frames
//   instruments/instrument_03.pti  slot 2, 500 frames (the Workspace name of firmware 1.5+)
//   instruments/instrument_05.mti  slot 4, named "Old Clap"
//   instruments/05 Clap.pti        slot 4, 800 frames, named "New Clap": the .pti wins
//   instruments/instrument_06.mti  slot 5, one bit of its CRC flipped: fails, the load goes on
//   instruments/readme.txt         ignored

#include "og_fixture.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace o3os::test::og {

struct FolderFile {
  std::string path;  // relative to the folder, '/' separated
  Bytes bytes;
};

inline Bytes text_bytes(const char* s) { return Bytes(s, s + std::strlen(s)); }

inline std::vector<FolderFile> synthetic_folder() {
  std::vector<FolderFile> files;
  ProjectSpec ps;  // v16, "Synthetic Project", playlist {1, 2, 2, 3, 12}
  files.push_back({"project.mt", project_bytes(ps)});
  files.push_back({"patterns/pattern_01.mtp", pattern_bytes(rich_pattern(12, 5, 101))});
  files.push_back({"patterns/pattern_02.mtp", pattern_bytes(rich_pattern(12, 5, 102))});
  files.push_back({"patterns/pattern_03.mtp", pattern_bytes(rich_pattern(12, 5, 103))});
  files.push_back({"patterns/pattern_12.mtp", pattern_bytes(rich_pattern(12, 5, 112))});
  Bytes cut = pattern_bytes(rich_pattern(12, 5, 107));
  cut.resize(100);
  files.push_back({"patterns/pattern_07.mtp", cut});
  files.push_back({"patterns/patternsMetadata", pamd_bytes({"intro", "verse", "chorus"})});
  files.push_back({"patterns/._pattern_01.mtp", Bytes(120, 0x5A)});
  files.push_back({"patterns/old/pattern_09.mtp", pattern_bytes(rich_pattern(12, 5, 109))});

  InstrumentSpec mti;
  mti.pti = false;
  mti.file_name = "Kick 808.wav";
  files.push_back({"instruments/instrument_01.mti", instrument_bytes(mti)});
  InstrumentSpec snare;
  snare.pti = true;
  snare.frames = 1000;
  snare.file_name = "Snare.wav";
  files.push_back({"instruments/02 Snare.pti", instrument_bytes(snare)});
  InstrumentSpec ws;
  ws.pti = true;
  ws.frames = 500;
  ws.file_name = "Workspace Hat.wav";
  files.push_back({"instruments/instrument_03.pti", instrument_bytes(ws)});
  InstrumentSpec old_clap;
  old_clap.pti = false;
  old_clap.file_name = "Old Clap";
  files.push_back({"instruments/instrument_05.mti", instrument_bytes(old_clap)});
  InstrumentSpec new_clap;
  new_clap.pti = true;
  new_clap.frames = 800;
  new_clap.file_name = "New Clap";
  files.push_back({"instruments/05 Clap.pti", instrument_bytes(new_clap)});
  InstrumentSpec broken;
  broken.pti = false;
  broken.file_name = "Broken";
  Bytes bad = instrument_bytes(broken);
  bad[100] ^= 0x01;  // a byte inside the checksummed part: the CRC no longer matches
  files.push_back({"instruments/instrument_06.mti", bad});
  files.push_back({"instruments/readme.txt", text_bytes("not an instrument")});
  return files;
}

// What the folder is worth, so the tests do not repeat the numbers: files by kind.
inline constexpr unsigned kFolderPatternsGood = 4;      // 01, 02, 03, 12
inline constexpr unsigned kFolderPatternsBad = 1;       // 07
inline constexpr unsigned kFolderPtiFiles = 3;          // 02, 03, 05: always applied
inline constexpr unsigned kFolderMtiFiles = 2;          // 01, 05: applied, except that 05 may be skipped (a .pti of its slot) or overwritten, by order
inline constexpr unsigned kFolderInstrumentsBad = 1;    // 06
inline constexpr std::uint32_t kFolderPcmFrames = 1000 + 500 + 800;
inline constexpr unsigned kFolderIgnored = 4;           // patternsMetadata, ._pattern_01.mtp, old/, readme.txt

}  // namespace o3os::test::og
