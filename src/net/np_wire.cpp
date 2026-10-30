// DOA4 netplay wire format and the reliable start-data transfer (see np_wire.h for the contract).
// Pure: no SDK, no Windows, no sockets. All integers are packed and read byte by byte, little-endian.

#include "np_wire.h"

#include <algorithm>
#include <utility>

namespace doa4::np::wire {
namespace {

constexpr size_t kMaxFilesText = 1000;  // Hello::files, the one text field above kMaxText
constexpr size_t kChunkFieldBytes = 10;  // u32 msg_id, u8 msg_type, u8 count, u8 index, u8 0, u16 payload length
constexpr size_t kAckBytes = 4 + 4 * 8;  // u32 msg_id, 4 x u64
constexpr uint8_t kLastType = 11;        // Type::kBootReply
constexpr uint8_t kLastRefusal = 5;      // Refusal::kSession
constexpr uint8_t kLastLeave = 3;        // LeaveReason::kInternal

// The largest Hello body (every field at its limit) must leave room for the header in one datagram.
static_assert(kHeaderBytes + 8 + (2 + kMaxText) + (2 + kMaxFilesText) + 1 + 1 + 4 <= kMaxDatagram);

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>* out) : out_(out) {}
  void U8(uint8_t v) { out_->push_back(v); }
  void U16(uint16_t v) {
    U8(uint8_t(v & 0xFF));
    U8(uint8_t(v >> 8));
  }
  void U32(uint32_t v) {
    U16(uint16_t(v & 0xFFFF));
    U16(uint16_t(v >> 16));
  }
  void U64(uint64_t v) {
    U32(uint32_t(v & 0xFFFFFFFFu));
    U32(uint32_t(v >> 32));
  }
  void Bytes(std::span<const uint8_t> b) { out_->insert(out_->end(), b.begin(), b.end()); }
  // u16 length + bytes; false (and nothing written) above the limit.
  bool Text(const std::string& s, size_t limit) {
    if (s.size() > limit) return false;
    U16(uint16_t(s.size()));
    out_->insert(out_->end(), s.begin(), s.end());
    return true;
  }

 private:
  std::vector<uint8_t>* out_;
};

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> in) : in_(in) {}
  size_t Left() const { return in_.size() - pos_; }
  bool AtEnd() const { return pos_ == in_.size(); }
  bool U8(uint8_t* v) {
    if (Left() < 1) return false;
    *v = in_[pos_++];
    return true;
  }
  bool U16(uint16_t* v) {
    uint8_t lo = 0, hi = 0;
    if (!U8(&lo) || !U8(&hi)) return false;
    *v = uint16_t(lo | (hi << 8));
    return true;
  }
  bool U32(uint32_t* v) {
    uint16_t lo = 0, hi = 0;
    if (!U16(&lo) || !U16(&hi)) return false;
    *v = uint32_t(lo) | (uint32_t(hi) << 16);
    return true;
  }
  bool U64(uint64_t* v) {
    uint32_t lo = 0, hi = 0;
    if (!U32(&lo) || !U32(&hi)) return false;
    *v = uint64_t(lo) | (uint64_t(hi) << 32);
    return true;
  }
  bool Bytes(size_t n, std::span<const uint8_t>* out) {
    if (n > Left()) return false;
    *out = in_.subspan(pos_, n);
    pos_ += n;
    return true;
  }
  // u16 length + bytes; false above the limit or when the bytes are not all there.
  bool Text(size_t limit, std::string* out) {
    uint16_t n = 0;
    if (!U16(&n) || n > limit) return false;
    std::span<const uint8_t> b;
    if (!Bytes(n, &b)) return false;
    out->assign(b.begin(), b.end());
    return true;
  }

 private:
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
};

bool BitTest(const std::array<uint64_t, 4>& m, unsigned i) { return ((m[i / 64] >> (i % 64)) & 1u) != 0; }
void BitSet(std::array<uint64_t, 4>* m, unsigned i) { (*m)[i / 64] |= uint64_t(1) << (i % 64); }
bool BitsAll(const std::array<uint64_t, 4>& m, unsigned n) {  // bits 0 .. n-1 all set
  for (unsigned i = 0; i < n; ++i)
    if (!BitTest(m, i)) return false;
  return true;
}

}  // namespace

