// Host test for src/net/np_rules.cpp (the pure rules of the DOA4 netplay lobby: join codes, STUN and NAT class,
// input delay, pacer, release channel, handshake verdict). Built and run by run_np_rules_test.bat;
// "np_rules_host_test.exe -v" prints each group before it runs, so a crash (the build uses the standard
// library's hardening mode, which fast-fails an out-of-range index) names the group it happened in.
//
// Every expected value is derived independently of the code under test:
//  - the join-code golden vectors come from running DOA2U's own join_code.c (doa2u-release 6c35c7b, read-only,
//    compiled in a scratch folder) on the same packed values: DOA2U's port 7000+i and DOA4's 7200+i pack to the
//    same 36 bits, a port outside the range packs the same 48 bits in both;
//  - the delay table, the pacer table, the STUN bytes and the plain-base64 strings come from a small model
//    script written from the brief's formulas (not from the C++), cross-checked against the brief's numbers;
//  - the STUN datagrams, the GitHub release lists and the identity texts are built by this file's own helpers
//    (Bld, StunMsg, Json), never by the code under test.

#include "../../src/net/np_rules.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace doa4::np;
using Bytes = std::vector<uint8_t>;
using std::string_view_literals::operator""sv;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;
static const char* g_group = "";
// Variadic: a check may carry commas outside parentheses (a braced initialiser, template arguments).
#define CHECK(...)                                                                        \
  do {                                                                                    \
    ++g_checks;                                                                           \
    if (!(__VA_ARGS__)) {                                                                 \
      std::printf("FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_group, #__VA_ARGS__);    \
      std::fflush(stdout); /* a later check may crash (a hardened out-of-range read) */   \
      ++g_failures;                                                                       \
    }                                                                                     \
  } while (0)

// Names the checks that follow: a FAIL line carries it, so a red check is attributable to a rule.
static void Group(const char* name) {
  g_group = name;
  if (g_verbose) {
    std::printf("group: %s\n", name);
    std::fflush(stdout);
  }
}

// ---- helpers (independent of the code under test) ----

static rules::Endpoint Ep(int a, int b, int c, int d, unsigned port) {
  rules::Endpoint e;
  e.ip = {uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d)};
  e.port = uint16_t(port);
  return e;
}
static std::span<const uint8_t> S(const Bytes& b) { return std::span<const uint8_t>(b.data(), b.size()); }
static bool Has(const std::string& hay, std::string_view needle) { return hay.find(needle) != std::string::npos; }

struct Rng {  // xorshift64: a seeded, reproducible stream
  uint64_t x;
  explicit Rng(uint64_t seed) : x(seed) {}
  uint64_t Next() {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  }
  size_t Below(size_t n) { return size_t(Next() % n); }
};

// =====================================================================================================
// Join codes (DOA2U join_code_test.c with 7000 -> 7200)
// =====================================================================================================

static char FlipCase(char c) {
  if (c >= 'a' && c <= 'z') return char(c - 32);
  if (c >= 'A' && c <= 'Z') return char(c + 32);
  return c == '0' ? '1' : '0';  // a digit, '-' or '_': another symbol of the alphabet
}

// ---- the join-code check symbol: a model written from the scheme in np_rules.cpp. GF(2^6) here is by log/antilog
// tables (the product multiplies by shift-and-xor) and phi by character class (the product by index arithmetic). ----
static const char* const kAlpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

struct GfTables {
  unsigned exp[126] = {};
  int lg[64] = {};
  GfTables() {
    unsigned v = 1;
    for (int i = 0; i < 63; ++i) {
      exp[i] = v;
      lg[v] = i;
      v <<= 1;
      if (v & 64) v ^= 0x43;  // x^6 + x + 1
    }
    for (int i = 63; i < 126; ++i) exp[i] = exp[i - 63];
  }
  unsigned Mul(unsigned a, unsigned b) const { return (a == 0 || b == 0) ? 0u : exp[lg[a] + lg[b]]; }
};
static const GfTables kGf;

static unsigned ModelPhi(char c) {
  if (c >= 'A' && c <= 'Z') return unsigned(c - 'A');
  if (c >= 'a' && c <= 'z') return 32u + unsigned(c - 'a');  // the upper-case value ^ 32
  if (c >= '0' && c <= '9') {
    const unsigned d = unsigned(c - '0');
    return d <= 5 ? 26 + d : 52 + d;
  }
  return c == '-' ? 62u : 63u;  // '_'
}
static const unsigned kModelW7[7] = {1, 2, 4, 8, 16, 32, 63};
static const unsigned kModelW9[9] = {1, 2, 4, 8, 16, 32, 7, 56, 11};
static const unsigned* ModelWeights(size_t symbols) { return symbols == 7 ? kModelW7 : kModelW9; }

static unsigned ModelSyndrome(const std::string& code) {  // 0 for a valid code
  const unsigned* w = ModelWeights(code.size());
  unsigned s = 0;
  for (size_t i = 0; i < code.size(); ++i) s ^= kGf.Mul(w[i], ModelPhi(code[i]));
  return s;
}
// Sets code[j] to the one symbol that makes the model's sum 0.
static std::string ModelSolveAt(std::string code, size_t j) {
  const unsigned* w = ModelWeights(code.size());
  unsigned s = 0;
  for (size_t i = 0; i < code.size(); ++i)
    if (i != j) s ^= kGf.Mul(w[i], ModelPhi(code[i]));
  for (const char* p = kAlpha; *p; ++p)
    if (kGf.Mul(w[j], ModelPhi(*p)) == s) {
      code[j] = *p;
      return code;
    }
  return std::string();  // cannot happen: w[j] != 0 and phi is a bijection
}
// A valid code of `symbols` symbols with `a` at pos and, when has_b, `b` at pos + 1; the other symbols are random,
// one position outside those is solved for. So every (position, symbol) and every adjacent pair can be built.
static std::string ModelCodeWith(size_t symbols, size_t pos, char a, bool has_b, char b, Rng& rng) {
  std::string code(symbols, 'A');
  for (char& c : code) c = kAlpha[rng.Below(64)];
  code[pos] = a;
  const size_t last_fixed = has_b ? pos + 1 : pos;
  if (has_b) code[pos + 1] = b;
  size_t free_pos = 0;
  if (free_pos == pos || free_pos == last_fixed) free_pos = symbols - 1;
  return ModelSolveAt(code, free_pos);
}
static bool Accepts(const std::string& code) {
  rules::Endpoint e;
  return rules::ParseJoinCode(code, &e, 7200);
}

static void TestJoinCodes() {
  static_assert(rules::kPortBase == 7200 && rules::kPorts == 16);  // spec 5: DOA4's range 7200-7215
  rules::Endpoint back;
  Rng rng(0xC0DE0003ull);

  Group("join code: 7 symbols (6 + check) for a port inside 7200-7215, and the code round-trips");
  for (const rules::Endpoint& e : {Ep(203, 0, 113, 7, 7203), Ep(203, 0, 113, 7, 7215), Ep(203, 0, 113, 7, 7200)}) {
    const std::string code = rules::MakeJoinCode(e);
    CHECK(code.size() == 7);
    CHECK(ModelSyndrome(code) == 0);
    back = Ep(9, 9, 9, 9, 999);
    CHECK(rules::ParseJoinCode(code, &back, 7200) && back == e);
  }

  Group("join code: 9 symbols (8 + check) for a port outside the range (either side of it)");
  for (const rules::Endpoint& e : {Ep(198, 51, 100, 20, 49152), Ep(198, 51, 100, 20, 7216), Ep(198, 51, 100, 20, 7199),
                                   Ep(198, 51, 100, 20, 1), Ep(198, 51, 100, 20, 65535)}) {
    const std::string code = rules::MakeJoinCode(e);
    CHECK(code.size() == 9);
    CHECK(ModelSyndrome(code) == 0);
    back = Ep(9, 9, 9, 9, 999);
    CHECK(rules::ParseJoinCode(code, &back, 7200) && back == e);
  }
  CHECK(rules::MakeJoinCode(Ep(1, 2, 3, 4, 0)).empty());  // port 0: nothing to encode

  Group("join code: round trip for ALL 16 port indices, on several addresses");
  for (const rules::Endpoint& base : {Ep(203, 0, 113, 7, 0), Ep(10, 20, 30, 40, 0), Ep(1, 0, 0, 0, 0), Ep(255, 255, 255, 255, 0)}) {
    std::set<std::string> seen;
    for (unsigned index = 0; index < 16; ++index) {
      rules::Endpoint e = base;
      e.port = uint16_t(7200 + index);
      const std::string code = rules::MakeJoinCode(e);
      seen.insert(code);
      CHECK(code.size() == 7);
      back = Ep(9, 9, 9, 9, 999);
      CHECK(rules::ParseJoinCode(code, &back, 7200) && back == e);
    }
    CHECK(seen.size() == 16);  // sixteen ports, sixteen different codes
  }

  Group("join code: golden vectors - payload from DOA2U's join_code.c (the key and rounds), check symbol from the model script");
  {
    struct G { rules::Endpoint ep; const char* code; };
    const G golden[] = {
        {Ep(203, 0, 113, 7, 7203), "N-iddwg"},      {Ep(203, 0, 113, 7, 7215), "Orjdz6c"},
        {Ep(198, 51, 100, 20, 49152), "q-k-QhCu5"}, {Ep(198, 51, 100, 20, 7216), "Jnjg3w5aA"},
        {Ep(198, 51, 100, 20, 7199), "M8SdzSwln"},  {Ep(192, 168, 1, 5, 7200), "RRyjQg8"},
        {Ep(255, 255, 255, 255, 65535), "Yj4PF0PoI"}, {Ep(1, 0, 0, 0, 7200), "hqnptDF"},
        {Ep(10, 20, 30, 40, 7208), "pt4SMa1"},      {Ep(127, 0, 0, 1, 7201), "eGazdOJ"},
        {Ep(8, 8, 8, 8, 1), "DZHKPrOWY"},           {Ep(192, 168, 1, 5, 7207), "jAwLL2p"},
    };
    for (const G& g : golden) {
      CHECK(rules::MakeJoinCode(g.ep) == g.code);
      back = Ep(9, 9, 9, 9, 999);
      CHECK(rules::ParseJoinCode(g.code, &back, 7200) && back == g.ep);
    }
    // JoinCodeTextOf takes the packed value: ip << 4 | port index (7 symbols), ip << 16 | port (9 symbols).
    CHECK(rules::JoinCodeTextOf(0xCB0071073ull, 7) == "N-iddwg");        // 203.0.113.7 = CB007107, index 3
    CHECK(rules::JoinCodeTextOf(0xC6336414C000ull, 9) == "q-k-QhCu5");   // 198.51.100.20 = C6336414, port C000 = 49152
    CHECK(rules::JoinCodeTextOf(0xCB0071073ull, 7) == rules::MakeJoinCode(Ep(203, 0, 113, 7, 7203)));
  }

  Group("join code: JoinCodeTextOf knows only 7 and 9 symbols");
  for (int n : {-7, 0, 1, 5, 6, 8, 10, 11, 12, 64}) CHECK(rules::JoinCodeTextOf(0x123456789ull, n).empty());
  CHECK(rules::JoinCodeTextOf(0x123456789ull, 7).size() == 7 && rules::JoinCodeTextOf(0x123456789ull, 9).size() == 9);

  Group("join code: the code does not show the address (its payload is not the plain base64 of it)");
  // plain base64 (URL-safe alphabet) of the packed values, from the model script
  CHECK(rules::MakeJoinCode(Ep(203, 0, 113, 7, 7203)).substr(0, 6) != "ywBxBz");
  CHECK(rules::MakeJoinCode(Ep(198, 51, 100, 20, 49152)).substr(0, 8) != "xjNkFMAA");
  {  // neighbouring addresses give unrelated codes: most symbols differ, where the plain encoding changed one or two
    unsigned differ = 0, total = 0;
    for (int i = 1; i < 200; ++i) {
      const std::string a = rules::MakeJoinCode(Ep(192, 168, 1, i, 7200));
      const std::string b = rules::MakeJoinCode(Ep(192, 168, 1, i + 1, 7200));
      for (size_t k = 0; k < 6; ++k) differ += a[k] != b[k];
      total += 6;
    }
    CHECK(total == 199 * 6 && differ * 2 > total);
  }

  // the sample of real codes the edit tests below use: 5,000 addresses in both forms
  std::vector<std::pair<std::string, rules::Endpoint>> sample;
  Group("join code: 5,000 addresses round-trip, in both forms (every port index among them)");
  for (unsigned i = 0; i < 5000; ++i) {
    const unsigned v = i * 2654435761u;
    const bool in_range = (i & 1) != 0;
    const unsigned port = in_range ? 7200 + ((i >> 1) % 16) : 1024 + (i * 7919u) % 64000;
    const rules::Endpoint e = Ep(int(v >> 24) | 1, int(v >> 16) & 255, int(v >> 8) & 255, int(v) & 255, port);
    const std::string code = rules::MakeJoinCode(e);
    const bool indexed = port >= 7200 && port <= 7215;
    CHECK(code.size() == (indexed ? 7u : 9u));
    CHECK(ModelSyndrome(code) == 0);
    back = Ep(9, 9, 9, 9, 999);
    CHECK(rules::ParseJoinCode(code, &back, 7200) && back == e);
    sample.emplace_back(code, e);
  }

  Group("join code: the check symbol agrees with the model on random texts, both lengths, valid or not");
  {
    unsigned long long both_valid = 0, accepted_but_invalid = 0, valid_but_refused = 0;
    for (size_t symbols : {size_t(7), size_t(9)}) {
      for (int i = 0; i < 200000; ++i) {
        std::string t(symbols, 'A');
        for (char& c : t) c = kAlpha[rng.Below(64)];
        const bool model_valid = ModelSyndrome(t) == 0;
        const bool product_accepts = Accepts(t);
        accepted_but_invalid += product_accepts && !model_valid;
        valid_but_refused += model_valid && !product_accepts;  // only a text that decodes to port 0 (once in 65,536)
        both_valid += model_valid && product_accepts;
      }
    }
    CHECK(accepted_but_invalid == 0);
    CHECK(valid_but_refused <= 1);
    CHECK(both_valid > 4000);  // about 1 in 64 of 400,000: the loop saw valid codes too
  }

  Group("join code: EVERY single-symbol substitution is refused - every position, every symbol, every other symbol");
  {
    unsigned long long codes = 0, edits = 0, bad = 0, skipped = 0;
    for (size_t symbols : {size_t(7), size_t(9)}) {
      for (size_t pos = 0; pos < symbols; ++pos) {
        for (const char* pa = kAlpha; *pa; ++pa) {
          const std::string code = ModelCodeWith(symbols, pos, *pa, false, 'A', rng);
          // the control: the unedited code is accepted (a random 9-symbol code decodes to port 0 once in 65,536: skipped)
          if (!Accepts(code)) { ++skipped; continue; }
          ++codes;
          for (const char* pb = kAlpha; *pb; ++pb) {
            if (*pb == *pa) continue;
            std::string edited = code;
            edited[pos] = *pb;
            ++edits;
            bad += Accepts(edited);
          }
        }
      }
    }
    CHECK(codes + skipped == (7 + 9) * 64 && edits == codes * 63);
    CHECK(skipped <= 3 && bad == 0);
  }

  Group("join code: EVERY adjacent swap of two different symbols is refused - every pair of positions, every pair of symbols");
  {
    unsigned long long codes = 0, swaps = 0, bad = 0, skipped = 0;
    for (size_t symbols : {size_t(7), size_t(9)}) {
      for (size_t pos = 0; pos + 1 < symbols; ++pos) {  // pos + 1 == symbols - 1 is the last payload symbol with the check
        for (const char* pa = kAlpha; *pa; ++pa) {
          for (const char* pb = kAlpha; *pb; ++pb) {
            const std::string code = ModelCodeWith(symbols, pos, *pa, true, *pb, rng);
            if (!Accepts(code)) { ++skipped; continue; }  // (a 9-symbol code that decodes to port 0: once in 65,536)
            ++codes;
            std::string swapped = code;
            std::swap(swapped[pos], swapped[pos + 1]);
            if (*pa == *pb) {
              bad += swapped != code || !Accepts(swapped);  // two equal symbols: the swap is the same text
            } else {
              ++swaps;
              bad += Accepts(swapped);
            }
          }
        }
      }
    }
    CHECK(codes + skipped == (6 + 8) * 64 * 64 && swaps <= (6 + 8) * 64 * 63 && swaps > (6 + 8) * 64 * 63 - 64);
    CHECK(skipped <= 6 && bad == 0);
  }

  Group("join code: the same on the 5,000 real codes (substitution at every position, adjacent swaps, a wrong check symbol)");
  {
    unsigned long long edits = 0, swaps = 0, bad = 0;
    std::set<std::pair<size_t, char>> covered;  // (position, symbol) pairs the real codes contain
    for (const auto& [code, e] : sample) {
      for (size_t pos = 0; pos < code.size(); ++pos) {
        if (code.size() == 7) covered.emplace(pos, code[pos]);
        for (const char* pb = kAlpha; *pb; ++pb) {
          if (*pb == code[pos]) continue;
          std::string edited = code;
          edited[pos] = *pb;
          ++edits;
          bad += Accepts(edited);
        }
      }
      for (size_t pos = 0; pos + 1 < code.size(); ++pos) {
        std::string swapped = code;
        std::swap(swapped[pos], swapped[pos + 1]);
        if (code[pos] == code[pos + 1]) continue;
        ++swaps;
        bad += Accepts(swapped);
      }
    }
    CHECK(edits > 2000000 && swaps > 30000);
    CHECK(bad == 0);
    CHECK(covered.size() == 7 * 64);  // the 7-symbol sample holds every symbol at every position
  }

  Group("join code: lower-casing a code is refused (except the few position sets the check cannot see)");
  {
    // Lower-casing adds 32 to phi at each upper-case letter, so it changes the sum by 32 * xor(weights of those
    // positions): refused unless that xor is 0 - all 7 symbols upper-case (7 symbols), or one of 7 sets of >= 4
    // positions (9 symbols). The expectation below is computed from the weights, not from the product.
    unsigned long long with_upper = 0, refused = 0, wrong = 0;
    auto lower = [](std::string s) {
      for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c + 32);
      return s;
    };
    auto expect_refused = [](const std::string& code) {
      const unsigned* w = ModelWeights(code.size());
      unsigned x = 0;
      for (size_t i = 0; i < code.size(); ++i)
        if (code[i] >= 'A' && code[i] <= 'Z') x ^= w[i];
      return x != 0;
    };
    for (const auto& [code, e] : sample) {
      const std::string low = lower(code);
      if (low == code) continue;  // no upper-case letter
      ++with_upper;
      const bool accepted = Accepts(low);
      refused += !accepted;
      wrong += accepted == expect_refused(code);
    }
    CHECK(with_upper > 4000);
    CHECK(wrong == 0);                       // exactly the predicted codes are accepted, none other
    CHECK(refused * 100 > with_upper * 98);  // about 99% of the real codes with a capital are refused when lower-cased
    // the golden codes, each lower-cased: refused (none of them is one of the unseen sets)
    for (const char* g : {"N-iddwg", "Orjdz6c", "q-k-QhCu5", "Jnjg3w5aA", "M8SdzSwln", "RRyjQg8", "Yj4PF0PoI", "hqnptDF",
                          "pt4SMa1", "eGazdOJ", "DZHKPrOWY", "jAwLL2p"}) {
      CHECK(lower(g) != g && !Accepts(lower(g)));
    }
    // the limit, pinned: a 7-symbol code made only of capital letters lower-cases to ANOTHER valid code
    std::string all_upper;
    for (int tries = 0; tries < 1000 && all_upper.empty(); ++tries) {
      std::string payload(6, 'A');
      for (char& c : payload) c = char('A' + rng.Below(26));
      std::string code = payload + 'A';
      code = ModelSolveAt(code, 6);
      if (code[6] >= 'A' && code[6] <= 'Z') all_upper = code;
    }
    CHECK(all_upper.size() == 7 && Accepts(all_upper));
    CHECK(Accepts(lower(all_upper)) && lower(all_upper) != all_upper);
  }

  Group("join code: case-sensitive - one changed symbol is refused, at every position");
  for (const rules::Endpoint& e : {Ep(203, 0, 113, 7, 7203), Ep(198, 51, 100, 20, 49152)}) {
    const std::string code = rules::MakeJoinCode(e);
    for (size_t k = 0; k < code.size(); ++k) {
      std::string flipped = code;
      flipped[k] = FlipCase(flipped[k]);
      CHECK(flipped != code);
      CHECK(!Accepts(flipped));
    }
  }

  Group("join code: blanks around and inside are ignored");
  {
    const rules::Endpoint e = Ep(203, 0, 113, 7, 7203);
    const std::string code = rules::MakeJoinCode(e);
    const std::string spaced = "  " + code.substr(0, 3) + " " + code.substr(3) + " \r\n";
    back = Ep(9, 9, 9, 9, 999);
    CHECK(rules::ParseJoinCode(spaced, &back, 7200) && back == e);
    std::string everywhere = "\t";
    for (char c : code) { everywhere += c; everywhere += (c == code[0]) ? " \t" : "\r\n"; }
    back = Ep(9, 9, 9, 9, 999);
    CHECK(rules::ParseJoinCode(everywhere, &back, 7200) && back == e);
    const rules::Endpoint e9 = Ep(198, 51, 100, 20, 49152);
    const std::string code9 = rules::MakeJoinCode(e9);
    back = Ep(9, 9, 9, 9, 999);
    CHECK(rules::ParseJoinCode(" " + code9.substr(0, 4) + "\t" + code9.substr(4) + "\n", &back, 7200) && back == e9);
    // blanks do not count toward the length: 9 symbols with blanks is still 9, 10 symbols with blanks is 10
    CHECK(!rules::ParseJoinCode(" " + code9 + " A ", &back, 7200));
  }

  Group("join code: dotted addresses with the default port, with a port, with blanks");
  back = Ep(9, 9, 9, 9, 999);
  CHECK(rules::ParseJoinCode("192.168.1.5", &back, 7204) && back == Ep(192, 168, 1, 5, 7204));
  CHECK(rules::ParseJoinCode("192.168.1.5", &back) && back == Ep(192, 168, 1, 5, 7200));  // default_port = kPortBase
  CHECK(rules::ParseJoinCode("192.168.1.5:7201", &back, 7204) && back.port == 7201);
  CHECK(rules::ParseJoinCode("127.0.0.1:7201 ", &back, 7200) && back.ip[0] == 127 && back.port == 7201);
  CHECK(rules::ParseJoinCode("  192.168.1.5:7201 \t\r\n", &back, 7204) && back == Ep(192, 168, 1, 5, 7201));
  // accept edges: the largest octet and port, the smallest port, and every octet alone being the non-zero one
  CHECK(rules::ParseJoinCode("255.255.255.255:65535", &back, 7200) && back == Ep(255, 255, 255, 255, 65535));
  CHECK(rules::ParseJoinCode("1.2.3.4:1", &back, 7200) && back == Ep(1, 2, 3, 4, 1));
  CHECK(rules::ParseJoinCode("1.0.0.0", &back, 7200) && back == Ep(1, 0, 0, 0, 7200));
  CHECK(rules::ParseJoinCode("0.1.0.0", &back, 7200) && back == Ep(0, 1, 0, 0, 7200));
  CHECK(rules::ParseJoinCode("0.0.1.0", &back, 7200) && back == Ep(0, 0, 1, 0, 7200));
  CHECK(rules::ParseJoinCode("0.0.0.1", &back, 7200) && back == Ep(0, 0, 0, 1, 7200));
  CHECK(rules::ParseJoinCode("192.168.100.100:7200", &back, 7200) && back == Ep(192, 168, 100, 100, 7200));

  Group("join code: the same non-zero rule for the code forms (address 0.0.0.1 is valid, 0.0.0.0 is not)");
  CHECK(rules::ParseJoinCode(rules::JoinCodeTextOf(0x1ull << 4, 7), &back, 7200) && back == Ep(0, 0, 0, 1, 7200));
  CHECK(rules::ParseJoinCode(rules::JoinCodeTextOf(0x100ull << 4 | 15, 7), &back, 7200) && back == Ep(0, 0, 1, 0, 7215));
  CHECK(rules::ParseJoinCode(rules::JoinCodeTextOf(0x1000000ull << 4 | 5, 7), &back, 7200) && back == Ep(1, 0, 0, 0, 7205));
  CHECK(rules::ParseJoinCode(rules::JoinCodeTextOf(0x10000ull << 4 | 2, 7), &back, 7200) && back == Ep(0, 1, 0, 0, 7202));
  // a 9-symbol code carries the full port even inside the range (MakeJoinCode never writes one)
  CHECK(rules::ParseJoinCode(rules::JoinCodeTextOf(0xC6336414ull << 16 | 7203, 9), &back, 7200) &&
        back == Ep(198, 51, 100, 20, 7203));
  CHECK(rules::ParseJoinCode(rules::JoinCodeTextOf(0x1ull << 16 | 1, 9), &back, 7200) && back == Ep(0, 0, 0, 1, 1));

  Group("join code: rejections, and the entry field's text");
  {
    const rules::Endpoint sentinel = Ep(9, 9, 9, 9, 999);
    const std::string good7 = rules::MakeJoinCode(Ep(203, 0, 113, 7, 7203));
    const std::string good9 = rules::MakeJoinCode(Ep(198, 51, 100, 20, 49152));
    const std::string rejects[] = {
        good7.substr(0, 6),                    // the payload alone: the old 6-symbol form
        good7.substr(0, 5),                    // 5 symbols
        good7 + "A",                           // 8
        good9.substr(0, 8),                    // the payload alone: the old 8-symbol form
        good9 + "A",                           // 10
        std::string(40, 'A'),                  // far too long
        "ywB!BzA",                             // not in the alphabet
        "ywB+BzA", "ywB/BzA", "ywB=BzA",       // standard base64 symbols are not in the URL-safe alphabet
        std::string("ywB") + char(0xE9) + "BzA",  // a high byte (a signed-char table index would be negative)
        std::string("ywB") + char(0xFF) + "BzA",
        std::string("ywB") + '\0' + "BzA",     // an embedded NUL is not a symbol
        "",                                    // nothing
        "  \r\n",                              // blanks only
        "192.168.1.5:0",                       // port 0
        "192.168.1.5:",                        // a colon without digits
        "192.168.1.5:7200x",                   // trailing junk
        "192.168.1.5x",
        "192.168.1.5 :7200",                   // a blank before the colon
        "192.168.1.5:7200:1",
        "256.1.1.1",                           // octet over 255
        "1.2.3.256",
        "1.2.3.1000",                          // four digits in an octet
        "1234.1.1.1",
        "0001.2.3.4", "1.2.3.0004", "1.2.0004.4",  // four digits that fit 255: still four
        "192.168.1.5:70000",                   // port over 65535
        "1.2.3.4:65536",                       // the first port over
        "1.2.3.4:99999999999999999999",        // a port that would overflow
        "0.0.0.0", "0.0.0.0:7200",             // the zero address
        "1.2.3",                               // three octets
        "1.2.3.4.5",                           // five
        "1..3.4", ".1.2.3", "1.2.3.",          // empty octets
        "-1.2.3.4",
        rules::JoinCodeTextOf(0, 7),                         // a code (valid check) of 0.0.0.0
        rules::JoinCodeTextOf(0x5ull, 7),                    // 0.0.0.0, port index 5
        rules::JoinCodeTextOf(0x01020304ull << 16, 9),       // a code of port 0
        rules::JoinCodeTextOf(0, 9),                         // 0.0.0.0 and port 0
        rules::JoinCodeTextOf(0x1ull << 16, 9),              // 0.0.0.1, port 0
    };
    for (const std::string& r : rejects) {
      rules::Endpoint got = sentinel;
      CHECK(!rules::ParseJoinCode(r, &got, 7200));
      CHECK(got == sentinel);  // a refused text leaves the caller's endpoint as it was
    }
    // a VALID code with the high bit set on one symbol: a lookup that masks each byte to 7 bits would take it for the
    // original code (the check symbol no longer catches that, so it needs its own check)
    for (const std::string& code : {good7, good9}) {
      for (size_t k = 0; k < code.size(); ++k) {
        std::string t = code;
        t[k] = char(uint8_t(t[k]) | 0x80);
        CHECK(!Accepts(t));
      }
    }
    CHECK(!rules::ParseJoinCode("192.168.1.5", &back, 0));  // no default port: nothing to connect to
    CHECK(!rules::ParseJoinCode("1.2.3.4", nullptr, 7200));
    CHECK(!rules::ParseJoinCode(std::string_view(), &back, 7200));
    CHECK(rules::ParseJoinCode(good7, nullptr, 7200) == false);
    CHECK(std::string(rules::kBadCodeText) == "That code doesn't look right - check it (capital letters matter)");
  }

  Group("join code: EndpointText");
  CHECK(rules::EndpointText(Ep(203, 0, 113, 7, 7203)) == "203.0.113.7:7203");
  CHECK(rules::EndpointText(Ep(0, 0, 0, 0, 0)) == "0.0.0.0:0");
  CHECK(rules::EndpointText(Ep(255, 255, 255, 255, 65535)) == "255.255.255.255:65535");
  CHECK(rules::EndpointText(Ep(10, 0, 9, 100, 7210)) == "10.0.9.100:7210");
}

// =====================================================================================================
// STUN (RFC 5389 binding)
// =====================================================================================================

struct Bld {  // big-endian builder for STUN datagrams
  Bytes b;
  Bld& U8(unsigned v) { b.push_back(uint8_t(v & 0xFF)); return *this; }
  Bld& U16(unsigned v) { U8(v >> 8); return U8(v); }
  Bld& Raw(const Bytes& r) { b.insert(b.end(), r.begin(), r.end()); return *this; }
  Bld& Fill(size_t n, uint8_t v) { b.insert(b.end(), n, v); return *this; }
};
struct Attr { unsigned type; Bytes value; };

static const std::array<uint8_t, 4> kCookie = {0x21, 0x12, 0xA4, 0x42};

static Bytes XorMapped(const rules::Endpoint& e) {  // value of an XOR-MAPPED-ADDRESS, IPv4
  Bld v;
  v.U8(0).U8(1).U16(e.port ^ 0x2112);
  for (int i = 0; i < 4; ++i) v.U8(e.ip[size_t(i)] ^ kCookie[size_t(i)]);
  return v.b;
}
static Bytes Mapped(const rules::Endpoint& e) {  // value of a MAPPED-ADDRESS, IPv4
  Bld v;
  v.U8(0).U8(1).U16(e.port);
  for (int i = 0; i < 4; ++i) v.U8(e.ip[size_t(i)]);
  return v.b;
}
// header (type, message length = the attribute bytes incl. padding + len_adjust, cookie, tid) + attributes
static Bytes StunMsg(unsigned type, const std::array<uint8_t, 12>& tid, const std::vector<Attr>& attrs, int len_adjust = 0) {
  Bld body;
  for (const Attr& a : attrs) {
    body.U16(a.type).U16(unsigned(a.value.size())).Raw(a.value);
    body.Fill((4 - a.value.size() % 4) % 4, 0);
  }
  Bld m;
  m.U16(type).U16(unsigned(int(body.b.size()) + len_adjust));
  for (uint8_t c : kCookie) m.U8(c);
  for (uint8_t t : tid) m.U8(t);
  m.Raw(body.b);
  return m.b;
}

static const std::array<uint8_t, 12> kTid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

// Asserts a refusal and that the caller's endpoint is left as it was.
static void StunRefused(const Bytes& resp, const std::array<uint8_t, 12>& tid, int line) {
  rules::Endpoint sentinel = Ep(9, 9, 9, 9, 999), m = sentinel;
  ++g_checks;
  if (rules::StunMapped(S(resp), tid, &m) || !(m == sentinel)) {
    std::printf("FAIL %s:%d [%s]: a refused response must return false and leave *mapped alone\n", __FILE__, line, g_group);
    std::fflush(stdout);
    ++g_failures;
  }
}

static void TestStun() {
  Group("STUN: the binding request");
  {
    const auto req = rules::StunRequest(kTid);
    CHECK(req.size() == 20);
    CHECK(req[0] == 0x00 && req[1] == 0x01 && req[2] == 0x00 && req[3] == 0x00);     // binding request, no attributes
    CHECK(req[4] == 0x21 && req[5] == 0x12 && req[6] == 0xA4 && req[7] == 0x42);     // the magic cookie
    CHECK(std::equal(kTid.begin(), kTid.end(), req.begin() + 8));                   // the transaction id
    const std::array<uint8_t, 12> other = {0xF0, 0xE0, 0xD0, 0xC0, 0xB0, 0xA0, 0x90, 0x80, 0x70, 0x60, 0x50, 0x40};
    const auto req2 = rules::StunRequest(other);
    CHECK(std::equal(other.begin(), other.end(), req2.begin() + 8) && req2[1] == 0x01 && req2[4] == 0x21);
  }

  Group("STUN: IsStun is the cookie at bytes 4..7 of at least 20 bytes");
  {
    const auto req = rules::StunRequest(kTid);
    Bytes r(req.begin(), req.end());
    CHECK(rules::IsStun(S(r)));
    CHECK(!rules::IsStun(std::span<const uint8_t>(r.data(), 19)));  // one byte short
    CHECK(!rules::IsStun(std::span<const uint8_t>()));
    for (size_t k = 4; k < 8; ++k) {  // each cookie byte, changed alone
      Bytes c = r;
      c[k] ^= 0x01;
      CHECK(!rules::IsStun(S(c)));
    }
    Bytes wrong_offset(20, 0);  // the cookie at bytes 0..3
    wrong_offset[0] = 0x21; wrong_offset[1] = 0x12; wrong_offset[2] = 0xA4; wrong_offset[3] = 0x42;
    CHECK(!rules::IsStun(S(wrong_offset)));
    CHECK(!rules::IsStun(S(Bytes(20, 0))));
    Bytes longer = r;
    longer.insert(longer.end(), 100, 0xAB);  // a longer datagram is still STUN
    CHECK(rules::IsStun(S(longer)));
    // the lobby tells STUN from its own packets by this test: a DOA4 datagram is never STUN
    const Bytes ping_body = {1, 0, 0, 0};
    const Bytes ping = wire::Encode(wire::Type::kPing, 0x12345678u, S(ping_body));
    CHECK(ping.size() == wire::kHeaderBytes + 4);
    Bytes padded = ping;
    padded.resize(40, 0);
    CHECK(!rules::IsStun(S(padded)));
    CHECK(!rules::IsStun(S(ping)));
  }

  Group("STUN: the brief's response (XOR-MAPPED-ADDRESS 203.0.113.7:51234)");
  {
    const uint8_t resp[32] = {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                              0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xE9, 0x30, 0xEA, 0x12, 0xD5, 0x45};
    rules::Endpoint m;
    CHECK(rules::StunMapped(resp, kTid, &m) && m.port == 51234 && m.ip == std::array<uint8_t, 4>{203, 0, 113, 7});
    // this file's builder produces the same bytes (so the other fixtures are built the same way)
    const Bytes built = StunMsg(0x0101, kTid, {{0x0020, XorMapped(Ep(203, 0, 113, 7, 51234))}});
    CHECK(built == Bytes(resp, resp + 32));
    CHECK(rules::IsStun(S(built)));
  }

  Group("STUN: RFC 5769 section 2.2, the sample IPv4 response (an externally known answer: 192.0.2.1:32853)");
  {
    // Header, SOFTWARE "test vector" (11 bytes + 1 pad), XOR-MAPPED-ADDRESS (port 0x8055 ^ 0x2112 = 0xA147, address
    // C0 00 02 01 ^ 21 12 A4 42 = E1 12 A6 43), MESSAGE-INTEGRITY and FINGERPRINT. This parser checks neither of the
    // last two, so their values here are placeholders (0xAA, 0xBB); the attribute sizes are the RFC's: 16 + 12 + 24 + 8 = 60.
    const std::array<uint8_t, 12> rfc_tid = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
    Bytes m = {0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42};
    m.insert(m.end(), rfc_tid.begin(), rfc_tid.end());
    m.insert(m.end(), {0x80, 0x22, 0x00, 0x0b, 't', 'e', 's', 't', ' ', 'v', 'e', 'c', 't', 'o', 'r', 0x20});
    m.insert(m.end(), {0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43});
    m.insert(m.end(), {0x00, 0x08, 0x00, 0x14});
    m.insert(m.end(), 20, 0xAA);
    m.insert(m.end(), {0x80, 0x28, 0x00, 0x04, 0xBB, 0xBB, 0xBB, 0xBB});
    CHECK(m.size() == 20 + 60);
    rules::Endpoint got;
    CHECK(rules::StunMapped(S(m), rfc_tid, &got) && got == Ep(192, 0, 2, 1, 32853));
    CHECK(rules::IsStun(S(m)));
    StunRefused(m, kTid, __LINE__);  // not this transaction
  }

  Group("STUN: other mapped addresses, both attribute kinds");
  for (const rules::Endpoint& e : {Ep(198, 51, 100, 20, 49152), Ep(10, 20, 30, 40, 7200), Ep(255, 255, 255, 255, 65535),
                                   Ep(1, 1, 1, 1, 1), Ep(203, 0, 113, 7, 0x2112)}) {
    rules::Endpoint m;
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x0020, XorMapped(e)}})), kTid, &m) && m == e);
    m = Ep(9, 9, 9, 9, 999);
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x0001, Mapped(e)}})), kTid, &m) && m == e);  // no XOR here
  }

  Group("STUN: XOR-MAPPED-ADDRESS wins over MAPPED-ADDRESS in either order; padding and unknown attributes");
  {
    const rules::Endpoint xored = Ep(203, 0, 113, 7, 51234), plain = Ep(9, 9, 9, 9, 1111);
    rules::Endpoint m;
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x0001, Mapped(plain)}, {0x0020, XorMapped(xored)}})), kTid, &m) && m == xored);
    m = Ep(0, 0, 0, 0, 0);
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x0020, XorMapped(xored)}, {0x0001, Mapped(plain)}})), kTid, &m) && m == xored);
    // an attribute of 5 bytes is padded to 8 before the next one; one of 4 is not padded; one of 0 is just a header
    m = Ep(0, 0, 0, 0, 0);
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x8022, Bytes{'a', 'b', 'c', 'd', 'e'}}, {0x0020, XorMapped(xored)}})), kTid, &m) && m == xored);
    m = Ep(0, 0, 0, 0, 0);
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x8028, Bytes{1, 2, 3, 4}}, {0x0020, XorMapped(xored)}})), kTid, &m) && m == xored);
    m = Ep(0, 0, 0, 0, 0);
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x8029, Bytes{}}, {0x0020, XorMapped(xored)}})), kTid, &m) && m == xored);
    m = Ep(0, 0, 0, 0, 0);
    CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x8022, Bytes{1, 2, 3}}, {0x8022, Bytes{1, 2, 3, 4, 5, 6}}, {0x0020, XorMapped(xored)}})), kTid, &m) && m == xored);
    // bytes after the message the header describes are ignored
    Bytes trailing = StunMsg(0x0101, kTid, {{0x0020, XorMapped(xored)}});
    trailing.insert(trailing.end(), {0xDE, 0xAD, 0xBE, 0xEF});
    m = Ep(0, 0, 0, 0, 0);
    CHECK(rules::StunMapped(S(trailing), kTid, &m) && m == xored);
  }

  Group("STUN: a response that is not a usable binding success for this transaction");
  {
    const rules::Endpoint e = Ep(203, 0, 113, 7, 51234);
    const Bytes good = StunMsg(0x0101, kTid, {{0x0020, XorMapped(e)}});
    rules::Endpoint m;
    CHECK(rules::StunMapped(S(good), kTid, &m));  // the control: this one is accepted
    for (size_t k = 0; k < 12; ++k) {              // another transaction id, differing in one byte, at each position
      auto other = kTid;
      other[k] ^= 0x80;
      StunRefused(good, other, __LINE__);
    }
    for (size_t n = 0; n < good.size(); ++n) StunRefused(Bytes(good.begin(), good.begin() + long(n)), kTid, __LINE__);  // every cut, 31 included
    StunRefused(StunMsg(0x0111, kTid, {{0x0020, XorMapped(e)}}), kTid, __LINE__);   // a binding error
    StunRefused(StunMsg(0x0001, kTid, {{0x0020, XorMapped(e)}}), kTid, __LINE__);   // a binding request
    StunRefused(StunMsg(0x0102, kTid, {{0x0020, XorMapped(e)}}), kTid, __LINE__);
    StunRefused(StunMsg(0x0101, kTid, {}), kTid, __LINE__);                         // a success with no attribute
    StunRefused(StunMsg(0x0101, kTid, {{0x8022, Bytes{1, 2, 3, 4}}}), kTid, __LINE__);  // no mapped address in it
    for (size_t k = 4; k < 8; ++k) {                                               // a wrong cookie byte, alone
      Bytes c = good;
      c[k] ^= 0x01;
      StunRefused(c, kTid, __LINE__);
    }
    {  // an attribute whose length points past the end of the datagram
      Bytes c = good;
      c[22] = 0x00; c[23] = 0x09;  // XOR-MAPPED-ADDRESS claims 9 value bytes, 8 are there
      StunRefused(c, kTid, __LINE__);
      c[22] = 0xFF; c[23] = 0xFF;
      StunRefused(c, kTid, __LINE__);
    }
    // the message length field says less than the attribute needs although the datagram holds it all
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, XorMapped(e)}}, -4), kTid, __LINE__);
    // ... and more than the datagram holds
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, XorMapped(e)}}, +4), kTid, __LINE__);
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, XorMapped(e)}}, +0x100), kTid, __LINE__);
    // an address attribute too short to hold an IPv4 address (and for lengths 0..3, too short to name a family),
    // last in the datagram so that reading a family byte that is not there reads past the end
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, Bytes{}}}), kTid, __LINE__);
    StunRefused(StunMsg(0x0101, kTid, {{0x0001, Bytes{}}}), kTid, __LINE__);
    for (size_t n = 1; n <= 3; ++n) {
      StunRefused(StunMsg(0x0101, kTid, {{0x0020, Bytes(n, 0x01)}}), kTid, __LINE__);
      StunRefused(StunMsg(0x0101, kTid, {{0x0001, Bytes(n, 0x01)}}), kTid, __LINE__);
    }
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, Bytes{0, 1, 0xE9, 0x30}}}), kTid, __LINE__);
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, Bytes{0, 1, 0xE9, 0x30, 0xEA, 0x12, 0xD5}}}), kTid, __LINE__);
    Bytes v6 = {0, 2, 0xE9, 0x30};
    v6.insert(v6.end(), 16, 0x11);
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, v6}}), kTid, __LINE__);
    // a mapped address nobody can use: port 0 or 0.0.0.0, in either attribute kind (fix round 1)
    for (const rules::Endpoint& z : {Ep(203, 0, 113, 7, 0), Ep(0, 0, 0, 0, 51234), Ep(0, 0, 0, 0, 0)}) {
      StunRefused(StunMsg(0x0101, kTid, {{0x0020, XorMapped(z)}}), kTid, __LINE__);
      StunRefused(StunMsg(0x0101, kTid, {{0x0001, Mapped(z)}}), kTid, __LINE__);
    }
    // the XOR attribute is the one used: a zero XOR answer is refused even beside a usable MAPPED one
    StunRefused(StunMsg(0x0101, kTid, {{0x0020, XorMapped(Ep(0, 0, 0, 0, 0))}, {0x0001, Mapped(e)}}), kTid, __LINE__);
    // the nearest usable answers are accepted (controls: every octet alone non-zero, the smallest port)
    for (const rules::Endpoint& u : {Ep(203, 0, 113, 7, 1), Ep(0, 0, 0, 1, 51234), Ep(0, 0, 1, 0, 51234),
                                     Ep(0, 1, 0, 0, 51234), Ep(1, 0, 0, 0, 51234)}) {
      rules::Endpoint got;
      CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x0020, XorMapped(u)}})), kTid, &got) && got == u);
      got = Ep(9, 9, 9, 9, 999);
      CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x0001, Mapped(u)}})), kTid, &got) && got == u);
    }
    {  // a zero MAPPED-ADDRESS beside a usable XOR one does not matter: the XOR one wins
      rules::Endpoint got;
      CHECK(rules::StunMapped(S(StunMsg(0x0101, kTid, {{0x0001, Mapped(Ep(0, 0, 0, 0, 0))}, {0x0020, XorMapped(e)}})), kTid, &got) && got == e);
    }
    {  // two stray bytes after the last attribute, counted in the message length: not a whole attribute header
      Bytes c = good;
      c.insert(c.end(), {0x00, 0x00});
      c[3] += 2;
      StunRefused(c, kTid, __LINE__);
    }
    CHECK(!rules::StunMapped(S(good), kTid, nullptr));
  }
}

