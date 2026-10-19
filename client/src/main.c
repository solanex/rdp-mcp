/* rdp-mcp-client: kleine RDP-client op basis van FreeRDP + SDL3.
 *
 * De gebruiker ziet en bedient de sessie in een gewoon venster. Via een
 * Unix-socket (control.c) kan een MCP-server tegelijk screenshots nemen en
 * muis-/toetsenbordinvoer sturen. Ctrl+Alt+Shift+A zet AI-besturing aan/uit. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/client/cmdline.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <winpr/synch.h>

#include "rdpmcp.h"

Uint32 RDPMCP_EVENT_PAINT;
Uint32 RDPMCP_EVENT_RESIZE;
Uint32 RDPMCP_EVENT_DISCONNECTED;
Uint32 RDPMCP_EVENT_AI;

#define AI_HIGHLIGHT_MS 1500

void rdpmcp_push_event(Uint32 type)
{
	SDL_Event ev = { 0 };
	ev.type = type;
	SDL_PushEvent(&ev);
}

/* ---------- framebuffer (rdp-thread) ---------- */

/* Kopieert een rechthoek uit de FreeRDP-GDI-buffer naar onze eigen buffer.
 * Bij een andere resolutie wordt de buffer opnieuw aangemaakt en volledig gevuld. */
static void fb_update(mcpContext* mc, int x, int y, int w, int h)
{
	rdpGdi* gdi = mc->common.context.gdi;
	const int gw = (int)gdi->width;
	const int gh = (int)gdi->height;

	pthread_mutex_lock(&mc->fb_lock);
	if (!mc->fb || mc->fb_w != gw || mc->fb_h != gh)
	{
		uint8_t* fb = realloc(mc->fb, (size_t)gw * gh * 4);
		if (!fb)
		{
			pthread_mutex_unlock(&mc->fb_lock);
			return;
		}
		mc->fb = fb;
		mc->fb_w = gw;
		mc->fb_h = gh;
		mc->fb_resized = true;
		x = 0;
		y = 0;
		w = gw;
		h = gh;
	}

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > gw) w = gw - x;
	if (y + h > gh) h = gh - y;

	if (w > 0 && h > 0)
	{
		for (int row = y; row < y + h; row++)
			memcpy(mc->fb + ((size_t)row * gw + x) * 4,
			       gdi->primary_buffer + (size_t)row * gdi->stride + (size_t)x * 4, (size_t)w * 4);

		if (!mc->fb_has_dirty)
		{
			mc->dirty_x1 = x;
			mc->dirty_y1 = y;
			mc->dirty_x2 = x + w;
			mc->dirty_y2 = y + h;
			mc->fb_has_dirty = true;
		}
		else
		{
			if (x < mc->dirty_x1) mc->dirty_x1 = x;
			if (y < mc->dirty_y1) mc->dirty_y1 = y;
			if (x + w > mc->dirty_x2) mc->dirty_x2 = x + w;
			if (y + h > mc->dirty_y2) mc->dirty_y2 = y + h;
		}
	}
	pthread_mutex_unlock(&mc->fb_lock);

	if (!atomic_exchange(&mc->paint_pending, true))
		rdpmcp_push_event(RDPMCP_EVENT_PAINT);
}

static BOOL mcp_begin_paint(rdpContext* context)
{
	HGDI_WND hwnd = context->gdi->primary->hdc->hwnd;
	hwnd->invalid->null = TRUE;
	hwnd->ninvalid = 0;
	return TRUE;
}

static BOOL mcp_end_paint(rdpContext* context)
{
	mcpContext* mc = (mcpContext*)context;
	HGDI_WND hwnd = context->gdi->primary->hdc->hwnd;

	if (hwnd->invalid->null)
		return TRUE;

	fb_update(mc, hwnd->invalid->x, hwnd->invalid->y, hwnd->invalid->w, hwnd->invalid->h);
	hwnd->invalid->null = TRUE;
	hwnd->ninvalid = 0;
	return TRUE;
}

static BOOL mcp_desktop_resize(rdpContext* context)
{
	mcpContext* mc = (mcpContext*)context;
	rdpSettings* settings = context->settings;

	if (!gdi_resize(context->gdi, freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
	                freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)))
		return FALSE;

	fb_update(mc, 0, 0, (int)context->gdi->width, (int)context->gdi->height);
	rdpmcp_push_event(RDPMCP_EVENT_RESIZE);
	return TRUE;
}

/* ---------- FreeRDP-callbacks ---------- */

