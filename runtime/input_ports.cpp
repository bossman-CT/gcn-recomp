// See input_ports.h.
#include "input_ports.h"
#include "input_map.h"
#include "input_script.h"
#include "runtime.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
// After the standard headers: SDL_endian.h trips over clang's intrinsics otherwise.
#include <SDL.h>

namespace input_ports {
namespace {

enum Key {
    kDevice, kConnected, kLayout, kGyro, kGyroSensitivity, kGyroDeadzone, kGyroSmoothing,
    kGyroHold, kGyroHorizontal, kGyroInvertX, kGyroInvertY, kCount
};

const SettingInfo kSettings[kCount] = {
    {"device", nullptr, "auto", 0, 0, "Which controller takes the port: auto (the next to connect), none, or a device's name"},
    {"connected", "auto|always", "auto", 0, 0, "always: the port reads as plugged in even without a controller"},
    {"layout", "positional|modern", "positional", 0, 0,
     "positional: the face buttons as they sit; modern: bottom confirms, right goes back"},
    {"gyro", "off|stick|cstick", "off", 0, 0, "The controller's gyro onto a stick"},
    {"gyro_sensitivity", nullptr, "1", 0.05f, 10.0f, "Full deflection at 100 degrees a second, times this"},
    {"gyro_deadzone", nullptr, "2", 0.0f, 30.0f, "Turns slower than this, in degrees a second, read as nothing"},
    {"gyro_smoothing", nullptr, "0.03", 0.0f, 0.5f, "The time constant of a low-pass on the gyro, in seconds"},
    {"gyro_hold", nullptr, "none", 0, 0, "Aim only while this gamepad control is held"},
    {"gyro_horizontal", "yaw|roll", "yaw", 0, 0, "Turning (yaw) or tilting (roll) the controller steers left and right"},
    {"gyro_invert_x", "off|on", "off", 0, 0, "Left and right the other way round"},
    {"gyro_invert_y", "off|on", "off", 0, 0, "Tilting the controller up pushes the stick down"},
};

int find_key(const char* key) {
    for (int i = 0; i < kCount; i++)
        if (!strcmp(kSettings[i].key, key)) return i;
    return -1;
}

// The position of `v` among "a|b|c", or -1.
int choice_index(const char* choices, const std::string& v) {
    int i = 0;
    for (const char* p = choices;; i++) {
        const char* e = strchr(p, '|');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (v.size() == n && !SDL_strncasecmp(v.c_str(), p, n)) return i;
        if (!e) return -1;
        p = e + 1;
    }
}

struct Config {
    enum { Auto, None, Named } device = Auto;
    std::string device_name;
    int device_nth = 1;
    bool always = false, modern = false;
    int gyro = 0;  // 0 off, 1 stick, 2 cstick
    float sensitivity = 1.0f, deadzone = 2.0f, smoothing = 0.03f;
    int hold_button = -1, hold_axis = -1;
    bool roll = false, invert_x = false, invert_y = false;
};

struct Port {
    SDL_GameController* ctl = nullptr;
    SDL_JoystickID id = -1;
    int type = -1;  // SDL_GameControllerType, -1 without a controller
    std::string label;
    std::string last;  // GUID and label of the controller it has or last had
    bool has_gyro = false, gyro_on = false;
    float gx = 0.0f, gy = 0.0f;  // the smoothed rate, degrees a second
    uint64_t gyro_t = 0;
    bool rumbling = false;
    Config cfg;
    std::string override_value[kCount];
    bool overridden[kCount] = {};
};

Port g_ports[4];
void (*g_filter)(int, PadState&) = nullptr;
std::vector<SDL_JoystickID> g_said_full;  // controllers told there is no free port

// "Name #2" -> ("Name", 2)
void split_nth(const std::string& s, std::string& name, int& nth) {
    name = s;
    nth = 1;
    const size_t h = s.rfind(" #");
    if (h == std::string::npos || h + 2 >= s.size()) return;
    for (size_t i = h + 2; i < s.size(); i++)
        if (!isdigit((unsigned char)s[i])) return;
    name = s.substr(0, h);
    nth = atoi(s.c_str() + h + 2);
}

std::string value_of(int port, int key) {
    const Port& P = g_ports[port];
    if (P.overridden[key]) return P.override_value[key];
    // Which controller takes a port can't depend on the controller's type.
    if (const char* v = input_map_setting(port, key == kDevice ? -1 : P.type, kSettings[key].key)) return v;
    return kSettings[key].def;
}

float number_of(int port, int key) { return (float)atof(value_of(port, key).c_str()); }
int choice_of(int port, int key) { return std::max(choice_index(kSettings[key].choices, value_of(port, key)), 0); }

void resolve(int port) {
    Config& c = g_ports[port].cfg;
    c = Config();
    const std::string dev = value_of(port, kDevice);
    if (!SDL_strcasecmp(dev.c_str(), "auto")) c.device = Config::Auto;
    else if (!SDL_strcasecmp(dev.c_str(), "none")) c.device = Config::None;
    else {
        c.device = Config::Named;
        split_nth(dev, c.device_name, c.device_nth);
    }
    c.always = choice_of(port, kConnected) == 1;
    c.modern = choice_of(port, kLayout) == 1;
    c.gyro = choice_of(port, kGyro);
    c.sensitivity = number_of(port, kGyroSensitivity);
    c.deadzone = number_of(port, kGyroDeadzone);
    c.smoothing = number_of(port, kGyroSmoothing);
    const std::string hold = value_of(port, kGyroHold);
    if (SDL_strcasecmp(hold.c_str(), "none") && !input_map_gamepad_control(hold, c.hold_button, c.hold_axis))
        c.hold_button = c.hold_axis = -1;
    c.roll = choice_of(port, kGyroHorizontal) == 1;
    c.invert_x = choice_of(port, kGyroInvertX) == 1;
    c.invert_y = choice_of(port, kGyroInvertY) == 1;
}

bool names(const Config& c, const std::string& label) {
    std::string name;
    int nth;
    split_nth(label, name, nth);
    return c.device == Config::Named && nth == c.device_nth && !SDL_strcasecmp(name.c_str(), c.device_name.c_str());
}

// The port a newly opened controller takes, or -1: a port that names it, else the port it
// last had, else the lowest free one.
int pick(const std::string& label, const std::string& last) {
    for (int p = 0; p < 4; p++)
        if (!g_ports[p].ctl && names(g_ports[p].cfg, label)) return p;
    for (int p = 0; p < 4; p++)
        if (!g_ports[p].ctl && g_ports[p].cfg.device == Config::Auto && g_ports[p].last == last) return p;
    for (int p = 0; p < 4; p++)
        if (!g_ports[p].ctl && g_ports[p].cfg.device == Config::Auto) return p;
    return -1;
}

void attach(int port, SDL_GameController* c, SDL_JoystickID id, const std::string& label, const std::string& last) {
    Port& P = g_ports[port];
    P.ctl = c;
    P.id = id;
    P.label = label;
    P.last = last;
    P.type = (int)SDL_GameControllerGetType(c);
    P.has_gyro = SDL_GameControllerHasSensor(c, SDL_SENSOR_GYRO) == SDL_TRUE;
    P.gyro_on = false;
    P.rumbling = false;
    resolve(port);
    fprintf(stderr, "[input] port %d: %s\n", port + 1, describe(port).c_str());
}

// Every game controller SDL has that is in no port yet, in SDL's order, given a port.
void assign_all() {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i) || input_map_binds_index(i)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool placed = false;
        for (const auto& P : g_ports) placed |= P.ctl && P.id == id;
        if (placed) continue;
        SDL_GameController* c = SDL_GameControllerOpen(i);
        if (!c) continue;
        SDL_Joystick* j = SDL_GameControllerGetJoystick(c);
        const std::string label = input_map_device_label(j);
        char guid[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof(guid));
        const std::string last = std::string(guid) + "|" + label;
        const int port = pick(label, last);
        if (port < 0) {
            if (std::find(g_said_full.begin(), g_said_full.end(), id) == g_said_full.end()) {
                g_said_full.push_back(id);
                fprintf(stderr, "[input] \"%s\": no port is free for it\n", label.c_str());
            }
            SDL_GameControllerClose(c);
            continue;
        }
        attach(port, c, id, label, last);
    }
}