const char* DropName(Drop d) {
  switch (d) {
    case Drop::kNone: return "none";
    case Drop::kShort: return "short";
    case Drop::kMagic: return "magic";
    case Drop::kProto: return "proto";
    case Drop::kType: return "type";
    case Drop::kLength: return "length";
    case Drop::kBody: return "body";
    case Drop::kToken: return "token";
    case Drop::kSource: return "source";
    case Drop::kTooLarge: return "too-large";
    case Drop::kCount: break;
  }
  return "?";
}

std::vector<uint8_t> Encode(Type type, uint32_t token, std::span<const uint8_t> body) {
  const uint8_t t = uint8_t(type);
  if (t < 1 || t > kLastType) return {};
  if (body.size() > kMaxDatagram - kHeaderBytes) return {};
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + body.size());
  Writer w(&out);
  w.U32(kMagic);
  w.U8(kProto);
  w.U8(t);
  w.U16(uint16_t(body.size()));
  w.U32(token);
  w.Bytes(body);
  return out;
}

Drop Decode(std::span<const uint8_t> datagram, Packet* out) {
  if (datagram.size() < kHeaderBytes) return Drop::kShort;
  if (datagram.size() > kMaxDatagram) return Drop::kTooLarge;
  Reader r(datagram);
  uint32_t magic = 0, token = 0;
  uint8_t proto = 0, type = 0;
  uint16_t body_bytes = 0;
  if (!r.U32(&magic) || !r.U8(&proto) || !r.U8(&type) || !r.U16(&body_bytes) || !r.U32(&token)) return Drop::kShort;
  if (magic != kMagic) return Drop::kMagic;
  if (proto != kProto) return Drop::kProto;
  if (type < 1 || type > kLastType) return Drop::kType;
  if (body_bytes != datagram.size() - kHeaderBytes) return Drop::kLength;
  out->type = Type(type);
  out->token = token;
  out->body = datagram.subspan(kHeaderBytes, body_bytes);
  return Drop::kNone;
}

// ---- message bodies ----

std::vector<uint8_t> EncodeHello(const Hello& h) {
  std::vector<uint8_t> out;
  Writer w(&out);
  w.U64(h.build);
  if (!w.Text(h.release, kMaxText) || !w.Text(h.files, kMaxFilesText)) return {};
  w.U8(h.delay_auto);
  w.U8(h.delay_frames);
  w.U32(h.session_token);
  return out;
}
bool DecodeHello(std::span<const uint8_t> body, Hello* out) {
  Reader r(body);
  Hello h;
  if (!r.U64(&h.build) || !r.Text(kMaxText, &h.release) || !r.Text(kMaxFilesText, &h.files) || !r.U8(&h.delay_auto) ||
      !r.U8(&h.delay_frames) || !r.U32(&h.session_token) || !r.AtEnd())
    return false;
  *out = std::move(h);
  return true;
}

std::vector<uint8_t> EncodeWelcome(const Welcome& m) {
  if (uint8_t(m.refusal) > kLastRefusal) return {};
  std::vector<uint8_t> out;
  Writer w(&out);
  w.U8(uint8_t(m.refusal));
  w.U64(m.build);
  if (!w.Text(m.release, kMaxText) || !w.Text(m.text, kMaxText)) return {};
  w.U32(m.session_token);
  return out;
}
bool DecodeWelcome(std::span<const uint8_t> body, Welcome* out) {
  Reader r(body);
  Welcome m;
  uint8_t refusal = 0;
  if (!r.U8(&refusal) || refusal > kLastRefusal || !r.U64(&m.build) || !r.Text(kMaxText, &m.release) ||
      !r.Text(kMaxText, &m.text) || !r.U32(&m.session_token) || !r.AtEnd())
    return false;
  m.refusal = Refusal(refusal);
  *out = std::move(m);
  return true;
}

std::vector<uint8_t> EncodePing(const Ping& m) {
  std::vector<uint8_t> out;
  Writer w(&out);
  w.U32(m.t_ms);
  return out;
}
bool DecodePing(std::span<const uint8_t> body, Ping* out) {
  Reader r(body);
  Ping m;
  if (!r.U32(&m.t_ms) || !r.AtEnd()) return false;
  *out = m;
  return true;
}

