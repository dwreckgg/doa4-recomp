// DOA4 netplay phase 0: the record / replay / hash session; phase 1 (Task 12): the online mode. See np_session.h.
//
// Hooks (weak-alias overrides of the game's functions, phase0_facts.md 1-2):
//   sub_824F4498  pre-wrap = the frame hook: its only caller is the main-loop
//                 head loc_824F41F4, so it runs once per loop pass, before the
//                 GPU begin, the pad poll, the update/draw and the present.
//   sub_82792040  the game's only XamInputGetState path (users 0-3): in a
//                 session it returns the staged input of the frame.
//   sub_82792038  XamInputGetCapabilities (called on connection changes).
// Outside a session every hook calls the original at once.

#include "np_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <xmmintrin.h>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>

#include "generated/default/dead_or_alive_4_init.h"

#include "../doa4_gamefiles.h"
#include "../doa4_photo_win.h"
#include "../doa4_tweaks.h"
#include "../doa4_unlocks.h"
#include "np_core.h"
#include "np_det.h"
#include "np_det_internal.h"
#include "np_exec.h"
#include "np_restart.h"
#include "np_gekko.h"
#include "np_rules.h"
#include "np_selftest.h"
#include "np_transport.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Task 1 review M1 (carry 2l): the dot-product self-test is run-time evidence and depends on the CPU; this is the
// compile-time half. The SDK's canonical-NaN redirect (rex/ppc/dot_product.h, reached through the generated init
// header above) must be in effect in this translation unit, as in every generated one: the raw-product macro it
// defines exists, and simde_mm_dp_ps expands to the canonicalising call.
#ifndef REX_PPC_RAW_DOT_PRODUCT_PS
#error "the SDK's canonical-NaN dot-product redirect (rex/ppc/dot_product.h) is not in effect: netplay needs it"
#endif
#define DOA4_NP_STR2(x) #x
#define DOA4_NP_STR(x) DOA4_NP_STR2(x)
static_assert(std::string_view(DOA4_NP_STR(simde_mm_dp_ps(a, b, 0xFF))).find("CanonicalizeDotProductNaN") !=
                  std::string_view::npos,
              "simde_mm_dp_ps is not the SDK's canonical-NaN redirect here: netplay needs it");

extern "C" {
REX_EXTERN(__imp__sub_824F4498);
REX_EXTERN(__imp__sub_82792040);
REX_EXTERN(__imp__sub_82792038);
}

namespace doa4::np {
namespace {

constexpr uint32_t kDataLo = 0x82B70000;  // XEX .data/.bss (phase0_facts.md 10)
constexpr uint32_t kDataHi = 0x835B0000;  // rounded up to the 64 KB block
constexpr uint32_t kBlock = 0x10000;
constexpr uint32_t kNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
constexpr uint32_t kMtState = 0x833F7600;  // DOA4_NP_FORCE_DESYNC flips a byte of this MT state (Task 10 switch)

enum class Mode { kOff, kRecord, kReplay, kOnline };

struct Job {
  enum Kind { kShot, kDump } kind;
  uint32_t frame;
  std::vector<uint8_t> bytes;  // kDump
};

// One packet of the game thread's GekkoNet traffic (carry 2f): the ring is dumped on every reported hold and every
// rx silence of 50 ms or more.
struct PacketNote {
  bool tx = false;
  double t_us = 0;
  int32_t frame = 0;
  uint32_t size = 0;
  uint8_t head[16] = {};
};

struct State {
  Mode mode = Mode::kOff;
  Host host;
  std::filesystem::path dir;
  std::filesystem::path log_path;
  FILE* log = nullptr;
  std::mutex log_mu;

  // input
  InputQueue queue;
  Recording replay;
  FrameInput staged;
  FILE* rec = nullptr;
  uint64_t seed = 1;

  // frames
  uint32_t frame = 0;  // frames begun so far: the frame being staged is frame - 1
  uint32_t quit_frame = UINT32_MAX;
  bool quitting = false;
  std::vector<uint32_t> shot_frames;
  uint32_t shot_every = 0;
  std::vector<uint32_t> dump_frames;

  // hashes
  std::vector<Region> regions;
  FILE* digest = nullptr;
  FILE* sweep = nullptr;
  FILE* counters = nullptr;
  std::vector<uint32_t> counter_row;
  uint32_t sweep_per_frame = 4;
  std::vector<uint8_t> scratch;
  // Task 12: the digest row and the sweep row are two members, filled every session frame (the online checksum folds
  // both); `sweep_first` is the sweep row's first block.
  std::vector<uint64_t> digest_row;
  std::vector<uint64_t> sweep_row;
  uint32_t sweep_first = 0;
  // Per sweep block k: its address range and the kSweepMasks inside it, built once in Start so that
  // hashing a block never allocates.
  std::vector<Region> block_regions;
  // Per sweep block: 1 once its first hash with SDK handle stash words zeroed was logged (sized in Start).
  std::vector<uint8_t> stash_logged;
  bool timing_logged = false;  // HoldFrameTiming's one log line

  // timing (QPC ticks)
  double qpc_us = 0;
  int64_t last_frame_qpc = 0;
  double sum_hook_us = 0, max_hook_us = 0, sum_hash_us = 0, max_hash_us = 0, sum_period_us = 0;
  double max_period_us = 0;
  double sum_det_us = 0, max_det_us = 0;
  double sum_net_us = 0, max_net_us = 0, sum_wait_us = 0, max_wait_us = 0, sum_pacer_us = 0, sum_ahead = 0;
  uint32_t window = 0;

  // writer thread
  std::thread writer;
  std::mutex job_mu;
  std::condition_variable job_cv;
  std::deque<Job> jobs;
  bool stop = false;

  // ---- replay parity (R21) and the unlock ----
  bool unlock = false;                  // online, or a replay of a recording with unlock=1
  std::string guest_meta = "(none)";    // the guest text applied (newlines as ';'), "(none)" when none
  std::string unlock_meta = "(none)";   // "1" / "0" / "(none)"
  uint32_t unlocks_applied = 0;
  bool unlock_seen_logged = false;

  // ---- online ----
  handoff::Handoff ho;
  int slot = 0;
  bool local_script = false;
  std::unique_ptr<gekko::Session> gk;
  std::unique_ptr<exec::Executor> ex;
  std::unique_ptr<live::Session> live;
  std::atomic<live::Session*> live_ptr{nullptr};
  bool ended = false;
  std::atomic<bool> running{false};      // the online session runs (until its end)
  std::atomic<int32_t> session_frame{-1};
  std::atomic<int64_t> last_hook_ms{0};  // steady ms of the last frame hook's start
  std::atomic<bool> in_hold{false};      // inside live::Session::Frame
  std::atomic<bool> refused{false};      // carry 2g: the peer answered as a lobby
  std::atomic<uint64_t> pending_peer_left{UINT64_MAX};  // (reason << 32) | frame, before the live session exists
  std::atomic<int32_t> pending_leave{INT32_MAX};
  FILE* events = nullptr;
  std::mutex events_mu;
  double unix_offset_ms = 0;  // unix ms = Now() / 1000 + offset
  uint64_t hold_begin_us = 0;
  std::atomic<bool> banner_shown{false};  // read by the UI thread (BannerWaiting)
  HANDLE timer = nullptr;      // a high-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), game thread
  std::mutex result_mu;
  live::FrameResult last_result;
  std::mutex timing_mu;
  NetTiming timing;
  uint32_t mxcsr0 = 0;
  uint32_t mxcsr_logged = 0;
  double last_hook_end_us = 0;
  uint32_t away_logged = 0;
  // the packet ring and the rx / tx silence watch (game thread)
  PacketNote ring[64];
  uint32_t ring_next = 0;
  double last_rx_us = 0, last_tx_us = 0;
  uint32_t silence_logs = 0, ring_dumps = 0;
  // the end and the threads of the online mode
  std::thread watch;
  std::thread restarter;
  std::mutex watch_mu;
  std::condition_variable watch_cv;
  bool watch_stop = false;

