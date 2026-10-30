// Host test for src/net/np_wire.cpp (the DOA4 netplay wire format and the reliable start-data transfer).
// Built and run by run_np_wire_test.bat; "np_wire_host_test.exe -v" prints each group before it runs, so a
// crash (the build uses the standard library's hardening mode, which fast-fails an out-of-range index or
// subspan) names the group it happened in.
//
// Every expected byte below is derived by hand from the layout in np_wire.h and the brief (Task 2), never by
// running the encoder; the hand-built datagrams and bodies use their own little builder (Bld), not Encode.

#include "../../src/net/np_wire.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace doa4::np::wire;
using Bytes = std::vector<uint8_t>;

static int g_checks = 0;
static int g_failures = 0;
static bool g_verbose = false;
static const char* g_group = "";
#define CHECK(cond)                                                                   \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(cond)) {                                                                    \
      std::printf("FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_group, #cond);       \
      ++g_failures;                                                                   \
    }                                                                                 \
  } while (0)

// Names the checks that follow: a FAIL line carries it, so a red check is attributable to a rule.
static void Group(const char* name) {
  g_group = name;
  if (g_verbose) {
    std::printf("group: %s\n", name);
    std::fflush(stdout);
  }
}

// The values the brief fixes, pinned here from the brief and not read back from the header.
static_assert(kHeaderBytes == 12 && kMaxDatagram == 1400 && kChunkBytes == 1200 && kMaxChunks == 255 && kMaxText == 200);
static_assert(kMaxChunks * kChunkBytes == 306000);
static_assert(kMagic == 0x34414F44u && kProto == 1 && kMsgBoot == 1);

// ---- helpers (independent of the code under test) ----

static std::span<const uint8_t> S(const Bytes& b) { return std::span<const uint8_t>(b.data(), b.size()); }

struct Bld {  // little-endian body builder
  Bytes b;
  Bld& U8(unsigned v) { b.push_back(uint8_t(v & 0xFF)); return *this; }
  Bld& U16(unsigned v) { U8(v); return U8(v >> 8); }
  Bld& U32(uint32_t v) { U16(v & 0xFFFF); return U16(v >> 16); }
  Bld& U64(uint64_t v) { U32(uint32_t(v)); return U32(uint32_t(v >> 32)); }
  Bld& Fill(size_t n, uint8_t v) { b.insert(b.end(), n, v); return *this; }
  Bld& Raw(const Bytes& r) { b.insert(b.end(), r.begin(), r.end()); return *this; }
  Bld& Str(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); return *this; }
};

// A datagram built by hand: the magic is spelled as its four bytes.
static Bytes Hand(unsigned proto, unsigned type, unsigned len_field, uint32_t token, const Bytes& body) {
  Bld b;
  b.U8(0x44).U8(0x4F).U8(0x41).U8(0x34).U8(proto).U8(type).U16(len_field).U32(token).Raw(body);
  return b.b;
}

struct Rng {
  uint64_t x;
  explicit Rng(uint64_t seed) : x(seed) {}
  uint8_t Byte() {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return uint8_t(x);
  }
  unsigned U16() {
    unsigned lo = Byte();
    unsigned hi = Byte();
    return lo | (hi << 8);
  }
};

static Bytes Pattern(size_t n, uint64_t seed) {
  Rng r(seed);
  Bytes b(n);
  for (auto& v : b) v = r.Byte();
  return b;
}

// n bytes of text that include NUL, LF, CR and high bytes (a text field is bytes, not a C string).
static std::string TextOf(size_t n, unsigned seed) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = char((i * 7 + seed) & 0xFF);
  return s;
}

// ---- message fixtures: every field away from its default ----

static Hello MakeHello() {
  Hello h;
  h.build = 0x0807060504030201ull;
  h.release = "dev";
  h.files = "a=1\n";
  h.delay_auto = 0;
  h.delay_frames = 3;
  h.session_token = 0xAABBCCDDu;
  return h;
}
static Hello BigHello() {
  Hello h = MakeHello();
  h.release = TextOf(200, 3);
  h.files = TextOf(1000, 11);
  return h;
}
static Welcome MakeWelcome() {
  Welcome w;
  w.refusal = Refusal::kBuild;
  w.build = 0x1122334455667788ull;
  w.release = "r1";
  w.text = "no";
  w.session_token = 0x0A0B0C0Du;
  return w;
}
static Welcome BigWelcome() {
  Welcome w = MakeWelcome();
  w.release = TextOf(200, 5);
  w.text = TextOf(200, 9);
  return w;
}
static BootReply MakeBootReply() {
  BootReply r;
  r.session_token = 0x11223344u;
  r.ok = 1;
  r.text = "x";
  return r;
}
static BootReply BigBootReply() {
  BootReply r = MakeBootReply();
  r.text = TextOf(200, 13);
  return r;
}
static Leave MakeLeave() {
  Leave l;
  l.reason = LeaveReason::kDesync;
  l.frame = -2;
  return l;
}
static Ack MakeAck() {
  Ack a;
  a.msg_id = 0x01020304u;
  a.have = {0x0807060504030201ull, 0x1ull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull};
  return a;
}

static bool Same(const Hello& a, const Hello& b) {
  return a.build == b.build && a.release == b.release && a.files == b.files && a.delay_auto == b.delay_auto &&
         a.delay_frames == b.delay_frames && a.session_token == b.session_token;
}
static bool Same(const Welcome& a, const Welcome& b) {
  return a.refusal == b.refusal && a.build == b.build && a.release == b.release && a.text == b.text &&
         a.session_token == b.session_token;
}
static bool Same(const Ping& a, const Ping& b) { return a.t_ms == b.t_ms; }
static bool Same(const Ack& a, const Ack& b) { return a.msg_id == b.msg_id && a.have == b.have; }
static bool Same(const Leave& a, const Leave& b) { return a.reason == b.reason && a.frame == b.frame; }
static bool Same(const BootReply& a, const BootReply& b) {
  return a.session_token == b.session_token && a.ok == b.ok && a.text == b.text;
}
static bool Same(const Chunk& a, const Chunk& b) {
  return a.msg_id == b.msg_id && a.msg_type == b.msg_type && a.count == b.count && a.index == b.index &&
         a.payload.size() == b.payload.size() &&
         (a.payload.empty() || std::memcmp(a.payload.data(), b.payload.data(), a.payload.size()) == 0);
}

// How many fields differ from a default-constructed message (a fixture precondition, asserted).
static int NonDefault(const Hello& h) {
  const Hello d;
  return int(h.build != d.build) + int(h.release != d.release) + int(h.files != d.files) +
         int(h.delay_auto != d.delay_auto) + int(h.delay_frames != d.delay_frames) +
         int(h.session_token != d.session_token);
}
static int NonDefault(const Welcome& w) {
  const Welcome d;
  return int(w.refusal != d.refusal) + int(w.build != d.build) + int(w.release != d.release) + int(w.text != d.text) +
         int(w.session_token != d.session_token);
}
static int NonDefault(const BootReply& r) {
  const BootReply d;
  return int(r.session_token != d.session_token) + int(r.ok != d.ok) + int(r.text != d.text);
}
static int NonDefault(const Leave& l) {
  const Leave d;
  return int(l.reason != d.reason) + int(l.frame != d.frame);
}
static int NonDefault(const Ack& a) {
  const Ack d;
  return int(a.msg_id != d.msg_id) + int(a.have[0] != d.have[0]) + int(a.have[1] != d.have[1]) +
         int(a.have[2] != d.have[2]) + int(a.have[3] != d.have[3]);
}

// Every strict prefix of a valid body must be refused, and a refusal must leave *out as it was. The cut body is
// its own heap block, so a read past it is visible to the hardened standard library.
template <class T, class Dec>
static void CheckPrefixesAndTail(const Bytes& body, const T& sentinel, Dec dec, size_t* refused_out, bool* untouched_out,
                                 bool* trailing_refused_out) {
  size_t refused = 0;
  bool untouched = true;
  for (size_t n = 0; n < body.size(); ++n) {
    Bytes cut(body.begin(), body.begin() + n);
    T t = sentinel;
    if (!dec(S(cut), &t)) {
      ++refused;
      if (!Same(t, sentinel)) untouched = false;
    }
  }
  bool trailing_refused = true;
  for (unsigned extra : {0u, 0xFFu}) {  // one trailing byte of either value
    Bytes plus = body;
    plus.push_back(uint8_t(extra));
    T t = sentinel;
    if (dec(S(plus), &t)) trailing_refused = false;
    else if (!Same(t, sentinel)) untouched = false;
  }
  *refused_out = refused;
  *untouched_out = untouched;
  *trailing_refused_out = trailing_refused;
}

// ---- header and drop reasons ----