// =====================================================================================================
// NAT class and its texts
// =====================================================================================================

static void TestNat() {
  Group("NAT class from the mapped endpoints of the STUN servers that answered");
  {
    const rules::Endpoint a = Ep(203, 0, 113, 7, 51234);
    const rules::Endpoint other_port = Ep(203, 0, 113, 7, 51235);
    const rules::Endpoint other_ip = Ep(203, 0, 113, 8, 51234);
    auto cls = [](const std::vector<rules::Endpoint>& v) { return rules::ClassifyNat(std::span<const rules::Endpoint>(v)); };
    CHECK(cls({}) == rules::Nat::kNoAnswer);
    CHECK(cls({a}) == rules::Nat::kOneAnswer);
    CHECK(cls({a, a}) == rules::Nat::kCone);
    CHECK(cls({a, other_port}) == rules::Nat::kSymmetric);
    CHECK(cls({a, other_ip}) == rules::Nat::kSymmetric);
    CHECK(cls({other_port, a}) == rules::Nat::kSymmetric);
    // three or more: any one that differs decides, wherever it stands
    CHECK(cls({a, a, a}) == rules::Nat::kCone);
    CHECK(cls({a, a, a, a}) == rules::Nat::kCone);
    CHECK(cls({a, a, other_port}) == rules::Nat::kSymmetric);
    CHECK(cls({a, other_port, a}) == rules::Nat::kSymmetric);
    CHECK(cls({other_port, a, a}) == rules::Nat::kSymmetric);
    CHECK(cls({a, a, other_ip}) == rules::Nat::kSymmetric);
    CHECK(cls({other_ip, a, a}) == rules::Nat::kSymmetric);
    CHECK(cls({a, other_port, other_ip}) == rules::Nat::kSymmetric);
    CHECK(cls({a, a, a, other_port}) == rules::Nat::kSymmetric);
  }

  Group("NAT notes and the connect-timeout text, exactly");
  CHECK(std::string(rules::NatNote(rules::Nat::kNoAnswer)) == "Local network only (no answer from the STUN servers)");
  CHECK(std::string(rules::NatNote(rules::Nat::kOneAnswer)) == "Public address found (one STUN server answered)");
  CHECK(std::string(rules::NatNote(rules::Nat::kCone)) == "Public address found");
  CHECK(std::string(rules::NatNote(rules::Nat::kSymmetric)) ==
        "Your network changes ports per destination (symmetric NAT): players outside it may not reach you");
  {
    // the brief's sentence, then the ask to check the code (fix round 1), then the symmetric-NAT note when it applies
    const std::string base = "A direct connection is not possible between your networks; relay arrives with lobbies."
                             " Also check the code you entered (capital letters matter).";
    CHECK(rules::ConnectTimeoutText(rules::Nat::kCone) == base);
    CHECK(rules::ConnectTimeoutText(rules::Nat::kSymmetric) == base + " (Your network changes ports per destination.)");
    CHECK(rules::ConnectTimeoutText(rules::Nat::kNoAnswer) == base);   // the suffix is for a symmetric NAT only
    CHECK(rules::ConnectTimeoutText(rules::Nat::kOneAnswer) == base);
  }
}

