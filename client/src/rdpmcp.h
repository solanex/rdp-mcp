#ifndef RDPMCP_H
#define RDPMCP_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <freerdp/client.h>
#include <freerdp/freerdp.h>

#include <SDL3/SDL.h>

/* Gedeelde toestand van één RDP-sessie. Drie threads gebruiken dit:
 *  - main:    SDL-venster, lokale muis/toetsenbord, tekenen
 *  - rdp:     FreeRDP-eventloop, schrijft de framebuffer
 *  - control: Unix-socket voor de MCP-server (screenshot + AI-invoer) */
typedef struct
{
	rdpClientContext common; /* moet eerst staan: FreeRDP cast hiernaar */

	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;

	/* Kopie van het remote scherm in BGRX32, beschermd door fb_lock */
	pthread_mutex_t fb_lock;
	uint8_t* fb;
	int fb_w, fb_h;
	bool fb_resized;
	bool fb_has_dirty;
	int dirty_x1, dirty_y1, dirty_x2, dirty_y2;
	atomic_bool paint_pending;

	/* Alle freerdp_input_*-aanroepen gaan via deze lock */
	pthread_mutex_t input_lock;
	atomic_bool connected;

	/* AI-besturing */
	atomic_bool ai_enabled;
	_Atomic uint64_t ai_last_ms;
	atomic_int ai_x, ai_y;

	/* Opties */
	bool cmd_as_win;

	/* Control-socket */
	char host[256];
	char sock_path[PATH_MAX];
	int listen_fd;
	pthread_t control_thread;
} mcpContext;

/* Eigen SDL-events (geregistreerd in main.c) */
extern Uint32 RDPMCP_EVENT_PAINT;
extern Uint32 RDPMCP_EVENT_RESIZE;
extern Uint32 RDPMCP_EVENT_DISCONNECTED;
extern Uint32 RDPMCP_EVENT_AI;

void rdpmcp_push_event(Uint32 type);

/* keymap.c: SDL-scancode naar RDP-scancode (0x100 = extended), 0 = onbekend */
uint32_t rdpmcp_sdl_to_rdp_scancode(SDL_Scancode sc, bool cmd_as_win);

/* control.c */
bool rdpmcp_control_start(mcpContext* mc);
void rdpmcp_control_stop(mcpContext* mc);

#endif
