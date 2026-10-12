// ============================================================================
// ShmRing — the survey's option (1): POSIX shared-memory ring buffer.
// Lowest latency (one copy, no syscalls per read) and near-zero-copy transfer.
//
// Layout: 256-byte header (power-of-two ring capacity, metadata, write_pos)
// followed by ring data. Producer = audiostackd (capture thread), consumer =
// the engine. Consumers keep their own read position in-process, so multiple
// readers can each read independently.
// Default capacity 30 s — same idea as the "fixed 30-second ring buffer" in
// the engine's pipeline notes.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "audiostack/format.hpp"

namespace astk {

constexpr uint32_t kShmMagic = 0x52545341u; // "ASTR"
constexpr uint32_t kShmVersion = 1;
constexpr size_t kShmHeaderSize = 256;

struct alignas(8) ShmHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t rate;        // Hz
    uint32_t channels;
    uint32_t encoding;    // Encoding
    uint32_t capacity;    // ring data bytes (power of two)
    uint64_t write_pos;   // total bytes written by the producer (monotonic)
    uint64_t epoch_us;    // CLOCK_MONOTONIC at write_pos == 0
    uint64_t seq;
    // fields: u32*6 = 24 B (no padding, alignas(8) puts write_pos@24) + u64*3 = 24 B -> 48 B
    uint8_t  reserved[kShmHeaderSize - 48];
};
static_assert(sizeof(ShmHeader) == kShmHeaderSize);

class ShmRingWriter {
public:
    ~ShmRingWriter() { close(true); }
    bool open(const std::string& name, uint32_t capacity_bytes,
              unsigned rate, unsigned channels, Encoding enc, std::string& err);
    void write(const void* data, size_t n); // overwrites the oldest data
    uint64_t pos() const;
    void close(bool unlink_shm);

private:
    int fd_ = -1;
    void* map_ = nullptr;
    size_t map_size_ = 0;
    std::string name_;
};

class ShmRingReader {
public:
    ~ShmRingReader() { close(); }
    bool open(const std::string& name, std::string& err);
    // jump to the current write position: read only the live stream from now on
    void seek_to_now();
    // seek to an absolute position (0 == creation time)
    void seek(uint64_t pos);
    // read up to `want` bytes. On overrun (data overwritten) lost_bytes grows
    // and the reader resyncs to the newest data.
    size_t read(void* dst, size_t want);
    uint64_t available() const;
    uint64_t lost_bytes() const { return lost_; }
    const ShmHeader* header() const;
    void close();

private:
    int fd_ = -1;
    void* map_ = nullptr;
    size_t map_size_ = 0;
    uint64_t rpos_ = 0;
    uint64_t lost_ = 0;
    bool have_rpos_ = false;
};

} // namespace astk