// =====================================================================================================
// Input delay and pacing
// =====================================================================================================

static void TestDelayPacer() {
  Group("DelayFromRtt: retail floor((rtt*2997 + 199999) / 100000), 0..33 ms -> 2, no measurement -> 10, clamped 1..8");
  CHECK(rules::DelayFromRtt(0) == 2);    // the <=33 branch: the formula alone gives 1 here (199,999)
  CHECK(rules::DelayFromRtt(1) == 2);
  CHECK(rules::DelayFromRtt(33) == 2);   // 298,900 - the formula gives 2 too, so the 33 edge itself cannot be pinned
  CHECK(rules::DelayFromRtt(34) == 3);   // 301,897 / 100,000
  CHECK(rules::DelayFromRtt(100) == 4);  // 499,699
  CHECK(rules::DelayFromRtt(101) == 5);  // 502,696
  CHECK(rules::DelayFromRtt(200) == 7);  // 799,399
  CHECK(rules::DelayFromRtt(201) == 8);  // 802,396
  CHECK(rules::DelayFromRtt(266) == 8);  // retail: >= 266 ms -> 10 (doa4_own_online.md:81); the formula gives 9 here, 10 from 267; clamped
  CHECK(rules::DelayFromRtt(-1) == 8);   // no measurement: retail's "no QoS -> 10", clamped (F20c; Start needs 4 samples)

  Group("DelayFromRtt: every step edge of the formula, both sides (from the model script)");
  // the formula steps at rtt 67, 101, 134, 167, 201 (and 234, 267, ... above the clamp); 34 is the first step
  // above the <=33 branch's 2
  const std::pair<int, int> edges[] = {{33, 2}, {34, 3}, {66, 3}, {67, 4}, {100, 4}, {101, 5}, {133, 5}, {134, 6},
                                       {166, 6}, {167, 7}, {200, 7}, {201, 8}, {233, 8}, {234, 8}, {266, 8}, {267, 8},
                                       {300, 8}, {301, 8}, {1000, 8}, {100000, 8}, {INT_MAX, 8}};
  for (const auto& [rtt, want] : edges) CHECK(rules::DelayFromRtt(rtt) == want);
  for (int rtt : {-2, -100, INT_MIN}) CHECK(rules::DelayFromRtt(rtt) == 8);  // any negative is "no measurement"
  {  // a sweep against the formula written out in this file, over every plausible round trip
    bool ok = true;
    int prev = 0;
    for (int rtt = 0; rtt <= 3000; ++rtt) {
      const long long raw = rtt <= 33 ? 2 : (rtt * 2997LL + 199999LL) / 100000LL;
      const int want = int(std::min(8LL, std::max(1LL, raw)));
      const int got = rules::DelayFromRtt(rtt);
      ok = ok && got == want && got >= prev && got >= 1 && got <= 8;
      prev = got;
    }
    CHECK(ok);
  }

  Group("PacerExtraUs: GekkoNet's frames_ahead as given (already halved), spread over 60 frames, at most 1000 us");
  CHECK(rules::PacerExtraUs(0.99f) == 0);   CHECK(rules::PacerExtraUs(1.0f) == 278);   // 277.78
  CHECK(rules::PacerExtraUs(2.0f) == 556);  CHECK(rules::PacerExtraUs(3.0f) == 833);   // 555.57, 833.35
  CHECK(rules::PacerExtraUs(8.0f) == 1000); CHECK(rules::PacerExtraUs(-3.0f) == 0);    // 8.0 -> 2,222.27 uncapped
  CHECK(rules::PacerExtraUs(1.5f) == 417);    // 416.675: rounded, not cut
  CHECK(rules::PacerExtraUs(3.5f) == 972);    // 972.24, just under the cap
  CHECK(rules::PacerExtraUs(3.59f) == 997);   // 997.24
  CHECK(rules::PacerExtraUs(1.125f) == 313);  // 312.50625: the rounding of 16667 (16666 gives 312.49 -> 312)
  CHECK(rules::PacerExtraUs(3.375f) == 938);  // 937.51875 (16666 gives 937.46 -> 937)
  CHECK(rules::PacerExtraUs(2.5f) == 694);    // 694.458 (16668 gives 694.5 -> 695)
  CHECK(rules::PacerExtraUs(3.6f) == 1000);   // 1000.02: the first value that reaches the cap
  CHECK(rules::PacerExtraUs(3.7f) == 1000);   // 1,027.80 uncapped
  CHECK(rules::PacerExtraUs(7.2f) == 1000);   // 2,000.04 uncapped
  CHECK(rules::PacerExtraUs(100.0f) == 1000);
  CHECK(rules::PacerExtraUs(0.0f) == 0);      CHECK(rules::PacerExtraUs(-0.0f) == 0);
  CHECK(rules::PacerExtraUs(0.5f) == 0);      CHECK(rules::PacerExtraUs(0.999999f) == 0);
  CHECK(rules::PacerExtraUs(std::numeric_limits<float>::infinity()) == 1000);
  CHECK(rules::PacerExtraUs(std::numeric_limits<float>::max()) == 1000);
  CHECK(rules::PacerExtraUs(-std::numeric_limits<float>::infinity()) == 0);
  CHECK(rules::PacerExtraUs(std::numeric_limits<float>::quiet_NaN()) == 0);
}

