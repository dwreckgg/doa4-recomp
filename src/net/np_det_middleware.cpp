// DOA4 netplay phase 0: the determinism layer's middleware fixes (sound and
// movie middleware, the game's loader thread, the audio callbacks). Each is a
// thread whose progress the game polls; in a session that progress happens at
// points fixed by the game's own calls instead of at host-timed points.
//   audio   the guest audio callbacks run within a per-frame budget of
//           800 samples (3 or 4 callbacks of 256) granted at the frame hook,
//           from boot (Tekken 6 rex_audio_lockstep / rex_audio_advance).
//   movie   movies are not played (sub_8242A168 returns at once, "finished"
//           answers yes): CRI Sofdec playback ran on the middleware's threads in
//           host time, so a movie took a different number of frames per run.
//   loader  the loader thread's jobs run only while the frame hook holds a gate
//           open, so a job queued in frame f is done when frame f + 1 starts.
//   cri     the CRI middleware's three server threads no longer run their server
//           passes; the main thread runs one pass of the three groups at every
//           frame boundary and whenever it spins on the tick count in a frame.
//   crijob  jobs posted to the middleware's two job threads run inline on the
//           posting thread.

#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>

#include "generated/default/dead_or_alive_4_init.h"

#include "np_det_internal.h"
#include "np_session.h"

extern "C" {
REX_EXTERN(__imp__sub_8242A168);
REX_EXTERN(__imp__sub_8242A3A8);
REX_EXTERN(__imp__sub_8242A0F0);
REX_EXTERN(__imp__sub_8277EFC8);
REX_EXTERN(__imp__sub_829BC7F0);
REX_EXTERN(__imp__sub_829BF498);
REX_EXTERN(__imp__sub_829C9958);
// SDK netplay exports (sdk-np0 src/audio/audio_system.cpp).
__declspec(dllimport) void rex_audio_lockstep(int on);
__declspec(dllimport) int rex_audio_advance(uint32_t n, int timeout_ms);
}

namespace doa4::np::det {
namespace {

// ---- audio ----
bool g_audio_alive = false;
uint32_t g_audio_late = 0;

void AudioStep(uint32_t f) {
  const uint32_t n = uint32_t((uint64_t(f) + 1) * 800 / 256 - uint64_t(f) * 800 / 256);
  if (rex_audio_advance(n, g_audio_alive ? 250 : 2)) {
    if (!g_audio_alive) LogLine("audio: first callbacks in the lockstep at frame " + std::to_string(f));
    g_audio_alive = true;
  } else if (!g_audio_alive) {
    rex_audio_lockstep(1);  // no audio client yet: drop the budget again
  } else {
    Count(kFbAudioLate);
    if (g_audio_late++ < 20) LogLine("audio: frame " + std::to_string(f) + ": callbacks not done after 250 ms");
  }
}

// ---- loader ----
// The loader thread (entry 0x823FC5A0, started by the main loop's init
// sub_824F4560) busy-polls a 16-entry ring at 0x830ADB0C (12 bytes each, state
// byte at +0: 0 free, 1 queued, 2 running, 3 done; index 0x830ADB08, critical
// section 0x830ADBCC) and runs each job through sub_8277EFC8; the main thread
// checks for state 3 (sub_823FC700).
constexpr uint32_t kLoaderRing = 0x830ADB0C;
constexpr uint32_t kLoaderThreadStart = 0x823FC5A0;
std::mutex g_loader_mu;
std::condition_variable g_loader_cv;
bool g_loader_open = false;
int g_loader_busy = 0;
uint32_t g_loader_jobs = 0, g_loader_timeouts = 0, g_loader_outside = 0;
double g_loader_wait_ms = 0, g_loader_wait_max = 0;

bool LoaderPending(const uint8_t* base) {
  for (uint32_t i = 0; i < 16; ++i) {
    const uint8_t s = base[kLoaderRing + 12 * i];
    if (s == 1 || s == 2) return true;
  }
  return false;
}

void LoaderBoundary(uint8_t* base, uint32_t frame) {
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_lock lk(g_loader_mu);
  g_loader_open = true;
  g_loader_cv.notify_all();
  bool waited = false;
  while (g_loader_busy > 0 || LoaderPending(base)) {
    waited = true;
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5)) {
      Count(kFbLoaderIdle);
      if (g_loader_timeouts++ < 10) LogLine("loader: frame " + std::to_string(frame) + ": not idle after 5 s");
      break;
    }
    g_loader_cv.wait_for(lk, std::chrono::milliseconds(1));
  }
  g_loader_open = false;
  if (waited) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_loader_wait_ms += ms;
    g_loader_wait_max = std::max(g_loader_wait_max, ms);
  }
  if (frame % 600 == 599 && g_loader_jobs) {
    LogLine(fmt::format("loader: {} jobs in the last 600 frames, boundary wait {:.1f} ms (max {:.1f} ms)",
                        g_loader_jobs, g_loader_wait_ms, g_loader_wait_max));
    g_loader_jobs = 0;
    g_loader_wait_ms = g_loader_wait_max = 0;
  }
}

