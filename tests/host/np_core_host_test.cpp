// Host test for src/net/np_core.cpp (pad scripts, input queue, recordings,
// masked copies, sweep batches). Built and run by run_np_core_test.bat.

#include "../../src/net/np_core.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace doa4::np;

static void TestPadBytes() {
  Pad pad;
  pad.buttons = 0x1234;
  pad.lt = 0xAB;
  pad.rt = 0x01;
  pad.lx = -2;
  pad.ly = 0x7F00;
  pad.rx = -32768;
  pad.ry = 1;
  uint8_t g[12];
  PadToGuest(pad, g);
  const uint8_t want[12] = {0x12, 0x34, 0xAB, 0x01, 0xFF, 0xFE, 0x7F, 0x00, 0x80, 0x00, 0x00, 0x01};
  CHECK(std::memcmp(g, want, 12) == 0);  // big-endian, the guest's XINPUT_GAMEPAD order
  CHECK(PadFromGuest(g) == pad);
}

static void TestScript() {
  Script s;
  std::string error;
  const char* text =
      "# boot\n"
      "connect 0 1\n"
      "6 A+UP START\n"
      "3 - -   # neutral\n"
      "shot\n"
      "2 LX=-32768+LT=255\n"
      "quit\n";
  CHECK(ParseScript(text, &s, &error));
  CHECK(error.empty());
  CHECK(s.frames.size() == 11);
  if (s.frames.size() == 11) {
    CHECK(s.frames[0].connected == 3);
    CHECK(s.frames[0].pad[0].buttons == (kA | kUp));
    CHECK(s.frames[0].pad[1].buttons == kStart);
    CHECK(s.frames[5].pad[0].buttons == (kA | kUp));
    CHECK(s.frames[6].pad[0] == Pad{});
    CHECK(s.frames[6].pad[1] == Pad{});
    CHECK(s.frames[9].pad[0].lx == -32768);
    CHECK(s.frames[9].pad[0].lt == 255);
    CHECK(s.frames[9].pad[0].buttons == 0);
    CHECK(s.frames[9].pad[1] == Pad{});  // a user not named is neutral
    CHECK(s.frames[10].connected == 3);
  }
  // shot after 6 + 3 frames (not after the first line only), quit at the end.
  CHECK(s.events.size() == 2);
  if (s.events.size() == 2) {
    CHECK((s.events[0] == ScriptEvent{ScriptEvent::kShot, 9}));
    CHECK((s.events[1] == ScriptEvent{ScriptEvent::kQuit, 11}));
  }

  // Default connection: user 0 only.
  Script d;
  CHECK(ParseScript("1 B", &d, &error));
  CHECK(d.frames.size() == 1 && d.frames[0].connected == 1 && d.frames[0].pad[0].buttons == kB);

  // Errors name the line.
  Script e;
  CHECK(!ParseScript("1 A\n6 FOO\n", &e, &error));
  CHECK(error.find("line 2") != std::string::npos);
  CHECK(!ParseScript("x A\n", &e, &error));
  CHECK(!ParseScript("2 LX=40000\n", &e, &error));  // out of range for a stick
  CHECK(!ParseScript("connect 4\n", &e, &error));
  CHECK(!ParseScript("1 A B C D E\n", &e, &error));  // five users
}

static void TestRandom() {
  Script a, b, c;
  std::string error;
  CHECK(ParseScript("connect 0 1\nrandom 600 7 0 1\n", &a, &error));
  CHECK(ParseScript("connect 0 1\nrandom 600 7 0 1\n", &b, &error));
  CHECK(ParseScript("connect 0 1\nrandom 600 8 0 1\n", &c, &error));
  CHECK(a.frames.size() == 600);
  CHECK(a.frames == b.frames);   // a pure function of the seed
  CHECK(!(a.frames == c.frames));
  int pressed0 = 0, pressed1 = 0, distinct = 0;
  uint16_t last = 0xFFFF;
  for (const FrameInput& f : a.frames) {
    pressed0 += f.pad[0].buttons != 0;
    pressed1 += f.pad[1].buttons != 0;
    if (f.pad[0].buttons != last) ++distinct;
    last = f.pad[0].buttons;
    CHECK(f.pad[2] == Pad{});  // only the named users move
  }
  CHECK(pressed0 > 300 && pressed1 > 300);  // something is held most of the time
  CHECK(distinct > 50);                      // and it changes
  Script one;
  CHECK(ParseScript("random 100 7 1\n", &one, &error));
  for (const FrameInput& f : one.frames) CHECK(f.pad[0] == Pad{});
}

