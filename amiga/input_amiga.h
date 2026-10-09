/****************************************************************************
 *  input_amiga.h
 *
 *  Input handling for Genesis Plus GX Amiga Port
 *  Supports keyboard, joystick (gameport), and CD32 pad
 *
 ****************************************************************************/

#ifndef _INPUT_AMIGA_H_
#define _INPUT_AMIGA_H_

#include <intuition/intuition.h>

/* Initialize input subsystem */
int input_amiga_init(void);

/* Shutdown input subsystem */
void input_amiga_shutdown(void);

/* Process window IDCMP messages */
/* Returns: 0 to continue, 1 if quit requested */
int input_amiga_process(struct Window *window);

/* Read current input state for a player */
/* Returns: Genesis controller button mask */
unsigned short input_amiga_read(int player);

/* Set key mapping for a player */
/* key: Amiga raw key code */
/* button: Genesis button (INPUT_UP, INPUT_A, etc.) */
void input_amiga_set_key(int player, int key, int button);
void input_amiga_remap_button(int player, int button, int new_key);

/* Enable/disable joystick input */
void input_amiga_enable_joy(int player, int enable);

/* Genesis button definitions */
#define INPUT_BTN_UP      0x0001
#define INPUT_BTN_DOWN    0x0002
#define INPUT_BTN_LEFT    0x0004
#define INPUT_BTN_RIGHT   0x0008
#define INPUT_BTN_A       0x0010
#define INPUT_BTN_B       0x0020
#define INPUT_BTN_C       0x0040
#define INPUT_BTN_START   0x0080
#define INPUT_BTN_X       0x0100
#define INPUT_BTN_Y       0x0200
#define INPUT_BTN_Z       0x0400
#define INPUT_BTN_MODE    0x0800

/* Hotkey actions (set by input, polled by main loop) */
#define INPUT_HOTKEY_NONE  0
#define INPUT_HOTKEY_SAVE  1
#define INPUT_HOTKEY_LOAD  2
extern volatile int input_amiga_hotkey;

#endif /* _INPUT_AMIGA_H_ */