// ---- cri ----
// The sound/movie middleware runs three server threads. B (entry 0x829BC8F8)
// and C (0x829BC980) loop: sub_829BC7F0 sleeps to the next 1/60 s boundary of
// the performance counter, then they run their server group through
// sub_829BF498 (B group 2, C group 4); B then resumes A (0x829BC458), which runs
// group 6 and suspends itself. The passes ran on host time, so the sound and
// load state the game polls changed at different frames from run to run (a
// stop-and-wait loop, sub_82401F70, spun 750-886 times in frame 12; the boot's
// load loops 7-112 times). In a session A, B and C still loop but their passes
// do nothing; the main thread runs groups 2, 6, 4 at every frame boundary and
// once more for every repeated tick-count read from one call site in a frame
// (the game's wait loops read it for their timeouts), so each iteration of such
// a loop sees the state after one more pass.
constexpr uint32_t kCriThreadA = 0x829BC458, kCriThreadB = 0x829BC8F8, kCriThreadC = 0x829BC980;
std::atomic<bool> g_cri_ready{false};  // thread B reached its loop: the middleware is set up
thread_local bool t_on_main_pass = false;
uint32_t g_spin_steps_frame = 0, g_spin_steps_window = 0, g_spin_steps_max = 0;
std::vector<std::pair<uint32_t, uint32_t>> g_spin_lrs;  // main thread: tick-read LR -> reads this frame

void CriPass(PPCContext& ctx, uint8_t* base) {
  if (!g_cri_ready.load(std::memory_order_acquire)) return;
  Count(kPrCriPasses);
  const PPCContext saved = ctx;  // a call inside a hook: every register as it was
  const unsigned csr = _mm_getcsr();
  t_on_main_pass = true;
  for (uint32_t group : {2u, 6u, 4u}) {
    ctx.r3.u64 = group;
    __imp__sub_829BF498(ctx, base);
  }
  t_on_main_pass = false;
  ctx = saved;
  _mm_setcsr(csr);  // the cached flush mode in ctx must match the hardware again
}

// Hunt instrument (DOA4_NP_WATCH=<hex address>,<hex length>): logs which step
// of the frame boundary changes the watched bytes (none should: the boundary
// runs no game code except the middleware passes).
struct Watch {
  uint32_t lo = 0, len = 0;
  std::vector<uint8_t> copy;
  uint32_t logged = 0;
  void Init() {
    const char* e = std::getenv("DOA4_NP_WATCH");
    if (!e) return;
    lo = uint32_t(std::strtoul(e, nullptr, 16));
    const char* comma = std::strchr(e, ',');
    len = comma ? uint32_t(std::strtoul(comma + 1, nullptr, 16)) : 0;
  }
  void Check(uint8_t* base, uint32_t frame, const char* step) {
    if (!len) return;
    const uint8_t* p = base + lo;
    if (step && !copy.empty() && std::memcmp(copy.data(), p, len) != 0 && logged < 40) {
      ++logged;
      uint32_t at = 0;
      while (at < len && copy[at] == p[at]) ++at;
      LogLine(fmt::format("watch: {:08X} changed {} (frame {}, first byte +{:X})", lo, step, frame, at));
    }
    copy.assign(p, p + len);
  }
};
Watch g_watch;

}  // namespace

