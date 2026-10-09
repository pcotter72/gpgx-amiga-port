/****************************************************************************
 *  input_amiga.c
 *
 *  Input handling for Genesis Plus GX Amiga Port
 *  Supports keyboard, joystick (gameport), and CD32 pad via lowlevel.library
 *
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include <exec/exec.h>
#include <devices/gameport.h>
#include <devices/inputevent.h>
#include <libraries/lowlevel.h>

#include <proto/exec.h>
#include <proto/lowlevel.h>

#include "input_amiga.h"

/* Libraries */
struct Library *LowLevelBase = NULL;

/* Maximum players */
#define MAX_PLAYERS 2

/* Key mapping table */
#define MAX_KEY_MAPPINGS 16

typedef struct
{
    unsigned char key;
    unsigned short button;
} key_mapping_t;

/* Player input state */
typedef struct
{
    unsigned short state;           /* Current button state */
    int joy_enabled;                /* Use joystick input */
    key_mapping_t keys[MAX_KEY_MAPPINGS];
    int num_keys;
} player_input_t;

static player_input_t player_input[MAX_PLAYERS];

/* Keyboard state (256 keys max) */
static unsigned char key_state[32];  /* Bit array: 256 bits = 32 bytes */

/* Hotkey detection (polled by main loop) */
volatile int input_amiga_hotkey = 0;

/* Default key mappings for player 1 */
static const key_mapping_t default_keys_p1[] = {
    { 0x4C, INPUT_BTN_UP },      /* Up arrow */
    { 0x4D, INPUT_BTN_DOWN },    /* Down arrow */
    { 0x4F, INPUT_BTN_LEFT },    /* Left arrow */
    { 0x4E, INPUT_BTN_RIGHT },   /* Right arrow */
    { 0x01, INPUT_BTN_A },       /* 1 key */
    { 0x02, INPUT_BTN_B },       /* 2 key */
    { 0x03, INPUT_BTN_C },       /* 3 key */
    { 0x10, INPUT_BTN_X },       /* Q key */
    { 0x11, INPUT_BTN_Y },       /* W key */
    { 0x12, INPUT_BTN_Z },       /* E key */
    { 0x40, INPUT_BTN_START },   /* Space */
    { 0x44, INPUT_BTN_START },   /* Enter */
    { 0x00, 0 }                  /* Terminator */
};

/* Default key mappings for player 2 (numpad) */
static const key_mapping_t default_keys_p2[] = {
    { 0x3E, INPUT_BTN_UP },      /* Numpad 8 */
    { 0x1E, INPUT_BTN_DOWN },    /* Numpad 2 */
    { 0x2D, INPUT_BTN_LEFT },    /* Numpad 4 */
    { 0x2F, INPUT_BTN_RIGHT },   /* Numpad 6 */
    { 0x0F, INPUT_BTN_A },       /* Numpad 0 */
    { 0x3D, INPUT_BTN_B },       /* Numpad . */
    { 0x43, INPUT_BTN_C },       /* Numpad Enter */
    { 0x5F, INPUT_BTN_START },   /* Help */
    { 0x00, 0 }                  /* Terminator */
};

/****************************************************************************
 * Initialize input subsystem
 ****************************************************************************/
int input_amiga_init(void)
{
    int i;
    const key_mapping_t *map;
    
    /* Clear state */
    memset(player_input, 0, sizeof(player_input));
    memset(key_state, 0, sizeof(key_state));
    
    /* Set default key mappings */
    map = default_keys_p1;
    for (i = 0; map[i].button != 0 && i < MAX_KEY_MAPPINGS; i++)
    {
        player_input[0].keys[i] = map[i];
    }
    player_input[0].num_keys = i;
    
    map = default_keys_p2;
    for (i = 0; map[i].button != 0 && i < MAX_KEY_MAPPINGS; i++)
    {
        player_input[1].keys[i] = map[i];
    }
    player_input[1].num_keys = i;
    
    /* Try to open lowlevel.library for CD32 pad support */
    LowLevelBase = OpenLibrary("lowlevel.library", 40);
    if (LowLevelBase)
    {
        player_input[0].joy_enabled = 1;
        player_input[1].joy_enabled = 1;
    }
    
    return 1;
}

/****************************************************************************
 * Shutdown input subsystem
 ****************************************************************************/
void input_amiga_shutdown(void)
{
    if (LowLevelBase)
    {
        CloseLibrary(LowLevelBase);
        LowLevelBase = NULL;
    }
}

/****************************************************************************
 * Process window IDCMP messages
 ****************************************************************************/
int input_amiga_process(struct Window *window)
{
    struct IntuiMessage *msg;
    ULONG class;
    UWORD code;
    int quit = 0;
    
    if (!window)
        return 0;
    
    while ((msg = (struct IntuiMessage *)GetMsg(window->UserPort)))
    {
        class = msg->Class;
        code = msg->Code;
        ReplyMsg((struct Message *)msg);
        
        switch (class)
        {
            case IDCMP_CLOSEWINDOW:
                quit = 1;
                break;
            
            case IDCMP_ACTIVEWINDOW:
            case IDCMP_INACTIVEWINDOW:
                /* Focus changed: key-up events for held keys go to the other
                 * window, so any latched state here would stick. Clear it. */
                memset(key_state, 0, sizeof(key_state));
                break;
                
            case IDCMP_RAWKEY:
                {
                    int keycode = code & 0x7F;
                    int pressed = (code < 0x80);
                    int byte_idx = keycode >> 3;
                    int bit_mask = 1 << (keycode & 7);
                    
                    /* Check for ESC to quit */
                    if (keycode == 0x45 && pressed)
                    {
                        quit = 1;
                        break;
                    }
                    
                    /* Check for F5 (save state) and F8 (load state) */
                    if (pressed)
                    {
                        if (keycode == 0x54)  /* F5 */
                        {
                            input_amiga_hotkey = INPUT_HOTKEY_SAVE;
                        }
                        else if (keycode == 0x57)  /* F8 */
                        {
                            input_amiga_hotkey = INPUT_HOTKEY_LOAD;
                        }
                    }
                    
                    /* Update key state */
                    if (pressed)
                        key_state[byte_idx] |= bit_mask;
                    else
                        key_state[byte_idx] &= ~bit_mask;
                }
                break;
        }
    }
    
    return quit;
}

