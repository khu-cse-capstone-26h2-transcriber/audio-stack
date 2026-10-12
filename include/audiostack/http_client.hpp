// ============================================================================
// Minimal HTTP/1.1 POST client (POSIX sockets only, no libcurl).
// Sends transcripts to the Node/Express backend's ingest endpoints.
// Local (127.0.0.1 / same network) only — https not supported; put TLS in
// front of the backend (e.g. an nginx reverse proxy) if needed.
// ============================================================================
#pragma once

#include <string>

namespace astk {

struct Url {
    std::string scheme = "http";
    std::string host = "127.0.0.1";
    int port = 80;
    std::string path = "/";

    static bool parse(const std::string& s, Url& out, std::string& err);
};

struct HttpResponse {
    int status = 0;      // 0 = failure
    std::string body;
    std::string error;   // network/parse error message
    bool ok() const { return status >= 200 && status < 300; }
};

// Content-Type: application/json, Connection: close
HttpResponse http_post_json(const Url& url, const std::string& body, int timeout_ms = 3000);

} // namespace astk
