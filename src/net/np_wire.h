// src/net/np_wire.h - DOA4 netplay wire format (no SDK, no Windows). All integers little-endian.
//
// Every datagram is a 12-byte header (u32 magic, u8 proto, u8 type, u16 body bytes, u32 token) and a body whose
// length the header states EXACTLY (a datagram with fewer or more bytes than the header says is dropped as
// kLength). Decode checks, in this order, and reports the first that fails: kShort (under 12 bytes), kTooLarge
// (over kMaxDatagram), kMagic, kProto, kType (1..11), kLength. It does not check the token or the source: the
// caller owns both (kToken, kSource) and counts kBody when a DecodeX of a good packet fails.
// Every DecodeX is exact too: a body with a missing byte, a trailing byte, a text longer than its limit or a
// length that points past the end returns false and leaves *out as it was. Text fields are u16 length + bytes
// (no terminator); kMaxText bytes at most, except Hello::files (1,000). The encoders refuse (return an empty
// vector) what the decoders would refuse. Nothing here is thread-safe; the classes are driven by one thread.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace doa4::np::wire {

inline constexpr uint32_t kMagic = 0x34414F44u;  // the bytes 'D' 'O' 'A' '4' on the wire
inline constexpr uint8_t kProto = 1;             // bumped by any incompatible change
inline constexpr size_t kHeaderBytes = 12;       // u32 magic, u8 proto, u8 type, u16 body bytes, u32 token
inline constexpr size_t kMaxDatagram = 1400;     // header + body; never sent larger, dropped larger
inline constexpr size_t kChunkBytes = 1200;      // payload of one reliable chunk
inline constexpr unsigned kMaxChunks = 255;      // so a reliable message is at most 306,000 bytes
inline constexpr size_t kMaxText = 200;          // any text field

enum class Type : uint8_t {
  kHello = 1, kWelcome = 2, kPing = 3, kPong = 4, kChunk = 5, kAck = 6,
  kLeave = 7, kBye = 8, kPunch = 9, kGekko = 10, kBootReply = 11,
};
enum class Drop : uint8_t {  // why a datagram was dropped; kCount sizes the counter arrays
  kNone = 0, kShort, kMagic, kProto, kType, kLength, kBody, kToken, kSource, kTooLarge, kCount,
};
const char* DropName(Drop d);

struct Packet {
  Type type = Type::kPing;
  uint32_t token = 0;
  std::span<const uint8_t> body;  // points into the datagram
};

// Header + body; empty when header + body would exceed kMaxDatagram.
std::vector<uint8_t> Encode(Type type, uint32_t token, std::span<const uint8_t> body);
// kNone and *out filled, or the reason the datagram is dropped. Never throws, never reads past the span.
Drop Decode(std::span<const uint8_t> datagram, Packet* out);

enum class Refusal : uint8_t { kNone = 0, kProto = 1, kBuild = 2, kFiles = 3, kBusy = 4, kSession = 5 };
enum class LeaveReason : uint8_t { kLeft = 0, kDesync = 1, kTimeout = 2, kInternal = 3 };

struct Hello {  // joiner -> host (lobby), and both ways in a session process
  uint64_t build = 0;          // np::BuildIdValue() (Task 12): XXH3 of the exe and rexruntime.dll; 0 = unreadable
  std::string release;         // DOA4_RELEASE_TAG ("dev" for local builds)
  std::string files;           // np_files text: "group=hash16\n" lines, sorted
  uint8_t delay_auto = 1;
  uint8_t delay_frames = 2;    // the manual value (1..8)
  uint32_t session_token = 0;  // 0 in the lobby; the hand-off's token in a session process
};
struct Welcome {  // host -> joiner
  Refusal refusal = Refusal::kNone;
  uint64_t build = 0;
  std::string release;
  std::string text;            // the host's reason (shown when the joiner cannot compose its own advice)
  uint32_t session_token = 0;  // the random token every later packet carries (0 when refused)
};
struct Ping { uint32_t t_ms = 0; };  // kPing and kPong carry the same body
struct Chunk {  // wire: u32 msg_id, u8 msg_type, u8 count, u8 index, u8 0, u16 payload length, payload
  uint32_t msg_id = 0; uint8_t msg_type = 0; uint8_t count = 0; uint8_t index = 0;
  std::span<const uint8_t> payload;   // at most kChunkBytes; its length travels in the body, so a cut body is refused
};
struct Ack { uint32_t msg_id = 0; std::array<uint64_t, 4> have{}; };
// frame: the frame the sender's session ended at; for kDesync the frame the desync was found at (Task 15 G1: the
// receiver ends as a desync naming it). Same bytes as before: no protocol change.
struct Leave { LeaveReason reason = LeaveReason::kLeft; int32_t frame = 0; };
struct BootReply { uint32_t session_token = 0; uint8_t ok = 0; std::string text; };

