// ============================================================================
// Session model — corresponds 1:1 to the web/app part's "session CRUD" API.
// The daemon issues session_id and announces start/end to both the backend
// REST and the engine IPC.
// ============================================================================
#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>
#include <sstream>
#include <string>

namespace astk {

inline std::string iso8601_now() {
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    struct tm tm {};
    gmtime_r(&tt, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// e.g. sess-20261012-153001-4f2a
inline std::string new_session_id() {
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    struct tm tm {};
    gmtime_r(&tt, &tm);
    char date[24];
    std::strftime(date, sizeof date, "%Y%m%d-%H%M%S", &tm);
    static std::mt19937 rng {std::random_device {}()};
    std::uniform_int_distribution<int> dist(0, 0xffff);
    char rnd[8];
    std::snprintf(rnd, sizeof rnd, "%04x", dist(rng));
    std::ostringstream os;
    os << "sess-" << date << "-" << rnd;
    return os.str();
}

struct Session {
    std::string id;
    std::string started_at; // ISO8601
    bool active = false;

    void begin() {
        id = new_session_id();
        started_at = iso8601_now();
        active = true;
    }
    std::string end() { // returns the ended session's id, deactivates the session
        active = false;
        std::string e = id;
        id.clear();
        return e;
    }
};

} // namespace astk