static void TestQueue() {
  Script s1, s2;
  std::string error;
  CHECK(ParseScript("connect 0 1\n2 A\nshot\n1 B\n", &s1, &error));
  CHECK(ParseScript("1 X\nquit\n", &s2, &error));
  InputQueue q;
  std::vector<ScriptEvent::Kind> fired;
  // Nothing queued: neutral, user 0 connected.
  FrameInput f = q.Next(10, &fired);
  CHECK(f.connected == 1 && f.pad[0] == Pad{} && fired.empty());
  CHECK(q.Append(s1, 100) == 100);
  CHECK(q.Append(s2, 100) == 103);  // after everything queued
  CHECK(q.queued_until() == 104);
  for (uint32_t frame = 11; frame < 100; ++frame) q.Next(frame, &fired);
  CHECK(fired.empty());
  f = q.Next(100, &fired);
  CHECK(f.pad[0].buttons == kA && f.connected == 3);
  f = q.Next(101, &fired);
  CHECK(f.pad[0].buttons == kA && fired.empty());
  f = q.Next(102, &fired);
  CHECK(f.pad[0].buttons == kB);
  CHECK(fired.size() == 1 && fired[0] == ScriptEvent::kShot);  // shot at 100 + 2
  fired.clear();
  f = q.Next(103, &fired);
  CHECK(f.pad[0].buttons == kX && f.connected == 3);  // s2 has no connect line: keeps 0 1
  CHECK(fired.empty());
  f = q.Next(104, &fired);
  CHECK(f.pad[0] == Pad{} && f.connected == 3);  // empty again: neutral, last connected set
  CHECK(fired.size() == 1 && fired[0] == ScriptEvent::kQuit);
  // Appended later than the queue's end: starts at `now`.
  fired.clear();
  CHECK(q.Append(s2, 200) == 200);
  // Only the frames before a script's connect line inherit the queue's set.
  Script s3;
  CHECK(ParseScript("1 A\nconnect 0\n1 B\n", &s3, &error));
  CHECK(s3.inherit_frames == 1);
  CHECK(q.Append(s3, 300) == 300);
  for (uint32_t frame = 105; frame < 300; ++frame) q.Next(frame, &fired);
  f = q.Next(300, &fired);
  CHECK(f.pad[0].buttons == kA && f.connected == 3);
  f = q.Next(301, &fired);
  CHECK(f.pad[0].buttons == kB && f.connected == 1);
  f = q.Next(302, &fired);
  CHECK(f.pad[0] == Pad{} && f.connected == 1);
}