static BOOL mcp_pre_connect(freerdp* instance)
{
	rdpContext* context = instance->context;
	rdpSettings* settings = context->settings;

	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_MACINTOSH))
		return FALSE;
	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_MACINTOSH))
		return FALSE;
	/* Unicode-invoer aanvragen; FreeRDP zet dit na de capability-uitwisseling terug op wat
	 * de server ondersteunt. Zonder dit staat het altijd uit en kan type_text geen é of € typen. */
	if (!freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, TRUE))
		return FALSE;

	/* Standaardhandlers van FreeRDP koppelen o.a. de grafische pipeline (/gfx) aan GDI */
	PubSub_SubscribeChannelConnected(context->pubSub, freerdp_client_OnChannelConnectedEventHandler);
	PubSub_SubscribeChannelDisconnected(context->pubSub,
	                                    freerdp_client_OnChannelDisconnectedEventHandler);
	return TRUE;
}

static BOOL mcp_post_connect(freerdp* instance)
{
	rdpContext* context = instance->context;
	mcpContext* mc = (mcpContext*)context;

	if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
		return FALSE;

	context->update->BeginPaint = mcp_begin_paint;
	context->update->EndPaint = mcp_end_paint;
	context->update->DesktopResize = mcp_desktop_resize;

	const char* host = freerdp_settings_get_string(context->settings, FreeRDP_ServerHostname);
	snprintf(mc->host, sizeof(mc->host), "%s", host ? host : "?");

	fb_update(mc, 0, 0, (int)context->gdi->width, (int)context->gdi->height);
	atomic_store(&mc->connected, true);
	return TRUE;
}

static void mcp_post_disconnect(freerdp* instance)
{
	mcpContext* mc = (mcpContext*)instance->context;
	atomic_store(&mc->connected, false);
	gdi_free(instance);
}

static BOOL mcp_client_new(freerdp* instance, rdpContext* context)
{
	mcpContext* mc = (mcpContext*)context;

	instance->PreConnect = mcp_pre_connect;
	instance->PostConnect = mcp_post_connect;
	instance->PostDisconnect = mcp_post_disconnect;
	/* Wachtwoord en certificaat worden in de terminal gevraagd, nooit via MCP */
	instance->AuthenticateEx = client_cli_authenticate_ex;
	instance->VerifyCertificateEx = client_cli_verify_certificate_ex;
	instance->VerifyChangedCertificateEx = client_cli_verify_changed_certificate_ex;
	instance->PresentGatewayMessage = client_cli_present_gateway_message;
	instance->LogonErrorInfo = client_cli_logon_error_info;
	instance->ChooseSmartcard = client_cli_choose_smartcard;
	instance->GetAccessToken = client_cli_get_access_token;

	pthread_mutex_init(&mc->fb_lock, NULL);
	pthread_mutex_init(&mc->input_lock, NULL);
	atomic_store(&mc->ai_enabled, true);
	mc->listen_fd = -1;
	return TRUE;
}

static void mcp_client_free(freerdp* instance, rdpContext* context)
{
	mcpContext* mc = (mcpContext*)context;
	WINPR_UNUSED(instance);

	free(mc->fb);
	mc->fb = NULL;
	pthread_mutex_destroy(&mc->fb_lock);
	pthread_mutex_destroy(&mc->input_lock);
}

static int RdpClientEntry(RDP_CLIENT_ENTRY_POINTS* ep)
{
	memset(ep, 0, sizeof(*ep));
	ep->Version = RDP_CLIENT_INTERFACE_VERSION;
	ep->Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	ep->ContextSize = sizeof(mcpContext);
	ep->ClientNew = mcp_client_new;
	ep->ClientFree = mcp_client_free;
	return 0;
}

/* ---------- rdp-thread ---------- */

static void* rdp_thread(void* arg)
{
	mcpContext* mc = arg;
	rdpContext* context = &mc->common.context;

	while (!freerdp_shall_disconnect_context(context))
	{
		HANDLE handles[MAXIMUM_WAIT_OBJECTS] = { 0 };
		const DWORD count = freerdp_get_event_handles(context, handles, ARRAYSIZE(handles));
		if (count == 0)
			break;

		if (WaitForMultipleObjects(count, handles, FALSE, 100) == WAIT_FAILED)
			break;

		if (!freerdp_check_event_handles(context))
		{
			if (client_auto_reconnect_ex(context->instance, NULL))
				continue;
			break;
		}
	}

	atomic_store(&mc->connected, false);
	rdpmcp_push_event(RDPMCP_EVENT_DISCONNECTED);
	return NULL;
}

/* ---------- lokale invoer (main-thread) ---------- */

static bool key_down_state[512];

