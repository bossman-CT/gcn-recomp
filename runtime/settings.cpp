// Settings: the registry, settings.txt, and the latch at the guest's frame boundary. See settings.h.
#include "settings.h"
#include "runtime.h"
#include "platform.h"
#include "input_log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace settings {

// Registries are function-local statics, never destroyed: static initializers in other
// files may register before this file's own statics exist, and settings are read from
// other threads until the process exits.
static std::vector<Setting*>& registry() { static auto* v = new std::vector<Setting*>; return *v; }
static std::atomic<bool> g_frozen{false};
static std::atomic<bool> g_dirty{false};
static std::string& path_ref() { static auto* s = new std::string("settings.txt"); return *s; }
// --set=key=value, applied after the file.
static std::vector<std::pair<std::string, std::string>>& cli_sets() {
    static auto* v = new std::vector<std::pair<std::string, std::string>>;
    return *v;
}
// Lines of the file no setting claimed, written back as they were (a setting from a newer
// build, or from a feature this one leaves out).
static std::vector<std::pair<std::string, std::string>>& unknown_lines() {
    static auto* v = new std::vector<std::pair<std::string, std::string>>;
    return *v;
}
static std::vector<void (*)(uint32_t)>& guest_fns() { static auto* v = new std::vector<void (*)(uint32_t)>; return *v; }
// Filled at freeze(): the settings the guest reads, and those with work to do on its thread.
static std::vector<Setting*> g_guest;
static std::vector<Setting*> g_while_on;

struct Access {
    static void latch(Setting& s, double v) { s.latched_.store(v, std::memory_order_relaxed); }
    static double latched(const Setting& s) { return s.latched_.load(std::memory_order_relaxed); }
    static bool has_while_on(const Setting& s) { return s.while_on_ != nullptr; }
    static void run_while_on(Setting& s, uint32_t frame) { s.while_on_(frame); }
    static void set_names(Setting& s, std::initializer_list<const char*> n) { s.names_.assign(n.begin(), n.end()); }
    static void set_overridden(Setting& s, bool on) { s.overridden_ = on; }
    static void set_value_quiet(Setting& s, double v) { s.value_.store(v, std::memory_order_relaxed); }
    static std::string& file_text(Setting& s) { return s.file_text_; }
};

uint32_t next_order() {
    static uint32_t n = 0;
    return n++;
}

Setting::Setting(Type type, const char* key, const char* path, const char* label, double def, double lo, double hi)
    : type_(type), key_(key), path_(path), label_(label), def_(def), lo_(lo), hi_(hi), order_(next_order()),
      value_(def), latched_(def) {}

static double clamp_for(const Setting& s, double v) {
    switch (s.type()) {
    case Type::Bool: return v != 0.0 ? 1.0 : 0.0;
    case Type::Int: return std::clamp(std::round(v), s.lo(), s.hi());
    case Type::Float: return std::clamp(v, s.lo(), s.hi());
    case Type::Choice: return std::clamp(std::round(v), 0.0, (double)s.names().size() - 1);
    }
    return v;
}

bool Setting::set_value(double v) {
    v = clamp_for(*this, v);
    overridden_ = false;  // the menu's change is the player's: it is saved
    if (v == value()) return false;
    value_.store(v, std::memory_order_relaxed);
    g_dirty = true;
    changed();
    return true;
}

// A choice's name as settings.txt writes it: lowercase, spaces as underscores.
static std::string file_name(const char* name) {
    std::string s;
    for (const char* p = name; *p; p++) s += *p == ' ' ? '_' : (char)tolower((unsigned char)*p);
    return s;
}

std::string Setting::text_of(double v) const {
    char buf[64];
    switch (type_) {
    case Type::Bool: return v != 0.0 ? "on" : "off";
    case Type::Int: snprintf(buf, sizeof buf, "%d", (int)v); return buf;
    case Type::Float: snprintf(buf, sizeof buf, "%.9g", v); return buf;
    case Type::Choice: {
        const size_t i = (size_t)v;
        return i < names_.size() ? file_name(names_[i]) : std::to_string(i);
    }
    }
    return "";
}