static void TestHeader() {
  Group("header.golden");
  // Derived by hand: magic 0x34414F44 little-endian is 44 4F 41 34; proto 1; kPing = 3; length 0 (00 00);
  // token 0x01020304 little-endian is 04 03 02 01.
  const Bytes want = {0x44, 0x4F, 0x41, 0x34, 0x01, 0x03, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01};
  CHECK(Encode(Type::kPing, 0x01020304u, {}) == want);
  Packet p;
  p.token = 0xDEADBEEFu;
  CHECK(Decode(want, &p) == Drop::kNone);
  CHECK(p.type == Type::kPing);
  CHECK(p.token == 0x01020304u);
  CHECK(p.body.empty());

  Group("header.golden.body");
  const Bytes b3 = {1, 2, 3};
  const Bytes want3 = {0x44, 0x4F, 0x41, 0x34, 0x01, 0x01, 0x03, 0x00, 0xDD, 0xCC, 0xBB, 0xAA, 0x01, 0x02, 0x03};
  CHECK(Encode(Type::kHello, 0xAABBCCDDu, S(b3)) == want3);
  Packet q;
  CHECK(Decode(want3, &q) == Drop::kNone);
  CHECK(q.type == Type::kHello && q.token == 0xAABBCCDDu);
  CHECK(q.body.size() == 3 && q.body.data() == want3.data() + kHeaderBytes);  // the body points into the datagram
  CHECK(q.body.size() == 3 && q.body[0] == 1 && q.body[1] == 2 && q.body[2] == 3);  // the size test guards the indexing

  Group("header.types");
  for (unsigned t = 1; t <= 11; ++t) {  // both edges of the type range are inside: 1 and 11
    const Bytes dg = Hand(1, t, 0, 5, {});
    Packet pk;
    CHECK(Decode(dg, &pk) == Drop::kNone && unsigned(pk.type) == t);
    CHECK(Encode(Type(t), 5, {}) == dg);
  }
}

static void TestDrops() {
  Group("drop.short");
  const Bytes good = Hand(1, 3, 4, 7, Bytes{1, 2, 3, 4});
  Packet sentinel;
  sentinel.type = Type::kBye;
  sentinel.token = 0xDEADu;
  bool out_untouched = true;
  auto Drops = [&](const Bytes& dg) {
    Packet p = sentinel;
    const Drop d = Decode(dg, &p);
    if (d != Drop::kNone && (p.type != sentinel.type || p.token != sentinel.token || !p.body.empty())) out_untouched = false;
    return d;
  };
  CHECK(Drops(good) == Drop::kNone);
  CHECK(Drops(Bytes(good.begin(), good.begin() + 11)) == Drop::kShort);  // 11 bytes: one under the header
  CHECK(Drops(Bytes{}) == Drop::kShort);
  CHECK(Drops(Hand(1, 3, 0, 7, {})) == Drop::kNone);  // exactly 12 bytes is a whole header

  Group("drop.magic");
  for (size_t i = 0; i < 4; ++i) {  // every byte of the magic is checked, not only the first
    Bytes dg = good;
    dg[i] ^= 0x01;
    CHECK(Drops(dg) == Drop::kMagic);
  }
  Bytes wrong_first = good;
  wrong_first[0] = 0x45;
  CHECK(Drops(wrong_first) == Drop::kMagic);

  Group("drop.proto");
  CHECK(Drops(Hand(2, 3, 4, 7, Bytes{1, 2, 3, 4})) == Drop::kProto);
  CHECK(Drops(Hand(0, 3, 4, 7, Bytes{1, 2, 3, 4})) == Drop::kProto);    // below the current one
  CHECK(Drops(Hand(255, 3, 4, 7, Bytes{1, 2, 3, 4})) == Drop::kProto);

  Group("drop.type");
  CHECK(Drops(Hand(1, 0, 0, 7, {})) == Drop::kType);
  CHECK(Drops(Hand(1, 12, 0, 7, {})) == Drop::kType);   // 11 is the last type, 12 the first refused
  CHECK(Drops(Hand(1, 255, 0, 7, {})) == Drop::kType);

  Group("drop.length");
  CHECK(Drops(Hand(1, 3, 5, 7, Bytes{1, 2, 3, 4})) == Drop::kLength);       // length field 5, 4 body bytes
  CHECK(Drops(Hand(1, 3, 3, 7, Bytes{1, 2, 3, 4})) == Drop::kLength);       // a trailing byte past the stated length
  CHECK(Drops(Hand(1, 3, 0xFFFF, 7, Bytes{1, 2, 3, 4})) == Drop::kLength);
  CHECK(Drops(Hand(1, 3, 1, 7, {})) == Drop::kLength);                      // a body that is not there

  Group("drop.toolarge");
  const Bytes body1388(1388, 0xAB), body1389(1389, 0xAB), body1488(1488, 0xAB);
  CHECK(Drops(Hand(1, 3, 1388, 7, body1388)) == Drop::kNone);               // 1400 bytes: kMaxDatagram is allowed
  CHECK(Drops(Hand(1, 3, 1389, 7, body1389)) == Drop::kTooLarge);           // 1401 bytes, otherwise a good packet
  CHECK(Drops(Hand(1, 3, 1488, 7, body1488)) == Drop::kTooLarge);           // 1500 bytes

  Group("drop.order");  // a datagram that is wrong in two ways reports the first of: short, too large, magic, proto, type, length
  Bytes short_bad_magic = good;
  short_bad_magic.resize(11);
  short_bad_magic[0] = 0x45;
  CHECK(Drops(short_bad_magic) == Drop::kShort);
  Bytes big_bad_magic = Hand(1, 3, 1389, 7, body1389);
  big_bad_magic[0] = 0x45;
  CHECK(Drops(big_bad_magic) == Drop::kTooLarge);
  CHECK(Drops(Hand(1, 3, 5, 7, body1389)) == Drop::kTooLarge);              // too large and a wrong length field
  Bytes magic_proto = Hand(2, 3, 4, 7, Bytes{1, 2, 3, 4});
  magic_proto[1] = 0x00;
  CHECK(Drops(magic_proto) == Drop::kMagic);
  CHECK(Drops(Hand(2, 12, 4, 7, Bytes{1, 2, 3, 4})) == Drop::kProto);       // wrong proto and wrong type
  CHECK(Drops(Hand(1, 12, 5, 7, Bytes{1, 2, 3, 4})) == Drop::kType);        // wrong type and wrong length

  Group("drop.out-untouched");
  CHECK(out_untouched);  // every drop above left *out as the caller had it

  Group("drop.name");
  std::vector<std::string> names;
  for (unsigned d = 0; d < unsigned(Drop::kCount); ++d) {
    const char* n = DropName(Drop(d));
    CHECK(n != nullptr && n[0] != '\0');
    if (n) names.emplace_back(n);
  }
  std::sort(names.begin(), names.end());
  CHECK(names.size() == unsigned(Drop::kCount) && std::adjacent_find(names.begin(), names.end()) == names.end());
  CHECK(std::string(DropName(Drop::kCount)) == "?");
  CHECK(std::string(DropName(Drop(200))) == "?");
}

static void TestEncodeLimits() {
  Group("encode.limit");
  CHECK(Encode(Type::kPing, 1, {}).size() == 12);
  CHECK(Encode(Type::kPing, 1, S(Bytes(1388, 0))).size() == 1400);  // header + body == kMaxDatagram is allowed
  CHECK(Encode(Type::kPing, 1, S(Bytes(1389, 0))).empty());         // one more is refused
  CHECK(Encode(Type::kPing, 1, S(Bytes(60000, 0))).empty());
  CHECK(Encode(Type(0), 1, {}).empty());                            // a packet Decode would drop is not built
  CHECK(Encode(Type(12), 1, {}).empty());
  CHECK(Encode(Type(255), 1, {}).empty());

  Group("encode.roundtrip");
  const Bytes body = Pattern(100, 42);
  for (unsigned t = 1; t <= 11; ++t) {
    const Bytes dg = Encode(Type(t), 0xFEDCBA98u, S(body));
    Packet p;
    CHECK(Decode(dg, &p) == Drop::kNone);
    CHECK(unsigned(p.type) == t && p.token == 0xFEDCBA98u);
    CHECK(Bytes(p.body.begin(), p.body.end()) == body);
  }
}

// ---- message bodies ----

template <class T>
static void ExpectRoundTrip(const T& m, const Bytes& body, bool (*dec)(std::span<const uint8_t>, T*)) {
  T back;
  CHECK(dec(S(body), &back));
  CHECK(Same(back, m));
}

