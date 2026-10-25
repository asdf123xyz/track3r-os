// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FILES part B: the RECENT list and its file, Settings/recent.o3rc (app/recent_list.h). The format is new and versioned from its first byte: "O3RC 1", then up to five project names, newest first.
// The loader never fails: a missing, short, damaged, oversized or foreign file is an empty list (or the lines that are good), and nothing is read past the bytes it was given. Round trip, the header
// rules, a version that is not 1, invalid and repeated names, the five-name cap, the list's own operations, and a mutation sweep over a good file.
#include <doctest/doctest.h>

#include <o3os/app/recent_list.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using o3os::app::RecentList;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes bytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

unsigned parse(RecentList& list, const std::string& text) {
  const Bytes b = bytes(text);
  return list.parse(b.data(), b.size());
}

std::string file_of(const RecentList& list) {
  std::uint8_t out[RecentList::kMaxFileBytes];
  const std::size_t n = list.write(out, sizeof out);
  return std::string(reinterpret_cast<const char*>(out), n);
}

std::vector<std::string> names(const RecentList& list) {
  std::vector<std::string> out;
  for (unsigned i = 0; i < list.count(); ++i) {
    out.push_back(list.name(i));
  }
  return out;
}

using Names = std::vector<std::string>;

}  // namespace

TEST_CASE("recent file: round trip of a full list, in order, and the file is what the format says") {
  RecentList a;
  for (const char* n : {"Oldest", "Song 2", "Track 01", "My-Song_v2.1", "Newest"}) {
    CHECK(a.add(n));
  }
  CHECK(names(a) == Names{"Newest", "My-Song_v2.1", "Track 01", "Song 2", "Oldest"});
  const std::string file = file_of(a);
  CHECK(file == "O3RC 1\nNewest\nMy-Song_v2.1\nTrack 01\nSong 2\nOldest\n");
  RecentList b;
  CHECK(parse(b, file) == 5u);
  CHECK(names(b) == names(a));
  CHECK(file_of(b) == file);  // and the file of what was read is the same file
  CHECK(file.size() <= RecentList::kMaxFileBytes);
  // an empty list is a file of its header
  RecentList empty;
  CHECK(file_of(empty) == "O3RC 1\n");
  CHECK(parse(b, "O3RC 1\n") == 0u);
  CHECK(b.count() == 0u);
  // the longest names still fit the size the format promises
  RecentList longest;
  for (char c : {'a', 'b', 'c', 'd', 'e'}) {
    CHECK(longest.add(std::string(31, c).c_str()));
  }
  CHECK(file_of(longest).size() <= RecentList::kMaxFileBytes);
  RecentList back;
  CHECK(parse(back, file_of(longest)) == 5u);
}

TEST_CASE("recent file: a missing, empty, short or foreign file is an empty list and the list it replaces is gone") {
  RecentList list;
  list.add("Old");
  CHECK(list.parse(nullptr, 0) == 0u);
  CHECK(list.count() == 0u);
  list.add("Old");
  CHECK(parse(list, "") == 0u);
  CHECK(list.count() == 0u);
  for (const char* bad : {"O", "O3RC", "O3RC ", "O3RC 1x\nA\n", "O3RC  1\nA\n", "O3RC1\nA\n", "o3rc 1\nA\n", " O3RC 1\nA\n", "O3RC 1 \nA\n", "O3RC\n1\nA\n", "\nO3RC 1\nA\n", "A\n", "A\nO3RC 1\n",
                          "O3RC -1\nA\n", "O3RC +1\nA\n", "O3RC 01x\nA\n", "O3RC 1.0\nA\n", "O3PJ 1\nA\n", "\xFF\xFE\x00\x01", "\x00\x00\x00\x00\x00\x00\x00\x00"}) {
    INFO("file: " << bad);
    CHECK(list.parse(reinterpret_cast<const std::uint8_t*>(bad), std::strlen(bad) != 0 ? std::strlen(bad) : 8u) == 0u);
    CHECK(list.count() == 0u);
  }
}

