/* rdp-mcp-client: eenvoudige SDL3-frontend voor één sessie, met de gewone
 * FreeRDP-opdrachtregelopties. Handig voor tests en op Linux; op de Mac is er
 * ook de app in mac/.
 *
 * Ctrl+Alt+Shift+A zet AI-besturing aan/uit. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keymap.h"
#include "rdpmcp_core.h"

#define AI_HIGHLIGHT_MS 1500

static Uint32 EV_FRAME;
static Uint32 EV_STATE;
static Uint32 EV_AI;
static Uint32 EV_CLIPBOARD;
static Uint32 EV_POINTER;

typedef struct
{
	rdpmcp_session* session;
	SDL_Window* window;
	SDL_Renderer* renderer;
	SDL_Texture* texture;
	int tex_w, tex_h;
	bool window_sized;
	bool cmd_as_win;
	const char* host;
	uint64_t ai_last;
	int ai_x, ai_y;
	SDL_Cursor* cursor;
} app_t;

typedef struct
{
	int w, h, hot_x, hot_y; /* w = 0: verbergen, w < 0: standaardcursor */
	uint8_t bgra[];
} pointer_msg;

/* ---------- callbacks (achtergrondthreads) ---------- */

static void push(Uint32 type, Sint32 code, void* data1, void* data2)
{
	SDL_Event ev = { 0 };
	ev.type = type;
	ev.user.code = code;
	ev.user.data1 = data1;
	ev.user.data2 = data2;
	SDL_PushEvent(&ev);
}

static void cb_frame(void* user)
{
	(void)user;
	push(EV_FRAME, 0, NULL, NULL);
}

static void cb_state(void* user, rdpmcp_state state, const char* message)
{
	(void)user;
	push(EV_STATE, (Sint32)state, message ? strdup(message) : NULL, NULL);
}

static void cb_ai(void* user, int x, int y)
{
	(void)user;
	push(EV_AI, 0, (void*)(intptr_t)x, (void*)(intptr_t)y);
}

static void cb_clipboard(void* user, const char* text)
{
	(void)user;
	push(EV_CLIPBOARD, 0, strdup(text), NULL);
}

static void cb_pointer(void* user, const uint8_t* bgra, int w, int h, int hot_x, int hot_y)
{
	(void)user;
	const size_t len = bgra ? (size_t)w * (size_t)h * 4 : 0;
	pointer_msg* m = malloc(sizeof(*m) + len);
	if (!m)
		return;
	*m = (pointer_msg){ bgra ? w : (w < 0 ? -1 : 0), h, hot_x, hot_y };
	if (len)
		memcpy(m->bgra, bgra, len);
	push(EV_POINTER, 0, m, NULL);
}

static void apply_pointer(app_t* a, pointer_msg* m)
{
	if (m->w == 0)
	{
		SDL_HideCursor();
		return;
	}
	SDL_Cursor* next = NULL;
	if (m->w > 0)
	{
		SDL_Surface* surface =
		    SDL_CreateSurfaceFrom(m->w, m->h, SDL_PIXELFORMAT_BGRA32, m->bgra, m->w * 4);
		if (surface)
		{
			next = SDL_CreateColorCursor(surface, m->hot_x, m->hot_y);
			SDL_DestroySurface(surface);
		}
	}
	SDL_SetCursor(next ? next : SDL_GetDefaultCursor());
	SDL_ShowCursor();
	if (a->cursor)
		SDL_DestroyCursor(a->cursor);
	a->cursor = next;
}

/* ---------- invoer en tekenen (main-thread) ---------- */

static void update_title(app_t* a)
{
	char title[512];
	snprintf(title, sizeof(title), "rdp-mcp — %s — AI control %s (Ctrl+Alt+Shift+A)", a->host,
	         rdpmcp_get_ai_enabled(a->session) ? "ON" : "OFF");
	SDL_SetWindowTitle(a->window, title);
}