static void TestMessages() {
  Group("msg.golden");
  // Hello: u64 build | text release | text files | u8 delay_auto | u8 delay_frames | u32 session_token.
  const Hello hello = MakeHello();
  CHECK(NonDefault(hello) == 6);
  const Bytes want_hello = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,              // build
                            0x03, 0x00, 0x64, 0x65, 0x76,                                  // "dev"
                            0x04, 0x00, 0x61, 0x3D, 0x31, 0x0A,                            // "a=1\n"
                            0x00, 0x03,                                                    // delay_auto 0, delay_frames 3
                            0xDD, 0xCC, 0xBB, 0xAA};                                       // session_token
  CHECK(EncodeHello(hello) == want_hello);
  // Welcome: u8 refusal | u64 build | text release | text text | u32 session_token.
  const Welcome welcome = MakeWelcome();
  CHECK(NonDefault(welcome) == 5);
  const Bytes want_welcome = {0x02,                                                        // kBuild
                              0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,              // build
                              0x02, 0x00, 0x72, 0x31,                                      // "r1"
                              0x02, 0x00, 0x6E, 0x6F,                                      // "no"
                              0x0D, 0x0C, 0x0B, 0x0A};                                     // session_token
  CHECK(EncodeWelcome(welcome) == want_welcome);
  // kPing and kPong: u32 t_ms.
  CHECK(EncodePing(Ping{0x01020304u}) == (Bytes{0x04, 0x03, 0x02, 0x01}));
  // Ack: u32 msg_id | 4 x u64 have, word 0 first.
  const Ack ack = MakeAck();
  CHECK(NonDefault(ack) == 5);
  const Bytes want_ack = {0x04, 0x03, 0x02, 0x01,
                          0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                          0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(EncodeAck(ack) == want_ack);
  // Leave: u8 reason | i32 frame.
  const Leave leave = MakeLeave();
  CHECK(NonDefault(leave) == 2);
  CHECK(EncodeLeave(leave) == (Bytes{0x01, 0xFE, 0xFF, 0xFF, 0xFF}));
  Leave edge;
  edge.reason = LeaveReason::kInternal;
  edge.frame = INT32_MAX;
  CHECK(EncodeLeave(edge) == (Bytes{0x03, 0xFF, 0xFF, 0xFF, 0x7F}));
  edge.reason = LeaveReason::kTimeout;
  edge.frame = INT32_MIN;
  CHECK(EncodeLeave(edge) == (Bytes{0x02, 0x00, 0x00, 0x00, 0x80}));
  // BootReply: u32 session_token | u8 ok | text.
  const BootReply boot = MakeBootReply();
  CHECK(NonDefault(boot) == 3);
  CHECK(EncodeBootReply(boot) == (Bytes{0x44, 0x33, 0x22, 0x11, 0x01, 0x01, 0x00, 0x78}));
  // Chunk: u32 msg_id | u8 msg_type | u8 count | u8 index | u8 0 | u16 payload length | payload.
  const Bytes pay = {0xAA, 0xBB};
  Chunk ck;
  ck.msg_id = 0x0A0B0C0Du;
  ck.msg_type = 1;
  ck.count = 3;
  ck.index = 2;
  ck.payload = S(pay);
  CHECK(EncodeChunk(ck) == (Bytes{0x0D, 0x0C, 0x0B, 0x0A, 0x01, 0x03, 0x02, 0x00, 0x02, 0x00, 0xAA, 0xBB}));

  Group("msg.roundtrip");
  ExpectRoundTrip<Hello>(hello, EncodeHello(hello), &DecodeHello);
  ExpectRoundTrip<Welcome>(welcome, EncodeWelcome(welcome), &DecodeWelcome);
  ExpectRoundTrip<Ping>(Ping{0xFEDCBA98u}, EncodePing(Ping{0xFEDCBA98u}), &DecodePing);
  ExpectRoundTrip<Ack>(ack, EncodeAck(ack), &DecodeAck);
  ExpectRoundTrip<Leave>(leave, EncodeLeave(leave), &DecodeLeave);
  ExpectRoundTrip<BootReply>(boot, EncodeBootReply(boot), &DecodeBootReply);
  for (unsigned r = 0; r <= 3; ++r) {  // every leave reason, and the extremes of the frame
    Leave l;
    l.reason = LeaveReason(r);
    l.frame = (r % 2) ? INT32_MIN : INT32_MAX;
    ExpectRoundTrip<Leave>(l, EncodeLeave(l), &DecodeLeave);
  }
  for (unsigned r = 0; r <= 5; ++r) {  // every refusal
    Welcome w = welcome;
    w.refusal = Refusal(r);
    ExpectRoundTrip<Welcome>(w, EncodeWelcome(w), &DecodeWelcome);
  }
  {
    Hello h = hello;  // the delay values are carried as given (their range is the session's rule, not the wire's)
    h.delay_frames = 255;
    h.delay_auto = 255;
    ExpectRoundTrip<Hello>(h, EncodeHello(h), &DecodeHello);
    BootReply r = boot;
    r.ok = 255;
    ExpectRoundTrip<BootReply>(r, EncodeBootReply(r), &DecodeBootReply);
  }
  {
    Chunk back;
    const Bytes body = EncodeChunk(ck);
    CHECK(DecodeChunk(S(body), &back));
    CHECK(Same(back, ck));
    CHECK(back.payload.data() == body.data() + 10);  // the payload points into the body
  }

  Group("msg.roundtrip.max");
  const Hello big_hello = BigHello();
  CHECK(NonDefault(big_hello) == 6 && big_hello.release.size() == 200 && big_hello.files.size() == 1000);
  ExpectRoundTrip<Hello>(big_hello, EncodeHello(big_hello), &DecodeHello);
  const Welcome big_welcome = BigWelcome();
  CHECK(NonDefault(big_welcome) == 5 && big_welcome.release.size() == 200 && big_welcome.text.size() == 200);
  ExpectRoundTrip<Welcome>(big_welcome, EncodeWelcome(big_welcome), &DecodeWelcome);
  const BootReply big_boot = BigBootReply();
  CHECK(big_boot.text.size() == 200);
  ExpectRoundTrip<BootReply>(big_boot, EncodeBootReply(big_boot), &DecodeBootReply);
  // The largest Hello still fits one datagram: 12 header + 8 + (2 + 200) + (2 + 1000) + 1 + 1 + 4 = 1230.
  const Bytes big_dg = Encode(Type::kHello, 9, S(EncodeHello(big_hello)));
  CHECK(big_dg.size() == 1230 && big_dg.size() <= kMaxDatagram);
  Packet p;
  Hello back;
  CHECK(Decode(big_dg, &p) == Drop::kNone && DecodeHello(p.body, &back) && Same(back, big_hello));

  Group("msg.text.limit");
  {
    Hello h = hello;
    h.release = std::string(200, 'r');  // the accept edge
    CHECK(!EncodeHello(h).empty());
    h.release = std::string(201, 'r');
    CHECK(EncodeHello(h).empty());      // the reject edge
    h = hello;
    h.files = std::string(1000, 'f');
    CHECK(!EncodeHello(h).empty());
    h.files = std::string(1001, 'f');
    CHECK(EncodeHello(h).empty());
    Welcome w = welcome;
    w.release = std::string(201, 'r');
    CHECK(EncodeWelcome(w).empty());
    w = welcome;
    w.text = std::string(200, 't');
    CHECK(!EncodeWelcome(w).empty());
    w.text = std::string(201, 't');
    CHECK(EncodeWelcome(w).empty());
    BootReply r = boot;
    r.text = std::string(200, 't');
    CHECK(!EncodeBootReply(r).empty());
    r.text = std::string(201, 't');
    CHECK(EncodeBootReply(r).empty());
    CHECK(EncodeWelcome(Welcome{Refusal(6), 1, "r", "t", 1}).empty());     // an unknown refusal is not sent
    CHECK(EncodeLeave(Leave{LeaveReason(4), 1}).empty());                  // nor an unknown leave reason
    Chunk c;
    const Bytes max_payload(1200, 0x5A), over(1201, 0x5A);
    c.count = 1;
    c.payload = S(max_payload);
    CHECK(!EncodeChunk(c).empty());                                         // 1200: the accept edge
    c.payload = S(over);
    CHECK(EncodeChunk(c).empty());                                          // 1201: refused
  }

  Group("msg.decode.limit");
  {
    // A Welcome body built by hand: u8 refusal | u64 build | text release | text text | u32 token.
    auto WelcomeBody = [](size_t release_len_field, size_t release_bytes) {
      Bld b;
      b.U8(0).U64(1).U16(unsigned(release_len_field)).Fill(release_bytes, 'r').U16(0).U32(0);
      return b.b;
    };
    Welcome w;
    CHECK(DecodeWelcome(S(WelcomeBody(200, 200)), &w) && w.release.size() == 200);  // the limit itself is accepted
    CHECK(!DecodeWelcome(S(WelcomeBody(201, 201)), &w));    // over the limit, every byte present: the limit check alone refuses it
    CHECK(!DecodeWelcome(S(WelcomeBody(0xFFFF, 0xFFFF)), &w));
    Bld hb;  // a Hello whose files text is 1,001 bytes, all present
    hb.U64(1).U16(0).U16(1001).Fill(1001, 'f').U8(1).U8(2).U32(0);
    Hello h;
    CHECK(!DecodeHello(S(hb.b), &h));
    Bld ok;  // the same with 1,000
    ok.U64(1).U16(0).U16(1000).Fill(1000, 'f').U8(1).U8(2).U32(0);
    CHECK(DecodeHello(S(ok.b), &h) && h.files.size() == 1000);
  }

  Group("msg.decode.bounds");
  {
    // A text length that points past the end of the body, below the limit (so only the bounds check can refuse it).
    Bld b;
    b.U8(0).U64(1).U16(100).Fill(50, 'r');  // release says 100 bytes, 50 follow, nothing after
    Welcome w;
    CHECK(!DecodeWelcome(S(b.b), &w));
    Bld c;  // one byte short at the very end of the second text
    c.U8(0).U64(1).U16(1).Fill(1, 'r').U16(5).Fill(4, 't');
    CHECK(!DecodeWelcome(S(c.b), &w));
    Bld d;  // the BootReply text: u32 token, u8 ok, text length 3 with 2 bytes
    d.U32(1).U8(1).U16(3).Fill(2, 'x');
    BootReply r;
    CHECK(!DecodeBootReply(S(d.b), &r));
    Bld e;  // a length of 0xFFFF in a short body
    e.U32(1).U8(1).U16(0xFFFF).Fill(2, 'x');
    CHECK(!DecodeBootReply(S(e.b), &r));
  }

  Group("msg.decode.enum");
  {
    Bld b;  // a Welcome with refusal 6 (the last known is 5), otherwise good
    b.U8(6).U64(1).U16(0).U16(0).U32(0);
    Welcome w;
    CHECK(!DecodeWelcome(S(b.b), &w));
    Bld ok;
    ok.U8(5).U64(1).U16(0).U16(0).U32(0);
    CHECK(DecodeWelcome(S(ok.b), &w) && w.refusal == Refusal::kSession);
    Bld l;  // a Leave with reason 4 (the last known is 3)
    l.U8(4).U32(0);
    Leave lv;
    CHECK(!DecodeLeave(S(l.b), &lv));
    Bld lok;
    lok.U8(3).U32(0);
    CHECK(DecodeLeave(S(lok.b), &lv) && lv.reason == LeaveReason::kInternal);
  }

  Group("msg.decode.prefix");  // each decoder refuses every strict prefix of a valid body, a trailing byte, and leaves *out alone
  size_t refused = 0;
  bool untouched = false, trailing = false;
  {
    const Hello sentinel = [] { Hello s = MakeHello(); s.release = "sentinel"; return s; }();
    for (const Hello* m : {&hello, &big_hello}) {
      const Bytes body = EncodeHello(*m);
      CheckPrefixesAndTail<Hello>(body, sentinel, [](std::span<const uint8_t> s, Hello* o) { return DecodeHello(s, o); }, &refused, &untouched, &trailing);
      CHECK(refused == body.size() && untouched && trailing);
    }
  }
  {
    const Welcome sentinel = [] { Welcome s = MakeWelcome(); s.text = "sentinel"; return s; }();
    for (const Welcome* m : {&welcome, &big_welcome}) {
      const Bytes body = EncodeWelcome(*m);
      CheckPrefixesAndTail<Welcome>(body, sentinel, [](std::span<const uint8_t> s, Welcome* o) { return DecodeWelcome(s, o); }, &refused, &untouched, &trailing);
      CHECK(refused == body.size() && untouched && trailing);
    }
  }
  {
    const Ping sentinel{1};
    const Bytes body = EncodePing(Ping{0x01020304u});
    CheckPrefixesAndTail<Ping>(body, sentinel, [](std::span<const uint8_t> s, Ping* o) { return DecodePing(s, o); }, &refused, &untouched, &trailing);
    CHECK(refused == body.size() && refused == 4 && untouched && trailing);
  }
  {
    Ack sentinel = ack;
    sentinel.msg_id = 99;
    const Bytes body = EncodeAck(ack);
    CheckPrefixesAndTail<Ack>(body, sentinel, [](std::span<const uint8_t> s, Ack* o) { return DecodeAck(s, o); }, &refused, &untouched, &trailing);
    CHECK(refused == body.size() && refused == 36 && untouched && trailing);
  }
  {
    Leave sentinel = leave;
    sentinel.frame = 99;
    const Bytes body = EncodeLeave(leave);
    CheckPrefixesAndTail<Leave>(body, sentinel, [](std::span<const uint8_t> s, Leave* o) { return DecodeLeave(s, o); }, &refused, &untouched, &trailing);
    CHECK(refused == body.size() && refused == 5 && untouched && trailing);
  }
  {
    const BootReply sentinel = [] { BootReply s = MakeBootReply(); s.text = "sentinel"; return s; }();
    for (const BootReply* m : {&boot, &big_boot}) {
      const Bytes body = EncodeBootReply(*m);
      CheckPrefixesAndTail<BootReply>(body, sentinel, [](std::span<const uint8_t> s, BootReply* o) { return DecodeBootReply(s, o); }, &refused, &untouched, &trailing);
      CHECK(refused == body.size() && untouched && trailing);
    }
  }
  {
    Chunk sentinel;
    sentinel.msg_id = 99;
    for (size_t plen : {size_t(5), size_t(1200)}) {
      const Bytes payload = Pattern(plen, 7);
      Chunk c = ck;
      c.payload = S(payload);
      const Bytes body = EncodeChunk(c);
      CheckPrefixesAndTail<Chunk>(body, sentinel, [](std::span<const uint8_t> s, Chunk* o) { return DecodeChunk(s, o); }, &refused, &untouched, &trailing);
      CHECK(refused == body.size() && body.size() == 10 + plen && untouched && trailing);
    }
  }

  Group("msg.chunk");
  {
    // A chunk body by hand: msg_id, type, count, index, pad, length field, payload bytes.
    auto ChunkBody = [](unsigned len_field, size_t bytes, unsigned pad) {
      Bld b;
      b.U32(7).U8(1).U8(2).U8(0).U8(pad).U16(len_field).Fill(bytes, 0x33);
      return b.b;
    };
    Chunk c;
    CHECK(DecodeChunk(S(ChunkBody(1200, 1200, 0)), &c) && c.payload.size() == 1200);   // kChunkBytes is accepted
    CHECK(!DecodeChunk(S(ChunkBody(1201, 1201, 0)), &c));                              // one more is refused though every byte is present
    CHECK(!DecodeChunk(S(ChunkBody(5, 4, 0)), &c));                                    // the length field claims more than the body holds
    CHECK(!DecodeChunk(S(ChunkBody(3, 4, 0)), &c));                                    // a body longer than fields + length
    CHECK(DecodeChunk(S(ChunkBody(0, 0, 0)), &c) && c.payload.empty());                // an empty payload parses (the receiver judges it)
    CHECK(DecodeChunk(S(ChunkBody(4, 4, 0x55)), &c) && c.payload.size() == 4);         // the reserved byte is ignored on decode
    CHECK(DecodeChunk(S(ChunkBody(4, 4, 0)), &c) && c.msg_id == 7 && c.msg_type == 1 && c.count == 2 && c.index == 0);
  }
}

