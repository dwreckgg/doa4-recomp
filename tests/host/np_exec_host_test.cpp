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

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>  // GetProcessMemoryInfo (K32GetProcessMemoryInfo in kernel32): the memory watchdog of the duplicating link

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// The constructor clamps the player count to 1..kUsers: a count outside it would index past the four-pad record
// (and a zero-player executor could never connect anyone). Predicted RED with the clamp removed: 2 failures
// (Executor(0) refuses the 16-byte list or takes mask 00; Executor(9) refuses the 64-byte list).
static void TestExecutorClamp() {
  Group("executor, player count clamp");
  {
    exec::Executor ex(0, nullptr);  // clamped up to one player: a 16-byte list, mask 01
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(0, Bytes(16, 0))}, 0);
    CHECKF(st.advanced && st.input.connected == 0x01, "Executor(0): advanced %d, mask %02X (expected 1, 01)", int(st.advanced),
           st.input.connected);
  }
  {
    exec::Executor ex(9, nullptr);  // clamped down to four players: a 64-byte list, mask 0F
    exec::Step st = ex.Run(std::vector<exec::Event>{Advance(0, Bytes(64, 0))}, 0);
    CHECKF(st.advanced && st.input.connected == 0x0F, "Executor(9): advanced %d, mask %02X (expected 1, 0F)", int(st.advanced),
           st.input.connected);
  }
}

