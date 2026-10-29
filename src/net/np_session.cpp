// DOA4 netplay phase 0: the record / replay / hash session. See np_session.h.
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
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>

#include "generated/default/dead_or_alive_4_init.h"

#include "../doa4_photo_win.h"
#include "../doa4_tweaks.h"
#include "np_core.h"
#include "np_det.h"
#include "np_det_internal.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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

enum class Mode { kOff, kRecord, kReplay };

struct Job {
  enum Kind { kShot, kDump } kind;
  uint32_t frame;
  std::vector<uint8_t> bytes;  // kDump
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
  std::vector<uint64_t> row;
  // Per sweep block k: its address range and the kSweepMasks inside it, built once in Start so that
  // hashing a block never allocates.
  std::vector<Region> block_regions;
  bool timing_logged = false;  // HoldFrameTiming's one log line

  // timing (QPC ticks)
  double qpc_us = 0;
  int64_t last_frame_qpc = 0;
  double sum_hook_us = 0, max_hook_us = 0, sum_hash_us = 0, max_hash_us = 0, sum_period_us = 0;
  double max_period_us = 0;
  double sum_det_us = 0, max_det_us = 0;
  uint32_t window = 0;

  // writer thread
  std::thread writer;
  std::mutex job_mu;
  std::condition_variable job_cv;
  std::deque<Job> jobs;
  bool stop = false;

  // A game-requested exit() (HalReturnToFirmware, "return to the dashboard": sdk xboxkrnl_hal.cpp:34)
  // runs the static destructors without OnShutdown, so the writer may still be joinable here, and a
  // joinable std::thread's destructor calls std::terminate. Detach it: the process is going away.
  ~State() {
    if (writer.joinable()) writer.detach();
  }
};

State& S() {
  static State s;
  return s;
}

std::atomic<bool> g_active{false};

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

// The exe and the runtime DLL beside it (XXH3), as 16 hex digits: two PCs replay one recording only
// with the same build.
std::string BuildId() {
  std::wstring path(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  const std::filesystem::path exe(path.substr(0, n));
  uint64_t h = 0;
  for (const std::filesystem::path& p : {exe, exe.parent_path() / "rexruntime.dll"}) {
    std::string bytes;
    if (ReadFile(p, &bytes)) h = XXH3_64bits_withSeed(bytes.data(), bytes.size(), h);
  }
  return fmt::format("{:016x}", h);
}

// The recording's meta text (np_core.h): what a replay must match to run at all. The tweaks patch the
// game's tables at boot (SetUpExtras, before this runs), so their state is part of the build.
std::string CurrentMeta(uint32_t sweep_per_frame) {
  std::string tweaks;
  for (const auto& t : doa4::tweaks::AllTweaks()) {
    tweaks += fmt::format("{}{}={}", tweaks.empty() ? "" : ",", t.cvar, rex::cvar::Query<bool>(t.cvar) ? 1 : 0);
  }
  const std::string serial = Env("DOA4_NP_SERIAL");
  return "build=" + BuildId() + "\nfixes=" + det::FixSetName() + "\nserial=" +
         (serial.empty() ? std::string("default") : serial) + "\nsweep=" + std::to_string(sweep_per_frame) +
         "\ntweaks=" + tweaks + "\n";
}

// Reads and deletes a request file; false if it is not there.
bool Take(const char* name, std::string* text) {
  const std::filesystem::path p = S().dir / name;
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return false;
  if (!ReadFile(p, text)) return false;
  std::filesystem::remove(p, ec);
  return true;
}

double Now(State& s) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * s.qpc_us;
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
  if (std::getenv("DOA4_NP_EXTRA_REGIONS")) {  // hunt: the stage-effect timers and a vblank-like counter
    r.push_back({"stage_fx", 0x82EDA260, 0x4A0, {}});
    r.push_back({"ctr_831A7AC0", 0x831A7AC0, 0x20, {}});
  }
  return r;
}

// Ends the session: closes the recording and the hash files, switches the determinism layer off and
// (ask_host) asks the host to quit. Callers: OnFrame's replay-end check and quit-frame check (the game's
// main thread; they ask the host) and Shutdown (the host is already shutting down: it does not).
void Quit(State& s, const std::string& why, bool ask_host = true) {
  if (s.quitting) return;
  s.quitting = true;
  Log(std::string("quit at frame ") + std::to_string(s.frame ? s.frame - 1 : 0) + ": " + why);
  if (s.rec) std::fclose(s.rec), s.rec = nullptr;
  if (s.digest) std::fclose(s.digest), s.digest = nullptr;
  if (s.sweep) std::fclose(s.sweep), s.sweep = nullptr;
  if (s.counters) std::fclose(s.counters), s.counters = nullptr;
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
    if (text.rfind("quit", 0) == 0) s.quit_frame = std::min(s.quit_frame, f);
    if (text.rfind("dump", 0) == 0) s.dump_frames.insert(s.dump_frames.begin(), f);
  }
}

