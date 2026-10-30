// DOA4 netplay phase 0: the pure parts of the record / replay / hash tooling
// (no SDK, no Windows), host-tested by tests/host/np_core_host_test.cpp.
//
// - Pad scripts: the text a test writes to drive users 0-3 frame by frame.
// - Recordings: the raw 12-byte XINPUT_GAMEPAD of users 0-3 per frame, as
//   injected, plus the session seed.
// - Digest and sweep files: per-frame hashes of chosen guest regions and of a
//   rolling window of 64 KB blocks of .data, compared by tools/np_compare.py.
//
// File layouts are little-endian on disk except the gamepad bytes, which are
// stored exactly as the guest reads them (big-endian XINPUT_GAMEPAD).

#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace doa4::np {

inline constexpr int kUsers = 4;

// XINPUT_GAMEPAD in host byte order.
struct Pad {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const Pad&) const = default;
};

// One frame of input: users 0-3.
struct FrameInput {
  uint8_t connected = 0;  // bit u: user u is connected
  std::array<Pad, kUsers> pad{};
  bool operator==(const FrameInput&) const = default;
};

// XINPUT_GAMEPAD button bits.
enum Button : uint16_t {
  kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008,
  kStart = 0x0010, kBack = 0x0020, kLThumb = 0x0040, kRThumb = 0x0080,
  kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};

// The 12 guest bytes of a gamepad (big-endian XINPUT_GAMEPAD) and back.
void PadToGuest(const Pad& pad, uint8_t out[12]);
Pad PadFromGuest(const uint8_t in[12]);

// ---- Pad scripts ----
// One command per line; '#' starts a comment.
//   connect U [U ...]     users connected from this line on (the default is user 0)
//   <frames> <u0> [<u1> [<u2> [<u3>]]]
//                         hold for <frames> frames. Each <uN> is '-' (neutral) or
//                         items joined by '+': A B X Y UP DOWN LEFT RIGHT START BACK
//                         LB RB LS RS, or LT=n RT=n LX=n LY=n RX=n RY=n. Users not
//                         named are neutral.
//   random <frames> <seed> <u> [<u> ...]
//                         pseudo-random fighting input for the named users
//                         (directions and A/B/X/Y held 2-12 frames), a pure
//                         function of the seed
//   shot                  a screenshot when this point is reached
//   quit                  end the run when this point is reached
struct ScriptEvent {
  enum Kind { kShot, kQuit };
  Kind kind = kShot;
  uint32_t frame = 0;  // relative to the script's first frame
  bool operator==(const ScriptEvent&) const = default;
};

struct Script {
  std::vector<FrameInput> frames;  // the input of each frame from the script's start
  std::vector<ScriptEvent> events;
  // Frames before the script's first connect line: parsed with user 0
  // connected, but a script appended to a running queue gives them the
  // connected set the queue had (a request file without a connect line must
  // not unplug a pad).
  size_t inherit_frames = 0;
};

// Returns false and the line and reason in *error on a malformed script.
bool ParseScript(std::string_view text, Script* out, std::string* error);

// Live input: scripts appended while the game runs (request files) play one
// after the other; when the queue is empty the input is neutral with the last
// connected set. Events fire at absolute frames.
class InputQueue {
 public:
  // Appends `script` to play after everything queued, starting no earlier than
  // `now` (the frame being staged). Returns the absolute first frame.
  uint32_t Append(const Script& script, uint32_t now);
  // The input of frame `frame` (called once per frame, frames increasing);
  // events due at this frame are appended to *fired.
  FrameInput Next(uint32_t frame, std::vector<ScriptEvent::Kind>* fired);
  uint32_t queued_until() const { return end_; }  // first frame with no queued input

 private:
  struct Item {
    uint32_t frame;
    FrameInput input;
  };
  std::vector<Item> items_;  // in frame order
  std::vector<ScriptEvent> events_;  // absolute frames
  size_t next_item_ = 0;
  size_t next_event_ = 0;
  uint32_t end_ = 0;
  uint8_t connected_ = 1;  // of the last frame played
  uint8_t tail_connected_ = 1;  // of the last frame queued
};