  // A game-requested exit() (HalReturnToFirmware, "return to the dashboard": sdk xboxkrnl_hal.cpp:34)
  // runs the static destructors without OnShutdown, so the writer may still be joinable here, and a
  // joinable std::thread's destructor calls std::terminate. Detach it: the process is going away.
  ~State() {
    if (writer.joinable()) writer.detach();
    if (watch.joinable()) watch.detach();
    if (restarter.joinable()) restarter.detach();
    live_ptr.store(nullptr);
    // never destroyed at exit: they would touch GekkoNet and the transport, both going away
    (void)live.release();
    (void)gk.release();
    (void)ex.release();
  }
};

State& S() {
  static State s;
  return s;
}

std::atomic<bool> g_active{false};
std::atomic<bool> g_switch_online{false};
std::atomic<bool> g_switch_test_dir{false};
std::mutex g_early_mu;
std::vector<std::string> g_early;  // EarlyLog lines, written into np_log.txt when it opens
std::string g_skipped;             // fix round M1: guest settings skipped as unregistered (comma list; meta key)

void Log(const std::string& line) {
  State& s = S();
  std::lock_guard lk(s.log_mu);
  if (s.log) {
    std::fprintf(s.log, "%s\n", line.c_str());
    std::fflush(s.log);
  }
  REXLOG_INFO("[doa4][NP] {}", line);
}

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

// R22: a phase-0 test switch ("" in a session without a test dir).
std::string Switch(const char* name) {
  const char* v = TestSwitch(name);
  return v ? std::string(v) : std::string();
}

// A Task 10 test switch (read only with DOA4_NP_TEST_DIR set, R11), or a phase-0 one that also applies to a session.
std::string TestDirEnv(const char* name) { return g_switch_test_dir.load() ? Env(name) : std::string(); }

std::vector<uint32_t> FrameList(const std::string& text) {
  std::vector<uint32_t> out;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(uint32_t(std::strtoul(item.c_str(), nullptr, 10)));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// A harness file read with FILE_SHARE_DELETE (carry 2j: a reader that holds a file open without it makes the
// writer's rename-over fail on Windows).
bool ReadShared(const std::filesystem::path& path, std::string* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out->clear();
  char buf[65536];
  DWORD n = 0;
  bool ok = true;
  while ((ok = ::ReadFile(h, buf, sizeof buf, &n, nullptr) != FALSE) && n > 0) out->append(buf, n);
  CloseHandle(h);
  return ok;
}

// The exe and the runtime DLL beside it (XXH3), computed once: 0 when either cannot be read.
uint64_t ComputeBuildId(std::string* unreadable) {
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  const std::filesystem::path exe(path.substr(0, n));
  uint64_t h = 0;
  for (const std::filesystem::path& p : {exe, exe.parent_path() / "rexruntime.dll"}) {
    std::string bytes;
    if (!ReadFile(p, &bytes) || bytes.empty()) {
      *unreadable = p.string();
      return 0;
    }
    h = XXH3_64bits_withSeed(bytes.data(), bytes.size(), h);
  }
  return h == 0 ? 1 : h;  // (0 means unreadable)
}

struct BuildIdCache {
  std::once_flag once;
  uint64_t value = 0;
  std::string unreadable;
};
BuildIdCache& Bid() {
  static BuildIdCache c;
  return c;
}

// The build id as 16 hex digits: two PCs replay one recording only with the same build. A file that cannot be read
// makes the id "unreadable", which agrees with nothing (MetaMismatch), so a recording is never replayed on the
// strength of a hash of nothing.
std::string BuildId() {
  const uint64_t v = BuildIdValue();
  if (v == 0) {
    Log("build: " + Bid().unreadable + " cannot be read; build=unreadable (a recording made or replayed here is refused)");
    return "unreadable";
  }
  return fmt::format("{:016x}", v);
}

// default.xex of the game folder the session runs on (XXH3, 16 hex digits): two PCs replay one recording
// only on the same dump of the game. Unreadable (no folder, no file, an empty file) is "unreadable", as
// for the build.
std::string DataId(const std::filesystem::path& game_data_root) {
  const std::filesystem::path xex = game_data_root / doa4::gamefiles::kXexName;
  std::string bytes;
  if (game_data_root.empty() || !ReadFile(xex, &bytes) || bytes.empty()) {
    Log("game data: " + xex.string() + " cannot be read; data=unreadable (a recording made or replayed here is refused)");
    return "unreadable";
  }
  return fmt::format("{:016x}", XXH3_64bits(bytes.data(), bytes.size()));
}

// A guest settings text as one meta value: newlines written as ';'.
std::string GuestMeta(const std::string& text) {
  std::string out = text;
  std::replace(out.begin(), out.end(), '\n', ';');
  return out;
}

// The recording's meta text (np_core.h): what a replay must match to run at all. The tweaks patch the
// game's tables at boot (SetUpExtras, before this runs), so their state is part of the build. `extra` = the
// mode's guest / unlock keys (and the online session's informational key).
std::string CurrentMeta(const std::filesystem::path& game_data_root, uint32_t sweep_per_frame, const std::string& extra) {
  std::string tweaks;
  for (const auto& t : doa4::tweaks::AllTweaks()) {
    tweaks += fmt::format("{}{}={}", tweaks.empty() ? "" : ",", t.cvar, rex::cvar::Query<bool>(t.cvar) ? 1 : 0);
  }
  const std::string serial = Switch("DOA4_NP_SERIAL");
  return "build=" + BuildId() + "\ndata=" + DataId(game_data_root) + "\nfixes=" + det::FixSetName() +
         "\nserial=" + (serial.empty() ? std::string("default") : serial) + "\nsweep=" +
         std::to_string(sweep_per_frame) + "\ntweaks=" + tweaks + "\n" + extra;
}

// Reads and deletes a request file; false if it is not there.
bool Take(const char* name, std::string* text) {
  const std::filesystem::path p = S().dir / name;
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return false;
  if (!ReadShared(p, text)) return false;
  std::filesystem::remove(p, ec);
  return true;
}

double Now(State& s) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * s.qpc_us;
}

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t UnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void Enqueue(Job job) {
  State& s = S();
  {
    std::lock_guard lk(s.job_mu);
    s.jobs.push_back(std::move(job));
  }
  s.job_cv.notify_one();
}

void WriterLoop() {
  State& s = S();
  for (;;) {
    Job job;
    {
      std::unique_lock lk(s.job_mu);
      s.job_cv.wait(lk, [&] { return s.stop || !s.jobs.empty(); });
      if (s.jobs.empty()) return;
      job = std::move(s.jobs.front());
      s.jobs.pop_front();
    }
    if (job.kind == Job::kShot) {
      doa4::photo::Image image;
      if (!s.host.capture || !s.host.capture(&image)) {
        Log("shot " + std::to_string(job.frame) + ": no guest output yet");
        continue;
      }
      char name[32];
      std::snprintf(name, sizeof(name), "f%06u.png", job.frame);
      const std::string err = doa4::photo::WriteImage(image, s.dir / "shots" / name,
                                                      doa4::photo::FileType::kPng);
      Log("shot " + std::to_string(job.frame) + (err.empty() ? " saved" : ": " + err));
    } else {
      char name[40];
      std::snprintf(name, sizeof(name), "data_%06u.bin", job.frame);
      std::error_code ec;
      std::filesystem::create_directories(s.dir / "dumps", ec);
      std::ofstream out(s.dir / "dumps" / name, std::ios::binary);
      out.write(reinterpret_cast<const char*>(job.bytes.data()), std::streamsize(job.bytes.size()));
      Log("dump " + std::to_string(job.frame) + (out ? " saved" : " FAILED"));
    }
  }
}

// Host bookkeeping inside the .data sweep, hashed as zero: each range is touched only by middleware,
// driver or lock code and never by the game's update (classification: notes/research/
// netplay_phase0_results.md, "Sweep words"). One exception, by judgement: the vblank count is also read
// by game code, but only as a wait's exit condition (nothing derived from it is stored); a frame-keyed
// value would never advance inside that wait.
struct SweepMask {
  uint32_t address;
  uint32_t length;
  const char* why;
};
constexpr SweepMask kSweepMasks[] = {
    // debug 82B70000 (notes/plans/2026-10-30-doa4-netplay-phase1/debug-82b70000-report.md): online pair p1t14b desynced
    // at frame 10 from this block at frame 0. The host had stashed the SDK handle in this critical section's header
    // (wait-list words "REX\0" + handle), the joiner had not: the CRT thread-start/exit notifier sub_827939E0 (called
    // by every new thread's start trampoline sub_82797768 and by ExitThread's sub_82791288) holds the section while it
    // walks the notify list, so two of the ~30 threads starting at boot meet in it only by host timing, and the waiter
    // (RtlEnterCriticalSection -> KeWaitForSingleObject) makes the SDK stash. Census over 1849 kept .data dumps: the
    // words held their static self-pointer 82B747D8 82B747D8 in 1848 and the stash in 1 (p1t14b host); the lock count,
    // recursion count and owner were at rest (-1, 0, 0) in all 1849. Readers: np_xref over 82B747C0..82B74800 lists
    // sub_827939E0 and sub_82793A40, which pass 0x82B747D0 only to RtlEnter/LeaveCriticalSection (SDK imports), and
    // sub_827930E0, which reads the table 82B747C0..82B747CF; a second search of the generated code for offsets
    // 18384..18407 from a 0x82B7 base found no other use, and no .data word holds a pointer into the header. So no
    // guest code reads these 8 bytes; only the SDK writes and reads them.
    {0x82B747D8, 8, "wait-list words of the CRT thread-notify critical section 0x82B747D0 (sub_827939E0, "
                    "sub_82793A40): the SDK's handle stash appears there only when two starting threads meet in it"},
    {0x82F6C9D0, 8, "CRI server threads B and A: loop-pass counters (sub_829BC8F8, sub_829BC458; zeroed by "
                    "sub_829BCAC8), counted by their host-timed idle loops"},
    {0x830ADBCC, 0x1C, "critical section of the 16-slot request table 0x830ADB08 (initialised in sub_824F4560, "
                       "entered by sub_823FC5A0..sub_823FC808): lock count, recursion count, owner thread"},
    {0x831A7AC0, 4, "vblank count stored by the D3D vblank callback sub_82766400 on the host vblank interrupt; "
                    "read only as the wait condition of the vsync wait sub_824F1360 (via sub_824F0C60)"},
    {0x8357A560, 0x400, "GPU timestamp ring 2 of the graphics object 0x83579C28 (+1292): 64 start and 64 end "
                        "slots stored by the GPU-interrupt callback sub_82765540; read only by sub_82765EF8, "
                        "whose result HoldFrameTiming replaces"},
    {0x8357D520, 4, "D3D resource id counter (atomic increment in sub_8275D658): ids for D3D state caching"},
};

std::vector<Region> DigestRegions() {
  std::vector<Region> r;
  r.push_back({"game_obj", 0x8320D110, 0x63B0, {}});  // + 5 x 4336 B fighter blocks at +3836
  r.push_back({"netmatch", 0x830C1CB0, 0x38C0, {}});
  r.push_back({"pad_entries", 0x831011E0, 5 * 44, {}});
  r.push_back({"pad_mgr", 0x8357C2D8, 0xD60, {}});
  Region slots{"task_slots", 0x83100F28, 24 * 28, {}};
  for (uint32_t i = 0; i < 24; ++i) slots.masks.push_back({i * 28 + 16, 8});  // mftb profile words
  r.push_back(slots);
  r.push_back({"mt_833F7600", 0x833F7600, 2504, {}});
  r.push_back({"mt_8357B5C0", 0x8357B5C0, 2504, {}});
  r.push_back({"mt_83420020", 0x83420020, 2504, {}});
  r.push_back({"mt_8321B198", 0x8321B198, 2504, {}});
  r.push_back({"frame_ctr", 0x831013E4, 4, {}});
  if (TestSwitch("DOA4_NP_EXTRA_REGIONS")) {  // hunt: the stage-effect timers and a vblank-like counter
    r.push_back({"stage_fx", 0x82EDA260, 0x4A0, {}});
    r.push_back({"ctr_831A7AC0", 0x831A7AC0, 0x20, {}});
  }
  return r;
}

void CloseFiles(State& s) {
  if (s.rec) std::fclose(s.rec), s.rec = nullptr;
  if (s.digest) std::fclose(s.digest), s.digest = nullptr;
  if (s.sweep) std::fclose(s.sweep), s.sweep = nullptr;
  if (s.counters) std::fclose(s.counters), s.counters = nullptr;
}

// Ends the session: closes the recording and the hash files, switches the determinism layer off and
// (ask_host) asks the host to quit. Callers: OnFrame's replay-end check and quit-frame check (the game's
// main thread; they ask the host) and Shutdown (the host is already shutting down: it does not).
void Quit(State& s, const std::string& why, bool ask_host = true) {
  if (s.quitting) return;
  s.quitting = true;
  Log(std::string("quit at frame ") + std::to_string(s.frame ? s.frame - 1 : 0) + ": " + why);
  CloseFiles(s);
  det::Stop();
  g_active.store(false, std::memory_order_release);  // the hooks pass through from here on
  if (ask_host && s.host.quit) s.host.quit();
}

void PollRequests(State& s, uint32_t f) {
  std::string text;
  if (Take("np_pad.on", &text)) {
    if (s.mode != Mode::kRecord) {
      Log("np_pad.on ignored: not recording");
    } else {
      Script script;
      std::string error;
      if (ParseScript(text, &script, &error)) {
        const uint32_t start = s.queue.Append(script, f);
        Log("np_pad.on: " + std::to_string(script.frames.size()) + " frames from frame " +
            std::to_string(start));
      } else {
        Log("np_pad.on: " + error);
      }
    }
  }
  if (Take("np_shot.on", &text)) Enqueue({Job::kShot, f, {}});
  if (Take("np_ctl.on", &text)) {
    // Not Quit() here: PollRequests runs after this frame's digest row is written but before its
    // input is recorded, and Quit() closes the recording. The run ends at step 3 of OnFrame like a
    // quit frame, so the recording and its digest have the same length and its replay matches.
    if (s.mode == Mode::kOnline) {
      if (text.rfind("leave", 0) == 0) {  // this side leaves at the next frame (both sides end at that frame)
        if (live::Session* l = s.live_ptr.load()) l->Leave(int32_t(f) + 1);
        Log("np_ctl.on: leave at frame " + std::to_string(f + 1));
      }
      if (text.rfind("shot", 0) == 0) Enqueue({Job::kShot, f, {}});
    } else if (text.rfind("quit", 0) == 0) {
      s.quit_frame = std::min(s.quit_frame, f);
    }
    if (text.rfind("dump", 0) == 0) s.dump_frames.insert(s.dump_frames.begin(), f);
  }
}

// The rows of the state entering frame f: digest_row, sweep_row (and sweep_first), counter_row. Always computed
// (Task 12: the online checksum folds the digest and sweep rows every frame).
void HashRows(State& s, uint8_t* base, uint32_t f) {
  s.digest_row.assign(s.regions.size(), 0);
  for (size_t i = 0; i < s.regions.size(); ++i) {
    const Region& r = s.regions[i];
    if (r.masks.empty()) {
      s.digest_row[i] = XXH3_64bits(base + r.address, r.length);
    } else {
      MaskedCopy(r, base + r.address, &s.scratch);
      s.digest_row[i] = XXH3_64bits(s.scratch.data(), s.scratch.size());
    }
  }
  const uint32_t nblocks = (kDataHi - kDataLo) / kBlock;
  s.sweep_first = SweepFirst(f, s.sweep_per_frame, nblocks);
  s.sweep_row.assign(s.sweep_per_frame, 0);
  for (uint32_t i = 0; i < s.sweep_per_frame; ++i) {
    const uint32_t k = (s.sweep_first + i) % nblocks;
    const uint32_t lo = kDataLo + k * kBlock;
    const Region& r = s.block_regions[k];
    // The SDK's handle stash words (np_core.h kStashSignature) are hashed as zero; the word before the block is
    // read only inside .data (block 0 has no `before`).
    const uint8_t* before = lo > kDataLo ? base + lo - 4 : nullptr;
    if (r.masks.empty() && !HasStashWords(base + lo, kBlock, before)) {
      s.sweep_row[i] = XXH3_64bits(base + lo, kBlock);
    } else {
      const uint32_t stash = SweepBlockCopy(r, base + lo, before, &s.scratch);
      s.sweep_row[i] = XXH3_64bits(s.scratch.data(), s.scratch.size());
      if (stash && k < s.stash_logged.size() && !s.stash_logged[k]) {
        s.stash_logged[k] = 1;
        char line[128];
        std::snprintf(line, sizeof(line), "sweep: block %08X: %u SDK handle stash words hashed as zero (frame %u)", lo,
                      stash, f);
        Log(line);
      }
    }
  }
  det::CounterValues(&s.counter_row);
}

// Writes the rows HashRows computed for frame f into the files that are open.
void WriteRows(State& s, uint32_t f) {
  if (s.digest) {
    const uint32_t head[2] = {f, 0};
    std::fwrite(head, sizeof(head), 1, s.digest);
    std::fwrite(s.digest_row.data(), sizeof(uint64_t), s.digest_row.size(), s.digest);
  }
  if (s.sweep) {
    const uint32_t head[2] = {f, s.sweep_first};
    std::fwrite(head, sizeof(head), 1, s.sweep);
    std::fwrite(s.sweep_row.data(), sizeof(uint64_t), s.sweep_row.size(), s.sweep);
  }
  if (s.counters) {
    const std::string rec = EncodeCountersFrame(f, s.counter_row);
    std::fwrite(rec.data(), 1, rec.size(), s.counters);
  }
  if ((f % 60) == 0) {
    if (s.digest) std::fflush(s.digest);
    if (s.sweep) std::fflush(s.sweep);
    if (s.counters) std::fflush(s.counters);
    if (s.rec) std::fflush(s.rec);
  }
}

// sub_824F4498 (the frame function; its pre-wrap is the frame hook) measures the GPU's last frame from
// timestamp ring 2 of the graphics object 0x83579C28 (sub_82765EF8: end - start of the newest complete
// slot pair; the slots are stored by the GPU-interrupt callback sub_82765540 on the host's GPU
// interrupt thread, so the frame they read is host-timed) and stores it into 0x831008E8 as percent of a
// 59.94 Hz frame (100 * ticks / (KeQueryPerformanceFrequency * 0x822BC600)), and 0x822BC828 / that,
// capped at 0x82166944 (60), into 0x831008E4. The game reads 0x831008E8 in sub_826BF030 (compared with
// 0x8204FEF8 and a threshold table: a flag at +48 of its object) and sub_8271B318 (compared with the
// thresholds at 0x8215E740..0x8215E748: which of r5 = 4..7 it passes on). In a session both words
// hold what a 60 fps frame gives: the game's own arithmetic on one frame of the virtual timebase
// (kTimebaseHz / 60 ticks), with the constants read from the image - 99.9 (0x42C7CCC7) and 60, the
// values the replays showed whenever the measured slot pair was exactly one virtual frame apart.
void HoldFrameTiming(uint8_t* base) {
  State& s = S();
  const float period = rex::memory::load_and_swap<float>(base + 0x822BC600);
  const float percent = rex::memory::load_and_swap<float>(base + 0x8204FEF8);
  const float fps_num = rex::memory::load_and_swap<float>(base + 0x822BC828);
  const float fps_cap = rex::memory::load_and_swap<float>(base + 0x82166944);
  const float measure = float(kTimebaseHz / 60) / (float(kTimebaseHz) * period);  // sub_82765EF8
  const float gpu = measure * percent;                                              // sub_824F4498
  float fps = fps_num / gpu;
  if (fps > fps_cap) fps = fps_cap;
  rex::memory::store_and_swap<float>(base + 0x831008E8, gpu);
  rex::memory::store_and_swap<float>(base + 0x831008E4, fps);
  if (!s.timing_logged) {
    s.timing_logged = true;
    uint32_t bits;
    std::memcpy(&bits, &gpu, 4);
    char line[160];
    std::snprintf(line, sizeof(line), "frame timing held: 0x831008E8 = %.6g (%08X), 0x831008E4 = %.6g", gpu, bits,
                  fps);
    Log(line);
  }
}

// Step (3) of the frame hook: unlock everything for the profile the game has loaded, every frame (a later reload of the
// save would otherwise take it back). FindProfile is false until the save has been read.
void UnlockStep(State& s, uint32_t f) {
  if (!s.unlock || !s.host.guest) return;
  const doa4::GuestBytes& g = *s.host.guest;
  const std::optional<uint32_t> p = doa4::unlocks::FindProfile(g);
  if (!p) return;
  if (doa4::unlocks::IsUnlocked(g, *p, doa4::unlocks::Unlock::kEverything)) {
    if (!s.unlock_seen_logged) {  // once: the evidence that the step ran on a save that needed nothing
      s.unlock_seen_logged = true;
      Log("unlock: profile found at frame " + std::to_string(f) + ", everything already unlocked");
    }
    return;
  }
  const doa4::unlocks::UnlockResult r = doa4::unlocks::ApplyUnlock(g, *p, doa4::unlocks::Unlock::kEverything);
  ++s.unlocks_applied;
  char line[160];
  std::snprintf(line, sizeof line, "unlock: everything at frame %u (characters %d, costumes %d, voices %d, stages %d)", f,
                r.characters, r.costumes, r.voices, r.stages);
  Log(line);
}

// ---- online ----

const char* EndText(live::End e) {
  switch (e) {
    case live::End::kLeft: return "left";
    case live::End::kPeerLeft: return "peer left";
    case live::End::kLost: return "lost";
    case live::End::kDesync: return "desync";
    case live::End::kFrame0Timeout: return "frame 0 timeout";
    case live::End::kInternal: return "internal";
    case live::End::kNone: break;
  }
  return "none";
}

// The terminal reason np_pair judges: left | peer left | lost | frame 0 timeout | desync at frame ... | internal: ...
std::string TerminalReason(const live::FrameResult& r, bool refused) {
  if (refused) return "internal: the other player's game did not start the session";
  switch (r.end) {
    case live::End::kDesync: return r.why;  // "desync at frame N (this side X, other side Y)"
    case live::End::kInternal: return "internal: " + r.why;
    default: return EndText(r.end);
  }
}

wire::LeaveReason LeaveReasonFor(live::End e) {
  switch (e) {
    case live::End::kDesync: return wire::LeaveReason::kDesync;
    case live::End::kLost:
    case live::End::kFrame0Timeout: return wire::LeaveReason::kTimeout;
    case live::End::kInternal: return wire::LeaveReason::kInternal;
    default: return wire::LeaveReason::kLeft;
  }
}

void Event(State& s, int64_t unix_ms, const std::string& text) {
  std::lock_guard lk(s.events_mu);
  if (!s.events) return;
  std::fprintf(s.events, "%lld %s\n", static_cast<long long>(unix_ms), text.c_str());
  std::fflush(s.events);
}

int64_t UnixFromUs(State& s, uint64_t at_us) { return int64_t(double(at_us) / 1000.0 + s.unix_offset_ms); }

void NotePacket(State& s, bool tx, uint32_t size, const uint8_t* bytes) {
  PacketNote& n = s.ring[s.ring_next++ % 64];
  n.tx = tx;
  n.t_us = Now(s);
  n.frame = s.session_frame.load(std::memory_order_relaxed);
  n.size = size;
  std::memset(n.head, 0, sizeof n.head);
  std::memcpy(n.head, bytes, std::min<size_t>(size, sizeof n.head));
}

void DumpRing(State& s, const std::string& why) {
  if (s.ring_dumps >= 50) return;  // a bounded log
  ++s.ring_dumps;
  const double now = Now(s);
  const uint32_t n = std::min<uint32_t>(s.ring_next, 64);
  Log("packet ring (" + why + "): the last " + std::to_string(n) + " GekkoNet packets, oldest first");
  for (uint32_t i = 0; i < n; ++i) {
    const PacketNote& p = s.ring[(s.ring_next - n + i) % 64];
    char line[200];
    int k = std::snprintf(line, sizeof line, "  %s %7.1f ms ago frame %d %u B:", p.tx ? "tx" : "rx",
                          (now - p.t_us) / 1000.0, int(p.frame), unsigned(p.size));
    for (uint32_t b = 0; b < std::min<uint32_t>(p.size, 16) && k < int(sizeof line) - 4; ++b) {
      k += std::snprintf(line + k, sizeof line - k, " %02X", p.head[b]);
    }
    Log(line);
  }
}

void OnLiveEvent(live::Event e, int32_t frame, uint64_t at_us) {
  State& s = S();
  switch (e) {
    case live::Event::kHoldBegin:
      s.hold_begin_us = at_us;
      Event(s, UnixFromUs(s, at_us), "hold begin frame " + std::to_string(frame));
      DumpRing(s, "hold at frame " + std::to_string(frame));
      break;
    case live::Event::kBannerWaiting:
      s.banner_shown = true;
      Event(s, UnixFromUs(s, at_us), "banner waiting");
      if (s.host.overlay_changed) s.host.overlay_changed();
      break;
    case live::Event::kHoldEnd: {
      const uint64_t n = (at_us - s.hold_begin_us) / 1000;
      Event(s, UnixFromUs(s, at_us), "hold end frame " + std::to_string(frame) + " after " + std::to_string(n) + " ms");
      // cleared first: the overlay update is posted to the UI thread and must see the banner gone
      if (s.banner_shown.exchange(false) && s.host.overlay_changed) s.host.overlay_changed();
      break;
    }
    case live::Event::kEnd: break;  // written by EndOnline with the reason
  }
}

// carry 2e: GekkoNet's network stats (with units) and the transport's counters.
void LogNetStats(State& s, const std::string& when) {
  std::string line = "net stats " + when + ":";
  if (s.gk) {
    const gekko::NetStats st = s.gk->Stats();
    char b[160];
    std::snprintf(b, sizeof b, " ping %.1f ms, jitter %.1f ms, sent %.2f kb/s, received %.2f kb/s;", st.ping_ms,
                  st.jitter_ms, st.kb_sent, st.kb_received);
    line += b;
  }
  if (net::Transport* t = s.host.transport ? s.host.transport() : nullptr) {
    const net::Stats ts = t->stats();
    uint32_t dropped = 0;
    std::string reasons;
    for (size_t r = 0; r < size_t(wire::Drop::kCount); ++r) {
      dropped += ts.dropped[r];
      if (ts.dropped[r]) reasons += " " + std::to_string(r) + ":" + std::to_string(ts.dropped[r]);
    }
    line += " transport discards " + std::to_string(dropped) + (reasons.empty() ? "" : " (by reason" + reasons + ")") +
            ", queue_full " + std::to_string(ts.queue_full) + ", wrong_thread " + std::to_string(ts.wrong_thread) +
            ", gekko_early " + std::to_string(ts.gekko_early) + ", sim dropped " + std::to_string(ts.sim_dropped) +
            " delayed " + std::to_string(ts.sim_delayed) + " drop hits " + std::to_string(ts.sim_drop_hits);
  }
  if (s.gk) line += ", sync responses dropped " + std::to_string(s.gk->sync_responses_dropped());
  Log(line);
}

// The end of an online session (game thread, or Abort before the first frame): the terminal lines, the Leave, the
// det layer off, the overlay, and the offline relaunch after 5 s (8 s for a desync, R16).
void EndOnline(State& s, const live::FrameResult& r, bool at_hook) {
  if (s.ended) return;
  s.ended = true;
  s.running.store(false);
  const bool refused = s.refused.load();
  const std::string reason = TerminalReason(r, refused);
  const int32_t f = r.end_frame;
  if (!r.why.empty()) Log("session end: " + std::string(EndText(r.end)) + " (" + r.why + ")");
  const uint32_t handed = s.ex ? s.ex->saves_filled() : 0;
  if (at_hook) LogNetStats(s, "at the end");
  Log("session ended at frame " + std::to_string(f) + ": " + reason);
  Log("checksums handed to GekkoNet: " + std::to_string(handed));
  Event(s, UnixMs(), "session ended at frame " + std::to_string(f) + ": " + reason);
  if (r.end == live::End::kDesync) Log("desync: " + r.why + " (a dump of .data was queued)");
  // the Leave to the peer (lobby channel, sent 3 times by the lobby); on a desync too (carry 2i: a lossy link can drop
  // GekkoNet's one checksum message)
  if (s.host.send_leave && r.end != live::End::kPeerLeft) s.host.send_leave(f, LeaveReasonFor(r.end));
  {
    std::lock_guard lk(s.result_mu);
    s.last_result = r;
  }
  if (s.live) s.live->Close();  // carry 2i: Executor::DropPending, then gekko Destroy
  CloseFiles(s);
  {
    std::lock_guard lk(s.events_mu);
    if (s.events) std::fclose(s.events), s.events = nullptr;
  }
  det::Stop();
  g_active.store(false, std::memory_order_release);
  s.quitting = true;
  if (s.host.overlay_changed) s.host.overlay_changed();
  const int delay_ms = r.end == live::End::kDesync ? 8000 : 5000;
  Log("offline restart in " + std::to_string(delay_ms / 1000) + " s");
  s.restarter = std::thread([delay_ms] {
    State& st = S();
    std::unique_lock lk(st.watch_mu);
    if (st.watch_cv.wait_for(lk, std::chrono::milliseconds(delay_ms), [&] { return st.watch_stop; })) return;
    lk.unlock();
    Log("offline restart");
    if (st.host.restart) st.host.restart(false);
  });
}

// The hang watch (one line when no frame hook ran for 30 s while the session runs) and the session_refused poll
// (carry 2g).
void WatchLoop() {
  State& s = S();
  bool hang_logged = false;
  std::unique_lock lk(s.watch_mu);
  while (!s.watch_cv.wait_for(lk, std::chrono::milliseconds(250), [&] { return s.watch_stop; })) {
    lk.unlock();
    if (s.running.load()) {
      if (!s.refused.load() && s.host.session_refused && s.host.session_refused()) {
        s.refused.store(true);
        Log("the other player's game answered as a lobby: it did not start the session (ending at once)");
        s.pending_peer_left.store(uint64_t(uint8_t(wire::LeaveReason::kInternal)) << 32 | 0u);
        if (live::Session* l = s.live_ptr.load()) l->PeerLeft(0, wire::LeaveReason::kInternal);
      }
      const int64_t last = s.last_hook_ms.load();
      if (!hang_logged && last && SteadyMs() - last > 30000) {
        hang_logged = true;
        Log("hang watch: no frame for 30 s (last frame " + std::to_string(s.session_frame.load()) + ", " +
            (s.in_hold.load() ? "in a hold" : "in the game") + ")");
      }
    }
    lk.lock();
  }
}

bool CreateLive(State& s) {
  net::Transport* t = s.host.transport ? s.host.transport() : nullptr;
  if (!t) {
    Log("WARN session: no transport at the first frame (the session lobby did not start)");
    return false;
  }
  s.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!s.timer) {
    Log("high-resolution waitable timer unavailable (Windows error " + std::to_string(GetLastError()) +
        "); a plain waitable timer is used");
    s.timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }
  s.gk = std::make_unique<gekko::Session>();
  s.ex = std::make_unique<exec::Executor>(2, [](int32_t, const FrameInput& input) {
    State& st = S();
    if (st.rec) {  // rec.d4r = the confirmed-input tap
      const std::string bytes = EncodeRecFrame(input);
      std::fwrite(bytes.data(), 1, bytes.size(), st.rec);
    }
  });
  gekko::Config gc;
  gc.num_players = 2;
  gc.local_slot = s.slot;
  gc.delay = s.ho.delay;
  const bool created = s.gk->Create(
      gc,
      [t](std::span<const uint8_t> payload) {
        State& st = S();
        const double now = Now(st);
        if (st.last_tx_us && now - st.last_tx_us >= 50000 && st.silence_logs < 100) {
          ++st.silence_logs;
          char line[96];
          std::snprintf(line, sizeof line, "tx silence %.1f ms before frame %d", (now - st.last_tx_us) / 1000.0,
                        int(st.session_frame.load()));
          Log(line);
        }
        st.last_tx_us = now;
        NotePacket(st, true, uint32_t(payload.size()), payload.data());
        t->PushGekko(std::vector<uint8_t>(payload.begin(), payload.end()));
      },
      [t](std::vector<uint8_t>* payload) {
        if (!t->PopGekko(payload)) return false;
        State& st = S();
        const double now = Now(st);
        if (st.last_rx_us && now - st.last_rx_us >= 50000) {
          if (st.silence_logs < 100) {
            ++st.silence_logs;
            char line[96];
            std::snprintf(line, sizeof line, "rx silence %.1f ms before frame %d", (now - st.last_rx_us) / 1000.0,
                          int(st.session_frame.load()));
            Log(line);
          }
          DumpRing(st, "rx silence");
        }
        st.last_rx_us = now;
        NotePacket(st, false, uint32_t(payload->size()), payload->data());
        return true;
      });
  if (!created) {
    Log("WARN session: GekkoNet session could not be created");
    return false;
  }
  HANDLE arrived = t->gekko_arrived();
  live::Clock clock;
  clock.now_us = [] { return uint64_t(Now(S())); };
  clock.wait_packet = [arrived](uint32_t max_ms) {
    State& st = S();
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(max_ms) * 10000;
    SetWaitableTimer(st.timer, &due, 0, nullptr, nullptr, FALSE);
    HANDLE hs[2] = {arrived, st.timer};
    if (arrived) {
      WaitForMultipleObjects(2, hs, FALSE, max_ms + 100);
    } else {
      WaitForSingleObject(st.timer, max_ms + 100);
    }
    CancelWaitableTimer(st.timer);
  };
  clock.sleep_precise = [](uint32_t us) {
    State& st = S();
    LARGE_INTEGER due;
    due.QuadPart = -int64_t(us) * 10;
    SetWaitableTimer(st.timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(st.timer, us / 1000 + 100);
  };
  live::Hooks hooks;
  hooks.on_event = OnLiveEvent;
  s.live = std::make_unique<live::Session>(s.gk.get(), s.ex.get(), clock, live::Limits{}, hooks);
  s.live_ptr.store(s.live.get());
  // what arrived before the session existed (the stores come first on the other threads, then their live_ptr read)
  const uint64_t pl = s.pending_peer_left.load();
  if (pl != UINT64_MAX) s.live->PeerLeft(int32_t(uint32_t(pl)), wire::LeaveReason(uint8_t(pl >> 32)));
  const int32_t lv = s.pending_leave.load();
  if (lv != INT32_MAX) s.live->Leave(lv);
  Log("GekkoNet session created at the first frame hook: slot " + std::to_string(s.slot) + ", delay " +
      std::to_string(s.ho.delay));
  return true;
}

// Test switch DOA4_NP_FORCE_STASH=<hex address>[,<hex address>...] (debug 82B70000): at frame 0, before the state
// entering it is hashed, the SDK makes its host object for each guest dispatcher header named, through
// KeResetEvent on that header (the event is unsignalled, so the only guest-memory change is the SDK's handle stash
// over the header's wait-list words). It stands in for a boot-time contention on a critical section, whose waiter
// stashes the same way (RtlEnterCriticalSection -> KeWaitForSingleObject -> XObject::GetNativeObject).
void ForceStash(PPCContext& ctx, uint8_t* base) {
  std::stringstream in(TestDirEnv("DOA4_NP_FORCE_STASH"));
  std::string item;
  while (std::getline(in, item, ',')) {
    const uint32_t a = uint32_t(std::strtoul(item.c_str(), nullptr, 16));
    if (a == 0) continue;
    const uint64_t r3 = ctx.r3.u64;
    ctx.r3.u64 = a;
    __imp__KeResetEvent(ctx, base);
    ctx.r3.u64 = r3;
    const uint8_t* w = base + a + 8;
    char line[128];
    std::snprintf(line, sizeof line, "forced stash: header %08X wait-list words now %02X%02X%02X%02X %02X%02X%02X%02X",
                  a, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    Log(line);
  }
}

void OnFrameOnline(PPCContext& ctx, uint8_t* base) {
  State& s = S();
  const double t0 = Now(s);
  const uint32_t f = s.frame++;
  if (s.quitting || s.ended) return;
  s.session_frame.store(int32_t(f));
  s.last_hook_ms.store(SteadyMs());
  // carry 2f: the game thread away from the frame hook for more than 50 ms
  if (s.last_hook_end_us && t0 - s.last_hook_end_us > 50000 && s.away_logged < 100) {
    ++s.away_logged;
    char line[112];
    std::snprintf(line, sizeof line, "frame %u: the game thread was %.1f ms away from the frame hook", f,
                  (t0 - s.last_hook_end_us) / 1000.0);
    Log(line);
  }
  if (f == 0) {
    s.mxcsr0 = _mm_getcsr() & 0xFFC0;
    s.unix_offset_ms = double(UnixMs()) - t0 / 1000.0;
    Event(s, UnixMs(), std::string("session start ") + (s.slot == 0 ? "host" : "joiner"));
    if (!CreateLive(s)) {
      live::FrameResult r;
      r.end = live::End::kInternal;
      r.end_frame = 0;
      r.why = "the session could not start (see the WARN line)";
      EndOnline(s, r, true);
      return;
    }
  }

  if (f == 0) ForceStash(ctx, base);

  // (1) the state entering f, and its checksum
  const double h0 = Now(s);
  HashRows(s, base, f);
  const uint32_t checksum = exec::FoldChecksum(int32_t(f), s.digest_row, s.sweep_row);
  const double h1 = Now(s);

  // (2) dumps, requests, the forced desync (after hashing: the next frame's state differs)
  bool dump = false;
  while (!s.dump_frames.empty() && s.dump_frames.front() <= f) {
    dump = true;
    s.dump_frames.erase(s.dump_frames.begin());
  }
  if (dump) {
    Job job{Job::kDump, f, {}};
    job.bytes.assign(base + kDataLo, base + kDataHi);
    Enqueue(std::move(job));
  }
  if ((f % 15) == 0) PollRequests(s, f);
  static const std::string force = TestDirEnv("DOA4_NP_FORCE_DESYNC");
  if (!force.empty() && std::strtoul(force.c_str(), nullptr, 10) == f) {
    base[kMtState + 100] ^= 0x01;
    Log("forced desync: one byte of the MT state 0x833F7600 flipped at frame " + std::to_string(f));
  }

  // (3) the unlock
  UnlockStep(s, f);

  // (4) the determinism boundary (R1: before any wait for the other side)
  const double d0 = Now(s);
  det::OnFrame(f, ctx, base);
  const double det_us = Now(s) - d0;

  // (5) the local pad
  Pad local;
  std::vector<ScriptEvent::Kind> fired;
  if (s.local_script) {
    const FrameInput in = s.queue.Next(f, &fired);
    local = in.pad[s.slot];
  } else if (s.host.read_local_pad) {
    Pad p;
    if (s.host.read_local_pad(&p)) local = p;
  }
  bool shot = false;
  for (ScriptEvent::Kind k : fired) {
    if (k == ScriptEvent::kShot) shot = true;
    if (k == ScriptEvent::kQuit) {
      s.live->Leave(int32_t(f));
      Log("script quit: leaving at frame " + std::to_string(f));
    }
  }

  // (6) lockstep
  s.in_hold.store(true);
  const live::FrameResult r = s.live->Frame(int32_t(f), checksum, local);
  s.in_hold.store(false);
  {
    std::lock_guard lk(s.result_mu);
    s.last_result = r;
  }
  if (!r.ok) {
    if (r.end == live::End::kDesync) {
      Job job{Job::kDump, f, {}};
      job.bytes.assign(base + kDataLo, base + kDataHi);
      Enqueue(std::move(job));
    }
    EndOnline(s, r, true);
    return;
  }
  s.staged = r.input;
  WriteRows(s, f);  // the rows of the frames that advanced: a replay of rec.d4r hashes exactly these

  // (7) shots
  while (!s.shot_frames.empty() && s.shot_frames.front() <= f) {
    shot |= s.shot_frames.front() == f;
    s.shot_frames.erase(s.shot_frames.begin());
  }
  if (s.shot_every && f % s.shot_every == 0) shot = true;
  if (shot) Enqueue({Job::kShot, f, {}});

  // (8) timing: online hook = the frame hook's time minus det, wait and pacer (net included; ruling F19)
  const double t1 = Now(s);
  const double net = double(r.timing.net_us), wait = double(r.timing.wait_us), pacer = double(r.timing.pacer_us);
  const double hook = std::max(0.0, t1 - t0 - det_us - wait - pacer), hash = h1 - h0;
  s.sum_det_us += det_us;
  s.max_det_us = std::max(s.max_det_us, det_us);
  s.sum_hook_us += hook;
  s.max_hook_us = std::max(s.max_hook_us, hook);
  s.sum_hash_us += hash;
  s.max_hash_us = std::max(s.max_hash_us, hash);
  s.sum_net_us += net;
  s.max_net_us = std::max(s.max_net_us, net);
  s.sum_wait_us += wait;
  s.max_wait_us = std::max(s.max_wait_us, wait);
  s.sum_pacer_us += pacer;
  const float ahead = s.live->frames_ahead();
  s.sum_ahead += ahead;
  if (s.last_frame_qpc) {
    const double period = t0 - double(s.last_frame_qpc);
    s.sum_period_us += period;
    s.max_period_us = std::max(s.max_period_us, period);
  }
  s.last_frame_qpc = int64_t(t0);
  {
    std::lock_guard lk(s.timing_mu);
    s.timing.frame = int32_t(f);
    s.timing.delay = s.ho.delay;
    s.timing.frames_ahead = ahead;
    s.timing.net_us = net;
    s.timing.wait_us = wait;
    s.timing.pacer_us = pacer;
    s.timing.hook_us = hook;
    s.timing.hash_us = hash;
    s.timing.holds = r.holds;  // ruling F8
  }
  if (++s.window == 600) {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "frame %u: hook avg %.1f us max %.1f us, hash avg %.1f us max %.1f us, "
                  "det wait avg %.1f us max %.1f us, frame period avg %.3f ms (%.2f fps) max %.2f ms, "
                  "net avg %.1f us max %.1f us, wait avg %.1f us max %.1f us, pacer avg %.1f us, holds %u, ahead avg %.2f",
                  f, s.sum_hook_us / 600, s.max_hook_us, s.sum_hash_us / 600, s.max_hash_us, s.sum_det_us / 600,
                  s.max_det_us, s.sum_period_us / 600 / 1000, 600e6 / std::max(1.0, s.sum_period_us),
                  s.max_period_us / 1000, s.sum_net_us / 600, s.max_net_us, s.sum_wait_us / 600, s.max_wait_us,
                  s.sum_pacer_us / 600, unsigned(r.holds), s.sum_ahead / 600);
    Log(line);
    LogNetStats(s, "at frame " + std::to_string(f));
    // R10: the MXCSR control bits against the value at the session's start, logged once per change
    const uint32_t now_csr = _mm_getcsr() & 0xFFC0;
    if (MxcsrControlDiffers(now_csr, s.mxcsr0) && s.mxcsr_logged != now_csr) {
      s.mxcsr_logged = now_csr;
      char m[112];
      std::snprintf(m, sizeof m, "MXCSR control bits %04X differ from the session start's %04X (frame %u)", now_csr,
                    s.mxcsr0, f);
      Log(m);
    }
    s.window = 0;
    s.sum_hook_us = s.max_hook_us = s.sum_hash_us = s.max_hash_us = 0;
    s.sum_det_us = s.max_det_us = 0;
    s.sum_period_us = s.max_period_us = 0;
    s.sum_net_us = s.max_net_us = s.sum_wait_us = s.max_wait_us = s.sum_pacer_us = s.sum_ahead = 0;
  }
  s.last_hook_end_us = Now(s);
}

// The frame hook: runs at the loop head before frame f. The state hashed here
// is the state entering frame f (everything frame f - 1 did).
void OnFrame(PPCContext& ctx, uint8_t* base) {
  State& s = S();
  if (s.mode == Mode::kOnline) {
    OnFrameOnline(ctx, base);
    return;
  }
  const double t0 = Now(s);
  const uint32_t f = s.frame++;
  if (s.quitting) return;
  // A replay ends where its recording ends: it hashes exactly the frames the record run hashed.
  if (s.mode == Mode::kReplay && f >= s.replay.frames.size()) {
    Quit(s, "end of the recording");
    return;
  }

  // 1. Hash the state entering frame f.
  const double h0 = Now(s);
  HashRows(s, base, f);
  WriteRows(s, f);
  const double h1 = Now(s);
  bool dump = false;
  while (!s.dump_frames.empty() && s.dump_frames.front() <= f) {  // (a request lands on f + 1)
    dump = true;
    s.dump_frames.erase(s.dump_frames.begin());
  }
  if (dump) {
    Job job{Job::kDump, f, {}};
    job.bytes.assign(base + kDataLo, base + kDataHi);
    Enqueue(std::move(job));
  }

  // 2. Requests; the unlock of a replay of a session (R21: the frame hook's step 3, before det); the input of frame f.
  if ((f % 15) == 0) PollRequests(s, f);
  UnlockStep(s, f);
  std::vector<ScriptEvent::Kind> fired;
  if (s.mode == Mode::kReplay) {
    s.staged = s.replay.frames[f];
  } else {
    s.staged = s.queue.Next(f, &fired);
    if (s.rec) {
      const std::string bytes = EncodeRecFrame(s.staged);
      std::fwrite(bytes.data(), 1, bytes.size(), s.rec);
    }
  }
  const double d0 = Now(s);
  det::OnFrame(f, ctx, base);
  const double det_us = Now(s) - d0;

  // 3. Screenshots and the end of the run.
  bool shot = false;
  for (ScriptEvent::Kind k : fired) {
    if (k == ScriptEvent::kShot) shot = true;
    if (k == ScriptEvent::kQuit) s.quit_frame = std::min(s.quit_frame, f);
  }
  while (!s.shot_frames.empty() && s.shot_frames.front() <= f) {
    shot |= s.shot_frames.front() == f;
    s.shot_frames.erase(s.shot_frames.begin());
  }
  if (s.shot_every && f % s.shot_every == 0) shot = true;
  if (shot) Enqueue({Job::kShot, f, {}});
  if (f >= s.quit_frame) {
    Quit(s, "quit frame");
    return;
  }

  // 4. Timing: hook cost (without the determinism layer's waits), hash cost,
  // the determinism layer (audio wait), frame period; 600-frame averages.
  const double t1 = Now(s);
  const double hook = t1 - t0 - det_us, hash = h1 - h0;
  s.sum_det_us += det_us;
  s.max_det_us = std::max(s.max_det_us, det_us);
  s.sum_hook_us += hook;
  s.max_hook_us = std::max(s.max_hook_us, hook);
  s.sum_hash_us += hash;
  s.max_hash_us = std::max(s.max_hash_us, hash);
  if (s.last_frame_qpc) {
    const double period = t0 - double(s.last_frame_qpc);
    s.sum_period_us += period;
    s.max_period_us = std::max(s.max_period_us, period);
  }
  s.last_frame_qpc = int64_t(t0);
  if (++s.window == 600) {
    char line[320];
    std::snprintf(line, sizeof(line),
                  "frame %u: hook avg %.1f us max %.1f us, hash avg %.1f us max %.1f us, "
                  "det wait avg %.1f us max %.1f us, frame period avg %.3f ms (%.2f fps) max %.2f ms",
                  f, s.sum_hook_us / 600, s.max_hook_us, s.sum_hash_us / 600, s.max_hash_us,
                  s.sum_det_us / 600, s.max_det_us, s.sum_period_us / 600 / 1000,
                  600e6 / std::max(1.0, s.sum_period_us), s.max_period_us / 1000);
    Log(line);
    s.window = 0;
    s.sum_hook_us = s.max_hook_us = s.sum_hash_us = s.max_hash_us = 0;
    s.sum_det_us = s.max_det_us = 0;
    s.sum_period_us = s.max_period_us = 0;
  }
}

// Opens the hash files, builds the sweep masks and starts the writer (every mode).
void OpenHashFiles(State& s) {
  const uint32_t nblocks = (kDataHi - kDataLo) / kBlock;
  s.regions = DigestRegions();
  s.digest = _wfopen((s.dir / "digest.d4d").wstring().c_str(), L"wb");
  if (s.digest) {
    const std::string head = EncodeDigestHeader(s.regions);
    std::fwrite(head.data(), 1, head.size(), s.digest);
  }
  s.sweep = _wfopen((s.dir / "sweep.d4s").wstring().c_str(), L"wb");
  if (s.sweep) {
    const std::string head = EncodeSweepHeader(kDataLo, kDataHi, kBlock, s.sweep_per_frame);
    std::fwrite(head.data(), 1, head.size(), s.sweep);
  }
  {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const SweepMask& m : kSweepMasks) ranges.push_back({m.address, m.length});
    s.block_regions = BlockRegions(kDataLo, kBlock, nblocks, ranges);
    uint32_t masked = 0, blocks = 0;
    for (const Region& b : s.block_regions) {
      blocks += b.masks.empty() ? 0 : 1;
      for (const Mask& m : b.masks) masked += m.length;
    }
    Log("sweep masks: " + std::to_string(std::size(kSweepMasks)) + " ranges, " + std::to_string(masked) +
        " bytes in " + std::to_string(blocks) + " blocks hashed as zero; and every SDK handle stash word (the word "
        "after a \"REX\\0\" header signature) hashed as zero");
    s.stash_logged.assign(nblocks, 0);
  }
  s.counters = _wfopen((s.dir / "counters.d4k").wstring().c_str(), L"wb");
  if (s.counters) {
    const std::string head = EncodeCountersHeader(det::CounterNames());
    std::fwrite(head.data(), 1, head.size(), s.counters);
  }
  s.writer = std::thread(WriterLoop);
}

// Opens np_log.txt (with the lines EarlyLog kept) and the QPC scale.
void OpenLog(State& s) {
  s.log = _wfopen((s.dir / "np_log.txt").wstring().c_str(), L"w");
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  s.qpc_us = 1e6 / double(freq.QuadPart);
  std::vector<std::string> early;
  {
    std::lock_guard lk(g_early_mu);
    early.swap(g_early);
  }
  std::lock_guard lk(s.log_mu);
  if (s.log) {
    for (const std::string& l : early) std::fprintf(s.log, "%s\n", l.c_str());
    std::fflush(s.log);
  }
}

void ReadSweepSetting(State& s, const std::string& sweep) {
  const uint32_t nblocks = (kDataHi - kDataLo) / kBlock;
  if (sweep == "all") {
    s.sweep_per_frame = nblocks;
  } else if (!sweep.empty()) {
    s.sweep_per_frame = std::clamp<uint32_t>(std::strtoul(sweep.c_str(), nullptr, 10), 1, nblocks);
  }
}

void StartOnline(Host host) {
  State& s = S();
  s.mode = Mode::kOnline;
  s.host = std::move(host);
  s.ho = *s.host.online;
  s.slot = s.ho.role == handoff::Role::kHost ? 0 : 1;
  s.seed = s.ho.seed;
  s.dir = s.host.session_dir;
  std::error_code ec;
  std::filesystem::create_directories(s.dir / "shots", ec);
  OpenLog(s);
  Log("build " + BuildId() + " release " + s.host.release + " data " + DataId(s.host.game_data_root) + " fixes " +
      det::FixSetName());
  char tok[16];
  std::snprintf(tok, sizeof tok, "%08X", unsigned(s.ho.session_token));
  Log(std::string("session: ") + (s.slot == 0 ? "host" : "joiner") + ", delay " + std::to_string(s.ho.delay) +
      ", token " + tok + ", peer " + rules::EndpointText(s.ho.peer) + ", seed " + std::to_string(s.seed));
  s.events = _wfopen((s.dir / "events.txt").wstring().c_str(), L"a");
  ReadSweepSetting(s, TestDirEnv("DOA4_NP_SWEEP"));
  s.unlock = true;
  s.guest_meta = GuestMeta(s.ho.guest_settings);
  s.unlock_meta = "1";
  const std::string extra = "guest=" + s.guest_meta + "\nunlock=1\nguest_skipped=" +
                            (g_skipped.empty() ? std::string("none") : g_skipped) + "\nsession=online role=" +
                            std::string(s.slot == 0 ? "host" : "joiner") + " delay=" + std::to_string(s.ho.delay) + "\n";
  const std::string meta = CurrentMeta(s.host.game_data_root, s.sweep_per_frame, extra);
  s.rec = _wfopen((s.dir / "rec.d4r").wstring().c_str(), L"wb");
  const std::string head = EncodeRecHeader(s.seed, 0, meta);
  if (s.rec) std::fwrite(head.data(), 1, head.size(), s.rec);
  if (const std::string path = TestDirEnv("DOA4_NP_LOCAL_SCRIPT"); !path.empty()) {
    std::string text, error;
    Script script;
    if (!ReadShared(path, &text) || !ParseScript(text, &script, &error)) {
      Log("local script " + path + ": " + (error.empty() ? "unreadable" : error));
    } else {
      s.queue.Append(script, 0);
      s.local_script = true;
      Log("local script " + path + ": " + std::to_string(script.frames.size()) + " frames, column " +
          std::to_string(s.slot));
    }
  }
  s.shot_frames = FrameList(TestDirEnv("DOA4_NP_SHOTS"));
  s.shot_every = std::strtoul(TestDirEnv("DOA4_NP_SHOT_EVERY").c_str(), nullptr, 10);
  s.dump_frames = FrameList(TestDirEnv("DOA4_NP_DUMP"));
  OpenHashFiles(s);
  det::Start(s.seed, s.dir);
  s.running.store(true);
  s.watch = std::thread(WatchLoop);
  g_active.store(true, std::memory_order_release);
  Log("session on: online, " + std::string(s.slot == 0 ? "host" : "joiner") + ", sweep " +
      std::to_string(s.sweep_per_frame) + " blocks/frame");
}

}  // namespace

