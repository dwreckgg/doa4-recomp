// Host test for src/net/np_exec (the event-list executor, the 16-byte player input, the checksum fold) and
// src/net/np_gekko (the wrapper around a real GekkoNet session). No SDK, no socket, no file: two gekko::Session
// objects in one process are joined by in-memory queues (Send / Receive). Built and run by run_np_exec_test.bat.
// "np_exec_host_test.exe -v" prints each group before it runs. The real-GekkoNet groups sleep 1 ms per tick until
// both sessions have started (the library's hand-shake is paced by the steady clock, backend.cpp:375).
//
// The link between the two sessions (contract, one sentence): a perfect link between exactly two endpoints; each
// payload is delivered once, whole and in order, to the other side's first Receive call at or after its due tick
// (due = send tick + the link's delay in ticks). A real link loses, reorders and duplicates; that is the
// transport's tests (Task 7), not this file's. Desync latencies printed here are on this link: they are not
// network latencies.
//
// What the real-GekkoNet groups pin about the library (as vendored in third_party/GekkoNet):
//  - the checksum of frame f is read from the Save event of frame f at the START of the update that advances f+1
//    (game_session.cpp:203 and 365-385), so the executor has to be told the checksum before that update: the
//    order control runs the harness without that call and shows the desync is then reported two frames late;
//  - the first update of a started session also carries a Save of frame -1 (game_session.cpp:11, 619-623), which
//    the executor accepts and ignores.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../src/net/np_exec.h"
#include "../../src/net/np_gekko.h"

using namespace doa4::np;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;

