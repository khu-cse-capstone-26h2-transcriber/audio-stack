// ============================================================================
// EngineIpcServer — the survey's option (2): AF_UNIX SOCK_STREAM.
// The engine connects as a client to:
//   /run/audiostack/engine.sock
// ControlServer — line-based text control for humans/scripts/Node:
//   /run/audiostack/ctrl.sock   (status, session rotate, level, help ...)
// ============================================================================
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "audiostack/protocol.hpp"

namespace astk {

class EngineIpcServer {
public:
    using FrameCb = std::function<void(const Frame&)>;
    using ClientCb = std::function<void(bool connected)>;

    bool start(const std::string& path, std::string& err);
    void stop();

    int listen_fd() const { return lfd_; }
    int client_fd() const { return cfd_; }
    bool has_client() const { return cfd_ >= 0; }

    // whether POLLOUT should be watched (unsent data queued)
    bool wants_write() const { return !wbuf_.empty(); }

    bool send_frame(uint8_t type, uint16_t flags, const void* data, size_t len);
    bool send_json(uint8_t type, const std::string& json);
    bool send_audio(const int16_t* pcm, size_t samples, uint16_t flags, uint64_t t_us);

    // call when poll reports readable. false = client connection was dropped
    bool handle_client_read(std::string& err);
    // accept a new engine connection (the previous client, if any, is replaced).
    // true = accepted
    bool handle_accept();

    void on_frame(FrameCb cb) { frame_cb_ = std::move(cb); }
    void on_client(ClientCb cb) { client_cb_ = std::move(cb); }

    uint64_t frames_sent = 0, frames_recv = 0, bytes_sent = 0, audio_bytes_sent = 0;

private:
    void drop_client(const char* why);
    int lfd_ = -1, cfd_ = -1;
    std::vector<uint8_t> rbuf_, wbuf_;
    uint32_t seq_ = 0;
    std::string path_;
    FrameCb frame_cb_;
    ClientCb client_cb_;
};

class ControlServer {
public:
    using CmdCb = std::function<std::string(const std::string& line)>;

    bool start(const std::string& path, std::string& err);
    void stop();

    int listen_fd() const { return lfd_; }
    int client_fd_at(size_t i) const { return i < clients_.size() ? clients_[i].fd : -1; }
    size_t client_count() const { return clients_.size(); }

    void handle_accept();
    void handle_client_read(int fd);
    void handle_client_write(int fd);

    void on_command(CmdCb cb) { cmd_ = std::move(cb); }

private:
    struct Client {
        int fd = -1;
        std::string in, out;
    };
    int lfd_ = -1;
    std::string path_;
    std::vector<Client> clients_;
    CmdCb cmd_;
    int client_index(int fd) const;
    void close_client(int fd);
};

} // namespace astk
