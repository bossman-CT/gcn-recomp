// The menu's generic pages: what the runtime itself does (the game, the window, the
// renderer, the sound). A game's own entries come from its SOURCES. See overlay.h.
#include "overlay.h"
#include "menu.h"
#include "settings.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include <SDL.h>
#include "imgui.h"
#include <cmath>
#include <string>

void audio_set_volume(float v);

namespace overlay {

// ---- Game ----
static void resume() { close(); }
static void reset() { reset_console(); }
static void quit() { request_quit(); }

static const bool s_entries = [] {
    menu::button("Game", "Resume", resume, "Back to the game.");
    menu::button("Game", "Reset", reset, "Starts the console again, as its reset button would.",
                 "Reset the console? Anything not saved in the game is lost.");
    menu::button("Game", "Quit", quit, "Closes the game.", "Quit the game? Anything not saved in the game is lost.");
    menu::layout("Graphics", {"Display", "Rendering"});
    menu::layout("Help", {"About", "Keys", "Files"});
    return true;
}();

// ---- Graphics ----
static void apply_fullscreen(bool on) {
    if (SDL_Window* w = window()) {
        const bool full = SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (full != on) SDL_SetWindowFullscreen(w, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
}
static auto& s_fullscreen = settings::toggle("graphics.fullscreen", "Graphics/Display", "Fullscreen")
                                .help("Borderless fullscreen on the window's display. F11 or Alt+Enter switches it too.")
                                .on_change(apply_fullscreen);

static auto& s_internal_scale =
    settings::slider("graphics.internal_scale", "Graphics/Rendering", "Internal resolution", 2, 1, 8)
        .format("%dx native")
        .help("How many times the console's own resolution the game is drawn at: higher is sharper, and "
              "slower. --scale sets it for one run.")
        .on_change([](int v) { gx::render_set_internal_scale(v); });

// The renderer widens the picture itself (render_set_aspect), so this is the host's alone:
// the guest never sees it, and it can change while the game runs.
static const float kAspects[] = {4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f};
static auto& s_aspect =
    settings::choice("graphics.aspect", "Graphics/Display", "Aspect ratio", {"4:3", "16:9", "16:10", "21:9"}, 0)
        .help("The shape of the picture. A wider one shows more of the scene at the sides. --aspect sets it for "
              "one run, to any ratio.")
        .on_change([](int v) { gx::render_set_aspect(kAspects[v]); });
static bool g_custom_aspect;  // --aspect gave a ratio none of the choices is

static void draw_aspect_note() {
    if (g_custom_aspect && std::fabs(gx::render_aspect() - kAspects[s_aspect.get()]) > 0.01f)
        ImGui::TextDisabled("Shown at %.2f:1 for this run (--aspect).", gx::render_aspect());
}

// ---- Audio ----
static auto& s_volume = settings::slider("audio.master_volume", "Audio/Volume", "Master volume", 100, 0, 100)
                            .format("%d%%")
                            .help("Everything the game plays.")
                            .on_change([](int v) { audio_set_volume(v / 100.0f); });

// ---- Help ----
static void draw_about() {
    ImGui::TextUnformatted(GCN_APP_LABEL);
    ImGui::TextDisabled("OpenGL: %s", (const char*)glGetString(GL_RENDERER));
    ImGui::TextDisabled("        %s", (const char*)glGetString(GL_VERSION));
    ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
}
static void draw_keys() {
    ImGui::BulletText("F1, or Esc: open and close this menu");
    ImGui::BulletText("A controller's Back (View, Select, Share, -): the same");
    ImGui::BulletText("Hold L + R + Start for a second: open it, on a controller without Back");
    ImGui::BulletText("D-pad or arrow keys to move, A or Enter to choose, B to cancel");
    ImGui::BulletText("F11 or Alt+Enter: fullscreen");
}
static void draw_files() {
    ImGui::TextWrapped("Settings: %s, in the game's folder. Only what you change is written there.",
                       settings::file_path().c_str());
    ImGui::TextWrapped("Controller bindings: controls.txt. The memory card and logs: saves/.");
}
static const bool s_help = [] {
    add_draw("Graphics/Display", draw_aspect_note);
    add_draw("Help/About", draw_about);
    add_draw("Help/Keys", draw_keys);
    add_draw("Help/Files", draw_files);
    return true;
}();

void pages_startup(int& scale, bool& fullscreen, bool aspect_given) {
    if (scale > 0) settings::set_override("graphics.internal_scale", std::to_string(scale).c_str());
    else scale = s_internal_scale.get();
    if (fullscreen) settings::set_override("graphics.fullscreen", "on");
    else fullscreen = s_fullscreen.get();
    if (aspect_given) {
        // The command line's ratio stands; the setting follows it when it is one of the choices.
        const float a = gx::render_aspect();
        g_custom_aspect = true;
        for (int i = 0; i < (int)(sizeof kAspects / sizeof kAspects[0]); i++)
            if (std::fabs(a - kAspects[i]) < 0.01f) {
                settings::set_override("graphics.aspect", s_aspect.text_of(i).c_str());
                g_custom_aspect = false;
            }
    } else {
        gx::render_set_aspect(kAspects[s_aspect.get()]);
    }
    audio_set_volume(s_volume.get() / 100.0f);
}

void pages_frame() {
    // F11 and Alt+Enter switch the window themselves; the setting follows, so it's kept.
    if (SDL_Window* w = window()) {
        const bool full = SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        if (full != s_fullscreen.get()) s_fullscreen.set(full);
    }
}

}  // namespace overlay
