#include "audiostack/ipc_server.hpp"
#include "audiostack/log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <sys/types.h>

namespace astk {

// ---------------------------------------------------------------- utilities --
static bool set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) >= 0;
}

static bool mkdir_p(const std::string& path) {
    std::string cur;
    size_t i = 0;
    if (!path.empty() && path[0] == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (!part.empty()) {
            cur += part;
            if (mkdir(cur.c_str(), 0775) != 0 && errno != EEXIST) return false;
        }
        cur += "/";
        i = j + 1;
    }
    return true;
}

static int listen_unix(const std::string& path, std::string& err) {
    if (path.size() >= sizeof(sockaddr_un::sun_path)) {
        err = "socket path too long: " + path;
        return -1;
    }
    std::string dir = path.substr(0, path.find_last_of('/'));
    if (!dir.empty() && !mkdir_p(dir)) {
        err = "cannot create directory " + dir + ": " + std::strerror(errno);
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { err = "socket: " + std::string(std::strerror(errno)); return -1; }
    struct sockaddr_un sa {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof sa.sun_path - 1);
    unlink(path.c_str()); // clean up a stale socket
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0) {
        err = "bind(" + path + "): " + std::strerror(errno);
        close(fd);
        return -1;
    }
    chmod(path.c_str(), 0660); // allow engine processes in the audio group
    if (listen(fd, 4) < 0) {
        err = "listen: " + std::string(std::strerror(errno));
        close(fd);
        unlink(path.c_str());
        return -1;
    }
    return fd;
}

// ========================================================= EngineIpcServer ==
bool EngineIpcServer::start(const std::string& path, std::string& err) {
    stop();
    lfd_ = listen_unix(path, err);
    if (lfd_ < 0) return false;
    path_ = path;
    LOG_INFO("engine IPC listening on %s", path.c_str());
    return true;
}

void EngineIpcServer::stop() {
    if (cfd_ >= 0) { close(cfd_); cfd_ = -1; }
    if (lfd_ >= 0) { close(lfd_); lfd_ = -1; }
    if (!path_.empty()) { unlink(path_.c_str()); path_.clear(); }
    rbuf_.clear();
    wbuf_.clear();
}

bool EngineIpcServer::handle_accept() {
    if (lfd_ < 0) return false;
    int fd = accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return false;
    if (cfd_ >= 0) drop_client("replaced by new engine connection");
    cfd_ = fd;
    rbuf_.clear();
    LOG_INFO("engine connected (fd=%d)", fd);
    if (client_cb_) client_cb_(true);
    return true;
}

void EngineIpcServer::drop_client(const char* why) {
    if (cfd_ < 0) return;
    LOG_INFO("engine disconnected: %s", why);
    close(cfd_);
    cfd_ = -1;
    rbuf_.clear();
    wbuf_.clear();
    if (client_cb_) client_cb_(false);
}

bool EngineIpcServer::handle_client_read(std::string& err) {
    if (cfd_ < 0) return false;
    uint8_t tmp[16384];
    ssize_t n = recv(cfd_, tmp, sizeof tmp, 0);
    if (n == 0) { drop_client("closed by engine"); return false; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        err = std::string("recv: ") + std::strerror(errno);
        drop_client(err.c_str());
        return false;
    }
    rbuf_.insert(rbuf_.end(), tmp, tmp + n);

    // frame parsing: 24-byte header + payload
    size_t consumed = 0;
    while (rbuf_.size() - consumed >= kHeaderSize) {
        Frame f;
        if (!decode_header(rbuf_.data() + consumed, f)) {
            err = "bad frame magic/version/len — protocol mismatch?";
            drop_client(err.c_str());
            return false;
        }
        uint32_t len = get_u32(rbuf_.data() + consumed + 20);
        if (rbuf_.size() - consumed < kHeaderSize + len) break; // partial frame: wait
        f.payload.assign(rbuf_.begin() + long(consumed + kHeaderSize),
                         rbuf_.begin() + long(consumed + kHeaderSize + len));
        consumed += kHeaderSize + len;
        frames_recv++;
        if (f.type == MSG_PING) {
            send_frame(MSG_PONG, 0, nullptr, 0);
        } else if (frame_cb_) {
            frame_cb_(f);
        }
    }
    rbuf_.erase(rbuf_.begin(), rbuf_.begin() + long(consumed));
    return true;
}

