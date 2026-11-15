// The menu's registry. See menu.h.
#include "menu.h"
#include "settings.h"
#include <algorithm>
#include <atomic>
#include <map>

namespace menu {

static std::vector<Button>& button_list() { static auto* v = new std::vector<Button>; return *v; }
static std::vector<Note>& note_list() { static auto* v = new std::vector<Note>; return *v; }
static std::map<std::string, std::vector<std::string>>& layouts() {
    static auto* m = new std::map<std::string, std::vector<std::string>>;
    return *m;
}
static std::atomic<bool> g_open{false};
static std::vector<const char* (*)()>& hint_list() { static auto* v = new std::vector<const char* (*)()>; return *v; }
static Question g_question;
static bool g_asked;
static void (*g_quit)();

void button(const char* path, const char* label, void (*fn)(), const char* help, const char* confirm) {
    button_list().push_back(Button{path, label, help, confirm, fn, settings::next_order()});
}

void note(const char* path, const char* text) { note_list().push_back(Note{path, text, settings::next_order()}); }

void layout(const char* path, std::initializer_list<const char*> children) {
    auto& v = layouts()[path];
    v.assign(children.begin(), children.end());
}

bool is_open() { return g_open; }
void set_open(bool open) { g_open = open; }

void hint(const char* (*text)()) { hint_list().push_back(text); }
const std::vector<const char* (*)()>& hints() { return hint_list(); }

void ask(const char* question, void (*yes)()) {
    g_question = Question{question, yes};
    g_asked = true;
}
bool take_question(Question& out) {
    if (!g_asked) return false;
    g_asked = false;
    out = g_question;
    return true;
}

void quit() {
    if (g_quit) g_quit();
}
void set_quit(void (*fn)()) { g_quit = fn; }

const std::vector<Button>& buttons() { return button_list(); }
const std::vector<Note>& notes() { return note_list(); }

void sort_children(const std::string& path, std::vector<std::string>& children) {
    const auto it = layouts().find(path);
    auto rank = [&](const std::string& c) -> size_t {
        if (it == layouts().end()) return SIZE_MAX;
        const auto& v = it->second;
        const auto f = std::find(v.begin(), v.end(), c);
        return f == v.end() ? SIZE_MAX : (size_t)(f - v.begin());
    };
    std::stable_sort(children.begin(), children.end(), [&](const std::string& a, const std::string& b) {
        const size_t ra = rank(a), rb = rank(b);
        return ra != rb ? ra < rb : a < b;
    });
}

}  // namespace menu
