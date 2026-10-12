// astk-list-devices — inspect and suggest ALSA capture devices.
// usage: astk-list-devices [--json]
#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "audiostack/alsa_capture.hpp"
#include "audiostack/log.hpp"

using namespace astk;

int main(int argc, char** argv) {
    bool as_json = argc > 1 && std::string(argv[1]) == "--json";

    auto cards = list_cards();
    auto pcms = list_pcms();

    if (!as_json) {
        std::printf("=== /proc/asound/cards ===\n");
        for (auto& c : cards)
            std::printf("  card %d: id=%-20s name=%s\n", c.num, c.id.c_str(), c.name.c_str());
        std::printf("\n=== /proc/asound/pcm (capture streams) ===\n");
        for (auto& p : pcms)
            if (p.capture)
                std::printf("  plughw:%d,%d  card=%s  %s\n", p.card, p.device,
                            p.card < (int)cards.size() ? cards[p.card].id.c_str() : "?", p.name.c_str());
        std::printf("\n=== /dev/snd/by-id (USB audio symlinks) ===\n");
        DIR* d = opendir("/dev/snd/by-id");
        bool any = false;
        if (d) {
            struct dirent* e;
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                std::printf("  %s\n", e->d_name);
                any = true;
            }
            closedir(d);
        }
        if (!any) std::printf("  (none — no USB audio attached or by-id unsupported)\n");

        std::printf("\n=== suggestions ===\n");
        // 1) USB mics (ReSpeaker etc.) first, 2) Rubik Pi 3 on-board plughw:0,2, 3) default
        for (auto& c : cards) {
            bool usb = c.name.find("USB") != std::string::npos ||
                       c.id.find("UAC") != std::string::npos ||
                       c.name.find("ReSpeaker") != std::string::npos ||
                       c.id.find("Array") != std::string::npos;
            if (usb) {
                for (auto& p : pcms)
                    if (p.card == c.num && p.capture) {
                        std::printf("  USB mic candidate: plughw:%d,%d  (--device '%s')\n", p.card, p.device,
                                    ("plughw:CARD=" + c.id).c_str());
                    }
            }
        }
        for (auto& c : cards) {
            if (c.id.find("qcs6490") != std::string::npos || c.name.find("RB3") != std::string::npos ||
                c.name.find("q6") != std::string::npos) {
                std::printf("  Rubik Pi/RB3 on-board card detected (id=%s). Mic input is usually device 2:\n",
                            c.id.c_str());
                std::printf("    amixer -c %d cset \"name=MultiMedia3 Mixer PRIMARY_MI2S_TX\" 1\n", c.num);
                std::printf("    --device plughw:%d,2   (per official docs; may vary by firmware)\n", c.num);
            }
        }
        std::printf("  otherwise: --device default (ALSA default capture)\n");
    } else {
        std::printf("{\"cards\":[");
        for (size_t i = 0; i < cards.size(); i++) {
            if (i) std::printf(",");
            std::printf("{\"num\":%d,\"id\":\"%s\",\"name\":\"%s\"}", cards[i].num,
                        cards[i].id.c_str(), cards[i].name.c_str());
        }
        std::printf("],\"capture_pcms\":[");
        bool first = true;
        for (auto& p : pcms) {
            if (!p.capture) continue;
            if (!first) std::printf(",");
            first = false;
            std::printf("{\"card\":%d,\"device\":%d,\"name\":\"%s\"}", p.card, p.device, p.name.c_str());
        }
        std::printf("]}\n");
    }
    return cards.empty() ? 1 : 0;
}
