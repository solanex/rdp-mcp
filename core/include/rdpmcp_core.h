/* rdpmcp-core: RDP-sessies op basis van FreeRDP, met een control-socket per
 * sessie waarlangs een MCP-server meekijkt en bestuurt.
 *
 * Een frontend (SDL-client, Mac-app) maakt sessies aan, tekent de framebuffer
 * en geeft lokale muis/toetsenbord door. Alle callbacks komen van
 * achtergrondthreads; de frontend moet zelf naar zijn UI-thread springen. */

#ifndef RDPMCP_CORE_H
#define RDPMCP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct rdpmcp_session rdpmcp_session;

	typedef enum
	{
		RDPMCP_STATE_CONNECTING = 0,
		RDPMCP_STATE_CONNECTED = 1,
		RDPMCP_STATE_DISCONNECTED = 2, /* netjes afgesloten */
		RDPMCP_STATE_FAILED = 3        /* verbinden mislukt of verbinding verbroken */
	} rdpmcp_state;

	typedef enum
	{
		RDPMCP_CERT_REJECT = 0,
		RDPMCP_CERT_ACCEPT_ALWAYS = 1, /* FreeRDP onthoudt het certificaat */
		RDPMCP_CERT_ACCEPT_ONCE = 2
	} rdpmcp_cert_decision;

	typedef enum
	{
		RDPMCP_BUTTON_LEFT = 0,
		RDPMCP_BUTTON_RIGHT = 1,
		RDPMCP_BUTTON_MIDDLE = 2,
		RDPMCP_BUTTON_X1 = 3,
		RDPMCP_BUTTON_X2 = 4
	} rdpmcp_button;

	typedef struct
	{
		int x, y, w, h;
	} rdpmcp_rect;

	typedef struct
	{
		void* user;

		/* Er is nieuw beeld; haal het op met rdpmcp_frame_lock. Wordt samengevoegd:
		 * na een melding komt er pas een nieuwe na de volgende rdpmcp_frame_lock. */
		void (*frame)(void* user);
		/* Statuswijziging; message is een leesbare uitleg of NULL */
		void (*state)(void* user, rdpmcp_state state, const char* message);
		/* De AI deed iets; x < 0 betekent zonder muispositie (bv. een toets) */
		void (*ai_activity)(void* user, int x, int y);
		/* Tekst gekopieerd in de remote sessie: op het lokale klembord zetten */
		void (*remote_clipboard)(void* user, const char* text);
		/* Korte melding voor de gebruiker (bv. geweigerde AI-plakactie). NULL = stdout */
		void (*notice)(void* user, const char* message);

		/* Blokkerende vragen aan de gebruiker. NULL = vragen in de terminal. */
		rdpmcp_cert_decision (*verify_certificate)(void* user, const char* host, uint16_t port,
		                                           const char* common_name, const char* subject,
		                                           const char* issuer, const char* fingerprint,
		                                           bool changed);
		/* Vul ontbrekende gegevens in (malloc/strdup; de kern geeft ze vrij).
		 * false = de gebruiker annuleerde. */
		bool (*authenticate)(void* user, char** username, char** password, char** domain);
	} rdpmcp_callbacks;

	typedef struct
	{
		const char* name;     /* naam uit het adresboek; zichtbaar voor de AI */
		const char* host;
		uint16_t port;        /* 0 = 3389 */
		const char* username; /* NULL = vragen */
		const char* domain;
		const char* password; /* NULL = vragen; wordt meteen gekopieerd */
		const char* gateway;  /* RD Gateway, NULL = geen */
		uint32_t width;       /* 0 = 1024 */
		uint32_t height;      /* 0 = 768 */
		uint32_t keyboard_layout; /* bv. 0x80C (Belgisch), 0 = standaard */
		bool ignore_certificate;
		bool dynamic_resolution; /* remote resolutie volgt rdpmcp_request_resize */
		bool clipboard;
		bool ai_enabled;
	} rdpmcp_config;

	/* Sessie op basis van een configuratie (Mac-app) */
	rdpmcp_session* rdpmcp_session_new(const rdpmcp_config* config,
	                                   const rdpmcp_callbacks* callbacks);

	/* Sessie op basis van FreeRDP-opdrachtregelopties (SDL-client). Bij /help of een
	 * fout wordt NULL teruggegeven en staat de exitcode in *exit_code. */
	rdpmcp_session* rdpmcp_session_new_from_args(int argc, char** argv, bool clipboard,
	                                             bool ai_enabled,
	                                             const rdpmcp_callbacks* callbacks,
	                                             int* exit_code);

	/* Verbindt in een eigen thread en opent de control-socket */
	bool rdpmcp_session_start(rdpmcp_session* s);
	/* Vraagt de verbinding te sluiten; keert meteen terug */
	void rdpmcp_session_disconnect(rdpmcp_session* s);
	/* Sluit af, wacht op de threads en geeft alles vrij */
	void rdpmcp_session_free(rdpmcp_session* s);

	rdpmcp_state rdpmcp_session_state(rdpmcp_session* s);
	const char* rdpmcp_session_socket_path(rdpmcp_session* s);

	/* Framebuffer in BGRX32 (stride = breedte * 4). dirty krijgt het gewijzigde
	 * gebied sinds de vorige lock (w = 0: niets), resized of de grootte veranderde.
	 * Altijd afsluiten met rdpmcp_frame_unlock. Kan NULL teruggeven. */
	const uint8_t* rdpmcp_frame_lock(rdpmcp_session* s, int* width, int* height,
	                                 rdpmcp_rect* dirty, bool* resized);
	void rdpmcp_frame_unlock(rdpmcp_session* s);

	/* Lokale invoer van de gebruiker (remote coördinaten, RDP-scancodes set 1,
	 * 0x100 = extended) */
	void rdpmcp_input_key(rdpmcp_session* s, uint32_t scancode, bool down, bool repeat);
	void rdpmcp_input_move(rdpmcp_session* s, int x, int y);
	void rdpmcp_input_button(rdpmcp_session* s, rdpmcp_button button, bool down, int x, int y);
	/* notches: positief = omhoog / naar rechts */
	void rdpmcp_input_wheel(rdpmcp_session* s, bool horizontal, int notches, int x, int y);
	void rdpmcp_input_focus(rdpmcp_session* s, bool caps_lock, bool num_lock, bool scroll_lock);
	/* Alle ingedrukte toetsen loslaten (bij focusverlies) */
	void rdpmcp_input_release_all(rdpmcp_session* s);

	void rdpmcp_set_ai_enabled(rdpmcp_session* s, bool enabled);
	bool rdpmcp_get_ai_enabled(rdpmcp_session* s);

	/* Het lokale klembord veranderde (text = NULL: geen tekst) */
	void rdpmcp_local_clipboard_changed(rdpmcp_session* s, const char* text);

	/* Vraag de server om deze resolutie (enkel met dynamic_resolution) */
	void rdpmcp_request_resize(rdpmcp_session* s, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif
