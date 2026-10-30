// DPPS cross-CPU check (netplay phase 0, spec section 4 "NEW RISK").
//
// The SDK's code generator turns the guest's vmsum3fp128 / vmsum4fp128 (23,791
// sites) into DPPS with masks 0xEF / 0xFF. Lockstep needs DPPS to give the same
// bits on every CPU that runs the game. This program runs both masks over 10^6
// seeded random vector pairs in five input classes, under the default MXCSR and
// under FTZ|DAZ (the guest's VMX flush mode), and prints one hash per class and
// mode plus a combined hash. Two PCs that print the same combined hash computed
// identical bits; if they differ, the per-class lines say where.
//
// The raw DPPS hashes differ between an AMD and an Intel core in the NaN-input
// class (the two CPUs return different NaN payloads). The SDK therefore makes every
// dot product return one canonical NaN (0xFFC00000), and this program also hashes
// the canonicalised results, computed through the SDK's own function (included
// from <rex/ppc/dot_product.h>, not copied): two PCs must print the same
// "DPPS canonical hash" even where the raw "DPPS combined hash" differs.
//
// Built by run_dpps_test.bat with the game's CPU level (-march=x86-64-v3, so the
// compiler emits VEX vdpps as the game does). The only SDK header is
// <rex/ppc/dot_product.h> (and simde's, which it includes).

#include <rex/ppc/dot_product.h>

#include <immintrin.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

struct Rng {  // xorshift64*
  uint64_t s;
  uint32_t Next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return uint32_t((s * 0x2545F4914F6CDD1Dull) >> 32);
  }
  float Uniform() { return float(Next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)
};

float FromBits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// One float of an input class.
float Draw(Rng& r, int cls) {
  const uint32_t sign = (r.Next() & 1u) << 31;
  switch (cls) {
    case 0: {  // normal: exponents 2^-20 .. 2^20
      const int e = int(r.Next() % 41) - 20;
      return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), e)));
    }
    case 1: {  // cancellation: large terms of opposite sign beside small ones
      static const float kMags[] = {1e8f, 1e7f, 4096.0f, 1.0f, 1e-3f, 1e-7f};
      const float m = kMags[r.Next() % 6] * (1.0f + r.Uniform() * 1e-3f);
      return FromBits(sign | Bits(m));
    }
    case 2: {  // denormals, zeros, and normals whose products underflow
      switch (r.Next() % 4) {
        case 0: return FromBits(sign | (r.Next() & 0x007FFFFFu));       // denormal
        case 1: return FromBits(sign);                                   // +-0
        case 2: return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), -120)));
        default: return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), -70)));
      }
    }
    case 3: {  // infinities and overflow beside finite values
      switch (r.Next() % 4) {
        case 0: return FromBits(sign | 0x7F800000u);                     // +-inf
        case 1: return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), 100)));
        case 2: return 0.0f;
        default: return FromBits(sign | Bits(1.0f + r.Uniform()));
      }
    }
    default: {  // NaN inputs with payloads, beside finite values
      if (r.Next() % 3 == 0) return FromBits(sign | 0x7F800000u | 1u | (r.Next() & 0x007FFFFFu));
      return FromBits(sign | Bits(1.0f + r.Uniform()));
    }
  }
}

uint64_t Fnv(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    h ^= (v >> (8 * i)) & 0xFF;
    h *= 0x100000001B3ull;
  }
  return h;
}

// Software models of DPPS (mask 0xFF/0xEF, result in lane 0), with the same
// MXCSR applied to the scalar operations. kTree is the Intel SDM order
// ((p0+p1)+(p2+p3)); kChain is the likeliest other order (((p0+p1)+p2)+p3).
// The run counts how often the hardware differs from each, so the output shows
// the fixture can tell summation orders apart (kChain must differ somewhere).
enum Order { kTree, kChain };
__declspec(noinline) float Model(const float* a, const float* b, bool lane0, Order order) {
  volatile float p0 = lane0 ? a[0] * b[0] : 0.0f;
  volatile float p1 = a[1] * b[1], p2 = a[2] * b[2], p3 = a[3] * b[3];
  if (order == kTree) {
    volatile float s01 = p0 + p1, s23 = p2 + p3;
    return s01 + s23;
  }
  volatile float s01 = p0 + p1;
  volatile float s012 = s01 + p2;
  return s012 + p3;
}