TEST_CASE("recent file: a version that is not 1 is an empty list, whatever follows (a newer file is not read as this one)") {
  RecentList list;
  for (const char* header : {"O3RC 2", "O3RC 0", "O3RC 10", "O3RC 99", "O3RC 100", "O3RC 255", "O3RC 65537", "O3RC 4294967297", "O3RC 99999999999999999999"}) {
    INFO("header: " << header);
    CHECK(parse(list, std::string(header) + "\nA\nB\n") == 0u);
    CHECK(list.count() == 0u);
  }
  CHECK(parse(list, "O3RC 1\nA\nB\n") == 2u);
  CHECK(parse(list, "O3RC 1\r\nA\r\nB\r\n") == 2u);  // a file saved by an editor with CR LF is read
  CHECK(names(list) == Names{"A", "B"});
}

TEST_CASE("recent file: lines that are not a project name are skipped, the others kept in order") {
  RecentList list;
  CHECK(parse(list, "O3RC 1\nGood\n\n..\n../x\na/b\n.hidden\nCON\ntrail \n lead\nTwo Words\ncaf\xC3\xA9\na\tb\nGood\ngood\nLast\n") == 3u);
  CHECK(names(list) == Names{"Good", "Two Words", "Last"});  // the repeats (any case) are one entry; a name FAT would refuse is not a name
  // a line past the fifth is not read
  CHECK(parse(list, "O3RC 1\nA\nB\nC\nD\nE\nF\nG\n") == 5u);
  CHECK(names(list) == Names{"A", "B", "C", "D", "E"});
  // a line that is too long, a name with an embedded NUL, no final newline, only newlines
  CHECK(parse(list, std::string("O3RC 1\n") + std::string(32, 'x') + "\nOk\n") == 1u);
  CHECK(names(list) == Names{"Ok"});
  CHECK(parse(list, std::string("O3RC 1\nab\0cd\nOk\n", 16)) == 1u);
  CHECK(names(list) == Names{"Ok"});
  CHECK(parse(list, "O3RC 1\nNo Newline") == 1u);
  CHECK(names(list) == Names{"No Newline"});
  CHECK(parse(list, "O3RC 1\n\n\n\n\n") == 0u);
}

TEST_CASE("recent file: a file over the size the format allows is damaged: an empty list") {
  RecentList list;
  std::string big = "O3RC 1\n";
  while (big.size() <= RecentList::kMaxFileBytes) {
    big += "A name\n";
  }
  CHECK(parse(list, big) == 0u);
  CHECK(list.count() == 0u);
  CHECK(parse(list, std::string(100000, 'x')) == 0u);
  // the largest good file, with CR LF line ends, is read whole
  std::string max = "O3RC 1\r\n";
  for (char c : {'a', 'b', 'c', 'd', 'e'}) {
    max += std::string(31, c) + "\r\n";
  }
  CHECK(max.size() == RecentList::kMaxFileBytes);
  CHECK(parse(list, max) == 5u);
}

