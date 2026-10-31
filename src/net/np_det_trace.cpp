// DOA4 netplay phase 0: the determinism layer's diagnostics (DOA4_NP_TRACE).
//
// DOA4_NP_TRACE contains "clock": <dir>/clock_trace.txt gets one line per frame
// with the main thread's clock reads by kind and caller, the other threads'
// read counts, and per-caller call counts of a few functions the hunt needed
// (the sound polls sub_824026E8 / sub_82401CD8, the scene RNG's five draw
// functions, the job workers' wakes). A count that differs between two replays
// of one recording is a loop or a thread whose work leaks host timing; diff the
// two files to find the first such frame. The thread census (entry point and
// current LR of every guest thread) is logged at frames 300 and 3000 in every
// session.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xthread.h>

#include "generated/default/dead_or_alive_4_init.h"

#include "np_det_internal.h"
#include "np_session.h"

namespace doa4::np::det {
namespace {

constexpr const char* kReadNames[] = {"qpc",  "tick", "ft",         "sdktb", "sdkft",
                                      "call24026E8", "call2401CD8", "mt8357B5C0", "wake",
                                      "call26868A8", "call2686B10"};
static_assert(sizeof(kReadNames) / sizeof(kReadNames[0]) == kRdKinds);

FILE* g_trace = nullptr;
std::mutex g_mu;
std::map<std::pair<int, uint32_t>, uint32_t> g_main;   // main thread: (kind, caller) -> count
std::map<std::pair<int, uint32_t>, uint32_t> g_other;  // other threads, call kinds: (kind, caller) -> count
uint32_t g_other_reads[kRdKinds] = {};

void LogThreads(uint32_t frame) {
  auto* ks = rex::system::kernel_state();
  auto* table = ks ? ks->object_table() : nullptr;
  if (!table) return;
  std::string line = "threads at frame " + std::to_string(frame) + ":";
  for (auto& t : table->GetObjectsByType<rex::system::XThread>()) {
    if (!t) continue;
    auto* ts = t->thread_state();
    const uint32_t lr = ts && ts->context() ? uint32_t(ts->context()->lr) : 0;
    line += fmt::format(" [{} start {:08X} lr {:08X}{}{}]", t->thread_id(),
                        t->creation_params() ? uint32_t(t->creation_params()->start_address) : 0, lr,
                        t->is_guest_thread() ? "" : " host", t->main_thread() ? " main" : "");
  }
  LogLine(line);
}

}  // namespace

void TraceStart(const std::filesystem::path& dir) {
  const char* t = TestSwitch("DOA4_NP_TRACE");
  if (t && std::string_view(t).find("clock") != std::string_view::npos) {
    g_trace = _wfopen((dir / "clock_trace.txt").wstring().c_str(), L"w");
  }
}

bool Tracing() { return g_trace != nullptr; }

void TraceRead(int kind, uint32_t lr) {
  if (!g_trace) return;
  // A plain host thread (no XThread; GetCurrentThread asserts there) counts as "other".
  rex::system::XThread* t =
      rex::system::XThread::IsInThread() ? rex::system::XThread::GetCurrentThread() : nullptr;
  std::lock_guard lk(g_mu);
  if (t && t->main_thread()) {
    g_main[{kind, lr}]++;
  } else {
    g_other_reads[kind]++;
    if (kind >= kRdCall0) g_other[{kind, lr}]++;
  }
}

void TraceFrame(uint32_t frame) {
  if (frame == 300 || frame == 3000) LogThreads(frame);
  if (!g_trace || frame == 0) return;
  std::lock_guard lk(g_mu);
  std::string line = "f " + std::to_string(frame - 1) + ":";
  for (const auto& [key, n] : g_main) line += fmt::format(" {}:{:08X}x{}", kReadNames[key.first], key.second, n);
  line += " | other";
  for (int k = 0; k < kRdKinds; ++k) line += fmt::format(" {}={}", kReadNames[k], g_other_reads[k]);
  for (const auto& [key, n] : g_other) line += fmt::format(" {}:{:08X}x{}", kReadNames[key.first], key.second, n);
  std::fprintf(g_trace, "%s\n", line.c_str());
  g_main.clear();
  g_other.clear();
  for (uint32_t& n : g_other_reads) n = 0;
}

}  // namespace doa4::np::det

using namespace doa4::np::det;

// Call counts (trace only): the sound polls that spun at boot and frame 12, and
// the scene RNG's five draw functions.
extern "C" {
REX_EXTERN(__imp__sub_824026E8);
REX_EXTERN(__imp__sub_82401CD8);
REX_EXTERN(__imp__sub_8276B5F0);
REX_EXTERN(__imp__sub_8276B6A8);
REX_EXTERN(__imp__sub_8276B770);
REX_EXTERN(__imp__sub_8276B818);
REX_EXTERN(__imp__sub_8276B8F8);
REX_EXTERN(__imp__sub_826868A8);
REX_EXTERN(__imp__sub_82686B10);
}

#define NP_TRACE_CALL(fn, kind)          \
  REX_HOOK_RAW(fn) {                     \
    TraceRead(kind, uint32_t(ctx.lr));   \
    __imp__##fn(ctx, base);              \
  }
NP_TRACE_CALL(sub_824026E8, kRdCall0)
NP_TRACE_CALL(sub_82401CD8, kRdCall1)
NP_TRACE_CALL(sub_8276B5F0, kRdMt)
NP_TRACE_CALL(sub_8276B6A8, kRdMt)
NP_TRACE_CALL(sub_8276B770, kRdMt)
NP_TRACE_CALL(sub_8276B818, kRdMt)
NP_TRACE_CALL(sub_8276B8F8, kRdMt)
NP_TRACE_CALL(sub_826868A8, kRdCall2)
NP_TRACE_CALL(sub_82686B10, kRdCall3)
