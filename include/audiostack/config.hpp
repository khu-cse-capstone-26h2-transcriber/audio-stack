// ============================================================================
// INI-style config parser (no dependencies).
//   [section]
//   key = value      ; comments
// Daemon search order: --config arg > ./audiostack.conf > ./config/audiostack.conf
//                      > /etc/audiostack/audiostack.conf
// ============================================================================
#pragma once

#include <map>
#include <string>

namespace astk {

class Config {
public:
    bool load_file(const std::string& path, std::string& err);

    std::string get(const std::string& section, const std::string& key,
                    const std::string& def = {}) const {
        auto it = vals_.find(section + "\x1f" + key);
        return it == vals_.end() ? def : it->second;
    }
    int get_int(const std::string& s, const std::string& k, int def) const {
        auto it = vals_.find(s + "\x1f" + k);
        return it == vals_.end() ? def : std::stoi(it->second);
    }
    double get_double(const std::string& s, const std::string& k, double def) const {
        auto it = vals_.find(s + "\x1f" + k);
        return it == vals_.end() ? def : std::stod(it->second);
    }
    bool get_bool(const std::string& s, const std::string& k, bool def) const {
        auto it = vals_.find(s + "\x1f" + k);
        if (it == vals_.end()) return def;
        const std::string& v = it->second;
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

private:
    std::map<std::string, std::string> vals_;
};

} // namespace astk