// ---- Recordings ----
// Header (32 bytes): "D4NPREC1", u32 version (2; 1 = the proof's, still read), u32 frame bytes (52),
// u64 session seed, u32 flags, u32 meta bytes (0 in version 1). Version 2 then holds the meta text
// ("key=value" lines: build, data, fixes, serial, sweep, tweaks) before the frames. Then one 52-byte record
// per frame from frame 0: u8 connected mask, 3 zero bytes, 4 x 12 guest gamepad bytes.
// A recording cut short by a crash is still readable (whole frames only).
inline constexpr size_t kRecHeaderBytes = 32;
inline constexpr size_t kRecFrameBytes = 52;

// Always writes version 2; `meta` = "key=value" lines.
std::string EncodeRecHeader(uint64_t seed, uint32_t flags, std::string_view meta = {});
std::string EncodeRecFrame(const FrameInput& input);

struct Recording {
  uint64_t seed = 0;
  uint32_t flags = 0;
  std::string meta;  // empty for version 1
  std::vector<FrameInput> frames;
};
// Returns false with the reason on a bad header; a trailing partial frame is
// dropped (reported in *note).
bool DecodeRecording(std::string_view bytes, Recording* out, std::string* error,
                     std::string* note = nullptr);
// "" when the keys build, data, fixes, serial, sweep and tweaks of two meta texts agree (a key missing
// from one text, as in a version-1 recording, reads "(none)"); else "<key> <recorded> vs <current>; ..."
// in that key order. Other keys are ignored. The value "unreadable" (the exe or the game data could not
// be hashed) agrees with nothing, itself included.
std::string MetaMismatch(std::string_view recorded, std::string_view current);

// ---- Fix sets (DOA4_NP_FIX; the recording's meta key "fixes") ----
// The determinism layer's fixes in bit order: fix i is bit 1u << i (np_det_internal.h Fix, pinned there).
inline constexpr const char* kFixNames[] = {"seed",  "clock",  "sdkclock", "xam",    "audio",     "movie", "loader",
                                            "cri",   "crijob", "workers",  "notify", "keepalive", "reader"};
inline constexpr int kFixCount = int(sizeof(kFixNames) / sizeof(kFixNames[0]));
// "all", empty or null: every bit (~0u); "none": 0; "a,b,c": those fixes; "all-a-b": all but a and b. Names that are
// not fixes are ignored.
uint32_t ParseFixSet(const char* text);
// The names of the fixes in `bits`, comma separated in fix order, or "none": the text the session logs and the
// recording's meta holds ("fixes=" + this), so a recording made with another set is refused (MetaMismatch).
std::string FixSetText(uint32_t bits);

// ---- Digest regions ----
// A guest region hashed every frame, minus masked byte ranges (host-timing
// words), which are hashed as zero.
struct Mask {
  uint32_t offset = 0;
  uint32_t length = 0;
};
struct Region {
  const char* name = "";
  uint32_t address = 0;
  uint32_t length = 0;
  std::vector<Mask> masks;
};

// Copies [address, address + length) of guest memory from `bytes` into `out`
// with every mask zeroed; `out` is resized to the region's length.
void MaskedCopy(const Region& region, const uint8_t* bytes, std::vector<uint8_t>* out);

// Digest file: "D4NPDIG1", u32 version (1), u32 region count, then per region
// u32 address, u32 length, char name[24]; then per frame u32 frame, u32 0,
// u64 hash per region.
std::string EncodeDigestHeader(const std::vector<Region>& regions);
// Sweep file: "D4NPSWP1", u32 version (1), u32 lo, u32 hi, u32 block,
// u32 blocks per frame; then per frame u32 frame, u32 first block, u64 hash per
// block (blocks first, first + 1, ... wrapping at the end of the range).
std::string EncodeSweepHeader(uint32_t lo, uint32_t hi, uint32_t block, uint32_t per_frame);
// The first block index of `frame`'s batch.
uint32_t SweepFirst(uint32_t frame, uint32_t per_frame, uint32_t nblocks);
// Per sweep block (lo + i * block, i < nblocks), the masks of the guest ranges {address, length}
// that touch it, as offsets within the block; a range across a block border is split.
std::vector<std::vector<Mask>> BlockMasks(uint32_t lo, uint32_t block, uint32_t nblocks,
                                          const std::vector<std::pair<uint32_t, uint32_t>>& ranges);
