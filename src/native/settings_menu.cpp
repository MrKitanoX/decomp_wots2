// The PC settings menu: see include/native/settings_menu.h.
//
// Tabs: Vídeo (window / borderless / exclusive fullscreen, monitor, size,
// refresh, vsync, frame rate), Gráficos (internal lines, shape, HUD, brightness,
// gamma, the modern lighting and HD textures), Áudio (overall, music, effects,
// voices, movies, mute when the window is in the background), Controles (keys
// and controller buttons, camera, dead zone, vibration) and Jogo (language,
// saving into a card slot at any moment outside a scene, back to the title,
// quit).  Everything applies at once; closing the menu writes it down.

#include "native/settings_menu.h"

#include <SDL.h>

#include "hal/gs.h"
#include "hal/pad.h"
#include "hal/spu2.h"
#include "native/editor.h"
#include "native/ini.h"
#include "native/texture_pack.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

extern "C" int Native_QuickSave(int slot);                                     // game/mcard.cpp
extern "C" int Native_SlotInfo(int used[3], int day[3], int period[3], int* current);
extern "C" int Native_GameInProgress();
extern "C" int Native_EventRunning();                                          // game/event_player.cpp

namespace {

const char* const kSettingsFile = "settings.ini";
const char* const kLanguageIni = "mods/ptbr/mod.ini";

bool sOpen = false;
std::vector<std::string> sArgs;

// ── Sound ────────────────────────────────────────────────────────────────────
struct Audio {
    int master = 70, music = 100, effects = 100, voices = 100, movies = 100;
    bool muteUnfocused = true;
};
Audio sAudio;
bool sFocused = true;

void apply_audio() {
    hal::spu2::set_master(sAudio.master / 100.0f);
    hal::spu2::set_category_volume(hal::spu2::kMusic, sAudio.music / 100.0f);
    hal::spu2::set_category_volume(hal::spu2::kEffects, sAudio.effects / 100.0f);
    hal::spu2::set_category_volume(hal::spu2::kVoice, sAudio.voices / 100.0f);
    hal::spu2::set_category_volume(hal::spu2::kMovie, sAudio.movies / 100.0f);
    hal::spu2::set_muted(sAudio.muteUnfocused && !sFocused);
}

// ── Screen (edited, then applied with a confirmation) ────────────────────────
struct Display {
    int mode = 0;                   // 0 window, 1 borderless fullscreen, 2 exclusive fullscreen
    int display = 0;
    int winW = 0, winH = 0;
    int exW = 0, exH = 0, exHz = 0;
};
Display sDisp;
bool sRevertAsk = false;
Uint32 sRevertUntil = 0;
hal::gs::Video sRevertTo;

Display display_from(const hal::gs::Video& v) {
    Display d;
    d.mode = !v.fullscreen ? 0 : v.exclusive ? 2 : 1;
    SDL_Window* w = hal::gs::window();
    d.display = v.display >= 0 ? v.display : (w ? std::max(0, SDL_GetWindowDisplayIndex(w)) : 0);
    if (v.windowWidth > 0) {
        d.winW = v.windowWidth;
        d.winH = v.windowHeight;
    } else if (w != nullptr && !v.fullscreen) {
        SDL_GetWindowSize(w, &d.winW, &d.winH);
    }
    SDL_DisplayMode desk = {};
    SDL_GetDesktopDisplayMode(d.display, &desk);
    d.exW = v.modeWidth > 0 ? v.modeWidth : desk.w;
    d.exH = v.modeHeight > 0 ? v.modeHeight : desk.h;
    d.exHz = v.modeRefresh > 0 ? v.modeRefresh : desk.refresh_rate;
    if (d.winW <= 0) { d.winW = 1280; d.winH = 960; }
    return d;
}
hal::gs::Video with_display(hal::gs::Video v, const Display& d) {
    v.fullscreen = d.mode != 0;
    v.exclusive = d.mode == 2;
    v.display = d.display;
    v.windowWidth = d.winW;
    v.windowHeight = d.winH;
    v.modeWidth = d.exW;
    v.modeHeight = d.exH;
    v.modeRefresh = d.exHz;
    return v;
}

// ── Controls ─────────────────────────────────────────────────────────────────
hal::pad::Config sPad;
int sCaptureKey = -1, sCapturePad = -1;
void note(const std::string& s);

const char* const kActionName[hal::pad::kActions] = {
    "Start (pausa)", "Select", "Direcional cima", "Direcional baixo", "Direcional esquerda", "Direcional direita",
    "X - Pular / confirmar", "Círculo - Conversar", "Quadrado - Atacar / pegar", "Triângulo - Atacar / cancelar",
    "L1 - Sacar / guardar espada", "R1 - Defender", "L2", "R2", "L3 - Correr", "R3 - Câmera atrás",
    "Andar para frente", "Andar para trás", "Andar para a esquerda", "Andar para a direita",
    "Câmera para cima", "Câmera para baixo", "Câmera para a esquerda", "Câmera para a direita",
    "Andar inteiro (segurar)",
};

std::string pad_input_name(int in) {
    if (in == hal::pad::kPadLeftTrigger) return "LT / L2";
    if (in == hal::pad::kPadRightTrigger) return "RT / R2";
    switch (in) {
    case SDL_CONTROLLER_BUTTON_A: return "A / X";
    case SDL_CONTROLLER_BUTTON_B: return "B / Círculo";
    case SDL_CONTROLLER_BUTTON_X: return "X / Quadrado";
    case SDL_CONTROLLER_BUTTON_Y: return "Y / Triângulo";
    case SDL_CONTROLLER_BUTTON_BACK: return "View / Select";
    case SDL_CONTROLLER_BUTTON_START: return "Menu / Start";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB / L1";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB / R1";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "Direcional cima";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "Direcional baixo";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "Direcional esquerda";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "Direcional direita";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "Guia";
    default: break;
    }
    if (in < 0) return "(nenhum)";
    const char* s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)in);
    return s ? s : "?";
}
std::string pad_input_id(int in) {
    if (in == hal::pad::kPadLeftTrigger) return "lefttrigger";
    if (in == hal::pad::kPadRightTrigger) return "righttrigger";
    if (in < 0 || in >= SDL_CONTROLLER_BUTTON_MAX) return "none";
    const char* s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)in);
    return s ? s : "none";
}
// Mouse inputs: SDL buttons 1-5, the wheel as 6/7, -1 none.
static const char* const kMouseIds[] = {"none", "left", "middle", "right", "x1", "x2", "wheel_up", "wheel_down"};
static const char* const kMouseNames[] = {"—", "Botão esquerdo", "Botão do meio", "Botão direito",
                                          "Botão lateral 1", "Botão lateral 2", "Roda para cima", "Roda para baixo"};
const char* mouse_input_id(int in) { return in >= 1 && in <= 7 ? kMouseIds[in] : "none"; }
const char* mouse_input_name(int in) { return in >= 1 && in <= 7 ? kMouseNames[in] : kMouseNames[0]; }
int mouse_input_from_id(const std::string& s) {
    for (int i = 1; i <= 7; i++)
        if (s == kMouseIds[i]) return i;
    return s == "none" ? -1 : -2;
}
static bool same_bindings(const hal::pad::Config& a, const hal::pad::Config& b) {
    for (int i = 0; i < hal::pad::kActions; i++)
        if (a.keys[i] != b.keys[i]) return false;
    for (int i = 0; i < hal::pad::kButtonActions; i++)
        if (a.mouse[i] != b.mouse[i]) return false;
    return a.walkDefaultFull == b.walkDefaultFull;
}
const char* layout_id(const hal::pad::Config& c) {
    if (same_bindings(c, hal::pad::default_config())) return "moderno";
    if (same_bindings(c, hal::pad::classic_config())) return "classico";
    return c.walkDefaultFull ? "personalizado" : "personalizado_classico";
}

int pad_input_from_id(const std::string& s) {
    if (s == "lefttrigger") return hal::pad::kPadLeftTrigger;
    if (s == "righttrigger") return hal::pad::kPadRightTrigger;
    if (s == "none") return -1;
    const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(s.c_str());
    return b == SDL_CONTROLLER_BUTTON_INVALID ? -2 : (int)b;
}

// ── Language (the PT-BR mod) ─────────────────────────────────────────────────
bool sHasPtbr = false, sPtbrLoaded = false, sPtbrWanted = false;

bool read_ptbr_enabled() {
    std::map<std::string, std::string> m;
    if (!ini::read(kLanguageIni, m))
        return false;
    // (the release package carries only the patch: the files exist once the
    // installer has made them from the player's disc)
    if (FILE* f = fopen("mods/ptbr/files/Us/task/alltask", "rb")) fclose(f);
    else return false;
    sHasPtbr = true;
    auto it = m.find("mod.enabled");
    return it == m.end() || it->second != "0";
}

void restart() {
    // Writes everything down, then starts the game again with the same
    // arguments and leaves.
    settings_menu::close();
    std::string exe = sArgs.empty() ? std::string("wots2") : sArgs[0];
#ifdef _WIN32
    if (char* base = SDL_GetBasePath()) {
        exe = std::string(base) + "wots2.exe";
        SDL_free(base);
    }
    std::vector<std::string> quoted;
    for (size_t i = 0; i < sArgs.size(); i++)
        quoted.push_back("\"" + (i == 0 ? exe : sArgs[i]) + "\"");
    if (quoted.empty())
        quoted.push_back("\"" + exe + "\"");
    std::vector<const char*> argv;
    for (const std::string& s : quoted) argv.push_back(s.c_str());
    argv.push_back(nullptr);
    _spawnv(_P_NOWAIT, exe.c_str(), argv.data());
#else
    std::vector<char*> argv;
    for (std::string& s : sArgs) argv.push_back(s.data());
    argv.push_back(nullptr);
    if (fork() == 0) {
        execv(exe.c_str(), argv.data());
        _exit(1);
    }
#endif
    printf("[settings] restarting\n");
    exit(0);
}

// ── Saving ───────────────────────────────────────────────────────────────────
int sSlotUsed[3] = {}, sSlotDay[3] = {}, sSlotPeriod[3] = {}, sCurrentSlot = -1;
bool sHaveCard = false;
std::string sNote;
Uint32 sNoteUntil = 0;
bool sTitleRequest = false;
bool sGraphicsChanged = false;

void refresh_slots() {
    sHaveCard = Native_SlotInfo(sSlotUsed, sSlotDay, sSlotPeriod, &sCurrentSlot) != 0;
}
void note(const std::string& s) {
    sNote = s;
    sNoteUntil = SDL_GetTicks() + 6000;
}

// ── settings.ini ─────────────────────────────────────────────────────────────
std::string num(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.3g", v);
    return b;
}
std::string size_str(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }
bool parse_size(const std::string& s, int* w, int* h) { return sscanf(s.c_str(), "%dx%d", w, h) == 2 && *w > 0 && *h > 0; }

