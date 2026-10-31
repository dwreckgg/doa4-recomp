// src/net/np_rules.h - pure rules of the lobby (no SDK, no Windows). Join codes and the release-channel helpers are
// ported from DOA2U (join_code.c, release_channel.c at doa2u-release 6c35c7b); the rest is new.
// Every function that fills an out parameter leaves it as it was when it returns false. Nothing here reads past
// its input, throws on bad input or keeps state.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "np_wire.h"

namespace doa4::np::rules {

// ---- Join codes (DOA2U join_code.c; R9) ----
// A code carries one IPv4 address:port as symbols of the URL-safe base64 alphabet (A-Z a-z 0-9 - _, case-sensitive),
// shuffled with a fixed key first (a 6-round Feistel network over its 36 or 48 bits) so the address cannot be read
// off the code. 6 payload symbols = address (32 bits) + port index (4 bits), the port being kPortBase + index; 8 =
// address + the full port (16 bits), for a router that rewrote the port outside that range. One CHECK SYMBOL follows
// the payload, so a code is 7 or 9 symbols: a position-weighted check in GF(2^6) (np_rules.cpp has the scheme and its
// proof) that refuses EVERY single-symbol substitution and EVERY adjacent swap of two different symbols, and almost
// every change of letter case (an upper-case code typed in lower case); the payload encoding is the one above.
inline constexpr uint16_t kPortBase = 7200;  // spec 5: DOA4's range 7200-7215
inline constexpr uint16_t kPorts = 16;
struct Endpoint {
  std::array<uint8_t, 4> ip{};
  uint16_t port = 0;
  bool operator==(const Endpoint&) const = default;
};
std::string MakeJoinCode(const Endpoint& ep);              // 7 symbols (port in 7200-7215) or 9; "" for port 0
// The code text of an already-packed value (join_code_text_of): ip << 4 | port index for 7 symbols (6 + check),
// ip << 16 | port for 9 (8 + check); "" for any other symbol count.
std::string JoinCodeTextOf(uint64_t packed, int symbols);
// a code (7 or 9 symbols, blanks anywhere ignored), "a.b.c.d" (port default_port) or "a.b.c.d:port" (surrounding
// blanks ignored). False for anything else - a wrong length, a symbol outside the alphabet, a failed check symbol,
// the address 0.0.0.0, port 0 (also a default_port of 0): the entry field then shows kBadCodeText.
bool ParseJoinCode(std::string_view text, Endpoint* out, uint16_t default_port = kPortBase);
// The entry field's text when ParseJoinCode refuses, exactly (ASCII apostrophe and hyphen).
inline constexpr const char* kBadCodeText = "That code doesn't look right - check it (capital letters matter)";
std::string EndpointText(const Endpoint& ep);              // "a.b.c.d:port" (logs only; never shown on screen)

// ---- STUN (RFC 5389 binding) ----
std::array<uint8_t, 20> StunRequest(const std::array<uint8_t, 12>& transaction);
bool IsStun(std::span<const uint8_t> datagram);  // >= 20 bytes and the magic cookie 21 12 A4 42 at bytes 4..7
// The mapped address of a binding success with this transaction id: XOR-MAPPED-ADDRESS, else MAPPED-ADDRESS (IPv4).
// The message length in the header bounds the attributes; bytes after it are ignored. A cut message, a binding
// error or request, another transaction id, an attribute whose length passes the message's end, a stray attribute
// header, an address attribute too short for IPv4, and a mapped address of 0.0.0.0 or port 0 are refused. An IPv6
// address attribute is skipped.
bool StunMapped(std::span<const uint8_t> response, const std::array<uint8_t, 12>& transaction, Endpoint* mapped);
enum class Nat : uint8_t { kNoAnswer, kOneAnswer, kCone, kSymmetric };
Nat ClassifyNat(std::span<const Endpoint> answers);  // mapped endpoints, one per STUN server that answered
// The Online tab's note under "Your code", exactly:
//   kNoAnswer  "Local network only (no answer from the STUN servers)"
//   kOneAnswer "Public address found (one STUN server answered)"
//   kCone      "Public address found"
//   kSymmetric "Your network changes ports per destination (symmetric NAT): players outside it may not reach you"
const char* NatNote(Nat nat);
// The connect-timeout text (spec 7: "Symmetric NAT on both sides / punch fails"), shown after EVERY timeout:
//   "A direct connection is not possible between your networks; relay arrives with lobbies."
//   " Also check the code you entered (capital letters matter)."
// with " (Your network changes ports per destination.)" appended after both when this side is kSymmetric.
std::string ConnectTimeoutText(Nat nat);

// ---- Input delay and pacing (spec 5) ----
// retail (notes/research/doa4_own_online.md:81): no QoS -> 10; <= 33 ms -> 2; >= 266 ms -> 10; else
// floor((rtt*2997 + 199999) / 100000) (9 at 266, 10 from 267, so retail's own 10 at 266 is a jump); then clamped 1..8,
// which makes everything from 234 ms up 8. Here: < 0 (no measurement) -> 10; 0..33 -> 2; else the formula; clamped.
int DelayFromRtt(int rtt_ms);
// frames_ahead = gekko_frames_ahead() as GekkoNet gives it (averaged over 26 frames and already halved, backend.cpp:
// 1545-1562; controller ruling F5): 0 below 1.0, else min(1000, lround(frames_ahead * 16667 / 60)) - that frame
// count spread over 60 frames
int PacerExtraUs(float frames_ahead);

// ---- Release channel (DOA2U release_channel.c) ----
// A tag is "<channel>-v<a.b.c>"; each number has at most 9 digits.
bool ParseVersion(std::string_view tag, std::array<int, 3>* out);  // "netplay-v1.2.3", "v1.2.3", "1.2.3"
int CompareVersions(std::string_view a, std::string_view b);       // <0, 0, >0; an unparsable tag sorts lowest
bool IsDevBuild(std::string_view tag);                             // unparsable ("dev") or build number 0
struct ReleasePick { std::string tag; std::map<std::string, std::string> asset_urls; };
// From GitHub's GET /repos/<owner>/<repo>/releases JSON (any order): the highest non-draft release whose tag is
// "<channel>-v<a.b.c>" and which has every required asset (by exact name). The pick's asset_urls holds exactly the
// required assets, each name -> its browser_download_url. The text must be a complete JSON array of releases;
// strings with escape sequences are not read (a release whose tag or required asset needs one is skipped).
bool PickRelease(std::string_view json, std::string_view channel, std::span<const std::string_view> required,
                 ReleasePick* out);
// Who is older, who updates, where. `theirs` may be "" (a game that sent no release tag); a tag that is not a
// version ("dev", a local build) is a self-built game.
std::string MismatchAdvice(std::string_view mine, std::string_view theirs);

// ---- Handshake verdict (host side) ----
struct Identity { uint64_t build = 0; std::string release; std::string files; };  // build 0 = unreadable
struct Verdict { wire::Refusal refusal = wire::Refusal::kNone; std::string text; };
// busy -> kBusy; a build of 0 on either side -> kBuild ("One game could not read its own build - reinstall it from
// the newest release.") - an unreadable build agrees with nothing, itself included; builds differ -> kBuild with
// MismatchAdvice; a files text with no group at all on either side -> kFiles ("The game files could not be read on
// the host / the joiner / either side."); files differ -> kFiles naming the groups. MismatchAdvice is called as
// (host.release, joiner.release). The release tags are advice only: the same build and files under another tag agree.
Verdict Judge(const Identity& host, const Identity& joiner, bool host_busy);
// Names, sorted: a group whose value differs or is missing on one side, and every group whose value is "unreadable"
// or "missing" on either side (np_files: an unreadable or missing group agrees with nothing, itself included). A text is "name=value" lines: the name ends at
// the first '=', blank lines are skipped, a line without '=' is a group with an empty value. A name that appears
// twice in one text is not valid (np_files writes each group once); which value counts is unspecified.
std::vector<std::string> DifferingGroups(std::string_view files_a, std::string_view files_b);

}  // namespace doa4::np::rules