bool Active() { return g_active.load(std::memory_order_relaxed); }

const FrameInput* StagedInput() { return Active() ? &S().staged : nullptr; }

uint32_t CurrentFrame() { return S().frame ? S().frame - 1 : 0; }

uint64_t SessionSeed() { return S().seed; }

uint64_t BuildIdValue() {
  BuildIdCache& c = Bid();
  std::call_once(c.once, [&] { c.value = ComputeBuildId(&c.unreadable); });
  return c.value;
}

const char* TestSwitch(const char* name) {
  if (!SwitchesAllowed(g_switch_online.load(), g_switch_test_dir.load())) return nullptr;
  return std::getenv(name);
}

void SetSwitchContext(bool online, bool test_dir) {
  g_switch_online.store(online);
  g_switch_test_dir.store(test_dir);
}

void EarlyLog(const std::string& line) {
  REXLOG_INFO("{}", line);
  std::lock_guard lk(g_early_mu);
  g_early.push_back(line);
}

bool ApplyGuestText(const std::string& text, std::string* why, bool* touched) {
  why->clear();
  bool any_set = false;
  std::string skipped;
  const std::string err = handoff::ApplyGuestSettings(
      text, [&](const std::string& name, const std::string& value) {
        // Fix round M1: only the three names this build is known not to register (np_restart.h
        // KnownUnregisteredSetting: nothing in this exe reads them, and both sides run the same build - the hand-off's
        // build id is checked first) are skipped; any other unknown name is refused like a refused value.
        if (!rex::cvar::GetFlagInfo(name)) {
          if (!restart::KnownUnregisteredSetting(name)) return false;
          skipped += (skipped.empty() ? "" : ",") + name;
          return true;
        }
        any_set = true;  // fix round I1: from here on this process holds host-imposed values
        return rex::cvar::SetFlagByName(name, value);
      });
  if (touched) *touched = any_set;
  g_skipped = skipped;
  if (!err.empty()) {
    *why = err;
    EarlyLog("WARN session: guest settings refused: " + err);
    return false;
  }
  bool same = true;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
    if (!rex::cvar::GetFlagInfo(name) && restart::KnownUnregisteredSetting(name)) {
      EarlyLog("[doa4][NET] guest setting " + name + "=" + value + " (not registered in this build: skipped)");
      continue;
    }
    const std::string back = rex::cvar::Query<std::string>(name);
    EarlyLog("[doa4][NET] guest setting " + name + "=" + value + " (read back " + back + ")");
    if (back != value) {
      same = false;
      EarlyLog("WARN session: guest setting " + name + " reads back " + back + ", not " + value);
      if (why->empty()) *why = "guest setting " + name + " reads back " + back + ", not " + value;
    }
  }
  return same;
}

