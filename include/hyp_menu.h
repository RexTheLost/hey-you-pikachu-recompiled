#ifndef HYP_MENU_H
#define HYP_MENU_H

struct SDL_Window;

// The window's menu bar (Game / Graphics / Audio / Input / Help), Win32.
namespace hyp::menu {
    // Window thread: build the menu bar and apply the saved settings.
    void attach(SDL_Window* window);
    void toggle_fullscreen();
    void show_controls();
}

#endif
