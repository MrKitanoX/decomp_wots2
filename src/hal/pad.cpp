#include "hal/pad.h"
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "hal/gs.h"

// Current button state per port (PS2 convention: 0 = pressed)
static u16 s_buttons[2] = { 0xFFFF, 0xFFFF };
static u8  s_lx[2] = { 128, 128 }, s_ly[2] = { 128, 128 };
static u8  s_rx[2] = { 128, 128 }, s_ry[2] = { 128, 128 };

// Keyboard → PS2 button map (port 0)
struct KeyMap { SDL_Keycode key; hal::pad::Button btn; };
static constexpr KeyMap KEY_MAP[] = {
    { SDLK_RETURN,    hal::pad::START    },
    { SDLK_RSHIFT,    hal::pad::SELECT   },
    { SDLK_UP,        hal::pad::UP       },
    { SDLK_DOWN,      hal::pad::DOWN     },
    { SDLK_LEFT,      hal::pad::LEFT     },
    { SDLK_RIGHT,     hal::pad::RIGHT    },
    { SDLK_z,         hal::pad::CROSS    },
    { SDLK_x,         hal::pad::CIRCLE   },
    { SDLK_a,         hal::pad::SQUARE   },
    { SDLK_s,         hal::pad::TRIANGLE },
    { SDLK_q,         hal::pad::L1       },
    { SDLK_w,         hal::pad::R1       },
    { SDLK_e,         hal::pad::L2       },
    { SDLK_r,         hal::pad::R2       },
    { SDLK_c,         hal::pad::L3       },     // (L3: sprint, with the sword away)
    { SDLK_v,         hal::pad::R3       },     // (R3: the camera back behind)
};

// The mouse (PC settings): relative motion for the camera, its buttons,
// whether the cursor is taken, and which kind of input came last.
static bool s_cursorNeeded = false, s_captureArmed = true, s_lastKeyboard = true;
static float s_mouseLook[2] = {0.0f, 0.0f};      // this frame's turn and tilt, radians
static int s_wheel = 0;                          // notches since the last poll (+ up)

// PC settings (settings menu): bindings, camera and sticks.
static hal::pad::Config s_config;
static bool s_configReady = false;
static const hal::pad::Button kActionButton[hal::pad::kButtonActions] = {
    hal::pad::START, hal::pad::SELECT, hal::pad::UP, hal::pad::DOWN, hal::pad::LEFT, hal::pad::RIGHT,
    hal::pad::CROSS, hal::pad::CIRCLE, hal::pad::SQUARE, hal::pad::TRIANGLE,
    hal::pad::L1, hal::pad::R1, hal::pad::L2, hal::pad::R2, hal::pad::L3, hal::pad::R3,
};

// Debug aid: the left stick held at (x, y) for the next two polls.
static u8  s_debugStick[2] = {0x80, 0x80};
static int s_debugStickFrames = 0;
namespace hal::pad {
void debug_stick(u8 x, u8 y) {
    s_debugStick[0] = x;
    s_debugStick[1] = y;
    s_debugStickFrames = 2;
}
}  // namespace hal::pad

// Debug aid: WOTS2_INPUT="350:start,420:down,450:cross" presses each button
// for three frames starting at the given frame; "600-700:up" holds one down
// over a range of frames.
struct ScriptedPress { int frame, end; hal::pad::Button btn; };
static std::vector<ScriptedPress> s_script;

static void load_script() {
    const char* e = getenv("WOTS2_INPUT");
    if (!e)
        return;
    static const struct { const char* name; hal::pad::Button btn; } kNames[] = {
        {"start", hal::pad::START}, {"select", hal::pad::SELECT}, {"up", hal::pad::UP},
        {"down", hal::pad::DOWN}, {"left", hal::pad::LEFT}, {"right", hal::pad::RIGHT},
        {"cross", hal::pad::CROSS}, {"circle", hal::pad::CIRCLE}, {"square", hal::pad::SQUARE},
        {"triangle", hal::pad::TRIANGLE}, {"l1", hal::pad::L1}, {"r1", hal::pad::R1},
        {"l2", hal::pad::L2}, {"r2", hal::pad::R2}, {"l3", hal::pad::L3}, {"r3", hal::pad::R3},
    };
    std::string spec = e;
    if (spec.size() > 1 && spec[0] == '@') {            // "@file": the list is in a file (a long run's does not fit the environment)
        std::string text;
        if (FILE* f = fopen(spec.c_str() + 1, "rb")) {
            char buf[4096];
            for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
                text.append(buf, n);
            fclose(f);
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        spec = text;
    }
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string::npos) end = spec.size();
        std::string item = spec.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos)
            for (const auto& n : kNames)
                if (item.substr(colon + 1) == n.name) {
                    const int from = atoi(item.c_str());
                    const size_t dash = item.find('-');
                    const int to = dash != std::string::npos && dash < colon ? atoi(item.c_str() + dash + 1) + 1 : from + 3;
                    s_script.push_back({from, to, n.btn});
                }
        pos = end + 1;
    }
}

// Game controllers (SDL's "game controller" layout, so any pad SDL knows is a
// DualShock here: the buttons by their place, not their letters).  Up to
// two; opened as they are plugged in.
static SDL_GameController* s_pads[2] = {nullptr, nullptr};
static int s_scanTimer = 0;

static void scan_controllers() {
    // A scripted run (debug) is steered by its script alone: a controller
    // that happens to be plugged in — and in somebody's hands — stays out.
    static const bool scripted = getenv("WOTS2_INPUT") || getenv("WOTS2_TOUR") || getenv("WOTS2_EXIT_FRAME");
    if (scripted)
        return;
    for (SDL_GameController*& pad : s_pads)
        if (pad && !SDL_GameControllerGetAttached(pad)) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
    if (s_pads[0] && s_pads[1])
        return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i))
            continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool open = false;
        for (SDL_GameController* pad : s_pads)
            if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id)
                open = true;
        if (open)
            continue;
        for (SDL_GameController*& pad : s_pads)
            if (!pad) {
                pad = SDL_GameControllerOpen(i);
                if (pad)
                    printf("[PAD] port %d: %s\n", (int)(&pad - s_pads), SDL_GameControllerName(pad));
                break;
            }
    }
}

// A stick as the two bytes a DualShock 2 gives (0x80 = centred).  A modern
// stick moves in a circle: pushed home on a diagonal each axis reads only
// 71 %, short of the 75 % the game takes for "run" (it tests the axes one by
// one, 0x1C76B0), where the PS2's stick reads nearly full on both.  So the
// circle is stretched toward the square, by up to 1.3.
static void stick_bytes(SDL_GameController* pad, SDL_GameControllerAxis ax, SDL_GameControllerAxis ay, u8* bx, u8* by,
                        float deadzone = 0.21f) {
    float x = SDL_GameControllerGetAxis(pad, ax) / 32767.0f, y = SDL_GameControllerGetAxis(pad, ay) / 32767.0f;
    const float len = sqrtf(x * x + y * y);
    *bx = *by = 0x80;
    if (len < deadzone)                 // a worn stick does not rest at the centre
        return;
    const float most = fabsf(x) > fabsf(y) ? fabsf(x) : fabsf(y);
    float gain = len / most;
    if (gain > 1.3f) gain = 1.3f;
    x *= gain; y *= gain;
    x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
    y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
    *bx = (u8)(0x80 + (int)lroundf(x * 127.0f));
    *by = (u8)(0x80 + (int)lroundf(y * 127.0f));
}

static u8 axis_byte(SDL_GameController* pad, SDL_GameControllerAxis axis) {
    const int v = SDL_GameControllerGetAxis(pad, axis);
    if (v > -7000 && v < 7000)          // a worn stick does not rest at the centre
        return 0x80;
    return (u8)((v + 32768) >> 8);
}