void PrepareReplayRules() {
  State& s = S();
  if (Env("DOA4_NP") != "replay") return;
  std::string bytes, error, note;
  Recording rec;
  if (!ReadFile(Env("DOA4_NP_REC"), &bytes) || !DecodeRecording(bytes, &rec, &error, &note)) return;  // Start says why
  const std::string guest = MetaValue(rec.meta, "guest");
  const std::string unlock = MetaValue(rec.meta, "unlock");
  if (guest != "(none)") {
    std::string text = guest;
    std::replace(text.begin(), text.end(), ';', '\n');
    std::string why;
    if (ApplyGuestText(text, &why, nullptr)) {
      EarlyLog("replay: the recording's guest settings applied (R21)");
    } else {
      EarlyLog("replay: the recording's guest settings could not be applied: " + why);
    }
    s.guest_meta = guest;
  }
  if (unlock != "(none)") {
    s.unlock_meta = unlock;
    s.unlock = unlock == "1";
    EarlyLog("replay: the recording's unlock=" + unlock + (s.unlock ? " (the frame hook unlocks everything)" : ""));
  }
}

bool Online() { return S().mode == Mode::kOnline; }

// The hold banner is up (a hold has lasted >= 1 s and not ended yet); UI thread.
bool BannerWaiting() { return S().banner_shown.load(); }