static void send_key(mcpContext* mc, uint32_t code, bool down, bool repeat)
{
	if (!atomic_load(&mc->connected))
		return;
	pthread_mutex_lock(&mc->input_lock);
	freerdp_input_send_keyboard_event_ex(mc->common.context.input, down, repeat, code);
	pthread_mutex_unlock(&mc->input_lock);
	if (code < 512)
		key_down_state[code] = down;
}

/* Bij focusverlies (bv. Cmd+Tab) alle ingedrukte toetsen loslaten, anders blijven ze hangen */
static void release_all_keys(mcpContext* mc)
{
	for (uint32_t code = 0; code < 512; code++)
		if (key_down_state[code])
			send_key(mc, code, false, false);
}

static void sync_lock_keys(mcpContext* mc)
{
	if (!atomic_load(&mc->connected))
		return;
	const SDL_Keymod mod = SDL_GetModState();
	UINT16 flags = 0;
	if (mod & SDL_KMOD_CAPS)
		flags |= KBD_SYNC_CAPS_LOCK;
	if (mod & SDL_KMOD_NUM)
		flags |= KBD_SYNC_NUM_LOCK;
	if (mod & SDL_KMOD_SCROLL)
		flags |= KBD_SYNC_SCROLL_LOCK;
	pthread_mutex_lock(&mc->input_lock);
	freerdp_input_send_focus_in_event(mc->common.context.input, flags);
	pthread_mutex_unlock(&mc->input_lock);
}

static void send_mouse(mcpContext* mc, UINT16 flags, float fx, float fy)
{
	if (!atomic_load(&mc->connected) || mc->fb_w <= 0)
		return;
	int x = (int)fx;
	int y = (int)fy;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= mc->fb_w) x = mc->fb_w - 1;
	if (y >= mc->fb_h) y = mc->fb_h - 1;
	pthread_mutex_lock(&mc->input_lock);
	freerdp_input_send_mouse_event(mc->common.context.input, flags, (UINT16)x, (UINT16)y);
	pthread_mutex_unlock(&mc->input_lock);
}

static void send_xmouse(mcpContext* mc, UINT16 flags, float fx, float fy)
{
	if (!atomic_load(&mc->connected))
		return;
	pthread_mutex_lock(&mc->input_lock);
	freerdp_input_send_extended_mouse_event(mc->common.context.input, flags, (UINT16)fx,
	                                        (UINT16)fy);
	pthread_mutex_unlock(&mc->input_lock);
}

static void update_title(mcpContext* mc)
{
	char title[512];
	snprintf(title, sizeof(title), "rdp-mcp — %s — AI-besturing %s (Ctrl+Alt+Shift+A)",
	         mc->host[0] ? mc->host : "verbinden…",
	         atomic_load(&mc->ai_enabled) ? "AAN" : "UIT");
	SDL_SetWindowTitle(mc->window, title);
}

static void handle_event(mcpContext* mc, SDL_Event* ev, bool* running)
{
	if (ev->type == SDL_EVENT_QUIT || ev->type == RDPMCP_EVENT_DISCONNECTED)
	{
		*running = false;
		return;
	}

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
				atomic_store(&mc->ai_enabled, !atomic_load(&mc->ai_enabled));
				update_title(mc);
				printf("AI-besturing %s\n", atomic_load(&mc->ai_enabled) ? "aan" : "uit");
				fflush(stdout);
				return;
			}
			const uint32_t code = rdpmcp_sdl_to_rdp_scancode(ev->key.scancode, mc->cmd_as_win);
			if (code)
				send_key(mc, code, down, down && ev->key.repeat);
			break;
		}

		case SDL_EVENT_MOUSE_MOTION:
			SDL_ConvertEventToRenderCoordinates(mc->renderer, ev);
			send_mouse(mc, PTR_FLAGS_MOVE, ev->motion.x, ev->motion.y);
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			SDL_ConvertEventToRenderCoordinates(mc->renderer, ev);
			const bool down = ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			const float x = ev->button.x;
			const float y = ev->button.y;
			switch (ev->button.button)
			{
				case SDL_BUTTON_LEFT:
					send_mouse(mc, PTR_FLAGS_BUTTON1 | (down ? PTR_FLAGS_DOWN : 0), x, y);
					break;
				case SDL_BUTTON_RIGHT:
					send_mouse(mc, PTR_FLAGS_BUTTON2 | (down ? PTR_FLAGS_DOWN : 0), x, y);
					break;
				case SDL_BUTTON_MIDDLE:
					send_mouse(mc, PTR_FLAGS_BUTTON3 | (down ? PTR_FLAGS_DOWN : 0), x, y);
					break;
				case SDL_BUTTON_X1:
					send_xmouse(mc, PTR_XFLAGS_BUTTON1 | (down ? PTR_XFLAGS_DOWN : 0), x, y);
					break;
				case SDL_BUTTON_X2:
					send_xmouse(mc, PTR_XFLAGS_BUTTON2 | (down ? PTR_XFLAGS_DOWN : 0), x, y);
					break;
				default:
					break;
			}
			break;
		}

		case SDL_EVENT_MOUSE_WHEEL:
		{
			SDL_ConvertEventToRenderCoordinates(mc->renderer, ev);
			const float x = ev->wheel.mouse_x;
			const float y = ev->wheel.mouse_y;
			int dy = ev->wheel.integer_y;
			int dx = ev->wheel.integer_x;
			for (; dy > 0; dy--)
				send_mouse(mc, PTR_FLAGS_WHEEL | 0x0078, x, y);
			for (; dy < 0; dy++)
				send_mouse(mc, PTR_FLAGS_WHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088, x, y);
			for (; dx > 0; dx--)
				send_mouse(mc, PTR_FLAGS_HWHEEL | 0x0078, x, y);
			for (; dx < 0; dx++)
				send_mouse(mc, PTR_FLAGS_HWHEEL | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088, x, y);
			break;
		}

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			sync_lock_keys(mc);
			break;

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			release_all_keys(mc);
			break;

		default:
			break;
	}
}

