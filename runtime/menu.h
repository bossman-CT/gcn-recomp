// The in-game menu's entries beyond settings (settings.h): buttons, and the order of pages.
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

// The order of a menu's pages (path "Graphics") or of a page's sections ("Graphics/Display"):
// these first, in this order, then the rest alphabetically.
void layout(const char* path, std::initializer_list<const char*> children);

// Whether the menu is open, for code that wants to know (the frontend sets it).
bool is_open();
void set_open(bool open);

// For the frontend.
struct Button {
    const char *path, *label, *help, *confirm;
    void (*fn)();
    uint32_t order;
};
const std::vector<Button>& buttons();
// `children` sorted by layout() for `path`, then alphabetically.
void sort_children(const std::string& path, std::vector<std::string>& children);

}  // namespace menu