// The pending Save holds pointers into GekkoNet's state storage, which die with the session: the owner drops the
// pending Save BEFORE destroying the session (np_exec.h). Predicted RED with DropPending doing nothing: 2 failures
// (the dropped slot is written, saves_filled is 1).
static void TestExecutorDropPending() {
  Group("executor, DropPending");
  SaveSlots s0, s1;
  exec::Executor ex(2, nullptr);
  ex.Run(std::vector<exec::Event>{Advance(0, TwoPads(PadN(1), PadN(2))), Save(0, &s0)}, 0x11);  // Save 0 is pending
  ex.DropPending();
  exec::Step st = ex.Run({}, 0x55);  // what a Run after the session's death would have written through the pointers
  CHECK(!st.advanced && st.end == exec::End::kNone && st.frame == -1);
  CHECK(Untouched(s0));
  CHECK(ex.saves_filled() == 0);
  CHECK(ex.next_frame() == 1);
  // The executor stays usable: the next frame stages and its Save is filled one call later as usual.
  st = ex.Run(std::vector<exec::Event>{Advance(1, TwoPads(PadN(3), PadN(4))), Save(1, &s1)}, 0x22);
  CHECK(st.advanced && st.frame == 1 && st.end == exec::End::kNone);
  ex.Run({}, 0x33);
  CHECK(s1.checksum == 0x33 && ex.saves_filled() == 1 && Untouched(s0));
  // Dropping when nothing is pending changes nothing.
  exec::Executor ex2(2, nullptr);
  ex2.DropPending();
  exec::Step e2 = ex2.Run({}, 1);
  CHECK(!e2.advanced && e2.end == exec::End::kNone && ex2.saves_filled() == 0 && ex2.next_frame() == 0);
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

// Carry 3a (Task 7): one Update reads at most 256 payloads from Receive (GekkoNet asks once per update,
// game_session.cpp:177 and 666); the rest stay queued for the next Update, so a flood cannot hold the game thread.
// The payloads are empty (the wrapper skips them before GekkoNet, np_gekko.cpp AdapterReceive), so only the count of
// reads is observed. The fake Receive stops by itself after 1,000,000 reads or 5 s, so the uncapped wrapper ends with a
// red count instead of a hang. Predicted RED with the cap removed: 5 failures (endless Receive: 1,000,000 reads, the
// bound hit, still 1,000,000 after the second Update; 300 queued: all read by the first Update, one more empty read by
// the second).
static void TestWrapperDrainCap() {
  Group("wrapper, at most 256 Receive reads per Update");
  {
    gekko::Session s;
    uint64_t reads = 0;
    bool bound_hit = false;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(s.Create(gekko::Config{}, [](std::span<const uint8_t>) {}, [&](Bytes* out) {
      if (reads >= 1'000'000 || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
        bound_hit = true;
        return false;
      }
      ++reads;
      out->clear();
      return true;
    }));
    s.Update();
    CHECKF(reads == 256, "one Update read %llu payloads from a Receive that always has data (expected 256)", (unsigned long long)reads);
    CHECK(!bound_hit);
    s.Update();
    CHECKF(reads == 512, "two Updates read %llu (expected 512)", (unsigned long long)reads);
  }
  {
    gekko::Session s;
    std::deque<Bytes> q(300);  // 300 queued (empty) payloads
    int false_returns = 0;
    CHECK(s.Create(gekko::Config{}, [](std::span<const uint8_t>) {}, [&](Bytes* out) {
      if (q.empty()) {
        ++false_returns;
        return false;
      }
      *out = std::move(q.front());
      q.pop_front();
      return true;
    }));
    s.Update();
    CHECKF(q.size() == 44 && false_returns == 0, "after one Update %zu of 300 remain, %d empty reads (expected 44, 0)", q.size(),
           false_returns);
    s.Update();
    CHECKF(q.empty() && false_returns == 1, "after two Updates %zu remain, %d empty reads (expected 0, 1)", q.size(), false_returns);
  }
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
  bool dead = false;  // the link has been cut: every payload sent from now on is dropped, nothing is delivered
  int dropped = 0;    // payloads dropped while dead
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
  int disconnects = 0;  // kDisconnected events seen
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
  int take_every = 1;  // the harness takes a side's session events every Nth tick (and once more when the run ends)
  std::chrono::steady_clock::time_point stats_base{};  // when both sessions had started (Stats() read once there)
  // Dead-link run (RunPair's cut_after_frames): the cut moment, and each side's advances at the cut and at the end.
  std::chrono::steady_clock::time_point cut_at{};
  int adv_at_cut[2] = {0, 0};
  int adv_at_end[2] = {0, 0};
  long long outage_ms = 0;
};

// Takes one side's session events (Update() has already drained GekkoNet's into the wrapper).
static void DrainEvents(PairResult& r, int s) {
  Side& S = r.side[s];
  for (const gekko::SessionEvent& e : S.session.TakeSessionEvents()) {
    if (e.kind == gekko::SessionEvent::kStarted) S.started = true;
    if (e.kind == gekko::SessionEvent::kDisconnected) ++S.disconnects;
    if (e.kind == gekko::SessionEvent::kDesync) {
      S.desyncs.push_back(e);
      S.desync_tick.push_back(r.ticks);
    }
  }
}

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
  if (r.ticks % r.take_every == 0) DrainEvents(r, s);
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

// cut_after_frames >= 0: when both sides have that many Advances the link is cut (queues emptied, every later payload
// dropped) and the run goes on for dead_ms of WALL CLOCK at 5 ms per tick, because GekkoNet's disconnect timeout
// reads steady_clock (backend.cpp:733-759); the run ends there.
static void RunPair(PairResult& r, int bad_frame_side_b, bool pre_run, int delay_ticks, int tick_sleep_ms = 0, int take_every = 1,
                    int cut_after_frames = -1, int dead_ms = 0) {
  r.link.delay = delay_ticks;
  r.take_every = take_every;
  for (int s = 0; s < 2; ++s) {
    gekko::Config cfg;
    cfg.num_players = 2;
    cfg.local_slot = s;
    cfg.delay = kDelay;
    bool ok = r.side[s].session.Create(
        cfg,
        [&r, s](std::span<const uint8_t> payload) {
          if (r.link.dead) {
            ++r.link.dropped;
            return;
          }
          r.link.q[1 - s].push_back({r.link.now + r.link.delay, Bytes(payload.begin(), payload.end())});
          ++r.link.sent[s];
        },
        [&r, s](Bytes* out) {
          auto& q = r.link.q[s];
          if (r.link.dead || q.empty() || q.front().due > r.link.now) return false;
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
    if (cut_after_frames >= 0) {
      if (!r.link.dead && both_started && std::min(r.side[0].advances, r.side[1].advances) >= cut_after_frames) {
        r.link.dead = true;
        r.link.q[0].clear();
        r.link.q[1].clear();
        r.cut_at = std::chrono::steady_clock::now();
        for (int k = 0; k < 2; ++k) r.adv_at_cut[k] = r.side[k].advances;
      }
      if (r.link.dead) {
        r.outage_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - r.cut_at).count();
        if (r.outage_ms >= dead_ms) {
          for (int k = 0; k < 2; ++k) r.adv_at_end[k] = r.side[k].advances;
          ++r.ticks;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
    }
    if (both_started && r.side[0].advances >= kFrames && r.side[1].advances >= kFrames) {
      ++r.ticks;
      break;
    }
    if (!both_started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    else if (tick_sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(tick_sleep_ms));
  }
  DrainEvents(r, 0);  // the final take of a run that took only every Nth tick
  DrainEvents(r, 1);
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
  // has already sent what the slot held: the checksum of frame f-2 (two states are kept, storage.cpp:11). Predicted
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
    // Jitter is the spread of the round trips, not their average: on a link of constant delay it is a small fraction of
    // the ping (observed 1.1 and 1.3 ms against 93.6 and 93.5 ms; a jitter that reads the average ping equals it).
    // Predicted RED with the wrapper's jitter field fed from the average ping: 2 failures (one per side).
    CHECKF(st.jitter_ms < 0.5f * st.ping_ms, "side %d: jitter %.2f ms against ping %.2f ms on a constant-delay link", s, st.jitter_ms,
           st.ping_ms);
  }
  // And the desync detection still fires on a delayed link.
  PairResult d;
  RunPair(d, kBadFrame, true, 3);
  CommonChecks(d, "delayed desync");
  for (int s = 0; s < 2; ++s) ExpectOneDesync(d, s, kBadFrame, "delayed desync");
}

// "Since the last call": session events accumulate across Updates until the owner takes them (Task 11 takes them once
// per frame, after however many stalled Updates). The harness here takes only every 5th tick and once at the end.
// Predicted RED with Update() clearing the wrapper's event list at its start: 2 failures (the desync, raised in one
// Update and taken some Updates later, is lost on each side).
static void TestRealEventsBatched() {
  Group("real GekkoNet, session events taken every 5th tick");
  PairResult d;
  RunPair(d, kBadFrame, true, 0, 0, 5);
  CHECK(d.take_every == 5);
  for (int s = 0; s < 2; ++s) ExpectOneDesync(d, s, kBadFrame, "batched take");
  CHECK(d.side[0].started && d.side[1].started);
}

// R4 "no automatic disconnect". GekkoNet's own timeout defaults to 5000 ms (backend.cpp:16) and is judged against
// steady_clock (backend.cpp:733-759), so only a wall-clock wait can tell whether the wrapper turned it off
// (gekko_set_disconnect_timeout(session, 0), np_gekko.cpp Create). The link is cut after 100 frames and stays dead
// for 5.7 s. With the timeout off: no kDisconnected on either side and no more Advances than the lockstep lead
// allows. With it on (measured by the reviewer with the line deleted): kDisconnected on both sides at about 5.0 s and
// about 100 Advances on neutral peer input, a ghost match on each PC.
// Predicted RED with the line deleted: 4 failures (kDisconnected on each side, Advances in the outage on each side).
constexpr int kDeadMs = 5700;
static_assert(kDeadMs >= 5000 + 500, "the outage must outlast GekkoNet's default disconnect timeout (5000 ms) with a margin");
// CEILING on Advances after the cut: observed 4 and 0. Lockstep lets a side run on only until it lacks the peer's input
// for the next frame, i.e. by the lead it had plus the input delay (2): at most about 2 x 2 + 2; 8 leaves room and is
// still far from the ~100 of a session that went on without its peer.
constexpr int kDeadAdvanceCeiling = 2 * kDelay + 4;

static void TestRealDeadLink() {
  Group("real GekkoNet, link dead for 5.7 s of wall clock: no automatic disconnect");
  PairResult r;
  RunPair(r, -1, true, 0, 0, 1, 100, kDeadMs);
  CHECKF(r.link.dead, "the link was never cut (both sides must reach 100 Advances first)");
  CHECKF(r.adv_at_cut[0] >= 100 && r.adv_at_cut[1] >= 100, "Advances at the cut: A %d, B %d", r.adv_at_cut[0], r.adv_at_cut[1]);
  CHECKF(r.outage_ms >= kDeadMs, "the outage lasted %lld ms (expected >= %d)", r.outage_ms, kDeadMs);
  CHECKF(r.link.dropped > 0, "no payload was dropped while the link was dead: the sessions did not keep talking");
  for (int s = 0; s < 2; ++s) {
    const int adv = r.adv_at_end[s] - r.adv_at_cut[s];
    std::printf("info: dead link side %d: %d Advances in %lld ms of outage, %d kDisconnected, %d payloads dropped in all\n", s, adv,
                r.outage_ms, r.side[s].disconnects, r.link.dropped);
    CHECKF(r.side[s].disconnects == 0, "side %d: %d kDisconnected events during a %lld ms outage (R4: no automatic disconnect)", s,
           r.side[s].disconnects, r.outage_ms);
    CHECKF(adv <= kDeadAdvanceCeiling, "side %d: %d Advances while the link was dead (ceiling %d)", s, adv, kDeadAdvanceCeiling);
    CHECKF(!r.side[s].step_ended, "side %d: the executor ended the session", s);
  }
}

// Carry 3b (Task 7, from the Task 6 review): a link that delivers EVERY datagram twice, each direction through a bounded
// queue of 1024 (a full queue drops the datagram and counts it), 600 frames. The review grew a test process to 16 GB in
// 6 minutes on such a link (unbounded queues; GekkoNet answers every SyncResponse while connected, backend.cpp:956), so
// the group carries its watchdogs from its first run (#0434): it fails and stops after 60 s of wall clock, or when the
// process working set passes 1 GB, both checked every tick; and a watchdog thread ends the whole run with a FAIL line at
// 90 s or 1.5 GB, in case one Update never returns. The datagrams GekkoNet sends per frame are printed (maximum and the
// average over the first and the last 100 frames, per side): traffic that grows without bound shows as a rising count.
// The same harness with one copy per datagram runs first as the control (a normal link's count), and a third run replays
// one captured handshake SyncResponse into side B every 10 frames from frame 100 (the drip; the Task 7 review's probe).
// Why: once connected GekkoNet answers every SyncResponse it receives (backend.cpp:955-958, 984-987), so a duplicated or
// replayed one bounces between the sides until a datagram is lost, and each further one adds a chain. Measured before
// np_gekko's SyncResponse guard: the duplicating link sat at ~380 datagrams per frame per side (the 256-read cap and the
// queues), the drip grew 5 -> 20 -> 40 per frame (linear in the replays). Pinned with the guard: every run's last-100
// average <= 4 datagrams per frame per side (the control is 3.00: Inputs, InputAck, SessionHealth), and the drip run's
// guard dropped replays (positive evidence it acted). The drip goes red if a GekkoNet bump moves SyncResponse off byte 0 = 5.
constexpr int kDupFrames = 600;
constexpr double kMaxPerFrame = 4.0;  // datagrams per frame per side, last-100 average (the review's pin)
constexpr size_t kDupQueue = 1024;
constexpr long long kDupTimeLimitMs = 60'000;
constexpr size_t kDupMemLimit = size_t(1) << 30;  // 1 GB

static size_t WorkingSet() {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof pmc;
  return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? size_t(pmc.WorkingSetSize) : 0;
}

// copies = 2: the duplicating link; copies = 1: the same harness as a control (a normal link's traffic per frame).
// drip_every > 0: from side A's frame 100, one copy of the first SyncResponse A sent (captured in the handshake) is put
// in B's queue every drip_every of A's frames.
static void RunBoundedLink(int copies, const char* label, int drip_every = 0) {
  std::atomic<bool> group_done{false};
  const auto t0 = std::chrono::steady_clock::now();
  std::thread guard([&] {  // the backstop: an Update that never returns, or memory that grows inside one call
    while (!group_done.load()) {
      const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      const size_t ws = WorkingSet();
      if (ms > 90'000 || ws > (size_t(3) << 29)) {
        std::printf("FAIL watchdog: %s after %lld ms, working set %zu MB: run ended\n", label, ms, ws >> 20);
        std::fflush(stdout);
        std::_Exit(3);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  PairResult r;
  std::deque<Bytes> q[2];  // q[s]: what waits for side s
  size_t q_max[2] = {0, 0};
  int sent[2] = {0, 0}, delivered[2] = {0, 0}, dropped_full[2] = {0, 0};
  int cur[2] = {0, 0};
  std::vector<int> per_frame[2];  // datagrams side s sent during each of its frames
  Bytes captured_sr;              // the first SyncResponse side A sent (byte 0 = 5, GekkoNet net.h:27-33)
  int drips = 0, next_drip = 100;
  for (int s = 0; s < 2; ++s) {
    gekko::Config cfg;
    cfg.num_players = 2;
    cfg.local_slot = s;
    cfg.delay = kDelay;
    CHECK(r.side[s].session.Create(
        cfg,
        [&, s](std::span<const uint8_t> payload) {
          ++sent[s];
          ++cur[s];
          if (s == 0 && captured_sr.empty() && !payload.empty() && payload[0] == 5) captured_sr.assign(payload.begin(), payload.end());
          for (int copy = 0; copy < copies; ++copy) {
            if (q[1 - s].size() >= kDupQueue) {
              ++dropped_full[1 - s];
              continue;
            }
            q[1 - s].emplace_back(payload.begin(), payload.end());
            q_max[1 - s] = std::max(q_max[1 - s], q[1 - s].size());
          }
        },
        [&, s](Bytes* out) {
          if (q[s].empty()) return false;
          *out = std::move(q[s].front());
          q[s].pop_front();
          ++delivered[s];
          return true;
        }));
  }
  bool timed_out = false, mem_out = false;
  size_t ws_max = 0;
  long long elapsed_ms = 0;
  for (r.ticks = 0; r.ticks < 200'000; ++r.ticks) {
    const int before[2] = {r.side[0].advances, r.side[1].advances};
    StepSide(r, 0, true);
    StepSide(r, 1, true);
    for (int s = 0; s < 2; ++s)
      if (r.side[s].advances > before[s]) {
        per_frame[s].push_back(cur[s]);
        cur[s] = 0;
      }
    if (drip_every > 0 && !captured_sr.empty() && r.side[0].advances >= next_drip && q[1].size() < kDupQueue) {
      q[1].push_back(captured_sr);
      ++drips;
      next_drip += drip_every;
    }
    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const size_t ws = WorkingSet();
    ws_max = std::max(ws_max, ws);
    if (elapsed_ms > kDupTimeLimitMs) {
      timed_out = true;
      break;
    }
    if (ws > kDupMemLimit) {
      mem_out = true;
      break;
    }
    const bool both_started = r.side[0].started && r.side[1].started;
    if (both_started && r.side[0].advances >= kDupFrames && r.side[1].advances >= kDupFrames) {
      ++r.ticks;
      break;
    }
    if (!both_started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  DrainEvents(r, 0);
  DrainEvents(r, 1);
  group_done = true;
  guard.join();
  CHECKF(!timed_out, "%s ran past %lld ms (time watchdog)", label, kDupTimeLimitMs);
  CHECKF(!mem_out, "%s: the working set passed %zu MB (memory watchdog)", label, kDupMemLimit >> 20);
  std::printf("info: %s: %d ticks, %lld ms, working set max %zu MB\n", label, r.ticks, elapsed_ms, ws_max >> 20);
  for (int s = 0; s < 2; ++s) {
    const std::vector<int>& f = per_frame[s];
    int mx = 0;
    double last100 = 0;
    for (int v : f) mx = std::max(mx, v);
    const size_t n = std::min<size_t>(100, f.size());
    for (size_t i = f.size() - n; i < f.size(); ++i) last100 += f[i];
    if (n) last100 /= double(n);
    double first100 = 0;
    const size_t m = std::min<size_t>(100, f.size());
    for (size_t i = 0; i < m; ++i) first100 += f[i];
    if (m) first100 /= double(m);
    const uint32_t guard_drops = r.side[s].session.sync_responses_dropped();
    std::printf("info: %s side %d: %d advances, %d datagrams sent, %d delivered to it, %d dropped (queue full), queue "
                "max %zu; datagrams per frame: max %d, first-100 average %.2f, last-100 average %.2f; SyncResponses dropped by "
                "the guard %u\n",
                label, s, r.side[s].advances, sent[s], delivered[s], dropped_full[s], q_max[s], mx, first100, last100, guard_drops);
    CHECKF(n == 100 && last100 <= kMaxPerFrame, "%s side %d: last-100 average %.2f datagrams per frame (pin <= %.0f; %zu frames)",
           label, s, last100, kMaxPerFrame, n);
    if (copies == 1 && drip_every == 0)
      CHECKF(guard_drops == 0, "%s side %d: the guard dropped %u SyncResponses on a clean link", label, s, guard_drops);
    CHECKF(r.side[s].advances >= kDupFrames, "%s side %d: %d advances (expected %d)", label, s, r.side[s].advances, kDupFrames);
    CHECKF(r.side[s].desyncs.empty(), "%s side %d: %zu DesyncDetected events", label, s, r.side[s].desyncs.size());
    CHECKF(!r.side[s].step_ended, "%s side %d: the executor ended the session", label, s);
    CHECKF(r.side[s].disconnects == 0, "%s side %d: %d kDisconnected events", label, s, r.side[s].disconnects);
    CHECKF(q_max[s] <= kDupQueue, "%s side %d: queue reached %zu (bound %zu)", label, s, q_max[s], kDupQueue);
  }
  if (drip_every > 0) {
    // Fixture: the replays happened (50 from frame 100 to 600 at one per 10) and the guard is what kept the count flat.
    CHECKF(!captured_sr.empty() && drips >= 45, "%s: %d SyncResponses replayed (captured %zu bytes)", label, drips, captured_sr.size());
    CHECKF(r.side[1].session.sync_responses_dropped() > 0, "%s: the guard on B dropped no replayed SyncResponse", label);
    std::printf("info: %s: %d replayed\n", label, drips);
  }
  // The frames both sides ran carry the same inputs, in order, and the pattern each side added (CommonChecks).
  r.link.sent[0] = sent[0];
  r.link.sent[1] = sent[1];
  if (!timed_out && !mem_out) CommonChecks(r, label);
}

static void TestRealDuplicatingLink() {
  Group("real GekkoNet, every datagram delivered twice through bounded queues, 600 frames (and a single-copy control)");
  RunBoundedLink(1, "single-copy link (control)");
  RunBoundedLink(2, "duplicating link");
  RunBoundedLink(1, "SyncResponse drip (1 per 10 frames)", 10);
}

// The guard leaves the handshake to GekkoNet (it acts only after this side's kStarted), so handshakes over a lossy link
// still finish: 12 seeded runs at 10% loss each way, wall clock (GekkoNet resends the handshake every 200 ms,
// backend.cpp:375-381). The review measured 12/12 with the guard, mean 559 ms, max 1081 (unguarded: mean 565, max 1330).
// Watchdog: each run gives up after 5 s (FAIL), the whole group after 12 x 5 s.
static void TestRealHandshakeLoss() {
  Group("real GekkoNet, handshakes at 10% loss with the SyncResponse guard, 12 runs");
  int finished = 0;
  long long sum_ms = 0, max_ms = 0;
  for (uint32_t seed = 1; seed <= 12; ++seed) {
    PairResult r;
    std::deque<Bytes> q[2];
    uint32_t rng = seed * 2654435761u;
    auto lose = [&rng] {  // 10% loss, xorshift32
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      return rng % 100 < 10;
    };
    for (int s = 0; s < 2; ++s) {
      gekko::Config cfg;
      cfg.num_players = 2;
      cfg.local_slot = s;
      cfg.delay = kDelay;
      CHECK(r.side[s].session.Create(
          cfg,
          [&, s](std::span<const uint8_t> p) {
            if (!lose() && q[1 - s].size() < kDupQueue) q[1 - s].emplace_back(p.begin(), p.end());
          },
          [&, s](Bytes* out) {
            if (q[s].empty()) return false;
            *out = std::move(q[s].front());
            q[s].pop_front();
            return true;
          }));
    }
    const auto t0 = std::chrono::steady_clock::now();
    long long ms = 0;
    bool both = false;
    for (r.ticks = 0; !both && ms < 5000; ++r.ticks) {
      StepSide(r, 0, true);
      StepSide(r, 1, true);
      both = r.side[0].started && r.side[1].started;
      ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      if (!both) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (both) {
      ++finished;
      sum_ms += ms;
      max_ms = std::max(max_ms, ms);
    }
    CHECKF(both, "seed %u: the handshake at 10%% loss did not finish in 5 s", seed);
  }
  std::printf("info: handshakes at 10%% loss: %d of 12 finished, mean %lld ms, max %lld ms\n", finished,
              finished ? sum_ms / finished : 0, max_ms);
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a run ended by a watchdog or a crash still shows how far it got
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  TestPlayerInput();
  TestExecutorFake();
  TestExecutorTokens();
  TestExecutorFrameMinusOne();
  TestExecutorRefusals();
  TestExecutorPlayers();
  TestExecutorClamp();
  TestExecutorDropPending();
  TestFoldChecksum();
  TestWrapperLifecycle();
  TestWrapperDrainCap();
  TestRealClean();
  TestRealDesync();
  TestRealOrderControl();
  TestRealDelayed();
  TestRealEventsBatched();
  TestRealDeadLink();
  TestRealDuplicatingLink();
  TestRealHandshakeLoss();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