std::vector<uint8_t> EncodeChunk(const Chunk& c) {
  if (c.payload.size() > kChunkBytes) return {};
  std::vector<uint8_t> out;
  out.reserve(kChunkFieldBytes + c.payload.size());
  Writer w(&out);
  w.U32(c.msg_id);
  w.U8(c.msg_type);
  w.U8(c.count);
  w.U8(c.index);
  w.U8(0);
  w.U16(uint16_t(c.payload.size()));
  w.Bytes(c.payload);
  return out;
}
bool DecodeChunk(std::span<const uint8_t> body, Chunk* out) {
  Reader r(body);
  Chunk c;
  uint8_t reserved = 0;  // written as 0, ignored on decode
  uint16_t length = 0;
  if (!r.U32(&c.msg_id) || !r.U8(&c.msg_type) || !r.U8(&c.count) || !r.U8(&c.index) || !r.U8(&reserved) || !r.U16(&length) ||
      length > kChunkBytes || !r.Bytes(length, &c.payload) || !r.AtEnd())
    return false;
  *out = c;
  return true;
}

std::vector<uint8_t> EncodeAck(const Ack& a) {
  std::vector<uint8_t> out;
  out.reserve(kAckBytes);
  Writer w(&out);
  w.U32(a.msg_id);
  for (uint64_t word : a.have) w.U64(word);
  return out;
}
bool DecodeAck(std::span<const uint8_t> body, Ack* out) {
  Reader r(body);
  Ack a;
  if (!r.U32(&a.msg_id)) return false;
  for (uint64_t& word : a.have)
    if (!r.U64(&word)) return false;
  if (!r.AtEnd()) return false;
  *out = a;
  return true;
}

std::vector<uint8_t> EncodeLeave(const Leave& l) {
  if (uint8_t(l.reason) > kLastLeave) return {};
  std::vector<uint8_t> out;
  Writer w(&out);
  w.U8(uint8_t(l.reason));
  w.U32(uint32_t(l.frame));
  return out;
}
bool DecodeLeave(std::span<const uint8_t> body, Leave* out) {
  Reader r(body);
  uint8_t reason = 0;
  uint32_t frame = 0;
  if (!r.U8(&reason) || reason > kLastLeave || !r.U32(&frame) || !r.AtEnd()) return false;
  out->reason = LeaveReason(reason);
  out->frame = int32_t(frame);
  return true;
}

std::vector<uint8_t> EncodeBootReply(const BootReply& m) {
  std::vector<uint8_t> out;
  Writer w(&out);
  w.U32(m.session_token);
  w.U8(m.ok);
  if (!w.Text(m.text, kMaxText)) return {};
  return out;
}
bool DecodeBootReply(std::span<const uint8_t> body, BootReply* out) {
  Reader r(body);
  BootReply m;
  if (!r.U32(&m.session_token) || !r.U8(&m.ok) || !r.Text(kMaxText, &m.text) || !r.AtEnd()) return false;
  *out = std::move(m);
  return true;
}

// ---- reliable transfer ----

bool ReliableSender::Begin(uint32_t msg_id, uint8_t msg_type, std::span<const uint8_t> data, uint32_t now_ms) {
  if (state_ == State::kSending) return false;
  const size_t whole = data.size() / kChunkBytes;
  const size_t chunks = whole + (data.size() % kChunkBytes != 0 ? 1 : 0);
  if (chunks < 1 || chunks > kMaxChunks) return false;
  state_ = State::kSending;
  msg_id_ = msg_id;
  msg_type_ = msg_type;
  count_ = unsigned(chunks);
  data_.assign(data.begin(), data.end());
  acked_ = {};
  begin_ms_ = now_ms;
  last_send_ms_ = 0;
  sent_ = false;
  return true;
}