static void TestRecording() {
  FrameInput a, b;
  a.connected = 3;
  a.pad[0].buttons = kA;
  a.pad[1].lx = -100;
  b.connected = 1;
  b.pad[0].rt = 200;
  std::string bytes = EncodeRecHeader(0x1122334455667788ull, 5);
  CHECK(bytes.size() == kRecHeaderBytes);
  bytes += EncodeRecFrame(a);
  bytes += EncodeRecFrame(b);
  bytes += EncodeRecFrame(a);
  CHECK(bytes.size() == kRecHeaderBytes + 3 * kRecFrameBytes);
  // Guest byte layout of frame 0: connected, 3 zeros, then user 0's gamepad.
  CHECK(uint8_t(bytes[kRecHeaderBytes]) == 3);
  CHECK(uint8_t(bytes[kRecHeaderBytes + 4]) == 0x10 && uint8_t(bytes[kRecHeaderBytes + 5]) == 0x00);
  Recording r;
  std::string error, note;
  CHECK(DecodeRecording(bytes, &r, &error, &note));
  CHECK(r.seed == 0x1122334455667788ull && r.flags == 5);
  CHECK(r.frames.size() == 3);
  if (r.frames.size() == 3) {
    CHECK(r.frames[0] == a && r.frames[1] == b && r.frames[2] == a);
  }
  CHECK(note.empty());
  // A crash mid-frame: the partial frame is dropped and reported.
  std::string cut = bytes.substr(0, bytes.size() - 10);
  CHECK(DecodeRecording(cut, &r, &error, &note));
  CHECK(r.frames.size() == 2);
  CHECK(!note.empty());
  std::string bad = bytes;
  bad[0] = 'X';
  CHECK(!DecodeRecording(bad, &r, &error));
  CHECK(!DecodeRecording(bytes.substr(0, 10), &r, &error));

  // Version 2: the meta text follows the 32-byte header.
  std::string v2 = EncodeRecHeader(7, 0, "build=ab\nfixes=all\n");
  CHECK(v2.size() == kRecHeaderBytes + 19);
  CHECK(uint8_t(v2[8]) == 2 && uint8_t(v2[28]) == 19);
  v2 += EncodeRecFrame(a);
  CHECK(DecodeRecording(v2, &r, &error, &note));
  CHECK(r.meta == "build=ab\nfixes=all\n" && r.frames.size() == 1 && r.frames[0] == a);
  CHECK(!DecodeRecording(v2.substr(0, kRecHeaderBytes + 5), &r, &error));  // meta cut short
  // Version 1 (the proof's recordings) still reads, with no meta.
  std::string v1 = bytes;
  v1[8] = 1;
  v1[28] = 0;
  CHECK(DecodeRecording(v1, &r, &error, &note) && r.meta.empty() && r.frames.size() == 3);
  v1[28] = 7;  // version 1's word at byte 28 was "reserved": whatever it holds is not a meta length
  CHECK(DecodeRecording(v1, &r, &error, &note) && r.meta.empty() && r.frames.size() == 3 && note.empty());

  // Beyond the brief: wrong versions this excludes. The frame body is measured from the END of the meta
  // (a body measured from byte 32 turns the meta into a "partial frame": note set), the meta length is
  // checked at its edge (header + meta and no frames reads, one byte short does not), a recording whose
  // meta is empty is still version 2, and versions 0 and 3 are refused.
  std::string two = EncodeRecHeader(9, 1, "build=ab\nfixes=all\n");
  two += EncodeRecFrame(a);
  two += EncodeRecFrame(b);
  CHECK(DecodeRecording(two, &r, &error, &note));
  CHECK(note.empty() && r.frames.size() == 2 && r.frames[0] == a && r.frames[1] == b);
  CHECK(r.seed == 9 && r.flags == 1 && r.meta.size() == 19);
  const std::string head_only = EncodeRecHeader(9, 1, "build=ab\nfixes=all\n");
  CHECK(DecodeRecording(head_only, &r, &error, &note) && r.frames.empty() && r.meta.size() == 19);
  CHECK(!DecodeRecording(head_only.substr(0, head_only.size() - 1), &r, &error));
  std::string cut2 = two.substr(0, two.size() - 10);  // a crash mid-frame still drops the partial frame
  CHECK(DecodeRecording(cut2, &r, &error, &note) && r.frames.size() == 1 && !note.empty());
  CHECK(EncodeRecHeader(9, 1).size() == kRecHeaderBytes && uint8_t(EncodeRecHeader(9, 1)[8]) == 2);
  std::string v3 = two, v0 = two;
  v3[8] = 3;
  v0[8] = 0;
  CHECK(!DecodeRecording(v3, &r, &error) && error == "unsupported recording version");
  CHECK(!DecodeRecording(v0, &r, &error));
}

