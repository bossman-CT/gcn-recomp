// Settings: named, typed values a player changes from the in-game menu (overlay.h) or from
// settings.txt, each with a default, help text and flags.
//
// A setting is made by a static initializer, anywhere in the runtime or a game's SOURCES,
// and lives for the whole run:
//
//     static auto& show = settings::toggle("dev.show_thing", "Developer/Tools", "Show the thing")
//                             .help("What it does.");
//     ...
//     if (show) ...
//
// `key` is its name in settings.txt; it never changes once released, while the label can.
// Keys are lowercase and dotted, the first part naming the menu by convention (graphics.,
// audio., controls., enhancements., cheats., dev., menu.). `path` is where the menu shows
// it: "Menu/Page" or "Menu/Page/Section". `label` is what the menu says.
//
// Threads: the main thread is the only writer (the menu, settings.txt, --set), and
// on_change callbacks run there. Values are atomics, so any thread reads them without a
// lock. A setting flagged AffectsGuest is one the guest reads: get() returns the value
// latched at the guest's last frame boundary, so the guest sees one value for a whole
// frame, and every latched change is recorded in the input log against that frame
// (input_log_setting) and replayed from it, so a replay sees the same values at the same
// frames. Anything that writes guest memory runs on the guest thread, through while_on()
// or on_guest_frame(), never from the menu directly.
//
// This is the core of the menu: no SDL and no GL, so it links into every target, the
// headless benchmark included. The registry is frozen when the settings are loaded (or the
// guest starts); a registration after that, or a key registered twice, is fatal.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace settings {

enum Flags : uint32_t {
    Restart = 1 << 0,       // read once at startup: the menu says a change applies after a restart
    AffectsGuest = 1 << 1,  // the guest reads it: latched at each guest frame boundary, logged for replays
    Cheat = 1 << 2,         // changes the game's rules: a run with one on is a modified run
    Developer = 1 << 3,     // shown only with Developer > Show developer options
};

enum class Type : uint8_t { Bool, Int, Float, Choice };

// What every setting has, whatever its type: what the menu and settings.txt work with.
// Values are kept as doubles, which hold every bool, int, float and choice exactly.
class Setting {
public:
    Setting(Type type, const char* key, const char* path, const char* label, double def, double lo, double hi);
    virtual ~Setting() = default;

    Type type() const { return type_; }
    const char* key() const { return key_; }
    const char* path() const { return path_; }
    const char* label() const { return label_; }
    const char* help_text() const { return help_; }
    const char* format_text() const { return format_; }
    uint32_t flag_bits() const { return flags_; }
    double lo() const { return lo_; }
    double hi() const { return hi_; }
    double default_value() const { return def_; }
    const std::vector<const char*>& names() const { return names_; }
    uint32_t order() const { return order_; }

    // The value the menu shows and changes (any thread).
    double value() const { return value_.load(std::memory_order_relaxed); }
    // What the setting's readers see: for an AffectsGuest setting the value latched at the
    // guest's last frame boundary, otherwise the live value.
    double read() const {
        return (flags_ & AffectsGuest) ? latched_.load(std::memory_order_relaxed) : value();
    }
    bool is_default() const { return value() == def_; }
    // Whether it is enabled and shown, by the predicates below (main thread).
    bool enabled() const { return !enabled_if_ || enabled_if_(); }
    const char* disabled_why() const { return why_; }
    bool visible() const { return !visible_if_ || visible_if_(); }
    // Set for this run only, by --set or a command-line shorthand: what settings.txt holds
    // for it is written back unchanged, until the menu changes it.
    bool overridden() const { return overridden_; }

    // Main thread. Clamped to the setting's range; marks settings.txt for saving and calls
    // on_change if the value changed, and returns whether it did.
    bool set_value(double v);
    void reset() { set_value(def_); }

    // The value as settings.txt writes it ("on", "3", "0.5", a choice's name), and back.
    std::string text_of(double v) const;
    std::string text() const { return text_of(value()); }
    bool parse(const char* text, double& out) const;

protected:
    virtual void changed() {}
    friend struct Access;