#define CHECK(cond)                                               \
  do {                                                            \
    ++g_checks;                                                   \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

// A check that prints the observed values when it fails (the arguments after the condition are a printf).
#define CHECKF(cond, ...)                                                  \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);        \
      std::printf(__VA_ARGS__);                                            \
      std::printf("\n");                                                   \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

static void Group(const char* name) {
  if (g_verbose) {
    std::printf("group: %s\n", name);
    std::fflush(stdout);
  }
}

using Bytes = std::vector<uint8_t>;

// The pad of np_core_host_test's TestPadBytes (np_core_host_test.cpp:26-37).
static Pad TestPad() {
  Pad pad;
  pad.buttons = 0x1234;
  pad.lt = 0xAB;
  pad.rt = 0x01;
  pad.lx = -2;
  pad.ly = 0x7F00;
  pad.rx = -32768;
  pad.ry = 1;
  return pad;
}

static void TestPlayerInput() {
  Group("player input");
  uint8_t out[exec::kPlayerInputBytes];
  std::memset(out, 0x5A, sizeof out);  // so that the four reserved bytes are shown to be WRITTEN as zero
  exec::EncodePlayerInput(TestPad(), out);
  // The 12 guest bytes of TestPadBytes' pad, then four zeros.
  const uint8_t want[16] = {0x12, 0x34, 0xAB, 0x01, 0xFF, 0xFE, 0x7F, 0x00, 0x80, 0x00, 0x00, 0x01, 0, 0, 0, 0};
  CHECK(exec::kPlayerInputBytes == 16);
  CHECK(std::memcmp(out, want, 16) == 0);
  Pad back;
  CHECK(exec::DecodePlayerInput(out, &back));
  CHECK(back == TestPad());
  for (int i = 12; i < 16; ++i) {  // each reserved byte on its own
    uint8_t bad[16];
    std::memcpy(bad, out, 16);
    bad[i] = 1;
    Pad p;
    CHECKF(!exec::DecodePlayerInput(bad, &p), "reserved byte %d = 1 was accepted", i);
  }
  // The reserved bytes are not part of the pad: a high bit in each is refused too (not just the value 1).
  uint8_t high[16];
  std::memcpy(high, out, 16);
  high[15] = 0x80;
  Pad p;
  CHECK(!exec::DecodePlayerInput(high, &p));
}

// ---- fake event lists ----
static Pad PadN(int n) {
  Pad p;
  p.buttons = uint16_t(0x1000 + n);
  p.lt = uint8_t(n);
  p.rt = uint8_t(100 + n);
  p.lx = int16_t(-1000 * n - 1);
  p.ly = int16_t(1000 * n + 2);
  p.rx = int16_t(n);
  p.ry = int16_t(-n);
  return p;
}

static Bytes TwoPads(const Pad& a, const Pad& b) {
  Bytes v(32, 0);
  exec::EncodePlayerInput(a, v.data());
  exec::EncodePlayerInput(b, v.data() + 16);
  return v;
}

static exec::Event Advance(int frame, Bytes inputs) {
  exec::Event e;
  e.kind = exec::Event::kAdvance;
  e.frame = frame;
  e.inputs = std::move(inputs);
  return e;
}

struct SaveSlots {
  uint32_t checksum = 0xDEADBEEF;
  uint32_t state_len = 0xDEADBEEF;
  uint8_t state[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
};

static exec::Event Save(int frame, SaveSlots* s) {
  exec::Event e;
  e.kind = exec::Event::kSave;
  e.frame = frame;
  e.checksum = &s->checksum;
  e.state_len = &s->state_len;
  e.state = s->state;
  return e;
}

static exec::Event Load(int frame) {
  exec::Event e;
  e.kind = exec::Event::kLoad;
  e.frame = frame;
  return e;
}

static bool Untouched(const SaveSlots& s) {
  return s.checksum == 0xDEADBEEF && s.state_len == 0xDEADBEEF && s.state[0] == 0xEE && s.state[7] == 0xEE;
}

static void TestExecutorFake() {
  Group("executor, fake event lists");
  std::vector<std::pair<int32_t, FrameInput>> tapped;
  exec::Executor ex(2, [&](int32_t f, const FrameInput& in) { tapped.emplace_back(f, in); });
  SaveSlots s0, s1;
  CHECK(ex.next_frame() == 0 && ex.saves_filled() == 0);

  const std::vector<exec::Event> call1 = {Advance(0, TwoPads(PadN(1), PadN(2))), Save(0, &s0)};
  exec::Step st1 = ex.Run(call1, 0x11);
  CHECK(st1.advanced && st1.frame == 0 && st1.end == exec::End::kNone && st1.why.empty());
  FrameInput want0;
  want0.connected = 0x03;
  want0.pad[0] = PadN(1);
  want0.pad[1] = PadN(2);  // slots 2-3 stay default (zero) pads
  CHECK(st1.input == want0);
  CHECK(st1.input.pad[2] == Pad{} && st1.input.pad[3] == Pad{});
  CHECK(Untouched(s0));  // the Save of frame 0 waits for the NEXT call; 0x11 (the state before frame 0) is not its value
  CHECK(ex.next_frame() == 1 && ex.saves_filled() == 0);

  const std::vector<exec::Event> call2 = {Advance(1, TwoPads(PadN(3), PadN(4))), Save(1, &s1)};
  exec::Step st2 = ex.Run(call2, 0x22);
  CHECK(st2.advanced && st2.frame == 1 && st2.end == exec::End::kNone);
  FrameInput want1;
  want1.connected = 0x03;
  want1.pad[0] = PadN(3);
  want1.pad[1] = PadN(4);
  CHECK(st2.input == want1);
  CHECKF(s0.checksum == 0x22, "Save 0's checksum slot is %08X (expected 22)", s0.checksum);
  CHECK(s0.state_len == 8);
  const uint8_t tok0[8] = {0, 0, 0, 0, 0x22, 0, 0, 0};
  CHECK(std::memcmp(s0.state, tok0, 8) == 0);
  CHECK(Untouched(s1));  // frame 1's own Save is still pending: the new Save must not take 0x22
  CHECK(ex.saves_filled() == 1 && ex.next_frame() == 2);

  // An empty list advances nothing and still writes the pending Save.
  exec::Step st3 = ex.Run({}, 0x33);
  CHECK(!st3.advanced && st3.end == exec::End::kNone && st3.frame == -1);
  CHECKF(s1.checksum == 0x33 && s1.state_len == 8, "Save 1's slots: checksum %08X state_len %u", s1.checksum, s1.state_len);
  const uint8_t tok1[8] = {1, 0, 0, 0, 0x33, 0, 0, 0};
  CHECK(std::memcmp(s1.state, tok1, 8) == 0);
  CHECK(ex.saves_filled() == 2 && ex.next_frame() == 2);
  // ... and only once: the next empty list leaves the slots alone.
  ex.Run({}, 0x44);
  CHECK(s1.checksum == 0x33 && ex.saves_filled() == 2);

  CHECK(tapped.size() == 2);
  if (tapped.size() == 2) {
    CHECK(tapped[0].first == 0 && tapped[0].second == want0);
    CHECK(tapped[1].first == 1 && tapped[1].second == want1);
  }
}

// 300 frames in the order the game drives them (Run({}) with the checksum entering, then the events), so that the
// frame number passes 255 and every byte of the checksum differs: the token is little-endian in both fields.
static uint32_t CsOf(int f) { return 0xA1B2C3D4u ^ (uint32_t(f + 1) * 0x01000193u); }

static void TestExecutorTokens() {
  Group("executor, save tokens over 300 frames");
  constexpr int kN = 300;
  std::vector<SaveSlots> slots(kN);
  int taps = 0;
  exec::Executor ex(2, [&](int32_t f, const FrameInput&) { taps += (f == taps) ? 1 : 1000; });  // frames in order
  for (int f = 0; f < kN; ++f) {
    ex.Run({}, CsOf(f - 1));  // fills Save f-1 with the checksum of the state after frame f-1
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(f, TwoPads(PadN(1), PadN(2))), Save(f, &slots[size_t(f)])}, CsOf(f - 1));
    CHECK(st.advanced && st.frame == f && st.end == exec::End::kNone);
  }
  ex.Run({}, CsOf(kN - 1));
  CHECKF(ex.saves_filled() == uint32_t(kN), "%u saves filled for %d frames", ex.saves_filled(), kN);
  CHECKF(taps == kN, "tap bookkeeping %d (expected %d: every frame once, in order)", taps, kN);
  int bad_cs = 0, bad_len = 0, bad_tok = 0;
  int first_bad = -1;
  for (int f = 0; f < kN; ++f) {
    const SaveSlots& s = slots[size_t(f)];
    uint8_t want[8];
    for (int i = 0; i < 4; ++i) {
      want[i] = uint8_t(uint32_t(f) >> (8 * i));
      want[4 + i] = uint8_t(CsOf(f) >> (8 * i));
    }
    if (s.checksum != CsOf(f)) ++bad_cs;
    if (s.state_len != 8) ++bad_len;
    if (std::memcmp(s.state, want, 8) != 0) {
      if (!bad_tok) first_bad = f;
      ++bad_tok;
    }
  }
  CHECKF(bad_cs == 0, "%d of %d checksum slots hold the wrong value", bad_cs, kN);
  CHECKF(bad_len == 0, "%d of %d state_len slots are not 8", bad_len, kN);
  CHECKF(bad_tok == 0, "%d of %d tokens are not {frame LE, checksum LE}; first at frame %d", bad_tok, kN, first_bad);
  // The two frames the byte order of the frame field is read from: 258 = 0x0102.
  const uint8_t t258[8] = {0x02, 0x01, 0, 0, uint8_t(CsOf(258)), uint8_t(CsOf(258) >> 8), uint8_t(CsOf(258) >> 16), uint8_t(CsOf(258) >> 24)};
  CHECK(std::memcmp(slots[258].state, t258, 8) == 0);
  CHECK(CsOf(258) != CsOf(2) && uint8_t(CsOf(258)) != uint8_t(CsOf(258) >> 8));  // fixture: the checksum bytes differ
}