void save_settings() {
    const hal::gs::Video v = hal::gs::video();
    const Display d = display_from(v);
    static const char* const kModes[] = {"window", "borderless", "exclusive"};
    ini::write(kSettingsFile, {
        {"mode", kModes[d.mode]}, {"display", std::to_string(d.display)},
        {"window", size_str(d.winW, d.winH)}, {"fullscreen_size", size_str(d.exW, d.exH)},
        {"refresh", std::to_string(d.exHz)},
        {"brightness", num(v.brightness)}, {"gamma", num(v.gamma)},
    }, "display", true);
    ini::write(kSettingsFile, {
        {"master", std::to_string(sAudio.master)}, {"music", std::to_string(sAudio.music)},
        {"effects", std::to_string(sAudio.effects)}, {"voices", std::to_string(sAudio.voices)},
        {"movies", std::to_string(sAudio.movies)}, {"mute_unfocused", sAudio.muteUnfocused ? "1" : "0"},
    }, "audio", true);
    std::vector<std::pair<std::string, std::string>> controls;
    controls.push_back({"layout", layout_id(sPad)});
    for (int a = 0; a < hal::pad::kActions; a++) {
        const char* name = sPad.keys[a] > 0 ? SDL_GetScancodeName((SDL_Scancode)sPad.keys[a]) : "";
        controls.push_back({std::string("key_") + hal::pad::action_id(a), *name ? name : "none"});
    }
    for (int a = 0; a < hal::pad::kButtonActions; a++)
        controls.push_back({std::string("pad_") + hal::pad::action_id(a), pad_input_id(sPad.pad[a])});
    controls.push_back({"camera_sensitivity", num(sPad.cameraSensitivity)});
    controls.push_back({"invert_x", sPad.invertX ? "1" : "0"});
    controls.push_back({"invert_y", sPad.invertY ? "1" : "0"});
    controls.push_back({"deadzone", num(sPad.deadzone)});
    controls.push_back({"rumble", sPad.rumble ? "1" : "0"});
    for (int a = 0; a < hal::pad::kButtonActions; a++)
        controls.push_back({std::string("mouse_") + hal::pad::action_id(a), mouse_input_id(sPad.mouse[a])});
    controls.push_back({"mouse_sensitivity", num(sPad.mouseSensitivity)});
    controls.push_back({"mouse_invert_y", sPad.mouseInvertY ? "1" : "0"});
    controls.push_back({"mouse_capture", sPad.mouseCapture ? "1" : "0"});
    ini::write(kSettingsFile, controls, "controls", true);
    if (sGraphicsChanged) {
        editor::save_graphics();
        sGraphicsChanged = false;
    }
}

void load_settings() {
    std::map<std::string, std::string> m;
    const bool have = ini::read(kSettingsFile, m);
    auto get = [&](const char* key, const char* fallback = "") {
        auto it = m.find(key);
        return it == m.end() ? std::string(fallback) : it->second;
    };
    sPad = hal::pad::default_config();
    if (have) {
        hal::gs::Video v = hal::gs::video();
        Display d = display_from(v);
        const std::string mode = get("display.mode", "window");
        d.mode = mode == "exclusive" ? 2 : mode == "borderless" ? 1 : 0;
        d.display = atoi(get("display.display", "0").c_str());
        parse_size(get("display.window"), &d.winW, &d.winH);
        parse_size(get("display.fullscreen_size"), &d.exW, &d.exH);
        d.exHz = atoi(get("display.refresh", "0").c_str());
        v = with_display(v, d);
        v.brightness = (float)atof(get("display.brightness", "1").c_str());
        v.gamma = (float)atof(get("display.gamma", "1").c_str());
        if (v.brightness <= 0.0f) v.brightness = 1.0f;
        if (v.gamma <= 0.0f) v.gamma = 1.0f;
        hal::gs::set_video(v);

        auto pct = [&](const char* key, int fallback) {
            const std::string s = get(key);
            const int n = s.empty() ? fallback : atoi(s.c_str());
            return std::clamp(n, 0, 100);
        };
        sAudio.master = pct("audio.master", 70);
        sAudio.music = pct("audio.music", 100);
        sAudio.effects = pct("audio.effects", 100);
        sAudio.voices = pct("audio.voices", 100);
        sAudio.movies = pct("audio.movies", 100);
        sAudio.muteUnfocused = get("audio.mute_unfocused", "1") != "0";

        // Before the mouse (no "layout"), the keys saved were the old layout's
        // defaults: such a file starts on the modern one instead.
        const std::string layout = get("controls.layout");
        const bool classic = layout == "classico" || layout == "personalizado_classico";
        if (classic) sPad = hal::pad::classic_config();
        if (!layout.empty()) {
            for (int a = 0; a < hal::pad::kActions; a++) {
                const std::string s = get((std::string("controls.key_") + hal::pad::action_id(a)).c_str());
                if (s == "none") sPad.keys[a] = SDL_SCANCODE_UNKNOWN;
                else if (!s.empty() && SDL_GetScancodeFromName(s.c_str()) != SDL_SCANCODE_UNKNOWN)
                    sPad.keys[a] = SDL_GetScancodeFromName(s.c_str());
            }
            for (int a = 0; a < hal::pad::kButtonActions; a++) {
                const std::string s = get((std::string("controls.mouse_") + hal::pad::action_id(a)).c_str());
                if (!s.empty() && mouse_input_from_id(s) != -2)
                    sPad.mouse[a] = mouse_input_from_id(s);
            }
        }
        for (int a = 0; a < hal::pad::kButtonActions; a++) {
            const std::string s = get((std::string("controls.pad_") + hal::pad::action_id(a)).c_str());
            if (!s.empty() && pad_input_from_id(s) != -2)
                sPad.pad[a] = pad_input_from_id(s);
        }
        auto f = [&](const char* key, float fallback, float lo, float hi) {
            const std::string s = get(key);
            return std::clamp(s.empty() ? fallback : (float)atof(s.c_str()), lo, hi);
        };
        sPad.cameraSensitivity = f("controls.camera_sensitivity", 1.0f, 0.25f, 2.0f);
        sPad.invertX = get("controls.invert_x", "0") == "1";
        sPad.invertY = get("controls.invert_y", "0") == "1";
        sPad.deadzone = f("controls.deadzone", 0.21f, 0.05f, 0.5f);
        sPad.rumble = get("controls.rumble", "1") != "0";
        sPad.mouseSensitivity = f("controls.mouse_sensitivity", 1.0f, 0.1f, 4.0f);
        sPad.mouseInvertY = get("controls.mouse_invert_y", "0") == "1";
        sPad.mouseCapture = get("controls.mouse_capture", "1") != "0";
        printf("[settings] %s read\n", kSettingsFile);
    }
    hal::pad::set_config(sPad);
    apply_audio();
}

// ── Lighting presets ─────────────────────────────────────────────────────────
void lighting_preset(int p) {      // 1 low … 4 ultra
    hal::gs::Lighting l = hal::gs::lighting();
    l.shadows = true;
    l.mapShadows = p >= 2;
    l.shadowSize = p <= 1 ? 1024 : p == 2 ? 2048 : 4096;
    l.ao = p >= 2;
    l.lanterns = true;
    l.lanternShadows = p >= 3;
    l.bloom = p <= 1 ? 0.3f : 0.58f;
    l.godRays = p >= 3 ? (p == 4 ? 1.0f : 0.5f) : 0.0f;
    l.bump = p >= 3 ? (p == 4 ? 2.0f : 1.0f) : 0.0f;
    l.characterBump = p == 4 ? 0.5f : 0.0f;
    l.dust = p >= 2 ? 1.0f : 0.0f;
    l.moods = p >= 2;
    l.grade = true;
    hal::gs::set_lighting(l);
    if (hal::gs::lighting_held())
        hal::gs::toggle_lighting();
    sGraphicsChanged = true;
}

int nearest(float value, const std::vector<float>& options) {
    int best = 0;
    float gap = 1e30f;
    for (int i = 0; i < (int)options.size(); i++)
        if (fabsf(options[i] - value) < gap) { gap = fabsf(options[i] - value); best = i; }
    return best;
}

// ── The look: lacquer and gold ───────────────────────────────────────────────
// Night-blue panels with a seigaiha wave pattern, red lacquer for what is
// chosen, gold for frames and titles, cream text — the colours of the game's
// own menus, drawn with ImGui's draw lists.
const ImU32 kNight = IM_COL32(20, 26, 51, 255);         // #141A33
const ImU32 kNightDeep = IM_COL32(11, 14, 30, 255);
const ImU32 kWave = IM_COL32(52, 66, 112, 255);
const ImU32 kLacquer = IM_COL32(142, 27, 27, 255);      // #8E1B1B
const ImU32 kLacquerDark = IM_COL32(84, 14, 16, 255);
const ImU32 kLacquerLight = IM_COL32(184, 46, 40, 255);
const ImU32 kGold = IM_COL32(201, 162, 74, 255);        // #C9A24A
const ImU32 kGoldLight = IM_COL32(244, 214, 140, 255);
const ImU32 kCream = IM_COL32(239, 230, 207, 255);      // #EFE6CF
const ImU32 kBlack = IM_COL32(0, 0, 0, 255);

const char* const kTabName[] = {"Vídeo", "Gráficos", "Áudio", "Controles", "Jogo"};
const char* const kTabKanji[] = {"映像", "画質", "音声", "操作", "遊戯"};
constexpr int kTabs = 5;

int sTab = 0;
float sTabSince = 0.0f, sOpenedAt = 0.0f;
float sAlpha = 1.0f;                // the fade of what is being drawn
float sScale = 1.0f;                // 1 at 1080 lines
float sBodyPx = 26.0f;              // text size, pixels
bool sPadInput = false;             // the last input came from a controller
bool sFocusFirst = false;           // put the keyboard / controller cursor on the first row
bool sFocusContent = false;         // give the rows' window the focus
int sCaptureMouse = -1;
int sCaptureEndFrame = -100;
std::string sTip, sTipNext;         // the focused row's description (footer)
ImFont* sBody = nullptr;
ImFont* sTitle = nullptr;
ImFont* sKanji = nullptr;

ImU32 col(ImU32 c, float mul = 1.0f) {
    const float a = (float)((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(sAlpha * mul, 0.0f, 1.0f);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)(a + 0.5f) << IM_COL32_A_SHIFT);
}
ImU32 mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int shift) {
        const float x = (float)((a >> shift) & 0xFF), y = (float)((b >> shift) & 0xFF);
        return (ImU32)(x + (y - x) * t + 0.5f) << shift;
    };
    return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}
float ease(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }
ImFont* font_or_default(ImFont* f) { return f != nullptr ? f : ImGui::GetFont(); }
ImVec2 text_size(ImFont* f, float px, const char* t, const char* end = nullptr) {
    return font_or_default(f)->CalcTextSizeA(px, FLT_MAX, 0.0f, t, end);
}
void text(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 c, const char* t, const char* end = nullptr) {
    dl->AddText(font_or_default(f), px, ImVec2(floorf(p.x), floorf(p.y)), c, t, end);
}
// Text shortened with "..." to fit a width.
std::string fit(ImFont* f, float px, const std::string& s, float width) {
    if (text_size(f, px, s.c_str()).x <= width) return s;
    std::string out = s;
    while (!out.empty() && text_size(f, px, (out + "...").c_str()).x > width) {
        out.pop_back();
        while (!out.empty() && (out.back() & 0xC0) == 0x80) out.pop_back();   // whole UTF-8 characters
    }
    return out + "...";
}
void triangle(ImDrawList* dl, ImVec2 c, float r, int dir, ImU32 color) {   // dir: -1 left, +1 right
    dl->AddTriangleFilled(ImVec2(c.x + dir * r, c.y), ImVec2(c.x - dir * r * 0.7f, c.y - r),
                          ImVec2(c.x - dir * r * 0.7f, c.y + r), color);
}
void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 color) {
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), color);
}