// ---- fuzz ----

static bool DecodeBody(const Packet& p) {
  switch (p.type) {
    case Type::kHello: { Hello m; return DecodeHello(p.body, &m); }
    case Type::kWelcome: { Welcome m; return DecodeWelcome(p.body, &m); }
    case Type::kPing:
    case Type::kPong: { Ping m; return DecodePing(p.body, &m); }
    case Type::kChunk: { Chunk m; return DecodeChunk(p.body, &m); }
    case Type::kAck: { Ack m; return DecodeAck(p.body, &m); }
    case Type::kLeave: { Leave m; return DecodeLeave(p.body, &m); }
    case Type::kBootReply: { BootReply m; return DecodeBootReply(p.body, &m); }
    default: return true;  // kBye, kPunch, kGekko: opaque bodies, read by other modules
  }
}

// 0 refused, 1 accepted and the body is the canonical encoding of what was read, 2 accepted but not canonical.
// (The reserved byte of a chunk is ignored on decode, so it is normalised to 0 before the comparison.)
static int Canonical(Type type, const Bytes& body) {
  switch (type) {
    case Type::kHello: { Hello m; if (!DecodeHello(S(body), &m)) return 0; return EncodeHello(m) == body ? 1 : 2; }
    case Type::kWelcome: { Welcome m; if (!DecodeWelcome(S(body), &m)) return 0; return EncodeWelcome(m) == body ? 1 : 2; }
    case Type::kPing: { Ping m; if (!DecodePing(S(body), &m)) return 0; return EncodePing(m) == body ? 1 : 2; }
    case Type::kChunk: {
      Chunk m;
      if (!DecodeChunk(S(body), &m)) return 0;
      Bytes norm = body;
      norm[7] = 0;
      return EncodeChunk(m) == norm ? 1 : 2;
    }
    case Type::kAck: { Ack m; if (!DecodeAck(S(body), &m)) return 0; return EncodeAck(m) == body ? 1 : 2; }
    case Type::kLeave: { Leave m; if (!DecodeLeave(S(body), &m)) return 0; return EncodeLeave(m) == body ? 1 : 2; }
    case Type::kBootReply: { BootReply m; if (!DecodeBootReply(S(body), &m)) return 0; return EncodeBootReply(m) == body ? 1 : 2; }
    default: return 0;
  }
}

