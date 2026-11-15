// Scripted controller input. See input_script.h.
#include "input_script.h"
#include "runtime.h"
#include "gx/render.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct ScriptedPress { uint32_t at, dur; uint16_t buttons; int sx, sy, cx, cy, mx, my; int port; };
std::vector<ScriptedPress> g_script;
bool g_log;
}  // namespace

void input_script_init(const char* spec) {
    g_log = getenv("GCN_INPUT_LOG") != nullptr;
    if (!spec || !*spec) spec = getenv("GCN_INPUT");
    if (!spec || !*spec) return;
    std::string s = spec;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        // "2/900:A:10": port 2.
        int port = 0;
        const size_t slash = item.find('/');
        if (slash != std::string::npos && slash < item.find(':')) {
            port = atoi(item.c_str()) - 1;
            item = item.substr(slash + 1);
            if (port < 0 || port > 3) continue;
        }
        size_t c1 = item.find(':'), c2 = item.rfind(':');
        if (c1 == std::string::npos || c2 == c1) continue;
        ScriptedPress p{(uint32_t)atoi(item.c_str()), (uint32_t)atoi(item.c_str() + c2 + 1), 0, 0, 0, 0, 0, 0, 0, port};
        std::string btns = item.substr(c1 + 1, c2 - c1 - 1);
        size_t bp = 0;
        while (bp <= btns.size()) {
            size_t be = btns.find('+', bp);
            if (be == std::string::npos) be = btns.size();
            std::string b = btns.substr(bp, be - bp);
            bp = be + 1;
            if (b == "A") p.buttons |= PAD_A; else if (b == "B") p.buttons |= PAD_B;
            else if (b == "X") p.buttons |= PAD_X; else if (b == "Y") p.buttons |= PAD_Y;
            else if (b == "Z") p.buttons |= PAD_Z; else if (b == "L") p.buttons |= PAD_L;
            else if (b == "R") p.buttons |= PAD_R; else if (b == "START") p.buttons |= PAD_START;
            else if (b == "UP") p.buttons |= PAD_UP; else if (b == "DOWN") p.buttons |= PAD_DOWN;
            else if (b == "LEFT") p.buttons |= PAD_LEFT; else if (b == "RIGHT") p.buttons |= PAD_RIGHT;
            // SL SR SU SD push the stick all the way (100); SR19 and the like push it that far.
            // MX5 MY-3: the mouse moves that many counts each update (right and down positive).
            else if (b.size() >= 3 && b[0] == 'M' && (b[1] == 'X' || b[1] == 'Y')) {
                (b[1] == 'X' ? p.mx : p.my) = atoi(b.c_str() + 2);
            }
            // CL CR CU CD do the same with the C stick.
            else if (b.size() >= 2 && (b[0] == 'S' || b[0] == 'C') && strchr("LRUD", b[1])) {
                const int amount = b.size() > 2 ? atoi(b.c_str() + 2) : 100;
                int& x = b[0] == 'S' ? p.sx : p.cx;
                int& y = b[0] == 'S' ? p.sy : p.cy;
                if (b[1] == 'L') x = -amount; else if (b[1] == 'R') x = amount;
                else if (b[1] == 'U') y = amount; else y = -amount;
            }
        }
        g_script.push_back(p);
    }
    if (g_log) fprintf(stderr, "[input] %zu scripted press(es)\n", g_script.size());
}

unsigned input_script_ports() {
    unsigned m = 0;
    for (const auto& s : g_script) m |= 1u << s.port;
    return m;
}

bool input_script_now(int port, int* mx, int* my) {
    if (mx) *mx = 0;
    if (my) *my = 0;
    if (g_script.empty()) return false;
    const uint32_t frame = gx::g_frames_submitted.load();
    bool any = false;
    for (const auto& s : g_script) {
        if (s.port != port || frame < s.at || frame >= s.at + s.dur) continue;
        any = true;
        if (mx) *mx += s.mx;
        if (my) *my += s.my;
    }
    return any;
}

void input_script_apply(PadState& p, int port) {
    if (g_script.empty()) return;
    uint32_t frame = gx::g_frames_submitted.load();  // time base: presented game frames
    for (auto& s : g_script) {
        if (s.port == port && frame >= s.at && frame < s.at + s.dur) {
            if (g_log) fprintf(stderr, "[input] frame %u port %d press %04X\n", frame, port + 1, s.buttons);
            p.buttons |= s.buttons;
            if (s.sx) p.stick_x = (uint8_t)(128 + s.sx);
            if (s.sy) p.stick_y = (uint8_t)(128 + s.sy);
            if (s.cx) p.cstick_x = (uint8_t)(128 + s.cx);
            if (s.cy) p.cstick_y = (uint8_t)(128 + s.cy);
            if (s.buttons & PAD_L) p.trig_l = 255;
            if (s.buttons & PAD_R) p.trig_r = 255;
        }
    }
}