uint8_t axis_to_u8(int v, bool invert) {
    float f = v / 32767.0f;
    if (invert) f = -f;
    const int r = 128 + (int)(f * 100.0f);
    return (uint8_t)std::clamp(r, 0, 255);
}

void keyboard(PadState& p) {
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    if (k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & KMOD_ALT)) p.buttons |= PAD_START;  // Alt+Enter is fullscreen
    if (k[SDL_SCANCODE_X]) p.buttons |= PAD_A;
    if (k[SDL_SCANCODE_Z]) p.buttons |= PAD_B;
    if (k[SDL_SCANCODE_C]) p.buttons |= PAD_X;
    if (k[SDL_SCANCODE_S]) p.buttons |= PAD_Y;
    if (k[SDL_SCANCODE_D]) p.buttons |= PAD_Z;
    if (k[SDL_SCANCODE_Q]) { p.buttons |= PAD_L; p.trig_l = 255; }
    if (k[SDL_SCANCODE_W]) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (k[SDL_SCANCODE_I]) p.buttons |= PAD_UP;
    if (k[SDL_SCANCODE_K]) p.buttons |= PAD_DOWN;
    if (k[SDL_SCANCODE_J]) p.buttons |= PAD_LEFT;
    if (k[SDL_SCANCODE_L]) p.buttons |= PAD_RIGHT;
    int sx = 0, sy = 0;
    if (k[SDL_SCANCODE_LEFT]) sx -= 100;
    if (k[SDL_SCANCODE_RIGHT]) sx += 100;
    if (k[SDL_SCANCODE_UP]) sy += 100;
    if (k[SDL_SCANCODE_DOWN]) sy -= 100;
    if (sx || sy) {
        p.stick_x = (uint8_t)(128 + sx);
        p.stick_y = (uint8_t)(128 + sy);
    }
}