void MiddlewareStart() {
  g_watch.Init();
  if (On(kAudio)) rex_audio_lockstep(1);
}

// The loader gate's waiter (the sub_8277EFC8 hook, g_loader_cv.wait_for with the predicate
// g_loader_open || Stopped()) is woken here. Taking the mutex first closes the window in which a job
// has tested the predicate (false) but not yet blocked: it holds g_loader_mu until it blocks, so the
// notify below cannot be lost.
void MiddlewareStop() {
  { std::lock_guard lk(g_loader_mu); }
  g_loader_cv.notify_all();
}

void MiddlewareFrame(uint32_t frame, PPCContext& ctx, uint8_t* base) {
  g_watch.Check(base, frame, nullptr);  // the frame's own changes: not reported
  if (On(kLoader)) LoaderBoundary(base, frame);
  g_watch.Check(base, frame, "in the loader boundary");
  g_spin_lrs.clear();
  if (On(kCri)) {
    if (frame % 600 == 599 && g_spin_steps_window) {
      LogLine(fmt::format("cri: {} spin steps in the last 600 frames (most in one frame {})",
                          g_spin_steps_window, g_spin_steps_max));
      g_spin_steps_window = g_spin_steps_max = 0;
    }
    g_spin_steps_frame = 0;
    CriPass(ctx, base);
  }
  g_watch.Check(base, frame, "in the CRI pass");
  if (On(kAudio)) AudioStep(frame);
  g_watch.Check(base, frame, "in the audio step");
}

// The trigger (a second tick-count read from one call site on the main thread in a frame) is a function
// of the main thread's own call sequence, which lockstep keeps equal; the passes it adds are counted
// (pr_spin_steps) and compared every frame.
void SpinRead(PPCContext& ctx, uint8_t* base, uint32_t lr) {
  if (!On(kCri) || t_on_main_pass) return;  // a tick read inside a pass is not a wait loop of the frame
  for (auto& [site, n] : g_spin_lrs) {
    if (site == lr) {
      ++n;
      if (g_spin_steps_frame < 20000) {
        Count(kPrSpinSteps);
        ++g_spin_steps_frame;
        ++g_spin_steps_window;
        g_spin_steps_max = std::max(g_spin_steps_max, g_spin_steps_frame);
        CriPass(ctx, base);
      } else {
        Count(kFbSpinCap);
      }
      return;
    }
  }
  g_spin_lrs.emplace_back(lr, 1);
}

}  // namespace doa4::np::det

using namespace doa4::np::det;

// ---- movie ----
// sub_8242A168(r3 = movie index) starts a CRI Sofdec movie, sub_8242A3A8 says
// whether it has finished (CRI status 0/3/4) and sub_8242A0F0 stops it. Movie 18
// is the boot's opening (frames 20-1000).
namespace {
bool MoviesSkipped() { return doa4::np::Active() && On(kMovie); }
}  // namespace

REX_HOOK_RAW(sub_8242A168) {
  if (MoviesSkipped()) {
    doa4::np::LogLine(fmt::format("movie {} skipped at frame {} (lr {:08X})", ctx.r3.u32, Frame(),
                                  uint32_t(ctx.lr)));
    return;
  }
  __imp__sub_8242A168(ctx, base);
}

// In a session every movie is skipped, so every "finished?" is yes and "stop" has nothing to stop.
REX_HOOK_RAW(sub_8242A3A8) {
  if (MoviesSkipped()) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8242A3A8(ctx, base);
}

REX_HOOK_RAW(sub_8242A0F0) {
  if (MoviesSkipped()) return;
  __imp__sub_8242A0F0(ctx, base);
}

