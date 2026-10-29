// DOA4 netplay phase 0: the determinism layer, active only in a session.
// Each fix is switched by DOA4_NP_FIX (a comma list, "all" or "none"; the
// default in a session is "all"), so the divergence a fix removes can be shown
// on one build by running with and without it. The fixes and the evidence for
// each are listed in np_det.cpp.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/ppc/context.h>

namespace doa4::np::det {

// Called by Start when a session begins (before guest code runs).
void Start(uint64_t seed, const std::filesystem::path& dir);
// Called by the frame hook (main thread, loop head) with the frame about to run.
void OnFrame(uint32_t frame, PPCContext& ctx, uint8_t* base);
// Called when the session ends (the game keeps running until it exits): the layer stops holding
// threads to the frame boundary and switches off everything Start switched on (lockstep audio and
// XMA, inline completions, headless dialogs, the clock override). Idempotent.
void Stop();

// The layer's counters (np_det_internal.h): names in file order, and their values now.
const std::vector<std::string>& CounterNames();
void CounterValues(std::vector<uint32_t>* out);

}  // namespace doa4::np::det
