#pragma once
#include <SDL.h>
struct ImFont;

// The level editor (native, not part of the game): F2 in the world.  See
// "The level editor" in the project notes and mods/README.md.
namespace editor {

// Every window event goes here first; true = the editor took it.
bool event(const SDL_Event& ev);
// Writes the graphics settings (picture, FPS, texture pack, lighting) into
// the mods' mod.ini files, as the F5 panel's Save buttons do.
void save_graphics();
// For the PC settings menu (native/settings_menu.cpp): the shared ImGui
// overlay on, its font, and whether the editor or photo mode is in use.
void show_overlay();
// 0 text (Segoe UI, 44 px), 1 titles (Cambria Bold, 64 px), 2 kanji (Yu Gothic Bold, 80 px); null if missing.
ImFont* menu_font(int kind = 0);
bool tool_open();
bool cursor_wanted();   // a tool, the lighting panel or the settings menu is open

}  // namespace editor