void HashFrame(State& s, uint8_t* base, uint32_t f) {
  if (s.digest) {
    s.row.assign(s.regions.size(), 0);
    for (size_t i = 0; i < s.regions.size(); ++i) {
      const Region& r = s.regions[i];
      if (r.masks.empty()) {
        s.row[i] = XXH3_64bits(base + r.address, r.length);
      } else {
        MaskedCopy(r, base + r.address, &s.scratch);
        s.row[i] = XXH3_64bits(s.scratch.data(), s.scratch.size());
      }
    }
    const uint32_t head[2] = {f, 0};
    std::fwrite(head, sizeof(head), 1, s.digest);
    std::fwrite(s.row.data(), sizeof(uint64_t), s.row.size(), s.digest);
  }
  if (s.sweep) {
    const uint32_t nblocks = (kDataHi - kDataLo) / kBlock;
    const uint32_t first = SweepFirst(f, s.sweep_per_frame, nblocks);
    s.row.assign(s.sweep_per_frame, 0);
    for (uint32_t i = 0; i < s.sweep_per_frame; ++i) {
      const uint32_t k = (first + i) % nblocks;
      const uint32_t lo = kDataLo + k * kBlock;
      const Region& r = s.block_regions[k];
      if (r.masks.empty()) {
        s.row[i] = XXH3_64bits(base + lo, kBlock);
      } else {
        MaskedCopy(r, base + lo, &s.scratch);
        s.row[i] = XXH3_64bits(s.scratch.data(), s.scratch.size());
      }
    }
    const uint32_t head[2] = {f, first};
    std::fwrite(head, sizeof(head), 1, s.sweep);
    std::fwrite(s.row.data(), sizeof(uint64_t), s.row.size(), s.sweep);
  }
  if (s.counters) {
    det::CounterValues(&s.counter_row);
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

// The frame hook: runs at the loop head before frame f. The state hashed here
// is the state entering frame f (everything frame f - 1 did).
void OnFrame(PPCContext& ctx, uint8_t* base) {
  State& s = S();
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
  HashFrame(s, base, f);
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

  // 2. Requests, then the input of frame f.
  if ((f % 15) == 0) PollRequests(s, f);
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

}  // namespace

bool Active() { return g_active.load(std::memory_order_relaxed); }

const FrameInput* StagedInput() { return Active() ? &S().staged : nullptr; }

uint32_t CurrentFrame() { return S().frame ? S().frame - 1 : 0; }

uint64_t SessionSeed() { return S().seed; }

void Start(Host host) {
  State& s = S();
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
  s.log = _wfopen((s.dir / "np_log.txt").wstring().c_str(), L"w");
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  s.qpc_us = 1e6 / double(freq.QuadPart);
  // The sweep setting is part of the recording's meta, so it is read before the recording is opened.
  const std::string sweep = Env("DOA4_NP_SWEEP");
  const uint32_t nblocks = (kDataHi - kDataLo) / kBlock;
  if (sweep == "all") {
    s.sweep_per_frame = nblocks;
  } else if (!sweep.empty()) {
    s.sweep_per_frame = std::clamp<uint32_t>(std::strtoul(sweep.c_str(), nullptr, 10), 1, nblocks);
  }
  const std::string meta = CurrentMeta(s.sweep_per_frame);

  std::string error;
  if (s.mode == Mode::kReplay) {
    std::string bytes, note;
    const std::filesystem::path rec = Env("DOA4_NP_REC");
    if (!ReadFile(rec, &bytes) || !DecodeRecording(bytes, &s.replay, &error, &note)) {
      Log("cannot replay " + rec.string() + ": " + (error.empty() ? "unreadable" : error));
      s.mode = Mode::kOff;
      return;
    }
    s.seed = s.replay.seed;
    Log("replay " + rec.string() + ": " + std::to_string(s.replay.frames.size()) + " frames, seed " +
        std::to_string(s.seed) + (note.empty() ? "" : " (" + note + ")"));
    // A recording made by another build, fix set, worker mode, sweep setting or tweak state would end in
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
        " bytes in " + std::to_string(blocks) + " blocks hashed as zero");
  }
  s.counters = _wfopen((s.dir / "counters.d4k").wstring().c_str(), L"wb");
  if (s.counters) {
    const std::string head = EncodeCountersHeader(det::CounterNames());
    std::fwrite(head.data(), 1, head.size(), s.counters);
  }
  s.writer = std::thread(WriterLoop);
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
void Shutdown() {
  State& s = S();
  if (s.mode == Mode::kOff) return;
  Quit(s, "shutdown", /*ask_host=*/false);  // nothing if the session already quit
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
