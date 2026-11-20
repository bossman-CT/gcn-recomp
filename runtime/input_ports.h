// The GameCube's four controller ports on the desktop: which game controller drives each,
// how a gamepad's controls become the pad's (the face-button layout), and gyro aiming.
//
// Ports. Every game controller SDL opens gets a port of its own, 1 to 4, in the order the
// controllers connect (those present at startup in SDL's order). A port keeps its
// controller until it is unplugged; the others stay where they are. A controller plugged
// in later takes the port it last had, if that is free, and otherwise the lowest free one.
// The keyboard is always on port 1, with the first controller. A port with no controller
// reads as unplugged, as on the console, unless controls.txt says `connected = always` for
// it (a second player on the keyboard). A controller that controls.txt binds by name
// ("<device>" button 3) is the file's alone and takes no port. Each assignment is printed
// on stderr as it happens.
//
// Per-port settings, in controls.txt (input_map.h has the file's sections) or from a menu
// through setting_get/setting_set below:
//
//   device = auto | none | "<name>[ #n]"   which controller takes the port (default auto:
//                                          the next to connect; none keeps it empty)
//   connected = auto | always              always: plugged in even without a controller
//   layout = positional | modern           the face buttons (default positional):
//       positional  as the buttons sit: bottom A, left B, right X, top Y
//       modern      bottom A (confirm), right B (back), left X, top Y
//   gyro = off | stick | cstick            the controller's gyro onto that stick (off)
//   gyro_sensitivity = <f>                 full deflection at 100 degrees a second, times
//                                          this (default 1)
//   gyro_deadzone = <degrees per second>   slower turns read as nothing (default 2)
//   gyro_smoothing = <seconds>             the time constant of a low-pass (default 0.03;
//                                          0 for none)
//   gyro_hold = none | <gamepad control>   aim only while that is held (default none)
//   gyro_horizontal = yaw | roll           turning or tilting the controller steers
//   gyro_invert_x = off | on
//   gyro_invert_y = off | on               on: tilting the controller up pushes down
//
// Gamepad controls are SDL's names for a standard controller's: a b x y (bottom, right,
// left, top), back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown
// dpleft dpright misc1 paddle1-4 touchpad, and the axes leftx lefty rightx righty
// lefttrigger righttrigger. PlayStation and Xbox names work too: cross circle square
// triangle l1 r1 l2 r2 l3 r3 share options ps; lb rb lt rt ls rs view menu.
//
// The gyro is read as a rate: turning the controller pushes the stick in proportion to
// how fast it turns, and holding it still lets the stick go. A game that aims by the
// stick then aims as far as the controller has turned.
#pragma once
#include "hw/pad.h"
#include <string>

union SDL_Event;