bool SameBits(float x, float y) {
  return Bits(x) == Bits(y) || (std::isnan(x) && std::isnan(y));
}

struct Counts {
  uint64_t nans = 0, tree_diff = 0, chain_diff = 0;
  // The canonicalised run (same pairs): its hash, the lanes that are not a canonical NaN where the
  // raw lane is a NaN, and the lanes where the game's redirected simde_mm_dp_ps differs from the
  // SDK function applied to the raw result.
  uint64_t canon_hash = 0xCBF29CE484222325ull;
  uint64_t canon_not_canonical = 0, redirect_diff = 0, canon_changed_lanes = 0;
};

constexpr uint32_t kCanonicalNaN = 0xFFC00000u;

// Kept out of line so the optimiser cannot fold or reorder the DPPS calls
// across the MXCSR change.
__declspec(noinline) uint64_t RunClass(int cls, uint32_t mxcsr, Counts* counts) {
  _mm_setcsr(mxcsr);
  Rng r{0x9E3779B97F4A7C15ull ^ uint64_t(cls + 1) * 0xD1B54A32D192ED03ull};
  uint64_t h = 0xCBF29CE484222325ull;
  alignas(16) float a[4], b[4], o1[4], o2[4];
  for (int i = 0; i < 200000; ++i) {
    for (int k = 0; k < 4; ++k) {
      a[k] = Draw(r, cls);
      b[k] = Draw(r, cls);
    }
    const __m128 va = _mm_load_ps(a), vb = _mm_load_ps(b);
    _mm_store_ps(o1, _mm_dp_ps(va, vb, 0xEF));
    _mm_store_ps(o2, _mm_dp_ps(va, vb, 0xFF));
    for (int k = 0; k < 4; ++k) {
      h = Fnv(h, Bits(o1[k]));
      h = Fnv(h, Bits(o2[k]));
      counts->nans += std::isnan(o1[k]) + std::isnan(o2[k]);
    }
    // The canonicalised results of the same pairs: the SDK function over the raw results, and the
    // game's own call shape (simde_mm_dp_ps, redirected by the SDK header) as a cross-check.
    alignas(16) float c1[4], c2[4], g1[4], g2[4];
    simde_mm_store_ps(c1, rex::ppc::CanonicalizeDotProductNaN(simde_mm_load_ps(o1)));
    simde_mm_store_ps(c2, rex::ppc::CanonicalizeDotProductNaN(simde_mm_load_ps(o2)));
    simde_mm_store_ps(g1, simde_mm_dp_ps(simde_mm_load_ps(a), simde_mm_load_ps(b), 0xEF));
    simde_mm_store_ps(g2, simde_mm_dp_ps(simde_mm_load_ps(a), simde_mm_load_ps(b), 0xFF));
    for (int k = 0; k < 4; ++k) {
      counts->canon_hash = Fnv(counts->canon_hash, Bits(c1[k]));
      counts->canon_hash = Fnv(counts->canon_hash, Bits(c2[k]));
      counts->canon_not_canonical += (std::isnan(o1[k]) && Bits(c1[k]) != kCanonicalNaN) +
                                     (std::isnan(o2[k]) && Bits(c2[k]) != kCanonicalNaN);
      counts->canon_changed_lanes += (Bits(c1[k]) != Bits(o1[k])) + (Bits(c2[k]) != Bits(o2[k]));
      counts->redirect_diff += (Bits(g1[k]) != Bits(c1[k])) + (Bits(g2[k]) != Bits(c2[k]));
    }
    counts->tree_diff += !SameBits(o1[0], Model(a, b, false, kTree)) +
                         !SameBits(o2[0], Model(a, b, true, kTree));
    counts->chain_diff += !SameBits(o1[0], Model(a, b, false, kChain)) +
                          !SameBits(o2[0], Model(a, b, true, kChain));
  }
  _mm_setcsr(0x1F80);
  return h;
}

}  // namespace

