/*
 * Doom input back end: translates the USB game pad state into Doom events.
 *
 * Controller layout (DualSense / DualShock 4):
 *
 *   In game                             In menus
 *   -------                             --------
 *   Left stick    move / strafe         Left stick / D-pad  navigate
 *   Right stick   turn                  Cross               select ('y' at prompts)
 *   D-pad         move / turn           Circle              back   ('n' at prompts)
 *   R2, Square    fire                  Options             close menu
 *   Cross         use (doors, switches)
 *   L2            run (inverts always-run)
 *   L3            toggle always-run (default on)
 *   R1 / L1       next / previous weapon
 *   Triangle, touch pad click   automap
 *   Options, PS   menu
 *   Create        pause
 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ra8_port.h"

#include "doomtype.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "d_event.h"
#include "p_saveg.h"

/* Menu internals from m_menu.c (plain globals in the engine). */
extern boolean menuactive;
extern int     messageToPrint;
extern boolean messageNeedsInput;
extern int     saveStringEnter;
extern int     saveSlot;
extern int     saveCharIndex;
extern char    savegamestrings[10][SAVESTRINGSIZE];

/* Required by the engine (normally in i_input.c). */
int vanilla_keyboard_mapping = 1;

#define STICK_DEADZONE        (20)
#define MENU_STICK_THRESHOLD  (72)
#define MENU_REPEAT_DELAY_MS  (400)
#define MENU_REPEAT_RATE_MS   (110)

typedef enum
{
    CTX_GAME,
    CTX_MENU,
    CTX_PROMPT,                        /* y/n message box                 */
    CTX_SAVENAME,                      /* typing a save game description  */
} input_ctx_t;

typedef struct
{
    uint32_t mask;
    int      game_key;
    int      menu_key;
    int      prompt_key;
} button_map_t;

static const button_map_t g_button_map[] =
{
    /* mask              in game         in menus        at y/n prompts */
    { PAD_BTN_CROSS,     KEY_USE,        KEY_ENTER,      'y'          },
    { PAD_BTN_CIRCLE,    0,              KEY_BACKSPACE,  'n'          },
    { PAD_BTN_SQUARE,    KEY_FIRE,       0,              0            },
    { PAD_BTN_TRIANGLE,  KEY_TAB,        0,              0            },
    { PAD_BTN_R2,        KEY_FIRE,       KEY_ENTER,      'y'          },
    { PAD_BTN_R1,        ']',            0,              0            },
    { PAD_BTN_L1,        '[',            0,              0            },
    { PAD_BTN_OPTIONS,   KEY_ESCAPE,     KEY_ESCAPE,     KEY_ESCAPE   },
    { PAD_BTN_PS,        KEY_ESCAPE,     KEY_ESCAPE,     KEY_ESCAPE   },
    { PAD_BTN_CREATE,    KEY_PAUSE,      0,              0            },
    { PAD_BTN_TOUCH,     KEY_TAB,        0,              0            },
    { PAD_BTN_UP,        KEY_UPARROW,    0,              0            },
    { PAD_BTN_DOWN,      KEY_DOWNARROW,  0,              0            },
    { PAD_BTN_LEFT,      KEY_LEFTARROW,  0,              0            },
    { PAD_BTN_RIGHT,     KEY_RIGHTARROW, 0,              0            },
};

#define NUM_BUTTONS  (sizeof(g_button_map) / sizeof(g_button_map[0]))

static pad_state_t g_pad;                       /* Latest snapshot          */
static int         g_held_key[NUM_BUTTONS];     /* Key sent for each button */
static uint32_t    g_prev_buttons;
static bool        g_prev_connected;

static bool        g_autorun = true;
static bool        g_run_key_down;

/* Menu navigation (D-pad or left stick) with auto repeat. */
static int         g_nav_key;
static uint32_t    g_nav_next_ms;

static void post_key (evtype_t type, int key)
{
    event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type  = type;
    ev.data1 = key;
    ev.data2 = (ev_keydown == type) ? key : 0;
    D_PostEvent(&ev);
}

