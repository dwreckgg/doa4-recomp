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
//   xam       XAM's deferred overlapped completions (message boxes, content,
//             enumerators) complete inline with no host delay, and XAM dialogs
//             answer with their default button (headless).
//   audio, movie, loader, cri, crijob: np_det_middleware.cpp.
//   workers:  np_det_workers.cpp.
// DOA4_NP_FIX: "all" (the default), "none", a comma list, or "all-x-y" (all but
// x and y).

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

extern "C" {
REX_EXTERN(__imp__sub_82334C60);
REX_EXTERN(__imp__sub_82790138);
REX_EXTERN(__imp__sub_82792660);
REX_EXTERN(__imp__sub_82790130);
REX_EXTERN(__imp__sub_82793B68);
// SDK netplay exports (sdk-np0 src/system/kernel_state.cpp, src/audio/audio_system.cpp).
__declspec(dllimport) void rex_overlapped_inline(int on);
__declspec(dllimport) void rex_audio_lockstep(int on);
}

namespace doa4::np::det {
namespace {

struct FixName {
  const char* name;
  uint32_t bit;
};
constexpr FixName kFixNames[] = {
    {"seed", kSeed},     {"clock", kClock},   {"sdkclock", kSdkClock}, {"xam", kXam},
    {"audio", kAudio},   {"movie", kMovie},   {"loader", kLoader},     {"cri", kCri},
    {"crijob", kCriJob}, {"workers", kWorkers}};

std::atomic<uint32_t> g_fixes{0};
std::atomic<uint32_t> g_frame{0};
std::atomic<bool> g_stopped{false};
std::string g_headless_before = "false";  // the headless cvar as Start found it; Stop puts it back
uint64_t g_seed = 1;
SeedSeries g_seeds;  // seedings so far per MT in the session (np_core.h; Start resets it)
std::atomic<uint32_t> g_counters[kCounterCount];
constexpr const char* kCounterNames[] = {
    "fb_worker_token", "fb_main_wait",  "fb_unranked",    "fb_loader_outside", "fb_loader_idle",
    "fb_audio_late",   "fb_spin_cap",   "fb_cri_ready",   "fb_audio_start",    "pr_cri_passes",
    "pr_spin_steps",   "pr_loader_jobs", "pr_cri_jobs",   "pr_reseeds",        "pr_notify"};
static_assert(std::size(kCounterNames) == kCounterCount, "one name per Counter, in enum order");

uint32_t ParseFixes(const char* text) {
  if (!text || !*text || std::string_view(text) == "all") return ~0u;
  if (std::string_view(text) == "none") return 0;
  std::string_view s(text);
  uint32_t bits = 0;
  char sep = ',';
  bool minus = false;
  if (s.substr(0, 4) == "all-") {  // "all-x-y": everything but x and y
    bits = ~0u;
    s.remove_prefix(4);
    sep = '-';
    minus = true;
  }
  while (!s.empty()) {
    const size_t at = s.find(sep);
    const std::string_view item = s.substr(0, at);
    for (const FixName& f : kFixNames) {
      if (item == f.name) bits = minus ? (bits & ~f.bit) : (bits | f.bit);
    }
    if (at == std::string_view::npos) break;
    s.remove_prefix(at + 1);
  }
  return bits;
}

// ---- Virtual clocks ----
// The formulas and the never-backwards rule are in np_core.h; each thread keeps its own clock.
thread_local ThreadClock t_clock;

// The SDK's guest-time override: guest threads get the virtual clocks; host
// threads (GPU vblank, audio worker, kernel dispatch) keep the host clock.
bool GuestTime(int kind, uint64_t* value) {
  if (!On(kSdkClock)) return false;
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

}  // namespace

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

void ThreadCreated(uint32_t start, uint32_t tid) { WorkerCreated(start, tid); }

void Start(uint64_t seed, const std::filesystem::path& dir) {
  g_seed = seed;
  g_seeds.Reset();  // the series counts the seedings of this session
  TraceStart(dir);
  const uint32_t fixes = ParseFixes(std::getenv("DOA4_NP_FIX"));
  g_fixes.store(fixes, std::memory_order_release);
  std::string names;
  for (const FixName& f : kFixNames) {
    if (fixes & f.bit) names += std::string(names.empty() ? "" : ",") + f.name;
  }
  LogLine("determinism fixes: " + (names.empty() ? std::string("none") : names));
  if (fixes & kSdkClock) rex::chrono::Clock::set_guest_time_override(&GuestTime);
  g_headless_before = rex::cvar::GetFlagByName("headless");
  if (fixes & kXam) {
    rex_overlapped_inline(1);
    rex::cvar::SetFlagByName("headless", "true");
  }
  MiddlewareStart();
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
  const uint32_t fixes = g_fixes.load(std::memory_order_acquire);
  if (fixes & kAudio) rex_audio_lockstep(0);  // audio and XMA lockstep off (sdk-np0 audio_system.cpp:420)
  if (fixes & kXam) {
    rex_overlapped_inline(0);
    rex::cvar::SetFlagByName("headless", g_headless_before);
  }
  if (fixes & kSdkClock) rex::chrono::Clock::set_guest_time_override(nullptr);
  const auto state = [&](uint32_t bit) { return (fixes & bit) ? "released" : "never held"; };
  LogLine(fmt::format(
      "determinism layer off: lockstep audio {}, inline completions and headless {} (headless is now '{}', was '{}'), "
      "clock override {}",
      state(kAudio), state(kXam), rex::cvar::GetFlagByName("headless"), g_headless_before, state(kSdkClock)));
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