namespace input_ports {

// After SDL_Init (with SDL_INIT_GAMECONTROLLER) and input_map_load.
void init();
// Every SDL event may be passed: controllers coming and going.
void event(const SDL_Event& e);
// Port `port` (0 to 3) as the desktop drives it, merged into `p`: on port 0 the keyboard,
// then the port's controller through its layout, controls.txt's bindings for the port,
// and the gyro. Runs the port's rumble from the game's motor. Leaves `p.connected` alone.
void merge(int port, PadState& p);
// All four ports, built and handed to the guest: merge, scripted input (input_script.h),
// the game's filters (add_filter), the frontend's (set_filter), pad_set_state. Once per pass
// of the frontend's loop.
void update();
// A frontend's hook on each port's pad, the last before the guest: a menu holding the
// game's input while it is open, say.
void set_filter(void (*fn)(int port, PadState& p));
// A game's hook on each port's pad, before the frontend's (set_filter): a control scheme
// reshaping what the controller does for the game, which may depend on what the game is
// doing (read from guest memory). Called for every port, connected or not. Several may be
// added; they run in the order added. The input log records the pad they leave, so a
// replay needs nothing more.
void add_filter(void (*fn)(int port, PadState& p));
// The game's filters on one port's pad, in order: for a frontend's update. Defined in
// input_filters.cpp, which every target links, so that a game's SOURCES can add a filter
// in the benchmark too.
void run_filters(int port, PadState& p);

// What drives a port, before any mapping, for a filter with its own idea of how a
// controller should play (a game's aiming scheme). Filled by the desktop frontend each
// update; elsewhere it stays empty.
struct Raw {
    bool pad = false;                      // a game controller drives the port
    float lx = 0, ly = 0, rx = 0, ry = 0;  // its sticks, -1 to 1, right and up positive, no dead zone
    bool gyro = false;                     // its gyro, while wanted (below) and it has one
    float gx = 0, gy = 0;                  // degrees a second, right and up positive, after the port's
                                           // gyro dead zone, smoothing, hold control and inversion
    bool mouse = false;                    // the mouse, on port 1, while wanted and captured
    float mx = 0, my = 0;                  // its motion since the last update, in counts, right and down positive
    unsigned mbuttons = 0;                 // held: 1 left, 2 middle, 4 right, 8 and 16 the side buttons
    int wheel = 0;                         // notches since the last update, away from the player positive
};
enum Want : unsigned { WantGyro = 1, WantMouse = 2 };
// The port's raw input for the current update. False, leaving `out` empty, where nothing
// provides it.
bool raw(int port, Raw& out);
// What a filter wants on this port, kept until changed: WantGyro reads the gyro even with the
// port's gyro setting off and adds it to no stick (the filter uses Raw's instead);
// WantMouse, on port 1, captures the mouse in relative mode, which the menu releases while
// it is open and the window gives up when it loses focus.
void want(int port, unsigned what);
// For the frontend: what was wanted, and the raw input it read.
unsigned wanted(int port);
void set_raw(int port, const Raw& r);

// Whether the port reads as plugged in.
bool connected(int port);
// What drives the port, for a menu or a message: `"<device>" (<type>)`, or "" for nothing.
std::string describe(int port);
// The SDL_GameControllerType of the controller on the port, -1 with none (the keyboard alone, or
// an empty port). Where a launcher's input layer (Steam Input) stands between SDL and the
// device, SDL reports the real controller's type when the launcher says what it is.
int type(int port);

// ---- settings ----
struct SettingInfo {
    const char* key;      // as controls.txt spells it
    const char* choices;  // "a|b|c" for a choice, nullptr for a number or free text
    const char* def;      // the default, as text
    float lo, hi;         // a number's range; both 0 for text or a choice
    const char* help;
};
// The per-port settings, in the order above. `count` receives how many.
const SettingInfo* settings(int* count);
// The value in force for `port`, as text: a menu's override, else controls.txt, else the
// default. Empty for an unknown key.
std::string setting_get(int port, const char* key);
// Overrides the file for this run (a menu). False, with `err` saying why, if the value is
// not one the setting takes.
bool setting_set(int port, const char* key, const std::string& value, std::string* err = nullptr);
// Drops setting_set's override, so that controls.txt (or the default) decides again.
void setting_clear(int port, const char* key);
//
// The menu's Controls > Controller page sets the face-button layout, the gyro and its
// sensitivity for every port (controls.layout, controls.gyro, controls.gyro_sensitivity in
// settings.txt). Each says "From controls.txt" until changed, and then wins over the file.

// For input_map.cpp's parser: whether `key` is a per-port setting, and whether `value`
// suits it.
bool is_setting(const std::string& key);
bool check_setting(const std::string& key, const std::string& value, std::string& err);
// For input_map.cpp's sections: the controller types a name covers, as bits by
// SDL_GameControllerType (xbox360 xboxone ps3 ps4 ps5 switch joycon virtual other, and
// xbox, playstation and nintendo for the families). 0 if it names none.
unsigned type_mask(const std::string& name);
// A controller type's name, as above.
const char* type_name(int sdl_type);

}  // namespace input_ports
