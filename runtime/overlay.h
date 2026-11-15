// The in-game menu over the desktop frontend's window: Dear ImGui on the window's own GL
// context, drawn after the game's frame. It shows the entries of settings.h and menu.h.
//
// F1 toggles it; Esc opens and closes it (menu.escape_quits brings back Esc quitting); a
// controller's Back button (View, Select, Share, -) toggles it, and holding L + R + Start
// for a second opens it, for a controller without one. While it is open the game is paused
// (menu.pause), at its next frame boundary, and the game's pad reads at rest; buttons
// still held when it closes reach the game only once released.
//
// Where main.cpp calls it: startup() after the command line, before the window exists;
// init() once GL is loaded, which also installs filter_pad() as input_ports' filter;
// handle_event() for each SDL event; draw() after a frame is presented, before the swap; and
// redraw() when no frame came, so the menu stays up over a paused game.
//
// Test switches, so the menu can be checked without a person at the keys:
//   GCN_MENU_OPEN=<frame>[:<menu>]  opens it at that presented frame, on that menu
//   GCN_MENU_SHOT=<dir>             with GCN_MENU_OPEN, once the game is paused writes a
//                                   picture of each menu to <dir>/menu_<n>_<name>.png, then quits
#pragma once
#include "hw/pad.h"

struct SDL_Window;
union SDL_Event;

namespace overlay {

// Reads settings.txt (settings::load). `scale`, `match_window` and `fullscreen` are the
// command line's (0 and false when not given: --scale=N, --scale=window, --fullscreen),
// which override the settings for this run; on return they hold what the window and
// renderer should start with, and render_set_match_output has the second. `aspect_given`:
// --aspect or GCN_ASPECT was, and render_set_aspect has it.
void startup(char** argv, float& scale, bool& match_window, bool& fullscreen, bool aspect_given);
// Sets ImGui up on the window and its current GL context.
void init(SDL_Window* win);
// Whether the menu took the event, in which case nothing else should act on it.
bool handle_event(const SDL_Event& e);
// The pad the game is about to get for `port`: at rest while the menu has it.
void filter_pad(int port, PadState& p);
// Over a frame just presented in framebuffer 0.
void draw(int w, int h);
// No frame came: if the menu is up, the last frame is shown again with the menu over it,
// and this returns true (the caller swaps).
bool redraw(int w, int h);
bool quit_requested();
// At exit: saves settings.txt if anything changed.
void shutdown();

// ---- for the generic pages (overlay_pages.cpp) ----
SDL_Window* window();
void open(const char* menu = nullptr);
void close();
void request_quit();
void reset_console();
// Content a page draws itself with ImGui, at `path` ("Menu/Page[/Section]"), among its
// entries in the order made.
void add_draw(const char* path, void (*draw)());
// The pages' own startup and per-frame work.
void pages_startup(float& scale, bool& match_window, bool& fullscreen, bool aspect_given);
void pages_frame();

}  // namespace overlay