static bool iequal(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

bool Setting::parse(const char* text, double& out) const {
    char* end = nullptr;
    switch (type_) {
    case Type::Bool:
        if (iequal(text, "on") || iequal(text, "true") || iequal(text, "yes") || !strcmp(text, "1")) { out = 1; return true; }
        if (iequal(text, "off") || iequal(text, "false") || iequal(text, "no") || !strcmp(text, "0")) { out = 0; return true; }
        return false;
    case Type::Int: {
        const long n = strtol(text, &end, 10);
        if (end == text || *end) return false;
        out = (double)n;
        return true;
    }
    case Type::Float: {
        const double d = strtod(text, &end);
        if (end == text || *end || !std::isfinite(d)) return false;
        out = d;
        return true;
    }
    case Type::Choice:
        for (size_t i = 0; i < names_.size(); i++)
            if (file_name(names_[i]) == file_name(text)) { out = (double)i; return true; }
        return false;
    }
    return false;
}

static void add(Setting* s) {
    if (g_frozen) fatal("settings: %s was made after the settings were loaded", s->key());
    for (const Setting* o : registry())
        if (!strcmp(o->key(), s->key())) fatal("settings: %s is made twice", s->key());
    registry().push_back(s);
}

Typed<bool>& toggle(const char* key, const char* path, const char* label, bool def) {
    auto* s = new Typed<bool>(Type::Bool, key, path, label, def ? 1.0 : 0.0, 0.0, 1.0);
    add(s);
    return *s;
}
Typed<int>& slider(const char* key, const char* path, const char* label, int def, int lo, int hi) {
    auto* s = new Typed<int>(Type::Int, key, path, label, def, lo, hi);
    add(s);
    return *s;
}
Typed<float>& slider(const char* key, const char* path, const char* label, float def, float lo, float hi) {
    auto* s = new Typed<float>(Type::Float, key, path, label, def, lo, hi);
    add(s);
    return *s;
}
Typed<int>& choice(const char* key, const char* path, const char* label, std::initializer_list<const char*> names,
                   int def) {
    auto* s = new Typed<int>(Type::Choice, key, path, label, def, 0, (double)names.size() - 1);
    Access::set_names(*s, names);
    add(s);
    return *s;
}

const std::vector<Setting*>& all() { return registry(); }

Setting* find(const char* key) {
    for (Setting* s : registry())
        if (!strcmp(s->key(), key)) return s;
    return nullptr;
}

void freeze() {
    if (g_frozen.exchange(true)) return;
    for (Setting* s : registry()) {
        if (s->flag_bits() & AffectsGuest) g_guest.push_back(s);
        if (Access::has_while_on(*s)) g_while_on.push_back(s);
    }
}

// ---- settings.txt ----

const std::string& file_path() { return path_ref(); }
bool dirty() { return g_dirty; }

bool parse_arg(const char* arg) {
    if (!strncmp(arg, "--settings=", 11)) {
        path_ref() = arg + 11;
        return true;
    }
    if (!strncmp(arg, "--set=", 6)) {
        const char* kv = arg + 6;
        const char* eq = strchr(kv, '=');
        if (!eq || eq == kv) {
            fprintf(stderr, "--set: expected --set=<key>=<value>, not %s\n", arg);
            return true;
        }
        cli_sets().emplace_back(std::string(kv, eq - kv), std::string(eq + 1));
        return true;
    }
    return false;
}

int list(FILE* out) {
    std::vector<Setting*> v = registry();
    std::sort(v.begin(), v.end(), [](const Setting* a, const Setting* b) { return strcmp(a->key(), b->key()) < 0; });
    for (const Setting* s : v) {
        fprintf(out, "%s = %s", s->key(), s->text_of(s->default_value()).c_str());
        if (s->type() == Type::Int || s->type() == Type::Float)
            fprintf(out, "  (%s to %s)", s->text_of(s->lo()).c_str(), s->text_of(s->hi()).c_str());
        if (s->type() == Type::Choice) {
            fprintf(out, "  (");
            for (size_t i = 0; i < s->names().size(); i++) fprintf(out, "%s%s", i ? ", " : "", s->text_of((double)i).c_str());
            fprintf(out, ")");
        }
        fprintf(out, "\n    %s: %s\n", s->path(), s->label());
        if (s->help_text()) fprintf(out, "    %s\n", s->help_text());
    }
    return 0;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// A value as the file or --set gives it: a string in double quotes loses them.
static std::string unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

void load() {
    if (const char* e = getenv("GCN_SETTINGS"); e && *e && path_ref() == "settings.txt") path_ref() = e;
    freeze();
    if (FILE* f = fopen(path_ref().c_str(), "rb")) {
        char line[1024];
        int n = 0;
        while (fgets(line, sizeof line, f)) {
            n++;
            // A comment is a whole line: a value may start with # (a colour).
            const std::string l = trim(line);
            if (l.empty() || l[0] == '#') continue;
            const size_t eq = l.find('=');
            if (eq == std::string::npos) {
                fprintf(stderr, "[settings] %s:%d: expected key = value\n", path_ref().c_str(), n);
                continue;
            }
            const std::string key = trim(l.substr(0, eq)), value = trim(l.substr(eq + 1));
            if (key == "version") continue;  // 1: nothing to migrate yet
            Setting* s = find(key.c_str());
            double v;
            if (!s) {
                unknown_lines().emplace_back(key, value);
            } else if (s->parse(unquote(value).c_str(), v)) {
                Access::set_value_quiet(*s, clamp_for(*s, v));
                Access::file_text(*s) = value;
            } else {
                fprintf(stderr, "[settings] %s:%d: %s cannot be %s; using %s\n", path_ref().c_str(), n, key.c_str(),
                        value.c_str(), s->text().c_str());
            }
        }
        fclose(f);
    }
    for (const auto& [key, value] : cli_sets()) set_override(key.c_str(), value.c_str());
    g_dirty = false;
}

bool set_override(const char* key, const char* text) {
    Setting* s = find(key);
    double v;
    if (!s) {
        fprintf(stderr, "[settings] there is no setting %s (--list-settings lists them)\n", key);
        return false;
    }
    if (!s->parse(unquote(text).c_str(), v)) {
        fprintf(stderr, "[settings] %s cannot be %s\n", key, text);
        return false;
    }
    Access::set_value_quiet(*s, clamp_for(*s, v));
    Access::set_overridden(*s, true);
    return true;
}

bool save() {
    if (!g_dirty.exchange(false)) return true;
    std::vector<Setting*> v = registry();
    std::sort(v.begin(), v.end(), [](const Setting* a, const Setting* b) { return strcmp(a->key(), b->key()) < 0; });
    const std::string tmp = path_ref() + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "[settings] cannot write %s\n", tmp.c_str());
        return false;
    }
    fprintf(f, "# Settings, written by the in-game menu (F1). Only settings changed from their defaults\n"
               "# are listed; run with --list-settings for every one, its default and what it does.\n"
               "# Edit this only while the game isn't running: the menu writes over it.\n"
               "version = 1\n\n");
    for (Setting* s : v) {
        std::string& kept = Access::file_text(*s);
        if (!s->overridden()) kept = s->is_default() ? std::string() : s->text();
        if (!kept.empty()) fprintf(f, "%s = %s\n", s->key(), kept.c_str());
    }
    if (!unknown_lines().empty()) {
        fprintf(f, "\n# Not known to this build, kept as they were:\n");
        for (const auto& [key, value] : unknown_lines()) fprintf(f, "%s = %s\n", key.c_str(), value.c_str());
    }
    const bool ok = fclose(f) == 0 && plat_replace_file(tmp.c_str(), path_ref().c_str());
    if (!ok) fprintf(stderr, "[settings] cannot write %s\n", path_ref().c_str());
    return ok;
}

