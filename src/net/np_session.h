// DOA4 netplay phase 0: the record / replay / hash session (proof of concept).
//
// Inert unless DOA4_NP is set (offline play runs the original code: every
// hook checks one bool and calls the original). Configuration, all optional
// except DOA4_NP and DOA4_NP_DIR:
//   DOA4_NP=record|replay   record: input from DOA4_NP_SCRIPT and request files
//                           (np_pad.on); replay: input from DOA4_NP_REC
//   DOA4_NP_DIR=<folder>    outputs: np_log.txt, rec.d4r (record), digest.d4d,
//                           sweep.d4s, counters.d4k, shots/, dumps/
//   DOA4_NP_SCRIPT=<file>   pad script played from frame 0 (record)
//   DOA4_NP_REC=<file>      recording to replay; the run quits after its last frame. A recording
//                           made by another build, game data (default.xex), fix set, worker mode
//                           (DOA4_NP_SERIAL), sweep setting or Extras tweak state is refused, and
//                           so is one that cannot be read: np_log.txt gets a "refused: ..." or
//                           "cannot replay ..." line and the host is asked to quit, with no session
//   DOA4_NP_ALLOW_MISMATCH=1  replay a recording of another build etc. anyway (hunting only; a
//                           recording that cannot be read is never replayed)
//   DOA4_NP_SEED=<n>        session seed written into the recording (record)
//   DOA4_NP_QUIT=<frame>    quit when this frame is reached
//   DOA4_NP_SHOTS=f,f,...   screenshots (the presenter's guest output) at frames
//   DOA4_NP_SHOT_EVERY=<n>  a screenshot every n frames
//   DOA4_NP_SWEEP=<n>|all   64 KB .data blocks hashed per frame (default 4)
//   DOA4_NP_DUMP=f,f,...    full .data dumps at frames (dumps/data_<frame>.bin)
//   DOA4_NP_HEAPDUMP=1      each dump also copies the committed pages of the guest heaps 00000000 and
//                           40000000 (dumps/heap_<frame>.bin, region index heap_<frame>.txt; debug)
// Request files in DOA4_NP_DIR, taken (deleted) when read, polled every 15 frames:
//   np_pad.on   a pad script appended to the input queue (record only)
//   np_shot.on  a screenshot now
//   np_ctl.on   "quit" (the run ends after this frame, as a quit frame) | "dump"

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "../doa4_guest.h"
#include "../doa4_photo.h"
#include "np_handoff.h"
#include "np_live.h"
#include "np_wire.h"

// Netplay phase 1 (Task 12): the ONLINE mode. A process started for an online session (the app read a hand-off at
// OnPostInitLogging, Host::online set) runs the same frame hook in lockstep through np_live: (1) hash the state
// entering f and fold the checksum, (2) dumps / requests, (3) the unlock (every frame), (4) det::OnFrame (R1: the
// determinism boundary before any wait), (5) the local pad (the test script's column, else the real pad),
// (6) live::Session::Frame (blocks until frame f's inputs are confirmed; the input it returns is staged for the
// injection hooks), (7) shots, (8) the 600-frame timing line. The session writes into Host::session_dir: np_log.txt,
// rec.d4r (the confirmed-input tap), digest.d4d, sweep.d4s, counters.d4k (rows of the frames that advanced), events.txt.
// At the end: "session ended at frame F: <reason>", "checksums handed to GekkoNet: N", the Leave to the peer, the det
// layer stopped, and after 5 s (8 s for a desync) Host::restart(false) (the offline relaunch).

namespace doa4::np {

namespace net {
class Transport;
}

struct Host {
  // The presenter's current guest output (any thread). False if none yet.
  std::function<bool(doa4::photo::Image*)> capture;
  // Ends the process the normal way (posted to the UI thread).
  std::function<void()> quit;
  // The folder the game runs from (the one holding default.xex): the recording's meta holds a hash of
  // its default.xex, so a replay on another dump of the game is refused.
  std::filesystem::path game_data_root;