static void TestMetaMismatch() {
  const std::string cur = "build=aa\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0\n";
  CHECK(!cur.empty() && MetaMismatch(cur, cur).empty());
  CHECK(MetaMismatch("build=bb\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0\n", cur) == "build bb vs aa");
  CHECK(MetaMismatch("build=aa\nfixes=all\nserial=3\nsweep=4\ntweaks=x=1\n", cur) ==
        "serial 3 vs default; tweaks x=1 vs x=0");
  CHECK(MetaMismatch("", cur).find("build (none) vs aa") == 0);  // a version-1 recording
  CHECK(MetaMismatch("other=1\nbuild=aa\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0", cur).empty());

  // Beyond the brief: wrong versions this excludes.
  // Every one of the five keys is compared on its own (a key dropped from the list survives the
  // brief's build / serial / tweaks fixtures).
  CHECK(MetaMismatch("build=aa\nfixes=none\nserial=default\nsweep=4\ntweaks=x=0\n", cur) == "fixes none vs all");
  CHECK(MetaMismatch("build=aa\nfixes=all\nserial=default\nsweep=all\ntweaks=x=0\n", cur) == "sweep all vs 4");
  // A version-1 recording names all five keys, in the fixed order, with the whole current value.
  CHECK(MetaMismatch("", cur) ==
        "build (none) vs aa; fixes (none) vs all; serial (none) vs default; sweep (none) vs 4; tweaks (none) vs x=0");
  // Keys are whole names at the start of a line: "xbuild" and "build2" are other keys (a substring find
  // or a prefix compare reads their values), and a key with a value that holds '=' keeps all of it.
  CHECK(MetaMismatch("xbuild=zz\nbuild2=yy\nbuild=aa\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0\n", cur).empty());
  CHECK(MetaMismatch("xbuild=aa\nbuild2=aa\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0\n", cur) ==
        "build (none) vs aa");
  CHECK(MetaMismatch("build=aa\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0,y=1\n", cur) ==
        "tweaks x=0,y=1 vs x=0");
  // Keys outside the five never count, whatever their values; an empty value is a value, not "(none)".
  CHECK(MetaMismatch("other=2\nbuild=aa\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0\n",
                     "other=1\nbuild=aa\nfixes=all\nserial=default\nsweep=4\ntweaks=x=0\n").empty());
  CHECK(MetaMismatch("build=aa\nfixes=all\nserial=\nsweep=4\ntweaks=x=0\n", cur) == "serial  vs default");
  CHECK(MetaMismatch("build=aa\nfixes=all\nserial=default\nsweep=4\n", cur) == "tweaks (none) vs x=0");
}

static void TestMaskedCopy() {
  uint8_t mem[16];
  for (int i = 0; i < 16; ++i) mem[i] = uint8_t(i + 1);
  Region region{"r", 0x1000, 16, {{4, 2}, {14, 2}}};
  std::vector<uint8_t> out;
  MaskedCopy(region, mem, &out);
  CHECK(out.size() == 16);
  if (out.size() == 16) {
    CHECK(out[3] == 4 && out[4] == 0 && out[5] == 0 && out[6] == 7);
    CHECK(out[13] == 14 && out[14] == 0 && out[15] == 0);
  }
  Region beyond{"b", 0, 4, {{2, 10}}};  // a mask running past the end is clipped
  MaskedCopy(beyond, mem, &out);
  CHECK(out.size() == 4 && out[1] == 2 && out[2] == 0 && out[3] == 0);
}

static void TestBlockMasks() {
  const auto m = BlockMasks(0x82B70000, 0x10000, 4, {{0x82B70010, 4}, {0x82B8FFFE, 4}, {0x82BB0000, 8}});
  CHECK(m.size() == 4);
  if (m.size() == 4) {
    CHECK(m[0].size() == 1 && m[0][0].offset == 0x10 && m[0][0].length == 4);
    CHECK(m[1].size() == 1 && m[1][0].offset == 0xFFFE && m[1][0].length == 2);  // split at the border
    CHECK(m[2].size() == 1 && m[2][0].offset == 0 && m[2][0].length == 2);
    CHECK(m[3].empty());  // 0x82BB0000 is past the 4 blocks
  }
  // Wrong versions this excludes: a block size or base taken from the sweep's constants instead of the
  // arguments (0x100-byte blocks at 0x1000), a range starting below lo (index underflow, or dropped
  // instead of clipped), a range covering a whole block and running past the end (not clipped at hi),
  // a zero-length range (an empty mask), two ranges in one block (one lost, or reordered).
  // (helpers, not size guards: every check below runs whatever BlockMasks returns, so a mutant cannot
  // shrink the check count)
  const auto n = BlockMasks(0x1000, 0x100, 3, {{0x0FF8, 0x10}, {0x10F0, 0x220}, {0x1020, 0}, {0x1040, 2}});
  const std::vector<Mask> none;
  auto block = [&](size_t k) -> const std::vector<Mask>& { return k < n.size() ? n[k] : none; };
  auto has = [&](size_t k, size_t j, uint32_t offset, uint32_t length) {
    return j < block(k).size() && block(k)[j].offset == offset && block(k)[j].length == length;
  };
  CHECK(n.size() == 3);
  CHECK(block(0).size() == 3);
  CHECK(has(0, 0, 0, 8));        // 0x0FF8..0x1008 clipped to lo
  CHECK(has(0, 1, 0xF0, 0x10));  // 0x10F0..0x1100
  CHECK(has(0, 2, 0x40, 2));     // in input order; the empty range adds nothing
  CHECK(block(1).size() == 1 && has(1, 0, 0, 0x100));  // the whole block
  CHECK(block(2).size() == 1 && has(2, 0, 0, 0x100));  // 0x1310 clipped at 0x1300
  CHECK(BlockMasks(0x1000, 0x100, 0, {{0x1000, 4}}).empty());  // no blocks, no masks (and no write past them)
  // The sweep's use: MaskedCopy of a block with its masks zeroes exactly the masked bytes.
  std::vector<uint8_t> mem(0x300, 0xAA);
  std::vector<uint8_t> out;
  MaskedCopy(Region{"", 0x1100, 0x100, block(1)}, mem.data() + 0x100, &out);
  CHECK(out.size() == 0x100 && std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0; }));
  MaskedCopy(Region{"", 0x1000, 0x100, block(0)}, mem.data(), &out);
  CHECK(out.size() == 0x100 && out[7] == 0 && out[8] == 0xAA && out[0x3F] == 0xAA && out[0x40] == 0 &&
        out[0x41] == 0 && out[0x42] == 0xAA && out[0xEF] == 0xAA && out[0xF0] == 0 && out[0xFF] == 0);
}