int main() {
  // Positive control: (1,2,3,4).(5,6,7,8) = 70 over all lanes; mask 0xEF drops lane 0
  // (1*5) = 65.
  const __m128 x = _mm_setr_ps(1, 2, 3, 4), y = _mm_setr_ps(5, 6, 7, 8);
  alignas(16) float c1[4], c2[4];
  _mm_store_ps(c1, _mm_dp_ps(x, y, 0xFF));
  _mm_store_ps(c2, _mm_dp_ps(x, y, 0xEF));
  const bool control = c1[0] == 70.0f && c1[3] == 70.0f && c2[0] == 65.0f && c2[2] == 65.0f;
  std::printf("control: 0xFF -> %g, 0xEF -> %g (%s)\n", c1[0], c2[0], control ? "ok" : "WRONG");

  static const char* kClass[] = {"normal", "cancel", "denormal", "inf", "nan"};
  static const struct {
    const char* name;
    uint32_t mxcsr;
  } kModes[] = {{"default", 0x1F80}, {"ftz_daz", 0x1F80 | 0x8000 | 0x0040}};
  uint64_t all = 0xCBF29CE484222325ull;
  uint64_t all_canon = 0xCBF29CE484222325ull;
  uint64_t chain_total = 0;
  uint64_t not_canonical_total = 0, redirect_diff_total = 0;
  char canon_lines[2][5][256];
  for (int mi = 0; mi < 2; ++mi) {
    const auto& m = kModes[mi];
    for (int cls = 0; cls < 5; ++cls) {
      Counts counts;
      const uint64_t h = RunClass(cls, m.mxcsr, &counts);
      std::printf("%-8s %-8s %016llx  (%llu NaN results; lane 0 vs SDM-order model: %llu differ, "
                  "vs chained-order model: %llu differ)\n",
                  m.name, kClass[cls], (unsigned long long)h, (unsigned long long)counts.nans,
                  (unsigned long long)counts.tree_diff, (unsigned long long)counts.chain_diff);
      all = Fnv(Fnv(all, uint32_t(h)), uint32_t(h >> 32));
      chain_total += counts.chain_diff;
      // The canonical lines are printed after the raw ones, so the raw output is unchanged.
      std::snprintf(canon_lines[mi][cls], sizeof(canon_lines[mi][cls]),
                    "canonical %-8s %-8s %016llx  (%llu lanes changed by canonicalising; NaN lanes not "
                    "canonical after: %llu; game's simde_mm_dp_ps vs SDK function: %llu differ)",
                    m.name, kClass[cls], (unsigned long long)counts.canon_hash,
                    (unsigned long long)counts.canon_changed_lanes,
                    (unsigned long long)counts.canon_not_canonical, (unsigned long long)counts.redirect_diff);
      all_canon = Fnv(Fnv(all_canon, uint32_t(counts.canon_hash)), uint32_t(counts.canon_hash >> 32));
      not_canonical_total += counts.canon_not_canonical;
      redirect_diff_total += counts.redirect_diff;
    }
  }
  // The fixture must be able to tell a different summation order apart.
  std::printf("fixture discriminates summation order: %s\n", chain_total ? "yes" : "NO");
  std::printf("DPPS combined hash: %016llx (2 modes x 5 classes x 200000 vector pairs x 2 masks)\n",
              (unsigned long long)all);
  for (int mi = 0; mi < 2; ++mi)
    for (int cls = 0; cls < 5; ++cls) std::printf("%s\n", canon_lines[mi][cls]);
  const bool canon_ok = not_canonical_total == 0 && redirect_diff_total == 0;
  std::printf("canonical NaN results all 0xFFC00000 and the redirect agrees with the SDK function: %s\n",
              canon_ok ? "yes" : "NO");
  std::printf("DPPS canonical hash: %016llx (2 modes x 5 classes x 200000 vector pairs x 2 masks)\n",
              (unsigned long long)all_canon);
  return control && canon_ok ? 0 : 1;
}