// The waves (seigaiha): rows of half-discs, each row over the one above.
void seigaiha(ImDrawList* dl, ImRect r, float radius, ImU32 fill, ImU32 line) {
    dl->PushClipRect(r.Min, r.Max, true);
    const float thick = std::max(1.0f, radius * 0.045f);
    int row = 0;
    for (float y = r.Min.y; y < r.Max.y + radius; y += radius * 0.5f, row++) {
        const float shift = (row & 1) ? radius : 0.0f;
        for (float x = r.Min.x - radius + shift; x < r.Max.x + radius; x += radius * 2.0f) {
            dl->PathArcTo(ImVec2(x, y), radius, IM_PI, 2.0f * IM_PI, 14);
            dl->PathFillConvex(fill);
            for (float k : {0.92f, 0.68f, 0.44f}) {
                dl->PathArcTo(ImVec2(x, y), radius * k, IM_PI, 2.0f * IM_PI, 14);
                dl->PathStroke(line, 0, thick);
            }
        }
    }
    dl->PopClipRect();
}
// Two gold lines with gold corner brackets: the game's frame.
void gold_frame(ImDrawList* dl, ImRect r, float s, float mul = 1.0f) {
    dl->AddRect(r.Min, r.Max, col(kGold, mul), 3.0f * s, 0, 2.0f * s);
    const float in = 5.0f * s;
    dl->AddRect(ImVec2(r.Min.x + in, r.Min.y + in), ImVec2(r.Max.x - in, r.Max.y - in), col(kGold, 0.35f * mul), 2.0f * s, 0, 1.0f * s);
    const float k = 18.0f * s, t = 3.0f * s;
    const ImVec2 c[4] = {r.Min, ImVec2(r.Max.x, r.Min.y), r.Max, ImVec2(r.Min.x, r.Max.y)};
    const float dx[4] = {1, -1, -1, 1}, dy[4] = {1, 1, -1, -1};
    for (int i = 0; i < 4; i++) {
        dl->AddLine(c[i], ImVec2(c[i].x + dx[i] * k, c[i].y), col(kGoldLight, mul), t);
        dl->AddLine(c[i], ImVec2(c[i].x, c[i].y + dy[i] * k), col(kGoldLight, mul), t);
    }
}

// ── Rows ─────────────────────────────────────────────────────────────────────
// Every setting is a row the width of the panel: its name on the left, its
// value on the right.  Mouse: hover and click (the arrows, the bar).  Keyboard
// and controller: up / down to move, left / right to change, Enter / A to use.
struct Row {
    ImRect r;
    bool pressed = false, hovered = false, focused = false, active = false, disabled = false, lit = false;
    bool byMouse = false;
};
float row_h() { return 54.0f * sScale; }
float pad_x() { return 22.0f * sScale; }
bool nav_visible() { return GImGui->NavCursorVisible; }

// Called right after an item: the cursor goes there when a tab opens.
void focus_hook() {
    if (sFocusFirst && ImGui::GetItemID() != 0) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        sFocusFirst = false;
    }
}
Row row_begin(const char* id, const char* tip, float height = 0.0f) {
    Row w;
    const float h = height > 0.0f ? height : row_h();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    w.disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    w.pressed = ImGui::InvisibleButton(id, ImVec2(width, h), ImGuiButtonFlags_EnableNav);
    if (!w.disabled) focus_hook();
    w.r = ImRect(p, ImVec2(p.x + width, p.y + h));
    w.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    w.focused = ImGui::IsItemFocused();
    w.active = ImGui::IsItemActive();
    w.byMouse = w.pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (w.pressed && ImGui::GetFrameCount() - sCaptureEndFrame < 15)
        w.pressed = false;                 // (the press that ended a capture)
    w.lit = (w.hovered && !w.disabled) || (w.focused && nav_visible());
    if (tip != nullptr && (w.hovered || (w.focused && nav_visible())))
        sTipNext = tip;
    return w;
}
// The row's background: a lacquer glow and the gold cursor when it is chosen.
void row_back(ImDrawList* dl, const Row& w) {
    const float s = sScale;
    if (w.lit) {
        const bool strong = w.focused && nav_visible();
        dl->AddRectFilledMultiColor(w.r.Min, w.r.Max, col(kLacquer, strong ? 0.95f : 0.6f), col(kLacquerDark, 0.15f),
                                    col(kLacquerDark, 0.15f), col(kLacquer, strong ? 0.95f : 0.6f));
        dl->AddLine(ImVec2(w.r.Min.x, w.r.Max.y - 1.0f), ImVec2(w.r.Max.x, w.r.Max.y - 1.0f), col(kGold, 0.55f), 1.0f * s);
        dl->AddLine(ImVec2(w.r.Min.x, w.r.Min.y), ImVec2(w.r.Max.x, w.r.Min.y), col(kGold, 0.3f), 1.0f * s);
        triangle(dl, ImVec2(w.r.Min.x + 9.0f * s, (w.r.Min.y + w.r.Max.y) * 0.5f), 6.0f * s, 1, col(kGoldLight));
    } else {
        dl->AddLine(ImVec2(w.r.Min.x + pad_x(), w.r.Max.y - 1.0f), ImVec2(w.r.Max.x - pad_x(), w.r.Max.y - 1.0f),
                    col(kGold, 0.12f), 1.0f);
    }
}
void row_label(ImDrawList* dl, const Row& w, const char* label, float maxWidth) {
    const float y = (w.r.Min.y + w.r.Max.y - sBodyPx) * 0.5f;
    const std::string t = fit(sBody, sBodyPx, label, maxWidth);
    text(dl, sBody, sBodyPx, ImVec2(w.r.Min.x + pad_x() + 10.0f * sScale, y),
         col(w.lit ? kGoldLight : kCream, w.disabled ? 0.4f : 1.0f), t.c_str());
}
ImRect value_rect(const Row& w) {
    const float width = std::min(w.r.GetWidth() * 0.46f, 470.0f * sScale);
    return ImRect(ImVec2(w.r.Max.x - pad_x() - width, w.r.Min.y + 8.0f * sScale), ImVec2(w.r.Max.x - pad_x(), w.r.Max.y - 8.0f * sScale));
}
int nav_dir(const Row& w) {
    if (!w.focused || w.disabled || ImGui::GetFrameCount() - sCaptureEndFrame < 15) return 0;
    using namespace ImGui;
    if (IsKeyPressed(ImGuiKey_LeftArrow) || IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
        IsKeyPressed(ImGuiKey_GamepadLStickLeft) || IsKeyPressed(ImGuiKey_A))
        return -1;
    if (IsKeyPressed(ImGuiKey_RightArrow) || IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
        IsKeyPressed(ImGuiKey_GamepadLStickRight) || IsKeyPressed(ImGuiKey_D))
        return 1;
    return 0;
}

// ◀ value ▶
bool option(const char* label, int* cur, const std::vector<std::string>& items, const char* tip = nullptr) {
    Row w = row_begin(label, tip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImRect v = value_rect(w);
    const float aw = 34.0f * sScale;
    const ImRect left(v.Min, ImVec2(v.Min.x + aw, v.Max.y)), right(ImVec2(v.Max.x - aw, v.Min.y), v.Max);
    const int n = (int)items.size();
    bool changed = false;
    if (!w.disabled && n > 0) {
        int dir = nav_dir(w);
        if (w.pressed) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            dir = w.byMouse && left.Contains(m) ? -1 : 1;
        }
        if (dir != 0) {
            *cur = ((*cur + dir) % n + n) % n;
            changed = true;
        }
    }
    row_back(dl, w);
    row_label(dl, w, label, v.Min.x - w.r.Min.x - pad_x() * 2.0f);
    const float dim = w.disabled ? 0.35f : 1.0f;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float cy = (v.Min.y + v.Max.y) * 0.5f;
    triangle(dl, ImVec2(left.GetCenter().x, cy), 8.0f * sScale, -1, col(w.hovered && left.Contains(m) ? kGoldLight : kGold, dim));
    triangle(dl, ImVec2(right.GetCenter().x, cy), 8.0f * sScale, 1, col(w.hovered && right.Contains(m) ? kGoldLight : kGold, dim));
    const std::string value = *cur >= 0 && *cur < n ? items[*cur] : std::string("-");
    const std::string shown = fit(sBody, sBodyPx, value, v.GetWidth() - aw * 2.0f - 8.0f * sScale);
    const ImVec2 ts = text_size(sBody, sBodyPx, shown.c_str());
    const float ty = cy - sBodyPx * 0.5f - (n > 1 && n <= 8 ? 4.0f * sScale : 0.0f);
    text(dl, sBody, sBodyPx, ImVec2(v.GetCenter().x - ts.x * 0.5f, ty), col(kCream, dim), shown.c_str());
    if (n > 1 && n <= 8) {          // a dot for each choice
        const float gap = 12.0f * sScale, y = v.Max.y - 4.0f * sScale;
        float x = v.GetCenter().x - gap * (n - 1) * 0.5f;
        for (int i = 0; i < n; i++, x += gap)
            diamond(dl, ImVec2(x, y), (i == *cur ? 3.5f : 2.2f) * sScale, col(i == *cur ? kGoldLight : kGold, (i == *cur ? 1.0f : 0.4f) * dim));
    }
    return changed;
}