// The sweep hashes a masked block through a Region built ONCE at Start (BlockRegions), where it used to
// build Region{"", lo, block, BlockMasks(...)[k]} - a copy of the mask vector, one heap allocation - for
// every masked block of every frame. The bytes hashed must not change: here the old per-hash construction
// and the prebuilt Region copy the same 64 KB blocks, and the results are compared byte for byte.
static void TestBlockRegions() {
  const uint32_t lo = 0x82B70000, block = 0x10000, nblocks = 5;
  // Block 0 holds two ranges (in input order), blocks 1 and 2 share one range across their border,
  // block 3 is covered whole, block 4 has none.
  const std::vector<std::pair<uint32_t, uint32_t>> ranges = {
      {0x82B70010, 4}, {0x82B8FFFE, 4}, {0x82BA0000, 0x10000}, {0x82B70200, 0x1C}};
  std::vector<uint8_t> mem(size_t(block) * nblocks);
  for (size_t i = 0; i < mem.size(); ++i) mem[i] = uint8_t(1 + (i * 31 + i / 251) % 250);  // never 0
  const std::vector<Region> regions = BlockRegions(lo, block, nblocks, ranges);
  const auto masks = BlockMasks(lo, block, nblocks, ranges);
  CHECK(regions.size() == nblocks && masks.size() == nblocks);
  // (every check below runs whatever BlockRegions returns, so a mutant cannot shrink the check count)
  const Region no_region;
  const std::vector<Mask> no_masks;
  auto region = [&](uint32_t k) -> const Region& { return k < regions.size() ? regions[k] : no_region; };
  auto mask_list = [&](uint32_t k) -> const std::vector<Mask>& { return k < masks.size() ? masks[k] : no_masks; };
  uint32_t masked_blocks = 0, differing = 0;
  std::vector<uint8_t> legacy, fresh;
  for (uint32_t k = 0; k < nblocks; ++k) {
    const uint8_t* at = mem.data() + size_t(k) * block;
    MaskedCopy(Region{"", lo + k * block, block, mask_list(k)}, at, &legacy);  // the per-hash construction
    MaskedCopy(region(k), at, &fresh);
    CHECK(legacy == fresh);
    CHECK(fresh.size() == block);
    CHECK(region(k).address == lo + k * block && region(k).length == block);
    CHECK(region(k).masks.size() == mask_list(k).size());
    masked_blocks += region(k).masks.empty() ? 0 : 1;
    differing += std::equal(fresh.begin(), fresh.end(), at) ? 0 : 1;  // the mask changed bytes of this block
  }
  // Non-vacuous: four blocks are masked and their copies differ from the raw bytes; the fifth is not.
  CHECK(masked_blocks == 4 && differing == 4);
  CHECK(region(4).masks.empty());
  CHECK(region(0).masks.size() == 2 && region(0).masks[0].offset == 0x10 && region(0).masks[1].offset == 0x200);
  // The masked bytes are zeros in a block whose raw bytes are all non-zero.
  MaskedCopy(region(0), mem.data(), &fresh);
  auto byte = [&](size_t i) { return i < fresh.size() ? int(fresh[i]) : -1; };  // -1: no such byte
  CHECK(byte(0xF) > 0 && byte(0x10) == 0 && byte(0x13) == 0 && byte(0x14) > 0);
  CHECK(byte(0x1FF) > 0 && byte(0x200) == 0 && byte(0x21B) == 0 && byte(0x21C) > 0);
  MaskedCopy(region(3), mem.data() + size_t(3) * block, &fresh);
  CHECK(fresh.size() == block && std::all_of(fresh.begin(), fresh.end(), [](uint8_t b) { return b == 0; }));
  // No blocks, no regions.
  CHECK(BlockRegions(lo, block, 0, ranges).empty());
}