/****************************************************************************
 * Check if a key is pressed
 ****************************************************************************/
static int key_pressed(int keycode)
{
    if (keycode < 0 || keycode >= 256)
        return 0;
    return (key_state[keycode >> 3] & (1 << (keycode & 7))) != 0;
}

/****************************************************************************
 * Read joystick via lowlevel.library
 ****************************************************************************/
static unsigned short read_lowlevel_joy(int port)
{
    unsigned short state = 0;
    ULONG joy;
    ULONG joytype;
    
    if (!LowLevelBase)
        return 0;
    
    /* ReadJoyPort(0) = Amiga port 0 (mouse port, under power LED)
     * ReadJoyPort(1) = Amiga port 1 (joystick port, right side)
     * For Genesis emulation: player 0 uses Amiga port 1 (joystick) */
    joy = ReadJoyPort(port == 0 ? 1 : 0);
    
    /* Only accept real joystick or game controller types */
    joytype = joy & JP_TYPE_MASK;
    if (joytype != JP_TYPE_GAMECTLR && joytype != JP_TYPE_JOYSTK)
        return 0;
    
    /* Map directions */
    if (joy & JPF_JOY_UP)    state |= INPUT_BTN_UP;
    if (joy & JPF_JOY_DOWN)  state |= INPUT_BTN_DOWN;
    if (joy & JPF_JOY_LEFT)  state |= INPUT_BTN_LEFT;
    if (joy & JPF_JOY_RIGHT) state |= INPUT_BTN_RIGHT;
    
    /* Check controller type for button mapping */
    if (joytype == JP_TYPE_GAMECTLR)
    {
        /* CD32 controller — matches Genesis 6-button layout */
        if (joy & JPF_BUTTON_RED)     state |= INPUT_BTN_A;     /* Red     = A */
        if (joy & JPF_BUTTON_BLUE)    state |= INPUT_BTN_B;     /* Blue    = B */
        if (joy & JPF_BUTTON_GREEN)   state |= INPUT_BTN_C;     /* Green   = C */
        if (joy & JPF_BUTTON_YELLOW)  state |= INPUT_BTN_X;     /* Yellow  = X */
        if (joy & JPF_BUTTON_REVERSE) state |= INPUT_BTN_Y;     /* Rev     = Y */
        if (joy & JPF_BUTTON_FORWARD) state |= INPUT_BTN_Z;     /* Fwd     = Z */
        if (joy & JPF_BUTTON_PLAY)    state |= INPUT_BTN_START; /* Play    = Start */
    }
    else
    {
        /* Standard joystick - single/dual button */
        if (joy & JPF_BUTTON_RED)    state |= INPUT_BTN_A;
        if (joy & JPF_BUTTON_BLUE)   state |= INPUT_BTN_B;
    }
    
    return state;
}

/****************************************************************************
 * Read current input state
 ****************************************************************************/
unsigned short input_amiga_read(int player)
{
    unsigned short state = 0;
    int i;
    
    if (player < 0 || player >= MAX_PLAYERS)
        return 0;
    
    /* Read keyboard input */
    for (i = 0; i < player_input[player].num_keys; i++)
    {
        if (key_pressed(player_input[player].keys[i].key))
        {
            state |= player_input[player].keys[i].button;
        }
    }
    
    /* Read joystick input if enabled */
    if (player_input[player].joy_enabled)
    {
        state |= read_lowlevel_joy(player);
    }
    
    return state;
}

/****************************************************************************
 * Set key mapping
 ****************************************************************************/
void input_amiga_set_key(int player, int key, int button)
{
    int i;
    
    if (player < 0 || player >= MAX_PLAYERS)
        return;
    
    /* Check if key already mapped, update it */
    for (i = 0; i < player_input[player].num_keys; i++)
    {
        if (player_input[player].keys[i].key == key)
        {
            player_input[player].keys[i].button = button;
            return;
        }
    }
    
    /* Add new mapping if space available */
    if (player_input[player].num_keys < MAX_KEY_MAPPINGS)
    {
        player_input[player].keys[player_input[player].num_keys].key = key;
        player_input[player].keys[player_input[player].num_keys].button = button;
        player_input[player].num_keys++;
    }
}

void input_amiga_remap_button(int player, int button, int new_key)
{
    int i;
    
    if (player < 0 || player >= MAX_PLAYERS)
        return;
    
    /* Find and replace the first existing mapping for this button */
    for (i = 0; i < player_input[player].num_keys; i++)
    {
        if (player_input[player].keys[i].button == button)
        {
            player_input[player].keys[i].key = new_key;
            return;
        }
    }
    
    /* Button not found in defaults, add as new mapping */
    if (player_input[player].num_keys < MAX_KEY_MAPPINGS)
    {
        player_input[player].keys[player_input[player].num_keys].key = new_key;
        player_input[player].keys[player_input[player].num_keys].button = button;
        player_input[player].num_keys++;
    }
}

/****************************************************************************
 * Enable/disable joystick
 ****************************************************************************/
void input_amiga_enable_joy(int player, int enable)
{
    if (player < 0 || player >= MAX_PLAYERS)
        return;
    
    player_input[player].joy_enabled = enable;
}