static void TestExecutorFrameMinusOne() {
  Group("executor, the start-up Save of frame -1");
  SaveSlots sm, s0;
  int taps = 0;
  exec::Executor ex(2, [&](int32_t, const FrameInput&) { ++taps; });
  // [Save -1] alone: accepted and ignored.
  exec::Step st = ex.Run(std::vector<exec::Event>{Save(-1, &sm)}, 0x77);
  CHECK(!st.advanced && st.end == exec::End::kNone);
  CHECK(ex.next_frame() == 0 && ex.saves_filled() == 0 && taps == 0);
  exec::Step e2 = ex.Run({}, 0x78);
  CHECK(e2.end == exec::End::kNone && Untouched(sm) && ex.saves_filled() == 0);  // never filled, never counted
  // The first Advance after it is frame 0 and is accepted.
  exec::Step a0 = ex.Run(std::vector<exec::Event>{Advance(0, TwoPads(PadN(1), PadN(2))), Save(0, &s0)}, 0x79);
  CHECK(a0.end == exec::End::kNone && a0.advanced && a0.frame == 0 && taps == 1);
  // [Save -1, Advance 0, Save 0] in one call, on a fresh executor.
  SaveSlots sm2, s02;
  exec::Executor ex2(2, nullptr);
  const std::vector<exec::Event> first = {Save(-1, &sm2), Advance(0, TwoPads(PadN(1), PadN(2))), Save(0, &s02)};
  exec::Step a = ex2.Run(first, 0x01);
  CHECK(a.advanced && a.frame == 0 && a.end == exec::End::kNone);
  CHECK(Untouched(sm2));
  ex2.Run({}, 0x99);
  CHECK(s02.checksum == 0x99 && ex2.saves_filled() == 1 && Untouched(sm2));
  // A second Save of frame -1 is not the start-up save.
  exec::Executor ex5(2, nullptr);
  CHECK(ex5.Run(std::vector<exec::Event>{Save(-1, &sm)}, 0).end == exec::End::kNone);
  CHECK(ex5.Run(std::vector<exec::Event>{Save(-1, &sm)}, 0).end == exec::End::kInternal);
  // Not the first event of the list: refused.
  exec::Executor ex3(2, nullptr);
  const std::vector<exec::Event> late = {Advance(0, TwoPads(PadN(1), PadN(2))), Save(-1, &sm)};
  CHECK(ex3.Run(late, 0).end == exec::End::kInternal);
  // After an Advance has run: refused.
  exec::Executor ex4(2, nullptr);
  ex4.Run(std::vector<exec::Event>{Advance(0, TwoPads(PadN(1), PadN(2)))}, 0);
  CHECK(ex4.Run(std::vector<exec::Event>{Save(-1, &sm)}, 0).end == exec::End::kInternal);
  // Any other frame number as the first event is not the start-up save.
  exec::Executor ex6(2, nullptr);
  CHECK(ex6.Run(std::vector<exec::Event>{Save(-2, &sm)}, 0).end == exec::End::kInternal);
  exec::Executor ex7(2, nullptr);
  CHECK(ex7.Run(std::vector<exec::Event>{Save(0, &sm)}, 0).end == exec::End::kInternal);
  CHECK(Untouched(sm));
}

