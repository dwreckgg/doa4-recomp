// Host test of the dot-product NaN redirect (netplay phase 0, Task 11b).
//
// The friend's Intel PC and this AMD PC give different NaN payloads out of DPPS when the inputs
// carry NaNs (dpps_host_test: class "nan" differs). The SDK therefore redirects every
// simde_mm_dp_ps (the code generator's vmsum3fp128 / vmsum4fp128, masks 0xEF / 0xFF) to a result
// whose NaN lanes are all 0xFFC00000. This test checks that redirect the way the generated code
// meets it.
//
// The include chain of the generated code (generated/default/dead_or_alive_4_recomp.N.cpp):
//   #include "dead_or_alive_4_init.h"          (generated/default/dead_or_alive_4_init.h:24)
//     #include <rex/ppc.h>                     (sdk include/rex/ppc.h)
//       #include <rex/ppc/intrinsics.h>        (simde's sse4.1.h at its line 23, then the next include)
//         #include <rex/ppc/dot_product.h>     (defines the simde_mm_dp_ps redirect)
// This file includes <rex/ppc.h>, the same umbrella header, and calls simde_mm_dp_ps in the exact
// shape of sdk src/codegen/builders/vector.cpp (build_vmsum3fp128 / build_vmsum4fp128).
//
// Built by run_dot_product_test.bat with the game's CPU level (-march=x86-64-v3 -msse4.1: simde's
// native SSE4.1 path, where simde_mm_dp_ps is a macro for _mm_dp_ps).

#include <rex/ppc.h>

#include <immintrin.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/net/np_selftest.h"

#if !defined(SIMDE_X86_SSE4_1_NATIVE)
#error "build with -march=x86-64-v3 -msse4.1 like the game: this test checks simde's native path"
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
int g_payload_differs = 0;  // set by TestFixtures: NaN results the hardware gives a non-canonical payload

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);               \
    }                                                                   \
  } while (0)

// DOA4_CI (set, non-empty, by the release workflow): a CI runner's CPU may return the canonical NaN out of the raw
// DPPS, which says nothing about the redirect. So the checks on the raw hardware payloads (TestFixtures'
// payload_differs > 0, and TestRandom's noncanon > 0 in class "nan", once per MXCSR mode: 3 checks) are printed
// there, not checked (phase 0 progress.md:177). The redirect checks stay CHECKs everywhere, and so does
// nan_cases >= 40, which counts the pinned table's NaN cases and cannot depend on the CPU.
const bool g_ci = [] {
  const char* v = std::getenv("DOA4_CI");
  return v != nullptr && *v != '\0';
}();
#define CPU_CHECK(cond)                                                                       \
  do {                                                                                        \
    if (g_ci)                                                                                 \
      std::printf("INFO line %d (DOA4_CI: printed, not checked): %s is %s\n", __LINE__, #cond, \
                  (cond) ? "true" : "false");                                                 \
    else                                                                                      \
      CHECK(cond);                                                                            \
  } while (0)

constexpr uint32_t kCanonicalNaN = 0xFFC00000u;  // pinned: x86's "real indefinite" QNaN, per the ruling

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float FromBits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
bool IsNaNBits(uint32_t u) { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu) != 0; }

struct alignas(16) V4 {
  float f[4];
  uint32_t b(int i) const { return Bits(f[i]); }
};
V4 Make(float a, float b, float c, float d) {
  V4 v;
  v.f[0] = a;
  v.f[1] = b;
  v.f[2] = c;
  v.f[3] = d;
  return v;
}

