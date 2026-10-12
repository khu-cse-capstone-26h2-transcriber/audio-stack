#include "audiostack/http_client.hpp"
#include "audiostack/log.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace astk {

bool Url::parse(const std::string& s, Url& out, std::string& err) {
    auto p = s.find("://");
    if (p == std::string::npos) {
        err = "url missing scheme: " + s;
        return false;
    }
    out.scheme = s.substr(0, p);
    size_t rest = p + 3;
    size_t slash = s.find('/', rest);
    std::string hp = slash == std::string::npos ? s.substr(rest) : s.substr(rest, slash - rest);
    out.path = slash == std::string::npos ? "/" : s.substr(slash);
    if (hp.empty()) {
        err = "url missing host: " + s;
        return false;
    }
    size_t colon = hp.rfind(':');
    if (colon != std::string::npos) {
        out.host = hp.substr(0, colon);
        out.port = std::atoi(hp.c_str() + colon + 1);
    } else {
        out.host = hp;
        out.port = (out.scheme == "https") ? 443 : 80;
    }
    if (out.port <= 0 || out.port > 65535) {
        err = "bad port in url: " + s;
        return false;
    }
    return true;
}

static int connect_timeout(const char* host, const char* port, int timeout_ms) {
    struct addrinfo hints {}, *ai = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &ai);
    if (rc != 0) {
        LOG_DEBUG("getaddrinfo(%s): %s", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) { /* ok */ }
        else if (errno == EINPROGRESS) {
            struct pollfd pf {fd, POLLOUT, 0};
            if (poll(&pf, 1, timeout_ms) <= 0) { close(fd); fd = -1; continue; }
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr != 0) { close(fd); fd = -1; continue; }
        } else {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, fl); // restore blocking mode
        break;
    }
    freeaddrinfo(ai);
    return fd;
}

HttpResponse http_post_json(const Url& url, const std::string& body, int timeout_ms) {
    HttpResponse res;
    if (url.scheme != "http") {
        res.error = "scheme '" + url.scheme + "' unsupported (http only; put TLS at a proxy)";
        return res;
    }
    int fd = connect_timeout(url.host.c_str(), std::to_string(url.port).c_str(), timeout_ms);
    if (fd < 0) {
        res.error = "connect failed: http://" + url.host + ":" + std::to_string(url.port);
        return res;
    }
    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    std::ostringstream req;
    req << "POST " << url.path << " HTTP/1.1\r\n"
        << "Host: " << url.host << ":" << url.port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    std::string out = req.str();
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            res.error = "send failed";
            close(fd);
            return res;
        }
        sent += size_t(n);
    }

    std::string raw;
    char buf[4096];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            raw.append(buf, size_t(n));
            if (raw.size() > 65536) break; // safety cap
            // early exit once Content-Length is satisfied
            size_t hdr_end = raw.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                size_t cl = 0;
                bool have_cl = false;
                std::string h = raw.substr(0, hdr_end);
                size_t c = h.find("Content-Length:");
                if (c == std::string::npos) c = h.find("content-length:");
                if (c != std::string::npos) { cl = std::atoi(h.c_str() + c + 15); have_cl = true; }
                if (have_cl && raw.size() - (hdr_end + 4) >= cl) break;
            }
        } else if (n == 0) {
            break; // EOF
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            res.error = "recv failed";
            break;
        }
    }
    close(fd);

    if (raw.empty()) {
        if (res.error.empty()) res.error = "empty response";
        return res;
    }
    if (raw.compare(0, 5, "HTTP/") != 0) {
        res.error = "not an HTTP response";
        return res;
    }
    res.status = std::atoi(raw.c_str() + 9);
    size_t hdr_end = raw.find("\r\n\r\n");
    if (hdr_end != std::string::npos) res.body = raw.substr(hdr_end + 4);
    return res;
}

} // namespace astk