static void TestExecutorRefusals() {
  Group("executor, internal ends");
  SaveSlots s;
  {
    exec::Executor ex(2, nullptr);
    exec::Step st = ex.Run(std::vector<exec::Event>{Load(0)}, 0);
    CHECK(st.end == exec::End::kInternal && st.why == "load in lockstep" && !st.advanced);
    // The end is final: a well-formed list afterwards still reports it and stages nothing.
    exec::Step again = ex.Run(std::vector<exec::Event>{Advance(0, TwoPads(PadN(1), PadN(2)))}, 0);
    CHECK(again.end == exec::End::kInternal && !again.advanced);
  }
  {
    int taps = 0;
    exec::Executor ex(2, [&](int32_t, const FrameInput&) { ++taps; });
    const std::vector<exec::Event> two = {Advance(0, TwoPads(PadN(1), PadN(2))), Advance(1, TwoPads(PadN(1), PadN(2)))};
    exec::Step st = ex.Run(two, 0);
    CHECK(st.end == exec::End::kInternal && !st.advanced && !st.why.empty());
    CHECKF(taps == 0, "a refused list tapped %d frames (nothing of a refused list is staged)", taps);
  }
  {
    exec::Executor ex(2, nullptr);
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(2, TwoPads(PadN(1), PadN(2)))}, 0);
    CHECK(st.end == exec::End::kInternal);
    CHECK(ex.next_frame() == 0);
  }
  {
    exec::Executor ex(2, nullptr);  // a repeated frame
    CHECK(ex.Run(std::vector<exec::Event>{Advance(0, TwoPads(PadN(1), PadN(2)))}, 0).end == exec::End::kNone);
    CHECK(ex.Run(std::vector<exec::Event>{Advance(0, TwoPads(PadN(1), PadN(2)))}, 0).end == exec::End::kInternal);
  }
  {
    exec::Executor ex(2, nullptr);  // [Advance 4, Save 5] with 4 the next frame
    for (int f = 0; f < 4; ++f) ex.Run(std::vector<exec::Event>{Advance(f, TwoPads(PadN(1), PadN(2)))}, 0);
    CHECK(ex.next_frame() == 4);
    const std::vector<exec::Event> bad = {Advance(4, TwoPads(PadN(1), PadN(2))), Save(5, &s)};
    CHECK(ex.Run(bad, 0).end == exec::End::kInternal);
    ex.Run({}, 0x55);  // an ended executor writes no slot
    CHECK(Untouched(s));
  }
  {
    exec::Executor ex(2, nullptr);  // [Save 5] in the call after Advance 4 (no Advance in its own call)
    for (int f = 0; f < 5; ++f) ex.Run(std::vector<exec::Event>{Advance(f, TwoPads(PadN(1), PadN(2)))}, 0);
    CHECK(ex.next_frame() == 5);
    CHECK(ex.Run(std::vector<exec::Event>{Save(5, &s)}, 0).end == exec::End::kInternal);
    CHECK(Untouched(s));
  }
  {
    exec::Executor ex(2, nullptr);  // a Save with no Advance in the call, the first frame
    CHECK(ex.Run(std::vector<exec::Event>{Save(0, &s)}, 0).end == exec::End::kInternal);
  }
  {
    exec::Executor ex(2, nullptr);  // the Save before its Advance
    const std::vector<exec::Event> rev = {Save(0, &s), Advance(0, TwoPads(PadN(1), PadN(2)))};
    CHECK(ex.Run(rev, 0).end == exec::End::kInternal);
  }
  {
    exec::Executor ex(2, nullptr);  // two Saves for one Advance: the first slot would never be filled
    const std::vector<exec::Event> twice = {Advance(0, TwoPads(PadN(1), PadN(2))), Save(0, &s), Save(0, &s)};
    CHECK(ex.Run(twice, 0).end == exec::End::kInternal);
  }
  {
    exec::Executor ex(2, nullptr);  // a wrong input length, short and long
    CHECK(ex.Run(std::vector<exec::Event>{Advance(0, Bytes(31, 0))}, 0).end == exec::End::kInternal);
    exec::Executor ex2(2, nullptr);
    CHECK(ex2.Run(std::vector<exec::Event>{Advance(0, Bytes(33, 0))}, 0).end == exec::End::kInternal);
  }
  {
    int taps = 0;  // a reserved input byte that is not zero: neither staged nor tapped
    exec::Executor ex(2, [&](int32_t, const FrameInput&) { ++taps; });
    Bytes in = TwoPads(PadN(1), PadN(2));
    in[16 + 13] = 7;
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(0, in)}, 0);
    CHECK(st.end == exec::End::kInternal && !st.advanced && taps == 0);
  }
}

static void TestExecutorPlayers() {
  Group("executor, connected mask and slot order");
  {
    // The connected mask comes from the session's player count, not from the wire: both slots send a zero pad
    // (all 16 bytes zero, as GekkoNet delivers for the frames before the first input arrives) and both are connected.
    exec::Executor ex(2, nullptr);
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(0, Bytes(32, 0))}, 0);
    CHECKF(st.advanced && st.input.connected == 0x03, "connected mask %02X for two zero pads (expected 03)", st.input.connected);
  }
  {
    // One slot silent, the other not: still both connected.
    Bytes in(32, 0);
    exec::EncodePlayerInput(PadN(2), in.data() + 16);
    exec::Executor ex(2, nullptr);
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(0, in)}, 0);
    CHECK(st.advanced && st.input.connected == 0x03 && st.input.pad[0] == Pad{} && st.input.pad[1] == PadN(2));
  }
  for (int n : {1, 3, 4}) {
    Bytes in(size_t(n) * 16, 0);
    for (int p = 0; p < n; ++p) exec::EncodePlayerInput(PadN(p + 1), in.data() + size_t(p) * 16);
    exec::Executor ex(n, nullptr);
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(0, in)}, 0);
    CHECKF(st.advanced && st.input.connected == uint8_t((1u << n) - 1), "%d players: connected mask %02X", n, st.input.connected);
    bool order = true;
    for (int p = 0; p < kUsers; ++p) order = order && (st.input.pad[size_t(p)] == (p < n ? PadN(p + 1) : Pad{}));
    CHECKF(order, "%d players: the pads are not in slot order", n);
  }
}