namespace hal::pad {

void init()     { SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER); load_script(); }
void shutdown() {
    for (SDL_GameController*& pad : s_pads)
        if (pad) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

static bool s_blocked = false;

void poll() {
    if (s_scanTimer-- <= 0) {
        s_scanTimer = 60;
        scan_controllers();
    }
    const u8* keys = SDL_GetKeyboardState(nullptr);
    const Config cfg = config();
    auto held = [&](int action) {
        const int sc = cfg.keys[action];
        return sc > SDL_SCANCODE_UNKNOWN && sc < SDL_NUM_SCANCODES && keys[sc];
    };
    u16 pressed = 0xFFFF;
    for (int a = 0; a < kButtonActions; a++)
        if (held(a))
            pressed &= ~static_cast<u16>(kActionButton[a]);  // 0 = pressed

    // The mouse: taken while playing (relative mode), let go for tools, the
    // menu, another window; a click on the window takes it again (that click
    // does nothing in the game).
    SDL_Window* window = hal::gs::window();
    const bool focused = window != nullptr && SDL_GetKeyboardFocus() == window;
    if (!focused)
        s_captureArmed = false;
    int mx = 0, my = 0;
    const Uint32 mouseButtons = SDL_GetRelativeMouseState(&mx, &my);
    bool swallowClick = false;
    if (focused && !s_captureArmed && mouseButtons != 0 && !s_blocked && !s_cursorNeeded) {
        s_captureArmed = true;
        swallowClick = true;
    }
    const bool capture = cfg.mouseCapture && focused && s_captureArmed && !s_blocked && !s_cursorNeeded;
    if ((SDL_GetRelativeMouseMode() == SDL_TRUE) != capture) {
        SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE);
        mx = my = 0;
    }
    if (capture && !swallowClick) {
        for (int a = 0; a < kButtonActions; a++) {
            const int in = cfg.mouse[a];
            bool down = false;
            if (in >= 1 && in <= 5)
                down = (mouseButtons & SDL_BUTTON(in)) != 0;
            else if (in == kMouseWheelUp)
                down = s_wheel > 0;
            else if (in == kMouseWheelDown)
                down = s_wheel < 0;
            if (down)
                pressed &= ~static_cast<u16>(kActionButton[a]);
        }
    }
    s_wheel = 0;
    // The camera from the mouse: this frame's motion in radians, given to the
    // follow camera directly (camera.cpp) — the stick's dead zone would eat
    // small movements and its speed limit large ones.
    {
        const float gain = 0.0012f * cfg.mouseSensitivity;    // about 70 degrees per 1000 pixels at 1.0
        s_mouseLook[0] = capture ? mx * gain : 0.0f;
        s_mouseLook[1] = capture ? my * gain * (cfg.mouseInvertY ? -1.0f : 1.0f) : 0.0f;
        if (mx != 0 || my != 0 || (capture && mouseButtons != 0))
            s_lastKeyboard = true;
    }
    int frame = hal::gs::frame_count();
    for (const auto& sp : s_script)
        if (frame >= sp.frame && frame < sp.end)
            pressed &= ~static_cast<u16>(sp.btn);
    s_buttons[0] = pressed;
    s_buttons[1] = 0xFFFF;

    // The sticks from the keyboard: T / G / F / H tilt the left one part of
    // the way (all the way with Left Shift), J / L and I / K push the right.
    for (int port = 0; port < 2; port++)
        s_lx[port] = s_ly[port] = s_rx[port] = s_ry[port] = 0x80;
    const bool modifier = held(kWalkFull);
    const int tilt = (cfg.walkDefaultFull ? !modifier : modifier) ? 0x7F : 0x48;
    for (int a = 0; a < kActions; a++)
        if (held(a)) { s_lastKeyboard = true; break; }
    if (held(kLStickLeft))  s_lx[0] = (u8)(0x80 - tilt);
    if (held(kLStickRight)) s_lx[0] = (u8)(0x80 + tilt);
    if (held(kLStickUp))    s_ly[0] = (u8)(0x80 - tilt);
    if (held(kLStickDown))  s_ly[0] = (u8)(0x80 + tilt);
    if (held(kRStickLeft))  s_rx[0] = 0x00;
    if (held(kRStickRight)) s_rx[0] = 0xFF;
    if (held(kRStickUp))    s_ry[0] = 0x00;
    if (held(kRStickDown))  s_ry[0] = 0xFF;
    // Debug: WOTS2_LSTICK="x,y" (0–255) holds the left stick somewhere.
    // ("from:x,y" only from that frame on.)
    // ("from:x,y;from:x,y…": a step at each; the last one begun holds.)
    if (const char* fixed = getenv("WOTS2_LSTICK")) {
        bool any = false;
        int hx = 0x80, hy = 0x80;
        for (const char* p = fixed; p && *p; ) {
            int from = 0, fx = 0x80, fy = 0x80;
            if (strchr(fixed, ':'))
                sscanf(p, "%d:%d,%d", &from, &fx, &fy);
            else
                sscanf(p, "%d,%d", &fx, &fy);
            if (frame >= from) { hx = fx; hy = fy; any = true; }
            p = strchr(p, ';');
            if (p) p++;
        }
        if (any) {
            s_lx[0] = (u8)hx;
            s_ly[0] = (u8)hy;
        }
    }

    // Debug: WOTS2_RSTICK="from-to:x,y" holds the right stick over those frames.
    if (const char* r = getenv("WOTS2_RSTICK")) {
        int from = 0, to = 0, fx = 0x80, fy = 0x80;
        if (sscanf(r, "%d-%d:%d,%d", &from, &to, &fx, &fy) == 4 && frame >= from && frame <= to) {
            s_rx[0] = (u8)fx;
            s_ry[0] = (u8)fy;
        }
    }

    // Debug: a stick position some hook asked for (see debug_stick).
    if (s_debugStickFrames > 0) {
        s_debugStickFrames--;
        s_lx[0] = s_debugStick[0];
        s_ly[0] = s_debugStick[1];
    }

    for (int port = 0; port < 2; port++) {
        SDL_GameController* pad = s_pads[port];
        if (!pad)
            continue;
        for (int a = 0; a < kButtonActions; a++) {
            const int in = cfg.pad[a];
            bool down = false;
            if (in >= 0 && in < SDL_CONTROLLER_BUTTON_MAX)
                down = SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)in) != 0;
            else if (in == kPadLeftTrigger)
                down = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
            else if (in == kPadRightTrigger)
                down = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
            if (down) {
                s_buttons[port] &= ~static_cast<u16>(kActionButton[a]);
                s_lastKeyboard = false;
            }
        }
        u8 lx, ly, rx, ry;
        stick_bytes(pad, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, &lx, &ly, cfg.deadzone);
        stick_bytes(pad, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY, &rx, &ry, cfg.deadzone);
        if (lx != 0x80) s_lx[port] = lx;        // (a key held wins over a stick at rest)
        if (ly != 0x80) s_ly[port] = ly;
        if (rx != 0x80) s_rx[port] = rx;
        if (ry != 0x80) s_ry[port] = ry;
    }
    // The camera (right stick), keys and controller alike: sensitivity and inversion.
    for (int port = 0; port < 2; port++) {
        auto shape = [&](u8& v, bool invert) {
            float d = ((int)v - 0x80) * cfg.cameraSensitivity;
            if (invert) d = -d;
            const int out = 0x80 + (int)lroundf(d);
            v = (u8)(out < 0 ? 0 : out > 0xFF ? 0xFF : out);
        };
        if (s_rx[port] != 0x80) shape(s_rx[port], cfg.invertX);
        if (s_ry[port] != 0x80) shape(s_ry[port], cfg.invertY);
    }
    if (s_blocked)
        for (int port = 0; port < 2; port++) {
            s_buttons[port] = 0xFFFF;
            s_lx[port] = s_ly[port] = s_rx[port] = s_ry[port] = 0x80;
        }
}