static void TestFuzz() {
  Group("fuzz.datagrams");
  // 200,000 datagrams from the brief's generator (xorshift64 per byte, seed 0x9E3779B97F4A7C15, lengths 0..1500).
  // One in four starts with the valid magic and proto. So that the reasons behind the proto and the type check
  // and the body decoders are reached at all (a random length field matches the datagram's about once in 65,536),
  // one in sixteen carries the valid magic with a random proto, and one in eight of the valid ones also gets a
  // type in 1..11 and the matching length field.
  constexpr int kN = 200000;
  Rng rng(0x9E3779B97F4A7C15ull);
  int counts[size_t(Drop::kCount)] = {};
  int accepted_not_canonical = 0, accepted_bodies = 0;
  for (int i = 0; i < kN; ++i) {
    const size_t len = rng.U16() % 1501;
    Bytes d(len);
    for (auto& b : d) b = rng.Byte();
    if (len >= 5 && (i % 4 == 0 || i % 16 == 1)) {
      d[0] = 0x44; d[1] = 0x4F; d[2] = 0x41; d[3] = 0x34;
      if (i % 4 == 0) d[4] = 1;  // (i % 16 == 1 keeps the random proto byte)
    }
    if (len >= kHeaderBytes && i % 8 == 0) {
      d[5] = uint8_t(1 + d[5] % 11);
      d[6] = uint8_t((len - kHeaderBytes) & 0xFF);
      d[7] = uint8_t((len - kHeaderBytes) >> 8);
    }
    Packet p;
    Drop r = Decode(d, &p);
    if (r == Drop::kNone) {
      if (!DecodeBody(p)) {
        r = Drop::kBody;
      } else {
        ++accepted_bodies;
        if (Canonical(p.type, Bytes(p.body.begin(), p.body.end())) == 2) ++accepted_not_canonical;
      }
    }
    ++counts[unsigned(r) < unsigned(Drop::kCount) ? unsigned(r) : 0];
  }
  int sum = 0;
  std::printf("fuzz split of %d datagrams:", kN);
  for (unsigned d = 0; d < unsigned(Drop::kCount); ++d) {
    std::printf(" %s=%d", DropName(Drop(d)), counts[d]);
    sum += counts[d];
  }
  std::printf("\n");
  CHECK(sum == kN);
  // The fuzz reaches every reason Decode and the body decoders can produce (a split with a zero here is a fuzz that
  // never asked that question), and none of the two the caller owns.
  for (Drop d : {Drop::kNone, Drop::kShort, Drop::kMagic, Drop::kProto, Drop::kType, Drop::kLength, Drop::kBody, Drop::kTooLarge})
    CHECK(counts[unsigned(d)] > 100);
  CHECK(counts[unsigned(Drop::kToken)] == 0 && counts[unsigned(Drop::kSource)] == 0);
  CHECK(accepted_bodies > 100 && accepted_not_canonical == 0);
  std::printf("fuzz: %d good packets whose bodies decoded, %d of them not canonical\n", accepted_bodies, accepted_not_canonical);

  Group("fuzz.mutated");
  // 120,000 valid bodies with one to three random edits (flip, zero, 0xFF, truncate, extend, random u16): the
  // decoders see bodies that are nearly right, which random bytes almost never produce. Every body a decoder
  // accepts must be the canonical encoding of what it read (no trailing bytes, no lenient text).
  struct Seed {
    Type type;
    Bytes body;
  };
  std::vector<Seed> corpus;
  const Bytes pay5 = Pattern(5, 1), pay1200 = Pattern(1200, 2);
  Chunk c5 = {7, 1, 3, 1, S(pay5)}, c1200 = {8, 1, 1, 0, S(pay1200)};
  corpus.push_back({Type::kHello, EncodeHello(MakeHello())});
  corpus.push_back({Type::kHello, EncodeHello(BigHello())});
  corpus.push_back({Type::kWelcome, EncodeWelcome(MakeWelcome())});
  corpus.push_back({Type::kWelcome, EncodeWelcome(BigWelcome())});
  corpus.push_back({Type::kPing, EncodePing(Ping{0x01020304u})});
  corpus.push_back({Type::kChunk, EncodeChunk(c5)});
  corpus.push_back({Type::kChunk, EncodeChunk(c1200)});
  corpus.push_back({Type::kAck, EncodeAck(MakeAck())});
  corpus.push_back({Type::kLeave, EncodeLeave(MakeLeave())});
  corpus.push_back({Type::kBootReply, EncodeBootReply(MakeBootReply())});
  corpus.push_back({Type::kBootReply, EncodeBootReply(BigBootReply())});
  std::vector<int> accepted(corpus.size()), refused(corpus.size());
  int noncanonical = 0;
  Rng mut(0x9E3779B97F4A7C16ull);
  for (int i = 0; i < 120000; ++i) {
    const size_t k = size_t(i) % corpus.size();
    Bytes b = corpus[k].body;
    const unsigned edits = 1 + mut.Byte() % 3;
    for (unsigned e = 0; e < edits && !b.empty(); ++e) {
      const size_t pos = mut.U16() % b.size();
      switch (mut.Byte() % 6) {
        case 0: b[pos] ^= uint8_t(1u << (mut.Byte() % 8)); break;
        case 1: b[pos] = 0; break;
        case 2: b[pos] = 0xFF; break;
        case 3: b.resize(pos); break;
        case 4: for (unsigned n = 1 + mut.Byte() % 3; n; --n) b.push_back(mut.Byte()); break;
        default: b[pos] = mut.Byte(); if (pos + 1 < b.size()) b[pos + 1] = mut.Byte(); break;
      }
    }
    const int r = Canonical(corpus[k].type, b);
    if (r == 0) ++refused[k]; else ++accepted[k];
    if (r == 2) ++noncanonical;
  }
  CHECK(noncanonical == 0);
  bool reached = true;
  for (size_t k = 0; k < corpus.size(); ++k) {
    if (accepted[k] < 100 || refused[k] < 100) reached = false;
    std::printf("fuzz.mutated seed %zu: %d accepted, %d refused\n", k, accepted[k], refused[k]);
  }
  CHECK(reached);  // every seed was both accepted and refused often enough to mean something
}

// ---- reliable transfer ----

struct Parts {
  Bytes data;
  std::vector<Bytes> slices;
  unsigned count = 0;
};
// The message cut the way the sender must cut it: kChunkBytes per chunk, the last one shorter or equal.
static Parts MakeParts(size_t len, uint64_t seed) {
  Parts p;
  p.data = Pattern(len, seed);
  for (size_t off = 0; off < len; off += kChunkBytes)
    p.slices.emplace_back(p.data.begin() + off, p.data.begin() + std::min(len, off + kChunkBytes));
  p.count = unsigned(p.slices.size());
  return p;
}
static Chunk PartChunk(const Parts& p, uint32_t id, uint8_t type, unsigned i) {
  Chunk c;
  c.msg_id = id;
  c.msg_type = type;
  c.count = uint8_t(p.count);
  c.index = uint8_t(i);
  c.payload = S(p.slices[i]);
  return c;
}

using Due = std::vector<std::vector<uint8_t>>;
static std::vector<unsigned> IndicesOf(const Due& due) {  // the chunk indices of a Due result, in order
  std::vector<unsigned> out;
  for (const Bytes& body : due) {
    Chunk c;
    if (DecodeChunk(S(body), &c)) out.push_back(c.index);
    else out.push_back(9999);
  }
  return out;
}
static std::vector<unsigned> Range(unsigned a, unsigned b) {  // a, a+1, ..., b-1
  std::vector<unsigned> v;
  for (unsigned i = a; i < b; ++i) v.push_back(i);
  return v;
}

static Ack AckOf(uint32_t id, std::initializer_list<unsigned> chunks) {
  Ack a;
  a.msg_id = id;
  for (unsigned i : chunks) a.have[i / 64] |= 1ull << (i % 64);
  return a;
}

