// input_ports::add_filter, run_filters, raw and want (input_ports.h). Apart from input_ports.cpp, which
// only the desktop frontend links, so that a game's SOURCES may add a filter in every
// target; the headless ones simply never run it.
#include "input_ports.h"

#include <vector>

namespace input_ports {
namespace {

// A function-local static: a game adds its filter from a static initializer.
std::vector<void (*)(int, PadState&)>& filters() {
    static std::vector<void (*)(int, PadState&)> v;
    return v;
}

Raw g_raw[4];
bool g_has_raw[4];
unsigned g_want[4];

}  // namespace

void add_filter(void (*fn)(int port, PadState& p)) { filters().push_back(fn); }

void run_filters(int port, PadState& p) {
    for (auto fn : filters()) fn(port, p);
}

bool raw(int port, Raw& out) {
    out = Raw();
    if (port < 0 || port > 3 || !g_has_raw[port]) return false;
    out = g_raw[port];
    return true;
}

void want(int port, unsigned what) {
    if (port >= 0 && port < 4) g_want[port] = what;
}

unsigned wanted(int port) { return port >= 0 && port < 4 ? g_want[port] : 0; }

void set_raw(int port, const Raw& r) {
    if (port < 0 || port > 3) return;
    g_raw[port] = r;
    g_has_raw[port] = true;
}

}  // namespace input_ports
