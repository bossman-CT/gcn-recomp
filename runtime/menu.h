// The in-game menu's entries beyond settings (settings.h): buttons, notes, and the order of
// pages; and what a game shows over the picture itself: a hint, or a question.
// Like settings, they are registered from static initializers and frozen when the menu
// starts; the desktop frontend draws them (overlay.cpp). No SDL and no GL here, so a game's
// SOURCES can register entries in every target.
//
// The menu's top level is fixed: Game, Graphics, Controls, Audio, Enhancements, Cheats,
// then any menu a game adds (alphabetically), then Developer and Help. A menu with nothing
// in it is not shown. Pages and sections follow menu::layout(), then alphabetical order;
// entries in a page keep the order they were made in.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace menu {

// A button at `path` ("Menu/Page" or "Menu/Page/Section") that calls `fn` on the main
// thread. With `confirm`, the menu asks that question first. It must not write guest
// memory, which only the guest thread may do (settings::on_guest_frame).
void button(const char* path, const char* label, void (*fn)(), const char* help = nullptr,
            const char* confirm = nullptr);

// A line of text at `path`, wrapped to the page: what a page needs said that no setting
// says, such as where the game keeps a setting of its own.
void note(const char* path, const char* text);

// The order of a menu's pages (path "Graphics") or of a page's sections ("Graphics/Display"):
// these first, in this order, then the rest alphabetically.
void layout(const char* path, std::initializer_list<const char*> children);

// Whether the menu is open, for code that wants to know (the frontend sets it).
bool is_open();
void set_open(bool open);

// ---- over the game ----
// A line the frontend shows over the game while the menu is closed, whenever `text`
// returns one (null for none): a button a game offers on one of its screens, say. Called on
// the main thread each frame drawn. Register from a static initializer.
void hint(const char* (*text)());
// Asks the player a yes/no question in the menu's place: the game paused and its pad at rest,
// as with the menu open (even with "Let the game keep the controller"). Yes (A, Enter or a
// click) calls `yes` on the main thread; No, B or Esc just closes it. Main thread: a game's
// pad filter (input_ports::add_filter), say. A question not yet shown is replaced.
void ask(const char* question, void (*yes)());
// Ends the run as the menu's Quit does: settings.txt and the input log are saved. Main
// thread. Does nothing where no frontend has said how (the benchmark).
void quit();

// For the frontend.
struct Button {
    const char *path, *label, *help, *confirm;
    void (*fn)();
    uint32_t order;
};
const std::vector<Button>& buttons();
struct Note {
    const char *path, *text;
    uint32_t order;
};
const std::vector<Note>& notes();
// `children` sorted by layout() for `path`, then alphabetically.
void sort_children(const std::string& path, std::vector<std::string>& children);
// The hints registered, in order; and a question asked since the last call, if any.
const std::vector<const char* (*)()>& hints();
struct Question {
    const char* text;
    void (*yes)();
};
bool take_question(Question& out);
// How quit() ends the run.
void set_quit(void (*fn)());

}  // namespace menu