// The shapes the code generator emits (vector.cpp:186-205), one noinline function each so the
// optimiser cannot fold the NaN arithmetic away.
__declspec(noinline) V4 GenVmsum3(const V4& a, const V4& b) {
  V4 r;
  simde_mm_store_ps(r.f, simde_mm_dp_ps(simde_mm_load_ps(a.f), simde_mm_load_ps(b.f), 0xEF));
  return r;
}
__declspec(noinline) V4 GenVmsum4(const V4& a, const V4& b) {
  V4 r;
  simde_mm_store_ps(r.f, simde_mm_dp_ps(simde_mm_load_ps(a.f), simde_mm_load_ps(b.f), 0xFF));
  return r;
}
// The hardware instruction itself, with no SDK in between (clang's own intrinsic).
__declspec(noinline) V4 RawVmsum3(const V4& a, const V4& b) {
  V4 r;
  _mm_store_ps(r.f, _mm_dp_ps(_mm_load_ps(a.f), _mm_load_ps(b.f), 0xEF));
  return r;
}
__declspec(noinline) V4 RawVmsum4(const V4& a, const V4& b) {
  V4 r;
  _mm_store_ps(r.f, _mm_dp_ps(_mm_load_ps(a.f), _mm_load_ps(b.f), 0xFF));
  return r;
}

__declspec(noinline) V4 Canon(const V4& v) {
  V4 r;
  simde_mm_store_ps(r.f, rex::ppc::CanonicalizeDotProductNaN(simde_mm_load_ps(v.f)));
  return r;
}

struct Mxcsr {  // sets MXCSR for a scope, restores the default after
  explicit Mxcsr(uint32_t v) { _mm_setcsr(v); }
  ~Mxcsr() { _mm_setcsr(0x1F80); }
};
constexpr uint32_t kMxcsrDefault = 0x1F80;
constexpr uint32_t kMxcsrFtzDaz = 0x1F80 | 0x8000 | 0x0040;  // the guest's VMX flush mode

// ---- fixtures: one dot product each, with the NaN-ness of its result PINNED by hand ----------
struct Case {
  const char* name;
  V4 a, b;
  bool nan3;  // the 0xEF result is a NaN (lane 0 of the inputs is left out)
  bool nan4;  // the 0xFF result is a NaN
};

const float kInf = FromBits(0x7F800000u);
float N(uint32_t bits) { return FromBits(bits); }

const Case* Cases(int* n) {
  static const Case kCases[] = {
      // NaN inputs with payloads of their own
      {"qNaN payload in x only (excluded lane of 0xEF)", Make(N(0x7FC01234u), 2, 3, 4), Make(5, 6, 7, 8), false, true},
      {"payloads in y and z, opposite signs", Make(1, N(0x7FC05678u), N(0xFFC09ABCu), 4), Make(5, 6, 7, 8), true, true},
      {"signalling NaNs", Make(N(0x7F800001u), N(0xFF800001u), 3, 4), Make(5, 6, 7, 8), true, true},
      {"NaN in b only, other payload", Make(1, 2, 3, 4), Make(5, N(0x7FA00001u), 7, 8), true, true},
      {"NaN in every lane of both inputs, distinct payloads",
       Make(N(0x7FC00001u), N(0x7FC00002u), N(0x7FC00003u), N(0x7FC00004u)),
       Make(N(0xFFC00005u), N(0xFFC00006u), N(0xFFC00007u), N(0xFFC00008u)), true, true},
      {"all-ones NaN in w", Make(1, 2, 3, N(0xFFFFFFFFu)), Make(5, 6, 7, 8), true, true},
      {"NaN in the last lane, payload 0x40FFFF", Make(1, 2, 3, N(0x7FC0FFFFu)), Make(5, 6, 7, 8), true, true},
      {"NaN times zero", Make(1, N(0x7FC00777u), 1, 1), Make(1, 0, 1, 1), true, true},
      // invalid operations that make the hardware's own default NaN
      {"inf times zero in y", Make(1, kInf, 1, 1), Make(1, 0, 1, 1), true, true},
      {"inf times zero in x (excluded lane of 0xEF)", Make(kInf, 1, 1, 1), Make(0, 1, 1, 1), false, true},
      {"inf minus inf", Make(1, kInf, kInf, 0), Make(1, 1, -1, 0), true, true},
      {"inf minus inf across x and y (0xEF sees only -inf)", Make(kInf, kInf, 0, 0), Make(1, -1, 0, 0), false, true},
      // a NaN only in the excluded x lane, and the same lane with a finite product
      {"x lane NaN with payload, rest finite", Make(N(0x7FC0DEADu), 2, 3, 4), Make(5, 6, 7, 8), false, true},
      {"b x lane NaN, rest finite", Make(1, 2, 3, 4), Make(N(0xFFC0BEEFu), 6, 7, 8), false, true},
      // no NaN anywhere
      {"finite 70 / 65", Make(1, 2, 3, 4), Make(5, 6, 7, 8), false, false},
      {"negative zeros", Make(-0.0f, -0.0f, -0.0f, -0.0f), Make(1, 1, 1, 1), false, false},
      {"positive and negative zeros", Make(0.0f, 0.0f, 0.0f, 0.0f), Make(-1, -1, -1, -1), false, false},
      {"denormal inputs", Make(1e-40f, 1e-40f, 1e-40f, 1e-40f), Make(1, 1, 1, 1), false, false},
      {"denormal products", Make(1e-30f, 1e-30f, 1e-30f, 1e-30f), Make(1e-10f, 1e-10f, 1e-10f, 1e-10f), false, false},
      {"+inf result", Make(1, kInf, 1, 1), Make(1, 1, 1, 1), false, false},
      {"-inf result", Make(1, kInf, 1, 1), Make(1, -1, 1, 1), false, false},
      {"overflow of finite inputs", Make(3e38f, 3e38f, 3e38f, 3e38f), Make(10, 10, 10, 10), false, false},
      {"cancellation", Make(1e8f, -1e8f, 1, 0), Make(1, 1, 1, 0), false, false},
      {"huge and tiny", Make(1e30f, 1e-30f, 1e30f, 1e-30f), Make(1e-30f, 1e30f, 1e-30f, 1e30f), false, false},
  };
  *n = int(sizeof(kCases) / sizeof(kCases[0]));
  return kCases;
}

