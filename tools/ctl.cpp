// astk-ctl — control-socket client for the daemon.
// usage: astk-ctl [--socket PATH] status|stats|level|"session rotate"|shutdown
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
    std::string sock = "/run/audiostack/ctrl.sock";
    std::vector<std::string> words;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) sock = argv[++i];
        else words.push_back(a);
    }
    if (words.empty()) {
        std::printf("usage: astk-ctl [--socket PATH] <command...>\n"
                    "commands: status | stats | level | session rotate | shutdown | help\n");
        return 2;
    }
    std::string line;
    for (size_t i = 0; i < words.size(); i++) { if (i) line += " "; line += words[i]; }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa {};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, sock.c_str(), sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0) {
        std::fprintf(stderr, "connect(%s): %s\n", sock.c_str(), std::strerror(errno));
        std::fprintf(stderr, "is the daemon running? check: systemctl status audiostack\n");
        return 1;
    }
    std::string cmd = line + "\n";
    if (::write(fd, cmd.data(), cmd.size()) < 0) {
        std::fprintf(stderr, "write failed\n");
        return 1;
    }
    std::string resp;
    while (true) {
        struct pollfd pf {fd, POLLIN, 0};
        if (poll(&pf, 1, 3000) <= 0) break;
        char buf[4096];
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        resp.append(buf, size_t(n));
    }
    close(fd);
    std::fputs(resp.c_str(), stdout);
    return 0;
}
