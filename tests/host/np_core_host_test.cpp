// Host test for src/net/np_core.cpp (pad scripts, input queue, recordings,
// masked copies, sweep batches). Built and run by run_np_core_test.bat.

#include "../../src/net/np_core.h"

#include <cstdio>
#include <cstring>
#include <string>

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

int main() {
  TestPadBytes();
  TestScript();
  TestRandom();
  TestQueue();
  TestRecording();
  TestMaskedCopy();
  TestSweep();
  TestCounters();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