// =====================================================================================================
// Release channel: versions, the pick from GitHub's release list, advice
// =====================================================================================================

static void TestVersions() {
  Group("release: version parsing - the three spellings, and what is not a version");
  std::array<int, 3> v{};
  CHECK(rules::ParseVersion("netplay-v1.0.14", &v) && v == std::array<int, 3>{1, 0, 14});
  CHECK(rules::ParseVersion("v2.3.4", &v) && v == std::array<int, 3>{2, 3, 4});
  CHECK(rules::ParseVersion("1.0.0", &v) && v == std::array<int, 3>{1, 0, 0});
  CHECK(rules::ParseVersion("0.0.0", &v) && v == std::array<int, 3>{0, 0, 0});
  CHECK(rules::ParseVersion("netplay-v7.8.9", nullptr));  // validity alone
  CHECK(rules::ParseVersion("my-channel-v3.4.5", &v) && v == std::array<int, 3>{3, 4, 5});  // the version follows the LAST dash
  {
    const std::array<int, 3> keep = {7, 7, 7};
    for (std::string_view bad : {"netplay-nightly"sv, ""sv, "v1.0"sv, "dev"sv, "v1.0.0.1"sv, "v1..0"sv, "v1.0."sv, "1.0.0 "sv,
                                 " 1.0.0"sv, "netplay-v1.0.14-"sv, "netplay-v"sv, "v"sv, "netplay-1.0.x"sv, "vv1.0.0"sv,
                                 "netplay-v1.0.14-rc1"sv, "1.0.0\n"sv}) {
      v = keep;
      CHECK(!rules::ParseVersion(bad, &v));
      CHECK(v == keep);  // a text that is not a version leaves *out alone
    }
  }
  // the number limit: 9 digits are accepted, 10 are not (both sides pinned)
  CHECK(rules::ParseVersion("1.0.999999999", &v) && v[2] == 999999999);
  CHECK(rules::ParseVersion("999999999.999999999.999999999", &v) && v[0] == 999999999 && v[1] == 999999999);
  CHECK(rules::ParseVersion("100000000.0.0", &v) && v[0] == 100000000);
  CHECK(!rules::ParseVersion("1.0.1000000000", &v));
  CHECK(!rules::ParseVersion("1000000000.0.0", &v));
  CHECK(!rules::ParseVersion("1.0.99999999999999999999", &v));

  Group("release: order is numeric, not textual; every component decides when the ones before are equal");
  CHECK(rules::CompareVersions("netplay-v1.0.9", "netplay-v1.0.10") < 0);
  CHECK(rules::CompareVersions("netplay-v1.0.10", "netplay-v1.0.9") > 0);
  CHECK(rules::CompareVersions("v2.0.0", "v1.9.99") > 0);
  CHECK(rules::CompareVersions("v1.9.99", "v2.0.0") < 0);
  CHECK(rules::CompareVersions("v1.2.0", "v1.1.99") > 0);
  CHECK(rules::CompareVersions("v1.1.99", "v1.2.0") < 0);
  CHECK(rules::CompareVersions("v1.2.3", "v1.2.4") < 0);
  CHECK(rules::CompareVersions("v1.2.3", "v1.3.0") < 0);
  CHECK(rules::CompareVersions("v1.2.3", "v2.0.0") < 0);
  CHECK(rules::CompareVersions("v1.2.9", "v2.0.1") < 0);   // the last number alone would say >
  CHECK(rules::CompareVersions("netplay-v1.0.3", "v1.0.3") == 0);
  CHECK(rules::CompareVersions("1.2.3", "netplay-v1.2.3") == 0);
  CHECK(rules::CompareVersions("garbage", "1.0.0") < 0);   // an unparsable tag sorts lowest
  CHECK(rules::CompareVersions("1.0.0", "garbage") > 0);
  CHECK(rules::CompareVersions("", "0.0.0") < 0);          // even below 0.0.0
  CHECK(rules::CompareVersions("0.0.0", "dev") > 0);
  CHECK(rules::CompareVersions("garbage", "dev") == 0);    // two unparsable tags are equal
  CHECK(rules::CompareVersions("", "") == 0);

  Group("release: a dev build is an unparsable tag (\"dev\") or build number 0");
  CHECK(rules::IsDevBuild("netplay-v1.0.0"));
  CHECK(rules::IsDevBuild("v2.7.0"));
  CHECK(rules::IsDevBuild("0.0.0"));
  CHECK(!rules::IsDevBuild("netplay-v1.0.4"));
  CHECK(!rules::IsDevBuild("netplay-v1.0.10"));   // ends in 0, but the number is 10
  CHECK(!rules::IsDevBuild("netplay-v1.0.100"));
  CHECK(!rules::IsDevBuild("netplay-v0.0.1"));
  CHECK(!rules::IsDevBuild("netplay-v1.2.3"));
  CHECK(rules::IsDevBuild("dev"));                // DOA4_RELEASE_TAG of a local build
  CHECK(rules::IsDevBuild(""));                   // unparsable
  CHECK(rules::IsDevBuild("netplay-nightly"));
}

