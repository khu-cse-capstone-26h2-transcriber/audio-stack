// astk-list-devices — inspect and suggest ALSA capture devices.
// usage: astk-list-devices [--json]
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
        for (auto& p : pcms) {
            if (p.capture) {
                const char* card_id = "?";
                for (const auto& c : cards)
                    if (c.num == p.card) { card_id = c.id.c_str(); break; }
                std::printf("  plughw:%d,%d  card=%s  %s\n", p.card, p.device,
                            card_id, p.name.c_str());
            }
        }
        std::printf("\n=== suggestions ===\n");
        // Prioritize the onboard 3.5 mm microphone input on Ubuntu for Qualcomm.
        for (auto& c : cards) {
            if (c.id.find("qcs6490") != std::string::npos || c.name.find("RB3") != std::string::npos ||
                c.name.find("q6") != std::string::npos) {
                std::printf("  RUBIK Pi/RB3 onboard audio card detected (id=%s).\n",
                            c.id.c_str());
                for (const auto& p : pcms) {
                    if (p.card != c.num || p.device != 2 || !p.capture) continue;
                    std::printf("  3.5 mm headset microphone candidate (verify the image's route):\n");
                    std::printf("    amixer -c %d cset \"name=MultiMedia3 Mixer PRIMARY_MI2S_TX\" 1\n", c.num);
                    std::printf("    --device plughw:CARD=%s,DEV=2   (48 kHz board profile)\n", c.id.c_str());
                }
            }
        }
        std::printf("  Select a listed capture PCM if the board's device numbering differs.\n");
        std::printf("  Without a microphone (see README.md):\n"
                    "    python3 tools/feed_wav.py samples/jfk.wav --socket /tmp/astk-file/engine.sock\n");
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
