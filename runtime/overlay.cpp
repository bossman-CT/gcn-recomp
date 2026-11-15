// The in-game menu: input, pausing, drawing. See overlay.h; the generic pages are in
// overlay_pages.cpp, the entries they show in settings.h and menu.h.
#include "overlay.h"
#include "menu.h"
#include "settings.h"
#include "runtime.h"
#include "platform.h"
#include "input_log.h"
#include "input_ports.h"
#include "gx/gl.h"
#include "gx/render_gl.h"
#include <SDL.h>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

bool write_png(const char* path, const uint8_t* rgba, int w, int h);
void audio_pause(bool on);

namespace overlay {

// ---- the menu's own settings ----
static auto& s_pause = settings::toggle("menu.pause", "Game/Menu", "Pause the game while the menu is open", true)
                           .help("The game stops where it is while the menu is open, and its sound with it.");
static auto& s_keep_input =
    settings::toggle("menu.game_keeps_input", "Game/Menu", "Let the game keep the controller")
        .help("The game keeps running and reading the controller while the menu is open, for changing "
              "settings while playing. The game isn't paused, and the controller doesn't move around the menu.");
static auto& s_escape_quits = settings::toggle("menu.escape_quits", "Game/Menu", "Esc quits the game")
                                  .help("Esc quits at once instead of opening the menu. F1 still opens it.");
static auto& s_scale = settings::slider("menu.scale", "Game/Menu", "Menu size", 100, 50, 300)
                           .format("%d%%")
                           .help("On top of the size that suits the window's height.");
static auto& s_show_hint = settings::toggle("menu.show_hint", "Game/Menu", "Say how to open the menu at startup", true)
                               .help("A note for a few seconds when the game starts. It stops once the menu has been opened.");
static auto& s_show_dev = settings::toggle("dev.show_developer", "Developer/Options", "Show developer options")
                              .help("Shows the menu's entries meant for working on the game rather than playing it.");

// ---- state ----
static SDL_Window* g_win;
static char** g_argv;
static bool g_ready;                 // ImGui is set up
static bool g_open;
static bool g_quit;
static bool g_focus;                 // give the menu's window focus on the next frame
static bool g_paused;                // what was last asked of guest_pause and the audio
static std::string g_menu = "Game";  // the top menu shown
static std::string g_status;         // help for the item under the cursor or focus
static uint16_t g_mask[4];           // per port: buttons kept from the game until released (see filter_pad)
static uint32_t g_combo_since[4];    // per port: when L + R + Start went down, 0 while it isn't
static uint32_t g_start_ms;          // the first frame drawn, for the hint
static uint32_t g_dirty_since;       // when settings.txt was first found to need saving
static uint32_t g_last_redraw;
static float g_font_scale;           // the scale the font atlas was built at
// The last presented frame, kept while the menu is open to show again under it.
static GLuint g_frozen_fbo, g_frozen_tex;
static int g_frozen_w, g_frozen_h;
static bool g_have_frozen;
// Test switches (overlay.h).
static int g_open_at = -1;
static std::string g_open_menu, g_shot_dir;
static int g_shot_index, g_shot_wait;
static bool g_no_save;  // a test run (GCN_MENU_OPEN) leaves settings.txt alone

struct Draw { const char* path; void (*fn)(); uint32_t order; };
static std::vector<Draw>& draws() { static auto* v = new std::vector<Draw>; return *v; }
void add_draw(const char* path, void (*fn)()) { draws().push_back(Draw{path, fn, settings::next_order()}); }

SDL_Window* window() { return g_win; }
bool quit_requested() { return g_quit; }
void request_quit() { g_quit = true; }

static bool captures_input() { return g_open && !s_keep_input; }

// The guest and its sound paused exactly while the menu is open with pausing on.
static void sync_pause() {
    const bool want = g_open && s_pause && !s_keep_input;
    if (want == g_paused) return;
    g_paused = want;
    guest_pause(want);
    audio_pause(want);
}

void open(const char* which) {
    if (which) g_menu = which;
    if (g_open) return;
    g_open = true;
    menu::set_open(true);
    g_focus = true;
    if (g_ready) ImGui::GetIO().ClearInputKeys();
    SDL_ShowCursor(SDL_ENABLE);
    if (s_show_hint) s_show_hint.set(false);  // they've found it
    sync_pause();
}

void close() {
    if (!g_open) return;
    g_open = false;
    menu::set_open(false);
    for (int i = 0; i < 4; i++) {
        g_mask[i] = 0xFFFF;  // whatever is held now (the button that closed it) stays with the menu
        g_combo_since[i] = 0;
    }
    sync_pause();
    if (!g_no_save) settings::save();
}

void reset_console() {
    settings::save();
    input_log_end();
    plat_relaunch(g_argv);
}

void startup(char** argv, int& scale, bool& fullscreen, bool aspect_given) {
    g_argv = argv;
    settings::load();
    pages_startup(scale, fullscreen, aspect_given);
    if (const char* e = getenv("GCN_MENU_OPEN"); e && *e) {
        g_open_at = atoi(e);
        if (const char* c = strchr(e, ':')) g_open_menu = c + 1;
        // A test leaves the player's settings.txt alone; one it names itself is saved as usual.
        g_no_save = settings::file_path() == "settings.txt";
    }
    if (const char* e = getenv("GCN_MENU_SHOT"); e && *e) {
        g_shot_dir = e;
        plat_make_dirs(g_shot_dir);
    }
}

void shutdown() {
    if (!g_no_save) settings::save();
}

// ---- fonts and style, at the window's scale ----

static float effective_scale(int h) {
    // A quarter step per 180 lines above 720, so the menu keeps its size on screen.
    const float base = std::max(1.0f, std::floor(h / 720.0f * 4.0f) / 4.0f);
    return base * (float)s_scale.get() / 100.0f;
}

static void build_style(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg;
    cfg.SizePixels = std::round(13.0f * scale);
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2(12, 12);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
    g_font_scale = scale;
}

void init(SDL_Window* win) {
    g_win = win;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the menu places itself; there is nothing to remember
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL2_InitForOpenGL(win, SDL_GL_GetCurrentContext());
    input_ports::set_filter(filter_pad);
#ifdef GCN_GL_ES
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
    g_ready = true;
}

// ---- input ----

bool handle_event(const SDL_Event& e) {
    if (!g_ready) return false;
    if (e.type == SDL_QUIT || e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED ||
        e.type == SDL_WINDOWEVENT)
        return false;
    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        const SDL_Scancode k = e.key.keysym.scancode;
        if (k == SDL_SCANCODE_F11 || (k == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))) return false;
        if (k == SDL_SCANCODE_F1) {
            g_open ? close() : open();
            return true;
        }
        if (k == SDL_SCANCODE_ESCAPE) {
            if (!g_open) {
                if (s_escape_quits) return false;
                open();
                return true;
            }
            // Esc closes a question first, then the menu.
            if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) close();
            else ImGui_ImplSDL2_ProcessEvent(&e);
            return true;
        }
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        const uint8_t b = e.cbutton.button;
        if (b == SDL_CONTROLLER_BUTTON_BACK) {
            g_open ? close() : open();
            return true;
        }
        if (b == SDL_CONTROLLER_BUTTON_START && captures_input() &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            close();
            return true;
        }
    }
    if (!g_open) return false;
    ImGui_ImplSDL2_ProcessEvent(&e);
    switch (e.type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
        return true;
    default:
        return false;
    }
}

