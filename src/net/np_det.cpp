// DOA4 netplay phase 0: the determinism layer's core. See np_det.h.
//
// Fixes (DOA4_NP_FIX names; each is active only in a session). The evidence
// for each (two replays of one recording, first frame whose digest differs,
// with and without the fix) is in the proof's hand-back report.
//   seed      every Mersenne Twister seeding (sub_82334C60, the single seeding
//             function of the four MTs, phase0_facts.md 7) gets a value derived
//             from the session seed, the MT's address and how many times that MT
//             was seeded before (so each match draws differently, the same in
//             every run).
//   clock     the game's clock wrappers return time derived from the session
//             frame: GetTickCount (sub_82790138), QueryPerformanceCounter = mftb
//             (sub_82792660), KeQuerySystemTime (sub_82790130). Within a frame
//             every read of a thread advances its clock a little (10 us for the
//             time base, 1 ms per 16 reads of the tick count), so a loop that
//             waits on the clock still ends (Tekken 6 t6_determinism.cpp).
//   sdkclock  the same virtual time for every other guest read of the time base
//             and the system time (the inline mftb sites and the direct
//             KeQuerySystemTime imports), through the SDK's guest-time override;
//             guest threads only (the GPU vblank thread keeps the host clock).
//             The SDK's audio worker, while it runs the guest audio callback, gets
//             time from the callback number instead (AudioTimebase,
//             rex_audio_worker_callback), the same in every run.
//   xam       XAM's deferred overlapped completions (message boxes, content,
//             enumerators) complete inline with no host delay, and XAM dialogs
//             answer with their default button (headless).
//   audio, movie, loader, cri, crijob: np_det_middleware.cpp.
//   workers:  np_det_workers.cpp.
//   notify    system notifications (XNotifyGetNext) become visible at the frame boundary after they were
//             queued (sdk-np0 xnotifylistener.cpp). Nothing is dropped: this SDK posts no notification
//             about the host's own controllers (see its DroppedInLockstep). Every enqueue today is made
//             inside a guest call, so each is dequeued in the same frame in every run. The boundary is in
//             np_det_middleware.cpp.
//   keepalive the D3D runtime's kick thread (0x8275BD18) no longer re-presents the last frame when its 30 ms
//             HOST wait for a pending swap times out: sub_8275AD70 and sub_8275AEE0 return at once when the
//             kick thread calls them from that branch (lr 0x8275BD9C / 0x8275BDAC; np_core.h ClassifyKeepAlive).
//             Each re-present advanced the D3D CPU fence counter, which the main thread stamps into static
//             texture headers and into a pointer the dynamic-buffer allocator reads (0x8357C190), so the
//             count per frame followed host speed (Task 11a). The kick thread calling them from any other site
//             runs the call, logs one "WARN keep-alive" line (np_run.py fails such a run) and is counted; the
//             skip count is host-timed, so it is logged at the session's end, never a counter of the gate.
//   reader    after sub_82731790 started the save manager's one-shot file thread (0x82731758: one file
//             operation, then exit), the caller waits up to 2 s until that thread's host thread has ended;
//             the game polls the thread with a zero wait every frame, so under load the save manager went on
//             a frame later (Task 11a: the save-slot time 0x831B99D0 and the thread's state block 0x82E476A0).
//             A thread not found or not ended after 2 s is a fallback (fb_reader).
// DOA4_NP_FIX: "all" (the default), "none", a comma list, or "all-x-y" (all but
// x and y); names and bits: np_core.h kFixNames.

#include "np_det.h"

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xthread.h>

#include "generated/default/dead_or_alive_4_init.h"

#include "np_core.h"
#include "np_det_internal.h"
#include "np_session.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern "C" {
REX_EXTERN(__imp__sub_82334C60);
REX_EXTERN(__imp__sub_82790138);
REX_EXTERN(__imp__sub_82792660);
REX_EXTERN(__imp__sub_82790130);
REX_EXTERN(__imp__sub_82793B68);
REX_EXTERN(__imp__sub_8275AD70);
REX_EXTERN(__imp__sub_82731790);
// SDK netplay exports (sdk-np0 src/system/kernel_state.cpp, src/audio/audio_system.cpp,
// src/system/xnotifylistener.cpp).
__declspec(dllimport) void rex_overlapped_inline(int on);
__declspec(dllimport) void rex_notify_lockstep(int on);
__declspec(dllimport) void rex_audio_lockstep(int on);
__declspec(dllimport) int rex_audio_worker_callback(uint64_t* index);
}

