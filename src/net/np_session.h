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
// Request files in DOA4_NP_DIR, taken (deleted) when read, polled every 15 frames:
//   np_pad.on   a pad script appended to the input queue (record only)
//   np_shot.on  a screenshot now
//   np_ctl.on   "quit" (the run ends after this frame, as a quit frame) | "dump"

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "../doa4_photo.h"

namespace doa4::np {

struct Host {
  // The presenter's current guest output (any thread). False if none yet.
  std::function<bool(doa4::photo::Image*)> capture;
  // Ends the process the normal way (posted to the UI thread).
  std::function<void()> quit;
  // The folder the game runs from (the one holding default.xex): the recording's meta holds a hash of
  // its default.xex, so a replay on another dump of the game is refused.
  std::filesystem::path game_data_root;
};

// Reads the configuration; called once before any guest code runs.
void Start(Host host);
// Ends the session (if one runs), writes the shots and dumps still queued, joins the writer and
// closes the session log. Called first in OnShutdown, while the presenter still exists.
void Shutdown();
bool Active();

// For the other netplay files (np_det.cpp).
struct FrameInput;
const FrameInput* StagedInput();  // null outside a session
uint32_t CurrentFrame();          // the frame being run (the last frame hook's)
uint64_t SessionSeed();
void LogLine(const std::string& line);  // np_log.txt and the game log

}  // namespace doa4::np