TEST_CASE("recent file: every single-byte damage and every truncation of a good file gives a list that is valid, never more than five, and the loader stays inside the bytes it was given") {
  RecentList source;
  for (const char* n : {"Five", "Four", "Three", "Two", "One"}) {
    source.add(n);
  }
  const std::string good = file_of(source);
  for (std::size_t cut = 0; cut <= good.size(); ++cut) {  // truncation: exact bytes, so a read past the end would be a sanitizer's catch
    const Bytes b(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
    RecentList list;
    const unsigned n = list.parse(b.empty() ? nullptr : b.data(), b.size());
    CHECK(n == list.count());
    CHECK(n <= RecentList::kMax);
    for (unsigned i = 0; i < n; ++i) {
      CHECK(o3os::hal::storage::save::valid_name(list.name(i)));
    }
    if (cut < 7) {
      CHECK(n == 0u);  // no complete header
    }
  }
  for (std::size_t at = 0; at < good.size(); ++at) {
    for (unsigned flip : {0x01u, 0x20u, 0x80u, 0xFFu}) {
      Bytes b = bytes(good);
      b[at] = static_cast<std::uint8_t>(b[at] ^ flip);
      RecentList list;
      const unsigned n = list.parse(b.data(), b.size());
      CHECK(n == list.count());
      CHECK(n <= RecentList::kMax);
      for (unsigned i = 0; i < n; ++i) {
        CHECK(o3os::hal::storage::save::valid_name(list.name(i)));
      }
      if (at < 6 && flip != 0x20u) {
        CHECK(n == 0u);  // a damaged header gives nothing (a flip of 0x20 turns the space into a NUL-free other byte: also nothing, below)
      }
    }
  }
}

TEST_CASE("recent list: add puts a project first, moves it up when it is there, drops the oldest at five, refuses a name the card does not take") {
  RecentList list;
  const std::uint32_t g0 = list.generation();
  CHECK(list.add("A"));
  CHECK(list.add("B"));
  CHECK(names(list) == Names{"B", "A"});
  CHECK(list.add("A"));
  CHECK(names(list) == Names{"A", "B"});
  const std::uint32_t g1 = list.generation();
  CHECK(g1 != g0);
  CHECK(list.add("A"));  // already first: no change, no write wanted
  CHECK(list.generation() == g1);
  CHECK(list.add("a"));  // the same folder spelled another way (FAT): the spelling moves with it
  CHECK(names(list) == Names{"a", "B"});
  CHECK(list.generation() != g1);
  for (const char* n : {"C", "D", "E", "F"}) {
    CHECK(list.add(n));
  }
  CHECK(names(list) == Names{"F", "E", "D", "C", "a"});  // B left
  CHECK(list.count() == 5u);
  const std::uint32_t g2 = list.generation();
  for (const char* bad : {"", "..", "a/b", "CON", " x", "x ", "caf\xC3\xA9"}) {
    CHECK_FALSE(list.add(bad));
  }
  CHECK_FALSE(list.add(nullptr));
  CHECK(list.generation() == g2);
  CHECK(list.count() == 5u);
  // add of a name in the middle of a full list: the others keep their order, nobody leaves
  CHECK(list.add("D"));
  CHECK(names(list) == Names{"D", "F", "E", "C", "a"});
}

TEST_CASE("recent list: remove and drop") {
  RecentList list;
  for (const char* n : {"A", "B", "C"}) {
    list.add(n);
  }
  CHECK(names(list) == Names{"C", "B", "A"});
  const std::uint32_t g = list.generation();
  CHECK_FALSE(list.remove("Z"));
  CHECK(list.generation() == g);
  CHECK(list.remove("b"));
  CHECK(names(list) == Names{"C", "A"});
  CHECK(list.generation() != g);
  list.add("Q");
  list.add("R");
  // drop leaves the generation alone
  const std::uint32_t before = list.generation();
  CHECK(list.drop("Q"));
  CHECK(names(list) == Names{"R", "C", "A"});
  CHECK(list.generation() == before);
  CHECK_FALSE(list.drop("Q"));
  CHECK(list.contains("r"));
  CHECK_FALSE(list.contains("Q"));
  CHECK_FALSE(list.contains(nullptr));
  list.clear();
  CHECK(list.count() == 0u);
  CHECK(std::string(list.name(0)) == "");
  CHECK(std::string(list.name(99)) == "");
}

TEST_CASE("recent list: write refuses a buffer that is too small and writes nothing") {
  RecentList list;
  list.add("One");
  list.add("Two");
  std::uint8_t out[32];
  std::memset(out, 0xAA, sizeof out);
  CHECK(list.write(out, 5) == 0u);
  CHECK(out[0] == 0xAA);
  CHECK(list.write(nullptr, 100) == 0u);
  CHECK(list.write(out, sizeof out) == std::string("O3RC 1\nTwo\nOne\n").size());
}