// ---- loader ----
// sub_8277EFC8(r3, r4): the loader's job (also called directly by other code,
// which is never gated).
REX_HOOK_RAW(sub_8277EFC8) {
  if (!doa4::np::Active() || !On(kLoader) || ThreadStart() != kLoaderThreadStart) {
    __imp__sub_8277EFC8(ctx, base);
    return;
  }
  {
    std::unique_lock lk(g_loader_mu);
    if (!g_loader_cv.wait_for(lk, std::chrono::milliseconds(500), [] { return g_loader_open || Stopped(); })) {
      // The main thread did not reach a boundary in time (it may be waiting for
      // this job inside a frame): run anyway, and say so.
      Count(kFbLoaderOutside);
      if (g_loader_outside++ < 10) {
        doa4::np::LogLine("loader: a job ran outside the frame boundary (frame " + std::to_string(Frame()) + ")");
      }
    }
    ++g_loader_busy;
    ++g_loader_jobs;
    Count(kPrLoaderJobs);
  }
  __imp__sub_8277EFC8(ctx, base);
  {
    std::lock_guard lk(g_loader_mu);
    --g_loader_busy;
  }
  g_loader_cv.notify_all();
}

// ---- cri ----
// sub_829BC7F0: the server threads' sleep to the next 1/60 s; marks the
// middleware ready (thread B is in its loop).
REX_HOOK_RAW(sub_829BC7F0) {
  if (doa4::np::Active() && On(kCri) && ThreadStart() == kCriThreadB && !g_cri_ready.exchange(true)) {
    doa4::np::LogLine("cri: server threads running at frame " + std::to_string(Frame()));
  }
  __imp__sub_829BC7F0(ctx, base);
}

// sub_829BF498(group): one pass of a CRI server group. In a session the server
// threads' own calls do nothing; the main thread runs the passes (CriPass).
REX_HOOK_RAW(sub_829BF498) {
  if (doa4::np::Active() && On(kCri) && !t_on_main_pass) {
    const uint32_t start = ThreadStart();
    if (start == kCriThreadA || start == kCriThreadB || start == kCriThreadC) {
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_829BF498(ctx, base);
}

// ---- crijob ----
// sub_829C9958(job thread object r3, function r4, argument r5): posts a job to
// one of the middleware's two job threads (entry 0x829C96B0, event-woken): if
// the thread is idle it stores the job, marks it busy (+4), bumps the job
// counter (+60) and signals the thread, which runs fn(arg) and clears busy.
// Loads waited on those jobs at host-timed points. In a session the job runs
// inline on the posting thread before the post returns (busy is never set), so
// everyone who polls the object sees it finished at a point fixed by the game.
REX_HOOK_RAW(sub_829C9958) {
  const uint32_t obj = ctx.r3.u32;
  if (!doa4::np::Active() || !On(kCriJob) || !obj ||
      rex::memory::load_and_swap<uint32_t>(base + obj + 0) == 0 ||
      rex::memory::load_and_swap<uint32_t>(base + obj + 4) != 0) {
    __imp__sub_829C9958(ctx, base);  // error paths and a busy thread: as the game does
    return;
  }
  const uint32_t fn = ctx.r4.u32, arg = ctx.r5.u32;
  uint32_t id = rex::memory::load_and_swap<uint32_t>(base + obj + 60) + 1;
  if (id == 0x7FFFFFFFu) id = 1;
  rex::memory::store_and_swap<uint32_t>(base + obj + 60, id);
  if (fn) {
    const PPCContext saved = ctx;
    const unsigned csr = _mm_getcsr();
    ctx.r3.u64 = arg;
    REX_CALL_INDIRECT_FUNC(fn);
    ctx = saved;
    _mm_setcsr(csr);
  }
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 4) {
    doa4::np::LogLine(fmt::format("crijob: job {:08X}({:08X}) of {:08X} ran inline (frame {})", fn, arg, obj,
                                  Frame()));
  }
  Count(kPrCriJobs);
  ctx.r3.u64 = id;
}
