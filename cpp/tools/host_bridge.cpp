// SPDX-License-Identifier: Apache-2.0
// Host-side bridge for conformance tests against real host code (e.g. Kalico's
// box_protocol.py): runs cfs::App on the simulator and exchanges frames over
// stdin/stdout, one command per line:
//   insert <slot> | remove <slot> | advance <ms>
//   frame <hex> <timeout_ms>   -> "reply <hex>" or "reply -"
// The box starts unassigned (0xFE), as after a power-up.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cfs/app.hpp"
#include "sim.hpp"

namespace {
std::vector<uint8_t> unhex(const char* s) {
    std::vector<uint8_t> out;
    while (s[0] && s[1] && s[0] != ' ') {
        unsigned v = 0;
        if (std::sscanf(s, "%2x", &v) != 1) break;
        out.push_back(static_cast<uint8_t>(v));
        s += 2;
    }
    return out;
}
}  // namespace

int main() {
    cfs::sim::World w;
    cfs::Hardware hw = w.hardware();
    cfs::App app(hw, 0xFE);
    app.init();
    w.tick_hook = [&] { app.background(); };
    char line[1024];
    while (std::fgets(line, sizeof line, stdin)) {
        char cmd[16] = {};
        int arg = 0;
        if (std::sscanf(line, "%15s", cmd) != 1) continue;
        if (!std::strcmp(cmd, "insert") && std::sscanf(line + 6, "%d", &arg) == 1) {
            w.insert(arg);
        } else if (!std::strcmp(cmd, "remove") && std::sscanf(line + 6, "%d", &arg) == 1) {
            w.remove(arg);
        } else if (!std::strcmp(cmd, "advance") && std::sscanf(line + 7, "%d", &arg) == 1) {
            const uint32_t end = w.now_ms() + static_cast<uint32_t>(arg);
            while (static_cast<int32_t>(w.now_ms() - end) < 0) {
                app.loop_once();
                w.sleep_ms(1);
            }
        } else if (!std::strcmp(cmd, "frame")) {
            const char* p = line + 5;
            while (*p == ' ') ++p;
            const std::vector<uint8_t> f = unhex(p);
            const char* t = std::strchr(p, ' ');
            const uint32_t timeout = t ? static_cast<uint32_t>(std::atoi(t + 1)) : 1000;
            w.tx.clear();
            w.inject(w.now_ms(), f);
            const uint32_t end = w.now_ms() + timeout;
            std::vector<std::vector<uint8_t>> frames;
            while (static_cast<int32_t>(w.now_ms() - end) < 0) {
                app.loop_once();
                frames = w.sent_frames();
                if (!frames.empty()) break;
                w.sleep_ms(1);
            }
            if (frames.empty()) {
                std::printf("reply -\n");
            } else {
                std::printf("reply ");
                for (uint8_t b : frames.front()) std::printf("%02x", b);
                std::printf("\n");
            }
        } else {
            std::printf("error unknown command\n");
        }
        std::fflush(stdout);
    }
    return 0;
}