/* ---------- tekenen (main-thread) ---------- */

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
static bool render(mcpContext* mc)
{
	pthread_mutex_lock(&mc->fb_lock);
	atomic_store(&mc->paint_pending, false);
	if (mc->fb && (mc->fb_resized || !mc->texture))
	{
		if (mc->texture)
			SDL_DestroyTexture(mc->texture);
		mc->texture = SDL_CreateTexture(mc->renderer, SDL_PIXELFORMAT_XRGB8888,
		                                SDL_TEXTUREACCESS_STREAMING, mc->fb_w, mc->fb_h);
		SDL_SetRenderLogicalPresentation(mc->renderer, mc->fb_w, mc->fb_h,
		                                 SDL_LOGICAL_PRESENTATION_LETTERBOX);
		if (mc->texture)
			SDL_UpdateTexture(mc->texture, NULL, mc->fb, mc->fb_w * 4);
		mc->fb_resized = false;
		mc->fb_has_dirty = false;
	}
	else if (mc->texture && mc->fb_has_dirty)
	{
		const SDL_Rect r = { mc->dirty_x1, mc->dirty_y1, mc->dirty_x2 - mc->dirty_x1,
			                 mc->dirty_y2 - mc->dirty_y1 };
		SDL_UpdateTexture(mc->texture, &r,
		                  mc->fb + ((size_t)r.y * mc->fb_w + r.x) * 4, mc->fb_w * 4);
		mc->fb_has_dirty = false;
	}
	const int w = mc->fb_w;
	const int h = mc->fb_h;
	pthread_mutex_unlock(&mc->fb_lock);

	SDL_SetRenderDrawColor(mc->renderer, 0, 0, 0, 255);
	SDL_RenderClear(mc->renderer);
	if (mc->texture)
		SDL_RenderTexture(mc->renderer, mc->texture, NULL, NULL);

	bool highlight = false;
	const uint64_t last = atomic_load(&mc->ai_last_ms);
	if (mc->texture && last && SDL_GetTicks() - last < AI_HIGHLIGHT_MS)
	{
		highlight = true;
		const int t = w > 1600 ? 6 : 4;
		SDL_SetRenderDrawColor(mc->renderer, 255, 140, 0, 255);
		fill_border(mc->renderer, w, h, t);

		/* kruisje op de laatste AI-muispositie */
		const float ax = (float)atomic_load(&mc->ai_x);
		const float ay = (float)atomic_load(&mc->ai_y);
		const SDL_FRect cross[2] = { { ax - 12, ay - 2, 24, 4 }, { ax - 2, ay - 12, 4, 24 } };
		SDL_RenderFillRects(mc->renderer, cross, 2);
	}

	SDL_RenderPresent(mc->renderer);
	return highlight;
}

/* ---------- main ---------- */

static void usage_extra(void)
{
	printf("\nrdp-mcp-opties (bovenop de gewone FreeRDP-opties):\n"
	       "  --ai-off       start met AI-besturing uitgeschakeld\n"
	       "  --cmd-as-win   Cmd-toets = Windows-toets (standaard: Cmd = Ctrl)\n"
	       "\nVoorbeeld: rdp-mcp-client /v:server.example.be /u:jan /d:DOMEIN\n"
	       "Sneltoets in het venster: Ctrl+Alt+Shift+A zet AI-besturing aan/uit.\n");
}