void CheckLanes(const char* what, const char* name, uint32_t mask, uint32_t mxcsr, const V4& got, const V4& raw,
                bool want_nan) {
  bool ok = true;
  for (int i = 0; i < 4; ++i) {
    const uint32_t want = want_nan ? kCanonicalNaN : raw.b(i);
    if (got.b(i) != want) {
      ok = false;
      std::printf("FAIL %s [%s] mask 0x%02X mxcsr 0x%04X lane %d: got %08X want %08X (raw %08X)\n", what, name, mask,
                  mxcsr, i, got.b(i), want, raw.b(i));
    }
  }
  CHECK(ok);
}

void TestFixtures() {
  int n = 0;
  const Case* cases = Cases(&n);
  CHECK(n >= 20);
  int nan_cases = 0, payload_differs = 0;
  for (const uint32_t mxcsr : {kMxcsrDefault, kMxcsrFtzDaz}) {
    Mxcsr guard(mxcsr);
    for (int c = 0; c < n; ++c) {
      const Case& k = cases[c];
      for (int m = 0; m < 2; ++m) {
        const bool want_nan = m == 0 ? k.nan3 : k.nan4;
        const V4 raw = m == 0 ? RawVmsum3(k.a, k.b) : RawVmsum4(k.a, k.b);
        const V4 got = m == 0 ? GenVmsum3(k.a, k.b) : GenVmsum4(k.a, k.b);
        // The hand-pinned NaN-ness must agree with the hardware (guards the table itself).
        CHECK(IsNaNBits(raw.b(0)) == want_nan);
        CheckLanes("generated shape", k.name, m == 0 ? 0xEF : 0xFF, mxcsr, got, raw, want_nan);
        if (want_nan) {
          ++nan_cases;
          // Fixture precondition: some NaN results come out of the hardware with another payload,
          // which is what a missing redirect would leave in place.
          if (raw.b(0) != kCanonicalNaN) ++payload_differs;
        }
      }
    }
  }
  g_payload_differs = payload_differs;
  CHECK(nan_cases >= 40);
  CPU_CHECK(payload_differs > 0);  // else no fixture separates "redirect" from "no redirect" on this CPU
  std::printf("fixtures: %d cases x 2 masks x 2 MXCSR modes; %d NaN results, %d of them with a non-canonical hardware payload\n",
              n, nan_cases, payload_differs);

  // Pinned values, from the arithmetic and not from the code under test.
  {
    Mxcsr guard(kMxcsrDefault);
    const V4 a = Make(1, 2, 3, 4), b = Make(5, 6, 7, 8);
    const V4 r3 = GenVmsum3(a, b), r4 = GenVmsum4(a, b);
    for (int i = 0; i < 4; ++i) {
      CHECK(r3.b(i) == Bits(65.0f));
      CHECK(r4.b(i) == Bits(70.0f));
    }
    // The same inputs, a NaN planted in x: 0xEF still reads 65 bit for bit, 0xFF is the canonical NaN.
    const V4 an = Make(N(0x7FC01234u), 2, 3, 4);
    const V4 s3 = GenVmsum3(an, b), s4 = GenVmsum4(an, b);
    for (int i = 0; i < 4; ++i) {
      CHECK(s3.b(i) == Bits(65.0f));
      CHECK(s4.b(i) == kCanonicalNaN);
    }
  }
}

