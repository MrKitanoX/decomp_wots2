#pragma once
// The PC settings menu (native, not the game's): F1 or Esc anywhere, or L1 on
// the game's Settings screen.  Video, graphics, sound, controls, language and
// saving at any moment; the game and its sound stand still while it is open.
// Its own values live in settings.ini beside the program; the picture's shape,
// lines, frame rate and lighting stay in the mods' mod.ini (as F5 writes them).
union SDL_Event;   // (no <SDL.h> here: it would rename main.cpp's main)

namespace settings_menu {

void init(int argc, char* argv[]);   // reads settings.ini and applies it (after the mods)
bool event(const SDL_Event& ev);     // true = taken
bool is_open();
void open();
void close();
void draw();                         // inside the shared ImGui frame (native/editor.cpp)

}  // namespace settings_menu

extern "C" void Native_SettingsOpen();
extern "C" int  Native_SettingsIsOpen();
extern "C" int  Native_TakeTitleRequest();