int main(int argc, char* argv[])
{
	int rc = 1;
	bool ai_off = false;
	bool cmd_as_win = false;

	/* eigen opties eruit filteren, de rest gaat naar de FreeRDP-parser */
	char** args = calloc((size_t)argc + 1, sizeof(char*));
	int nargs = 0;
	for (int i = 0; i < argc; i++)
	{
		if (strcmp(argv[i], "--ai-off") == 0)
			ai_off = true;
		else if (strcmp(argv[i], "--cmd-as-win") == 0)
			cmd_as_win = true;
		else
			args[nargs++] = argv[i];
	}

	RDP_CLIENT_ENTRY_POINTS ep;
	RdpClientEntry(&ep);
	rdpContext* context = freerdp_client_context_new(&ep);
	if (!context)
		goto out_args;
	mcpContext* mc = (mcpContext*)context;
	mc->cmd_as_win = cmd_as_win;
	if (ai_off)
		atomic_store(&mc->ai_enabled, false);

	const int status =
	    freerdp_client_settings_parse_command_line(context->settings, nargs, args, FALSE);
	if (status)
	{
		rc = freerdp_client_settings_command_line_status_print(context->settings, status, nargs,
		                                                       args);
		if (status == COMMAND_LINE_STATUS_PRINT_HELP)
			usage_extra();
		goto out_context;
	}

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		goto out_context;
	}
	const Uint32 base = SDL_RegisterEvents(4);
	RDPMCP_EVENT_PAINT = base;
	RDPMCP_EVENT_RESIZE = base + 1;
	RDPMCP_EVENT_DISCONNECTED = base + 2;
	RDPMCP_EVENT_AI = base + 3;

	/* Verbinden vóór het venster opent, zodat wachtwoordvragen in de terminal rustig kunnen */
	if (!freerdp_connect(context->instance))
	{
		fprintf(stderr, "Verbinden mislukt: %s\n",
		        freerdp_get_last_error_string(freerdp_get_last_error(context)));
		goto out_sdl;
	}

	int win_w = mc->fb_w > 0 ? mc->fb_w : 1024;
	int win_h = mc->fb_h > 0 ? mc->fb_h : 768;
	const SDL_Rect* usable = NULL;
	SDL_Rect bounds;
	if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds))
		usable = &bounds;
	if (usable && (win_w > usable->w * 0.9 || win_h > usable->h * 0.9))
	{
		const double s = SDL_min(usable->w * 0.9 / win_w, usable->h * 0.9 / win_h);
		win_w = (int)(win_w * s);
		win_h = (int)(win_h * s);
	}

	mc->window = SDL_CreateWindow("rdp-mcp", win_w, win_h,
	                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if (!mc->window)
	{
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		goto out_disconnect;
	}
	mc->renderer = SDL_CreateRenderer(mc->window, NULL);
	if (!mc->renderer)
	{
		fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		goto out_disconnect;
	}
	SDL_SetRenderVSync(mc->renderer, 1);
	update_title(mc);

	if (!rdpmcp_control_start(mc))
		fprintf(stderr, "Waarschuwing: control-socket niet gestart, MCP werkt niet\n");
	else
		printf("MCP-control-socket: %s\n", mc->sock_path);
	fflush(stdout);

	pthread_t rdp;
	if (pthread_create(&rdp, NULL, rdp_thread, mc) != 0)
		goto out_control;

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
				if (ev.type == RDPMCP_EVENT_PAINT || ev.type == RDPMCP_EVENT_RESIZE ||
				    ev.type == RDPMCP_EVENT_AI || ev.type == SDL_EVENT_WINDOW_EXPOSED ||
				    ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
					needs_render = true;
				else
					handle_event(mc, &ev, &running);
			} while (running && SDL_PollEvent(&ev));
		}
		if (running && (needs_render || highlight))
		{
			highlight = render(mc);
			needs_render = false;
		}
	}

	freerdp_abort_connect_context(context);
	pthread_join(rdp, NULL);
	rc = 0;

out_control:
	rdpmcp_control_stop(mc);
out_disconnect:
	freerdp_disconnect(context->instance);
	if (mc->texture)
		SDL_DestroyTexture(mc->texture);
	if (mc->renderer)
		SDL_DestroyRenderer(mc->renderer);
	if (mc->window)
		SDL_DestroyWindow(mc->window);
out_sdl:
	SDL_Quit();
out_context:
	freerdp_client_context_free(context);
out_args:
	free(args);
	return rc;
}
