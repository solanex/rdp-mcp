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

	/* Laatste lokale toets/klik van de gebruiker (SDL_GetTicks) */
	_Atomic uint64_t user_last_ms;

	/* Klembord (clipboard.c), beschermd door clip_lock */
	struct s_cliprdr_client_context* cliprdr;
	pthread_mutex_t clip_lock;
	pthread_cond_t clip_cond;
	int clip_owner;          /* CLIP_OWNER_* */
	char* clip_local_text;   /* Mac-klembord (UTF-8, LF) */
	char* clip_remote_text;  /* remote klembord (UTF-8, LF) */
	char* clip_ai_text;      /* door de AI gezet (UTF-8, LF) */
	bool clip_pending;       /* wachten op tekst van de server */

	/* Opties */
	bool cmd_as_win;
	bool clipboard_enabled;

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
extern Uint32 RDPMCP_EVENT_CLIPBOARD; /* remote tekst klaar voor het Mac-klembord */

void rdpmcp_push_event(Uint32 type);

/* keymap.c: SDL-scancode naar RDP-scancode (0x100 = extended), 0 = onbekend */
uint32_t rdpmcp_sdl_to_rdp_scancode(SDL_Scancode sc, bool cmd_as_win);

/* clipboard.c */
enum
{
	CLIP_OWNER_NONE,
	CLIP_OWNER_LOCAL,  /* gebruiker kopieerde iets op de Mac */
	CLIP_OWNER_REMOTE, /* er werd iets gekopieerd in de remote sessie */
	CLIP_OWNER_AI      /* de AI zette tekst via CLIPSET */
};

enum
{
	CLIP_OK,
	CLIP_DISABLED,
	CLIP_LOCAL_PRIVATE,
	CLIP_NOT_READY
};

void rdpmcp_clip_init(mcpContext* mc);
void rdpmcp_clip_free(mcpContext* mc);
void rdpmcp_clip_connected(mcpContext* mc, struct s_cliprdr_client_context* cliprdr);
void rdpmcp_clip_disconnected(mcpContext* mc);
/* main-thread: het Mac-klembord is gewijzigd / remote tekst naar de Mac zetten */
void rdpmcp_clip_local_changed(mcpContext* mc);
void rdpmcp_clip_apply_remote(mcpContext* mc);
/* control-thread: lezen (wacht max. timeout_ms op de server) en zetten */
int rdpmcp_clip_get(mcpContext* mc, char** text, int timeout_ms);
int rdpmcp_clip_set_ai(mcpContext* mc, const char* text);

/* control.c */
bool rdpmcp_control_start(mcpContext* mc);
void rdpmcp_control_stop(mcpContext* mc);

#endif