void filter_pad(int port, PadState& p) {
    if (!g_ready || port < 0 || port > 3) return;
    uint16_t& mask = g_mask[port];
    uint32_t& combo_since = g_combo_since[port];
    // Holding L + R + Start for a second opens the menu, for a controller with no Back button.
    const uint16_t combo = PAD_L | PAD_R | PAD_START;
    if (!g_open && (p.buttons & combo) == combo) {
        const uint32_t now = SDL_GetTicks() | 1;
        if (!combo_since) combo_since = now;
        else if (now - combo_since >= 1000) open();
    } else {
        combo_since = 0;
    }
    if (captures_input()) {
        PadState rest;
        rest.connected = p.connected;
        p = rest;
        mask = 0xFFFF;
        return;
    }
    // Buttons held when the menu closed stay with it until they're let go, so the press
    // that closed it (A on Resume, Start, the combo) doesn't reach the game too.
    mask &= p.buttons;
    p.buttons &= (uint16_t)~mask;
    if (mask & PAD_L) p.trig_l = 0;
    if (mask & PAD_R) p.trig_r = 0;
}

// ---- the menu's tree ----

// An entry of a page: a setting, a button or something a page draws itself.
struct Entry {
    uint32_t order;
    settings::Setting* setting = nullptr;
    const menu::Button* button = nullptr;
    const Draw* draw = nullptr;
};
using Section = std::vector<Entry>;
using Page = std::map<std::string, Section>;       // by section ("" for none)
using Menu = std::map<std::string, Page>;          // by page ("" for entries on the menu itself)