static void TestSweep() {
  CHECK(SweepFirst(0, 4, 10) == 0);
  CHECK(SweepFirst(1, 4, 10) == 4);
  CHECK(SweepFirst(2, 4, 10) == 8);
  CHECK(SweepFirst(3, 4, 10) == 2);  // wraps: 12 % 10
  CHECK(SweepFirst(7, 10, 10) == 0);  // every block every frame
  CHECK(EncodeSweepHeader(0x82B70000, 0x835B0000, 0x10000, 4).size() == 32);
  std::vector<Region> regions{{"a", 1, 2, {}}, {"b", 3, 4, {}}};
  CHECK(EncodeDigestHeader(regions).size() == 16 + 2 * 32);
}

static void TestCounters() {
  const std::string h = EncodeCountersHeader({"fb_a", "pr_b"});
  CHECK(h.size() == 16 + 2 * 24);
  CHECK(h.substr(0, 8) == "D4NPCNT1");
  CHECK(uint8_t(h[8]) == 1 && h[9] == 0 && h[10] == 0 && h[11] == 0);
  CHECK(uint8_t(h[12]) == 2 && h[13] == 0);
  CHECK(std::string(h.data() + 16) == "fb_a");
  CHECK(std::string(h.data() + 40) == "pr_b");
  const std::string cut = EncodeCountersHeader({std::string(30, 'x')});
  CHECK(cut.size() == 16 + 24 && cut[16 + 22] == 'x' && cut[16 + 23] == '\0');  // cut to 23 + terminator
  const std::string f = EncodeCountersFrame(0x01020304, {5, 0xA0B0C0D0});
  const uint8_t want[16] = {4, 3, 2, 1, 0, 0, 0, 0, 5, 0, 0, 0, 0xD0, 0xC0, 0xB0, 0xA0};
  CHECK(f.size() == 16 && std::memcmp(f.data(), want, 16) == 0);
}