// ---- the guest ----

void on_guest_frame(void (*fn)(uint32_t frame)) {
    if (g_frozen) fatal("settings: on_guest_frame after the settings were loaded");
    guest_fns().push_back(fn);
}

// A setting a replay's log gives the guest. Replays keep going whatever they hold: a key
// this build lacks, or a value it cannot take, is said once and skipped.
static void apply_logged(const char* key, const char* text) {
    Setting* s = find(key);
    double v;
    if (s && (s->flag_bits() & AffectsGuest) && s->parse(text, v)) {
        Access::latch(*s, clamp_for(*s, v));
        return;
    }
    static bool said;
    if (!said) {
        said = true;
        fprintf(stderr, "[settings] the replay sets %s = %s, which this build's guest does not read; skipped\n", key, text);
    }
}

void start_guest() {
    freeze();
    if (input_replay_active()) {
        // The replay's own values, from the defaults up: a log from before a setting existed
        // was recorded with it at its default.
        for (Setting* s : g_guest) Access::latch(*s, s->default_value());
        input_replay_settings(0, apply_logged);
        return;
    }
    // Every one, changed or not: a replay starts from the defaults, which may have moved.
    for (Setting* s : g_guest) {
        Access::latch(*s, s->value());
        input_log_setting(0, s->key(), s->text().c_str());
    }
}

void guest_frame(uint32_t frame) {
    if (!g_guest.empty()) {
        if (input_replay_active()) {
            input_replay_settings(frame, apply_logged);
        } else {
            for (Setting* s : g_guest) {
                const double v = s->value();
                if (v == Access::latched(*s)) continue;
                Access::latch(*s, v);
                input_log_setting(frame, s->key(), s->text_of(v).c_str());
            }
        }
    }
    for (Setting* s : g_while_on)
        if (Access::latched(*s) != 0.0) Access::run_while_on(*s, frame);
    for (auto fn : guest_fns()) fn(frame);
}

}  // namespace settings