// BlockMasks as ready-made Regions: element k is {"", lo + k * block, block, block k's masks}. The sweep
// builds them once at Start and hashes through them (MaskedCopy), so hashing a masked block allocates
// nothing; the bytes hashed are those of Region{"", lo + k * block, block, BlockMasks(...)[k]}.
std::vector<Region> BlockRegions(uint32_t lo, uint32_t block, uint32_t nblocks,
                                 const std::vector<std::pair<uint32_t, uint32_t>>& ranges);

// ---- The SDK's handle stash in guest object headers (left out of the .data sweep, Task 11c) ----
// The first time the SDK uses a guest dispatcher object (an event, mutant or semaphore in guest memory) it creates a
// host object and stashes its handle in the guest header (sdk-np0 include/rex/system/xobject.h StashHandle):
// wait_list_flink = kXObjSignature = make_fourcc('R', 'E', 'X', '\0') = 0x52455800, stored big-endian, so the bytes
// "REX\0" lie in guest memory, and the next word, wait_list_blink, = the handle. The handle is the object table's next
// free slot, a host-timed number at boot (single_b con3: 0x82B74778 = F800005C against F8000054), and only the SDK
// reads it back (no guest code reads it). So the sweep hashes every 4-aligned word that follows the signature as
// zero, by structure (no address list). The signature word itself is hashed as it is.
inline constexpr uint8_t kStashSignature[4] = {0x52, 0x45, 0x58, 0x00};
// `bytes` = `length` bytes of guest memory from a 4-aligned guest address (length a multiple of 4); `before` = the 4
// guest bytes just before bytes[0], or null where there are none. True if some word of `bytes` follows a signature
// (a signature in the last word belongs to the next range's first word).
bool HasStashWords(const uint8_t* bytes, size_t length, const uint8_t* before);
// Zeroes every word of `bytes` that follows a signature, deciding from the bytes as they were before any was zeroed
// (a signature that is itself a stash word still marks the word after it). Returns the number of words zeroed.
uint32_t ZeroStashWords(uint8_t* bytes, size_t length, const uint8_t* before);
// The bytes the sweep hashes for one block: a copy of the region's guest bytes, its stash words zeroed (decided on the
// copy as made), then its masks zeroed (as MaskedCopy). Returns the number of stash words zeroed.
uint32_t SweepBlockCopy(const Region& region, const uint8_t* bytes, const uint8_t* before, std::vector<uint8_t>* out);

// Counter file: "D4NPCNT1", u32 version (1), u32 counter count, then per counter char name[24];
// then per frame u32 frame, u32 0, u32 value per counter (values as they are when the frame's
// state is hashed). Names starting "fb_" count fallbacks (a wait gave up and the game ran on: must
// stay 0), "pr_" count progress (equal in every run of one recording).
std::string EncodeCountersHeader(const std::vector<std::string>& names);
std::string EncodeCountersFrame(uint32_t frame, const std::vector<uint32_t>& values);

// ---- Virtual clocks (the determinism layer's "clock" and "sdkclock" fixes) ----
// Time derived from the session frame. Every read of a thread advances its clock a little within
// a frame (10 us of time base, 1 ms per 16 tick-count reads, 10 us of FILETIME), so a loop that
// waits on the clock still ends (Tekken 6 t6_determinism.cpp); a thread's clock never runs
// backwards, also when its reads in one frame went past the next frame's start (the next frame
// then continues from the last value).
inline constexpr uint64_t kTimebaseHz = 50000000;                 // KeQueryPerformanceFrequency
inline constexpr uint64_t kTimebaseBase = 1ull << 40;             // far from 0, the same in every session
inline constexpr uint32_t kTickBase = 600000;                     // GetTickCount at frame 0 (ms)
inline constexpr uint64_t kFileTimeBase = 0x01DB000000000000ull;  // a fixed date (FILETIME, 100 ns)