struct Face { SDL_GameControllerButton src; uint16_t bit; };
// SDL's a b x y are the bottom, right, left and top buttons.
const Face kPositional[4] = {{SDL_CONTROLLER_BUTTON_A, PAD_A}, {SDL_CONTROLLER_BUTTON_X, PAD_B},
                             {SDL_CONTROLLER_BUTTON_B, PAD_X}, {SDL_CONTROLLER_BUTTON_Y, PAD_Y}};
const Face kModern[4] = {{SDL_CONTROLLER_BUTTON_A, PAD_A}, {SDL_CONTROLLER_BUTTON_B, PAD_B},
                         {SDL_CONTROLLER_BUTTON_X, PAD_X}, {SDL_CONTROLLER_BUTTON_Y, PAD_Y}};
const Face kOthers[] = {
    {SDL_CONTROLLER_BUTTON_START, PAD_START}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_Z},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT},
};

// The default gamepad mapping, less what controls.txt takes over on this port: the pad
// controls it binds from the gamepad, and the gamepad controls it binds. A stick or
// trigger is taken over only while it is pushed, so that it can share the pad with the
// keyboard and a headset's controllers.
void gamepad(const Port& P, int port, PadState& p) {
    SDL_GameController* c = P.ctl;
    uint32_t skip = input_map_gamepad_override(port, P.type);
    const uint64_t used = input_map_gamepad_sources(port, P.type);
    auto axis_used = [&](SDL_GameControllerAxis a) { return (used >> (32 + a) & 1) != 0; };
    if (axis_used(SDL_CONTROLLER_AXIS_LEFTX)) skip |= INPUT_MAP_STICK_X;
    if (axis_used(SDL_CONTROLLER_AXIS_LEFTY)) skip |= INPUT_MAP_STICK_Y;
    if (axis_used(SDL_CONTROLLER_AXIS_RIGHTX)) skip |= INPUT_MAP_CSTICK_X;
    if (axis_used(SDL_CONTROLLER_AXIS_RIGHTY)) skip |= INPUT_MAP_CSTICK_Y;
    if (axis_used(SDL_CONTROLLER_AXIS_TRIGGERLEFT)) skip |= PAD_L;
    if (axis_used(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) skip |= PAD_R;
    auto press = [&](const Face& f) {
        if (!(skip & f.bit) && !(used >> f.src & 1) && SDL_GameControllerGetButton(c, f.src)) p.buttons |= f.bit;
    };
    for (const Face& f : P.cfg.modern ? kModern : kPositional) press(f);
    for (const Face& f : kOthers) press(f);
    const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    const int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (abs(lx) > 4000 || abs(ly) > 4000) {
        if (!(skip & INPUT_MAP_STICK_X)) p.stick_x = axis_to_u8(lx, false);
        if (!(skip & INPUT_MAP_STICK_Y)) p.stick_y = axis_to_u8(ly, true);
    }
    const int rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    const int ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    if (abs(rx) > 1000 || abs(ry) > 1000) {
        if (!(skip & INPUT_MAP_CSTICK_X)) p.cstick_x = axis_to_u8(rx, false);
        if (!(skip & INPUT_MAP_CSTICK_Y)) p.cstick_y = axis_to_u8(ry, true);
    }
    const int lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    const int rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (!(skip & PAD_L) && lt > 1000) { p.trig_l = std::max(p.trig_l, (uint8_t)(lt * 255 / 32767)); if (lt > 30000) p.buttons |= PAD_L; }
    if (!(skip & PAD_R) && rt > 1000) { p.trig_r = std::max(p.trig_r, (uint8_t)(rt * 255 / 32767)); if (rt > 30000) p.buttons |= PAD_R; }
}

bool held(SDL_GameController* c, const Config& cfg) {
    if (cfg.hold_button >= 0) return SDL_GameControllerGetButton(c, (SDL_GameControllerButton)cfg.hold_button);
    if (cfg.hold_axis >= 0) return abs(SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)cfg.hold_axis)) > 16000;
    return true;
}

