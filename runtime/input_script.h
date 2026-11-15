// Scripted controller input, for driving the game with no human at the controls.
//
// Shared by every frontend -- the SDL one, the headless EGL renderer and the
// benchmark -- since anything past the title screen needs button presses, and the
// headless targets have no other way to produce them.
#pragma once
#include "hw/pad.h"

// Parses "frame:BUTTON[+BUTTON]:duration,..." where frame is the presented-frame
// count at which to press, e.g. "900:START:10,1200:A:10". A press for another port than
// the first has the port in front: "2/900:A:10" (ports 1 to 4); a port the script drives
// reads as plugged in for the whole run.
// Buttons: A B X Y Z L R START UP DOWN LEFT RIGHT; stick: SL SR SU SD, all the way, or
// as far as a number after them says (SR19); C stick: CL CR CU CD, likewise.
// MX<n> and MY<n> move the mouse n counts each update (right and down positive), for a
// filter that reads it (input_ports::raw).
// GCN_INPUT_LOG=1 logs each press as it fires.
//
// `spec` null or empty falls back to the GCN_INPUT environment variable, which an
// Android app has no way to set -- it passes the script in directly instead.
void input_script_init(const char* spec = nullptr);

// Overlays any press for port `port` (0 to 3) that is active at the current
// presented-frame count.
void input_script_apply(PadState& p, int port = 0);
// The ports the script presses anything on, a bit per port (bit 0 the first).
unsigned input_script_ports();
// Whether a press is active on `port` now, and the mouse motion the active ones give.
bool input_script_now(int port, int* mx, int* my);