  // ---- online (Task 12) ----
  std::optional<handoff::Handoff> online;            // set when this process was started for a session
  std::filesystem::path session_dir;                 // where the session writes (ctl/session or <U>/netplay/sessions/..)
  std::function<bool(Pad* pad)> read_local_pad;      // InputSystem::GetState(0): the call XamInputGetState makes for user 0
  std::function<void(bool with_handoff)> restart;    // relaunch this exe (DOA4_WAIT_PID) and quit
  std::function<void()> overlay_changed;             // the banner or session state changed (posts to the UI thread)
  const GuestBytes* guest = nullptr;                 // for unlocks::FindProfile / ApplyUnlock
  std::function<void(int32_t frame, wire::LeaveReason reason)> send_leave;  // Lobby::SendLeave of the session lobby
  std::function<net::Transport*()> transport;        // the session lobby's transport, asked for at the first frame hook
                                                     // (the app starts the lobby right after np::Start)
  // Additions (Task 12):
  std::filesystem::path test_dir;                    // DOA4_NP_TEST_DIR or empty (the local script, request files)
  std::string release;                               // DOA4_RELEASE_TAG (np_log's first line)
  std::function<bool()> session_refused;             // the session lobby's Status::session_refused (carry 2g)
  std::function<void()> stop_transport;              // Shutdown's first step: the session lobby's transport Stop
  std::function<int(int32_t frame)> quit_session;    // Task 15 G2: Shutdown of a running session: Leave, flush (<= 150 ms), Stop;
                                                     // returns the sends handed to the socket (of 3)
};

// Reads the configuration; called once before any guest code runs.
void Start(Host host);
// Ends the session (if one runs), writes the shots and dumps still queued, joins the writer and
// closes the session log. Called first in OnShutdown, while the presenter still exists.
void Shutdown();
bool Active();

// ---- Task 12 ----
// The build id phase 0 prints as 16 hex digits (XXH3 of the exe and rexruntime.dll), as a number; 0 when either file
// cannot be read (Task 3's Judge refuses 0). Computed once.
uint64_t BuildIdValue();
// R22: the phase-0 test switches go through here: nullptr unless SwitchesAllowed(online, test dir set) (np_core.h).
const char* TestSwitch(const char* name);
// Set by the app at OnPostInitLogging, before the game's first thread (and before anything reads a switch).
void SetSwitchContext(bool online, bool test_dir);
// Lines logged before np_log.txt exists (the guest-setting lines of OnPostInitLogging): game.log now, np_log.txt at
// Start.
void EarlyLog(const std::string& line);
// Applies a guest settings text (handoff::ApplyGuestSettings through rex::cvar::SetFlagByName), then reads every name
// back and logs "[doa4][NET] guest setting <name>=<value> (read back <value>)". False (and *why) when the text is
// refused or any read-back differs ("WARN session: guest setting ..." logged).
// *touched (may be null): whether any setting was set before the outcome (fix round I1: a failure after a set means
// this process holds host-imposed values and must not go on). Only the three KnownUnregisteredSetting names
// (np_restart.h) are skipped when the registry does not know them; they are recorded in the session meta
// (guest_skipped=).
bool ApplyGuestText(const std::string& text, std::string* why, bool* touched);
// Replay parity (R21): at OnPostInitLogging, a replay (DOA4_NP=replay) reads its recording's meta and applies the
// recording's guest settings (when it has any) exactly as a session does; the unlock (meta unlock=1) runs in the frame
// hook. No-op otherwise.
void PrepareReplayRules();
bool Online();
bool BannerWaiting();                 // a hold has lasted >= 1 s and is still on (any thread; the overlay's banner)
int32_t SessionFrame();               // the last frame hook's frame (-1 before the first)
live::FrameResult LastResult();
// Review I2: the other player's game answered as a lobby (the refused-session end); read by the banner.
bool SessionRefused();
void RequestLeave();                  // UI: this side leaves at the next frame
void PeerLeft(int32_t frame, wire::LeaveReason reason);  // the session lobby's on_peer_left (I/O thread)
// Ends a session that cannot run (before or at its first frame): the terminal lines and the offline relaunch.
void Abort(const std::string& reason);
struct NetTiming {
  int32_t frame = -1;
  int delay = 0;
  float ping_ms = 0, frames_ahead = 0;
  double net_us = 0, wait_us = 0, pacer_us = 0, hook_us = 0, hash_us = 0;
  uint32_t holds = 0;
};
NetTiming LastTiming();  // written by the game thread at the end of each frame hook (copy under its own mutex)
// The boot self-test of the dot-product NaN redirect (np_selftest.h): "on" when this build's simde_mm_dp_ps returns
// the canonical NaN, else "OFF (<lane bit patterns>)". It touches no session state: callable at any time, and the
// app logs it once at start-up.
std::string DotProductSelfTest();

// For the other netplay files (np_det.cpp).
struct FrameInput;
const FrameInput* StagedInput();  // null outside a session
uint32_t CurrentFrame();          // the frame being run (the last frame hook's)
uint64_t SessionSeed();
void LogLine(const std::string& line);  // np_log.txt and the game log

}  // namespace doa4::np
