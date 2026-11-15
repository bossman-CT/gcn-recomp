// Controls from a text file: any joystick SDL sees -- a wheel, pedals, a button box, a
// gamepad without a mapping -- and any key, bound to the GameCube pad.
//
// The file (controls.txt in the current directory, or --controls=<path>) has one binding
// per line, `<pad control> = <source> [options]`, # to the end of a line a comment:
//
//   stick_x = "Simucube 2 Pro" axis 0 gain 2.5
//   A       = "Heusinkveld Sprint Pedals" axis 1
//   B       = "Heusinkveld Sprint Pedals" axis 2
//   R       = "Simucube 2 Pro" button 5
//   start   = key Return
//
// Pad controls:
//   A B X Y Z start dpad_up dpad_down dpad_left dpad_right   digital
//   L R                     the analog trigger, and its digital click past the threshold
//   stick_x stick_y cstick_x cstick_y                         a whole axis, -1 to +1
//   stick_left stick_right stick_up stick_down, cstick_...    one direction, 0 to 1
//
// Sources:
//   key <name>                    an SDL scancode name: X, Return, Space, "Left Shift"
//   "<device>" button <n>
//   "<device>" hat <n> up|down|left|right
//   "<device>" axis <n>           the whole axis. Onto a one-way control (a button, a
//                                 trigger, stick_left...) its whole travel is 0 to 1,
//                                 which is what a pedal resting at one end wants.
//   "<device>" axis <n>+ / <n>-   one half of an axis, from its centre outwards
// A device is named as SDL names it; "<name> #2" is the second of two with that name.
//
// Options, after the source:
//   invert          turn the axis round (a pedal that rests at the top of its range)
//   deadzone <f>    the fraction of the travel that reads as nothing (default 0)
//   gain <f>        multiplies the value before it is clamped: 2 is full lock at half
//                   the wheel's rotation (default 1)
//   threshold <f>   where a digital control is pressed (default 0.5; L and R 0.85)
//
// Several bindings may share a pad control: buttons are pressed if any is, one-way values
// take the largest, and a whole stick axis is the sum of its bindings, so stick_left and
// stick_right on two buttons cancel. A stick or trigger the file leaves at rest is left to
// the default keyboard and gamepad mapping, which stays on except for a device the file
// names.
//
// The port's own controller (input_ports.h) is a source too, by SDL's names for a standard
// controller's controls, with the stick axes up and right positive:
//   gamepad <control>             a b x y, leftshoulder, dpup, lefttrigger, leftx, ...
//   gamepad <axis>+ / <axis>-     one half of a stick axis
// A pad control bound from the gamepad, or to `none`, loses its default gamepad source on
// that port, and a gamepad control the file binds loses its default use, so that a remap
// never presses two things: `A = gamepad b` makes b the A button and nothing else, and
// leaves whatever b did by default unbound. `Z = none` frees a pad control. A stick axis bound from the gamepad has a dead zone of 0.12
// unless the line gives one.
//
// Sections. Lines before any section bind port 1, as they always have; settings there
// apply to every port. After a `[port 2]` line, bindings and settings are port 2's. After
// `[all]`, every port's. After a controller type -- `[ps4]`, `[playstation]`, `[xbox]`,
// `[nintendo]`, input_ports::type_mask -- they apply to every port that type of
// controller is on. A setting from a port's section wins over a type's, which wins over
// one for every port; among equals the last line wins. The per-port settings (`layout`,
// `gyro`, `device`, ...) are input_ports.h's. Force feedback and stick ranges stay global.
//
// --list-controls runs input_map_probe() instead of the game: it prints the devices and,
// for each control pressed or moved, the source to paste into the file.
//
// Force feedback (ffb.h) goes to the device stick_x is bound to, unless the file says
// otherwise:
//   ffb = off                     none
//   ffb_device = "<device>"       another device
//   ffb_<setting> = <number>      FfbSettings without its prefix: ffb_gain = 0.5
//
// Stick ranges: `stick_x_min = 19`, `stick_x_max = 75` and the same for stick_y, cstick_x
// and cstick_y, in the pad's units from the centre. A bound stick axis is spread from min,
// the first offset the game responds to, to max, where it is at full, so that the moment
// the binding leaves its own dead zone the game reacts. A whole axis on a stick axis has a
// dead zone of 0.005 unless the line gives one. A game supplies its own ranges
// (input_defaults.h); by default they are 0 and 100.
#pragma once
#include "hw/pad.h"
#include <cstdint>
#include <string>

union SDL_Event;
struct _SDL_Joystick;
typedef struct _SDL_Joystick SDL_Joystick;
struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

// input_map_gamepad_override's bits beyond the buttons' (PAD_*, PAD_L and PAD_R for the
// triggers too): the stick axes.
enum : uint32_t {
    INPUT_MAP_STICK_X = 0x10000, INPUT_MAP_STICK_Y = 0x20000,
    INPUT_MAP_CSTICK_X = 0x40000, INPUT_MAP_CSTICK_Y = 0x80000,
};

// Reads the file; true if there is none. Problems are reported on stderr, line by line,
// and the lines skipped.
bool input_map_load(const char* path);
// Devices coming and going. Every SDL event may be passed.
void input_map_event(const SDL_Event& e);
// Whether the file names the joystick at this SDL device index (the default gamepad
// mapping leaves such a device alone).
bool input_map_binds_index(int device_index);
// The file's bindings for port `port` (0 to 3), merged into `p`. `pad` is the port's game
// controller (or null) for gamepad sources, and `type` its SDL_GameControllerType (-1 for
// none) for the type sections. Port 0 also runs the wheel's force feedback.
void input_map_apply(int port, PadState& p, SDL_GameController* pad, int type);
// The pad controls the file takes from the default gamepad mapping on this port: PAD_*
// bits and INPUT_MAP_STICK_*.
uint32_t input_map_gamepad_override(int port, int type);
// The gamepad controls the file binds on this port, which lose their default use: bit
// `button` for a SDL_GameControllerButton, bit 32 + `axis` for an axis.
uint64_t input_map_gamepad_sources(int port, int type);
// The file's value for a per-port setting (input_ports.h) on this port, or null.
const char* input_map_setting(int port, int type, const char* key);
// A gamepad control's name: its SDL_GameControllerButton in `button`, or its axis in
// `axis`, the other -1. False for a name it doesn't know.
bool input_map_gamepad_control(const std::string& name, int& button, int& axis);
// How the file names a device: its SDL name, and "#n" if it is not the first of that name.
std::string input_map_device_label(SDL_Joystick* j);
// A stick axis's range (0 stick_x, 1 stick_y, 2 cstick_x, 3 cstick_y), in the pad's units
// from the centre: where the game starts to respond and where it is at full.
void input_map_stick_range(int axis, float& lo, float& hi);
// Stops the wheel's force feedback (ffb.h), before the program exits.
void input_map_shutdown();
// The --list-controls loop. SDL must not be initialised yet. Returns the exit code.
int input_map_probe();
// --test-ffb: the file's wheel with its force feedback and no game, showing where the
// steering is. SDL must not be initialised yet. Returns the exit code.
int input_map_test_ffb(const char* path);