// A bar: drag it, click it, or left / right in steps.
bool slider_core(const char* label, float* value, float lo, float hi, float step, float dragSnap,
                 const std::string& shown, const char* tip) {
    Row w = row_begin(label, tip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImRect v = value_rect(w);
    const float numW = 86.0f * sScale;
    const float cy = (v.Min.y + v.Max.y) * 0.5f;
    const ImRect bar(ImVec2(v.Min.x + 6.0f * sScale, cy - 4.0f * sScale), ImVec2(v.Max.x - numW, cy + 4.0f * sScale));
    bool changed = false;
    const float before = *value;
    if (!w.disabled) {
        const int dir = nav_dir(w);
        if (dir != 0) *value = std::clamp(*value + dir * step, lo, hi);
        const ImGuiIO& io = ImGui::GetIO();
        ImRect grab = bar;
        grab.Expand(ImVec2(10.0f * sScale, 16.0f * sScale));
        if (w.active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && grab.Contains(io.MouseClickedPos[0])) {
            float t = std::clamp((io.MousePos.x - bar.Min.x) / bar.GetWidth(), 0.0f, 1.0f);
            float x = lo + t * (hi - lo);
            if (dragSnap > 0.0f) x = roundf(x / dragSnap) * dragSnap;
            *value = std::clamp(x, lo, hi);
        }
    }
    changed = *value != before;
    row_back(dl, w);
    row_label(dl, w, label, v.Min.x - w.r.Min.x - pad_x() * 2.0f);
    const float dim = w.disabled ? 0.35f : 1.0f;
    const float t = hi > lo ? std::clamp((*value - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    const float x = bar.Min.x + bar.GetWidth() * t;
    dl->AddRectFilled(bar.Min, bar.Max, col(kNightDeep, 0.9f * dim), 4.0f * sScale);
    if (x > bar.Min.x + 1.0f)
        dl->AddRectFilledMultiColor(bar.Min, ImVec2(x, bar.Max.y), col(kLacquerDark, dim), col(kLacquerLight, dim),
                                    col(kLacquerLight, dim), col(kLacquerDark, dim));
    dl->AddRect(bar.Min, bar.Max, col(kGold, 0.5f * dim), 4.0f * sScale, 0, 1.0f * sScale);
    diamond(dl, ImVec2(x, cy), 11.0f * sScale, col(kNightDeep, dim));
    diamond(dl, ImVec2(x, cy), 9.0f * sScale, col(w.lit ? kGoldLight : kGold, dim));
    const ImVec2 ts = text_size(sBody, sBodyPx, shown.c_str());
    text(dl, sBody, sBodyPx, ImVec2(v.Max.x - ts.x, cy - sBodyPx * 0.5f), col(kCream, dim), shown.c_str());
    return changed;
}
bool slider(const char* label, float* value, float lo, float hi, float step, const char* fmt, const char* tip = nullptr) {
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *value);
    return slider_core(label, value, lo, hi, step, 0.01f, buf, tip);
}
bool slider_pct(const char* label, int* value, const char* tip = nullptr) {
    float f = (float)*value;
    const bool changed = slider_core(label, &f, 0.0f, 100.0f, 5.0f, 1.0f, std::to_string(*value) + "%", tip);
    *value = (int)lroundf(f);
    return changed;
}

// A lacquer switch that slides.
bool toggle(const char* label, bool* on, const char* tip = nullptr) {
    Row w = row_begin(label, tip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool changed = false;
    if (!w.disabled) {
        const int dir = nav_dir(w);
        if (w.pressed) { *on = !*on; changed = true; }
        else if (dir != 0 && *on != (dir > 0)) { *on = dir > 0; changed = true; }
    }
    float& anim = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID() + 1, *on ? 1.0f : 0.0f);
    anim += ((*on ? 1.0f : 0.0f) - anim) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    row_back(dl, w);
    const ImRect v = value_rect(w);
    row_label(dl, w, label, v.Min.x - w.r.Min.x - pad_x() * 2.0f);
    const float dim = w.disabled ? 0.35f : 1.0f;
    const float cy = (v.Min.y + v.Max.y) * 0.5f;
    const float pw = 66.0f * sScale, ph = 30.0f * sScale;
    const ImRect pill(ImVec2(v.Max.x - pw, cy - ph * 0.5f), ImVec2(v.Max.x, cy + ph * 0.5f));
    dl->AddRectFilled(pill.Min, pill.Max, col(mix(kNightDeep, kLacquer, anim), dim), ph * 0.5f);
    dl->AddRect(pill.Min, pill.Max, col(kGold, (0.45f + 0.55f * anim) * dim), ph * 0.5f, 0, 1.5f * sScale);
    const float kx = pill.Min.x + ph * 0.5f + (pw - ph) * anim;
    dl->AddCircleFilled(ImVec2(kx, cy), ph * 0.36f, col(mix(kCream, kGoldLight, anim), dim));
    const char* state = *on ? "Ligado" : "Desligado";
    const ImVec2 ts = text_size(sBody, sBodyPx, state);
    text(dl, sBody, sBodyPx, ImVec2(pill.Min.x - 14.0f * sScale - ts.x, cy - sBodyPx * 0.5f),
         col(*on ? kGoldLight : kCream, (*on ? 1.0f : 0.6f) * dim), state);
    return changed;
}

// A lacquer button: on its own (in a dialog) or at the right of a row.
void button_face(ImDrawList* dl, ImRect r, const char* label, bool lit, bool held, bool disabled) {
    const float dim = disabled ? 0.35f : 1.0f;
    const float rr = 4.0f * sScale;
    dl->AddRectFilled(ImVec2(r.Min.x + 2.0f * sScale, r.Min.y + 3.0f * sScale), ImVec2(r.Max.x + 2.0f * sScale, r.Max.y + 3.0f * sScale),
                      col(kBlack, 0.45f * dim), rr);
    const ImU32 top = held ? kLacquerDark : lit ? kLacquerLight : kLacquer;
    dl->AddRectFilledMultiColor(r.Min, r.Max, col(top, dim), col(top, dim), col(kLacquerDark, dim), col(kLacquerDark, dim));
    dl->AddRect(r.Min, r.Max, col(lit ? kGoldLight : kGold, dim), rr, 0, (lit ? 2.0f : 1.5f) * sScale);
    const ImVec2 ts = text_size(sBody, sBodyPx, label);
    text(dl, sBody, sBodyPx, ImVec2(r.GetCenter().x - ts.x * 0.5f, r.GetCenter().y - sBodyPx * 0.5f),
         col(lit ? kGoldLight : kCream, dim), label);
}
bool button(const char* label, float minWidth = 0.0f) {
    const ImVec2 ts = text_size(sBody, sBodyPx, label);
    const ImVec2 size(std::max(minWidth, ts.x + 48.0f * sScale), sBodyPx + 22.0f * sScale);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    if (pressed && ImGui::GetFrameCount() - sCaptureEndFrame < 15) pressed = false;
    const bool lit = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && nav_visible());
    button_face(ImGui::GetWindowDrawList(), ImRect(p, ImVec2(p.x + size.x, p.y + size.y)), label, lit && !disabled,
                ImGui::IsItemActive(), disabled);
    return pressed;
}
bool action(const char* label, const char* buttonText, const char* tip = nullptr) {
    Row w = row_begin(label, tip);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    row_back(dl, w);
    const ImRect v = value_rect(w);
    row_label(dl, w, label, v.Min.x - w.r.Min.x - pad_x() * 2.0f);
    const ImVec2 ts = text_size(sBody, sBodyPx, buttonText);
    const float bw = std::min(v.GetWidth(), ts.x + 56.0f * sScale);
    button_face(dl, ImRect(ImVec2(v.Max.x - bw, v.Min.y), v.Max), buttonText, w.lit && !w.disabled, w.active, w.disabled);
    return w.pressed && !w.disabled;
}

// A title over a group of rows.
void section(const char* title) {
    ImGui::Dummy(ImVec2(1.0f, 14.0f * sScale));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float px = 30.0f * sScale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    diamond(dl, ImVec2(p.x + 10.0f * sScale, p.y + px * 0.55f), 5.0f * sScale, col(kLacquerLight));
    diamond(dl, ImVec2(p.x + 10.0f * sScale, p.y + px * 0.55f), 3.0f * sScale, col(kGoldLight));
    text(dl, sTitle, px, ImVec2(p.x + 24.0f * sScale, p.y), col(kGold), title);
    const float tx = p.x + 24.0f * sScale + text_size(sTitle, px, title).x + 16.0f * sScale;
    const float ly = p.y + px * 0.58f;
    dl->AddRectFilledMultiColor(ImVec2(tx, ly), ImVec2(p.x + width, ly + 1.5f * sScale), col(kGold, 0.8f), col(kGold, 0.0f),
                                col(kGold, 0.0f), col(kGold, 0.8f));
    ImGui::Dummy(ImVec2(width, px + 8.0f * sScale));
}
// A remark under the rows: smaller, dimmer, wrapped.
void remark(const char* t, ImU32 color = kCream, float mul = 0.62f) {
    const float px = sBodyPx * 0.8f;
    const float width = ImGui::GetContentRegionAvail().x - pad_x() * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* f = font_or_default(sBody);
    const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, width, t);
    ImGui::GetWindowDrawList()->AddText(f, px, ImVec2(p.x + pad_x(), p.y + 6.0f * sScale), col(color, mul), t, nullptr, width);
    ImGui::Dummy(ImVec2(width, sz.y + 12.0f * sScale));
}

// A dialog in the menu's colours (its own text size: popups don't inherit it).
bool begin_modal(const char* name) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return false;
    ImGui::SetWindowFontScale(sBodyPx / (sBody ? sBody->FontSize : 13.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImRect r(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x,
                                                  ImGui::GetWindowPos().y + ImGui::GetWindowSize().y));
    seigaiha(dl, r, 46.0f * sScale, kNight, col(kWave, 0.5f));
    gold_frame(dl, r, sScale);
    const float px = 34.0f * sScale;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    text(dl, sTitle, px, p, col(kGold), name);
    ImGui::Dummy(ImVec2(std::max(520.0f * sScale, text_size(sTitle, px, name).x), px + 14.0f * sScale));
    return true;
}
void modal_text(const char* t) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    text(dl, sBody, sBodyPx, p, col(kCream), t);
    ImGui::Dummy(ImVec2(text_size(sBody, sBodyPx, t).x, sBodyPx + 18.0f * sScale));
}

