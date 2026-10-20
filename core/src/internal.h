#ifndef RDPMCP_INTERNAL_H
#define RDPMCP_INTERNAL_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>

#include <freerdp/client.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/freerdp.h>

#include "rdpmcp_core.h"

#define RDPMCP_MAX_CONNS 16

/* Gedeelde toestand van één sessie. Threads:
 *  - frontend: tekent en stuurt lokale invoer
 *  - rdp:      verbinden + FreeRDP-eventloop, schrijft de framebuffer
 *  - control:  Unix-socket voor de MCP-server */
struct rdpmcp_session
{
	rdpClientContext common; /* moet eerst staan: FreeRDP cast hiernaar */

	rdpmcp_callbacks cb;
	char name[256];
	char host[256];
	atomic_int state;
	pthread_t rdp_thread;
	bool rdp_thread_started;

	/* Kopie van het remote scherm in BGRX32, beschermd door fb_lock */
	pthread_mutex_t fb_lock;
	uint8_t* fb;
	int fb_w, fb_h;
	bool fb_resized;
	bool fb_has_dirty;
	int dirty_x1, dirty_y1, dirty_x2, dirty_y2;
	atomic_bool frame_pending;

	/* Alle freerdp_input_*-aanroepen gaan via deze lock */
	pthread_mutex_t input_lock;
	atomic_bool connected;
	bool key_down[512]; /* lokale toetsen, voor release_all */

	/* AI-besturing en wie het laatst iets deed (GetTickCount64) */
	atomic_bool ai_enabled;
	_Atomic uint64_t ai_last_ms;
	_Atomic uint64_t user_last_ms;
	atomic_int ai_x, ai_y;

	/* Dynamische resolutie */
	DispClientContext* disp;
	atomic_bool disp_ready;
	_Atomic uint32_t want_w, want_h; /* laatst gevraagde grootte, 0 = geen */

	/* Klembord (clipboard.c), beschermd door clip_lock */
	bool clipboard_enabled;
	CliprdrClientContext* cliprdr;
	pthread_mutex_t clip_lock;
	pthread_cond_t clip_cond;
	int clip_owner;         /* CLIP_OWNER_* */
	char* clip_local_text;  /* lokaal klembord (UTF-8, LF) */
	char* clip_remote_text; /* remote klembord (UTF-8, LF) */
	char* clip_ai_text;     /* door de AI gezet (UTF-8, LF) */
	uint8_t* clip_local_dib;  /* lokale afbeelding (CF_DIB) */
	size_t clip_local_dib_len;
	uint8_t* clip_remote_dib; /* remote afbeelding (CF_DIB) */
	size_t clip_remote_dib_len;
	bool clip_pending;        /* wachten op gegevens van de server */
	uint32_t clip_requested;  /* formaat van de lopende aanvraag, 0 = geen */
	bool clip_want_dib;       /* na de tekst ook de afbeelding ophalen */

	/* Control-socket */
	char sock_path[PATH_MAX];
	int listen_fd;
	pthread_t control_thread;

	/* Open clientverbindingen, zodat rdpmcp_control_stop op hun threads kan wachten */
	pthread_mutex_t conn_lock;
	pthread_cond_t conn_cond;
	int conn_fds[RDPMCP_MAX_CONNS];
	int conn_count;

	/* Door ai_guard geweigerde knoppen en toetsen: het loslaten ervan gaat ook niet door.
	 * Bits: GUARD_BIT_* (control.c) */
	atomic_uint ai_refused;
	/* Laatste toegelaten knop-neer van de AI, om het loslaten op dezelfde plek niet
	 * opnieuw te laten controleren */
	atomic_int ai_down_x, ai_down_y;
};

uint64_t rdpmcp_now_ms(void);

/* clipboard.c */
enum
{
	CLIP_OWNER_NONE,
	CLIP_OWNER_LOCAL,  /* gebruiker kopieerde iets lokaal */
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

void rdpmcp_clip_init(rdpmcp_session* s);
void rdpmcp_clip_free(rdpmcp_session* s);
void rdpmcp_clip_connected(rdpmcp_session* s, CliprdrClientContext* cliprdr);
void rdpmcp_clip_disconnected(rdpmcp_session* s);
int rdpmcp_clip_get(rdpmcp_session* s, char** text, int timeout_ms);
int rdpmcp_clip_set_ai(rdpmcp_session* s, const char* text);

/* control.c */
bool rdpmcp_control_start(rdpmcp_session* s);
void rdpmcp_control_stop(rdpmcp_session* s);
void rdpmcp_ai_touch(rdpmcp_session* s, int x, int y);

#endif
