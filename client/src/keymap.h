#ifndef RDPMCP_KEYMAP_H
#define RDPMCP_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL.h>

/* SDL-scancode naar RDP-scancode (0x100 = extended), 0 = onbekend */
uint32_t rdpmcp_sdl_to_rdp_scancode(SDL_Scancode sc, bool cmd_as_win);

#endif