void TestFunctionMixedLanes() {
  Mxcsr guard(kMxcsrDefault);
  // The function takes any vector: NaN lanes become the canonical NaN, every other lane is untouched.
  const V4 in1 = Make(1.0f, N(0x7FC01234u), -0.0f, N(0xFFC0BEEFu));
  const V4 o1 = Canon(in1);
  CHECK(o1.b(0) == Bits(1.0f));
  CHECK(o1.b(1) == kCanonicalNaN);
  CHECK(o1.b(2) == Bits(-0.0f));
  CHECK(o1.b(3) == kCanonicalNaN);
  const V4 in2 = Make(kInf, N(0x7F800001u), FromBits(0x00000001u), -kInf);
  const V4 o2 = Canon(in2);
  CHECK(o2.b(0) == 0x7F800000u);
  CHECK(o2.b(1) == kCanonicalNaN);
  CHECK(o2.b(2) == 0x00000001u);  // the smallest denormal, kept
  CHECK(o2.b(3) == 0xFF800000u);
  const V4 in3 = Make(N(0xFFFFFFFFu), N(0x7FFFFFFFu), N(0xFF800001u), N(0x7FC00000u));
  const V4 o3 = Canon(in3);
  for (int i = 0; i < 4; ++i) CHECK(o3.b(i) == kCanonicalNaN);
  // The canonical NaN is a fixed point.
  const V4 o4 = Canon(Canon(in3));
  for (int i = 0; i < 4; ++i) CHECK(o4.b(i) == kCanonicalNaN);
}

// Every one of the 2^32 bit patterns goes through the function once (four per vector, in any lane
// position); the oracle is the bit test of IEEE 754 binary32, not the SDK's compare.
__declspec(noinline) void Sweep(uint32_t first, uint32_t count_vectors, uint64_t* mismatches, uint64_t* nans) {
  uint64_t bad = 0, nan = 0;
  for (uint32_t i = 0; i < count_vectors; ++i) {
    const uint32_t base = first + i * 4u;
    uint32_t in[4] = {base, base + 1u, base + 2u, base + 3u};
    alignas(16) uint32_t out[4];
    const __m128 v = _mm_castsi128_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in)));
    _mm_store_si128(reinterpret_cast<__m128i*>(out),
                    _mm_castps_si128(rex::ppc::CanonicalizeDotProductNaN(v)));
    for (int k = 0; k < 4; ++k) {
      const bool isnan = IsNaNBits(in[k]);
      nan += isnan;
      bad += out[k] != (isnan ? kCanonicalNaN : in[k]);
    }
  }
  *mismatches += bad;
  *nans += nan;
}