static void TestFoldChecksum() {
  Group("checksum fold");
  // Every expected value below was produced by an independent Python FNV-1a run (the brief's one-liner):
  //   py -3 -I -c "import struct,functools; h=functools.reduce(lambda h,b: ((h^b)*0x100000001b3)&0xFFFFFFFFFFFFFFFF,
  //     struct.pack('<iQQQ',5,1,2,3), 0xcbf29ce484222325); print(hex((h^(h>>32))&0xFFFFFFFF))"   -> 0xd754e478
  // (struct.pack format '<i' + '<Q' per value; rows are concatenated, the frame is the first 4 bytes).
  const std::vector<uint64_t> a = {1, 2}, b = {2, 1}, sw = {3};
  CHECK(exec::FoldChecksum(5, a, sw) == 0xD754E478u);
  CHECK(exec::FoldChecksum(5, b, sw) == 0x6180228Bu);  // a sum or XOR fold would give the two the same value
  CHECK(exec::FoldChecksum(5, a, std::vector<uint64_t>{}) == 0x85B0AACFu);  // the sweep row counts
  CHECK(exec::FoldChecksum(5, a, std::vector<uint64_t>{4}) == 0xC1844F90u);  // one changed value in the sweep row
  CHECK(exec::FoldChecksum(6, a, sw) == 0x1E02732Eu);  // the frame counts
  CHECK(exec::FoldChecksum(-1, std::vector<uint64_t>{}, std::vector<uint64_t>{}) == 0xA7654F34u);  // frame as 4 LE bytes
  CHECK(exec::FoldChecksum(0, std::vector<uint64_t>{}, std::vector<uint64_t>{}) == 0xD0EB658Au);
  // One changed byte (the top byte of an 8-byte value) in a row of one value, each row.
  const uint64_t v = 0x0102030405060708ull, w = 0xFFFFFFFFFFFFFFFFull;
  const uint32_t base = exec::FoldChecksum(5, std::vector<uint64_t>{v}, std::vector<uint64_t>{w});
  CHECK(base == 0xB9000886u);
  CHECK(exec::FoldChecksum(5, std::vector<uint64_t>{v ^ (0xFFull << 56)}, std::vector<uint64_t>{w}) != base);
  CHECK(exec::FoldChecksum(5, std::vector<uint64_t>{v}, std::vector<uint64_t>{w ^ (0xFFull << 56)}) != base);
  CHECK(exec::FoldChecksum(5, std::vector<uint64_t>{v}, std::vector<uint64_t>{w ^ 1}) != base);
  CHECK(exec::FoldChecksum(5, std::vector<uint64_t>{v ^ 1}, std::vector<uint64_t>{w}) != base);
}