struct LinkResult {
  bool completed = false;
  Bytes message;
  uint8_t type = 0;
  int sends = 0, send_drops = 0, acks = 0, ack_drops = 0, rounds = 0;
  bool sender_done = false;
};
// The fake channel's contract (T3.1): a UDP-like link that drops by send count (every send_drop_every-th chunk
// send, every ack_drop_every-th ack) and, when mangle is set, delivers each round's chunks in reverse order and
// twice. It never corrupts. Every datagram goes through Encode and Decode, so the real header path is in the loop.
// The loop ends when the receiver has the message, or after 200 rounds.
static LinkResult RunLink(const Bytes& data, uint8_t type, int send_drop_every, int ack_drop_every, bool mangle) {
  LinkResult res;
  ReliableSender tx;
  ReliableReceiver rx;
  uint32_t now = 1000;
  if (!tx.Begin(77, type, S(data), now)) return res;
  for (int round = 0; round < 200 && !res.completed; ++round) {
    ++res.rounds;
    Due due = tx.Due(now, 150);
    if (mangle) {
      std::reverse(due.begin(), due.end());
      Due twice;
      for (const Bytes& b : due) { twice.push_back(b); twice.push_back(b); }
      due = twice;
    }
    for (const Bytes& body : due) {
      ++res.sends;
      if (send_drop_every && res.sends % send_drop_every == 0) { ++res.send_drops; continue; }
      const Bytes dg = Encode(Type::kChunk, 0xC0FFEEu, S(body));
      Packet pk;
      Chunk c;
      Ack a;
      if (Decode(dg, &pk) != Drop::kNone || pk.type != Type::kChunk || !DecodeChunk(pk.body, &c)) continue;
      const ReliableReceiver::Result r = rx.OnChunk(c, &a);
      if (r == ReliableReceiver::Result::kIgnored) continue;
      ++res.acks;
      if (ack_drop_every && res.acks % ack_drop_every == 0) { ++res.ack_drops; }
      else {
        const Bytes adg = Encode(Type::kAck, 0xC0FFEEu, S(EncodeAck(a)));
        Packet ap;
        Ack back;
        if (Decode(adg, &ap) == Drop::kNone && DecodeAck(ap.body, &back)) tx.OnAck(back);
      }
      if (r == ReliableReceiver::Result::kComplete) res.completed = true;
    }
    now += 150;  // the next round is one retransmit interval later
  }
  if (res.completed) res.message = rx.Take(&res.type);
  res.sender_done = tx.Done();
  return res;
}

static void TestLink() {
  Group("link.lossy");
  // 85,000 bytes = 70 chunks of 1,200 and one of 1,000: 71 chunks. Every third chunk send and every second ack is lost.
  const Bytes data = Pattern(85000, 0xABCDEF);
  const LinkResult r = RunLink(data, 9, 3, 2, false);
  CHECK(r.completed);
  CHECK(r.message == data);
  CHECK(r.type == 9);
  CHECK(r.send_drops > 20 && r.ack_drops > 20);   // the link really lost chunks and acks (non-vacuity)
  CHECK(r.sends > 71 && r.rounds >= 2);           // and the sender really retransmitted
  std::printf("link.lossy: %d rounds, %d chunk sends (%d lost), %d acks (%d lost)\n", r.rounds, r.sends, r.send_drops, r.acks, r.ack_drops);

  Group("link.mangled");  // reversed order and every chunk twice, with every fourth send and every third ack lost
  const LinkResult m = RunLink(data, 1, 4, 3, true);
  CHECK(m.completed && m.message == data && m.type == 1);
  CHECK(m.send_drops > 20 && m.ack_drops > 10 && m.sends > 71);

  Group("link.clean");  // no loss: one round, the sender sees every ack and is done
  const LinkResult c = RunLink(data, 1, 0, 0, false);
  CHECK(c.completed && c.message == data && c.rounds == 1 && c.sends == 71 && c.acks == 71);
  CHECK(c.sender_done);

  Group("link.sizes");  // the chunk-count edges end to end: 1, 1200, 1201, 2400, 2401 and the 255-chunk maximum
  for (size_t n : {size_t(1), size_t(1199), size_t(1200), size_t(1201), size_t(2399), size_t(2400), size_t(2401), size_t(306000)}) {
    const Bytes d = Pattern(n, n);
    const LinkResult x = RunLink(d, 1, 5, 2, false);
    CHECK(x.completed && x.message == d);
  }
}