// A release as GitHub lists it; the builder places the members in the order a test asks for.
struct Rel { std::string tag; bool draft = false; std::vector<std::string> assets; std::string body = ""; };  // body: JSON text, escapes included
static std::string RelUrl(const std::string& tag, const std::string& name) {
  return "https://github.com/o/r/releases/download/" + tag + "/" + name;
}
static std::string Json(const std::vector<Rel>& rels, bool draft_after_assets, const char* ws = "\n ") {
  std::string out = "[";
  int n = 0;
  for (const Rel& r : rels) {
    std::string assets = "\"assets\":[";
    for (size_t i = 0; i < r.assets.size(); ++i) {
      if (i) assets += ",";
      assets += "{\"name\":\"" + r.assets[i] + "\",\"uploader\":{\"login\":\"y\"},\"browser_download_url\":\"" + RelUrl(r.tag, r.assets[i]) + "\"}";
    }
    assets += "]";
    const std::string draft = std::string("\"draft\":") + (r.draft ? "true" : "false");
    const std::string head = "\"url\":\"https://api.github.com/repos/o/r/releases/" + std::to_string(++n) + "\",\"tag_name\":\"" + r.tag + "\"";
    const std::string body = r.body.empty() ? "fixes {a} [b] and \\\"quoted\\\" text, a brace } and a bracket ]" : r.body;
    const std::string tail = "\"prerelease\":true,\"author\":{\"login\":\"x\",\"id\":1},\"body\":\"" + body + "\"";
    if (n > 1) out += ",";
    out += ws;
    out += "{" + head + "," + (draft_after_assets ? assets + "," + draft : draft + "," + assets) + "," + tail + "}";
  }
  return out + "\n]\n";
}