// ---- the wrapper without a peer ----
static void TestWrapperLifecycle() {
  Group("wrapper, lifecycle");
  gekko::Session s;
  // Before Create: nothing happens and nothing crashes.
  CHECK(s.Update().empty());
  CHECK(s.TakeSessionEvents().empty());
  s.AddLocalInput(Pad{});
  CHECK(s.local_inputs_added() == 0);  // no session: not a call GekkoNet saw
  const gekko::NetStats z = s.Stats();
  CHECK(z.ping_ms == 0 && z.jitter_ms == 0 && z.kb_sent == 0 && z.kb_received == 0);
  CHECK(s.FramesAhead() == 0);
  s.Destroy();  // safe on a session that was never created

  gekko::Config bad;
  bad.local_slot = 2;  // no such slot in a two-player session
  CHECK(!s.Create(bad, nullptr, nullptr));
  CHECK(s.Update().empty());

  int sent = 0;
  gekko::Config ok;
  ok.local_slot = 1;
  CHECK(s.Create(ok, [&](std::span<const uint8_t> p) { sent += p.empty() ? 1000 : 1; }, [](Bytes*) { return false; }));
  s.AddLocalInput(TestPad());
  CHECK(s.local_inputs_added() == 1);
  for (int i = 0; i < 100 && sent == 0; ++i) {  // the hand-shake is paced by the clock (backend.cpp:375)
    s.Update();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECKF(sent > 0 && sent < 1000, "the hand-shake sent %d payloads through Send (no empty ones)", sent);
  s.Destroy();
  CHECK(s.Update().empty());
  s.Destroy();  // twice
}

// ---- real GekkoNet, two sessions in one process ----
struct Packet {
  int due;
  Bytes bytes;
};
struct Link {
  std::deque<Packet> q[2];  // q[s]: what waits for side s
  int sent[2] = {0, 0};
  int delay = 0;  // ticks each way
  int now = 0;
};

constexpr int kFrames = 300;
constexpr int kMaxTicks = 4000;
constexpr int kBadFrame = 150;
constexpr uint8_t kDelay = 2;
// "No perturbed frame": must not be a value last_frame can hold, -1 included (a probe of the executor that filled
// Save 0 in the same call as Advance 0 showed side A perturbing frame -1 when this was -1).
constexpr int kNoFrame = -1000;

struct Side {
  gekko::Session session;
  exec::Executor ex;
  uint64_t state = 0xCBF29CE484222325ull;
  int32_t last_frame = -1;  // the last frame advanced
  bool need_frame_start = true;
  bool started = false;
  int advances = 0;
  int updates = 0;
  int empty_updates_after_first = 0;  // updates that yielded no event after the first Advance (stalls)
  int bad_frame = kNoFrame;           // the frame whose checksummed state this side perturbs
  int write_tick = -1;                // the tick at which the perturbed checksum was handed to the Save slot
  uint32_t cs_bad = 0;                // the checksum this side handed over for frame kBadFrame (0 until it did)
  bool step_ended = false;
  std::vector<std::pair<int32_t, FrameInput>> taps;    // what the executor's ConfirmedTap saw
  std::vector<std::pair<int32_t, FrameInput>> staged;  // what Run returned for each Advance
  std::vector<gekko::SessionEvent> desyncs;
  std::vector<int> desync_tick;
  Side() : ex(2, [this](int32_t f, const FrameInput& in) { taps.emplace_back(f, in); }) {}
};

static uint64_t Mix(uint64_t h, uint8_t b) { return (h ^ b) * 0x100000001b3ull; }

static Pad LocalPad(int side, int index) {
  Pad p;
  p.buttons = uint16_t(0x1000 + 16 * side + (index & 0xF));
  p.lt = uint8_t(index);
  p.rt = uint8_t(side + 1);
  p.lx = int16_t(index * 3 + side);
  p.ly = int16_t(-index * 5 - side);
  p.rx = int16_t(index);
  p.ry = int16_t(side);
  return p;
}

struct PairResult {
  Side side[2];
  Link link;
  int ticks = 0;
  std::chrono::steady_clock::time_point stats_base{};  // when both sessions had started (Stats() read once there)
};

// Task 11's order, per frame: Run({}, checksum_entering) BEFORE the frame's first Update, AddLocalInput once, then
// Update + Run(events, checksum_entering) until an Advance (one Update per tick here, since two sides share a thread).
// `pre_run` false is the order control: the Run({}) is left out, so Save(f-1) is filled by the Run that follows the
// update for f, one update late.
static void StepSide(PairResult& r, int s, bool pre_run) {
  Side& S = r.side[s];
  auto entering = [&]() {
    uint32_t ce = exec::FoldChecksum(S.last_frame, std::vector<uint64_t>{S.state}, std::vector<uint64_t>{uint64_t(S.last_frame) * 3});
    if (S.last_frame == S.bad_frame) ce ^= 1u;  // the state is perturbed for the checksum only: one wrong frame
    return ce;
  };
  // Runs the executor and notes the tick at which the perturbed checksum went into a Save slot.
  auto run = [&](std::span<const exec::Event> events) {
    const uint32_t before = S.ex.saves_filled();
    const bool perturbed = S.last_frame == S.bad_frame;
    const uint32_t ce = entering();
    exec::Step st = S.ex.Run(events, ce);
    if (S.last_frame == kBadFrame && S.ex.saves_filled() > before && S.cs_bad == 0) S.cs_bad = ce;  // Save 150's fill
    if (perturbed && S.ex.saves_filled() > before && S.write_tick < 0) S.write_tick = r.ticks;
    return st;
  };
  if (S.need_frame_start) {
    if (pre_run) run({});
    S.session.AddLocalInput(LocalPad(s, S.advances));
    S.need_frame_start = false;
  }
  std::vector<exec::Event> ev = S.session.Update();
  ++S.updates;
  if (ev.empty() && S.advances > 0) ++S.empty_updates_after_first;
  exec::Step st = run(ev);
  if (st.end != exec::End::kNone) S.step_ended = true;
  for (const gekko::SessionEvent& e : S.session.TakeSessionEvents()) {
    if (e.kind == gekko::SessionEvent::kStarted) S.started = true;
    if (e.kind == gekko::SessionEvent::kDesync) {
      S.desyncs.push_back(e);
      S.desync_tick.push_back(r.ticks);
    }
  }
  if (st.advanced) {
    S.staged.emplace_back(st.frame, st.input);
    // The fake state advances from the staged inputs, identically on both sides.
    uint64_t h = S.state;
    h = Mix(h, st.input.connected);
    for (int p = 0; p < 2; ++p) {
      uint8_t g[12];
      PadToGuest(st.input.pad[size_t(p)], g);
      for (uint8_t b : g) h = Mix(h, b);
    }
    S.state = h;
    S.last_frame = st.frame;
    ++S.advances;
    S.need_frame_start = true;
  }
}

static void RunPair(PairResult& r, int bad_frame_side_b, bool pre_run, int delay_ticks, int tick_sleep_ms = 0) {
  r.link.delay = delay_ticks;
  for (int s = 0; s < 2; ++s) {
    gekko::Config cfg;
    cfg.num_players = 2;
    cfg.local_slot = s;
    cfg.delay = kDelay;
    bool ok = r.side[s].session.Create(
        cfg,
        [&r, s](std::span<const uint8_t> payload) {
          r.link.q[1 - s].push_back({r.link.now + r.link.delay, Bytes(payload.begin(), payload.end())});
          ++r.link.sent[s];
        },
        [&r, s](Bytes* out) {
          auto& q = r.link.q[s];
          if (q.empty() || q.front().due > r.link.now) return false;
          *out = std::move(q.front().bytes);
          q.pop_front();
          return true;
        });
    CHECK(ok);
  }
  r.side[1].bad_frame = bad_frame_side_b < 0 ? kNoFrame : bad_frame_side_b;
  for (r.ticks = 0; r.ticks < kMaxTicks; ++r.ticks) {
    r.link.now = r.ticks;
    StepSide(r, 0, pre_run);
    StepSide(r, 1, pre_run);
    const bool both_started = r.side[0].started && r.side[1].started;
    if (both_started && r.stats_base == std::chrono::steady_clock::time_point{}) {
      r.stats_base = std::chrono::steady_clock::now();
      r.side[0].session.Stats();  // GekkoNet's bandwidth rate has its baseline at the first read (net.cpp:92-100)
      r.side[1].session.Stats();
    }
    if (both_started && r.side[0].advances >= kFrames && r.side[1].advances >= kFrames) {
      ++r.ticks;
      break;
    }
    if (!both_started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    else if (tick_sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(tick_sleep_ms));
  }
}

static void CommonChecks(const PairResult& r, const char* label) {
  for (int s = 0; s < 2; ++s) {
    const Side& S = r.side[s];
    std::printf("info: %s side %d: %d advances in %d ticks, %u saves filled, %u local inputs added, %d stalled updates\n", label, s,
                S.advances, r.ticks, S.ex.saves_filled(), S.session.local_inputs_added(), S.empty_updates_after_first);
    CHECKF(S.started, "%s side %d: SessionStarted never raised", label, s);
    CHECKF(!S.step_ended, "%s side %d: the executor ended the session", label, s);
    CHECKF(S.advances >= kFrames, "%s side %d: %d advances in %d ticks", label, s, S.advances, r.ticks);
    CHECKF(S.ex.saves_filled() >= uint32_t(kFrames - 1), "%s side %d: %u saves filled (expected >= %d)", label, s, S.ex.saves_filled(),
           kFrames - 1);
    // Once per frame: the add of frame f is made before frame f's first Update, so after N Advances exactly N adds
    // were made (the add for frame N is made at the start of the next tick); never two for a frame, however many
    // updates a frame needed.
    CHECKF(S.session.local_inputs_added() == uint32_t(S.advances), "%s side %d: %u adds for %d advances", label, s,
           S.session.local_inputs_added(), S.advances);
    // The tap saw exactly what Run staged, once per Advance.
    CHECKF(S.taps.size() == size_t(S.advances) && S.taps == S.staged, "%s side %d: %zu taps, %zu staged records, %d advances", label,
           s, S.taps.size(), S.staged.size(), S.advances);
  }
  // Equal taps on both sides, frames 0, 1, 2, ... in order.
  const size_t common = std::min(r.side[0].taps.size(), r.side[1].taps.size());
  CHECKF(common >= size_t(kFrames), "%s: %zu common frames", label, common);
  size_t unequal = 0, misnumbered = 0, wrong = 0;
  for (size_t f = 0; f < common; ++f) {
    const auto& a = r.side[0].taps[f];
    const auto& b = r.side[1].taps[f];
    if (a.first != int32_t(f) || b.first != int32_t(f)) ++misnumbered;
    if (!(a.second == b.second)) ++unequal;
    // The record the executor staged: both users connected, and frame f >= delay carries the pad each side added
    // with advance count f - delay (GekkoNet files it at frame + delay); frames 0 and 1 carry empty input.
    FrameInput want;
    want.connected = 0x03;
    if (f >= kDelay) {
      want.pad[0] = LocalPad(0, int(f) - kDelay);
      want.pad[1] = LocalPad(1, int(f) - kDelay);
    }
    if (!(a.second == want)) ++wrong;
  }
  CHECKF(misnumbered == 0, "%s: %zu frames out of order", label, misnumbered);
  CHECKF(unequal == 0, "%s: %zu frames differ between the sides", label, unequal);
  CHECKF(wrong == 0, "%s: %zu frames differ from the pattern", label, wrong);
  CHECK(r.link.sent[0] > 0 && r.link.sent[1] > 0);
}

static void TestRealClean() {
  Group("real GekkoNet, clean 300 frames");
  PairResult r;
  RunPair(r, -1, true, 0);
  CommonChecks(r, "clean");
  CHECK(r.side[0].desyncs.empty() && r.side[1].desyncs.empty());
  // The wrapper's reads work on a live session.
  const gekko::NetStats st = r.side[0].session.Stats();
  const float ahead = r.side[0].session.FramesAhead();
  std::printf("info: clean: stats ping %.2f jitter %.2f kb sent %.2f received %.2f, frames ahead %.2f\n", st.ping_ms, st.jitter_ms,
              st.kb_sent, st.kb_received, ahead);
  CHECK(st.ping_ms >= 0 && st.jitter_ms >= 0 && st.kb_sent >= 0 && st.kb_received >= 0);  // sane; the rates are read in TestRealDelayed
}

static void ExpectOneDesync(const PairResult& r, int side, int frame, const char* label) {
  const Side& S = r.side[side];
  CHECKF(S.desyncs.size() == 1, "%s side %d: %zu DesyncDetected events (expected exactly 1)", label, side, S.desyncs.size());
  if (S.desyncs.size() != 1) return;
  CHECKF(S.desyncs[0].frame == frame, "%s side %d: DesyncDetected for frame %d (expected %d)", label, side, S.desyncs[0].frame, frame);
}

static void TestRealDesync() {
  Group("real GekkoNet, side B's state perturbed at frame 150");
  PairResult r;
  RunPair(r, kBadFrame, true, 0);
  CommonChecks(r, "desync");
  // The perturbed checksum of frame 150 is the one handed over by the Run({}) before frame 151's first update.
  CHECKF(r.side[1].write_tick >= 0, "side B never handed over the perturbed checksum");
  for (int s = 0; s < 2; ++s) ExpectOneDesync(r, s, kBadFrame, "desync");
  if (r.side[0].desyncs.size() == 1 && r.side[1].desyncs.size() == 1) {
    const gekko::SessionEvent& a = r.side[0].desyncs[0];
    const gekko::SessionEvent& b = r.side[1].desyncs[0];
    CHECK(a.local != a.remote);
    CHECK(a.local == b.remote && a.remote == b.local);
    // Which is which: each side's `local` is the checksum IT handed over for frame 150, `remote` the peer's.
    CHECKF(a.local == r.side[0].cs_bad && a.remote == r.side[1].cs_bad && r.side[0].cs_bad != r.side[1].cs_bad,
           "A: local %08X remote %08X; A handed %08X, B handed %08X", a.local, a.remote, r.side[0].cs_bad, r.side[1].cs_bad);
    CHECKF(b.local == r.side[1].cs_bad && b.remote == r.side[0].cs_bad, "B: local %08X remote %08X; A handed %08X, B handed %08X",
           b.local, b.remote, r.side[0].cs_bad, r.side[1].cs_bad);
    CHECKF((b.local ^ b.remote) == 1u, "local %08X remote %08X: the difference is not exactly the perturbation", b.local, b.remote);
  }
  // Latency in ticks after B's perturbed write: measured on an instant link only, printed, bounded loosely.
  for (int s = 0; s < 2; ++s) {
    if (r.side[s].desync_tick.empty() || r.side[1].write_tick < 0) continue;
    const int after = r.side[s].desync_tick[0] - r.side[1].write_tick;
    std::printf("info: desync: side %c saw it %d ticks after B's perturbed write (instant link, not a network latency)\n",
                s ? 'B' : 'A', after);
    CHECKF(after >= 0 && after <= 5, "side %d: DesyncDetected %d ticks after the write (expected 0..5)", s, after);
    // And not before it: no side may report the desync before the wrong value existed.
    CHECK(r.side[s].desync_tick[0] >= r.side[1].write_tick);
  }
}

static void TestRealOrderControl() {
  Group("real GekkoNet, order control: no Run({}) before the update");
  // The same perturbation, but the harness fills Save(f-1) only after the update for f (one update late). GekkoNet
  // has already sent what the slot held: the checksum of frame f-2 (two states are kept, storage.cpp:12). Predicted
  // before the run: frame 150's health is the good checksum of frame 148 on both sides (no desync at 150), and the
  // perturbed value of frame 150, written late into the slot frame 152 shares, is sent as frame 152's: both sides
  // report frame 152, not 150.
  PairResult r;
  RunPair(r, kBadFrame, false, 0);
  for (int s = 0; s < 2; ++s) {
    CHECKF(r.side[s].desyncs.size() == 1, "control side %d: %zu DesyncDetected events", s, r.side[s].desyncs.size());
    if (r.side[s].desyncs.size() == 1)
      CHECKF(r.side[s].desyncs[0].frame == kBadFrame + 2, "control side %d: DesyncDetected for frame %d (expected %d, two late)", s,
             r.side[s].desyncs[0].frame, kBadFrame + 2);
  }
  CHECK(r.side[0].advances >= kFrames && r.side[1].advances >= kFrames && !r.side[0].step_ended && !r.side[1].step_ended);
}

static void TestRealDelayed() {
  Group("real GekkoNet, delayed link: three ticks each way");
  PairResult r;
  RunPair(r, -1, true, 3, 3);  // 3 ms per tick after the hand-shake: the run lasts over a second, the rates need that
  CommonChecks(r, "delayed");
  CHECK(r.side[0].desyncs.empty() && r.side[1].desyncs.empty());
  // Not vacuous: frames needed several updates (the add stayed once per frame through them). FLOOR: 99 observed on
  // both sides at a delay of 3 ticks; 0 at a delay of 0 (TestRealClean).
  for (int s = 0; s < 2; ++s)
    CHECKF(r.side[s].empty_updates_after_first >= 50, "side %d: only %d updates after the first Advance yielded nothing", s,
           r.side[s].empty_updates_after_first);
  // The wrapper's reads, on a session that has run for over a second (GekkoNet rates a window of >= 1000 ms,
  // net.cpp:92-107, and a round trip of 2 x 3 ticks x 3 ms): data really flowed both ways and the ping is not zero.
  while (std::chrono::steady_clock::now() - r.stats_base < std::chrono::milliseconds(1100))
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  for (int s = 0; s < 2; ++s) {
    const gekko::NetStats st = r.side[s].session.Stats();
    std::printf("info: delayed side %d: ping %.2f ms jitter %.2f kb/s sent %.3f received %.3f\n", s, st.ping_ms,
                st.jitter_ms, st.kb_sent, st.kb_received);
    CHECKF(st.kb_sent > 0 && st.kb_received > 0, "side %d: kb sent %.4f received %.4f", s, st.kb_sent, st.kb_received);
    CHECKF(st.ping_ms > 0, "side %d: ping %.3f ms on a link with a round trip of six ticks", s, st.ping_ms);
  }
  // And the desync detection still fires on a delayed link.
  PairResult d;
  RunPair(d, kBadFrame, true, 3);
  CommonChecks(d, "delayed desync");
  for (int s = 0; s < 2; ++s) ExpectOneDesync(d, s, kBadFrame, "delayed desync");
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  TestPlayerInput();
  TestExecutorFake();
  TestExecutorTokens();
  TestExecutorFrameMinusOne();
  TestExecutorRefusals();
  TestExecutorPlayers();
  TestFoldChecksum();
  TestWrapperLifecycle();
  TestRealClean();
  TestRealDesync();
  TestRealOrderControl();
  TestRealDelayed();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
