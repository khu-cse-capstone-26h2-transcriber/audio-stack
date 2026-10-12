#include "audiostack/shm_ring.hpp"
#include "audiostack/log.hpp"
#include "audiostack/protocol.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace astk {

static inline uint64_t load_u64(const volatile uint64_t* p) {
    return __atomic_load_n((const uint64_t*)p, __ATOMIC_ACQUIRE);
}
static inline void store_u64(volatile uint64_t* p, uint64_t v) {
    __atomic_store_n((uint64_t*)p, v, __ATOMIC_RELEASE);
}

static uint32_t round_pow2(uint32_t v) {
    uint32_t p = 4096;
    while (p < v) p <<= 1;
    return p;
}

// ------------------------------------------------------------- writer -----
bool ShmRingWriter::open(const std::string& name, uint32_t capacity_bytes,
                         unsigned rate, unsigned channels, Encoding enc, std::string& err) {
    close(true);
    uint32_t cap = round_pow2(capacity_bytes);
    map_size_ = kShmHeaderSize + cap;
    fd_ = shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0) {
        err = "shm_open(" + name + "): " + std::strerror(errno);
        return false;
    }
    if (ftruncate(fd_, off_t(map_size_)) < 0) {
        err = std::string("ftruncate: ") + std::strerror(errno);
        close(true);
        return false;
    }
    map_ = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (map_ == MAP_FAILED) {
        err = std::string("mmap: ") + std::strerror(errno);
        map_ = nullptr;
        close(true);
        return false;
    }
    std::memset(map_, 0, map_size_);
    auto* h = (ShmHeader*)map_;
    h->magic = kShmMagic;
    h->version = kShmVersion;
    h->rate = rate;
    h->channels = channels;
    h->encoding = uint32_t(enc);
    h->capacity = cap;
    store_u64(&h->write_pos, 0);
    h->epoch_us = now_us();
    h->seq = 0;
    name_ = name;
    LOG_INFO("shm ring ready: name=%s capacity=%u bytes (~%.1f s of 16k mono s16)",
             name.c_str(), cap, double(cap) / (rate * channels * 2));
    return true;
}

void ShmRingWriter::write(const void* data, size_t n) {
    if (!map_ || n == 0) return;
    auto* h = (ShmHeader*)map_;
    const uint32_t cap = h->capacity;
    uint64_t wp = load_u64(&h->write_pos);
    const uint8_t* src = (const uint8_t*)data;
    if (n > cap) { // keep only the newest portion
        src += n - cap;
        n = cap;
    }
    uint8_t* ring = (uint8_t*)map_ + kShmHeaderSize;
    uint32_t off = uint32_t(wp & (cap - 1));
    uint32_t first = uint32_t(n) < (cap - off) ? uint32_t(n) : (cap - off);
    std::memcpy(ring + off, src, first);
    if (n > first) std::memcpy(ring, src + first, n - first);
    store_u64(&h->write_pos, wp + n);
    h->seq++;
}

uint64_t ShmRingWriter::pos() const {
    if (!map_) return 0;
    return load_u64(&((ShmHeader*)map_)->write_pos);
}

void ShmRingWriter::close(bool unlink_shm) {
    if (map_) { munmap(map_, map_size_); map_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (unlink_shm && !name_.empty()) shm_unlink(name_.c_str());
}

// ------------------------------------------------------------- reader -----
bool ShmRingReader::open(const std::string& name, std::string& err) {
    close();
    fd_ = shm_open(name.c_str(), O_RDONLY, 0);
    if (fd_ < 0) {
        err = "shm_open(" + name + "): " + std::strerror(errno);
        return false;
    }
    off_t sz = lseek(fd_, 0, SEEK_END);
    lseek(fd_, 0, SEEK_SET);
    if (sz < off_t(kShmHeaderSize + 4096)) {
        err = "shm too small";
        close();
        return false;
    }
    map_size_ = size_t(sz);
    map_ = mmap(nullptr, map_size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (map_ == MAP_FAILED) {
        err = std::string("mmap: ") + std::strerror(errno);
        map_ = nullptr;
        close();
        return false;
    }
    auto* h = (ShmHeader*)map_;
    if (h->magic != kShmMagic || h->version != kShmVersion) {
        err = "shm magic/version mismatch";
        close();
        return false;
    }
    return true;
}

const ShmHeader* ShmRingReader::header() const { return map_ ? (const ShmHeader*)map_ : nullptr; }

void ShmRingReader::seek_to_now() {
    if (!map_) return;
    rpos_ = load_u64(&((ShmHeader*)map_)->write_pos);
    have_rpos_ = true;
}

void ShmRingReader::seek(uint64_t pos) {
    rpos_ = pos;
    have_rpos_ = true;
}

uint64_t ShmRingReader::available() const {
    if (!map_) return 0;
    uint64_t wp = load_u64(&((ShmHeader*)map_)->write_pos);
    uint64_t rp = have_rpos_ ? rpos_ : wp;
    return wp > rp ? wp - rp : 0;
}

size_t ShmRingReader::read(void* dst, size_t want) {
    if (!map_ || want == 0) return 0;
    auto* h = (ShmHeader*)map_;
    const uint32_t cap = h->capacity;
    uint64_t wp = load_u64(&h->write_pos);
    if (!have_rpos_) { rpos_ = wp; have_rpos_ = true; } // default: start from now
    if (wp - rpos_ > cap) { // data was overwritten (miss)
        lost_ += (wp - rpos_) - cap;
        rpos_ = wp - cap;
    }
    uint64_t avail = wp - rpos_;
    size_t n = want < avail ? want : size_t(avail);
    if (n == 0) return 0;
    const uint8_t* ring = (const uint8_t*)map_ + kShmHeaderSize;
    uint32_t off = uint32_t(rpos_ & (cap - 1));
    size_t first = n < size_t(cap - off) ? n : size_t(cap - off);
    std::memcpy(dst, ring + off, first);
    if (n > first) std::memcpy((uint8_t*)dst + first, ring, n - first);
    rpos_ += n;
    return n;
}

void ShmRingReader::close() {
    if (map_) { munmap(map_, map_size_); map_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    have_rpos_ = false;
}

} // namespace astk