static void TestSender() {
  Group("sender.chunks");
  // Chunk counts and last-chunk lengths derived by hand: ceil(n / 1200) and n - 1200 * (count - 1).
  struct Case { size_t n; unsigned count; size_t last; };
  const Case cases[] = {{1, 1, 1}, {1199, 1, 1199}, {1200, 1, 1200}, {1201, 2, 1}, {2399, 2, 1199}, {2400, 2, 1200},
                        {2401, 3, 1}, {85000, 71, 1000}, {306000, 255, 1200}};
  for (const Case& cs : cases) {
    const Bytes data = Pattern(cs.n, cs.n + 5);
    ReliableSender tx;
    CHECK(tx.Begin(0x01020304u, 7, S(data), 1000));
    const Due due = tx.Due(1000, 150);
    CHECK(due.size() == cs.count);
    Bytes joined;
    bool ok = due.size() == cs.count;
    for (size_t i = 0; ok && i < due.size(); ++i) {
      Chunk c;
      if (!DecodeChunk(S(due[i]), &c) || c.msg_id != 0x01020304u || c.msg_type != 7 || c.count != cs.count || c.index != i ||
          c.payload.size() != (i + 1 == cs.count ? cs.last : kChunkBytes)) {
        ok = false;
        break;
      }
      joined.insert(joined.end(), c.payload.begin(), c.payload.end());
    }
    CHECK(ok);
    CHECK(joined == data);
  }

  Group("sender.begin");
  {
    ReliableSender tx;
    CHECK(!tx.Active() && !tx.Done());
    CHECK(tx.Begin(1, 1, S(Pattern(306000, 1)), 0));   // 255 chunks: the accept edge
    CHECK(tx.Active() && !tx.Done());
    tx.Reset();
    CHECK(!tx.Begin(1, 1, S(Pattern(306001, 1)), 0));  // 256 chunks: refused
    CHECK(!tx.Active() && !tx.Done() && tx.Due(0, 150).empty());
    CHECK(!tx.Begin(1, 1, {}, 0));                     // an empty message has no chunks
    CHECK(!tx.Active());
    CHECK(tx.Begin(1, 1, S(Bytes(1, 0)), 0));          // one byte is a message
    tx.Reset();
    CHECK(!tx.Active() && !tx.Done() && tx.Due(5000, 150).empty());
    CHECK(!tx.TimedOut(1u << 30, 15000));              // idle never times out
  }
  Group("sender.busy");
  {
    ReliableSender tx;
    const Bytes a = Pattern(3000, 1), b = Pattern(5000, 2);
    CHECK(tx.Begin(10, 1, S(a), 100));
    CHECK(!tx.Begin(11, 2, S(b), 200));                // busy: refused
    const Due due = tx.Due(100, 150);
    const std::vector<unsigned> idx = IndicesOf(due);
    Chunk c;
    CHECK(idx == Range(0, 3) && DecodeChunk(S(due[0]), &c) && c.msg_id == 10 && c.msg_type == 1 && c.count == 3);  // the first transfer is untouched
    CHECK(tx.Active() && !tx.TimedOut(100 + 14999, 15000));
    tx.OnAck(AckOf(10, {0, 1, 2}));
    CHECK(tx.Done() && !tx.Active());
    CHECK(tx.Begin(12, 3, S(b), 300));                 // a finished transfer is replaced by the next
    CHECK(tx.Active() && !tx.Done());
    Chunk d;
    const Due next = tx.Due(300, 150);
    CHECK(next.size() == 5 && DecodeChunk(S(next[0]), &d) && d.msg_id == 12 && d.msg_type == 3 && d.count == 5);
  }
  Group("sender.owns-data");
  {
    Bytes data = Pattern(2500, 3);
    const Bytes original = data;
    ReliableSender tx;
    CHECK(tx.Begin(1, 1, S(data), 0));
    std::fill(data.begin(), data.end(), 0xEE);         // the caller reuses its buffer
    const Due due = tx.Due(0, 150);
    Bytes joined;
    for (const Bytes& body : due) {
      Chunk c;
      if (DecodeChunk(S(body), &c)) joined.insert(joined.end(), c.payload.begin(), c.payload.end());
    }
    CHECK(joined == original);
  }

  Group("sender.due");
  {
    ReliableSender tx;
    const Bytes data = Pattern(6000, 4);               // 5 chunks
    CHECK(tx.Begin(7, 1, S(data), 1000));
    CHECK(IndicesOf(tx.Due(1000, 150)) == Range(0, 5));        // never sent: everything is due at once
    CHECK(tx.Due(1000, 150).empty());                           // and not again at the same instant
    CHECK(tx.Due(1149, 150).empty());                           // 149 ms after the last send: not yet
    CHECK(IndicesOf(tx.Due(1150, 150)) == Range(0, 5));         // 150 ms: due (the boundary, both sides)
    tx.OnAck(AckOf(7, {0, 2}));
    CHECK(tx.Due(1299, 150).empty());                           // 149 ms after the send at 1150
    CHECK(IndicesOf(tx.Due(1300, 150)) == (std::vector<unsigned>{1, 3, 4}));   // only the unacknowledged
    tx.OnAck(AckOf(7, {1}));
    CHECK(tx.Due(1449, 150).empty());
    CHECK(IndicesOf(tx.Due(1450, 150)) == (std::vector<unsigned>{3, 4}));
    tx.OnAck(AckOf(7, {3, 4}));
    CHECK(tx.Done() && !tx.Active());
    CHECK(tx.Due(100000, 150).empty());                         // nothing is due once everything is acknowledged
  }
  Group("sender.due.wrap");
  {
    ReliableSender tx;
    const Bytes data = Pattern(1500, 4);
    const uint32_t t0 = 0xFFFFFF9Cu;                            // 100 ms before the tick counter wraps
    CHECK(tx.Begin(7, 1, S(data), t0));
    CHECK(IndicesOf(tx.Due(t0, 150)) == Range(0, 2));
    CHECK(tx.Due(0x00000031u, 150).empty());                    // 49 + 100 = 149 ms later
    CHECK(IndicesOf(tx.Due(0x00000032u, 150)) == Range(0, 2));  // 150 ms later
  }
  Group("sender.due.zero-time");
  {
    ReliableSender tx;
    const Bytes data = Pattern(100, 4);
    CHECK(tx.Begin(7, 1, S(data), 0));
    CHECK(tx.Due(0, 150).size() == 1);                          // a send at tick 0 counts as a send (0 is not "never")
    CHECK(tx.Due(0, 150).empty() && tx.Due(149, 150).empty());
    CHECK(tx.Due(150, 150).size() == 1);
  }

  Group("sender.timeout");
  {
    ReliableSender tx;
    CHECK(!tx.TimedOut(0, 0));
    CHECK(tx.Begin(1, 1, S(Pattern(100, 1)), 5000));
    CHECK(!tx.TimedOut(5000, 15000));
    CHECK(!tx.TimedOut(5000 + 14999, 15000));                   // begin + timeout - 1
    CHECK(tx.TimedOut(5000 + 15000, 15000));                    // begin + timeout
    CHECK(tx.TimedOut(5000 + 15001, 15000));
    tx.OnAck(AckOf(1, {0}));
    CHECK(tx.Done() && !tx.TimedOut(5000 + 99999, 15000));      // a finished transfer cannot time out
    ReliableSender w;
    CHECK(w.Begin(1, 1, S(Pattern(100, 1)), 0xFFFFFFF0u));     // the tick counter wraps inside the timeout
    CHECK(!w.TimedOut(0xFFFFFFF0u, 100));
    CHECK(!w.TimedOut(0x00000053u, 100));                       // 0x53 + 0x10 = 99 ms
    CHECK(w.TimedOut(0x00000054u, 100));                        // 100 ms
    w.Reset();
    CHECK(!w.TimedOut(0x00000054u, 100));
  }

  Group("sender.ack");
  {
    const Bytes data = Pattern(84000, 6);                       // 70 chunks: the bitmap crosses a word
    ReliableSender tx;
    CHECK(tx.Begin(5, 1, S(data), 0));
    Ack wrong = AckOf(6, {});                                   // another message's ack, everything set
    wrong.have = {~0ull, ~0ull, ~0ull, ~0ull};
    tx.OnAck(wrong);
    CHECK(tx.Active() && !tx.Done());                           // ignored
    Ack low;
    low.msg_id = 5;
    low.have = {~0ull, 0, 0, 0};                                // chunks 0..63
    tx.OnAck(low);
    CHECK(tx.Active() && !tx.Done());
    CHECK(IndicesOf(tx.Due(0, 150)) == Range(64, 70));          // the first send after an ack: only the six unacknowledged
    Ack high;
    high.msg_id = 5;
    high.have = {0, 0x1F, 0, 0};                                // chunks 64..68: one short of 70
    tx.OnAck(high);
    CHECK(tx.Active() && !tx.Done());
    Ack beyond;
    beyond.msg_id = 5;
    beyond.have = {0, 0, ~0ull, ~0ull};                         // bits far past the last chunk say nothing about chunk 69
    tx.OnAck(beyond);
    CHECK(tx.Active() && !tx.Done());
    Ack last;
    last.msg_id = 5;
    last.have = {0, 0x20, 0, 0};                                // chunk 69
    tx.OnAck(last);
    CHECK(tx.Done() && !tx.Active());
    tx.OnAck(wrong);                                            // an ack after completion changes nothing
    CHECK(tx.Done());
    tx.Reset();
    CHECK(!tx.Done() && !tx.Active());
    tx.OnAck(low);                                              // an ack with nothing in flight changes nothing
    CHECK(!tx.Done() && !tx.Active());
  }
  Group("sender.ack.partial");
  {
    ReliableSender tx;
    const Bytes data = Pattern(84000, 6);
    CHECK(tx.Begin(5, 1, S(data), 0));
    CHECK(IndicesOf(tx.Due(0, 150)) == Range(0, 70));
    Ack a;
    a.msg_id = 5;
    a.have = {0xFFFFFFFFFFFFFFFEull, 0x3F, 0, 0};              // everything but chunk 0
    tx.OnAck(a);
    CHECK(!tx.Done());
    CHECK(IndicesOf(tx.Due(150, 150)) == (std::vector<unsigned>{0}));
    tx.OnAck(AckOf(5, {0}));
    CHECK(tx.Done());
  }
}

