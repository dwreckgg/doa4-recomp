// DOA4 netplay phase 0: the determinism layer, active only in a session.
// Each fix is switched by DOA4_NP_FIX (a comma list, "all" or "none"; the
// default in a session is "all"), so the divergence a fix removes can be shown
// on one build by running with and without it.

#pragma once

#include <cstdint>
#include <filesystem>

namespace doa4::np::det {

// Called by Start when a session begins (before guest code runs).
void Start(uint64_t seed, const std::filesystem::path& dir);
// Called by the frame hook with the frame about to run.
void OnFrame(uint32_t frame, uint8_t* base);

}  // namespace doa4::np::det
