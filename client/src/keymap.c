#include "rdpmcp.h"

#define EXT 0x100

/* SDL-scancodes zijn USB-HID-posities; RDP verwacht PC-scancodes (set 1).
 * Beide zijn positioneel, dus de remote toetsenbordindeling bepaalt het teken. */
uint32_t rdpmcp_sdl_to_rdp_scancode(SDL_Scancode sc, bool cmd_as_win)
{
	static const uint32_t letters[26] = {
		/* a    b     c     d     e     f     g     h     i     j     k     l     m */
		0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
		/* n    o     p     q     r     s     t     u     v     w     x     y     z */
		0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
	};

	if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
		return letters[sc - SDL_SCANCODE_A];
	if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0)
		return 0x02 + (sc - SDL_SCANCODE_1); /* 1..9 = 0x02..0x0A, 0 = 0x0B */
	if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10)
		return 0x3B + (sc - SDL_SCANCODE_F1);

	switch (sc)
	{
		case SDL_SCANCODE_RETURN: return 0x1C;
		case SDL_SCANCODE_ESCAPE: return 0x01;
		case SDL_SCANCODE_BACKSPACE: return 0x0E;
		case SDL_SCANCODE_TAB: return 0x0F;
		case SDL_SCANCODE_SPACE: return 0x39;
		case SDL_SCANCODE_MINUS: return 0x0C;
		case SDL_SCANCODE_EQUALS: return 0x0D;
		case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
		case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
		case SDL_SCANCODE_BACKSLASH: return 0x2B;
		case SDL_SCANCODE_NONUSHASH: return 0x2B;
		case SDL_SCANCODE_SEMICOLON: return 0x27;
		case SDL_SCANCODE_APOSTROPHE: return 0x28;
		case SDL_SCANCODE_GRAVE: return 0x29;
		case SDL_SCANCODE_COMMA: return 0x33;
		case SDL_SCANCODE_PERIOD: return 0x34;
		case SDL_SCANCODE_SLASH: return 0x35;
		case SDL_SCANCODE_NONUSBACKSLASH: return 0x56;
		case SDL_SCANCODE_CAPSLOCK: return 0x3A;
		case SDL_SCANCODE_F11: return 0x57;
		case SDL_SCANCODE_F12: return 0x58;
		case SDL_SCANCODE_PRINTSCREEN: return EXT | 0x37;
		case SDL_SCANCODE_SCROLLLOCK: return 0x46;
		case SDL_SCANCODE_INSERT: return EXT | 0x52;
		case SDL_SCANCODE_HOME: return EXT | 0x47;
		case SDL_SCANCODE_PAGEUP: return EXT | 0x49;
		case SDL_SCANCODE_DELETE: return EXT | 0x53;
		case SDL_SCANCODE_END: return EXT | 0x4F;
		case SDL_SCANCODE_PAGEDOWN: return EXT | 0x51;
		case SDL_SCANCODE_RIGHT: return EXT | 0x4D;
		case SDL_SCANCODE_LEFT: return EXT | 0x4B;
		case SDL_SCANCODE_DOWN: return EXT | 0x50;
		case SDL_SCANCODE_UP: return EXT | 0x48;
		case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
		case SDL_SCANCODE_KP_DIVIDE: return EXT | 0x35;
		case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
		case SDL_SCANCODE_KP_MINUS: return 0x4A;
		case SDL_SCANCODE_KP_PLUS: return 0x4E;
		case SDL_SCANCODE_KP_ENTER: return EXT | 0x1C;
		case SDL_SCANCODE_KP_1: return 0x4F;
		case SDL_SCANCODE_KP_2: return 0x50;
		case SDL_SCANCODE_KP_3: return 0x51;
		case SDL_SCANCODE_KP_4: return 0x4B;
		case SDL_SCANCODE_KP_5: return 0x4C;
		case SDL_SCANCODE_KP_6: return 0x4D;
		case SDL_SCANCODE_KP_7: return 0x47;
		case SDL_SCANCODE_KP_8: return 0x48;
		case SDL_SCANCODE_KP_9: return 0x49;
		case SDL_SCANCODE_KP_0: return 0x52;
		case SDL_SCANCODE_KP_PERIOD: return 0x53;
		case SDL_SCANCODE_KP_EQUALS: return 0x59;
		case SDL_SCANCODE_APPLICATION: return EXT | 0x5D;
		case SDL_SCANCODE_LCTRL: return 0x1D;
		case SDL_SCANCODE_LSHIFT: return 0x2A;
		case SDL_SCANCODE_LALT: return 0x38;
		case SDL_SCANCODE_RCTRL: return EXT | 0x1D;
		case SDL_SCANCODE_RSHIFT: return 0x36;
		case SDL_SCANCODE_RALT: return EXT | 0x38;
		/* Cmd op de Mac: standaard Ctrl (Cmd+C = kopiëren), met --cmd-as-win de Windows-toets */
		case SDL_SCANCODE_LGUI: return cmd_as_win ? (EXT | 0x5B) : 0x1D;
		case SDL_SCANCODE_RGUI: return cmd_as_win ? (EXT | 0x5C) : (EXT | 0x1D);
		default: return 0;
	}
}