// The virtual clocks (np_core.h): a read inside a frame advances the thread's clock a little; a
// frame whose start lies behind what an earlier frame's reads reached continues from the last value.
// Expected values are derived from the formulas: time base 50 MHz / 100000 = 500 ticks a read and
// 50,000,000 / 60 = 833,333 a frame; tick count 1 ms per 16 reads and 1000 / 60 = 16 ms a frame;
// FILETIME 100 a read and 10,000,000 / 60 = 166,666 a frame.
static void TestVirtualClocks() {
  ThreadClock c;
  CHECK(VirtualTimebase(&c, 0) == kTimebaseBase + 500);  // 10 us a read
  CHECK(VirtualTimebase(&c, 0) == kTimebaseBase + 1000);
  CHECK(VirtualTimebase(&c, 1) == kTimebaseBase + 833333 + 500);  // a new frame starts its reads again
  ThreadClock busy;
  uint64_t last = 0;
  for (int i = 0; i < 2000; ++i) last = VirtualTimebase(&busy, 0);  // more reads than a frame holds (1,667)
  CHECK(last == kTimebaseBase + 2000 * 500);
  CHECK(VirtualTimebase(&busy, 1) == last + 1);  // frame 1 would start at +833,833: never backwards
  ThreadClock t;
  CHECK(VirtualTickCount(&t, 0) == kTickBase);
  for (int i = 0; i < 300; ++i) VirtualTickCount(&t, 0);
  CHECK(VirtualTickCount(&t, 0) == kTickBase + 18);  // read 302: 301 >> 4 = 18 ms
  CHECK(VirtualTickCount(&t, 1) == kTickBase + 18);  // frame 1 starts at +16 ms: held at +18
  ThreadClock ft;
  CHECK(VirtualFileTime(&ft, 0) == kFileTimeBase + 100);
  for (int i = 0; i < 2000; ++i) VirtualFileTime(&ft, 0);  // last: +200,100
  CHECK(VirtualFileTime(&ft, 1) == kFileTimeBase + 200101);  // frame 1 would start at +166,766

  // Beyond the supplied checks. Each passes on the plain formulas, so each is proved by a mutation
  // probe, not by the RED: once a frame starts past the overrun the clock is back on the formula
  // (frame 2 starts at +1,666,666 time base, +33 ms, +333,333 FILETIME) and its reads count from the
  // frame's start again; and "never backwards" is a property of one thread's clock (a fresh thread
  // reading in frame 1 is not held up by the busy ones).
  CHECK(VirtualTimebase(&busy, 2) == kTimebaseBase + 1666666 + 500);
  CHECK(VirtualTimebase(&busy, 2) == kTimebaseBase + 1666666 + 1000);
  CHECK(VirtualTickCount(&t, 2) == kTickBase + 33);
  CHECK(VirtualFileTime(&ft, 2) == kFileTimeBase + 333333 + 100);
  ThreadClock other;
  CHECK(VirtualTimebase(&other, 1) == kTimebaseBase + 833333 + 500);
  CHECK(VirtualTickCount(&other, 1) == kTickBase + 16);
  CHECK(VirtualFileTime(&other, 1) == kFileTimeBase + 166666 + 100);

  // The step edge and a frame where two spellings of the formula disagree (review of Task 5, #0330).
  // Tick count: read n of a frame (1-based) returns (n - 1) >> 4 ms past the frame's start, so the first
  // step is at read 17: 15 -> +0, 16 -> +0, 17 -> +1. NOT the pre-increment spelling (++reads >> 4),
  // which steps at read 16; the reads 1 and 302 above give the same answers under both.
  ThreadClock edge;
  for (int i = 0; i < 14; ++i) VirtualTickCount(&edge, 0);
  CHECK(VirtualTickCount(&edge, 0) == kTickBase);      // read 15
  CHECK(VirtualTickCount(&edge, 0) == kTickBase);      // read 16
  CHECK(VirtualTickCount(&edge, 0) == kTickBase + 1);  // read 17
  // Frame 3, a new thread's first read. 3 * 50,000,000 / 60 = 2,500,000 (NOT frame * (50,000,000 / 60) =
  // 3 * 833,333 = 2,499,999); 3 * 1000 / 60 = 50 ms (NOT 3 * 16 = 48); 3 * 10,000,000 / 60 = 500,000
  // (NOT 3 * 166,666 = 499,998). Then the first read's own step: 500 time base, 100 FILETIME, +0 ms.
  ThreadClock f3tb, f3tick, f3ft;
  CHECK(VirtualTimebase(&f3tb, 3) == kTimebaseBase + 2500000 + 500);
  CHECK(VirtualTickCount(&f3tick, 3) == kTickBase + 50);
  CHECK(VirtualFileTime(&f3ft, 3) == kFileTimeBase + 500000 + 100);
}

// The SDK audio worker's clock (np_core.h): one callback renders 256 samples at 48 kHz, so callback n
// is n * 256 / 48000 s after the lockstep started. Derived: 3 callbacks = 768 samples = 16 ms =
// 800,000 ticks of 50 MHz = 160,000 FILETIME units; 2 callbacks = 512 samples = 10.67 ms, truncated to 10.
static void TestAudioClocks() {
  CHECK(AudioTimebase(0) == kTimebaseBase);
  CHECK(AudioTimebase(3) == kTimebaseBase + 800000);  // 768 samples = 16 ms of 50 MHz ticks
  CHECK(AudioTickCount(3) == kTickBase + 16);
  CHECK(AudioTickCount(2) == kTickBase + 10);         // 512 samples = 10.67 ms, truncated
  CHECK(AudioFileTime(3) == kFileTimeBase + 160000);

  // Beyond the supplied checks (each passes on the formulas as written; proved by a mutation probe).
  // The division truncates the whole product, not each callback: 1 callback = 266,666.67 ticks,
  // 2 = 533,333.33 (NOT 2 * 266,666 = 533,332). And the product does not overflow 32 bits: a 6,995-frame
  // session runs about 21,900 callbacks and 100,000 callbacks * 256 * 1000 = 2.56e10 > 2^32 (the tick
  // count truncates 533,333.33 ms; the time base 100,000 * 256 * 50e6 / 48000 = 26,666,666,666.67; FILETIME
  // 100,000 * 256 * 10e6 / 48000 = 5,333,333,333.33).
  CHECK(AudioTimebase(1) == kTimebaseBase + 266666);
  CHECK(AudioTimebase(2) == kTimebaseBase + 533333);
  CHECK(AudioTickCount(100000) == kTickBase + 533333);
  CHECK(AudioTimebase(100000) == kTimebaseBase + 26666666666ull);
  CHECK(AudioFileTime(100000) == kFileTimeBase + 5333333333ull);
}