std::vector<std::vector<uint8_t>> ReliableSender::Due(uint32_t now_ms, uint32_t retx_ms) {
  std::vector<std::vector<uint8_t>> out;
  if (state_ != State::kSending) return out;
  if (sent_ && uint32_t(now_ms - last_send_ms_) < retx_ms) return out;
  for (unsigned i = 0; i < count_; ++i) {
    if (BitTest(acked_, i)) continue;
    const size_t off = size_t(i) * kChunkBytes;
    const size_t n = std::min(kChunkBytes, data_.size() - off);
    Chunk c;
    c.msg_id = msg_id_;
    c.msg_type = msg_type_;
    c.count = uint8_t(count_);
    c.index = uint8_t(i);
    c.payload = std::span<const uint8_t>(data_.data() + off, n);
    out.push_back(EncodeChunk(c));
  }
  last_send_ms_ = now_ms;
  sent_ = true;
  return out;
}

void ReliableSender::OnAck(const Ack& ack) {
  if (state_ != State::kSending || ack.msg_id != msg_id_) return;
  for (size_t w = 0; w < acked_.size(); ++w) acked_[w] |= ack.have[w];
  if (BitsAll(acked_, count_)) {
    state_ = State::kComplete;
    std::vector<uint8_t>().swap(data_);  // nothing is resent any more
  }
}

bool ReliableSender::Active() const { return state_ == State::kSending; }
bool ReliableSender::Done() const { return state_ == State::kComplete; }

bool ReliableSender::TimedOut(uint32_t now_ms, uint32_t timeout_ms) const {
  return state_ == State::kSending && uint32_t(now_ms - begin_ms_) >= timeout_ms;
}

void ReliableSender::Reset() {
  state_ = State::kIdle;
  msg_id_ = 0;
  msg_type_ = 0;
  count_ = 0;
  std::vector<uint8_t>().swap(data_);
  acked_ = {};
  begin_ms_ = 0;
  last_send_ms_ = 0;
  sent_ = false;
}

ReliableReceiver::Result ReliableReceiver::OnChunk(const Chunk& chunk, Ack* ack) {
  // The chunk itself must be well formed, whatever state the receiver is in.
  if (chunk.count < 1 || chunk.count > kMaxChunks || chunk.index >= chunk.count) return Result::kIgnored;
  const size_t n = chunk.payload.size();
  if (n < 1 || n > kChunkBytes) return Result::kIgnored;
  const bool last = chunk.index == chunk.count - 1;
  if (!last && n != kChunkBytes) return Result::kIgnored;  // only the last chunk may be short

  switch (state_) {
    case State::kIdle:
      break;
    case State::kReceiving:  // the one message in progress; anything else is a stray
      if (chunk.msg_id != msg_id_ || chunk.count != count_ || chunk.msg_type != msg_type_) return Result::kIgnored;
      break;
    case State::kComplete:  // waiting to be taken: no second delivery, no second message
      return Result::kIgnored;
    case State::kTaken:
      if (chunk.msg_id == msg_id_) return Result::kIgnored;  // a duplicate of the message already delivered
      break;
  }
  if (state_ != State::kReceiving) {  // the first chunk of a new message
    state_ = State::kReceiving;
    msg_id_ = chunk.msg_id;
    msg_type_ = chunk.msg_type;
    count_ = chunk.count;
    last_len_ = 0;
    data_.assign(size_t(count_) * kChunkBytes, 0);
    have_ = {};
  }
  if (!BitTest(have_, chunk.index)) {  // a repeated chunk keeps the first copy
    std::copy(chunk.payload.begin(), chunk.payload.end(), data_.begin() + size_t(chunk.index) * kChunkBytes);
    BitSet(&have_, chunk.index);
    if (last) last_len_ = n;
  }
  ack->msg_id = msg_id_;
  ack->have = have_;
  if (!BitsAll(have_, count_)) return Result::kAck;
  data_.resize(size_t(count_ - 1) * kChunkBytes + last_len_);
  state_ = State::kComplete;
  return Result::kComplete;
}

std::vector<uint8_t> ReliableReceiver::Take(uint8_t* msg_type) {
  if (state_ != State::kComplete) return {};
  if (msg_type) *msg_type = msg_type_;
  state_ = State::kTaken;  // the id stays: a late duplicate of this message is not a new message
  return std::move(data_);
}

void ReliableReceiver::Reset() {
  state_ = State::kIdle;
  msg_id_ = 0;
  msg_type_ = 0;
  count_ = 0;
  last_len_ = 0;
  std::vector<uint8_t>().swap(data_);
  have_ = {};
}

}  // namespace doa4::np::wire