void TestFunctionSweep() {
  {
    Mxcsr guard(kMxcsrDefault);
    uint64_t bad = 0, nans = 0;
    Sweep(0u, 0x40000000u, &bad, &nans);  // all 2^32 patterns
    std::printf("sweep, default MXCSR: %llu mismatches over 2^32 patterns, %llu NaN patterns\n", (unsigned long long)bad,
                (unsigned long long)nans);
    CHECK(nans == 2ull * ((1ull << 23) - 1));  // non-vacuity: every NaN pattern was seen
    CHECK(bad == 0);
  }
  {
    // FTZ|DAZ: denormals must come through as the same bits (a compare must not treat them as NaN or zero them).
    Mxcsr guard(kMxcsrFtzDaz);
    uint64_t bad = 0, nans = 0;
    Sweep(0x00000000u, 0x00200000u, &bad, &nans);  // 0x00000000..0x007FFFFF: +denormals and +0
    Sweep(0x80000000u, 0x00200000u, &bad, &nans);  // the negative ones
    Sweep(0x7F800000u, 0x00200000u, &bad, &nans);  // +inf and the positive NaNs
    Sweep(0xFF800000u, 0x00200000u, &bad, &nans);  // -inf and the negative NaNs
    CHECK(bad == 0);
    CHECK(nans == 2ull * ((1ull << 23) - 1) - 0ull);
  }
}

// ---- random pairs through the generated shape, against a scalar oracle ----------------------
struct Rng {  // xorshift64*
  uint64_t s;
  uint32_t Next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return uint32_t((s * 0x2545F4914F6CDD1Dull) >> 32);
  }
  float Uniform() { return float(Next() >> 8) * (1.0f / 16777216.0f); }
};

float Draw(Rng& r, int cls) {
  const uint32_t sign = (r.Next() & 1u) << 31;
  switch (cls) {
    case 0: return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), int(r.Next() % 41) - 20)));
    case 1: {
      static const float kMags[] = {1e8f, 1e7f, 4096.0f, 1.0f, 1e-3f, 1e-7f};
      return FromBits(sign | Bits(kMags[r.Next() % 6] * (1.0f + r.Uniform() * 1e-3f)));
    }
    case 2:
      switch (r.Next() % 4) {
        case 0: return FromBits(sign | (r.Next() & 0x007FFFFFu));
        case 1: return FromBits(sign);
        case 2: return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), -120)));
        default: return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), -70)));
      }
    case 3:
      switch (r.Next() % 4) {
        case 0: return FromBits(sign | 0x7F800000u);
        case 1: return FromBits(sign | Bits(std::ldexp(1.0f + r.Uniform(), 100)));
        case 2: return 0.0f;
        default: return FromBits(sign | Bits(1.0f + r.Uniform()));
      }
    default:
      if (r.Next() % 3 == 0) return FromBits(sign | 0x7F800000u | 1u | (r.Next() & 0x007FFFFFu));
      return FromBits(sign | Bits(1.0f + r.Uniform()));
  }
}

__declspec(noinline) void RandomClass(int cls, uint32_t mxcsr, uint64_t* bad, uint64_t* nan_results,
                                      uint64_t* noncanon_raw_nans) {
  Mxcsr guard(mxcsr);
  Rng r{0x9E3779B97F4A7C15ull ^ uint64_t(cls + 1) * 0xD1B54A32D192ED03ull};
  for (int i = 0; i < 20000; ++i) {
    V4 a, b;
    for (int k = 0; k < 4; ++k) {
      a.f[k] = Draw(r, cls);
      b.f[k] = Draw(r, cls);
    }
    for (int m = 0; m < 2; ++m) {
      const V4 raw = m == 0 ? RawVmsum3(a, b) : RawVmsum4(a, b);
      const V4 got = m == 0 ? GenVmsum3(a, b) : GenVmsum4(a, b);
      for (int k = 0; k < 4; ++k) {
        const bool isnan = std::isnan(raw.f[k]);  // scalar oracle on the hardware's own lane
        *nan_results += isnan;
        *noncanon_raw_nans += isnan && raw.b(k) != kCanonicalNaN;
        *bad += got.b(k) != (isnan ? kCanonicalNaN : raw.b(k));
      }
    }
  }
}