static void split(const char* path, std::string& m, std::string& p, std::string& s) {
    const std::string full = path;
    const size_t a = full.find('/');
    m = full.substr(0, a);
    if (a == std::string::npos) return;
    const size_t b = full.find('/', a + 1);
    p = full.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    if (b != std::string::npos) s = full.substr(b + 1);
}

static std::map<std::string, Menu> build_tree() {
    std::map<std::string, Menu> tree;
    auto put = [&](const char* path, Entry e) {
        std::string m, p, s;
        split(path, m, p, s);
        tree[m][p][s].push_back(e);
    };
    for (settings::Setting* s : settings::all()) {
        if (!s->visible() || ((s->flag_bits() & settings::Developer) && !s_show_dev)) continue;
        Entry e;
        e.order = s->order();
        e.setting = s;
        put(s->path(), e);
    }
    for (const menu::Button& b : menu::buttons()) {
        Entry e;
        e.order = b.order;
        e.button = &b;
        put(b.path, e);
    }
    for (const Draw& d : draws()) {
        Entry e;
        e.order = d.order;
        e.draw = &d;
        put(d.path, e);
    }
    for (auto& [m, menu] : tree)
        for (auto& [p, page] : menu)
            for (auto& [s, section] : page)
                std::stable_sort(section.begin(), section.end(), [](const Entry& a, const Entry& b) { return a.order < b.order; });
    return tree;
}

// The top menus in the order shown: the fixed ones, a game's own before Developer and Help.
static std::vector<std::string> menu_order(const std::map<std::string, Menu>& tree) {
    static const char* kFirst[] = {"Game", "Graphics", "Controls", "Audio", "Enhancements", "Cheats"};
    static const char* kLast[] = {"Developer", "Help"};
    std::vector<std::string> out;
    for (const char* m : kFirst)
        if (tree.count(m)) out.push_back(m);
    for (const auto& [m, menu] : tree)
        if (std::find(std::begin(kFirst), std::end(kFirst), m) == std::end(kFirst) &&
            std::find(std::begin(kLast), std::end(kLast), m) == std::end(kLast))
            out.push_back(m);
    for (const char* m : kLast)
        if (tree.count(m)) out.push_back(m);
    return out;
}

static void note_status(const char* help, const char* extra = nullptr) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !ImGui::IsItemFocused()) return;
    g_status = help ? help : "";
    if (extra) g_status += (g_status.empty() ? "" : " ") + std::string(extra);
}

static void draw_setting(settings::Setting& s, float scale) {
    ImGui::PushID(s.key());
    const bool enabled = s.enabled();
    ImGui::BeginDisabled(!enabled);
    const char* label = s.label();
    switch (s.type()) {
    case settings::Type::Bool: {
        bool b = s.value() != 0.0;
        if (ImGui::Checkbox(label, &b)) s.set_value(b ? 1.0 : 0.0);
        break;
    }
    case settings::Type::Int: {
        int v = (int)s.value();
        ImGui::SetNextItemWidth(220.0f * scale);
        if (ImGui::SliderInt(label, &v, (int)s.lo(), (int)s.hi(), s.format_text() ? s.format_text() : "%d",
                             ImGuiSliderFlags_AlwaysClamp))
            s.set_value(v);
        break;
    }
    case settings::Type::Float: {
        float v = (float)s.value();
        ImGui::SetNextItemWidth(220.0f * scale);
        if (ImGui::SliderFloat(label, &v, (float)s.lo(), (float)s.hi(), s.format_text() ? s.format_text() : "%.2f",
                               ImGuiSliderFlags_AlwaysClamp))
            s.set_value(v);
        break;
    }
    case settings::Type::Choice: {
        const auto& names = s.names();
        int v = (int)s.value();
        ImGui::SetNextItemWidth(220.0f * scale);
        if (ImGui::BeginCombo(label, names[(size_t)v])) {
            for (int i = 0; i < (int)names.size(); i++)
                if (ImGui::Selectable(names[(size_t)i], i == v)) s.set_value(i);
            ImGui::EndCombo();
        }
        break;
    }
    }
    std::string extra;
    if (!enabled && s.disabled_why()) extra = s.disabled_why();
    if (s.flag_bits() & settings::Restart) extra += (extra.empty() ? "" : " ") + std::string("Takes effect after a restart.");
    if (s.overridden()) extra += (extra.empty() ? "" : " ") + std::string("Set from the command line for this run.");
    note_status(s.help_text(), extra.empty() ? nullptr : extra.c_str());
    ImGui::EndDisabled();
    if (!s.is_default()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) s.reset();
        note_status("Back to the default.");
    }
    ImGui::PopID();
}

