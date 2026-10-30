// src/net/np_handoff.h - what a session carries across the restart (DOA2U netplay_handoff.c, DOA4 fields).
// Once two players are connected the host decides the session (seed, input delay, its save, the guest-visible
// settings) and both games restart into it: each writes its side's hand-off to disk, relaunches, and the new
// process reads it before any guest code runs. The joiner receives the host's bytes over the reliable transfer, so
// every name, length and count in them is untrusted: a joiner writes files named and sized by a remote host.
// Pure (no SDK, no Windows, no files, no sockets); nothing here reads past its input or throws on bad input.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "np_rules.h"

namespace doa4::np::handoff {

// Layout (little-endian): "D4HO", u16 version (1), u64 build, text release, u8 role, u8 delay, u16 local_port,
// u8 peer ip[4], u16 peer port, u32 session_token, u64 seed, text guest_settings (u32 length), u32 file count,
// then per file: text name (u16 length), u32 data length, data. Every length checked before use; the bytes must
// end exactly where the last file does.
inline constexpr char kSavePrefix[] = "B13EBABEBABEBABE/544307D1/";  // the profile XUID (a constant of the SDK) / DOA4's title id
inline constexpr size_t kMaxFiles = 16;
inline constexpr size_t kMaxFileBytes = 256 * 1024;     // the real save's largest file is 81,920 bytes
inline constexpr size_t kMaxPackedBytes = 300000;       // below the reliable transfer's 306,000 (np_wire kMaxChunks)
inline constexpr size_t kMaxNameBytes = 120;
// Limits Task 4 adds beyond the brief's (an honest hand-off never comes near them):
inline constexpr size_t kMaxNameComponents = 6;         // the brief's "at most 6 components" of a name
inline constexpr size_t kMaxReleaseBytes = 200;         // the same limit as a release text on the wire (Hello, Welcome)
inline constexpr size_t kMaxGuestBytes = 16384;         // the guest settings text: about 40 short lines
static_assert(kMaxPackedBytes <= size_t(wire::kMaxChunks) * wire::kChunkBytes, "a hand-off must fit one reliable message");
static_assert(kMaxReleaseBytes == wire::kMaxText, "a release text is limited as on the wire");

enum class Role : uint8_t { kHost = 0, kJoiner = 1 };
struct File { std::string name; std::vector<uint8_t> data; };
struct Handoff {
  uint64_t build = 0;
  std::string release;
  Role role = Role::kHost;
  uint8_t delay = 2;            // frames, 1..8
  uint16_t local_port = 0;      // this side's UDP port before the restart (the session binds it again); non-zero
  rules::Endpoint peer;         // the other side as this side last heard it; address not 0.0.0.0, port non-zero
  uint32_t session_token = 0;   // non-zero
  uint64_t seed = 0;            // the session seed (np_session SessionSeed)
  std::string guest_settings;   // CaptureGuestSettings text
  std::vector<File> files;      // the host's save, names under kSavePrefix
};

// True if a joiner may write a file of this name below its session folder: starts with kSavePrefix; components of
// [A-Za-z0-9._-] only; no empty, "." or ".." component; no trailing dot; no Windows device name (CON, PRN, AUX, NUL,
// COM1-9, LPT1-9, with or without an extension; COM0 and LPT0 too, which newer Windows lists as reserved);
// at most 6 components; at most kMaxNameBytes.
// (A component with a trailing dot or space is another file on Windows: "a." is "a". The name character set has no
// ':' (alternate data streams), '\\', '~' (8.3 names), wildcard or control character, and the prefix is
// case-sensitive.)
bool NameOk(std::string_view name);

// The bytes of `h`; empty if a limit is broken, the packed total included. What breaks one: more than kMaxFiles
// files; a file over kMaxFileBytes or a name NameOk refuses; two names that are one file on Windows (equal but for
// the case of a letter) or a name that is also a folder of another (a/b with a/b/c); a release over
// kMaxReleaseBytes or guest settings over kMaxGuestBytes; a release that is not printable ASCII (0x20..0x7E); a role
// that is neither; a delay outside 1..8; a zero session token, local port or peer port; the peer address 0.0.0.0;
// guest settings text that ApplyGuestSettings would refuse (empty, a malformed line, a missing or unknown name, a
// trust-class setting off its default: the host fails early); a packed total over kMaxPackedBytes.
std::vector<uint8_t> Pack(const Handoff& h);
// "" (and *out filled) or why not (and *out empty). Refuses exactly what Pack refuses, and bytes that are cut short,
// have a wrong magic or another version, or have bytes left after the last file; whatever it accepts, Pack gives
// back the same bytes. The size is checked before anything is read, every length before the bytes it announces.
std::string Unpack(std::span<const uint8_t> bytes, Handoff* out);

// Guest-visible settings (R7). The names, in a fixed order: the SDK cvars of Task 4's census, then every
// tweaks::AllTweaks() cvar.
const std::vector<std::string>& GuestSettingNames();
// The TRUST class (fix round 1): carried settings whose non-default value weakens a boundary of the PC that applies
// it (the guest-writable game folder, patches loaded from disk, debug monitors, memory protection, host thread
// priority and affinity, a remote command line, a remote heap size). They are forced to their default on both
// sides: Capture writes the default whatever the host has, and an incoming text that carries another value is
// refused by Apply, Pack and Unpack with "guest setting <name> may not differ from its default". The default is the
// text the cvar registry prints ("true", "false", a decimal number, the string itself).
struct TrustedSetting {
  std::string name;
  std::string default_text;
};
const std::vector<TrustedSetting>& TrustedSettings();  // in GuestSettingNames() order

// "name=value\n" in list order, one query per name except the trust-class ones (written at their default, not asked
// for). Empty if a value holds a newline or a carriage return (no line can carry it; the session is not started
// with settings that cannot be handed over).
std::string CaptureGuestSettings(const std::function<std::string(const std::string& name)>& query);
// Sets every listed name through `set` (which returns false if the value is refused); "" or the first failure.
// A name missing from `text` or not in GuestSettingNames() is a failure.
// The whole text is checked before anything is set (a refused text sets nothing): every line is "name=value\n"
// (the name ends at the first '='; the value is the rest, empty allowed), a known name appears once, every listed
// name appears, no carriage return, and a trust-class setting is at its default. The sets then go in list order,
// whatever order the lines came in (the trust-class ones too, at their default: that is how the joiner's own value is
// forced), and stop at the first one `set` refuses (the settings already set stay set).
std::string ApplyGuestSettings(std::string_view text,
                               const std::function<bool(const std::string& name, const std::string& value)>& set);

}  // namespace doa4::np::handoff