namespace doa4::np::det {
namespace {

// The Fix bits (np_det_internal.h) are the indices of np_core.h kFixNames, which DOA4_NP_FIX and the meta use.
constexpr bool FixIs(Fix bit, int i, std::string_view name) {
  return i < kFixCount && bit == 1u << i && std::string_view(kFixNames[i]) == name;
}
static_assert(FixIs(kSeed, 0, "seed") && FixIs(kClock, 1, "clock") && FixIs(kSdkClock, 2, "sdkclock") &&
                  FixIs(kXam, 3, "xam") && FixIs(kAudio, 4, "audio") && FixIs(kMovie, 5, "movie") &&
                  FixIs(kLoader, 6, "loader") && FixIs(kCri, 7, "cri") && FixIs(kCriJob, 8, "crijob") &&
                  FixIs(kWorkers, 9, "workers") && FixIs(kNotify, 10, "notify") &&
                  FixIs(kKeepAlive, 11, "keepalive") && FixIs(kReader, 12, "reader") && kFixCount == 13,
              "one Fix bit per kFixNames entry, in that order");

std::atomic<uint32_t> g_fixes{0};
std::atomic<uint32_t> g_frame{0};
std::atomic<bool> g_stopped{false};
std::string g_headless_before = "false";  // the headless cvar as Start found it; Stop puts it back
uint64_t g_seed = 1;
SeedSeries g_seeds;  // seedings so far per MT in the session (np_core.h; Start resets it)
std::atomic<uint32_t> g_counters[kCounterCount];
constexpr const char* kCounterNames[] = {
    "fb_worker_token", "fb_main_wait",  "fb_unranked",    "fb_loader_outside", "fb_loader_idle",
    "fb_audio_late",   "fb_spin_cap",   "fb_cri_ready",   "fb_audio_start",    "fb_reader",
    "pr_cri_passes",   "pr_spin_steps", "pr_loader_jobs", "pr_cri_jobs",       "pr_reseeds",
    "pr_notify"};
static_assert(std::size(kCounterNames) == kCounterCount, "one name per Counter, in enum order");

// ---- keepalive ----
// Calls of the kick thread's timeout re-present skipped, per function (sub_8275AD70, sub_8275AEE0), and calls of
// the two by the kick thread from any other site (run, not skipped). Host-timed: logged at Stop, never a counter.
std::atomic<uint32_t> g_keepalive_skips[2];
std::atomic<uint32_t> g_keepalive_unexpected{0};

// ---- reader ----
std::atomic<uint32_t> g_reader_threads{0}, g_reader_fallbacks{0};

// ---- Virtual clocks ----
// The formulas and the never-backwards rule are in np_core.h; each thread keeps its own clock.
thread_local ThreadClock t_clock;

// The SDK's guest-time override: guest threads get the virtual clocks; host
// threads (GPU vblank, kernel dispatch) keep the host clock; the audio worker, while it runs
// a guest audio callback, gets time from its callback number (np_core.h AudioTimebase).
bool GuestTime(int kind, uint64_t* value) {
  if (!On(kSdkClock)) return false;
  uint64_t cb;
  if (rex_audio_worker_callback(&cb)) {  // the SDK audio worker running a guest callback
    *value = kind == 0 ? AudioTimebase(cb) : AudioFileTime(cb);
    return true;
  }
  // GetCurrentThread asserts on a thread that is not bound to an XThread (a debug SDK), so ask first.
  if (!rex::system::XThread::IsInThread()) return false;  // a plain host thread (no XThread)
  rex::system::XThread* thread = rex::system::XThread::GetCurrentThread();
  if (!thread->is_guest_thread()) return false;
  *value = kind == 0 ? VirtualTimebase(&t_clock, Frame()) : VirtualFileTime(&t_clock, Frame());
  if (Tracing()) {
    auto* ts = rex::runtime::ThreadState::Get();
    TraceRead(kind == 0 ? kRdSdkTb : kRdSdkFt, ts && ts->context() ? uint32_t(ts->context()->lr) : 0);
  }
  return true;
}

// The keepalive fix for one call of sub_8275AD70 / sub_8275AEE0: true = return at once.
bool KeepAliveSkip(KeepAliveFn fn, uint32_t lr) {
  if (!doa4::np::Active() || !On(kKeepAlive)) return false;
  switch (ClassifyKeepAlive(fn, ThreadStart(), lr)) {
    case KeepAliveCall::kPass:
      return false;
    case KeepAliveCall::kSkip:
      g_keepalive_skips[fn == KeepAliveFn::kAd70 ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
      return true;
    case KeepAliveCall::kUnexpected:
      // The fix assumes the timeout branch is the kick thread's only call site of the two; another one runs as
      // the game wrote it, and the run fails the gate (np_run.py: a "WARN " line).
      if (g_keepalive_unexpected.fetch_add(1, std::memory_order_relaxed) == 0) {
        LogLine(fmt::format("WARN keep-alive: {} called on the D3D kick thread {:08X} from lr {:08X}, not from {:08X} "
                            "(frame {}): run, not skipped; the fix's assumption does not hold",
                            fn == KeepAliveFn::kAd70 ? "sub_8275AD70" : "sub_8275AEE0", kD3dKickThreadStart, lr,
                            KeepAliveLr(fn), Frame()));
      }
      return false;
  }
  return false;
}

// The reader fix, after sub_82731790 returned `ret` (on the thread that called it).
void ReaderStarted(int32_t ret, uint8_t* base) {
  bool found = false, ended = false;
  uint32_t handle = 0;
  if (ret == 1) {
    handle = rex::memory::load_and_swap<uint32_t>(base + kReaderThreadHandle);
    auto* ks = rex::system::kernel_state();
    if (ks && handle) {
      auto thread = ks->object_table()->LookupObject<rex::system::XThread>(handle);
      if (thread && thread->thread()) {
        found = true;
        ended = WaitForSingleObject(thread->thread()->native_handle(), DWORD(kReaderWaitMs)) == WAIT_OBJECT_0;
      }
    }
  }
  const ReaderWait w = ReaderOutcome(ret, found, ended);
  if (w == ReaderWait::kNoThread) {
    LogLine(fmt::format("reader: frame {}: sub_82731790 returned {}: no file thread to wait for", Frame(), ret));
    return;
  }
  const uint32_t n = g_reader_threads.fetch_add(1, std::memory_order_relaxed);
  if (w == ReaderWait::kFallback) {
    Count(kFbReader);
    g_reader_fallbacks.fetch_add(1, std::memory_order_relaxed);
  }
  if (n < 20 || w == ReaderWait::kFallback) {
    LogLine(fmt::format("reader: frame {}: file thread (handle {:08X}) {}", Frame(), handle,
                        w == ReaderWait::kEnded ? "ended before the caller went on"
                        : found                 ? "not ended after 2 s: the game runs on (fb_reader)"
                                                : "not found: the game runs on (fb_reader)"));
  }
}

}  // namespace

bool SkipKeepAliveSwap(uint32_t lr) { return KeepAliveSkip(KeepAliveFn::kAee0, lr); }

bool On(uint32_t fix) {
  return (g_fixes.load(std::memory_order_relaxed) & fix) != 0 &&
         !g_stopped.load(std::memory_order_relaxed);
}
void Count(Counter c, uint32_t n) { g_counters[c].fetch_add(n, std::memory_order_relaxed); }

const std::vector<std::string>& CounterNames() {
  static const std::vector<std::string> names(std::begin(kCounterNames), std::end(kCounterNames));
  return names;
}

void CounterValues(std::vector<uint32_t>* out) {
  out->resize(kCounterCount);
  for (int i = 0; i < kCounterCount; ++i) (*out)[i] = g_counters[i].load(std::memory_order_relaxed);
}

bool Stopped() { return g_stopped.load(std::memory_order_relaxed); }
uint32_t Frame() { return g_frame.load(std::memory_order_acquire); }

uint32_t ThreadStart() {
  thread_local uint32_t start = ~0u;
  if (start == ~0u) {
    if (!rex::system::XThread::IsInThread()) return 0;  // a plain host thread: no entry point (not cached)
    rex::system::XThread* t = rex::system::XThread::GetCurrentThread();
    start = t && t->creation_params() ? uint32_t(t->creation_params()->start_address) : 0;
  }
  return start;
}

bool IsMainThread() {
  if (!rex::system::XThread::IsInThread()) return false;
  rex::system::XThread* t = rex::system::XThread::GetCurrentThread();
  return t && t->main_thread();
}

void ThreadCreated(uint32_t start, uint32_t tid) {
  WorkerCreated(start, tid);
  MiddlewareThreadCreated(start);
}

std::string FixSetName() { return FixSetText(ParseFixSet(std::getenv("DOA4_NP_FIX"))); }

void Start(uint64_t seed, const std::filesystem::path& dir) {
  g_seed = seed;
  g_seeds.Reset();  // the series counts the seedings of this session
  TraceStart(dir);
  const uint32_t fixes = ParseFixSet(std::getenv("DOA4_NP_FIX"));
  g_fixes.store(fixes, std::memory_order_release);
  LogLine("determinism fixes: " + FixSetName());
  if (fixes & kSdkClock) rex::chrono::Clock::set_guest_time_override(&GuestTime);
  g_headless_before = rex::cvar::GetFlagByName("headless");
  if (fixes & kXam) {
    rex_overlapped_inline(1);
    rex::cvar::SetFlagByName("headless", "true");
  }
  MiddlewareStart();
  if (fixes & kNotify) rex_notify_lockstep(1);  // everything queued so far is visible; later ones at a boundary
}

void OnFrame(uint32_t frame, PPCContext& ctx, uint8_t* base) {
  TraceFrame(frame);
  g_frame.store(frame, std::memory_order_release);
  WorkersFrame();
  MiddlewareFrame(frame, ctx, base);
}

void Stop() {
  if (g_stopped.exchange(true, std::memory_order_acq_rel)) return;  // once
  WorkersStop();
  MiddlewareStop();  // a loader job waiting at the gate sees Stopped() now, not after its 500 ms timeout
  const uint32_t fixes = g_fixes.load(std::memory_order_acquire);
  if (fixes & kAudio) rex_audio_lockstep(0);  // audio and XMA lockstep off (sdk-np0 audio_system.cpp:420)
  if (fixes & kNotify) rex_notify_lockstep(0);  // the guest sees every queued notification again
  if (fixes & kXam) {
    rex_overlapped_inline(0);
    rex::cvar::SetFlagByName("headless", g_headless_before);
  }
  if (fixes & kSdkClock) rex::chrono::Clock::set_guest_time_override(nullptr);
  const auto state = [&](uint32_t bit) { return (fixes & bit) ? "released" : "never held"; };
  LogLine(fmt::format(
      "determinism layer off: lockstep audio {}, inline completions and headless {} (headless is now '{}', was '{}'), "
      "clock override {}, notification lockstep {}",
      state(kAudio), state(kXam), rex::cvar::GetFlagByName("headless"), g_headless_before, state(kSdkClock),
      state(kNotify)));
  // Host-timed counts (how often the 30 ms timeout fired depends on the host's speed): logged, never gated on.
  LogLine(fmt::format("keep-alive ({}): {} calls skipped on the D3D kick thread (sub_8275AD70 {}, sub_8275AEE0 {}), "
                      "{} from another site",
                      (fixes & kKeepAlive) ? "on" : "off", g_keepalive_skips[0].load() + g_keepalive_skips[1].load(),
                      g_keepalive_skips[0].load(), g_keepalive_skips[1].load(), g_keepalive_unexpected.load()));
  LogLine(fmt::format("reader ({}): {} file threads waited for, {} fallbacks", (fixes & kReader) ? "on" : "off",
                      g_reader_threads.load(), g_reader_fallbacks.load()));
}

}  // namespace doa4::np::det

using namespace doa4::np::det;

// sub_82334C60(state r3, seed r4): seeds one Mersenne Twister.
REX_HOOK_RAW(sub_82334C60) {
  if (doa4::np::Active() && On(kSeed)) {
    const uint32_t state = ctx.r3.u32;
    const uint32_t old_seed = ctx.r4.u32;
    // From the session seed, this MT's address and how many times it was seeded before (np_core.h
    // MtSeed): the first seeding of each MT is the proof's value, a later one draws anew.
    const uint32_t seed = g_seeds.Next(g_seed, state);
    ctx.r4.u64 = seed;
    Count(kPrReseeds);
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 64) {
      doa4::np::LogLine(fmt::format("seed: MT {:08X} seed {:08X} -> {:08X} (frame {}, lr {:08X})", state,
                                    old_seed, seed, Frame(), uint32_t(ctx.lr)));
    }
  }
  __imp__sub_82334C60(ctx, base);
}

