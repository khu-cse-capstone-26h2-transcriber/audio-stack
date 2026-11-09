// ============================================================================
// ASTK wire protocol — UNIX-domain-socket frame format between the audio
// stack (daemon) and the Whisper engine. Spec: docs/PROTOCOL.md
//
// Frame = 24-byte header + payload. All integers little-endian.
//   offset  size  field
//   0       4     magic   'A','S','T','K' (0x4B535441)
//   4       1     version (1)
//   5       1     type    (MsgType)
//   6       2     flags   (FrameFlags)
//   8       4     seq     (32-bit counter maintained by the sender)
//   12      8     t_us    (CLOCK_MONOTONIC microseconds, capture instant for AUDIO)
//   20      4     payload_len (<= 1 MiB)
//   24      n     payload
//
// The AUDIO payload is raw S16_LE / mono / 16 kHz little-endian PCM.
// JSON frames (HELLO/RESULT/...) carry the whole payload as UTF-8 JSON.
// ============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace astk {

constexpr uint32_t kMagic       = 0x4B535441u; // "ASTK"
constexpr uint8_t  kProtoVersion = 1;
constexpr size_t   kHeaderSize   = 24;
constexpr uint32_t kMaxPayload   = 1u << 20;   // 1 MiB

enum MsgType : uint8_t {
    MSG_HELLO         = 0x01, // daemon -> engine  JSON: format/session/device metadata
    MSG_GOODBYE       = 0x02, // both directions   JSON: reason
    MSG_PING          = 0x03, // both directions   empty payload
    MSG_PONG          = 0x04, // both directions   empty payload
    MSG_AUDIO         = 0x10, // daemon -> engine  PCM s16le mono 16k
    MSG_SESSION_BEGIN = 0x11, // daemon -> engine  JSON: {"session_id":...}
    MSG_SESSION_END   = 0x12, // daemon -> engine  JSON: {"session_id":...}
    MSG_HEARTBEAT     = 0x13, // daemon -> engine  JSON: level/stats
    MSG_CTRL          = 0x14, // daemon -> engine  JSON: control events (xrun etc.)
    MSG_RESULT        = 0x20, // engine -> daemon  JSON: {"type":"partial|final",...}
    MSG_ENGINE_STATUS = 0x21, // engine -> daemon  JSON: model/state
    MSG_ERROR         = 0x2F, // both directions   JSON: {"code":...,"msg":...}
};

enum FrameFlags : uint16_t {
    kFlagNone    = 0,
    kFlagSilence = 1u << 0, // below the daemon energy gate (usable as a VAD hint)
    kFlagCached  = 1u << 1, // audio replayed from the daemon cache after (re)connect
    kFlagFinal   = 1u << 2, // last chunk of a session segment
};

struct Frame {
    uint8_t  type = 0;
    uint16_t flags = 0;
    uint32_t seq = 0;
    uint64_t t_us = 0;
    std::vector<uint8_t> payload;

    std::string payload_str() const {
        return std::string(payload.begin(), payload.end());
    }
};

// ---- little-endian helpers (serialization free of alignment/padding concerns)
inline void put_u16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void put_u64(uint8_t* p, uint64_t v) { put_u32(p, uint32_t(v)); put_u32(p + 4, uint32_t(v >> 32)); }

inline uint16_t get_u16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
inline uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t get_u64(const uint8_t* p) { return uint64_t(get_u32(p)) | (uint64_t(get_u32(p + 4)) << 32); }

inline uint64_t now_us() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

inline std::vector<uint8_t> encode_frame(const Frame& f) {
    std::vector<uint8_t> out(kHeaderSize + f.payload.size());
    uint8_t* h = out.data();
    put_u32(h + 0, kMagic);
    h[4] = kProtoVersion;
    h[5] = f.type;
    put_u16(h + 6, f.flags);
    put_u32(h + 8, f.seq);
    put_u64(h + 12, f.t_us);
    put_u32(h + 20, uint32_t(f.payload.size()));
    if (!f.payload.empty())
        std::memcpy(h + kHeaderSize, f.payload.data(), f.payload.size());
    return out;
}

// Decode the 24-byte header. Validates magic/version/payload length.
inline bool decode_header(const uint8_t* h, Frame& f) {
    if (get_u32(h + 0) != kMagic) return false;
    if (h[4] != kProtoVersion) return false;
    f.type = h[5];
    f.flags = get_u16(h + 6);
    f.seq = get_u32(h + 8);
    f.t_us = get_u64(h + 12);
    uint32_t len = get_u32(h + 20);
    if (len > kMaxPayload) return false;
    return true;
}

// ---- tiny JSON helpers (escaping only; payloads are passed through unparsed) --
inline void json_escape(const std::string& in, std::string& out) {
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (uint8_t(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", uint8_t(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

inline std::string json_str(const std::string& s) {
    std::string out = "\"";
    json_escape(s, out);
    out += "\"";
    return out;
}

namespace detail {
// Reads the JSON string starting at s[i] == '"' and leaves i after the closing
// quote. Returns -1 if unterminated, 1 if it holds a \u escape (not decoded;
// `out` is then incomplete), 0 otherwise. `out` may be null to skip.
inline int json_read_string(const std::string& s, size_t& i, std::string* out) {
    int rc = 0;
    for (++i; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') { ++i; return rc; }
        if (c != '\\') { if (out) out->push_back(c); continue; }
        if (++i >= s.size()) return -1;
        switch (s[i]) {
            case 'n': if (out) out->push_back('\n'); break;
            case 'r': if (out) out->push_back('\r'); break;
            case 't': if (out) out->push_back('\t'); break;
            case 'b': if (out) out->push_back('\b'); break;
            case 'f': if (out) out->push_back('\f'); break;
            case 'u': rc = 1; break;
            default:  if (out) out->push_back(s[i]); break; // " \ /
        }
    }
    return -1;
}
} // namespace detail

// Looks up a string member of the top-level JSON object (nested objects and
// arrays are skipped). Used only for the few fields the daemon itself acts on
// (RESULT/ENGINE_STATUS "session_id", "event"); payloads are still forwarded
// verbatim. Returns false if the key is missing, not a string, or malformed.
inline bool json_top_string(const std::string& s, const std::string& key, std::string& out) {
    size_t i = s.find_first_not_of(" \t\r\n");
    if (i == std::string::npos || s[i] != '{') return false;
    int depth = 0;
    bool expect_key = false;
    auto skip_ws = [&] {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    };
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"') {
            if (depth == 1 && expect_key) {
                std::string k;
                const int rk = detail::json_read_string(s, i, &k);
                if (rk < 0) return false;
                skip_ws();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                skip_ws();
                expect_key = false;
                if (rk == 0 && k == key) {
                    if (i >= s.size() || s[i] != '"') return false;
                    std::string v;
                    if (detail::json_read_string(s, i, &v) != 0) return false;
                    out = std::move(v);
                    return true;
                }
                continue;
            }
            if (detail::json_read_string(s, i, nullptr) < 0) return false;
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
            if (depth == 1) expect_key = true;
        } else if (c == '}' || c == ']') {
            if (--depth <= 0) return false;
        } else if (c == ',' && depth == 1) {
            expect_key = true;
        }
        ++i;
    }
    return false;
}

} // namespace astk