// The realistic tier: shaped like GET /repos/<owner>/<repo>/releases (nested author / uploader objects, a body with
// braces, brackets and escaped quotes). In order: 1. a draft netplay-v1.0.21; 2. v1.0.20 of the other channel;
// 3. netplay-nightly (not a version); 4. netplay-v1.0.19 with no SHA256SUMS.txt; 5. netplay-v1.0.18 with every
// required file and more <- the one to pick; 6. netplay-v1.0.17, older, complete.
#define ASSET(tag, name) "{\"name\":\"" name "\",\"uploader\":{\"login\":\"y\"},\"browser_download_url\":\"https://github.com/o/r/releases/download/" tag "/" name "\"}"
#define FULL(tag) ASSET(tag, "dead_or_alive_4.exe") "," ASSET(tag, "rexruntime.dll") "," ASSET(tag, "TracyClient.dll") "," ASSET(tag, "SHA256SUMS.txt")
static const char* const kList =
    "[\n"
    " {\"url\":\"https://api.github.com/repos/o/r/releases/6\",\"tag_name\":\"netplay-v1.0.21\",\"draft\":true,"
    "  \"prerelease\":true,\"author\":{\"login\":\"x\",\"id\":1},\"assets\":[" FULL("netplay-v1.0.21") "],\"body\":\"draft\"},\n"
    " {\"url\":\"https://api.github.com/repos/o/r/releases/5\",\"tag_name\":\"v1.0.20\",\"draft\":false,"
    "  \"prerelease\":false,\"author\":{\"login\":\"x\",\"id\":1},\"assets\":[" FULL("v1.0.20") "],\"body\":\"master\"},\n"
    " {\"url\":\"https://api.github.com/repos/o/r/releases/4\",\"tag_name\":\"netplay-nightly\",\"draft\":false,"
    "  \"prerelease\":true,\"assets\":[" FULL("netplay-nightly") "],\"body\":\"\"},\n"
    " {\"url\":\"https://api.github.com/repos/o/r/releases/3\",\"tag_name\":\"netplay-v1.0.19\",\"draft\":false,"
    "  \"prerelease\":true,\"author\":{\"login\":\"x\",\"id\":1},\"assets\":["
    ASSET("netplay-v1.0.19", "dead_or_alive_4.exe") "," ASSET("netplay-v1.0.19", "rexruntime.dll") "," ASSET("netplay-v1.0.19", "TracyClient.dll") ","
    ASSET("netplay-v1.0.19", "notes.txt") "],"
    "  \"zipball_url\":\"https://api.github.com/repos/o/r/zipball/netplay-v1.0.19\","
    "  \"body\":\"fixes {a} [b] and \\\"quoted\\\" text, a brace } and a bracket ]\"},\n"
    " {\"url\":\"https://api.github.com/repos/o/r/releases/2\",\"tag_name\":\"netplay-v1.0.18\",\"draft\":false,"
    "  \"prerelease\":true,\"author\":{\"login\":\"x\",\"id\":1},\"assets\":["
    ASSET("netplay-v1.0.18", "notes.txt") "," ASSET("netplay-v1.0.18", "DOA4-netplay-v1.0.18-win64.zip") "," FULL("netplay-v1.0.18") ","
    ASSET("netplay-v1.0.18", "doa4-launcher.zip") "],\"body\":\"\"},\n"
    " {\"url\":\"https://api.github.com/repos/o/r/releases/1\",\"tag_name\":\"netplay-v1.0.17\",\"draft\":false,"
    "  \"prerelease\":true,\"assets\":[" FULL("netplay-v1.0.17") "],\"body\":\"\"}\n"
    "]\n";

static const std::array<std::string_view, 4> kRequiredArr = {"dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll", "SHA256SUMS.txt"};
static std::span<const std::string_view> Required() { return std::span<const std::string_view>(kRequiredArr); }
static const std::vector<std::string> kFull = {"dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll", "SHA256SUMS.txt"};

// A lookup that cannot throw: a pick that lacks an asset is a failed check, not a crash that hides the checks after it.
static std::string UrlOf(const rules::ReleasePick& p, const std::string& name) {
  const auto it = p.asset_urls.find(name);
  return it == p.asset_urls.end() ? std::string("<absent>") : it->second;
}

static bool PickIs(const rules::ReleasePick& p, const std::string& tag) {  // the tag, and every required file's own URL
  if (p.tag != tag || p.asset_urls.size() != kRequiredArr.size()) return false;
  for (std::string_view n : kRequiredArr) {
    auto it = p.asset_urls.find(std::string(n));
    if (it == p.asset_urls.end() || it->second != RelUrl(tag, std::string(n))) return false;
  }
  return true;
}

