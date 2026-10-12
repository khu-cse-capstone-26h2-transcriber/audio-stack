// ============================================================================
// audiostackd — audio stack daemon
//
//   microphone (ALSA) ──[16k mono S16_LE pipeline]──┬──> engine IPC socket
//                                                   ├──> POSIX shm ring (optional)
//                                                   └──(RESULT replies)──> HTTP POST to the Node backend
//
// Defaults follow the survey: 16 kHz / S16_LE / mono, period 20 ms, buffer
// 4 periods, immediate snd_pcm_recover() on XRUN. Identical to the Whisper
// input specification.
// ============================================================================
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audiostack/audiostack.hpp"

using namespace astk;

static std::atomic<bool> g_stop {false};
static void handle_signal(int) { g_stop = true; }

// ---------------------------------------------------------------- options --
struct Options {
    std::string config_path, device, backend_url, engine_socket, log_level;
    bool no_backend = false, help = false, version = false;
};

static void usage() {
    std::printf(
        "audiostackd — on-device STT audio stack daemon (Rubik Pi 3 / Fedora)\n"
        "usage: audiostackd [options]\n"
        "  --config=PATH        config file (INI). default: ./audiostack.conf, /etc/audiostack/audiostack.conf\n"
        "  --device=ALSA_DEV    capture device override (e.g. plughw:1,0 / hw:CARD=Array)\n"
        "  --backend-url=URL    Node backend base URL (e.g. http://127.0.0.1:3000)\n"
        "  --engine-socket=PATH engine IPC socket path\n"
        "  --no-backend         disable backend HTTP delivery (engine/local testing only)\n"
        "  --log-level=lv       trace|debug|info|warn|error\n"
        "  --version, --help\n");
}

static bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto take_value = [&](std::string& dst) -> bool {
            size_t eq = a.find('=');
            if (eq != std::string::npos) { dst = a.substr(eq + 1); return true; }
            if (i + 1 < argc) { dst = argv[++i]; return true; }
            std::fprintf(stderr, "%s requires a value\n", a.c_str());
            return false;
        };
        auto is_opt = [&](const char* name) {
            size_t len = strlen(name);
            return a == name || (a.size() > len && a.compare(0, len, name) == 0 && a[len] == '=');
        };
        if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--version" || a == "-V") o.version = true;
        else if (a == "--no-backend") o.no_backend = true;
        else if (is_opt("--config")) { if (!take_value(o.config_path)) return false; }
        else if (is_opt("--device")) { if (!take_value(o.device)) return false; }
        else if (is_opt("--backend-url")) { if (!take_value(o.backend_url)) return false; }
        else if (is_opt("--engine-socket")) { if (!take_value(o.engine_socket)) return false; }
        else if (is_opt("--log-level")) { if (!take_value(o.log_level)) return false; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

static std::string substitute_path(const std::string& tmpl, const std::string& session) {
    std::string out = tmpl;
    size_t p = out.find("{session}");
    if (p != std::string::npos) out.replace(p, 9, session);
    return out;
}

// ---------------------------------------------------------------- backend --
class Backend {
public:
    struct Cfg {
        bool enabled = true;
        Url base;
        std::string session_path = "/api/sessions";
        std::string transcript_path = "/api/sessions/{session}/transcripts";
        std::string end_path = "/api/sessions/{session}/end";
        int timeout_ms = 3000;
        int retry_seconds = 3;
        int queue_max = 512;
    };

    explicit Backend(Cfg c) : cfg_(c) {
        th_ = std::thread([this] { loop(); });
    }
    ~Backend() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

    void enqueue(std::string path, std::string body) {
        if (!cfg_.enabled) return;
        std::lock_guard<std::mutex> lk(m_);
        while (int(q_.size()) >= cfg_.queue_max) {
            q_.pop_front();
            dropped_++;
        }
        q_.push_back({std::move(path), std::move(body)});
        cv_.notify_one();
    }

    size_t queued() { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    uint64_t sent() const { return sent_; }
    uint64_t failed() const { return failed_; }
    uint64_t dropped() const { return dropped_; }
    bool enabled() const { return cfg_.enabled; }

private:
    struct Item {
        std::string path, body;
    };
    Cfg cfg_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Item> q_;
    bool stop_ = false;
    std::thread th_;
    std::atomic<uint64_t> sent_ {0}, failed_ {0}, dropped_ {0};

    void loop() {
        while (true) {
            Item it;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait_for(lk, std::chrono::milliseconds(200),
                             [this] { return stop_ || !q_.empty(); });
                if (stop_) return;
                if (q_.empty()) continue;
                it = std::move(q_.front());
                q_.pop_front();
            }
            Url u = cfg_.base;
            u.path = it.path;
            HttpResponse r = http_post_json(u, it.body, cfg_.timeout_ms);
            if (r.ok()) {
                sent_++;
            } else {
                failed_++;
                LOG_WARN("backend POST %s failed: %s (status=%d)", it.path.c_str(),
                         r.error.empty() ? ("http " + std::to_string(r.status)).c_str() : r.error.c_str(),
                         r.status);
                // retry: wait a bit, then push back to the front to keep ordering
                for (int s = 0; s < cfg_.retry_seconds * 5 && !g_stop.load(); s++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::lock_guard<std::mutex> lk(m_);
                if (int(q_.size()) < cfg_.queue_max) q_.push_front(std::move(it));
            }
        }
    }
};

// ------------------------------------------------------------------- misc --
struct Chunk {
    std::vector<int16_t> pcm; // 16k mono s16le
    uint64_t t_us = 0;
    bool silence = false;
};

static bool mkdir_p_local(const std::string& path) {
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

// fall back to a user-writable runtime dir if /run/audiostack is unavailable
static std::string resolve_socket_path(const std::string& p) {
    if (!p.empty() && p[0] == '/') {
        std::string dir = p.substr(0, p.find_last_of('/'));
        if (!dir.empty() && mkdir_p_local(dir)) return p;
    }
    const char* xrd = std::getenv("XDG_RUNTIME_DIR");
    std::string base = (xrd && *xrd) ? std::string(xrd) : ("/tmp/audiostack-" + std::to_string(getuid()));
    std::string name = p.substr(p.find_last_of('/') + 1);
    std::string alt = base + "/" + name;
    LOG_WARN("socket dir not writable, falling back to %s", alt.c_str());
    return alt;
}

// ------------------------------------------------------------------- main --
int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 2;
    if (opt.help) { usage(); return 0; }
    if (opt.version) { std::printf("audiostackd 0.1.0\n"); return 0; }

    Logger::instance().set_ident("audiostackd");
    Config cfg;
    std::string cfg_path = opt.config_path;
    if (cfg_path.empty()) {
        const char* cands[] = {"./audiostack.conf", "./config/audiostack.conf",
                               "/etc/audiostack/audiostack.conf"};
        for (const char* c : cands)
            if (access(c, R_OK) == 0) { cfg_path = c; break; }
    }
    if (!cfg_path.empty()) {
        std::string err;
        if (!cfg.load_file(cfg_path, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        LOG_INFO("loaded config: %s", cfg_path.c_str());
    } else {
        LOG_INFO("no config file found; using built-in defaults");
    }
    Logger::instance().set_level(!opt.log_level.empty() ? opt.log_level
                                                        : cfg.get("log", "level", "info"));

    struct sigaction sa {};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // ---- capture configuration --------------------------------------------
    AlsaCapture::Request req;
    req.device = !opt.device.empty() ? opt.device : cfg.get("capture", "device", "default");
    req.channels = unsigned(cfg.get_int("capture", "channels", 1));
    req.period_ms = unsigned(cfg.get_int("capture", "period_ms", 20));
    req.buffer_periods = unsigned(cfg.get_int("capture", "buffer_periods", 4));
    const unsigned target_rate = unsigned(cfg.get_int("pre", "resample_to", 16000));
    const size_t ch_idx = size_t(cfg.get_int("capture", "channel_index", 0));
    const double gate_db = cfg.get_double("pre", "silence_gate_db", -45.0);
    const double hpz = cfg.get_double("pre", "highpass_hz", 50.0);

    AlsaCapture capture;
    bool opened = false;
    std::string err;
    unsigned try_rates[4] = {unsigned(cfg.get_int("capture", "native_rate", 0)), target_rate, 48000, 44100};
    for (unsigned r : try_rates) {
        if (r == 0) continue;
        req.rate = r;
        if (capture.open(req, err)) { opened = true; break; }
    }
    if (!opened) {
        LOG_ERROR("cannot open capture device '%s': %s", req.device.c_str(), err.c_str());
        LOG_ERROR("hint: check cards with astk-list-devices, verify audio group membership; "
                  "the on-board mic needs amixer routing (see README)");
        return 1;
    }
    const auto cst = capture.status();

    // ---- pre-processing (50 Hz high-pass, 16k mono) ------------------------
    Biquad hp;
    const bool use_hp = hpz > 0;
    if (use_hp) hp = make_highpass(hpz, double(cst.rate));
    std::unique_ptr<Resampler> rs;
    if (cst.rate != target_rate) {
        rs = std::make_unique<Resampler>(double(cst.rate), double(target_rate));
        LOG_INFO("resampling %u -> %u Hz (software)", cst.rate, target_rate);
    }

    // ---- engine IPC --------------------------------------------------------
    const std::string transport = cfg.get("engine", "transport", "socket");
    std::string engine_sock = resolve_socket_path(
        opt.engine_socket.empty() ? cfg.get("engine", "socket", "/run/audiostack/engine.sock")
                                  : opt.engine_socket);
    std::string ctrl_sock = resolve_socket_path(
        cfg.get("engine", "ctrl_socket",
                engine_sock.substr(0, engine_sock.find_last_of('/') + 1) + "ctrl.sock"));
    EngineIpcServer ipc;
    if (!ipc.start(engine_sock, err)) {
        LOG_ERROR("engine socket: %s", err.c_str());
        return 1;
    }
    ControlServer ctrl;
    if (!ctrl.start(ctrl_sock, err)) {
        LOG_ERROR("ctrl socket: %s", err.c_str());
        return 1;
    }
    ShmRingWriter shm;
    const std::string shm_name = cfg.get("engine", "shm_name", "/audiostack_ring");
    const int shm_seconds = cfg.get_int("engine", "shm_seconds", 30);
    if (transport == "shm" || transport == "both") {
        if (!shm.open(shm_name, uint32_t(target_rate) * 2 * unsigned(shm_seconds),
                      target_rate, 1, Encoding::S16LE, err)) {
            LOG_ERROR("shm ring: %s", err.c_str());
            return 1;
        }
    }

    // ---- backend (Node/Express + PostgreSQL + SSE) --------------------------
    Backend::Cfg bc;
    bc.enabled = !opt.no_backend && cfg.get_bool("backend", "enabled", true);
    std::string base_url = opt.backend_url.empty() ? cfg.get("backend", "base_url", "http://127.0.0.1:3000")
                                                   : opt.backend_url;
    if (bc.enabled && !Url::parse(base_url, bc.base, err)) {
        LOG_ERROR("backend base_url: %s", err.c_str());
        return 1;
    }
    bc.session_path = cfg.get("backend", "session_path", bc.session_path);
    bc.transcript_path = cfg.get("backend", "transcript_path", bc.transcript_path);
    bc.end_path = cfg.get("backend", "end_path", bc.end_path);
    bc.timeout_ms = cfg.get_int("backend", "timeout_ms", 3000);
    bc.retry_seconds = cfg.get_int("backend", "retry_seconds", 3);
    bc.queue_max = cfg.get_int("backend", "queue_max", 512);
    Backend backend(bc);

    // ---- session / stats ----------------------------------------------------
    Session session;
    session.begin();
    std::atomic<uint64_t> frames_in {0}, chunks_sent {0}, chunks_dropped {0}, results_fwd {0};
    std::atomic<int> level_db_x10 {-1400};
    std::atomic<bool> engine_conn {false};

    SpscRing<std::shared_ptr<Chunk>> ring(2048); // ~40 s of buffering (20 ms chunks)
    const int cache_seconds = cfg.get_int("engine", "cache_seconds", 10);
    std::mutex cache_mu;
    std::deque<std::shared_ptr<Chunk>> cache;
    size_t cache_samples = 0;
    const size_t cache_limit = size_t(target_rate) * size_t(cache_seconds);

    auto hello_json = [&]() {
        std::string j = "{";
        j += "\"proto\":1,\"name\":\"audiostackd\",\"version\":\"0.1.0\",";
        j += "\"format\":{\"rate\":" + std::to_string(target_rate) +
             ",\"channels\":1,\"encoding\":\"s16le\"},";
        j += "\"chunk_ms\":" + std::to_string(req.period_ms) + ",";
        j += "\"cache_seconds\":" + std::to_string(cache_seconds) + ",";
        j += "\"capture_device\":" + json_str(cst.device) + ",";
        j += "\"session_id\":" + json_str(session.id) + ",";
        j += "\"transport\":{\"socket\":" + json_str(engine_sock) +
             ",\"shm\":" + json_str(transport == "socket" ? "" : shm_name) + "}}";
        return j;
    };
    auto post_session_begin = [&]() {
        std::string b = "{\"session_id\":" + json_str(session.id) +
                        ",\"started_at\":" + json_str(session.started_at) +
                        ",\"device\":" + json_str(cst.device) + "}";
        backend.enqueue(bc.session_path, b);
    };
    auto post_session_end = [&](const std::string& id) {
        std::string b = "{\"session_id\":" + json_str(id) +
                        ",\"ended_at\":" + json_str(iso8601_now()) + "}";
        backend.enqueue(substitute_path(bc.end_path, id), b);
    };
    post_session_begin();
    if (ipc.has_client()) ipc.send_json(MSG_SESSION_BEGIN, "{\"session_id\":" + json_str(session.id) + "}");

    // ---- capture thread ------------------------------------------------------
    std::thread cap_th([&] {
        std::vector<uint8_t> raw(capture.period_bytes());
        std::vector<float> fbuf, rbuf;
        std::vector<int16_t> mono;
        while (!g_stop.load()) {
            int w = capture.wait_ms(200);
            if (w != 1) continue;
            snd_pcm_sframes_t fr = capture.readi(raw.data(), capture.period_frames());
            if (fr <= 0) continue;

            to_float_mono(raw.data(), size_t(fr), cst.channels, ch_idx, cst.enc, fbuf);
            if (use_hp)
                for (float& x : fbuf) x = float(hp.process(x));
            const float* src = fbuf.data();
            size_t n = fbuf.size();
            if (rs) {
                rbuf.clear();
                rs->process(fbuf.data(), fbuf.size(), rbuf);
                src = rbuf.data();
                n = rbuf.size();
            }
            if (n == 0) continue;
            mono.resize(n);
            float_to_s16(src, n, mono.data());

            float db = rms_to_dbfs(rms(mono.data(), n));
            level_db_x10.store(int(db * 10));

            auto c = std::make_shared<Chunk>();
            c->pcm = std::move(mono);
            c->t_us = now_us();
            c->silence = db < gate_db;
            mono = std::vector<int16_t>(); // ready for reuse after the move

            if (!ring.push(c)) chunks_dropped++;
            frames_in++;
            shm.write(c->pcm.data(), c->pcm.size() * 2); // no-op when not opened (transport=socket)

            {
                std::lock_guard<std::mutex> lk(cache_mu);
                cache_samples += c->pcm.size();
                cache.push_back(c);
                while (cache_samples > cache_limit && !cache.empty()) {
                    cache_samples -= cache.front()->pcm.size();
                    cache.pop_front();
                }
            }
        }
    });

    // ---- IPC thread (engine + control sockets) -------------------------------
    std::thread ipc_th([&] {
        const size_t chunk_samples = size_t(target_rate) * size_t(req.period_ms) / 1000;
        std::vector<int16_t> agg;
        uint64_t agg_t_us = 0;
        bool agg_silence = true;

        auto flush_agg = [&](bool final_chunk) {
            if (agg.empty() || !ipc.has_client()) {
                agg.clear();
                agg_t_us = 0;
                agg_silence = true;
                return;
            }
            uint16_t flags = agg_silence ? kFlagSilence : kFlagNone;
            if (final_chunk) flags |= kFlagFinal;
            ipc.send_audio(agg.data(), agg.size(), flags, agg_t_us);
            chunks_sent++;
            agg.clear();
            agg_t_us = 0;
            agg_silence = true;
        };

        ipc.on_client([&](bool connected) {
            engine_conn.store(connected);
            if (!connected) return;
            ipc.send_json(MSG_HELLO, hello_json());
            if (session.active)
                ipc.send_json(MSG_SESSION_BEGIN, "{\"session_id\":" + json_str(session.id) + "}");
            // replay the audio that would be lost while the engine was away
            // (within cache_seconds)
            std::deque<std::shared_ptr<Chunk>> replay;
            {
                std::lock_guard<std::mutex> lk(cache_mu);
                replay = cache;
            }
            for (auto& c : replay)
                ipc.send_audio(c->pcm.data(), c->pcm.size(), uint16_t(kFlagCached | (c->silence ? kFlagSilence : 0)), c->t_us);
            if (!replay.empty())
                LOG_INFO("replayed %zu cached chunks to engine", replay.size());
        });

        ipc.on_frame([&](const Frame& f) {
            if (f.type == MSG_RESULT) {
                std::string payload = f.payload_str();
                std::string transcript = (!payload.empty() && payload.front() == '{')
                                             ? payload
                                             : ("{\"type\":\"final\",\"text\":" + json_str(payload) + "}");
                std::string body = "{\"session_id\":" + json_str(session.id) +
                                   ",\"seq\":" + std::to_string(results_fwd.load() + 1) +
                                   ",\"received_at\":" + json_str(iso8601_now()) +
                                   ",\"audio_t_us\":" + std::to_string(f.t_us) +
                                   ",\"transcript\":" + transcript + "}";
                backend.enqueue(substitute_path(bc.transcript_path, session.id), body);
                results_fwd++;
                LOG_DEBUG("result -> backend: %.120s", payload.c_str());
            } else if (f.type == MSG_ENGINE_STATUS) {
                LOG_INFO("engine status: %.200s", f.payload_str().c_str());
            } else if (f.type == MSG_ERROR) {
                LOG_WARN("engine error frame: %.200s", f.payload_str().c_str());
            }
        });

        auto ctrl_handler = [&](const std::string& line) -> std::string {
            if (line == "help")
                return "commands: status | stats | level | session rotate | shutdown";
            if (line == "level")
                return std::to_string(double(level_db_x10.load()) / 10.0);
            if (line == "status" || line == "stats") {
                std::string j = "{";
                j += "\"running\":true,\"session_id\":" + json_str(session.id) +
                     ",\"started_at\":" + json_str(session.started_at);
                j += ",\"capture\":{\"device\":" + json_str(cst.device) +
                     ",\"native_rate\":" + std::to_string(cst.rate) +
                     ",\"target_rate\":" + std::to_string(target_rate) +
                     ",\"channels\":" + std::to_string(cst.channels) +
                     ",\"period_frames\":" + std::to_string(cst.period_frames) +
                     ",\"buffer_frames\":" + std::to_string(cst.buffer_frames) +
                     ",\"xruns\":" + std::to_string(cst.xruns) + "}";
                j += ",\"engine_connected\":" + std::string(ipc.has_client() ? "true" : "false");
                j += ",\"frames_in\":" + std::to_string(frames_in.load()) +
                     ",\"chunks_sent\":" + std::to_string(chunks_sent.load()) +
                     ",\"chunks_dropped\":" + std::to_string(chunks_dropped.load()) +
                     ",\"results_forwarded\":" + std::to_string(results_fwd.load());
                j += ",\"backend\":{\"enabled\":" + std::string(bc.enabled ? "true" : "false") +
                     ",\"queued\":" + std::to_string(backend.queued()) +
                     ",\"sent\":" + std::to_string(backend.sent()) +
                     ",\"failed\":" + std::to_string(backend.failed()) +
                     ",\"dropped\":" + std::to_string(backend.dropped()) + "}";
                j += ",\"level_db\":" + std::to_string(double(level_db_x10.load()) / 10.0);
                j += "}";
                return j;
            }
            if (line == "session rotate") {
                if (session.active) {
                    std::string old = session.end();
                    ipc.send_json(MSG_SESSION_END, "{\"session_id\":" + json_str(old) + "}");
                    post_session_end(old);
                }
                session.begin();
                post_session_begin();
                ipc.send_json(MSG_SESSION_BEGIN, "{\"session_id\":" + json_str(session.id) + "}");
                LOG_INFO("session rotated -> %s", session.id.c_str());
                return "OK " + session.id;
            }
            if (line == "shutdown") {
                g_stop = true;
                return "OK stopping";
            }
            return "ERR unknown command: " + line;
        };
        ctrl.on_command(ctrl_handler);

        uint64_t last_heartbeat = 0;
        while (!g_stop.load()) {
            struct pollfd pfds[16];
            nfds_t n = 0;
            const nfds_t eng_listen = n;
            pfds[n++] = {ipc.listen_fd(), POLLIN, 0};
            const bool has_eng = ipc.has_client();
            const nfds_t eng_conn_idx = has_eng ? n : nfds_t(-1);
            if (has_eng)
                pfds[n++] = {ipc.client_fd(), short(POLLIN | (ipc.wants_write() ? POLLOUT : 0)), 0};
            const nfds_t ctrl_listen = n;
            pfds[n++] = {ctrl.listen_fd(), POLLIN, 0};
            for (size_t i = 0; i < ctrl.client_count() && n < 16; i++) {
                int fd = ctrl.client_fd_at(i);
                pfds[n++] = {fd, POLLIN, 0};
            }
            poll(pfds, n, 200);

            if (pfds[eng_listen].revents & POLLIN) ipc.handle_accept();
            if (eng_conn_idx != nfds_t(-1) &&
                (pfds[eng_conn_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                std::string e;
                ipc.handle_client_read(e);
            }
            if (pfds[ctrl_listen].revents & POLLIN) ctrl.handle_accept();
            for (nfds_t i = ctrl_listen + 1; i < n; i++) {
                if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    ctrl.handle_client_read(pfds[i].fd);
                    // local UNIX socket: the (few-KB) reply almost always flushes at once
                    ctrl.handle_client_write(pfds[i].fd);
                }
            }

            // forward audio chunks (period_ms == chunk_ms recommended: 1:1)
            if (ipc.has_client()) {
                std::shared_ptr<Chunk> c;
                int budget = 64;
                while (budget-- > 0 && ring.pop(c)) {
                    agg.insert(agg.end(), c->pcm.begin(), c->pcm.end());
                    if (agg_t_us == 0) agg_t_us = c->t_us;
                    agg_silence = agg_silence && c->silence;
                    if (agg.size() >= chunk_samples) flush_agg(false);
                }
            }

            uint64_t now = now_us();
            if (ipc.has_client() && now - last_heartbeat >= 2000000ull) {
                last_heartbeat = now;
                std::string j = "{\"level_db\":" + std::to_string(double(level_db_x10.load()) / 10.0) +
                                ",\"session_id\":" + json_str(session.id) +
                                ",\"backlog\":" + std::to_string(ring.size()) + "}";
                ipc.send_json(MSG_HEARTBEAT, j);
            }
        }
        flush_agg(true);
    });

    cap_th.join();
    ipc_th.join();

    // ---- graceful shutdown ---------------------------------------------------
    if (ipc.has_client()) {
        ipc.send_json(MSG_GOODBYE, "{\"reason\":\"daemon shutdown\"}");
        if (session.active)
            ipc.send_json(MSG_SESSION_END, "{\"session_id\":" + json_str(session.id) + "}");
    }
    if (session.active) {
        std::string id = session.end();
        post_session_end(id);
    }
    ctrl.stop();
    ipc.stop();
    shm.close(true);
    capture.close();
    LOG_INFO("audiostackd stopped (frames_in=%llu sent=%llu dropped=%llu results=%llu)",
             (unsigned long long)frames_in.load(), (unsigned long long)chunks_sent.load(),
             (unsigned long long)chunks_dropped.load(), (unsigned long long)results_fwd.load());
    return 0;
}
