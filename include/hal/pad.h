#pragma once
#include "ps2_types.h"

// PAD (DualShock 2 controller) → SDL2 gamepad/keyboard abstraction.

namespace hal::pad {

void init();
void shutdown();
void poll();  // call once per frame
// While a tool takes the keyboard and mouse (the level editor): nothing pressed.
void set_blocked(bool blocked);

// Button bits (matches PS2 pad button byte layout)
enum Button : u16 {
    SELECT   = 0x0001,
    L3       = 0x0002,      // the sticks pressed in
    R3       = 0x0004,
    START    = 0x0008,
    UP       = 0x0010,
    RIGHT    = 0x0020,
    DOWN     = 0x0040,
    LEFT     = 0x0080,
    L2       = 0x0100,
    R2       = 0x0200,
    L1       = 0x0400,
    R1       = 0x0800,
    TRIANGLE = 0x1000,
    CIRCLE   = 0x2000,
    CROSS    = 0x4000,
    SQUARE   = 0x8000,
};

// Returns bitmask of currently-held buttons (0 = pressed, like PS2 hardware)
u16 get_buttons(u32 port);

// Analog stick values (0-255, 128 = center)
u8 get_left_x(u32 port);
u8 get_left_y(u32 port);
u8 get_right_x(u32 port);
u8 get_right_y(u32 port);

// Is there anything to read on this port?  (Port 0 always: the keyboard.)
bool connected(u32 port);
// The two motors of a DualShock: the small one is on or off, the big one has
// a strength.  Lasts until told otherwise (the game sends it every frame).
void rumble(u32 port, bool small, u8 big);

// PC settings: what the keyboard and the controller do.  Actions 0-15 are
// the DualShock buttons, then the two sticks' directions and the walk
// modifier (keyboard).  Keys are SDL scancodes; a controller input is an
// SDL_GameControllerButton, kPadLeftTrigger / kPadRightTrigger, or -1.
enum Action {
    kStart, kSelect, kUp, kDown, kLeft, kRight, kCross, kCircle, kSquare, kTriangle,
    kL1, kR1, kL2, kR2, kL3, kR3,
    kLStickUp, kLStickDown, kLStickLeft, kLStickRight,
    kRStickUp, kRStickDown, kRStickLeft, kRStickRight, kWalkFull,   // (walk modifier: see walkDefaultFull)
    kActions,
    kButtonActions = kLStickUp,
};
constexpr int kPadLeftTrigger = 100, kPadRightTrigger = 101;
// Mouse inputs for a button: SDL_BUTTON_LEFT … SDL_BUTTON_X2 (1-5), the
// wheel up / down (one press per notch), or -1.
constexpr int kMouseWheelUp = 6, kMouseWheelDown = 7;
struct Config {
    int   keys[kActions];
    int   pad[kButtonActions];
    int   mouse[kButtonActions];
    bool  walkDefaultFull = true;       // keys move the stick all the way; the modifier walks (else the reverse)
    float mouseSensitivity = 1.0f;      // the camera from the mouse, 0.1-4
    bool  mouseInvertY = false;
    bool  mouseCapture = true;          // the cursor is taken while playing (relative mouse)
    float cameraSensitivity = 1.0f;     // the right stick's movement, 0.25-2
    bool  invertX = false, invertY = false;
    float deadzone = 0.21f;             // sticks: how far before they count
    bool  rumble = true;
};
Config default_config();              // the modern layout: WASD + mouse
Config classic_config();              // the port's first layout: arrows, Z X A S, no mouse buttons
Config config();
void   set_config(const Config& config);
const char* action_id(int action);      // the settings.ini key

// The cursor is wanted (a tool drawn over the picture): the mouse is let go.
void set_cursor_needed(bool needed);
void mouse_wheel(int notches);          // SDL_MOUSEWHEEL, from the event pump
// This frame's mouse look in radians (right = +yaw, down = +pitch), then zero.
void take_mouse_look(float* yaw, float* pitch);
// The last input came from the mouse or keyboard (true) or a controller.
bool last_input_keyboard();

// Debug aid (scripted runs): hold port 0's left stick at (x, y) for the next
// two polls.
void debug_stick(u8 x, u8 y);

}  // namespace hal::pad