template <typename Container>
static bool flush_some(int fd, Container& buf) {
    while (!buf.empty()) {
        ssize_t n = send(fd, buf.data(), buf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            buf.erase(buf.begin(), buf.begin() + long(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false; // not everything sent yet — retry on POLLOUT
        } else {
            return false;
        }
    }
    return true;
}

bool EngineIpcServer::send_frame(uint8_t type, uint16_t flags, const void* data, size_t len) {
    if (cfd_ < 0) return false;
    Frame f;
    f.type = type;
    f.flags = flags;
    f.seq = seq_++;
    f.t_us = now_us();
    if (data && len) f.payload.assign((const uint8_t*)data, (const uint8_t*)data + len);
    auto bytes = encode_frame(f);
    wbuf_.insert(wbuf_.end(), bytes.begin(), bytes.end());
    bytes_sent += bytes.size();
    bool flushed = flush_some(cfd_, wbuf_);
    if (!flushed && wbuf_.size() > 8u * 1024 * 1024) { // engine is too slow
        drop_client("engine too slow, output buffer overflow");
        return false;
    }
    return true;
}

bool EngineIpcServer::send_json(uint8_t type, const std::string& json) {
    return send_frame(type, 0, json.data(), json.size());
}

bool EngineIpcServer::send_audio(const int16_t* pcm, size_t samples, uint16_t flags, uint64_t t_us) {
    if (cfd_ < 0) return false;
    Frame f;
    f.type = MSG_AUDIO;
    f.flags = flags;
    f.seq = seq_++;
    f.t_us = t_us; // keep the capture-instant timestamp
    const uint8_t* p = (const uint8_t*)pcm;
    f.payload.assign(p, p + samples * 2);
    auto bytes = encode_frame(f);
    wbuf_.insert(wbuf_.end(), bytes.begin(), bytes.end());
    bytes_sent += bytes.size();
    audio_bytes_sent += samples * 2;
    flush_some(cfd_, wbuf_);
    frames_sent++;
    return true;
}

// =========================================================== ControlServer ==
bool ControlServer::start(const std::string& path, std::string& err) {
    stop();
    lfd_ = listen_unix(path, err);
    if (lfd_ < 0) return false;
    path_ = path;
    LOG_INFO("control socket listening on %s", path.c_str());
    return true;
}

void ControlServer::stop() {
    for (auto& c : clients_) close(c.fd);
    clients_.clear();
    if (lfd_ >= 0) { close(lfd_); lfd_ = -1; }
    if (!path_.empty()) { unlink(path_.c_str()); path_.clear(); }
}

int ControlServer::client_index(int fd) const {
    for (size_t i = 0; i < clients_.size(); i++)
        if (clients_[i].fd == fd) return int(i);
    return -1;
}

void ControlServer::close_client(int fd) {
    int i = client_index(fd);
    if (i < 0) return;
    close(clients_[size_t(i)].fd);
    clients_.erase(clients_.begin() + i);
}

void ControlServer::handle_accept() {
    if (lfd_ < 0) return;
    int fd = accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return;
    clients_.push_back({fd, {}, {}});
}

void ControlServer::handle_client_read(int fd) {
    int i = client_index(fd);
    if (i < 0) return;
    Client& c = clients_[size_t(i)];
    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n == 0) { close_client(fd); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_client(fd);
        return;
    }
    c.in.append(buf, size_t(n));
    if (c.in.size() > 65536) { close_client(fd); return; } // runaway guard
    size_t pos;
    while ((pos = c.in.find('\n')) != std::string::npos) {
        std::string line = c.in.substr(0, pos);
        c.in.erase(c.in.begin(), c.in.begin() + long(pos + 1));
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::string reply = cmd_ ? cmd_(line) : std::string("ERR no handler");
        c.out += reply;
        c.out += "\n";
    }
}

void ControlServer::handle_client_write(int fd) {
    int i = client_index(fd);
    if (i < 0) return;
    flush_some(fd, clients_[size_t(i)].out);
    if (clients_[size_t(i)].out.empty()) {
        // reply complete — the line protocol is one-shot, so close to signal EOF
        close_client(fd);
    }
}

} // namespace astk