static void handle_input(app_t* a, SDL_Event* ev)
{
	rdpmcp_session* s = a->session;
	switch (ev->type)
	{
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
		{
			const bool down = ev->type == SDL_EVENT_KEY_DOWN;
			const SDL_Keymod mod = ev->key.mod;
			if (down && ev->key.scancode == SDL_SCANCODE_A && (mod & SDL_KMOD_CTRL) &&
			    (mod & SDL_KMOD_ALT) && (mod & SDL_KMOD_SHIFT))
			{
				rdpmcp_set_ai_enabled(s, !rdpmcp_get_ai_enabled(s));
				update_title(a);
				printf("AI control %s\n", rdpmcp_get_ai_enabled(s) ? "on" : "off");
				fflush(stdout);
				return;
			}
			const uint32_t code = rdpmcp_sdl_to_rdp_scancode(ev->key.scancode, a->cmd_as_win);
			if (code)
				rdpmcp_input_key(s, code, down, down && ev->key.repeat);
			break;
		}

		case SDL_EVENT_MOUSE_MOTION:
			SDL_ConvertEventToRenderCoordinates(a->renderer, ev);
			rdpmcp_input_move(s, (int)ev->motion.x, (int)ev->motion.y);
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			SDL_ConvertEventToRenderCoordinates(a->renderer, ev);
			const bool down = ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			rdpmcp_button b;
			switch (ev->button.button)
			{
				case SDL_BUTTON_LEFT: b = RDPMCP_BUTTON_LEFT; break;
				case SDL_BUTTON_RIGHT: b = RDPMCP_BUTTON_RIGHT; break;
				case SDL_BUTTON_MIDDLE: b = RDPMCP_BUTTON_MIDDLE; break;
				case SDL_BUTTON_X1: b = RDPMCP_BUTTON_X1; break;
				case SDL_BUTTON_X2: b = RDPMCP_BUTTON_X2; break;
				default: return;
			}
			rdpmcp_input_button(s, b, down, (int)ev->button.x, (int)ev->button.y);
			break;
		}

		case SDL_EVENT_MOUSE_WHEEL:
		{
			SDL_ConvertEventToRenderCoordinates(a->renderer, ev);
			const int x = (int)ev->wheel.mouse_x;
			const int y = (int)ev->wheel.mouse_y;
			if (ev->wheel.integer_y)
				rdpmcp_input_wheel(s, false, ev->wheel.integer_y, x, y);
			if (ev->wheel.integer_x)
				rdpmcp_input_wheel(s, true, ev->wheel.integer_x, x, y);
			break;
		}

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		{
			const SDL_Keymod mod = SDL_GetModState();
			rdpmcp_input_focus(s, mod & SDL_KMOD_CAPS, mod & SDL_KMOD_NUM, mod & SDL_KMOD_SCROLL);
			break;
		}

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			rdpmcp_input_release_all(s);
			break;

		case SDL_EVENT_CLIPBOARD_UPDATE:
		{
			char* text = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
			rdpmcp_local_clipboard_changed(s, text);
			SDL_free(text);
			break;
		}

		default:
			break;
	}
}

static void fit_window(app_t* a, int w, int h)
{
	SDL_Rect usable;
	if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
	    (w > usable.w * 0.9 || h > usable.h * 0.9))
	{
		const double f = SDL_min(usable.w * 0.9 / w, usable.h * 0.9 / h);
		w = (int)(w * f);
		h = (int)(h * f);
	}
	SDL_SetWindowSize(a->window, w, h);
}