void set_blocked(bool blocked) { s_blocked = blocked; }
void set_cursor_needed(bool needed) { s_cursorNeeded = needed; }
void take_mouse_look(float* yaw, float* pitch) {
    *yaw = s_blocked ? 0.0f : s_mouseLook[0];
    *pitch = s_blocked ? 0.0f : s_mouseLook[1];
    s_mouseLook[0] = s_mouseLook[1] = 0.0f;
}
bool last_input_keyboard() { return s_lastKeyboard; }
// (The wheel comes as events: hle.cpp's pump hands them here.)
void mouse_wheel(int notches) { s_wheel += notches; }

static Config base_config() {
    Config c;
    for (int& k : c.keys) k = SDL_SCANCODE_UNKNOWN;
    for (int& m : c.mouse) m = -1;
    const int pad[kButtonActions] = {
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        kPadLeftTrigger, kPadRightTrigger,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    };
    for (int a = 0; a < kButtonActions; a++) c.pad[a] = pad[a];
    return c;
}
Config classic_config() {
    Config c = base_config();
    for (const auto& km : KEY_MAP)
        for (int a = 0; a < kButtonActions; a++)
            if (kActionButton[a] == km.btn)
                c.keys[a] = SDL_GetScancodeFromKey(km.key);
    c.keys[kLStickUp] = SDL_SCANCODE_T;    c.keys[kLStickDown] = SDL_SCANCODE_G;
    c.keys[kLStickLeft] = SDL_SCANCODE_F;  c.keys[kLStickRight] = SDL_SCANCODE_H;
    c.keys[kRStickUp] = SDL_SCANCODE_I;    c.keys[kRStickDown] = SDL_SCANCODE_K;
    c.keys[kRStickLeft] = SDL_SCANCODE_J;  c.keys[kRStickRight] = SDL_SCANCODE_L;
    c.keys[kWalkFull] = SDL_SCANCODE_LSHIFT;
    c.walkDefaultFull = false;
    c.mouse[kSquare - 0] = SDL_BUTTON_LEFT;
    c.mouse[kR1] = SDL_BUTTON_RIGHT;
    c.mouse[kR3] = SDL_BUTTON_MIDDLE;
    return c;
}
Config default_config() {
    Config c = base_config();
    c.keys[kStart] = SDL_SCANCODE_RETURN;      c.keys[kSelect] = SDL_SCANCODE_TAB;
    c.keys[kUp] = SDL_SCANCODE_UP;             c.keys[kDown] = SDL_SCANCODE_DOWN;
    c.keys[kLeft] = SDL_SCANCODE_LEFT;         c.keys[kRight] = SDL_SCANCODE_RIGHT;
    c.keys[kCross] = SDL_SCANCODE_SPACE;       c.keys[kCircle] = SDL_SCANCODE_E;
    c.keys[kSquare] = SDL_SCANCODE_UNKNOWN;    c.keys[kTriangle] = SDL_SCANCODE_F;
    c.keys[kL1] = SDL_SCANCODE_Q;              c.keys[kR1] = SDL_SCANCODE_UNKNOWN;
    c.keys[kL2] = SDL_SCANCODE_Z;              c.keys[kR2] = SDL_SCANCODE_C;
    c.keys[kL3] = SDL_SCANCODE_LSHIFT;         c.keys[kR3] = SDL_SCANCODE_R;
    c.keys[kLStickUp] = SDL_SCANCODE_W;        c.keys[kLStickDown] = SDL_SCANCODE_S;
    c.keys[kLStickLeft] = SDL_SCANCODE_A;      c.keys[kLStickRight] = SDL_SCANCODE_D;
    c.keys[kRStickUp] = SDL_SCANCODE_I;        c.keys[kRStickDown] = SDL_SCANCODE_K;
    c.keys[kRStickLeft] = SDL_SCANCODE_J;      c.keys[kRStickRight] = SDL_SCANCODE_L;
    c.keys[kWalkFull] = SDL_SCANCODE_LCTRL;
    c.walkDefaultFull = true;
    c.mouse[kSquare] = SDL_BUTTON_LEFT;
    c.mouse[kR1] = SDL_BUTTON_RIGHT;
    c.mouse[kR3] = SDL_BUTTON_MIDDLE;
    return c;
}
Config config() {
    if (!s_configReady) { s_config = default_config(); s_configReady = true; }
    return s_config;
}
void set_config(const Config& c) { s_config = c; s_configReady = true; }
const char* action_id(int a) {
    static const char* const kIds[kActions] = {
        "start", "select", "up", "down", "left", "right", "cross", "circle", "square", "triangle",
        "l1", "r1", "l2", "r2", "l3", "r3",
        "lstick_up", "lstick_down", "lstick_left", "lstick_right",
        "rstick_up", "rstick_down", "rstick_left", "rstick_right", "walk_full",
    };
    return a >= 0 && a < kActions ? kIds[a] : "";
}

u16 get_buttons(u32 port) { return port < 2 ? s_buttons[port] : 0xFFFF; }
u8  get_left_x(u32 port)  { return port < 2 ? s_lx[port] : 128; }
u8  get_left_y(u32 port)  { return port < 2 ? s_ly[port] : 128; }
u8  get_right_x(u32 port) { return port < 2 ? s_rx[port] : 128; }
u8  get_right_y(u32 port) { return port < 2 ? s_ry[port] : 128; }

bool connected(u32 port) { return port == 0 || (port < 2 && s_pads[port] != nullptr); }

void rumble(u32 port, bool small, u8 big) {
    if (port >= 2 || !s_pads[port])
        return;
    if (!config().rumble) {
        small = false;
        big = 0;
    }
    SDL_GameControllerRumble(s_pads[port], (Uint16)(big * 257), small ? 0xFFFF : 0, small || big ? 250 : 0);
}

}  // namespace hal::pad
