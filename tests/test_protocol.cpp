#include "audiostack/protocol.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

using namespace astk;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static std::string get(const std::string& json, const char* key) {
    std::string v = "<none>";
    json_top_string(json, key, v);
    return v;
}

int main() try {
    // RESULT from the engine: the daemon reads only session_id
    const std::string result =
        R"({"type":"final","text":"a \"quoted\" , {not: an object}","lang":"ko",)"
        R"("session_id":"sess-20261108-212659-119d","seg_id":3,"t_start_ms":0})";
    require(get(result, "session_id") == "sess-20261108-212659-119d", "session_id");
    require(get(result, "text") == "a \"quoted\" , {not: an object}", "escaped text");
    require(get(result, "seg_id") == "<none>", "number is not a string");
    require(get(result, "missing") == "<none>", "missing key");

    // ENGINE_STATUS session_done
    const std::string st = R"( { "event" : "session_done", "session_id" : "sess-1", "finals" : 8 } )";
    require(get(st, "event") == "session_done", "event with spaces");
    require(get(st, "session_id") == "sess-1", "session_id with spaces");

    // keys inside nested objects/arrays and inside string values are not top-level
    const std::string nested =
        R"({"meta":{"session_id":"inner"},"list":["session_id","x"],"note":"\"session_id\":\"fake\""})";
    require(get(nested, "session_id") == "<none>", "nested or quoted key ignored");
    require(get(R"({"meta":{"session_id":"inner"},"session_id":"outer"})", "session_id") == "outer",
            "top-level key after a nested one");

    // malformed or non-object input
    require(get("plain text", "text") == "<none>", "not an object");
    require(get(R"({"session_id":"unterminated)", "session_id") == "<none>", "unterminated string");
    require(get(R"(["session_id","x"])", "session_id") == "<none>", "array at top level");
    require(get("{\"session_id\":\"caf\\u00e9\"}", "session_id") == "<none>", "unicode escape not decoded");

    std::puts("protocol tests passed");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAILED: %s\n", e.what());
    return 1;
}