// ── Tabs ─────────────────────────────────────────────────────────────────────
void tab_video() {
    const hal::gs::Video now = hal::gs::video();
    section("Tela");
    option("Modo de tela", &sDisp.mode, {"Janela", "Tela cheia sem borda", "Tela cheia exclusiva"},
           "Sem borda troca de janela rápido (Alt+Tab); exclusiva usa a resolução e a taxa escolhidas.");
    const int displays = std::max(1, SDL_GetNumVideoDisplays());
    std::vector<std::string> names;
    for (int i = 0; i < displays; i++) {
        const char* n = SDL_GetDisplayName(i);
        SDL_DisplayMode desk = {};
        SDL_GetDesktopDisplayMode(i, &desk);
        names.push_back(std::to_string(i + 1) + ": " + (n ? n : "Monitor") + " (" + size_str(desk.w, desk.h) + ")");
    }
    sDisp.display = std::clamp(sDisp.display, 0, displays - 1);
    option("Monitor", &sDisp.display, names, "Em qual monitor o jogo aparece.");

    if (sDisp.mode == 2) {
        // The display's own modes: the sizes, then the rates of the size chosen.
        std::vector<std::pair<int, int>> sizes;
        std::vector<int> rates;
        for (int i = 0; i < SDL_GetNumDisplayModes(sDisp.display); i++) {
            SDL_DisplayMode m = {};
            if (SDL_GetDisplayMode(sDisp.display, i, &m) != 0) continue;
            if (std::find(sizes.begin(), sizes.end(), std::make_pair(m.w, m.h)) == sizes.end())
                sizes.push_back({m.w, m.h});
            if (m.w == sDisp.exW && m.h == sDisp.exH && std::find(rates.begin(), rates.end(), m.refresh_rate) == rates.end())
                rates.push_back(m.refresh_rate);
        }
        std::vector<std::string> sizeNames;
        int pick = 0;
        for (size_t i = 0; i < sizes.size(); i++) {
            sizeNames.push_back(size_str(sizes[i].first, sizes[i].second));
            if (sizes[i].first == sDisp.exW && sizes[i].second == sDisp.exH) pick = (int)i;
        }
        if (option("Resolução", &pick, sizeNames, "A resolução do monitor em tela cheia exclusiva.") && pick < (int)sizes.size()) {
            sDisp.exW = sizes[pick].first;
            sDisp.exH = sizes[pick].second;
        }
        std::sort(rates.rbegin(), rates.rend());
        std::vector<std::string> rateNames;
        int rpick = 0;
        for (size_t i = 0; i < rates.size(); i++) {
            rateNames.push_back(std::to_string(rates[i]) + " Hz");
            if (rates[i] == sDisp.exHz) rpick = (int)i;
        }
        if (option("Taxa de atualização", &rpick, rateNames) && rpick < (int)rates.size())
            sDisp.exHz = rates[rpick];
    } else if (sDisp.mode == 0) {
        SDL_DisplayMode desk = {};
        SDL_GetDesktopDisplayMode(sDisp.display, &desk);
        static const int kSizes[][2] = {{1280, 720}, {1280, 960}, {1366, 768}, {1600, 900}, {1600, 1200},
                                        {1920, 1080}, {1920, 1440}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
        std::vector<std::pair<int, int>> sizes;
        for (const auto& s : kSizes)
            if (s[0] <= desk.w && s[1] <= desk.h) sizes.push_back({s[0], s[1]});
        if (std::find(sizes.begin(), sizes.end(), std::make_pair(sDisp.winW, sDisp.winH)) == sizes.end())
            sizes.insert(sizes.begin(), {sDisp.winW, sDisp.winH});
        std::vector<std::string> sizeNames;
        int pick = 0;
        for (size_t i = 0; i < sizes.size(); i++) {
            sizeNames.push_back(size_str(sizes[i].first, sizes[i].second));
            if (sizes[i].first == sDisp.winW && sizes[i].second == sDisp.winH) pick = (int)i;
        }
        if (option("Tamanho da janela", &pick, sizeNames)) {
            sDisp.winW = sizes[pick].first;
            sDisp.winH = sizes[pick].second;
        }
    } else {
        remark("Tela cheia sem borda usa a resolução da área de trabalho do monitor.");
    }
    const Display current = display_from(now);
    const bool differs = sDisp.mode != current.mode || sDisp.display != current.display ||
                         (sDisp.mode == 0 && (sDisp.winW != current.winW || sDisp.winH != current.winH)) ||
                         (sDisp.mode == 2 && (sDisp.exW != current.exW || sDisp.exH != current.exH || sDisp.exHz != current.exHz));
    ImGui::BeginDisabled(!differs);
    if (action("Aplicar modo de tela", "Aplicar", "Aplica a tela escolhida acima; se algo der errado, ela volta sozinha em 15 s.")) {
        sRevertTo = now;
        hal::gs::set_video(with_display(now, sDisp));
        sRevertAsk = true;
        sRevertUntil = SDL_GetTicks() + 15000;
        ImGui::OpenPopup("Manter estas configurações?");
    }
    ImGui::EndDisabled();
    remark("F11 alterna entre a janela e o modo de tela cheia escolhido.");

    if (begin_modal("Manter estas configurações?")) {
        const int left = (int)((sRevertUntil - std::min(sRevertUntil, SDL_GetTicks())) / 1000);
        char line[96];
        snprintf(line, sizeof line, "A tela volta ao modo anterior em %d s.", left);
        modal_text(line);
        if (button("Manter", 200.0f * sScale) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            sRevertAsk = false;
            sDisp = display_from(hal::gs::video());
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0.0f, 18.0f * sScale);
        if (button("Voltar", 200.0f * sScale) || left <= 0) {
            hal::gs::set_video(sRevertTo);
            sRevertAsk = false;
            sDisp = display_from(sRevertTo);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    section("Quadros");
    bool vsync = now.vsync;
    if (toggle("VSync (sincronia vertical)", &vsync, "Evita cortes na imagem; pode adicionar um pouco de atraso.")) {
        hal::gs::set_rate(now.fps, vsync);
        sGraphicsChanged = true;
    }
    const std::vector<int> fpsValue = {0, 60, 120, 144, 165, 240, -1};
    int fps = 6;
    for (int i = 0; i < (int)fpsValue.size(); i++)
        if (fpsValue[i] == now.fps) fps = i;
    if (now.fps > 0 && std::find(fpsValue.begin(), fpsValue.end(), now.fps) == fpsValue.end())
        fps = nearest((float)now.fps, {0, 60, 120, 144, 165, 240, 1e6f});
    if (option("Limite de FPS", &fps, {"30 (original)", "60", "120", "144", "165", "240", "Igual ao monitor"},
               "Acima de 30, os quadros intermediários são interpolados; o jogo em si continua a 30.")) {
        hal::gs::set_rate(fpsValue[fps], hal::gs::video().vsync);
        sGraphicsChanged = true;
    }
}

void tab_graphics() {
    hal::gs::Video v = hal::gs::video();
    section("Imagem");
    const std::vector<int> lines = {448, 720, 1080, 1440, 2160, 0};
    int line = v.height <= 0 ? 5 : nearest((float)v.height, {448, 720, 1080, 1440, 2160, -1e6f});
    if (option("Resolução interna", &line, {"448p (PS2 original)", "720p", "1080p", "1440p", "2160p (4K)", "Igual à janela"},
               "Quantas linhas o jogo desenha antes de ir para a tela. Mais = mais nítido e mais pesado.")) {
        v = hal::gs::video();
        v.height = lines[line];
        v.windowWidth = v.windowHeight = 0;
        hal::gs::set_video(v);
        sGraphicsChanged = true;
    }
    const std::vector<float> aspects = {0.0f, -1.0f, 16.0f / 9.0f, 21.0f / 9.0f};
    int aspect = v.aspect < 0.0f ? 1 : nearest(v.aspect, {0.0f, -9.0f, 16.0f / 9.0f, 21.0f / 9.0f});
    if (option("Proporção", &aspect, {"4:3 (original)", "Igual à janela", "16:9", "21:9"}, "Widescreen de verdade: mostra mais do cenário dos lados.")) {
        v = hal::gs::video();
        v.aspect = aspects[aspect];
        v.hudEdges = aspect != 0;
        v.windowWidth = v.windowHeight = 0;
        hal::gs::set_video(v);
        sGraphicsChanged = true;
    }
    bool hud = v.hudEdges;
    ImGui::BeginDisabled(v.aspect == 0.0f);
    if (toggle("HUD nas bordas da tela", &hud, "Em widescreen, leva o HUD para os cantos da tela em vez de deixá-lo no 4:3 do meio.")) {
        v = hal::gs::video();
        v.hudEdges = hud;
        v.windowWidth = v.windowHeight = 0;
        hal::gs::set_video(v);
        sGraphicsChanged = true;
    }
    ImGui::EndDisabled();
    float bright = v.brightness, gamma = v.gamma;
    const bool b1 = slider("Brilho", &bright, 0.5f, 1.5f, 0.05f, "%.2f");
    const bool b2 = slider("Gama", &gamma, 0.6f, 1.6f, 0.05f, "%.2f", "Clareia ou escurece os tons médios sem estourar os claros.");
    if (b1 || b2) {
        v = hal::gs::video();
        v.brightness = bright;
        v.gamma = gamma;
        v.windowWidth = v.windowHeight = 0;
        hal::gs::set_video(v);
    }

    section("Iluminação moderna");
    bool modern = !hal::gs::lighting_held();
    if (toggle("Ativada", &modern, "Sombras, luz das lanternas, bloom e cor modernos. F3 também liga e desliga, para comparar.")) {
        hal::gs::toggle_lighting();
        sGraphicsChanged = true;
    }
    static int preset = 0;
    if (option("Predefinição", &preset, {"Personalizada", "Baixa", "Média", "Alta", "Ultra"},
               "Escolha uma predefinição para ajustar tudo de uma vez.") && preset > 0)
        lighting_preset(preset);
    hal::gs::Lighting l = hal::gs::lighting();
    bool changed = false;
    ImGui::BeginDisabled(!modern);
    changed |= toggle("Sombras do sol", &l.shadows);
    int quality = l.shadowSize <= 1024 ? 0 : l.shadowSize <= 2048 ? 1 : 2;
    if (option("Qualidade das sombras", &quality, {"Baixa", "Alta", "Ultra"})) {
        l.shadowSize = quality == 0 ? 1024 : quality == 1 ? 2048 : 4096;
        changed = true;
    }
    changed |= toggle("Sombras do cenário", &l.mapShadows);
    changed |= toggle("Oclusão de ambiente", &l.ao, "Escurece cantos e frestas onde a luz chega menos.");
    changed |= toggle("Lanternas iluminam a rua", &l.lanterns);
    changed |= toggle("Lanternas projetam sombras", &l.lanternShadows);
    changed |= slider("Personagens na sombra", &l.groundLight, 0.0f, 1.0f, 0.05f, "%.2f");
    changed |= slider("Bloom", &l.bloom, 0.0f, 1.0f, 0.05f, "%.2f", "O brilho em volta das luzes fortes.");
    changed |= slider("Raios de sol", &l.godRays, 0.0f, 1.0f, 0.05f, "%.2f");
    changed |= slider("Relevo (bump)", &l.bump, 0.0f, 2.0f, 0.1f, "%.1f");
    changed |= option("Clima", &l.weather, {"Automático", "Sempre limpo", "Sempre chuva"});
    changed |= slider("Poeira e vento", &l.dust, 0.0f, 2.0f, 0.1f, "%.1f");
    changed |= toggle("Clima por horário (moods)", &l.moods);
    changed |= toggle("Correção de cor", &l.grade);
    ImGui::EndDisabled();
    if (changed) {
        hal::gs::set_lighting(l);
        sGraphicsChanged = true;
        preset = 0;
    }

    section("Texturas");
    bool pack = texture_pack::enabled();
    ImGui::BeginDisabled(!texture_pack::active());
    if (toggle("Texturas HD", &pack, "Usa os pacotes de texturas HD instalados nas pastas textures/ dos mods.")) {
        texture_pack::set_enabled(pack);
        sGraphicsChanged = true;
    }
    ImGui::EndDisabled();
    if (!texture_pack::active())
        remark("Nenhum pacote de texturas HD instalado (pasta textures/ de um mod).");
}

void tab_audio() {
    section("Volume");
    bool changed = false;
    changed |= slider_pct("Geral", &sAudio.master);
    changed |= slider_pct("Música", &sAudio.music);
    changed |= slider_pct("Efeitos", &sAudio.effects);
    changed |= slider_pct("Vozes", &sAudio.voices);
    changed |= slider_pct("Vídeos", &sAudio.movies, "As cenas em vídeo (abertura, finais).");
    changed |= toggle("Silenciar em segundo plano", &sAudio.muteUnfocused, "Sem som enquanto você usa outra janela.");
    if (changed)
        apply_audio();
    if (action("Restaurar volumes", "Restaurar")) {
        sAudio = Audio{};
        apply_audio();
    }
    remark("O som fica pausado enquanto este menu está aberto. Os volumes do próprio jogo (Settings) continuam valendo por cima destes.");
}

// Keyboard, controller and mouse for one action: three cells side by side.
const char* action_name(int a) {
    if (a == hal::pad::kWalkFull)
        return sPad.walkDefaultFull ? "Andar devagar (segurar)" : "Andar inteiro (segurar)";
    return kActionName[a];
}
void bind_row(int a) {
    const float h = row_h();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImRect r(p, ImVec2(p.x + width, p.y + h));
    const float labelW = width * 0.38f;
    const float gap = 10.0f * sScale;
    const float cellW = (width - labelW - pad_x() - gap * 2.0f) / 3.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // The cells first (as items), the row's glow under them after.
    struct Cell { std::string text; bool capturing, enabled; int kind; };
    const bool hasButton = a < hal::pad::kButtonActions;
    Cell cells[3] = {
        {sCaptureKey == a ? "pressione..." : sPad.keys[a] > 0 ? SDL_GetScancodeName((SDL_Scancode)sPad.keys[a]) : "-", sCaptureKey == a, true, 0},
        {sCapturePad == a ? "pressione..." : hasButton ? pad_input_name(sPad.pad[a]) : a < hal::pad::kRStickUp ? "Analógico esq." : a < hal::pad::kWalkFull ? "Analógico dir." : "-",
         sCapturePad == a, hasButton, 1},
        {sCaptureMouse == a ? "clique..." : hasButton ? mouse_input_name(sPad.mouse[a]) : a >= hal::pad::kRStickUp && a < hal::pad::kWalkFull ? "Mover o mouse" : "-",
         sCaptureMouse == a, hasButton, 2},
    };
    bool rowLit = false;
    ImRect cellRect[3];
    bool cellLit[3] = {}, cellHeld[3] = {};
    for (int c = 0; c < 3; c++) {
        const float x = r.Min.x + labelW + c * (cellW + gap);
        cellRect[c] = ImRect(ImVec2(x, r.Min.y + 7.0f * sScale), ImVec2(x + cellW, r.Max.y - 7.0f * sScale));
        if (!cells[c].enabled) continue;
        ImGui::SetCursorScreenPos(cellRect[c].Min);
        ImGui::PushID(a * 4 + c);
        bool pressed = ImGui::InvisibleButton("cell", cellRect[c].GetSize(), ImGuiButtonFlags_EnableNav);
        focus_hook();
        if (pressed && ImGui::GetFrameCount() - sCaptureEndFrame < 15) pressed = false;
        const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && nav_visible();
        cellLit[c] = hovered || focused;
        cellHeld[c] = ImGui::IsItemActive();
        rowLit |= cellLit[c];
        if (hovered || focused)
            sTipNext = c == 2 ? "Clique e depois aperte o botão do mouse (ou gire a roda). Esc cancela, Delete apaga."
                              : "Escolha e depois aperte a nova tecla ou botão. Esc cancela, Delete apaga.";
        if (pressed) {
            sCaptureKey = c == 0 ? a : -1;
            sCapturePad = c == 1 ? a : -1;
            sCaptureMouse = c == 2 ? a : -1;
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h));
    if (rowLit) {
        dl->AddRectFilledMultiColor(r.Min, r.Max, col(kLacquer, 0.55f), col(kLacquerDark, 0.1f), col(kLacquerDark, 0.1f), col(kLacquer, 0.55f));
        triangle(dl, ImVec2(r.Min.x + 9.0f * sScale, r.GetCenter().y), 6.0f * sScale, 1, col(kGoldLight));
    } else {
        dl->AddLine(ImVec2(r.Min.x + pad_x(), r.Max.y - 1.0f), ImVec2(r.Max.x - pad_x(), r.Max.y - 1.0f), col(kGold, 0.12f), 1.0f);
    }
    const std::string name = fit(sBody, sBodyPx, action_name(a), labelW - pad_x() - 16.0f * sScale);
    text(dl, sBody, sBodyPx, ImVec2(r.Min.x + pad_x() + 10.0f * sScale, r.GetCenter().y - sBodyPx * 0.5f), col(rowLit ? kGoldLight : kCream), name.c_str());
    const float px = sBodyPx * 0.86f;
    for (int c = 0; c < 3; c++) {
        const ImRect& cr = cellRect[c];
        const Cell& cell = cells[c];
        if (cell.enabled) {
            const float pulse = cell.capturing ? 0.6f + 0.4f * sinf((float)ImGui::GetTime() * 8.0f) : 1.0f;
            dl->AddRectFilled(cr.Min, cr.Max, col(cell.capturing ? kLacquer : cellHeld[c] ? kLacquerDark : kNightDeep, cell.capturing ? pulse : 0.85f), 4.0f * sScale);
            dl->AddRect(cr.Min, cr.Max, col(cellLit[c] || cell.capturing ? kGoldLight : kGold, cellLit[c] ? 1.0f : 0.45f), 4.0f * sScale, 0,
                        (cellLit[c] ? 2.0f : 1.0f) * sScale);
        }
        const std::string t = fit(sBody, px, cell.text, cr.GetWidth() - 12.0f * sScale);
        const ImVec2 ts = text_size(sBody, px, t.c_str());
        text(dl, sBody, px, ImVec2(cr.GetCenter().x - ts.x * 0.5f, cr.GetCenter().y - px * 0.5f),
             col(cell.capturing ? kGoldLight : kCream, cell.enabled ? 1.0f : 0.4f), t.c_str());
    }
}
void apply_layout(const hal::pad::Config& from) {
    for (int i = 0; i < hal::pad::kActions; i++) sPad.keys[i] = from.keys[i];
    for (int i = 0; i < hal::pad::kButtonActions; i++) sPad.mouse[i] = from.mouse[i];
    sPad.walkDefaultFull = from.walkDefaultFull;
    hal::pad::set_config(sPad);
}

void tab_controls() {
    section("Esquema");
    const std::string id = layout_id(sPad);
    int layout = id == "moderno" ? 0 : id == "classico" ? 1 : 2;
    const int was = layout;
    if (option("Teclado e mouse", &layout, {"Moderno (WASD + mouse)", "Clássico (setas, Z X A S)", "Personalizado"},
               "Moderno: WASD anda, mouse gira a câmera, clique ataca. Clássico: o layout antigo do port.")) {
        if (layout == 2) layout = was == 0 ? 1 : 0;
        apply_layout(layout == 0 ? hal::pad::default_config() : hal::pad::classic_config());
    }

    section("Mouse");
    bool changed = false;
    changed |= slider("Sensibilidade do mouse", &sPad.mouseSensitivity, 0.1f, 4.0f, 0.1f, "%.1f", "Quanto a câmera gira para cada movimento do mouse.");
    changed |= toggle("Inverter mouse vertical", &sPad.mouseInvertY);
    changed |= toggle("Capturar o mouse ao jogar", &sPad.mouseCapture,
                      "O cursor some enquanto você joga e volta no menu, ao trocar de janela ou com Alt+Tab. Clique na janela para capturar de novo.");

    section("Câmera e analógicos");
    changed |= slider("Sensibilidade da câmera (analógico)", &sPad.cameraSensitivity, 0.25f, 2.0f, 0.05f, "%.2f");
    changed |= toggle("Inverter câmera horizontal", &sPad.invertX);
    changed |= toggle("Inverter câmera vertical", &sPad.invertY);
    changed |= slider("Zona morta dos analógicos", &sPad.deadzone, 0.05f, 0.5f, 0.01f, "%.2f", "Quanto o analógico precisa sair do centro para contar.");
    changed |= toggle("Vibração", &sPad.rumble);
    if (changed)
        hal::pad::set_config(sPad);

    section("Botões");
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float labelW = width * 0.38f, gap = 10.0f * sScale;
        const float cellW = (width - labelW - pad_x() - gap * 2.0f) / 3.0f;
        const float px = sBodyPx * 0.78f;
        static const char* const kHead[] = {"TECLADO", "CONTROLE", "MOUSE"};
        for (int c = 0; c < 3; c++) {
            const float x = p.x + labelW + c * (cellW + gap) + cellW * 0.5f - text_size(sBody, px, kHead[c]).x * 0.5f;
            text(ImGui::GetWindowDrawList(), sBody, px, ImVec2(x, p.y), col(kGold, 0.85f), kHead[c]);
        }
        ImGui::Dummy(ImVec2(width, px + 6.0f * sScale));
    }
    for (int a = 0; a < hal::pad::kActions; a++)
        bind_row(a);
    ImGui::Dummy(ImVec2(1.0f, 8.0f * sScale));
    if (action("Restaurar controles padrão", "Restaurar", "Volta ao layout moderno e às opções de câmera originais.")) {
        sPad = hal::pad::default_config();
        hal::pad::set_config(sPad);
    }
}

void tab_game() {
    section("Idioma");
    if (!sHasPtbr) {
        remark("A tradução PT-BR não está instalada: abra o WotS2-Instalador.exe e ligue a opção da tradução.");
    } else {
        int lang = sPtbrWanted ? 0 : 1;
        if (option("Idioma dos textos", &lang, {"Português (Brasil)", "English (original)"}, "Vale depois de reiniciar o jogo.")) {
            sPtbrWanted = lang == 0;
            ini::write(kLanguageIni, {{"enabled", sPtbrWanted ? "1" : "0"}}, "mod");
        }
        if (sPtbrWanted != sPtbrLoaded && action("O idioma muda ao reiniciar", "Reiniciar agora"))
            restart();
    }

    section("Salvar em qualquer lugar");
    const bool inGame = Native_GameInProgress() != 0;
    const bool scene = Native_EventRunning() != 0;
    static const char* const kPeriods[] = {"Amanhecer", "Manhã", "Tarde", "Anoitecer", "Madrugada"};
    if (!sHaveCard)
        remark("Ainda não há arquivo de save no cartão (crie-o ao iniciar o jogo).");
    for (int i = 0; i < 3; i++) {
        char label[128];
        if (sSlotUsed[i])
            snprintf(label, sizeof label, "No.%d  ·  Dia %d, %s%s", i + 1, sSlotDay[i],
                     kPeriods[std::clamp(sSlotPeriod[i], 0, 4)], i == sCurrentSlot ? "  (atual)" : "");
        else
            snprintf(label, sizeof label, "No.%d  ·  vazio%s", i + 1, i == sCurrentSlot ? "  (atual)" : "");
        ImGui::BeginDisabled(!inGame || scene || !sHaveCard);
        ImGui::PushID(i);
        if (action(label, "Salvar aqui", "Grava o progresso neste espaço do cartão. Ao carregar, o jogo continua no mapa da cidade.")) {
            const int r = Native_QuickSave(i);
            static const char* const kWhy[] = {"", "sem arquivo de save", "cartão em uso", "falha ao ler",
                                               "falha ao gravar", "nenhum jogo em andamento"};
            if (r == 0) note("Progresso salvo no No." + std::to_string(i + 1) + ".");
            else note(std::string("Não foi possível salvar: ") + kWhy[std::clamp(-r, 0, 5)] + ".");
            refresh_slots();
        }
        ImGui::PopID();
        ImGui::EndDisabled();
    }
    if (!inGame)
        remark("Comece ou carregue um jogo para poder salvar.");
    else if (scene)
        remark("Não dá para salvar durante uma cena ou um carregamento.");
    if (!sNote.empty() && SDL_GetTicks() < sNoteUntil)
        remark(sNote.c_str(), kGoldLight, 1.0f);

    section("Carregar e sair");
    ImGui::BeginDisabled(!inGame || scene);
    if (action("Voltar ao título", "Voltar", "Para carregar outro save. O progresso não salvo será perdido."))
        ImGui::OpenPopup("Voltar ao título?");
    ImGui::EndDisabled();
    if (action("Sair do jogo", "Sair"))
        ImGui::OpenPopup("Sair do jogo?");
    if (begin_modal("Voltar ao título?")) {
        modal_text("O progresso não salvo será perdido.");
        if (button("Voltar ao título", 240.0f * sScale)) {
            sTitleRequest = true;
            ImGui::CloseCurrentPopup();
            settings_menu::close();
        }
        ImGui::SameLine(0.0f, 18.0f * sScale);
        if (button("Cancelar", 200.0f * sScale))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (begin_modal("Sair do jogo?")) {
        modal_text("O progresso não salvo será perdido.");
        if (button("Sair", 200.0f * sScale)) {
            settings_menu::close();
            printf("clean exit\n");
            exit(0);
        }
        ImGui::SameLine(0.0f, 18.0f * sScale);
        if (button("Cancelar", 200.0f * sScale))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ── The frame around it all ──────────────────────────────────────────────────
void switch_tab(int to) {
    to = (to + kTabs) % kTabs;
    if (to == sTab) return;
    sTab = to;
    sTabSince = (float)ImGui::GetTime();
    sFocusContent = true;
    sFocusFirst = true;
}
// A button prompt: a round face (controller) or a key cap (keyboard).
float prompt_width(const char* cap, const char* what, bool round) {
    const float px = sBodyPx * 0.8f;
    const float capW = round ? px * 1.35f : std::max(px * 1.4f, text_size(sBody, px * 0.85f, cap).x + 16.0f * sScale);
    return capW + 8.0f * sScale + text_size(sBody, px, what).x + 26.0f * sScale;
}
float prompt(ImDrawList* dl, float x, float cy, const char* cap, const char* what, bool round, ImU32 face = 0) {
    const float px = sBodyPx * 0.8f;
    const float cpx = px * 0.85f;
    const ImVec2 cs = text_size(sBody, cpx, cap);
    float capW;
    if (round) {
        capW = px * 1.35f;
        const ImVec2 c(x + capW * 0.5f, cy);
        dl->AddCircleFilled(c, capW * 0.5f, col(face != 0 ? face : kNightDeep));
        dl->AddCircle(c, capW * 0.5f, col(kGold), 0, 1.5f * sScale);
        text(dl, sBody, cpx, ImVec2(c.x - cs.x * 0.5f, c.y - cpx * 0.55f), col(kCream), cap);
    } else {
        capW = std::max(px * 1.4f, cs.x + 16.0f * sScale);
        const ImRect r(ImVec2(x, cy - px * 0.7f), ImVec2(x + capW, cy + px * 0.7f));
        dl->AddRectFilled(r.Min, r.Max, col(kNightDeep), 4.0f * sScale);
        dl->AddRect(r.Min, r.Max, col(kGold), 4.0f * sScale, 0, 1.5f * sScale);
        text(dl, sBody, cpx, ImVec2(r.GetCenter().x - cs.x * 0.5f, cy - cpx * 0.55f), col(kCream), cap);
    }
    text(dl, sBody, px, ImVec2(x + capW + 8.0f * sScale, cy - px * 0.55f), col(kCream, 0.85f), what);
    return capW + 8.0f * sScale + text_size(sBody, px, what).x + 26.0f * sScale;
}
void draw_footer(ImDrawList* dl, ImRect r) {
    dl->AddRectFilled(r.Min, r.Max, col(kNightDeep, 0.92f));
    dl->AddLine(r.Min, ImVec2(r.Max.x, r.Min.y), col(kGold, 0.6f), 1.5f * sScale);
    const float cy = r.GetCenter().y;
    struct P { const char* cap; const char* what; bool round; ImU32 face; };
    std::vector<P> ps;
    if (sPadInput)
        ps = {{"A", "Selecionar", true, IM_COL32(48, 110, 52, 255)}, {"B", "Fechar", true, IM_COL32(150, 36, 32, 255)},
              {"LB / RB", "Aba", false, 0}, {"+", "Mover / alterar", true, 0}};
    else
        ps = {{"Enter", "Selecionar", false, 0}, {"Esc", "Fechar", false, 0}, {"Q / E", "Aba", false, 0},
              {"Setas", "Mover / alterar", false, 0}};
    float total = 0.0f;
    for (const P& p : ps) total += prompt_width(p.cap, p.what, p.round);
    float x = r.Max.x - 22.0f * sScale - total;
    const float tipRight = x - 30.0f * sScale;
    for (const P& p : ps) x += prompt(dl, x, cy, p.cap, p.what, p.round, p.face);
    // The description of the row under the cursor.
    if (!sTip.empty()) {
        const float px = sBodyPx * 0.82f;
        const float width = tipRight - (r.Min.x + 24.0f * sScale);
        ImFont* f = font_or_default(sBody);
        const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, width, sTip.c_str());
        diamond(dl, ImVec2(r.Min.x + 14.0f * sScale, cy), 4.0f * sScale, col(kGold));
        dl->AddText(f, px, ImVec2(r.Min.x + 26.0f * sScale, cy - sz.y * 0.5f), col(kCream, 0.8f), sTip.c_str(), nullptr, width);
    }
}

}  // namespace

namespace settings_menu {

void init(int argc, char* argv[]) {
    for (int i = 0; i < argc; i++)
        sArgs.push_back(argv[i]);
    sPtbrLoaded = sPtbrWanted = read_ptbr_enabled();
    load_settings();
}

bool is_open() { return sOpen; }

void open() {
    if (sOpen || editor::tool_open())
        return;
    sOpen = true;
    sCaptureKey = sCapturePad = sCaptureMouse = -1;
    sDisp = display_from(hal::gs::video());
    sPad = hal::pad::config();
    refresh_slots();
    editor::show_overlay();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    hal::pad::set_blocked(true);
    hal::spu2::set_paused(true);
    SDL_ShowCursor(SDL_ENABLE);
    sOpenedAt = (float)ImGui::GetTime();
    sTabSince = sOpenedAt - 1.0f;
    sFocusContent = true;
    sFocusFirst = true;
    sTip.clear();
}

void close() {
    if (!sOpen)
        return;
    sOpen = false;
    sCaptureKey = sCapturePad = sCaptureMouse = -1;
    if (sRevertAsk) {                   // (closed while asking: the old screen back)
        hal::gs::set_video(sRevertTo);
        sRevertAsk = false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    if (!editor::tool_open())
        hal::pad::set_blocked(false);
    hal::spu2::set_paused(false);
    hal::pad::set_config(sPad);
    save_settings();
}

bool event(const SDL_Event& ev) {
    if (ev.type == SDL_WINDOWEVENT) {
        if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST || ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            sFocused = ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
            apply_audio();
        }
        if (sOpen)
            ImGui_ImplSDL2_ProcessEvent(&ev);
        return false;
    }
    if (!sOpen) {
        if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0 &&
            (ev.key.keysym.sym == SDLK_F1 || ev.key.keysym.sym == SDLK_ESCAPE)) {
            open();
            sPadInput = false;
            return sOpen;
        }
        return false;
    }
    // Which kind of input the prompts at the bottom show.
    if (ev.type == SDL_CONTROLLERBUTTONDOWN || (ev.type == SDL_CONTROLLERAXISMOTION && abs(ev.caxis.value) > 16000))
        sPadInput = true;
    else if (ev.type == SDL_KEYDOWN || ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEWHEEL)
        sPadInput = false;
    auto endCapture = [] {
        sCaptureKey = sCapturePad = sCaptureMouse = -1;
        sCaptureEndFrame = ImGui::GetFrameCount();
        hal::pad::set_config(sPad);
    };
    const bool clearKey = ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_DELETE || ev.key.keysym.sym == SDLK_BACKSPACE);
    // Waiting for a key, a button or a mouse button to bind.
    if (sCaptureKey >= 0 && ev.type == SDL_KEYDOWN) {
        if (ev.key.repeat != 0) return true;
        if (clearKey) sPad.keys[sCaptureKey] = SDL_SCANCODE_UNKNOWN;
        else if (ev.key.keysym.sym != SDLK_ESCAPE) sPad.keys[sCaptureKey] = ev.key.keysym.scancode;
        endCapture();
        return true;
    }
    if (sCapturePad >= 0 || sCaptureMouse >= 0) {
        if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_ESCAPE || clearKey)) {
            if (clearKey && sCapturePad >= 0) sPad.pad[sCapturePad] = -1;
            if (clearKey && sCaptureMouse >= 0) sPad.mouse[sCaptureMouse] = -1;
            endCapture();
            return true;
        }
    }
    if (sCapturePad >= 0) {
        int got = -2;
        if (ev.type == SDL_CONTROLLERBUTTONDOWN)
            got = ev.cbutton.button;
        else if (ev.type == SDL_CONTROLLERAXISMOTION && ev.caxis.value > 16000) {
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) got = hal::pad::kPadLeftTrigger;
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) got = hal::pad::kPadRightTrigger;
        }
        if (got != -2) {
            sPad.pad[sCapturePad] = got;
            endCapture();
            return true;
        }
    }
    if (sCaptureMouse >= 0) {
        int got = -2;
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button >= 1 && ev.button.button <= 5)
            got = ev.button.button;
        else if (ev.type == SDL_MOUSEWHEEL && ev.wheel.y != 0)
            got = ev.wheel.y > 0 ? hal::pad::kMouseWheelUp : hal::pad::kMouseWheelDown;
        if (got != -2) {
            sPad.mouse[sCaptureMouse] = got;
            endCapture();
            return true;
        }
        if (ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEBUTTONDOWN)
            return true;
    }
    // The keypad's arrows and Enter move and choose like the arrow keys (the
    // menu has nothing to type numbers into).
    SDL_Event e = ev;
    if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
        auto to = [&](SDL_Keycode k, SDL_Scancode sc) { e.key.keysym.sym = k; e.key.keysym.scancode = sc; };
        switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_KP_8: to(SDLK_UP, SDL_SCANCODE_UP); break;
        case SDL_SCANCODE_KP_2: to(SDLK_DOWN, SDL_SCANCODE_DOWN); break;
        case SDL_SCANCODE_KP_4: to(SDLK_LEFT, SDL_SCANCODE_LEFT); break;
        case SDL_SCANCODE_KP_6: to(SDLK_RIGHT, SDL_SCANCODE_RIGHT); break;
        case SDL_SCANCODE_KP_ENTER: to(SDLK_RETURN, SDL_SCANCODE_RETURN); break;
        default: break;
        }
    }
    ImGui_ImplSDL2_ProcessEvent(&e);
    switch (ev.type) {
    case SDL_QUIT:
        return false;
    case SDL_KEYDOWN:
        if (ev.key.keysym.sym == SDLK_F11)
            return false;
        if (ev.key.repeat == 0 && (ev.key.keysym.sym == SDLK_F1 ||
                                   (ev.key.keysym.sym == SDLK_ESCAPE && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)))) {
            close();
            return true;
        }
        return true;
    default:
        return true;
    }
}