// -1 to 1 added onto a stick axis, spread over the axis's range (input_map_stick_range) so
// that the first movement past the dead zone is already one the game responds to.
void add_to_stick(int axis, float v, uint8_t& out) {
    if (v == 0.0f) return;
    float lo, hi;
    input_map_stick_range(axis, lo, hi);
    const int off = (int)(lo + (hi - lo) * std::min(std::fabs(v), 1.0f) + 0.001f);
    const int was = (int)out - 128;
    const int lim = std::max((int)hi, abs(was));
    out = (uint8_t)(128 + std::clamp(was + (v < 0.0f ? -off : off), -lim, lim));
}

void gyro(Port& P, PadState& p) {
    const Config& c = P.cfg;
    const bool want = c.gyro != 0 && P.has_gyro;
    if (want != P.gyro_on) {
        SDL_GameControllerSetSensorEnabled(P.ctl, SDL_SENSOR_GYRO, want ? SDL_TRUE : SDL_FALSE);
        P.gyro_on = want;
        P.gx = P.gy = 0.0f;
        P.gyro_t = 0;
    }
    if (!want) return;
    const uint64_t now = SDL_GetPerformanceCounter();
    const float dt = P.gyro_t ? (float)(now - P.gyro_t) / (float)SDL_GetPerformanceFrequency() : 0.0f;
    P.gyro_t = now;
    if (!held(P.ctl, c)) {
        P.gx = P.gy = 0.0f;
        return;
    }
    float d[3];
    if (SDL_GameControllerGetSensorData(P.ctl, SDL_SENSOR_GYRO, d, 3) != 0) return;
    // SDL: radians a second, counter-clockwise about x (right), y (up) and z (towards the
    // player). Right and up positive, as on the pad.
    const float deg = 57.2957795f;
    float x = -(c.roll ? d[2] : d[1]) * deg, y = d[0] * deg;
    if (c.invert_x) x = -x;
    if (c.invert_y) y = -y;
    const float m = std::hypot(x, y);
    if (m <= c.deadzone) x = y = 0.0f;
    else {
        const float k = (m - c.deadzone) / m;
        x *= k;
        y *= k;
    }
    if (c.smoothing > 0.0f && dt > 0.0f) {
        const float a = 1.0f - std::exp(-dt / c.smoothing);
        P.gx += (x - P.gx) * a;
        P.gy += (y - P.gy) * a;
    } else {
        P.gx = x;
        P.gy = y;
    }
    const bool on_main = c.gyro == 1;
    add_to_stick(on_main ? 0 : 2, P.gx * c.sensitivity / 100.0f, on_main ? p.stick_x : p.cstick_x);
    add_to_stick(on_main ? 1 : 3, P.gy * c.sensitivity / 100.0f, on_main ? p.stick_y : p.cstick_y);
}

}  // namespace

