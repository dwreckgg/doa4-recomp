// DOA4 netplay phase 1: the boot self-test of the dot-product NaN redirect (Task 1, Step 6b).
//
// Phase 0's Task 11b redirects every simde_mm_dp_ps (the generated vmsum3fp128 / vmsum4fp128) to a result
// whose NaN lanes are all 0xFFC00000 (sdk include/rex/ppc/dot_product.h), so an AMD and an Intel PC give the same
// bits. The redirect is compile-time: the build id hashes bytes, not features, so an exe built against an SDK
// without it fails nothing by itself. This header is how the build says which one it is: np_session.cpp calls
// CanonicalNaN with a lambda around simde_mm_dp_ps and the app logs the answer at start-up.
//
// Header-only and SDK-free (the compiler's own <immintrin.h> only): the host test builds it with the SDK's
// simde_mm_dp_ps and with the raw _mm_dp_ps beside it.
#pragma once

#include <immintrin.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace doa4::np::selftest {

// `dp(a, b)` takes two __m128 and returns __m128; it is expected to compute simde_mm_dp_ps(a, b, 0xFF) (the
// immediate is a compile-time constant, so the caller's lambda binds it). It is called twice:
//  - a with a NaN of payload 0x7FC12345 in lane 1 and b with a NaN of payload 0xFFC0BEEF in lane 2, the other
//    lanes finite (neither payload is a default NaN, so a result that is not 0xFFC00000 shows what DPPS did);
//  - the finite pair (1, 2, 3, 4) . (5, 6, 7, 8), which must come out as 1*5 + 2*6 + 3*7 + 4*8 = 70.0f.
// Returns "on" when all four lanes of the NaN call are 0xFFC00000 and all four lanes of the finite call are
// the bits of 70.0f. Otherwise "OFF (<4 lane bit patterns>)": the four lanes of the NaN call when they are not
// all canonical, else the four lanes of the finite call; each lane is 8 upper-case hex digits, separated by
// single spaces.
template <class DotProduct>
std::string CanonicalNaN(DotProduct dp) {
  constexpr uint32_t kCanonical = 0xFFC00000u;
  constexpr uint32_t kSeventy = 0x428C0000u;  // 70.0f
  // The inputs come through volatile reads so the compiler cannot fold the call away at compile time:
  // the point is what this build's dot product does at run time.
  static const volatile uint32_t kNanA[4] = {0x3F800000u, 0x7FC12345u, 0x40400000u, 0x40800000u};  // 1, NaN, 3, 4
  static const volatile uint32_t kNanB[4] = {0x40A00000u, 0x40C00000u, 0xFFC0BEEFu, 0x41000000u};  // 5, 6, NaN, 8
  static const volatile uint32_t kFinA[4] = {0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u};  // 1, 2, 3, 4
  static const volatile uint32_t kFinB[4] = {0x40A00000u, 0x40C00000u, 0x40E00000u, 0x41000000u};  // 5, 6, 7, 8
  auto load = [](const volatile uint32_t (&w)[4]) {
    return _mm_castsi128_ps(_mm_set_epi32(int(w[3]), int(w[2]), int(w[1]), int(w[0])));
  };
  auto lanes = [](__m128 v, uint32_t (&out)[4]) {
    alignas(16) uint32_t w[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(w), _mm_castps_si128(v));
    for (int i = 0; i < 4; ++i) out[i] = w[i];
  };
  uint32_t nan_lanes[4], fin_lanes[4];
  lanes(dp(load(kNanA), load(kNanB)), nan_lanes);
  lanes(dp(load(kFinA), load(kFinB)), fin_lanes);
  bool nan_ok = true, fin_ok = true;
  for (int i = 0; i < 4; ++i) {
    nan_ok = nan_ok && nan_lanes[i] == kCanonical;
    fin_ok = fin_ok && fin_lanes[i] == kSeventy;
  }
  if (nan_ok && fin_ok) return "on";
  const uint32_t* shown = nan_ok ? fin_lanes : nan_lanes;
  char text[64];
  std::snprintf(text, sizeof(text), "OFF (%08X %08X %08X %08X)", shown[0], shown[1], shown[2], shown[3]);
  return text;
}

}  // namespace doa4::np::selftest