// The seed an MT gets at its (k + 1)th seeding in a session, and the per-MT count that makes k.
// Offline play seeds each of the four MTs once, at boot (a 6,995-frame Single record has 4 seedings,
// pr_reseeds = 4), so no replay reaches k >= 1: this is where the series is pinned.
// Oracles: k = 0 is what the proof's formula logged in a real run (np_log of a record run, session seed
// 1: "seed: MT 8321B198 seed 000003E8 -> 17CEE0B2" and the three others); the k >= 1 values come from a
// separate implementation of the formula (splitmix64 finaliser of session ^ state << 32 ^ k * golden
// ratio, low 32 bits, in Python), not from this code.
static void TestMtSeeds() {
  CHECK(MtSeed(1, 0x8321B198, 0) == 0x17CEE0B2);
  CHECK(MtSeed(1, 0x833F7600, 0) == 0xDEEFC502);
  CHECK(MtSeed(1, 0x83420020, 0) == 0xC635FC19);
  CHECK(MtSeed(1, 0x8357B5C0, 0) == 0x67431C31);
  CHECK(MtSeed(1, 0x833F7600, 1) == 0x31D0360E);
  CHECK(MtSeed(1, 0x833F7600, 2) == 0x79DC99F5);
  CHECK(MtSeed(1, 0x8321B198, 1) == 0x7EE65792);
  CHECK(MtSeed(2, 0x833F7600, 0) == 0xE175B2BB);  // another session seed, another draw
  CHECK(MtSeed(2, 0x833F7600, 1) == 0x62138727);

  SeedSeries series;
  CHECK(series.Next(1, 0x833F7600) == 0xDEEFC502);  // the first seeding is the proof's
  CHECK(series.Next(1, 0x833F7600) == 0x31D0360E);  // the second draws anew (NOT the first again)
  // Another MT counts on its own: its first seeding is still k = 0, although the first MT is at k = 2
  // (NOT one count for all MTs, which would give 0xD9309BFC here).
  CHECK(series.Next(1, 0x8321B198) == 0x17CEE0B2);
  CHECK(series.Next(1, 0x833F7600) == 0x79DC99F5);
  CHECK(series.Next(1, 0x8321B198) == 0x7EE65792);
  series.Reset();
  CHECK(series.Next(1, 0x833F7600) == 0xDEEFC502);  // a new session counts from the start again

  // Several threads seeding one MT: every k from 0 to n - 1 is handed out exactly once (the hook runs on
  // whatever guest thread seeds). A GUARD, not proof: a missing lock loses an update only when the
  // threads overlap, which this cannot force. The probe that removes the lock was caught in 10 of 10
  // runs, the clean build passed 30 of 30.
  SeedSeries shared;
  constexpr int kThreads = 8, kEach = 1000;
  std::vector<std::vector<uint32_t>> got(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&shared, &got, t] {
      for (int i = 0; i < kEach; ++i) got[t].push_back(shared.Next(1, 0x833F7600));
    });
  }
  for (std::thread& t : threads) t.join();
  std::vector<uint32_t> all, want;
  for (const auto& v : got) all.insert(all.end(), v.begin(), v.end());
  for (uint32_t k = 0; k < uint32_t(kThreads * kEach); ++k) want.push_back(MtSeed(1, 0x833F7600, k));
  std::sort(all.begin(), all.end());
  std::sort(want.begin(), want.end());
  CHECK(all.size() == size_t(kThreads * kEach));
  CHECK(all == want);
}

int main() {
  TestPadBytes();
  TestScript();
  TestRandom();
  TestQueue();
  TestRecording();
  TestMetaMismatch();
  TestMaskedCopy();
  TestBlockMasks();
  TestBlockRegions();
  TestSweep();
  TestCounters();
  TestVirtualClocks();
  TestAudioClocks();
  TestMtSeeds();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