static void TestPickRelease() {
  Group("PickRelease: the realistic list - the highest complete netplay release, its own tag with its own files");
  {
    rules::ReleasePick p;
    CHECK(rules::PickRelease(kList, "netplay", Required(), &p));
    CHECK(PickIs(p, "netplay-v1.0.18"));
    CHECK(p.asset_urls.count("notes.txt") == 0 && p.asset_urls.count("doa4-launcher.zip") == 0);  // only what was asked for
    CHECK(UrlOf(p, "SHA256SUMS.txt") == "https://github.com/o/r/releases/download/netplay-v1.0.18/SHA256SUMS.txt");
    // fewer required files: the missing-asset release (1.0.19 lacks SHA256SUMS.txt) now qualifies, and is higher
    rules::ReleasePick p2;
    const std::array<std::string_view, 3> three = {"dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll"};
    CHECK(rules::PickRelease(kList, "netplay", std::span<const std::string_view>(three), &p2));
    CHECK(p2.tag == "netplay-v1.0.19" && p2.asset_urls.size() == 3 && p2.asset_urls.count("SHA256SUMS.txt") == 0);
    // the other channel: its own releases
    rules::ReleasePick p3;
    CHECK(rules::PickRelease(kList, "v1.0", Required(), &p3) == false);  // "v1.0-v" is no prefix of any tag
    rules::ReleasePick p4;
    CHECK(rules::PickRelease(kList, "netplay-nightly", Required(), &p4) == false);
    // a channel that is a prefix of another channel's name is not that channel
    CHECK(!rules::PickRelease(kList, "net", Required(), &p4));
    CHECK(!rules::PickRelease(kList, "netplay-v1", Required(), &p4));
    CHECK(!rules::PickRelease(kList, "Netplay", Required(), &p4));  // channel names are exact
  }

  Group("PickRelease: a release list is in no documented order - the highest version wins in any permutation");
  {
    const Rel a{"netplay-v1.0.9", false, kFull}, b{"netplay-v1.0.10", false, kFull}, c{"netplay-v1.0.4", false, kFull};
    const Rel draft{"netplay-v1.0.11", true, kFull};                                 // (b) a draft, higher than the winner
    const Rel lacking{"netplay-v1.0.12", false, {"dead_or_alive_4.exe", "rexruntime.dll", "SHA256SUMS.txt"}};  // (c) no TracyClient.dll
    const Rel other_channel{"v1.0.20", false, kFull};                                // (d) another channel, higher
    const Rel nightly{"netplay-nightly", false, kFull};
    const Rel near_name{"netplay-v1.0.14", false, {"dead_or_alive_4.exe.zip", "rexruntime.dll", "TracyClient.dll", "SHA256SUMS.txt"}};
    const Rel near_name2{"netplay-v1.0.15", false, {"my_dead_or_alive_4.exe", "rexruntime.dll", "TracyClient.dll", "SHA256SUMS.txt"}};
    const Rel near_case{"netplay-v1.0.16", false, {"Dead_Or_Alive_4.exe", "rexruntime.dll", "TracyClient.dll", "SHA256SUMS.txt"}};
    const Rel wrong_case_channel{"Netplay-v1.0.17", false, kFull};
    const Rel two_dots{"netplay-v1.0.18.1", false, kFull};
    std::vector<Rel> all = {a, b, c, draft, lacking, other_channel, nightly, near_name, near_name2, near_case, wrong_case_channel, two_dots};
    // a draft that also lacks nothing; a draft listed with its members the other way round
    int ok = 0, runs = 0;
    Rng rng(0x5EED0003ull);
    for (int iter = 0; iter < 300; ++iter) {
      if (iter == 1) std::reverse(all.begin(), all.end());
      else if (iter > 1)
        for (size_t i = all.size() - 1; i > 0; --i) std::swap(all[i], all[rng.Below(i + 1)]);
      for (bool after : {false, true}) {
        ++runs;
        rules::ReleasePick p;
        if (rules::PickRelease(Json(all, after), "netplay", Required(), &p) && PickIs(p, "netplay-v1.0.10")) ++ok;
      }
    }
    CHECK(runs == 600 && ok == runs);
    // each rival alone beats nothing: with only the rivals in the list, there is no pick
    const std::vector<Rel> rivals = {draft, lacking, other_channel, nightly, near_name, near_name2, near_case, wrong_case_channel, two_dots};
    rules::ReleasePick p;
    CHECK(!rules::PickRelease(Json(rivals, false), "netplay", Required(), &p));
    CHECK(!rules::PickRelease(Json(rivals, true), "netplay", Required(), &p));
    // each rival's own rule, one at a time, against a lower complete release (so a pick exists only if it is skipped)
    for (const Rel& rival : rivals) {
      rules::ReleasePick q;
      CHECK(rules::PickRelease(Json({rival, c}, false), "netplay", Required(), &q) && PickIs(q, "netplay-v1.0.4"));
      CHECK(rules::PickRelease(Json({c, rival}, true), "netplay", Required(), &q) && PickIs(q, "netplay-v1.0.4"));
    }
    // numeric order, with the major number dominating: 2.0.0 beats 1.9.99, 1.0.10 beats 1.0.9
    CHECK(rules::PickRelease(Json({{"netplay-v1.9.99", false, kFull}, {"netplay-v2.0.0", false, kFull}, {"netplay-v1.10.0", false, kFull}}, false), "netplay", Required(), &p) && p.tag == "netplay-v2.0.0");
    // a draft alone is not offered, and a draft of a higher version does not hide a published one
    CHECK(!rules::PickRelease(Json({draft}, false), "netplay", Required(), &p));
    CHECK(rules::PickRelease(Json({draft, c}, false), "netplay", Required(), &p) && p.tag == "netplay-v1.0.4");
  }

  Group("PickRelease: not a release list, or a damaged one - false, *out untouched, nothing read past the end");
  {
    rules::ReleasePick sentinel;
    sentinel.tag = "sentinel";
    sentinel.asset_urls["k"] = "v";
    auto refused = [&](std::string_view json) {
      rules::ReleasePick p = sentinel;
      const bool r = rules::PickRelease(json, "netplay", Required(), &p);
      return !r && p.tag == "sentinel" && p.asset_urls.size() == 1 && UrlOf(p, "k") == "v";
    };
    CHECK(refused("{\"message\":\"API rate limit exceeded\"}"));    // (e) an error object
    CHECK(refused("{\"message\":\"Not Found\",\"documentation_url\":\"https://docs.github.com\"}"));
    CHECK(refused("[]"));
    CHECK(refused(""));
    CHECK(refused("  \n"));
    CHECK(refused("null"));
    CHECK(refused("[1,2,3]"));
    CHECK(refused("[{}]"));
    CHECK(refused("[{\"tag_name\":\"netplay-v1.0.1\"}]"));                 // no assets member
    CHECK(refused("[{\"assets\":[],\"tag_name\":\"netplay-v1.0.1\"}]"));   // no assets in it
    CHECK(refused("[{\"tag_name\":5,\"assets\":[]}]"));
    CHECK(refused("[{"));
    CHECK(refused("[{\"tag_name\":\"netplay-v1.0.1\",\"assets\":[{\"name\":\"a\""));
    CHECK(refused(std::string(200000, '[')));                               // nesting does not recurse
    CHECK(refused("[" + std::string(200000, '{')));
    CHECK(refused("[{\"tag_name\":\"" + std::string(100000, 'x')));        // a string that never ends
    {  // every proper prefix of the realistic list is a cut transfer: nothing is picked from it
      const std::string full = kList;
      const size_t last_bracket = full.rfind(']');
      bool all_refused = true;
      for (size_t n = 0; n <= last_bracket; ++n) all_refused = all_refused && refused(std::string_view(full.data(), n));
      CHECK(all_refused);
      rules::ReleasePick p;
      CHECK(rules::PickRelease(std::string_view(full.data(), last_bracket + 1), "netplay", Required(), &p) && p.tag == "netplay-v1.0.18");
    }
    {  // damaged copies of the list: no crash (the hardened library fast-fails an out-of-range read); a pick is a real tag
      const std::string full = kList;
      Rng rng(0xF00D0003ull);
      int picked = 0;
      for (int i = 0; i < 4000; ++i) {
        std::string d = full;
        const int hits = 1 + int(rng.Below(3));
        for (int k = 0; k < hits; ++k) d[rng.Below(d.size())] = "{}[]\",:\\ x"[rng.Below(10)];
        rules::ReleasePick p;
        if (rules::PickRelease(d, "netplay", Required(), &p)) {
          ++picked;
          if (p.tag.rfind("netplay-v", 0) != 0 || p.asset_urls.size() != 4) { CHECK(false); break; }
        }
      }
      CHECK(picked > 0);  // the loop reached the success path too (not every damage is fatal)
    }
    rules::ReleasePick p;
    CHECK(!rules::PickRelease(kList, "", Required(), &p));
    CHECK(!rules::PickRelease(kList, "netplay", Required(), nullptr));
    CHECK(rules::PickRelease("\n  \t[" + std::string(kList).substr(1), "netplay", Required(), &p) && p.tag == "netplay-v1.0.18");  // leading blanks
  }

  Group("PickRelease: escapes make an asset unusable (fail closed), a tag or url with a quote is read whole");
  {
    // the highest release has a URL with an escape sequence: it is skipped, the next one is picked
    std::string j = Json({{"netplay-v1.0.30", false, kFull}, {"netplay-v1.0.2", false, kFull}}, false);
    const size_t at = j.find("netplay-v1.0.30/SHA256SUMS.txt");
    j.replace(at, 14, "netplay-v1.0.\\u0033");  // ...v1.0.3 ... in the URL
    rules::ReleasePick p;
    CHECK(at != std::string::npos && rules::PickRelease(j, "netplay", Required(), &p) && p.tag == "netplay-v1.0.2");
    // a release whose body holds the words tag_name and draft does not confuse the reader (members are top-level only)
    std::string k = Json({{"netplay-v1.0.3", false, kFull}}, false);
    const size_t body = k.find("\"body\":\"");
    k.insert(body + 8, "\\\"tag_name\\\":\\\"netplay-v9.9.9\\\" \\\"draft\\\":true ");
    CHECK(rules::PickRelease(k, "netplay", Required(), &p) && p.tag == "netplay-v1.0.3");
  }

  Group("PickRelease: release bodies with escapes, a release whose assets is not a list, an empty channel");
  {
    // JSON strings whose escapes sit next to the brackets the reader counts: an ODD number of escaped quotes before a
    // brace (a reader that ends the string at an escaped quote then sees the brace), and a string that ends in an
    // escaped backslash (a reader that skips a backslash only before a quote then never sees the closing quote)
    const std::string bodies[] = {
        "one \\\" quote then } and ] after",
        "one \\\" quote then { and [ after",
        "ends with a backslash \\\\",
        "\\\\\\\" a backslash and a quote, then }",
        "\\\"",
        "\\\\",
    };
    const Rel low{"netplay-v1.0.4", false, kFull};
    for (const std::string& body : bodies) {
      const Rel high{"netplay-v1.0.50", false, kFull, body};
      for (bool after : {false, true}) {
        rules::ReleasePick p;
        CHECK(rules::PickRelease(Json({high, low}, after), "netplay", Required(), &p) && PickIs(p, "netplay-v1.0.50"));
        CHECK(rules::PickRelease(Json({low, high}, after), "netplay", Required(), &p) && PickIs(p, "netplay-v1.0.50"));
        // the same body on a release that must NOT be picked (a draft): it still must not hide the next one
        const Rel hidden_draft{"netplay-v1.0.60", true, kFull, body};
        CHECK(rules::PickRelease(Json({hidden_draft, low}, after), "netplay", Required(), &p) && PickIs(p, "netplay-v1.0.4"));
      }
    }
    // assets given as an object holding the four files: not a list of assets, so the release is skipped
    const std::string not_a_list =
        R"({"tag_name":"netplay-v1.0.40","draft":false,"assets":{"a":{"name":"dead_or_alive_4.exe","browser_download_url":"u1"},)"
        R"("b":{"name":"rexruntime.dll","browser_download_url":"u2"},"c":{"name":"TracyClient.dll","browser_download_url":"u3"},)"
        R"("d":{"name":"SHA256SUMS.txt","browser_download_url":"u4"}}})";
    const std::string with_good = "[" + not_a_list + "," + Json({low}, false).substr(1);
    rules::ReleasePick p;
    CHECK(rules::PickRelease(with_good, "netplay", Required(), &p) && PickIs(p, "netplay-v1.0.4"));
    // an asset without a download URL, or without a name, is not an asset
    const std::string no_url =
        R"([{"tag_name":"netplay-v1.0.41","draft":false,"assets":[{"name":"dead_or_alive_4.exe"},)"
        R"({"name":"rexruntime.dll","browser_download_url":"u2"},{"name":"TracyClient.dll","browser_download_url":"u3"},)"
        R"({"name":"SHA256SUMS.txt","browser_download_url":"u4"}]}])";
    CHECK(!rules::PickRelease(no_url, "netplay", Required(), &p));
    const std::string no_name =
        R"([{"tag_name":"netplay-v1.0.41","draft":false,"assets":[{"browser_download_url":"u1"},)"
        R"({"name":"rexruntime.dll","browser_download_url":"u2"},{"name":"TracyClient.dll","browser_download_url":"u3"},)"
        R"({"name":"SHA256SUMS.txt","browser_download_url":"u4"}]}])";
    const std::array<std::string_view, 3> three = {"rexruntime.dll", "TracyClient.dll", "SHA256SUMS.txt"};
    CHECK(!rules::PickRelease(no_name, "netplay", Required(), &p));
    CHECK(rules::PickRelease(no_name, "netplay", std::span<const std::string_view>(three), &p) && p.tag == "netplay-v1.0.41" && p.asset_urls.size() == 3);
    // an empty channel names no release, even where "-v1.0.1" is a tag that parses
    CHECK(rules::ParseVersion("-v1.0.1", nullptr));
    CHECK(!rules::PickRelease(Json({{"-v1.0.1", false, kFull}}, false), "", Required(), &p));
  }

  Group("PickRelease: required list edge cases");
  {
    rules::ReleasePick p;
    const std::array<std::string_view, 1> one = {"SHA256SUMS.txt"};
    CHECK(rules::PickRelease(kList, "netplay", std::span<const std::string_view>(one), &p) && p.tag == "netplay-v1.0.18" && p.asset_urls.size() == 1);
    const std::array<std::string_view, 2> missing = {"dead_or_alive_4.exe", "nothing-like-it.bin"};
    CHECK(!rules::PickRelease(kList, "netplay", std::span<const std::string_view>(missing), &p));
    const std::array<std::string_view, 1> exact = {"dead_or_alive_4.ex"};  // a prefix of a name is not the name
    CHECK(!rules::PickRelease(kList, "netplay", std::span<const std::string_view>(exact), &p));
    const std::array<std::string_view, 1> longer = {"dead_or_alive_4.exe2"};
    CHECK(!rules::PickRelease(kList, "netplay", std::span<const std::string_view>(longer), &p));
  }
}

static void TestAdvice() {
  Group("MismatchAdvice: who is older, who updates, where");
  const std::string older_me = rules::MismatchAdvice("netplay-v1.0.12", "netplay-v1.0.14");
  const std::string older_them = rules::MismatchAdvice("netplay-v1.0.14", "netplay-v1.0.12");
  const std::string self_me = rules::MismatchAdvice("netplay-v1.0.0", "netplay-v1.0.14");
  const std::string self_them = rules::MismatchAdvice("netplay-v1.0.14", "netplay-v1.0.0");
  const std::string no_tag = rules::MismatchAdvice("netplay-v1.0.14", "");
  const std::string same = rules::MismatchAdvice("netplay-v1.0.14", "netplay-v1.0.14");
  CHECK(Has(older_me, "You have an older release") && Has(older_me, "v1.0.14") && Has(older_me, "v1.0.12") && Has(older_me, "Check for Updates"));
  CHECK(Has(older_them, "The other player has an older release") && Has(older_them, "v1.0.12") && Has(older_them, "Check for Updates"));
  CHECK(!Has(older_me, "The other player has an older") && !Has(older_them, "You have an older"));
  CHECK(!Has(older_me, "netplay-v1.0.14") && !Has(older_them, "netplay-v1.0.12"));  // the tag is shown without its channel
  CHECK(Has(self_me, "You run a self-built exe"));
  CHECK(Has(self_them, "The other player runs a self-built exe"));
  // a build from before versions has an updater that cannot fetch netplay releases: the Releases page, never Check for Updates
  CHECK(Has(no_tag, "older game") && Has(no_tag, "Releases page") && !Has(no_tag, "Check for Updates"));
  // the same version with different builds: the updater refuses an equal tag, so the page again
  CHECK(Has(same, "same version") && Has(same, "Releases page") && Has(same, "v1.0.14"));

  Group("MismatchAdvice: a local build is \"dev\" (DOA4_RELEASE_TAG), which is not a version");
  CHECK(Has(rules::MismatchAdvice("dev", "netplay-v1.0.5"), "You run a self-built exe"));
  CHECK(Has(rules::MismatchAdvice("netplay-v1.0.5", "dev"), "The other player runs a self-built exe"));
  CHECK(Has(rules::MismatchAdvice("dev", "dev"), "You run a self-built exe"));  // both: the reader's own game first
  CHECK(Has(rules::MismatchAdvice("dev", ""), "older game"));  // no tag at all: the other game is the old one
  CHECK(!Has(rules::MismatchAdvice("netplay-v1.0.5", "dev"), "older game"));
  {  // every case reads differently from every other one
    const std::vector<std::string> all = {older_me, older_them, self_me, self_them, no_tag, same};
    bool distinct = true;
    for (size_t i = 0; i < all.size(); ++i)
      for (size_t j = i + 1; j < all.size(); ++j) distinct = distinct && all[i] != all[j];
    CHECK(distinct);
    for (const std::string& s : all) CHECK(!s.empty() && s.size() < 400);
  }
}