// GetTickCount: r3 = ms. On the main thread a second read from one call site in
// a frame is a wait loop: the middleware takes one step first (SpinRead).
REX_HOOK_RAW(sub_82790138) {
  if (doa4::np::Active() && On(kClock)) {
    uint64_t cb;
    if (rex_audio_worker_callback(&cb)) {  // the SDK audio worker's callback: time from its callback number
      ctx.r3.u64 = doa4::np::AudioTickCount(cb);
      return;
    }
    if (IsMainThread()) SpinRead(ctx, base, uint32_t(ctx.lr));
    ctx.r3.u64 = doa4::np::VirtualTickCount(&t_clock, Frame());
    TraceRead(kRdTick, uint32_t(ctx.lr));
    return;
  }
  __imp__sub_82790138(ctx, base);
}

// QueryPerformanceCounter: *(u64*)r3 = mftb; returns 1.
REX_HOOK_RAW(sub_82792660) {
  if (doa4::np::Active() && On(kClock)) {
    uint64_t cb;
    if (rex_audio_worker_callback(&cb)) {  // the SDK audio worker's callback: time from its callback number
      rex::memory::store_and_swap<uint64_t>(base + ctx.r3.u32, doa4::np::AudioTimebase(cb));
      ctx.r3.u64 = 1;
      return;
    }
    rex::memory::store_and_swap<uint64_t>(base + ctx.r3.u32, doa4::np::VirtualTimebase(&t_clock, Frame()));
    TraceRead(kRdQpc, uint32_t(ctx.lr));
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82792660(ctx, base);
}

// KeQuerySystemTime(FILETIME* r3).
REX_HOOK_RAW(sub_82790130) {
  if (doa4::np::Active() && On(kClock) && ctx.r3.u32) {
    uint64_t cb;
    if (rex_audio_worker_callback(&cb)) {  // the SDK audio worker's callback: time from its callback number
      rex::memory::store_and_swap<uint64_t>(base + ctx.r3.u32, doa4::np::AudioFileTime(cb));
      return;
    }
    rex::memory::store_and_swap<uint64_t>(base + ctx.r3.u32, doa4::np::VirtualFileTime(&t_clock, Frame()));
    TraceRead(kRdFileTime, uint32_t(ctx.lr));
    return;
  }
  __imp__sub_82790130(ctx, base);
}

// sub_82793B68(r3, stack size r4, start r5, parameter r6, flags r7, processor r8, thread id out r9):
// the game's CreateThread path (sub_82791B40 -> here -> ExCreateThread, phase0_facts.md 5); returns
// the new thread's handle, or 0. D3D creates its own thread with ExCreateThread directly (not here).
REX_HOOK_RAW(sub_82793B68) {
  const uint32_t start = ctx.r5.u32;
  __imp__sub_82793B68(ctx, base);
  if (!doa4::np::Active()) return;
  const uint32_t handle = ctx.r3.u32;
  uint32_t tid = 0;
  if (auto* ks = rex::system::kernel_state(); ks && handle) {
    if (auto thread = ks->object_table()->LookupObject<rex::system::XThread>(handle)) tid = thread->thread_id();
  }
  doa4::np::LogLine(fmt::format("thread created: start {:08X} id {} (frame {})", start, tid, Frame()));
  ThreadCreated(start, tid);
}

// sub_8275AD70(device r3): the D3D runtime's flush before a swap; the kick thread's keep-alive re-present calls it
// first (keepalive fix). sub_8275AEE0, the second call, is hooked in doa4_photo_hook.cpp (SkipKeepAliveSwap).
REX_HOOK_RAW(sub_8275AD70) {
  if (KeepAliveSkip(doa4::np::KeepAliveFn::kAd70, uint32_t(ctx.lr))) return;
  __imp__sub_8275AD70(ctx, base);
}

// sub_82731790: starts the save manager's one-shot file thread; returns 1, or -1 if it could not (reader fix).
REX_HOOK_RAW(sub_82731790) {
  __imp__sub_82731790(ctx, base);
  if (doa4::np::Active() && On(kReader)) ReaderStarted(ctx.r3.s32, base);
}