int32_t SessionFrame() { return S().session_frame.load(); }

live::FrameResult LastResult() {
  State& s = S();
  std::lock_guard lk(s.result_mu);
  return s.last_result;
}

void RequestLeave() {
  State& s = S();
  const int32_t at = s.session_frame.load() + 1;
  int32_t cur = s.pending_leave.load();
  while (at < cur && !s.pending_leave.compare_exchange_weak(cur, at)) {
  }
  if (live::Session* l = s.live_ptr.load()) l->Leave(at);
}

void PeerLeft(int32_t frame, wire::LeaveReason reason) {
  State& s = S();
  s.pending_peer_left.store(uint64_t(uint8_t(reason)) << 32 | uint32_t(frame));
  if (live::Session* l = s.live_ptr.load()) l->PeerLeft(frame, reason);
}

NetTiming LastTiming() {
  State& s = S();
  std::lock_guard lk(s.timing_mu);
  return s.timing;
}

void Abort(const std::string& reason) {
  State& s = S();
  if (s.mode != Mode::kOnline || s.ended) return;
  live::FrameResult r;
  r.end = live::End::kInternal;
  r.end_frame = 0;
  r.why = reason;
  EndOnline(s, r, false);
}

// This file reaches the SDK's simde_mm_dp_ps redirect the way every generated file does
// (generated/default/dead_or_alive_4_init.h -> <rex/ppc.h> -> intrinsics.h -> dot_product.h), so the lambda below
// meets the same macro the recompiled code does.
std::string DotProductSelfTest() {
  return selftest::CanonicalNaN([](__m128 a, __m128 b) { return simde_mm_dp_ps(a, b, 0xFF); });
}