    Type type_;
    const char* key_;
    const char* path_;
    const char* label_;
    const char* help_ = nullptr;
    const char* format_ = nullptr;
    const char* why_ = nullptr;
    uint32_t flags_ = 0;
    double def_, lo_, hi_;
    std::vector<const char*> names_;
    uint32_t order_;
    bool (*enabled_if_)() = nullptr;
    bool (*visible_if_)() = nullptr;
    void (*while_on_)(uint32_t frame) = nullptr;
    std::atomic<double> value_;
    std::atomic<double> latched_;
    bool overridden_ = false;
    std::string file_text_;  // what settings.txt held for it when loaded, or last saved
};

template <typename T>
class Typed : public Setting {
public:
    using Setting::Setting;
    T get() const { return (T)read(); }
    operator T() const { return get(); }
    void set(T v) { set_value((double)v); }

    // Options, chained where the setting is made.
    Typed& help(const char* text) { help_ = text; return *this; }                 // shown by the menu
    Typed& flags(uint32_t f) { flags_ |= f; return *this; }
    Typed& format(const char* printf_format) { format_ = printf_format; return *this; }  // a slider's label
    Typed& on_change(void (*fn)(T value)) { on_change_ = fn; return *this; }   // main thread, after a change
    Typed& enabled_if(bool (*fn)(), const char* why) { enabled_if_ = fn; why_ = why; return *this; }  // else greyed out
    Typed& visible_if(bool (*fn)()) { visible_if_ = fn; return *this; }
    // The guest thread, at the end of each guest frame the setting is on (latched): where a
    // setting that works on guest memory does it. Implies AffectsGuest.
    Typed& while_on(void (*fn)(uint32_t frame)) { while_on_ = fn; flags_ |= AffectsGuest; return *this; }

private:
    void changed() override { if (on_change_) on_change_(get_live()); }
    T get_live() const { return (T)value(); }
    void (*on_change_)(T) = nullptr;
};

// Making settings, from static initializers. The reference stays valid for the whole run.
Typed<bool>& toggle(const char* key, const char* path, const char* label, bool def = false);
Typed<int>& slider(const char* key, const char* path, const char* label, int def, int lo, int hi);
Typed<float>& slider(const char* key, const char* path, const char* label, float def, float lo, float hi);
// A choice is written to settings.txt by name, so reordering the names never changes what a
// file means: lowercase, with spaces as underscores ("Borderless window" -> borderless_window).
Typed<int>& choice(const char* key, const char* path, const char* label, std::initializer_list<const char*> names,
                   int def = 0);

// Every setting, in the order they were made; and one by its key, or null.
const std::vector<Setting*>& all();
Setting* find(const char* key);
// A counter shared with the menu's other entries (menu.h), so a page keeps the order its
// entries were made in.
uint32_t next_order();
// Stops registration: anything made after this is fatal. load() and start_guest() call it.
void freeze();

// ---- settings.txt ----
// The command line: --set=<key>=<value> (repeatable) changes a setting for this run only,
// and --settings=<path> names the file (default settings.txt in the working directory, or
// GCN_SETTINGS). Returns whether `arg` was one of these.
bool parse_arg(const char* arg);
// --list-settings: every setting, its default, its value and what it does.
int list(FILE* out);
// Reads the file (a missing one is fine), then applies --set. Call once, before the
// guest starts and before anything reads a setting it needs at startup.
void load();
// A setting changed for this run only, as --set does: what a command-line shorthand such
// as --scale=3 uses. False, with a warning, for an unknown key or a value it cannot take.
bool set_override(const char* key, const char* text);
// Writes the file if anything changed since it was read or last written: only settings
// changed from their defaults, and every line it did not recognize, through a temporary
// file. Main thread.
bool save();
bool dirty();
const std::string& file_path();

// ---- the guest ----
// Called on the guest thread at the end of each guest frame, after the settings latch.
void on_guest_frame(void (*fn)(uint32_t frame));
// The first latch, before the guest runs its first instruction (threads_start_boot).
void start_guest();
// The guest's frame boundary (gx/fifo.cpp): latches the AffectsGuest settings -- from the
// replay while one is running, otherwise from the live values, logging what changed --
// and runs the while_on and on_guest_frame callbacks.
void guest_frame(uint32_t frame);

}  // namespace settings