// =====================================================================================================
// Handshake verdict
// =====================================================================================================

static rules::Identity Id(uint64_t build, const char* release, const char* files) {
  rules::Identity i;
  i.build = build;
  i.release = release;
  i.files = files;
  return i;
}

static void TestJudge() {
  using R = wire::Refusal;
  const char* kFiles = "snd.afs=0123456789abcdef\nstg.afs=fedcba9876543210\n";
  const rules::Identity host = Id(0x1111222233334444ull, "netplay-v1.0.5", kFiles);

  Group("Judge: equal identities agree");
  {
    const rules::Verdict v = rules::Judge(host, host, false);
    CHECK(v.refusal == R::kNone && v.text.empty());
    // the release tag is only for advice: the same build and files under another tag is the same game
    const rules::Verdict w = rules::Judge(host, Id(host.build, "dev", kFiles), false);
    CHECK(w.refusal == R::kNone);
  }

  Group("Judge: builds differ -> kBuild with the advice of the two release tags (host first)");
  {
    const rules::Identity joiner = Id(host.build + 1, "netplay-v1.0.6", kFiles);
    const rules::Verdict v = rules::Judge(host, joiner, false);
    CHECK(v.refusal == R::kBuild);
    CHECK(Has(v.text, rules::MismatchAdvice(host.release, joiner.release)));
    CHECK(!Has(v.text, rules::MismatchAdvice(joiner.release, host.release)));
    CHECK(Has(v.text, "You have an older release"));  // the host has the older of the two
    const rules::Verdict dev = rules::Judge(host, Id(host.build ^ 1, "dev", kFiles), false);
    CHECK(dev.refusal == R::kBuild && Has(dev.text, "self-built"));
    // one build bit differs, in each position of the word
    for (int bit = 0; bit < 64; bit += 9) CHECK(rules::Judge(host, Id(host.build ^ (1ull << bit), "netplay-v1.0.5", kFiles), false).refusal == R::kBuild);
  }

  Group("Judge: files differ -> kFiles naming the groups that differ (and only those)");
  {
    const rules::Identity joiner = Id(host.build, "netplay-v1.0.5", "snd.afs=0123456789abcdef\nstg.afs=0000000000000000\n");
    const rules::Verdict v = rules::Judge(host, joiner, false);
    CHECK(v.refusal == R::kFiles && Has(v.text, "stg.afs") && !Has(v.text, "snd.afs"));
    const rules::Verdict two = rules::Judge(host, Id(host.build, "netplay-v1.0.5", "snd.afs=1\nstg.afs=2\n"), false);
    CHECK(two.refusal == R::kFiles && Has(two.text, "stg.afs") && Has(two.text, "snd.afs"));
    const rules::Verdict missing = rules::Judge(host, Id(host.build, "netplay-v1.0.5", "snd.afs=0123456789abcdef\n"), false);
    CHECK(missing.refusal == R::kFiles && Has(missing.text, "stg.afs"));
  }

  Group("Judge: busy -> kBusy whatever else differs");
  {
    CHECK(rules::Judge(host, host, true).refusal == R::kBusy);
    CHECK(!rules::Judge(host, host, true).text.empty());
    CHECK(rules::Judge(host, Id(host.build + 1, "netplay-v1.0.6", kFiles), true).refusal == R::kBusy);
    CHECK(rules::Judge(host, Id(host.build, "netplay-v1.0.5", "snd.afs=9\n"), true).refusal == R::kBusy);
    CHECK(rules::Judge(host, Id(0, "netplay-v1.0.5", kFiles), true).refusal == R::kBusy);
    CHECK(rules::Judge(Id(0, "dev", ""), Id(0, "dev", ""), true).refusal == R::kBusy);
  }

  Group("Judge: the build check comes before the files check");
  {
    const rules::Verdict v = rules::Judge(host, Id(host.build + 1, "netplay-v1.0.6", "snd.afs=9\n"), false);
    CHECK(v.refusal == R::kBuild && !Has(v.text, "stg.afs"));
    CHECK(rules::Judge(host, Id(0, "dev", "snd.afs=9\n"), false).refusal == R::kBuild);
  }

  Group("Judge: an unreadable build (0) agrees with nothing, itself included");
  {
    for (const rules::Verdict& v : {rules::Judge(Id(0, "dev", kFiles), Id(0, "dev", kFiles), false),
                                    rules::Judge(Id(0, "netplay-v1.0.5", kFiles), Id(0, "netplay-v1.0.5", kFiles), false),
                                    rules::Judge(Id(0, "dev", kFiles), host, false),
                                    rules::Judge(host, Id(0, "dev", kFiles), false)}) {
      CHECK(v.refusal == R::kBuild &&
            Has(v.text, "One game could not read its own build - reinstall it from the newest release"));  // names a remedy
    }
    // a refusal for a differing build is not worded as an unreadable one
    CHECK(!Has(rules::Judge(host, Id(host.build + 1, "netplay-v1.0.6", kFiles), false).text, "could not read its own build"));
    CHECK(rules::Judge(Id(1, "dev", kFiles), Id(1, "dev", kFiles), false).refusal == R::kNone);  // build 1 is a build
    CHECK(rules::Judge(Id(~0ull, "dev", kFiles), Id(~0ull, "dev", kFiles), false).refusal == R::kNone);
  }

  Group("Judge: an unreadable file group agrees with nothing, itself included");
  {
    const rules::Identity a = Id(host.build, "netplay-v1.0.5", "snd.afs=0123456789abcdef\nstg.afs=unreadable\n");
    const rules::Verdict v = rules::Judge(a, a, false);  // the same text on both sides
    CHECK(v.refusal == R::kFiles && Has(v.text, "stg.afs") && !Has(v.text, "snd.afs"));
    CHECK(rules::Judge(a, host, false).refusal == R::kFiles);
    CHECK(rules::Judge(host, a, false).refusal == R::kFiles);
  }

  Group("Judge: a files text with no group at all (the files could not be read) agrees with nothing, itself included");
  {
    const char* empties[] = {"", "\n", "\n\n"};
    for (const char* empty : empties) {
      const rules::Verdict h = rules::Judge(Id(host.build, "netplay-v1.0.5", empty), host, false);
      CHECK(h.refusal == R::kFiles && Has(h.text, "The game files could not be read on the host") && !Has(h.text, "joiner"));
      const rules::Verdict j = rules::Judge(host, Id(host.build, "netplay-v1.0.5", empty), false);
      CHECK(j.refusal == R::kFiles && Has(j.text, "The game files could not be read on the joiner") && !Has(j.text, "the host"));
      const rules::Verdict b = rules::Judge(Id(host.build, "netplay-v1.0.5", empty), Id(host.build, "netplay-v1.0.5", empty), false);
      CHECK(b.refusal == R::kFiles && Has(b.text, "The game files could not be read on either side"));
      // the earlier rules still come first: busy, an unreadable build, a differing build
      CHECK(rules::Judge(Id(host.build, "netplay-v1.0.5", empty), host, true).refusal == R::kBusy);
      CHECK(rules::Judge(Id(0, "netplay-v1.0.5", empty), host, false).refusal == R::kBuild);
      CHECK(rules::Judge(Id(host.build, "netplay-v1.0.5", empty), Id(host.build + 1, "netplay-v1.0.6", kFiles), false).refusal == R::kBuild);
    }
    // a line that is not blank is a group, even a bare "a=": that text is not unreadable
    CHECK(rules::Judge(Id(host.build, "netplay-v1.0.5", "a=\n"), Id(host.build, "netplay-v1.0.5", "a=\n"), false).refusal == R::kNone);
    CHECK(rules::Judge(host, host, false).refusal == R::kNone);  // the control: a readable pair agrees
  }

  Group("Judge: the verdict values are the wire's refusal codes");
  CHECK(uint8_t(R::kBuild) == 2 && uint8_t(R::kFiles) == 3 && uint8_t(R::kBusy) == 4 && uint8_t(R::kNone) == 0);
}

static void TestDifferingGroups() {
  using V = std::vector<std::string>;
  Group("DifferingGroups: a group whose value differs or is missing on one side");
  CHECK(rules::DifferingGroups("a=1\nb=2\n", "a=1\nb=3\nc=4\n") == V({"b", "c"}));
  CHECK(rules::DifferingGroups("a=1\nb=2\n", "a=1\nb=2\n").empty());
  CHECK(rules::DifferingGroups("", "").empty());
  CHECK(rules::DifferingGroups("a=1\n", "a=1\nb=2\n") == V({"b"}));   // missing on the first side
  CHECK(rules::DifferingGroups("a=1\nb=2\n", "a=1\n") == V({"b"}));   // missing on the second
  CHECK(rules::DifferingGroups("a=1\n", "") == V({"a"}));
  CHECK(rules::DifferingGroups("", "a=1\n") == V({"a"}));
  CHECK(rules::DifferingGroups("a=1\n", "a=2\n") == V({"a"}));

  Group("DifferingGroups: the names come back sorted, whatever order the texts list them in");
  CHECK(rules::DifferingGroups("z=1\nb=2\nm=3\n", "z=2\nb=3\nm=4\n") == V({"b", "m", "z"}));
  CHECK(rules::DifferingGroups("z=1\nb=2\n", "m=3\na=4\n") == V({"a", "b", "m", "z"}));
  CHECK(rules::DifferingGroups("b=2\nz=1\n", "z=1\nb=2\n").empty());  // the same groups in another order agree

  Group("DifferingGroups: \"unreadable\" agrees with nothing (np_files)");
  CHECK(rules::DifferingGroups("a=unreadable\n", "a=unreadable\n") == V({"a"}));
  CHECK(rules::DifferingGroups("a=unreadable\nb=1\n", "a=5\nb=1\n") == V({"a"}));   // on the first side only
  CHECK(rules::DifferingGroups("a=5\nb=1\n", "a=unreadable\nb=1\n") == V({"a"}));   // on the second side only
  CHECK(rules::DifferingGroups("a=unreadable\n", "") == V({"a"}));
  CHECK(rules::DifferingGroups("a=unreadable\nb=unreadable\nc=1\n", "a=unreadable\nb=2\nc=1\n") == V({"a", "b"}));
  CHECK(rules::DifferingGroups("a=unreadable2\n", "a=unreadable2\n").empty());      // only the exact word
  CHECK(rules::DifferingGroups("a=Unreadable\n", "a=Unreadable\n").empty());
  CHECK(rules::DifferingGroups("a=1\n", "a=1\n").empty());

  Group("DifferingGroups: how the lines are read");
  CHECK(rules::DifferingGroups("a=1\n\nb=2\n", "a=1\nb=2\n").empty());      // blank lines are nothing
  CHECK(rules::DifferingGroups("a=1\nb=2", "a=1\nb=2\n").empty());          // a last line without its newline counts
  CHECK(rules::DifferingGroups("a=b=c\n", "a=b=c\n").empty());              // the name ends at the first '='
  CHECK(rules::DifferingGroups("a=b=c\n", "a=b=d\n") == V({"a"}));
  CHECK(rules::DifferingGroups("a=b=c\n", "a=b\n") == V({"a"}));
  CHECK(rules::DifferingGroups("novalue\n", "") == V({"novalue"}));         // a line without '=' is a group nobody can match
  CHECK(rules::DifferingGroups("novalue\n", "novalue\n").empty());
  CHECK(rules::DifferingGroups("a=\n", "a=\n").empty());                    // an empty value is a value
  CHECK(rules::DifferingGroups("a=\n", "a=1\n") == V({"a"}));
  CHECK(rules::DifferingGroups("a=\n", "") == V({"a"}));
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  TestJoinCodes();
  TestStun();
  TestNat();
  TestDelayPacer();
  TestVersions();
  TestPickRelease();
  TestAdvice();
  TestJudge();
  TestDifferingGroups();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