void Start(Host host) {
  State& s = S();
  if (host.online) {
    StartOnline(std::move(host));
    return;
  }
  const std::string mode = Env("DOA4_NP");
  if (mode != "record" && mode != "replay") return;
  s.mode = mode == "record" ? Mode::kRecord : Mode::kReplay;
  s.host = std::move(host);
  s.dir = std::filesystem::path(Env("DOA4_NP_DIR"));
  if (s.dir.empty()) {
    REXLOG_ERROR("[doa4][NP] DOA4_NP={} needs DOA4_NP_DIR; session off", mode);
    s.mode = Mode::kOff;
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(s.dir / "shots", ec);
  OpenLog(s);
  // The sweep setting is part of the recording's meta, so it is read before the recording is opened.
  ReadSweepSetting(s, Env("DOA4_NP_SWEEP"));
  // R21: a record run writes its own guest text and unlock=0; a replay writes the keys it applied
  // (PrepareReplayRules), and neither key when the recording has none (a phase-0 recording stays replayable:
  // "(none)" vs "(none)").
  std::string extra;
  if (s.mode == Mode::kRecord) {
    const std::string guest = handoff::CaptureGuestSettings(
        [](const std::string& name) { return rex::cvar::Query<std::string>(name); });
    s.guest_meta = GuestMeta(guest);
    s.unlock_meta = "0";
    extra = "guest=" + s.guest_meta + "\nunlock=0\n";
  } else {
    if (s.guest_meta != "(none)") extra += "guest=" + s.guest_meta + "\n";
    if (s.unlock_meta != "(none)") extra += "unlock=" + s.unlock_meta + "\n";
  }
  const std::string meta = CurrentMeta(s.host.game_data_root, s.sweep_per_frame, extra);

  std::string error;
  if (s.mode == Mode::kReplay) {
    std::string bytes, note;
    const std::filesystem::path rec = Env("DOA4_NP_REC");
    if (!ReadFile(rec, &bytes) || !DecodeRecording(bytes, &s.replay, &error, &note)) {
      // A replay that cannot start ends the run like a refusal does: left running it would boot a normal
      // game and wait at the title for a test tool's timeout (or for a person).
      Log("cannot replay " + rec.string() + ": " + (error.empty() ? "unreadable" : error));
      s.mode = Mode::kOff;
      if (s.host.quit) s.host.quit();
      return;
    }
    s.seed = s.replay.seed;
    Log("replay " + rec.string() + ": " + std::to_string(s.replay.frames.size()) + " frames, seed " +
        std::to_string(s.seed) + (note.empty() ? "" : " (" + note + ")"));
    // A recording made by another build, game data, fix set, worker mode, sweep setting or tweak state would end in
    // a desync nobody can read: refuse it, with the reason (DOA4_NP_ALLOW_MISMATCH=1 is for hunting).
    if (const std::string why = MetaMismatch(s.replay.meta, meta); !why.empty()) {
      if (Env("DOA4_NP_ALLOW_MISMATCH") != "1") {
        Log("refused: the recording was made with a different " + why);
        s.mode = Mode::kOff;
        if (s.host.quit) s.host.quit();
        return;
      }
      Log("replaying anyway (DOA4_NP_ALLOW_MISMATCH=1): " + why);
    }
  } else {
    if (const std::string seed = Env("DOA4_NP_SEED"); !seed.empty()) {
      s.seed = std::strtoull(seed.c_str(), nullptr, 0);
    }
    s.rec = _wfopen((s.dir / "rec.d4r").wstring().c_str(), L"wb");
    const std::string head = EncodeRecHeader(s.seed, 0, meta);
    if (s.rec) std::fwrite(head.data(), 1, head.size(), s.rec);
    if (const std::string path = Env("DOA4_NP_SCRIPT"); !path.empty()) {
      std::string text;
      Script script;
      if (!ReadFile(path, &text) || !ParseScript(text, &script, &error)) {
        Log("script " + path + ": " + (error.empty() ? "unreadable" : error));
      } else {
        s.queue.Append(script, 0);
        Log("script " + path + ": " + std::to_string(script.frames.size()) + " frames");
      }
    }
    // (the meta bytes of the header just written, not the meta string's size: a header written without
    // the meta must show 0 here)
    Log("record: seed " + std::to_string(s.seed) + ", " + std::to_string(head.size() - kRecHeaderBytes) +
        " bytes of meta");
  }
  if (const std::string q = Env("DOA4_NP_QUIT"); !q.empty()) s.quit_frame = std::strtoul(q.c_str(), nullptr, 10);
  s.shot_frames = FrameList(Env("DOA4_NP_SHOTS"));
  s.shot_every = std::strtoul(Env("DOA4_NP_SHOT_EVERY").c_str(), nullptr, 10);
  s.dump_frames = FrameList(Env("DOA4_NP_DUMP"));

  OpenHashFiles(s);
  det::Start(s.seed, s.dir);
  g_active.store(true, std::memory_order_release);
  Log(std::string("session on: ") + mode + ", sweep " + std::to_string(s.sweep_per_frame) +
      " blocks/frame, quit frame " +
      (s.quit_frame == UINT32_MAX ? std::string("none") : std::to_string(s.quit_frame)));
}

void LogLine(const std::string& line) { Log(line); }

// The writer captures through the presenter, so it must be done before the presenter goes. The race this
// closes (a queued shot captured while the presenter is torn down) cannot be forced by a test: the check
// is a GUARD, not a proof. A record run that quits on a shot frame must log that shot line before the
// "session writer joined" line, and the join line must be the last line. Probe, 3 runs each, writer slowed
// by 3 s: joined 0/3 leaked, detached 3/3 leaked (the capture ran after Shutdown had returned).
// The host's shutdown watchdog (5 s, armed in OnShutdown just before this) ends the process if the drain
// is slower: 36 queued shots took 1.9 s.
// Online: the session's transport stops first (bounded by 200 ms, Task 7); a session still running (the game was
// closed) ends as a leave without the offline relaunch; the watch and restart threads stop.
void Shutdown() {
  State& s = S();
  if (s.mode == Mode::kOff) return;
  if (s.mode == Mode::kOnline) {
    if (s.host.stop_transport) s.host.stop_transport();
    {
      std::lock_guard lk(s.watch_mu);
      s.watch_stop = true;
    }
    s.watch_cv.notify_all();
    if (s.watch.joinable()) s.watch.join();
    if (s.restarter.joinable()) s.restarter.join();
    if (!s.ended) {
      const int32_t f = s.session_frame.load() < 0 ? 0 : s.session_frame.load();
      s.ended = true;  // no offline relaunch: the player closed the game
      s.running.store(false);
      Log("session end: the game was closed");
      Log("session ended at frame " + std::to_string(f) + ": left");
      Log("checksums handed to GekkoNet: " + std::to_string(s.ex ? s.ex->saves_filled() : 0));
      Event(s, UnixMs(), "session ended at frame " + std::to_string(f) + ": left");
      if (s.live) s.live->Close();
      CloseFiles(s);
      det::Stop();
      g_active.store(false, std::memory_order_release);
    }
    std::lock_guard lk(s.events_mu);
    if (s.events) std::fclose(s.events), s.events = nullptr;
  } else {
    Quit(s, "shutdown", /*ask_host=*/false);  // nothing if the session already quit
  }
  size_t left = 0;
  {
    std::lock_guard lk(s.job_mu);
    s.stop = true;
    left = s.jobs.size();
  }
  s.job_cv.notify_all();
  if (s.writer.joinable()) s.writer.join();  // writes what is queued, then returns
  Log("session writer joined (" + std::to_string(left) + " jobs were queued at shutdown)");
  std::lock_guard lk(s.log_mu);
  if (s.log) std::fclose(s.log), s.log = nullptr;
}

}  // namespace doa4::np