static void draw_button(const menu::Button& b) {
    ImGui::PushID(b.label);
    if (ImGui::Button(b.label)) {
        if (b.confirm) ImGui::OpenPopup("##confirm");
        else b.fn();
    }
    note_status(b.help);
    if (ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(b.confirm);
        ImGui::Spacing();
        if (ImGui::Button("Yes")) {
            ImGui::CloseCurrentPopup();
            b.fn();
        }
        ImGui::SameLine();
        if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) ImGui::CloseCurrentPopup();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void draw_section(const Section& sec, float scale) {
    // Buttons side by side; everything else on its own line.
    bool after_button = false;
    for (const Entry& e : sec) {
        if (e.button) {
            if (after_button) ImGui::SameLine();
            draw_button(*e.button);
            after_button = true;
            continue;
        }
        after_button = false;
        if (e.setting) draw_setting(*e.setting, scale);
        else if (e.draw) e.draw->fn();
    }
}

static void draw_menu(float scale) {
    const auto tree = build_tree();
    const auto order = menu_order(tree);
    if (std::find(order.begin(), order.end(), g_menu) == order.end()) g_menu = order.empty() ? "" : order[0];
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(ds.x * 0.92f, 720.0f * scale), std::min(ds.y * 0.88f, 460.0f * scale)),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (g_focus) ImGui::SetNextWindowFocus();
    ImGui::Begin(GCN_APP_LABEL "###menu", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);
    g_status.clear();
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 2.6f;
    ImGui::BeginChild("##menus", ImVec2(130.0f * scale, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    for (const std::string& m : order) {
        const bool sel = m == g_menu;
        if (ImGui::Selectable(m.c_str(), sel)) g_menu = m;
        // Moving through the list with the d-pad or the keys shows each menu as it goes.
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible && !sel) g_menu = m;
        if (g_focus && sel) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
    if (const auto it = tree.find(g_menu); it != tree.end()) {
        std::vector<std::string> pages;
        for (const auto& [p, page] : it->second) pages.push_back(p);
        menu::sort_children(g_menu, pages);
        for (const std::string& p : pages) {
            if (!p.empty()) ImGui::SeparatorText(p.c_str());
            const Page& page = it->second.at(p);
            std::vector<std::string> secs;
            for (const auto& [s, sec] : page) secs.push_back(s);
            menu::sort_children(g_menu + "/" + p, secs);
            for (const std::string& s : secs) {
                if (!s.empty()) ImGui::TextDisabled("%s", s.c_str());
                draw_section(page.at(s), scale);
            }
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (g_status.empty())
        g_status = captures_input() ? "F1, Esc or the controller's Back closes the menu." : "F1 or Esc closes the menu.";
    ImGui::TextWrapped("%s", g_status.c_str());
    ImGui::End();
    g_focus = false;
}

static void draw_hint() {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float pad = ImGui::GetStyle().WindowPadding.x;
    ImGui::SetNextWindowPos(ImVec2(pad, ds.y - pad), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Menu: F1, Esc, or the controller's Back button");
    ImGui::End();
}

// ---- drawing ----

static bool hint_showing() { return s_show_hint && !g_open && g_start_ms && SDL_GetTicks() - g_start_ms < 6000; }

// The frame in framebuffer 0, kept for redraw().
static void capture_frozen(int w, int h) {
    if (!g_frozen_fbo || w != g_frozen_w || h != g_frozen_h) {
        if (!g_frozen_fbo) {
            glGenFramebuffers(1, &g_frozen_fbo);
            glGenTextures(1, &g_frozen_tex);
        }
        glBindTexture(GL_TEXTURE_2D, g_frozen_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_frozen_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_frozen_tex, 0);
        g_frozen_w = w;
        g_frozen_h = h;
    }
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_frozen_fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    g_have_frozen = true;
}

// The kept frame into `fbo`, at the window's size now.
static void show_frozen(GLuint fbo, int w, int h) {
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    if (g_have_frozen) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_frozen_fbo);
        glBlitFramebuffer(0, 0, g_frozen_w, g_frozen_h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

// GCN_MENU_SHOT: once the game is paused under the menu, a picture of each top menu.
static void shot_step(int w, int h) {
    if (g_shot_dir.empty() || !g_open || g_quit) return;
    if (s_pause && !guest_paused()) return;
    if (++g_shot_wait < 10) return;  // a few frames for the layout to settle
    g_shot_wait = 0;
    static GLuint fbo, tex;
    static int fw, fh;
    if (!fbo || fw != w || fh != h) {
        if (!fbo) {
            glGenFramebuffers(1, &fbo);
            glGenTextures(1, &tex);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        fw = w;
        fh = h;
    }
    show_frozen(fbo, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    std::vector<uint8_t> px((size_t)w * h * 4), flip(px.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    const size_t stride = (size_t)w * 4;
    for (int y = 0; y < h; y++) memcpy(&flip[(size_t)y * stride], &px[(size_t)(h - 1 - y) * stride], stride);
    for (size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
    char path[1024];
    snprintf(path, sizeof path, "%s/menu_%d_%s.png", g_shot_dir.c_str(), g_shot_index, g_menu.c_str());
    write_png(path, flip.data(), w, h);
    fprintf(stderr, "[menu] %s written\n", path);
    const auto order = menu_order(build_tree());
    const auto it = std::find(order.begin(), order.end(), g_menu);
    g_shot_index++;
    if (it == order.end() || it + 1 == order.end()) g_quit = true;
    else g_menu = *(it + 1);
}

// One ImGui frame into framebuffer 0, if anything is to be shown.
static void render(int w, int h) {
    if (!g_open && !hint_showing()) return;
    // The backend makes its GL objects in its first NewFrame; the font atlas is rebuilt
    // after that, so there is one texture for it.
    ImGui_ImplOpenGL3_NewFrame();
    const float scale = effective_scale(h);
    if (scale != g_font_scale) build_style(scale);
    ImGuiIO& io = ImGui::GetIO();
    if (captures_input()) io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (g_open) draw_menu(scale);
    else draw_hint();
    ImGui::Render();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    shot_step(w, h);
}

// Saving: when the menu closes, at exit, and two seconds after a change while it's open.
static void autosave() {
    if (g_no_save) return;
    if (!settings::dirty()) { g_dirty_since = 0; return; }
    const uint32_t now = SDL_GetTicks() | 1;
    if (!g_dirty_since) g_dirty_since = now;
    else if (now - g_dirty_since >= 2000) { settings::save(); g_dirty_since = 0; }
}

static void frame_common() {
    if (!g_start_ms) g_start_ms = SDL_GetTicks() | 1;
    if (g_open_at >= 0 && gx::present_count() >= (uint32_t)g_open_at) {
        g_open_at = -1;
        open(g_open_menu.empty() ? nullptr : g_open_menu.c_str());
    }
    pages_frame();
    sync_pause();
    autosave();
}

void draw(int w, int h) {
    if (!g_ready) return;
    frame_common();
    if (g_open) capture_frozen(w, h);
    render(w, h);
    g_last_redraw = SDL_GetTicks();
}

bool redraw(int w, int h) {
    if (!g_ready || !g_open) return false;
    // A paused game sends no frames; about 60 a second is plenty for the menu.
    const uint32_t now = SDL_GetTicks();
    if (now - g_last_redraw < 16) return false;
    g_last_redraw = now;
    frame_common();
    if (!g_open) return false;
    show_frozen(0, w, h);
    render(w, h);
    return true;
}

}  // namespace overlay