void TestRandom() {
  static const char* kClass[] = {"normal", "cancel", "denormal", "inf", "nan"};
  for (const uint32_t mxcsr : {kMxcsrDefault, kMxcsrFtzDaz}) {
    for (int cls = 0; cls < 5; ++cls) {
      uint64_t bad = 0, nans = 0, noncanon = 0;
      RandomClass(cls, mxcsr, &bad, &nans, &noncanon);
      std::printf("random %-8s mxcsr 0x%04X: %llu NaN lanes (%llu non-canonical in hardware), %llu mismatches\n",
                  kClass[cls], mxcsr, (unsigned long long)nans, (unsigned long long)noncanon, (unsigned long long)bad);
      CHECK(bad == 0);
      if (cls <= 2) CHECK(nans == 0);       // non-vacuity: these classes make no NaN
      if (cls == 4) CPU_CHECK(noncanon > 0);  // the class the friend's PC differed on does carry payloads here
      if (cls >= 3) CHECK(nans > 0);
    }
  }
}

// ---- the boot self-test (src/net/np_selftest.h; netplay phase 1, Task 1 Step 6b) ---------------------
// np_session.cpp calls selftest::CanonicalNaN with a lambda around simde_mm_dp_ps(a, b, 0xFF) and the app
// logs "[doa4][LIFE] dot products: canonical NaN <result>" at start-up. The redirect is compile-time (the
// build id hashes bytes, not features), so this line is how a build against an SDK without it is seen.
__m128 Splat(uint32_t bits) { return _mm_castsi128_ps(_mm_set1_epi32(int(bits))); }
__m128 Lanes(uint32_t b0, uint32_t b1, uint32_t b2, uint32_t b3) {
  return _mm_castsi128_ps(_mm_set_epi32(int(b3), int(b2), int(b1), int(b0)));
}
uint32_t LaneBits(__m128 v, int lane) {
  alignas(16) uint32_t w[4];
  _mm_store_si128(reinterpret_cast<__m128i*>(w), _mm_castps_si128(v));
  return w[lane];
}
bool HasNaNLane(__m128 a, __m128 b) {
  for (int i = 0; i < 4; ++i)
    if (IsNaNBits(LaneBits(a, i)) || IsNaNBits(LaneBits(b, i))) return true;
  return false;
}

struct SelfTestCall {
  uint32_t a[4], b[4];
};
// A stand-in for the dot product (a hypothesis about what the redirect or the hardware returns, T3.1): `nan` is
// what it answers when an input holds a NaN, `fin` what it answers for the finite pair; it records its inputs.
struct FakeDp {
  __m128 nan, fin;
  std::vector<SelfTestCall>* calls;
  __m128 operator()(__m128 a, __m128 b) const {
    SelfTestCall c;
    for (int i = 0; i < 4; ++i) {
      c.a[i] = LaneBits(a, i);
      c.b[i] = LaneBits(b, i);
    }
    if (calls) calls->push_back(c);
    return HasNaNLane(a, b) ? nan : fin;
  }
};