// ---- Hooks ----

REX_HOOK_RAW(sub_824F4498) {
  if (doa4::np::Active()) doa4::np::OnFrame(ctx, base);
  __imp__sub_824F4498(ctx, base);
  // The frame function stores the host-timed GPU frame time into 0x831008E8 / 0x831008E4, which the
  // game reads (HoldFrameTiming): in a session they hold a 60 fps frame's values.
  if (doa4::np::Active() && doa4::np::det::On(doa4::np::det::kClock)) doa4::np::HoldFrameTiming(base);
}

// r3 = user (0-3), r4 = X_INPUT_STATE* (be32 packet number, then the 12-byte
// XINPUT_GAMEPAD). Returns r3 = 0 (connected) or ERROR_DEVICE_NOT_CONNECTED.
REX_HOOK_RAW(sub_82792040) {
  const doa4::np::FrameInput* in = doa4::np::StagedInput();
  if (!in) {
    __imp__sub_82792040(ctx, base);
    return;
  }
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  if (user >= uint32_t(doa4::np::kUsers) || !(in->connected & (1u << user))) {
    ctx.r3.u64 = doa4::np::kNotConnected;
    return;
  }
  if (state) {
    rex::memory::store_and_swap<uint32_t>(base + state, doa4::np::CurrentFrame());
    doa4::np::PadToGuest(in->pad[user], base + state + 4);
  }
  ctx.r3.u64 = 0;
}

// r3 = user, r4 = flags, r5 = X_INPUT_CAPABILITIES*: a standard wired gamepad
// for a connected user.
REX_HOOK_RAW(sub_82792038) {
  const doa4::np::FrameInput* in = doa4::np::StagedInput();
  if (!in) {
    __imp__sub_82792038(ctx, base);
    return;
  }
  const uint32_t user = ctx.r3.u32;
  const uint32_t caps = ctx.r5.u32;
  if (user >= uint32_t(doa4::np::kUsers) || !(in->connected & (1u << user))) {
    ctx.r3.u64 = doa4::np::kNotConnected;
    return;
  }
  if (caps) {
    uint8_t* p = base + caps;
    std::memset(p, 0, 20);
    p[0] = 1;  // XINPUT_DEVTYPE_GAMEPAD
    p[1] = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    doa4::np::Pad all;
    all.buttons = 0xF3FF;
    all.lt = all.rt = 0xFF;
    all.lx = all.ly = all.rx = all.ry = int16_t(0xFFC0);
    doa4::np::PadToGuest(all, p + 4);
    p[16] = p[17] = p[18] = p[19] = 0xFF;  // vibration motors
  }
  ctx.r3.u64 = 0;
}