static void upload_frame(app_t* a)
{
	int w = 0;
	int h = 0;
	rdpmcp_rect dirty;
	bool resized = false;
	const uint8_t* fb = rdpmcp_frame_lock(a->session, &w, &h, &dirty, &resized);
	if (fb && w > 0 && h > 0)
	{
		if (!a->texture || w != a->tex_w || h != a->tex_h)
		{
			if (a->texture)
				SDL_DestroyTexture(a->texture);
			a->texture = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_XRGB8888,
			                               SDL_TEXTUREACCESS_STREAMING, w, h);
			a->tex_w = w;
			a->tex_h = h;
			SDL_SetRenderLogicalPresentation(a->renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
			dirty = (rdpmcp_rect){ 0, 0, w, h };
			if (!a->window_sized)
			{
				fit_window(a, w, h);
				a->window_sized = true;
			}
		}
		if (a->texture && dirty.w > 0 && dirty.h > 0)
		{
			const SDL_Rect r = { dirty.x, dirty.y, dirty.w, dirty.h };
			SDL_UpdateTexture(a->texture, &r, fb + ((size_t)r.y * w + r.x) * 4, w * 4);
		}
	}
	rdpmcp_frame_unlock(a->session);
}

static void fill_border(SDL_Renderer* r, int w, int h, int t)
{
	const SDL_FRect rects[4] = {
		{ 0, 0, (float)w, (float)t },
		{ 0, (float)(h - t), (float)w, (float)t },
		{ 0, 0, (float)t, (float)h },
		{ (float)(w - t), 0, (float)t, (float)h },
	};
	SDL_RenderFillRects(r, rects, 4);
}

/* Geeft true terug als er een AI-markering getekend werd (dan later opnieuw tekenen) */
static bool render(app_t* a)
{
	SDL_SetRenderDrawColor(a->renderer, 0, 0, 0, 255);
	SDL_RenderClear(a->renderer);
	if (!a->texture)
	{
		SDL_RenderPresent(a->renderer);
		return false;
	}
	SDL_RenderTexture(a->renderer, a->texture, NULL, NULL);

	const bool highlight = a->ai_last && SDL_GetTicks() - a->ai_last < AI_HIGHLIGHT_MS;
	if (highlight)
	{
		SDL_SetRenderDrawColor(a->renderer, 255, 140, 0, 255);
		fill_border(a->renderer, a->tex_w, a->tex_h, a->tex_w > 1600 ? 6 : 4);
		/* kruisje op de laatste AI-muispositie */
		const float ax = (float)a->ai_x;
		const float ay = (float)a->ai_y;
		const SDL_FRect cross[2] = { { ax - 12, ay - 2, 24, 4 }, { ax - 2, ay - 12, 4, 24 } };
		SDL_RenderFillRects(a->renderer, cross, 2);
	}
	SDL_RenderPresent(a->renderer);
	return highlight;
}

/* ---------- main ---------- */

static void usage_extra(void)
{
	printf("\nrdp-mcp options (on top of the usual FreeRDP options):\n"
	       "  --ai-off       start with AI control disabled\n"
	       "  --cmd-as-win   Cmd key = Windows key (default: Cmd = Ctrl)\n"
	       "  --no-clipboard don't share the clipboard (not even with the AI)\n"
	       "\nExample: rdp-mcp-client /v:server.example.com /u:jane /d:EXAMPLE\n"
	       "Shortcut in the window: Ctrl+Alt+Shift+A turns AI control on/off.\n");
}