static void TestReceiver() {
  using R = ReliableReceiver::Result;
  Group("receiver.validity");
  {
    const Bytes full(1200, 0x11), short1(1), short1199(1199), over(1201, 0x11), empty;
    Ack sentinel = AckOf(0xAAAA, {5});
    auto Try = [&](unsigned count, unsigned index, const Bytes& payload) {
      ReliableReceiver rx;
      Chunk c;
      c.msg_id = 5;
      c.msg_type = 1;
      c.count = uint8_t(count);
      c.index = uint8_t(index);
      c.payload = S(payload);
      Ack a = sentinel;
      const R r = rx.OnChunk(c, &a);
      if (r == R::kIgnored && !Same(a, sentinel)) return R(99);   // an ignored chunk leaves *ack alone
      return r;
    };
    CHECK(Try(0, 0, full) == R::kIgnored);                        // no chunks in the message
    CHECK(Try(1, 0, short1) == R::kComplete);                     // one byte, one chunk
    CHECK(Try(1, 0, full) == R::kComplete);                       // 1200 in a last chunk
    CHECK(Try(1, 0, over) == R::kIgnored);                        // 1201: over the chunk size
    CHECK(Try(1, 0, empty) == R::kIgnored);                       // empty
    CHECK(Try(3, 3, full) == R::kIgnored);                        // index == count
    CHECK(Try(3, 255, full) == R::kIgnored);
    CHECK(Try(3, 2, short1) == R::kAck);                          // index == count - 1: the last chunk may be short
    CHECK(Try(3, 1, full) == R::kAck);
    CHECK(Try(3, 1, short1199) == R::kIgnored);                   // a middle chunk must be exactly 1200
    CHECK(Try(3, 0, short1) == R::kIgnored);
    CHECK(Try(3, 2, short1199) == R::kAck);
    CHECK(Try(3, 2, over) == R::kIgnored);
    CHECK(Try(255, 254, short1) == R::kAck);                      // count == kMaxChunks is allowed
    CHECK(Try(255, 0, full) == R::kAck);
  }
  Group("receiver.invalid-first-does-not-claim");
  {
    ReliableReceiver rx;
    const Parts p = MakeParts(3000, 3);
    const Bytes empty;
    Chunk bad = PartChunk(p, 5, 1, 0);
    bad.count = 0;
    Ack a;
    CHECK(rx.OnChunk(bad, &a) == R::kIgnored);                    // an invalid chunk starts nothing ...
    CHECK(rx.OnChunk(PartChunk(p, 6, 1, 0), &a) == R::kAck);      // ... so another message begins at once
    CHECK(a.msg_id == 6);
  }
  Group("receiver.ack");
  {
    ReliableReceiver rx;
    const Parts p = MakeParts(6000, 4);                           // 5 chunks
    Ack a;
    CHECK(rx.OnChunk(PartChunk(p, 0x01020304u, 9, 3), &a) == R::kAck);
    CHECK(a.msg_id == 0x01020304u && a.have == (std::array<uint64_t, 4>{1ull << 3, 0, 0, 0}));
    CHECK(rx.OnChunk(PartChunk(p, 0x01020304u, 9, 0), &a) == R::kAck);
    CHECK(a.have[0] == 9);
    CHECK(rx.OnChunk(PartChunk(p, 0x01020304u, 9, 3), &a) == R::kAck);   // the same chunk again is acknowledged again
    CHECK(a.have[0] == 9);
  }
  Group("receiver.in-progress");
  {
    ReliableReceiver rx;
    const Parts p = MakeParts(2500, 5);                           // 3 chunks, last 100 bytes
    const Parts other = MakeParts(2500, 6);
    Ack a;
    CHECK(rx.OnChunk(PartChunk(p, 10, 2, 0), &a) == R::kAck);
    Ack before = a;
    CHECK(rx.OnChunk(PartChunk(other, 11, 2, 0), &a) == R::kIgnored);     // another msg_id while receiving
    CHECK(Same(a, before));
    Chunk bigger = PartChunk(p, 10, 2, 1);
    bigger.count = 4;                                                    // the same msg_id with another count
    CHECK(rx.OnChunk(bigger, &a) == R::kIgnored);
    Chunk retyped = PartChunk(p, 10, 3, 1);                              // the same msg_id with another type
    CHECK(rx.OnChunk(retyped, &a) == R::kIgnored);
    Chunk beyond = PartChunk(p, 10, 2, 0);
    beyond.count = 255;
    beyond.index = 254;                                                  // would be past the buffer of a 3-chunk message
    CHECK(rx.OnChunk(beyond, &a) == R::kIgnored);
    CHECK(Same(a, before));
    CHECK(rx.OnChunk(PartChunk(p, 10, 2, 1), &a) == R::kAck);
    CHECK(a.have[0] == 3);                                               // the stray chunks changed nothing
    CHECK(rx.OnChunk(PartChunk(p, 10, 2, 2), &a) == R::kComplete);
    uint8_t type = 0xEE;
    CHECK(rx.Take(&type) == p.data && type == 2);                        // intact, from the right message
  }
  Group("receiver.complete");
  {
    const Parts p = MakeParts(6000, 7);                                  // 5 chunks
    ReliableReceiver rx;
    Ack a;
    uint8_t type = 0xEE;
    for (unsigned i : {4u, 0u, 2u, 1u}) {                                // count - 1 distinct chunks, out of order
      CHECK(rx.OnChunk(PartChunk(p, 3, 4, i), &a) == R::kAck);
      CHECK(rx.Take(&type).empty() && type == 0xEE);                     // nothing to take yet, *type untouched
    }
    CHECK(a.have[0] == 0x17);                                            // 0,1,2,4
    CHECK(rx.OnChunk(PartChunk(p, 3, 4, 3), &a) == R::kComplete);
    CHECK(a.have[0] == 0x1F);
    CHECK(rx.OnChunk(PartChunk(p, 3, 4, 3), &a) == R::kIgnored);         // a duplicate after completion, before Take
    const Bytes msg = rx.Take(&type);
    CHECK(msg == p.data && type == 4);
    type = 0xEE;
    CHECK(rx.Take(&type).empty() && type == 0xEE);                       // once
    for (unsigned i = 0; i < 5; ++i) {                                   // duplicates after Take, every index
      Ack s = AckOf(1, {1});
      CHECK(rx.OnChunk(PartChunk(p, 3, 4, i), &s) == R::kIgnored);
      CHECK(Same(s, AckOf(1, {1})));
    }
    CHECK(rx.Take(nullptr).empty());
  }
  Group("receiver.complete.missing-one");
  {
    const Parts p = MakeParts(6000, 12);                                 // 5 chunks
    for (unsigned missing = 0; missing < p.count; ++missing) {           // every chunk but one, in order: not complete
      ReliableReceiver rx;
      Ack a;
      uint8_t type = 0xEE;
      bool all_ack = true;
      for (unsigned i = 0; i < p.count; ++i) {
        if (i == missing) continue;
        if (rx.OnChunk(PartChunk(p, 8, 2, i), &a) != R::kAck) all_ack = false;
      }
      CHECK(all_ack);
      CHECK(rx.Take(&type).empty() && type == 0xEE);
      CHECK(rx.OnChunk(PartChunk(p, 8, 2, missing), &a) == R::kComplete);  // the one that was missing completes it
      CHECK(rx.Take(&type) == p.data && type == 2);
    }
  }
  Group("receiver.next-message");
  {
    ReliableReceiver rx;
    const Parts p = MakeParts(1300, 8), q = MakeParts(2500, 9);
    Ack a;
    CHECK(rx.OnChunk(PartChunk(p, 1, 1, 0), &a) == R::kAck);
    CHECK(rx.OnChunk(PartChunk(p, 1, 1, 1), &a) == R::kComplete);
    CHECK(rx.OnChunk(PartChunk(q, 2, 1, 0), &a) == R::kIgnored);         // completed but not taken: another message waits
    uint8_t type = 0;
    CHECK(rx.Take(&type) == p.data);
    CHECK(rx.OnChunk(PartChunk(q, 2, 5, 0), &a) == R::kAck);            // taken: the next message starts
    CHECK(a.msg_id == 2);
    CHECK(rx.OnChunk(PartChunk(q, 2, 5, 1), &a) == R::kAck);
    CHECK(rx.OnChunk(PartChunk(q, 2, 5, 2), &a) == R::kComplete);
    CHECK(rx.Take(&type) == q.data && type == 5);
    rx.Reset();                                                           // Reset forgets: the same id is a new message
    CHECK(rx.OnChunk(PartChunk(q, 2, 5, 0), &a) == R::kAck);
    CHECK(a.have[0] == 1);
    rx.Reset();                                                           // an abandoned transfer is dropped
    CHECK(rx.Take(&type).empty());
    CHECK(rx.OnChunk(PartChunk(p, 9, 1, 0), &a) == R::kAck && a.msg_id == 9);
  }
  Group("receiver.first-copy-wins");
  {
    ReliableReceiver rx;
    const Parts p = MakeParts(2500, 10);
    Bytes forged = p.slices[0];
    forged[0] ^= 0xFF;
    Chunk dup = PartChunk(p, 4, 1, 0);
    Ack a;
    CHECK(rx.OnChunk(dup, &a) == R::kAck);
    dup.payload = S(forged);                                             // the same chunk again, other bytes
    CHECK(rx.OnChunk(dup, &a) == R::kAck);
    CHECK(rx.OnChunk(PartChunk(p, 4, 1, 1), &a) == R::kAck);
    CHECK(rx.OnChunk(PartChunk(p, 4, 1, 2), &a) == R::kComplete);
    uint8_t type = 0;
    CHECK(rx.Take(&type) == p.data);
  }
  Group("receiver.bitmap");
  {
    // 255 chunks: the bitmap spans four words, the last holding 63 bits (192..254).
    const Parts p = MakeParts(306000, 11);
    CHECK(p.count == 255);
    ReliableReceiver rx;
    Ack a;
    for (unsigned i : {0u, 63u, 64u, 127u, 128u, 191u, 192u, 254u}) CHECK(rx.OnChunk(PartChunk(p, 12, 1, i), &a) == R::kAck);
    // Derived by hand: word 0 holds chunks 0 and 63; word 1 holds 64 and 127; word 2 holds 128 and 191; word 3 holds 192 and 254.
    CHECK(a.have[0] == (1ull | (1ull << 63)));
    CHECK(a.have[1] == (1ull | (1ull << 63)));
    CHECK(a.have[2] == (1ull | (1ull << 63)));
    CHECK(a.have[3] == (1ull | (1ull << 62)));
    int completes = 0;
    for (unsigned i = 255; i-- > 0;) {                                    // the rest, from the top down
      if (i == 0 || i == 63 || i == 64 || i == 127 || i == 128 || i == 191 || i == 192 || i == 254) continue;
      const R r = rx.OnChunk(PartChunk(p, 12, 1, i), &a);
      if (r == R::kComplete) ++completes;
      else if (r != R::kAck) completes = 1000;
    }
    CHECK(completes == 1);
    CHECK(a.have[0] == ~0ull && a.have[1] == ~0ull && a.have[2] == ~0ull && a.have[3] == 0x7FFFFFFFFFFFFFFFull);
    uint8_t type = 0;
    CHECK(rx.Take(&type) == p.data && type == 1);
  }
  Group("receiver.last-length");
  for (size_t n : {size_t(2400), size_t(2401), size_t(1201), size_t(1200), size_t(1)}) {
    const Parts p = MakeParts(n, n);
    for (int last_first = 0; last_first < 2; ++last_first) {              // the short last chunk first, or last
      ReliableReceiver rx;
      Ack a;
      R r = R::kIgnored;
      for (unsigned k = 0; k < p.count; ++k) {
        const unsigned i = last_first ? p.count - 1 - k : k;
        r = rx.OnChunk(PartChunk(p, 20, 6, i), &a);
      }
      uint8_t type = 0;
      CHECK(r == R::kComplete && rx.Take(&type) == p.data && type == 6);
    }
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "-v") == 0) g_verbose = true;
  TestHeader();
  TestDrops();
  TestEncodeLimits();
  TestMessages();
  TestFuzz();
  TestLink();
  TestSender();
  TestReceiver();
  std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
