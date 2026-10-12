#include "audiostack/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace astk {

static std::string trim(std::string s) {
    auto notspace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}

bool Config::load_file(const std::string& path, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open config: " + path;
        return false;
    }
    std::string line, section = "default";
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        // strip comments (';' or '#') — simple INI, no quote handling
        size_t cut = line.find_first_of(";#");
        if (cut != std::string::npos) line = line.substr(0, cut);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            std::ostringstream os;
            os << path << ":" << lineno << ": ignoring malformed line: " << line;
            err = os.str(); // keep it as a warning note and continue parsing
            continue;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        vals_[section + "\x1f" + key] = val;
    }
    return true;
}

} // namespace astk