int main(int argc, char* argv[])
{
	app_t a = { 0 };
	bool ai_off = false;
	bool no_clipboard = false;

	/* eigen opties eruit filteren, de rest gaat naar de FreeRDP-parser */
	char** args = calloc((size_t)argc + 1, sizeof(char*));
	int nargs = 0;
	for (int i = 0; i < argc; i++)
	{
		if (strcmp(argv[i], "--ai-off") == 0)
			ai_off = true;
		else if (strcmp(argv[i], "--cmd-as-win") == 0)
			a.cmd_as_win = true;
		else if (strcmp(argv[i], "--no-clipboard") == 0)
			no_clipboard = true;
		else
			args[nargs++] = argv[i];
	}

	const rdpmcp_callbacks cb = {
		.user = &a,
		.frame = cb_frame,
		.state = cb_state,
		.ai_activity = cb_ai,
		.remote_clipboard = cb_clipboard,
		.pointer = cb_pointer,
		/* certificaat en wachtwoord: in de terminal */
	};

	int rc = 1;
	a.session = rdpmcp_session_new_from_args(nargs, args, !no_clipboard, !ai_off, &cb, &rc);
	if (!a.session)
	{
		if (rc == -1)
		{
			usage_extra();
			rc = 0;
		}
		free(args);
		return rc;
	}
	for (int i = 1; i < nargs; i++)
		if (strncmp(args[i], "/v:", 3) == 0)
			a.host = args[i] + 3;
	if (!a.host)
		a.host = "?";

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		goto out;
	}
	const Uint32 base = SDL_RegisterEvents(5);
	EV_FRAME = base;
	EV_STATE = base + 1;
	EV_AI = base + 2;
	EV_CLIPBOARD = base + 3;
	EV_POINTER = base + 4;

	a.window = SDL_CreateWindow("rdp-mcp", 1024, 768,
	                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	a.renderer = a.window ? SDL_CreateRenderer(a.window, NULL) : NULL;
	if (!a.renderer)
	{
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		goto out_sdl;
	}
	SDL_SetRenderVSync(a.renderer, 1);
	update_title(&a);

	if (!rdpmcp_session_start(a.session))
		goto out_sdl;
	{
		char* text = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
		rdpmcp_local_clipboard_changed(a.session, text); /* huidige inhoud aanbieden */
		SDL_free(text);
	}
	if (rdpmcp_session_socket_path(a.session))
		printf("MCP control socket: %s\n", rdpmcp_session_socket_path(a.session));
	fflush(stdout);

	rc = 0;
	bool running = true;
	bool needs_render = true;
	bool highlight = false;
	while (running)
	{
		SDL_Event ev;
		if (SDL_WaitEventTimeout(&ev, highlight ? 30 : 250))
		{
			do
			{
				if (ev.type == SDL_EVENT_QUIT)
					running = false;
				else if (ev.type == EV_FRAME)
				{
					upload_frame(&a);
					needs_render = true;
				}
				else if (ev.type == EV_STATE)
				{
					char* msg = ev.user.data1;
					if (ev.user.code == RDPMCP_STATE_FAILED)
					{
						fprintf(stderr, "Connection failed or lost: %s\n", msg ? msg : "?");
						rc = 1;
						running = false;
					}
					else if (ev.user.code == RDPMCP_STATE_DISCONNECTED)
					{
						if (msg)
							printf("Connection closed: %s\n", msg);
						running = false;
					}
					free(msg);
				}
				else if (ev.type == EV_AI)
				{
					a.ai_last = SDL_GetTicks();
					if ((intptr_t)ev.user.data1 >= 0)
					{
						a.ai_x = (int)(intptr_t)ev.user.data1;
						a.ai_y = (int)(intptr_t)ev.user.data2;
					}
					needs_render = true;
				}
				else if (ev.type == EV_POINTER)
				{
					apply_pointer(&a, ev.user.data1);
					free(ev.user.data1);
				}
				else if (ev.type == EV_CLIPBOARD)
				{
					SDL_SetClipboardText(ev.user.data1);
					free(ev.user.data1);
				}
				else if (ev.type == SDL_EVENT_WINDOW_EXPOSED ||
				         ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
					needs_render = true;
				else
					handle_input(&a, &ev);
			} while (running && SDL_PollEvent(&ev));
		}
		if (running && (needs_render || highlight))
		{
			highlight = render(&a);
			needs_render = false;
		}
	}

out_sdl:
	rdpmcp_session_free(a.session);
	a.session = NULL;
	if (a.cursor)
		SDL_DestroyCursor(a.cursor);
	if (a.texture)
		SDL_DestroyTexture(a.texture);
	if (a.renderer)
		SDL_DestroyRenderer(a.renderer);
	if (a.window)
		SDL_DestroyWindow(a.window);
	SDL_Quit();
out:
	if (a.session)
		rdpmcp_session_free(a.session);
	free(args);
	return rc;
}