static uint32_t now_ms (void)
{
    return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static input_ctx_t current_context (void)
{
    if (saveStringEnter)
    {
        return CTX_SAVENAME;
    }

    if (messageToPrint && messageNeedsInput)
    {
        return CTX_PROMPT;
    }

    if (menuactive)
    {
        return CTX_MENU;
    }

    return CTX_GAME;
}

/* A game pad cannot type, so give an empty save slot a useful name. */
static void fill_default_save_name (void)
{
    if ((saveSlot >= 0) && (saveSlot < 10) && ('\0' == savegamestrings[saveSlot][0]))
    {
        snprintf(savegamestrings[saveSlot], SAVESTRINGSIZE, "E%dM%d SLOT%d", gameepisode, gamemap, saveSlot + 1);
        saveCharIndex = (int) strlen(savegamestrings[saveSlot]);
    }
}

static int key_for_button (size_t i, input_ctx_t ctx)
{
    switch (ctx)
    {
        case CTX_GAME:
            return g_button_map[i].game_key;

        case CTX_PROMPT:
            return g_button_map[i].prompt_key;

        case CTX_SAVENAME:
            if ((PAD_BTN_CROSS == g_button_map[i].mask) || (PAD_BTN_R2 == g_button_map[i].mask))
            {
                fill_default_save_name();

                return KEY_ENTER;
            }

            if (PAD_BTN_CIRCLE == g_button_map[i].mask)
            {
                return KEY_ESCAPE;     /* Cancel editing */
            }

            return 0;

        case CTX_MENU:
        default:
            return g_button_map[i].menu_key;
    }
}

static void release_all (void)
{
    for (size_t i = 0; i < NUM_BUTTONS; i++)
    {
        if (0 != g_held_key[i])
        {
            post_key(ev_keyup, g_held_key[i]);
            g_held_key[i] = 0;
        }
    }

    if (g_run_key_down)
    {
        post_key(ev_keyup, KEY_RSHIFT);
        g_run_key_down = false;
    }

    if (0 != g_nav_key)
    {
        post_key(ev_keyup, g_nav_key);
        g_nav_key = 0;
    }

    g_prev_buttons = 0;
}

/* Menu navigation from the D-pad or the left stick, with key repeat. */
static void update_menu_navigation (input_ctx_t ctx)
{
    int key = 0;

    if (CTX_MENU == ctx)
    {
        int lx = (int) g_pad.lx - 128;
        int ly = (int) g_pad.ly - 128;

        if ((g_pad.buttons & PAD_BTN_UP) || (ly < -MENU_STICK_THRESHOLD))
        {
            key = KEY_UPARROW;
        }
        else if ((g_pad.buttons & PAD_BTN_DOWN) || (ly > MENU_STICK_THRESHOLD))
        {
            key = KEY_DOWNARROW;
        }
        else if ((g_pad.buttons & PAD_BTN_LEFT) || (lx < -MENU_STICK_THRESHOLD))
        {
            key = KEY_LEFTARROW;
        }
        else if ((g_pad.buttons & PAD_BTN_RIGHT) || (lx > MENU_STICK_THRESHOLD))
        {
            key = KEY_RIGHTARROW;
        }
    }

    uint32_t t = now_ms();

    if (key != g_nav_key)
    {
        if (0 != g_nav_key)
        {
            post_key(ev_keyup, g_nav_key);
        }

        if (0 != key)
        {
            post_key(ev_keydown, key);
            g_nav_next_ms = t + MENU_REPEAT_DELAY_MS;
        }

        g_nav_key = key;
    }
    else if ((0 != key) && ((int32_t) (t - g_nav_next_ms) >= 0))
    {
        post_key(ev_keydown, key);
        g_nav_next_ms = t + MENU_REPEAT_RATE_MS;
    }
}

void I_InitInput (void)
{
    /* State is statically initialised; I_GetEvent() may already have run
     * (D_DoomLoop calls TryRunTics before I_InitGraphics), so do not reset
     * the connection tracking here. */
}

/* Called once per game tic from I_StartTic(). */
void I_GetEvent (void)
{
    usb_gamepad_get_state(&g_pad);

    if (g_pad.connected != g_prev_connected)
    {
        printf("INPUT: game pad %s\n", g_pad.connected ? "connected" : "disconnected");
        g_prev_connected = g_pad.connected;

        if (!g_pad.connected)
        {
            release_all();

            return;
        }
    }

    if (!g_pad.connected)
    {
        return;
    }

    input_ctx_t ctx     = current_context();
    uint32_t    buttons = g_pad.buttons;
    uint32_t    changed = buttons ^ g_prev_buttons;

    /* Buttons -> keys. The key sent on press is remembered so that the same
     * key is released even if the context changed while it was held. */
    for (size_t i = 0; i < NUM_BUTTONS; i++)
    {
        uint32_t mask = g_button_map[i].mask;

        if (0 == (changed & mask))
        {
            continue;
        }

        if (buttons & mask)
        {
            /* D-pad in menus is handled by the repeat logic below. */
            if ((CTX_MENU == ctx) && (mask & (PAD_BTN_UP | PAD_BTN_DOWN | PAD_BTN_LEFT | PAD_BTN_RIGHT)))
            {
                continue;
            }

            int key = key_for_button(i, ctx);
            if (0 != key)
            {
                post_key(ev_keydown, key);
                g_held_key[i] = key;
            }
        }
        else if (0 != g_held_key[i])
        {
            post_key(ev_keyup, g_held_key[i]);
            g_held_key[i] = 0;
        }
    }

    /* L3 toggles always-run; L2 temporarily inverts it. */
    if ((changed & PAD_BTN_L3) && (buttons & PAD_BTN_L3))
    {
        g_autorun = !g_autorun;
        printf("INPUT: always run %s\n", g_autorun ? "on" : "off");
    }

    bool run = g_autorun ^ (0 != (buttons & PAD_BTN_L2));
    if (run != g_run_key_down)
    {
        post_key(run ? ev_keydown : ev_keyup, KEY_RSHIFT);
        g_run_key_down = run;
    }

    update_menu_navigation(ctx);

    g_prev_buttons = buttons;
}

/* Apply a dead zone and rescale a stick axis to -127..127. */
static int stick_axis (uint8_t raw)
{
    int v   = (int) raw - 128;
    int mag = (v < 0) ? -v : v;

    if (mag <= STICK_DEADZONE)
    {
        return 0;
    }

    if (mag > 127)
    {
        mag = 127;
    }

    mag = ((mag - STICK_DEADZONE) * 127) / (127 - STICK_DEADZONE);

    return (v < 0) ? -mag : mag;
}

/* Analog movement hook used by G_BuildTiccmd() (see doom/tools/patch_engine.py). */
void I_GetAnalogAxes (int * p_turn, int * p_fwd, int * p_strafe)
{
    *p_turn   = 0;
    *p_fwd    = 0;
    *p_strafe = 0;

    if (!g_pad.connected || menuactive)
    {
        return;
    }

    int rx = stick_axis(g_pad.rx);

    /* Quadratic response on the turn axis for precise aiming. */
    *p_turn   = (rx * ((rx < 0) ? -rx : rx)) / 127;
    *p_fwd    = -stick_axis(g_pad.ly);
    *p_strafe = stick_axis(g_pad.lx);
}