struct ThreadClock {
  uint32_t frame = ~0u;
  uint32_t tb_reads = 0, tick_reads = 0, ft_reads = 0;
  uint64_t last_tb = 0, last_ft = 0;
  uint32_t last_tick = 0;
};
uint64_t VirtualTimebase(ThreadClock* c, uint32_t frame);   // strictly increasing per thread
uint32_t VirtualTickCount(ThreadClock* c, uint32_t frame);  // never decreasing per thread
uint64_t VirtualFileTime(ThreadClock* c, uint32_t frame);   // strictly increasing per thread

// The SDK audio worker's clock while it runs a guest audio callback (rex_audio_worker_callback):
// time from the callback number since the lockstep started, 256 samples at 48 kHz per callback.
uint64_t AudioTimebase(uint64_t callback);
uint32_t AudioTickCount(uint64_t callback);
uint64_t AudioFileTime(uint64_t callback);

// ---- Mersenne Twister seeds (the determinism layer's "seed" fix) ----
// Every seeding of one of the game's MTs (sub_82334C60, state = the MT's address) gets a value derived
// from the session seed, the MT's address and k, the number of times that MT was seeded before in the
// session: every run of one recording draws the same, and a later seeding does not repeat an earlier one.
// k = 0 is the value the proof gave.
uint32_t MtSeed(uint64_t session_seed, uint32_t state, uint32_t k);

class SeedSeries {
 public:
  uint32_t Next(uint64_t session_seed, uint32_t state);  // the seed for `state`'s next seeding
  void Reset();                                          // a new session counts from the start
 private:
  std::mutex mu_;
  std::unordered_map<uint32_t, uint32_t> seeded_;  // MT address -> seedings so far
};

// ---- The D3D runtime's keep-alive re-present (the determinism layer's "keepalive" fix) ----
// The D3D runtime's kick thread (entry 0x8275BD18, created by D3D itself with ExCreateThread) waits for a pending
// swap with a 30 ms HOST timeout; when the wait times out it re-presents the last frame: sub_8275AD70 from lr
// 0x8275BD9C, then sub_8275AEE0 (the VdSwap wrapper) from lr 0x8275BDAC. Each call advances the D3D CPU fence
// counter (device+10396), which the main thread later stamps into static texture headers and into a pointer the
// game's dynamic-buffer allocator reads, so how many happen in a frame is a function of host speed (Task 11a).
// In a session those two calls return at once. The main thread's own present calls the same two functions (from
// sub_82765AE0) and is never touched.
inline constexpr uint32_t kD3dKickThreadStart = 0x8275BD18;
enum class KeepAliveFn { kAd70, kAee0 };  // sub_8275AD70, sub_8275AEE0
// The return address of the kick thread's timeout re-present call to `fn` (the instruction after its bl).
uint32_t KeepAliveLr(KeepAliveFn fn);
enum class KeepAliveCall {
  kPass,        // not the kick thread: run the call
  kSkip,        // the kick thread's timeout re-present: return at once
  kUnexpected,  // the kick thread calling `fn` from another site: run the call, log one WARN, count it
};
// thread_start = the calling guest thread's entry point (0: not a guest thread), lr = the call's return address.
KeepAliveCall ClassifyKeepAlive(KeepAliveFn fn, uint32_t thread_start, uint32_t lr);

// ---- The save manager's one-shot file thread (the determinism layer's "reader" fix) ----
// sub_82731790 (called by the save manager) creates the file thread 0x82731758 suspended, keeps its handle at
// 0x82E476BC, resumes it and returns 1 (-1: the thread could not be created). The thread does one file operation
// and exits; the main thread polls the handle with a zero wait every frame, so under load the save manager went on
// a frame later (Task 11a). In a session the caller waits (up to 2 s) after sub_82731790 until the file thread's
// host thread has ended.
inline constexpr uint32_t kReaderThreadHandle = 0x82E476BC;
inline constexpr int kReaderWaitMs = 2000;
enum class ReaderWait {
  kNoThread,  // sub_82731790 did not start a thread: nothing to wait for
  kEnded,     // the file thread's host thread ended within the wait
  kFallback,  // the thread was not found or had not ended after kReaderWaitMs: the game runs on (fb_reader)
};
// ret = sub_82731790's return value; found = the handle at kReaderThreadHandle names a guest thread with a host
// thread; ended = that host thread ended within the wait (only read when found).
ReaderWait ReaderOutcome(int32_t ret, bool found, bool ended);

}  // namespace doa4::np