void TestSelfTest() {
  namespace st = doa4::np::selftest;
  const uint32_t kC = kCanonicalNaN, k70 = Bits(70.0f);
  const __m128 canon = Splat(kC), seventy = Splat(k70);

  // The real redirect, called as np_session.cpp calls it (the generated code's shape, imm8 0xFF).
  const std::string real = st::CanonicalNaN([](__m128 a, __m128 b) { return simde_mm_dp_ps(a, b, 0xFF); });
  std::printf("self-test through simde_mm_dp_ps: %s\n", real.c_str());
  CHECK(real == "on");

  // The raw hardware instruction is the negative control: on a CPU whose DPPS keeps NaN payloads (the fixtures
  // above found some, g_payload_differs) the self-test must say OFF and show the lanes it saw.
  const std::string raw = st::CanonicalNaN([](__m128 a, __m128 b) { return _mm_dp_ps(a, b, 0xFF); });
  std::printf("self-test through the raw _mm_dp_ps: %s\n", raw.c_str());
  if (g_payload_differs > 0) {
    CHECK(raw.rfind("OFF (", 0) == 0);  // the self-test's own inputs separate redirect from no redirect here
    CHECK(raw.size() == 5 + 4 * 8 + 3 + 1);
  } else {
    std::printf("INFO: no fixture NaN comes out of this CPU with a non-canonical payload; the raw control is informational\n");
  }

  // What the self-test feeds the dot product: one call with NaNs of their own payloads in different lanes, one finite pair.
  {
    std::vector<SelfTestCall> calls;
    const std::string s = st::CanonicalNaN(FakeDp{canon, seventy, &calls});
    CHECK(s == "on");
    CHECK(calls.size() == 2);
    int nan_calls = 0, fin_calls = 0;
    for (const SelfTestCall& c : calls) {
      bool any_nan = false;
      for (int i = 0; i < 4; ++i) any_nan = any_nan || IsNaNBits(c.a[i]) || IsNaNBits(c.b[i]);
      if (any_nan) {
        ++nan_calls;
        CHECK(c.a[1] == 0x7FC12345u);  // not the default payloads 0x7FC00000 / 0xFFC00000
        CHECK(c.b[2] == 0xFFC0BEEFu);
        CHECK(IsNaNBits(c.a[1]) && IsNaNBits(c.b[2]));
        int nan_lanes = 0;
        for (int i = 0; i < 4; ++i) nan_lanes += IsNaNBits(c.a[i]) + IsNaNBits(c.b[i]);
        CHECK(nan_lanes == 2);  // exactly those two, so the payload that comes back is theirs or the canonical one
      } else {
        ++fin_calls;
        const uint32_t one[4] = {Bits(1.0f), Bits(2.0f), Bits(3.0f), Bits(4.0f)};
        const uint32_t five[4] = {Bits(5.0f), Bits(6.0f), Bits(7.0f), Bits(8.0f)};
        for (int i = 0; i < 4; ++i) {
          CHECK(c.a[i] == one[i]);
          CHECK(c.b[i] == five[i]);
        }
      }
    }
    CHECK(nan_calls == 1 && fin_calls == 1);
  }

  // Verdicts on stand-ins (the exact text, since the gate and the report read it).
  CHECK(st::CanonicalNaN(FakeDp{canon, seventy, nullptr}) == "on");
  // 1. the hardware's payload kept (no redirect): every lane shows it
  CHECK(st::CanonicalNaN(FakeDp{Splat(0x7FC12345u), seventy, nullptr}) == "OFF (7FC12345 7FC12345 7FC12345 7FC12345)");
  // 2. one NaN lane left alone (only lanes 0-2 canonicalised): it is not "on", and the lane shows
  CHECK(st::CanonicalNaN(FakeDp{Lanes(kC, kC, kC, 0xFFC0BEEFu), seventy, nullptr}) == "OFF (FFC00000 FFC00000 FFC00000 FFC0BEEF)");
  CHECK(st::CanonicalNaN(FakeDp{Lanes(0x7FC12345u, kC, kC, kC), seventy, nullptr}) == "OFF (7FC12345 FFC00000 FFC00000 FFC00000)");
  // 3. another canonical value (0x7FC00000 is a quiet NaN too, but not the pinned one)
  CHECK(st::CanonicalNaN(FakeDp{Splat(0x7FC00000u), seventy, nullptr}) == "OFF (7FC00000 7FC00000 7FC00000 7FC00000)");
  // 4. the finite pair answered wrongly (a redirect that disturbs ordinary results): the finite lanes show
  CHECK(st::CanonicalNaN(FakeDp{canon, Splat(Bits(69.0f)), nullptr}) == "OFF (428A0000 428A0000 428A0000 428A0000)");
  CHECK(st::CanonicalNaN(FakeDp{canon, canon, nullptr}) == "OFF (FFC00000 FFC00000 FFC00000 FFC00000)");
  CHECK(st::CanonicalNaN(FakeDp{canon, Lanes(k70, k70, k70, 0u), nullptr}) == "OFF (428C0000 428C0000 428C0000 00000000)");
  // 5. both wrong: the NaN lanes are the ones shown
  CHECK(st::CanonicalNaN(FakeDp{Splat(0x7FC12345u), Splat(Bits(69.0f)), nullptr}) == "OFF (7FC12345 7FC12345 7FC12345 7FC12345)");
}

}  // namespace

int main() {
  TestFixtures();
  TestSelfTest();
  TestFunctionMixedLanes();
  TestRandom();
  TestFunctionSweep();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