void init() {
    for (int p = 0; p < 4; p++) resolve(p);
    for (int p = 1; p < 4; p++)
        if (g_ports[p].cfg.always) fprintf(stderr, "[input] port %d: connected without a controller\n", p + 1);
}

void event(const SDL_Event& e) {
    if (e.type == SDL_CONTROLLERDEVICEADDED) {
        assign_all();
    } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        g_said_full.erase(std::remove(g_said_full.begin(), g_said_full.end(), e.cdevice.which), g_said_full.end());
        for (int p = 0; p < 4; p++) {
            Port& P = g_ports[p];
            if (!P.ctl || P.id != e.cdevice.which) continue;
            fprintf(stderr, "[input] port %d: \"%s\" disconnected\n", p + 1, P.label.c_str());
            SDL_GameControllerClose(P.ctl);
            P.ctl = nullptr;
            P.id = -1;
            P.type = -1;
            P.has_gyro = P.gyro_on = P.rumbling = false;
            resolve(p);
        }
        assign_all();  // one that found no port may have one now
    }
}

void merge(int port, PadState& p) {
    Port& P = g_ports[port];
    if (port == 0) keyboard(p);
    if (P.ctl) gamepad(P, port, p);
    input_map_apply(port, p, P.ctl, P.type);
    if (P.ctl) gyro(P, p);
    // The pad's rumble motor on the controller, renewed while it runs.
    const bool motor = pad_motor(port) && !guest_paused();  // a paused game leaves its motor on
    if (P.ctl && (motor || P.rumbling)) SDL_GameControllerRumble(P.ctl, motor ? 0x6000 : 0, motor ? 0xA000 : 0, motor ? 200 : 0);
    P.rumbling = motor;
}

void update() {
    const unsigned scripted = input_script_ports();
    for (int port = 0; port < 4; port++) {
        PadState p;
        // A port the script drives is plugged in, as a controller would be.
        p.connected = connected(port) || (scripted >> port & 1);
        if (p.connected) merge(port, p);
        if (g_filter) g_filter(port, p);
        input_script_apply(p, port);
        pad_set_state(port, p);
    }
}

void set_filter(void (*fn)(int port, PadState& p)) { g_filter = fn; }

bool connected(int port) {
    if (port < 0 || port > 3) return false;
    return port == 0 || g_ports[port].ctl || g_ports[port].cfg.always;
}

std::string describe(int port) {
    if (port < 0 || port > 3) return "";
    const Port& P = g_ports[port];
    std::string s = port == 0 ? "keyboard" : "";
    if (P.ctl) {
        if (!s.empty()) s += " and ";
        s += "\"" + P.label + "\" (" + type_name(P.type) + (P.has_gyro ? ", gyro" : "") + ")";
    } else if (port > 0 && P.cfg.always) {
        s = "no controller, connected";
    }
    return s;
}

const SettingInfo* settings(int* count) {
    if (count) *count = kCount;
    return kSettings;
}

std::string setting_get(int port, const char* key) {
    const int k = find_key(key);
    if (k < 0 || port < 0 || port > 3) return "";
    return value_of(port, k);
}