std::vector<uint8_t> EncodeHello(const Hello&);          bool DecodeHello(std::span<const uint8_t>, Hello*);
std::vector<uint8_t> EncodeWelcome(const Welcome&);      bool DecodeWelcome(std::span<const uint8_t>, Welcome*);
std::vector<uint8_t> EncodePing(const Ping&);            bool DecodePing(std::span<const uint8_t>, Ping*);
std::vector<uint8_t> EncodeChunk(const Chunk&);          bool DecodeChunk(std::span<const uint8_t>, Chunk*);
std::vector<uint8_t> EncodeAck(const Ack&);              bool DecodeAck(std::span<const uint8_t>, Ack*);
std::vector<uint8_t> EncodeLeave(const Leave&);          bool DecodeLeave(std::span<const uint8_t>, Leave*);
std::vector<uint8_t> EncodeBootReply(const BootReply&);  bool DecodeBootReply(std::span<const uint8_t>, BootReply*);

inline constexpr uint8_t kMsgBoot = 1;  // the only reliable message type in phase 1: the packed hand-off

// Sends one reliable message as chunks; time is passed in (ms), so it is pure.
// States: idle -> sending (Begin) -> complete (every chunk acknowledged); Reset() goes back to idle.
// Active() is true only while sending, Done() only once complete. Times are u32 millisecond ticks and every
// elapsed time is an unsigned difference, so a tick counter that wraps is harmless.
class ReliableSender {
 public:
  // false: busy (a transfer is sending), an empty message, or more than kMaxChunks chunks (306,000 bytes is the
  // largest; 306,001 is refused). Begin from idle or complete copies the data and starts a new transfer.
  bool Begin(uint32_t msg_id, uint8_t msg_type, std::span<const uint8_t> data, uint32_t now_ms);
  // EncodeChunk bodies to (re)send now: every unacknowledged chunk when nothing was sent yet or retx_ms have
  // passed since the last send, otherwise empty. Empty unless sending.
  std::vector<std::vector<uint8_t>> Due(uint32_t now_ms, uint32_t retx_ms);
  // Merges the ack's bitmap (bits at or past the chunk count are ignored) when it names this message.
  void OnAck(const Ack& ack);
  bool Active() const;
  bool Done() const;                                       // every chunk acknowledged
  // Only while sending: true once timeout_ms have passed since Begin (>=: false at begin + timeout - 1).
  bool TimedOut(uint32_t now_ms, uint32_t timeout_ms) const;
  void Reset();

 private:
  enum class State : uint8_t { kIdle, kSending, kComplete };
  State state_ = State::kIdle;
  uint32_t msg_id_ = 0;
  uint8_t msg_type_ = 0;
  unsigned count_ = 0;                  // chunks in the message
  std::vector<uint8_t> data_;
  std::array<uint64_t, 4> acked_{};     // bit i: chunk i acknowledged
  uint32_t begin_ms_ = 0;
  uint32_t last_send_ms_ = 0;
  bool sent_ = false;                   // Due has sent at least once
};
// Reassembles one reliable message. One message at a time: chunks of another msg_id (or another count or type
// under the same msg_id) are ignored while a message is being received or waits to be taken, so a stray or
// hostile chunk cannot overwrite or resize the buffer. The caller Reset()s an abandoned transfer.
// A chunk of the message that already completed, taken or not, is acknowledged again with the full bitmap (kAck,
// never kComplete: the message is delivered once), so one lost final ack cannot leave the sender resending until
// it times out; a chunk with that msg_id but another count or type is ignored.
// A chunk is valid when 1 <= count <= kMaxChunks, index < count, 1 <= payload <= kChunkBytes, and every chunk but
// the last has exactly kChunkBytes. Receiving a chunk twice keeps the first copy and acknowledges again.
class ReliableReceiver {
 public:
  enum class Result { kIgnored, kAck, kComplete };
  Result OnChunk(const Chunk& chunk, Ack* ack);  // kAck / kComplete fill *ack (send it back); kIgnored leaves it
  std::vector<uint8_t> Take(uint8_t* msg_type);  // the completed message, once (empty when there is none)
  void Reset();

 private:
  enum class State : uint8_t { kIdle, kReceiving, kComplete, kTaken };
  State state_ = State::kIdle;
  uint32_t msg_id_ = 0;                 // of the message received, completed or taken
  uint8_t msg_type_ = 0;
  unsigned count_ = 0;
  size_t last_len_ = 0;                 // payload bytes of the last chunk (known once it arrived)
  std::vector<uint8_t> data_;           // count_ * kChunkBytes while receiving; the message once complete
  std::array<uint64_t, 4> have_{};
};

}  // namespace doa4::np::wire
