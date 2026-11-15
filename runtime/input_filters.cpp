// input_ports::add_filter and run_filters (input_ports.h). Apart from input_ports.cpp, which
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

}  // namespace

void add_filter(void (*fn)(int port, PadState& p)) { filters().push_back(fn); }

void run_filters(int port, PadState& p) {
    for (auto fn : filters()) fn(port, p);
}

}  // namespace input_ports