bool setting_set(int port, const char* key, const std::string& value, std::string* err) {
    std::string e;
    if (port < 0 || port > 3) e = "no such port";
    else if (!check_setting(key, value, e)) {}
    if (!e.empty()) {
        if (err) *err = e;
        return false;
    }
    const int k = find_key(key);
    g_ports[port].override_value[k] = value;
    g_ports[port].overridden[k] = true;
    resolve(port);
    return true;
}

bool is_setting(const std::string& key) { return find_key(key.c_str()) >= 0; }

bool check_setting(const std::string& key, const std::string& value, std::string& err) {
    const int k = find_key(key.c_str());
    if (k < 0) { err = "unknown setting \"" + key + "\""; return false; }
    const SettingInfo& s = kSettings[k];
    if (s.choices) {
        if (choice_index(s.choices, value) >= 0) return true;
        std::string list = s.choices;
        std::replace(list.begin(), list.end(), '|', ' ');
        err = key + " is one of: " + list;
        return false;
    }
    if (k == kDevice) {
        if (!value.empty()) return true;
        err = "device is auto, none or a device's name";
        return false;
    }
    if (k == kGyroHold) {
        int b, a;
        if (!SDL_strcasecmp(value.c_str(), "none") || input_map_gamepad_control(value, b, a)) return true;
        err = "gyro_hold is none or a gamepad control (leftshoulder, lefttrigger, l1, ...)";
        return false;
    }
    char* end;
    const float v = strtof(value.c_str(), &end);
    if (value.empty() || *end || v < s.lo || v > s.hi) {
        char buf[96];
        snprintf(buf, sizeof(buf), " is a number from %g to %g", s.lo, s.hi);
        err = key + buf;
        return false;
    }
    return true;
}

namespace {
constexpr unsigned bit(SDL_GameControllerType t) { return 1u << (unsigned)t; }
const unsigned kXbox = bit(SDL_CONTROLLER_TYPE_XBOX360) | bit(SDL_CONTROLLER_TYPE_XBOXONE);
const unsigned kPlayStation = bit(SDL_CONTROLLER_TYPE_PS3) | bit(SDL_CONTROLLER_TYPE_PS4) | bit(SDL_CONTROLLER_TYPE_PS5);
const unsigned kJoyCon = bit(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT) |
                         bit(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT) |
                         bit(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR);
const struct { const char* name; unsigned mask; } kTypes[] = {
    {"xbox360", bit(SDL_CONTROLLER_TYPE_XBOX360)}, {"xboxone", bit(SDL_CONTROLLER_TYPE_XBOXONE)},
    {"ps3", bit(SDL_CONTROLLER_TYPE_PS3)}, {"ps4", bit(SDL_CONTROLLER_TYPE_PS4)}, {"ps5", bit(SDL_CONTROLLER_TYPE_PS5)},
    {"switch", bit(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO)}, {"joycon", kJoyCon},
    {"virtual", bit(SDL_CONTROLLER_TYPE_VIRTUAL)}, {"luna", bit(SDL_CONTROLLER_TYPE_AMAZON_LUNA)},
    {"stadia", bit(SDL_CONTROLLER_TYPE_GOOGLE_STADIA)}, {"shield", bit(SDL_CONTROLLER_TYPE_NVIDIA_SHIELD)},
    {"other", bit(SDL_CONTROLLER_TYPE_UNKNOWN)},
    {"xbox", kXbox}, {"playstation", kPlayStation},
    {"nintendo", bit(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) | kJoyCon},
};
}  // namespace

unsigned type_mask(const std::string& name) {
    for (const auto& t : kTypes)
        if (!SDL_strcasecmp(t.name, name.c_str())) return t.mask;
    return 0;
}

const char* type_name(int sdl_type) {
    if (sdl_type < 0) return "none";
    for (const auto& t : kTypes)
        if (t.mask == (1u << (unsigned)sdl_type)) return t.name;
    for (const auto& t : kTypes)
        if (t.mask & (1u << (unsigned)sdl_type)) return t.name;
    return "other";
}

}  // namespace input_ports