void draw() {
    if (!sOpen)
        return;
    ImGuiIO& io = ImGui::GetIO();
    const float now = (float)ImGui::GetTime();
    const float s = sScale = std::clamp(io.DisplaySize.y / 1080.0f, 0.6f, 2.2f);
    sBody = editor::menu_font(0);
    sTitle = editor::menu_font(1);
    sKanji = editor::menu_font(2);
    sBodyPx = 26.0f * s;
    const bool capturing = sCaptureKey >= 0 || sCapturePad >= 0 || sCaptureMouse >= 0;
    // The controller doesn't move the cursor while a button is being bound.
    if (capturing) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    const float open = ease((now - sOpenedAt) / 0.22f);
    sAlpha = open;
    sTip = sTipNext;
    sTipNext.clear();

    // The game behind, darkened.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, (int)(150 * open)));

    const ImVec2 size(std::min(io.DisplaySize.x * 0.94f, 1560.0f * s), std::min(io.DisplaySize.y * 0.92f, 980.0f * s));
    const ImVec2 pos((io.DisplaySize.x - size.x) * 0.5f, (io.DisplaySize.y - size.y) * 0.5f + (1.0f - open) * 24.0f * s);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    if (now - sOpenedAt < 0.05f)
        ImGui::SetNextWindowFocus();

    int colors = 0, vars = 0;
    auto color = [&](ImGuiCol c, ImU32 v) { ImGui::PushStyleColor(c, v); colors++; };
    color(ImGuiCol_Text, kCream);
    color(ImGuiCol_PopupBg, kNight);
    color(ImGuiCol_WindowBg, kNight);
    color(ImGuiCol_Border, kGold);
    color(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 120));
    color(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));            // (the rows draw their own cursor)
    color(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    color(ImGuiCol_ScrollbarGrab, col(kGold, 0.45f));
    color(ImGuiCol_ScrollbarGrabHovered, col(kGold, 0.75f));
    color(ImGuiCol_ScrollbarGrabActive, kGoldLight);
    auto var = [&](ImGuiStyleVar v, float x) { ImGui::PushStyleVar(v, x); vars++; };
    auto var2 = [&](ImGuiStyleVar v, ImVec2 x) { ImGui::PushStyleVar(v, x); vars++; };
    var(ImGuiStyleVar_Alpha, open);
    var(ImGuiStyleVar_WindowRounding, 4.0f * s);
    var(ImGuiStyleVar_WindowBorderSize, 0.0f);
    var(ImGuiStyleVar_PopupBorderSize, 0.0f);
    var2(ImGuiStyleVar_WindowPadding, ImVec2(30.0f * s, 26.0f * s));
    var2(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    var(ImGuiStyleVar_ScrollbarSize, 10.0f * s);
    var(ImGuiStyleVar_ScrollbarRounding, 5.0f * s);

    ImGui::Begin("##pcsettings", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    if (sBody != nullptr)
        ImGui::SetWindowFontScale(sBodyPx / sBody->FontSize);
    if (ImGui::IsWindowFocused() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
        sFocusContent = true;               // (a click on the frame: the rows keep the keyboard)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImRect P(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    const float headH = 112.0f * s, footH = 62.0f * s, colW = std::min(300.0f * s, size.x * 0.24f);
    const ImRect head(P.Min, ImVec2(P.Max.x, P.Min.y + headH));
    const ImRect foot(ImVec2(P.Min.x, P.Max.y - footH), P.Max);
    const ImRect side(ImVec2(P.Min.x, head.Max.y), ImVec2(P.Min.x + colW, foot.Min.y));
    const ImRect body(ImVec2(side.Max.x, head.Max.y), ImVec2(P.Max.x, foot.Min.y));

    // Shadow, body with waves, the side column, the header in lacquer.
    for (int i = 1; i <= 4; i++)
        dl->AddRectFilled(ImVec2(P.Min.x - i * 3.0f * s, P.Min.y - i * 2.0f * s), ImVec2(P.Max.x + i * 3.0f * s, P.Max.y + i * 4.0f * s),
                          col(kBlack, 0.12f), 8.0f * s);
    dl->AddRectFilled(P.Min, P.Max, col(kNight), 4.0f * s);
    seigaiha(dl, body, 58.0f * s, col(kNight), col(kWave, 0.55f));
    dl->AddRectFilledMultiColor(body.Min, ImVec2(body.Min.x + body.GetWidth() * 0.6f, body.Max.y), col(kNight, 0.92f),
                                col(kNight, 0.0f), col(kNight, 0.0f), col(kNight, 0.92f));
    dl->AddRectFilledMultiColor(side.Min, side.Max, col(kNightDeep, 0.96f), col(kNightDeep, 0.85f), col(kNightDeep, 0.85f), col(kNightDeep, 0.96f));
    dl->AddLine(ImVec2(side.Max.x, side.Min.y), ImVec2(side.Max.x, side.Max.y), col(kGold, 0.55f), 1.5f * s);
    dl->AddRectFilledMultiColor(head.Min, head.Max, col(kLacquerLight), col(kLacquer), col(kLacquerDark), col(kLacquerDark));
    seigaiha(dl, ImRect(ImVec2(head.Max.x - head.GetWidth() * 0.4f, head.Min.y), head.Max), 34.0f * s, col(kLacquer, 0.0f),
             col(kLacquerLight, 0.35f));
    dl->AddLine(ImVec2(head.Min.x, head.Max.y - 1.0f), ImVec2(head.Max.x, head.Max.y - 1.0f), col(kGold), 2.5f * s);
    dl->AddLine(ImVec2(head.Min.x, head.Max.y + 3.0f * s), ImVec2(head.Max.x, head.Max.y + 3.0f * s), col(kGold, 0.4f), 1.0f * s);
    // 設定 in a gold ring, then the name.
    {
        const float kpx = 58.0f * s;
        const ImVec2 kc(head.Min.x + 40.0f * s + kpx, head.GetCenter().y);
        dl->AddCircleFilled(kc, kpx * 0.98f, col(kLacquerDark, 0.85f));
        dl->AddCircle(kc, kpx * 0.98f, col(kGold), 0, 2.5f * s);
        dl->AddCircle(kc, kpx * 0.98f - 5.0f * s, col(kGold, 0.45f), 0, 1.0f * s);
        const char* k = "設定";
        const ImVec2 ks = text_size(sKanji, kpx * 0.82f, k);
        text(dl, sKanji, kpx * 0.82f, ImVec2(kc.x - ks.x * 0.5f, kc.y - ks.y * 0.5f), col(kGoldLight), k);
        const float tpx = 46.0f * s;
        const float tx = kc.x + kpx + 26.0f * s;
        text(dl, sTitle, tpx, ImVec2(tx + 2.0f * s, head.GetCenter().y - tpx * 0.62f + 2.0f * s), col(kBlack, 0.5f), "CONFIGURAÇÕES");
        text(dl, sTitle, tpx, ImVec2(tx, head.GetCenter().y - tpx * 0.62f), col(kCream), "CONFIGURAÇÕES");
        const float spx = sBodyPx * 0.78f;
        text(dl, sBody, spx, ImVec2(tx + 2.0f * s, head.GetCenter().y + tpx * 0.42f), col(kGoldLight, 0.85f),
             "Way of the Samurai 2  ·  versão para PC");
    }
    // The tabs, down the side: kanji over the name; the chosen one in lacquer.
    {
        if (!capturing && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
                switch_tab(sTab - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_E, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
                switch_tab(sTab + 1);
        }
        const float tabH = 92.0f * s, x0 = side.Min.x + 14.0f * s, x1 = side.Max.x - 14.0f * s;
        float y = side.Min.y + 22.0f * s;
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        for (int i = 0; i < kTabs; i++, y += tabH + 10.0f * s) {
            const ImRect r(ImVec2(x0, y), ImVec2(x1, y + tabH));
            ImGui::SetCursorScreenPos(r.Min);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("tab", r.GetSize()) && !capturing) {
                switch_tab(i);
                sFocusFirst = false;      // (the mouse: no keyboard cursor)
            }
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool on = i == sTab;
            if (on) {
                dl->AddRectFilledMultiColor(r.Min, r.Max, col(kLacquerLight), col(kLacquer), col(kLacquerDark), col(kLacquerDark));
                dl->AddRect(r.Min, r.Max, col(kGold), 3.0f * s, 0, 2.0f * s);
                triangle(dl, ImVec2(r.Min.x - 2.0f * s, r.GetCenter().y), 9.0f * s, 1, col(kGoldLight));
            } else if (hovered) {
                dl->AddRectFilled(r.Min, r.Max, col(kLacquer, 0.35f), 3.0f * s);
                dl->AddRect(r.Min, r.Max, col(kGold, 0.5f), 3.0f * s, 0, 1.0f * s);
            }
            const float kpx = 40.0f * s;
            const ImVec2 ks = text_size(sKanji, kpx, kTabKanji[i]);
            text(dl, sKanji, kpx, ImVec2(r.GetCenter().x - ks.x * 0.5f, r.Min.y + 8.0f * s), col(on ? kGoldLight : kGold, on ? 1.0f : 0.55f), kTabKanji[i]);
            const ImVec2 ns = text_size(sBody, sBodyPx, kTabName[i]);
            text(dl, sBody, sBodyPx, ImVec2(r.GetCenter().x - ns.x * 0.5f, r.Max.y - sBodyPx - 10.0f * s), col(kCream, on ? 1.0f : 0.6f), kTabName[i]);
        }
        ImGui::PopItemFlag();
        const char* caps = sPadInput ? "LB / RB" : "Q / E";
        const float px = sBodyPx * 0.75f;
        const std::string hintText = std::string(caps) + "  trocam de aba";
        const ImVec2 hs = text_size(sBody, px, hintText.c_str());
        text(dl, sBody, px, ImVec2(side.GetCenter().x - hs.x * 0.5f, side.Max.y - px - 16.0f * s), col(kCream, 0.5f), hintText.c_str());
    }
    draw_footer(dl, foot);
    gold_frame(dl, P, s);

    // The rows, sliding in when the tab changes.
    const float tabT = ease((now - sTabSince) / 0.25f);
    const ImVec2 cMin(body.Min.x + 24.0f * s + (1.0f - tabT) * 28.0f * s, body.Min.y + 12.0f * s);
    const ImVec2 cSize(body.GetWidth() - 44.0f * s, body.GetHeight() - 20.0f * s);
    ImGui::SetCursorScreenPos(cMin);
    if (sFocusContent) {
        ImGui::SetNextWindowFocus();
        sFocusContent = false;
    }
    if (now - sTabSince < 0.02f)
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open * tabT);
    sAlpha = open * tabT;
    ImGui::BeginChild("rows", cSize, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    switch (sTab) {
    case 0: tab_video(); break;
    case 1: tab_graphics(); break;
    case 2: tab_audio(); break;
    case 3: tab_controls(); break;
    default: tab_game(); break;
    }
    ImGui::Dummy(ImVec2(1.0f, 20.0f * s));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    sAlpha = open;
    ImGui::End();
    ImGui::PopStyleVar(vars);
    ImGui::PopStyleColor(colors);
    sFocusFirst = false;            // (only ever for the frame it was asked in, or the next)

    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!capturing && ImGui::GetFrameCount() - sCaptureEndFrame >= 15 &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) || (!popup && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))))
        close();
}

}  // namespace settings_menu

extern "C" void Native_SettingsOpen() { settings_menu::open(); }
extern "C" int Native_SettingsIsOpen() { return settings_menu::is_open() ? 1 : 0; }
extern "C" int Native_TakeTitleRequest() {
    const bool r = sTitleRequest;
    sTitleRequest = false;
    return r ? 1 : 0;
}
