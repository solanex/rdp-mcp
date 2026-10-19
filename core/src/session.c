/* Eén RDP-sessie: FreeRDP-context, verbindings-thread, framebuffer en invoer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/channels/disp.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/settings.h>
#include <winpr/synch.h>
#include <winpr/sysinfo.h>

#include "internal.h"

uint64_t rdpmcp_now_ms(void)
{
	return GetTickCount64();
}

static void set_state(rdpmcp_session* s, rdpmcp_state state, const char* message)
{
	atomic_store(&s->state, (int)state);
	if (s->cb.state)
		s->cb.state(s->cb.user, state, message);
}

/* ---------- framebuffer (rdp-thread) ---------- */

/* Kopieert een rechthoek uit de FreeRDP-GDI-buffer naar onze eigen buffer.
 * Bij een andere resolutie wordt de buffer opnieuw aangemaakt en volledig gevuld. */
static void fb_update(rdpmcp_session* s, int x, int y, int w, int h)
{
	rdpGdi* gdi = s->common.context.gdi;
	const int gw = (int)gdi->width;
	const int gh = (int)gdi->height;

	pthread_mutex_lock(&s->fb_lock);
	if (!s->fb || s->fb_w != gw || s->fb_h != gh)
	{
		uint8_t* fb = realloc(s->fb, (size_t)gw * gh * 4);
		if (!fb)
		{
			pthread_mutex_unlock(&s->fb_lock);
			return;
		}
		s->fb = fb;
		s->fb_w = gw;
		s->fb_h = gh;
		s->fb_resized = true;
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
			memcpy(s->fb + ((size_t)row * gw + x) * 4,
			       gdi->primary_buffer + (size_t)row * gdi->stride + (size_t)x * 4, (size_t)w * 4);

		if (!s->fb_has_dirty)
		{
			s->dirty_x1 = x;
			s->dirty_y1 = y;
			s->dirty_x2 = x + w;
			s->dirty_y2 = y + h;
			s->fb_has_dirty = true;
		}
		else
		{
			if (x < s->dirty_x1) s->dirty_x1 = x;
			if (y < s->dirty_y1) s->dirty_y1 = y;
			if (x + w > s->dirty_x2) s->dirty_x2 = x + w;
			if (y + h > s->dirty_y2) s->dirty_y2 = y + h;
		}
	}
	pthread_mutex_unlock(&s->fb_lock);

	if (!atomic_exchange(&s->frame_pending, true) && s->cb.frame)
		s->cb.frame(s->cb.user);
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
	rdpmcp_session* s = (rdpmcp_session*)context;
	HGDI_WND hwnd = context->gdi->primary->hdc->hwnd;

	if (hwnd->invalid->null)
		return TRUE;

	fb_update(s, hwnd->invalid->x, hwnd->invalid->y, hwnd->invalid->w, hwnd->invalid->h);
	hwnd->invalid->null = TRUE;
	hwnd->ninvalid = 0;
	return TRUE;
}

static BOOL mcp_desktop_resize(rdpContext* context)
{
	rdpmcp_session* s = (rdpmcp_session*)context;
	rdpSettings* settings = context->settings;

	if (!gdi_resize(context->gdi, freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth),
	                freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight)))
		return FALSE;

	fb_update(s, 0, 0, (int)context->gdi->width, (int)context->gdi->height);
	return TRUE;
}

/* ---------- dynamische resolutie ---------- */

static void disp_send(rdpmcp_session* s)
{
	const uint32_t w = atomic_load(&s->want_w);
	const uint32_t h = atomic_load(&s->want_h);
	if (!w || !h || !atomic_load(&s->disp_ready) || !s->disp)
		return;

	pthread_mutex_lock(&s->fb_lock);
	const bool same = (uint32_t)s->fb_w == w && (uint32_t)s->fb_h == h;
	pthread_mutex_unlock(&s->fb_lock);
	if (same)
		return;

	DISPLAY_CONTROL_MONITOR_LAYOUT layout = { 0 };
	layout.Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
	layout.Width = w;
	layout.Height = h;
	layout.PhysicalWidth = w;
	layout.PhysicalHeight = h;
	layout.DesktopScaleFactor = 100;
	layout.DeviceScaleFactor = 100;
	pthread_mutex_lock(&s->input_lock);
	(void)s->disp->SendMonitorLayout(s->disp, 1, &layout);
	pthread_mutex_unlock(&s->input_lock);
}

static UINT disp_caps(DispClientContext* disp, UINT32 max_monitors, UINT32 factor_a,
                      UINT32 factor_b)
{
	rdpmcp_session* s = disp->custom;
	WINPR_UNUSED(max_monitors);
	WINPR_UNUSED(factor_a);
	WINPR_UNUSED(factor_b);
	atomic_store(&s->disp_ready, true);
	disp_send(s);
	return CHANNEL_RC_OK;
}

void rdpmcp_request_resize(rdpmcp_session* s, uint32_t width, uint32_t height)
{
	if (!freerdp_settings_get_bool(s->common.context.settings, FreeRDP_DynamicResolutionUpdate))
		return;
	/* Windows wil een even breedte tussen 200 en 8192 */
	width &= ~1u;
	if (width < 200) width = 200;
	if (height < 200) height = 200;
	if (width > 8192) width = 8192;
	if (height > 8192) height = 8192;
	atomic_store(&s->want_w, width);
	atomic_store(&s->want_h, height);
	disp_send(s);
}

/* ---------- FreeRDP-callbacks ---------- */

/* Klembord en display zelf afhandelen; de standaardhandlers van FreeRDP koppelen
 * de rest, o.a. de grafische pipeline (/gfx) aan GDI */
static void mcp_channel_connected(void* context, const ChannelConnectedEventArgs* e)
{
	rdpmcp_session* s = context;
	if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
		rdpmcp_clip_connected(s, (CliprdrClientContext*)e->pInterface);
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		s->disp = (DispClientContext*)e->pInterface;
		s->disp->custom = s;
		s->disp->DisplayControlCaps = disp_caps;
	}
	else
		freerdp_client_OnChannelConnectedEventHandler(context, e);
}

static void mcp_channel_disconnected(void* context, const ChannelDisconnectedEventArgs* e)
{
	rdpmcp_session* s = context;
	if (strcmp(e->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
		rdpmcp_clip_disconnected(s);
	else if (strcmp(e->name, DISP_DVC_CHANNEL_NAME) == 0)
	{
		atomic_store(&s->disp_ready, false);
		s->disp = NULL;
	}
	else
		freerdp_client_OnChannelDisconnectedEventHandler(context, e);
}

static BOOL mcp_pre_connect(freerdp* instance)
{
	rdpContext* context = instance->context;
	rdpSettings* settings = context->settings;
	rdpmcp_session* s = (rdpmcp_session*)context;

	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_MACINTOSH))
		return FALSE;
	if (!freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_MACINTOSH))
		return FALSE;
	/* Unicode-invoer aanvragen; FreeRDP zet dit na de capability-uitwisseling terug op wat
	 * de server ondersteunt. Zonder dit staat het altijd uit en kan type_text geen é of € typen. */
	if (!freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, TRUE))
		return FALSE;

	if (!s->clipboard_enabled &&
	    !freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, FALSE))
		return FALSE;
	s->clipboard_enabled = freerdp_settings_get_bool(settings, FreeRDP_RedirectClipboard);

	PubSub_SubscribeChannelConnected(context->pubSub, mcp_channel_connected);
	PubSub_SubscribeChannelDisconnected(context->pubSub, mcp_channel_disconnected);
	return TRUE;
}

static BOOL mcp_post_connect(freerdp* instance)
{
	rdpContext* context = instance->context;
	rdpmcp_session* s = (rdpmcp_session*)context;

	if (!gdi_init(instance, PIXEL_FORMAT_BGRX32))
		return FALSE;

	context->update->BeginPaint = mcp_begin_paint;
	context->update->EndPaint = mcp_end_paint;
	context->update->DesktopResize = mcp_desktop_resize;

	fb_update(s, 0, 0, (int)context->gdi->width, (int)context->gdi->height);
	atomic_store(&s->connected, true);
	return TRUE;
}

static void mcp_post_disconnect(freerdp* instance)
{
	rdpmcp_session* s = (rdpmcp_session*)instance->context;
	atomic_store(&s->connected, false);
	gdi_free(instance);
}

static char* dup_or_null(const char* v)
{
	return v ? strdup(v) : NULL;
}

/* Wachtwoorden vragen via de frontend (dialoogvenster) of anders in de terminal */
static BOOL mcp_authenticate(freerdp* instance, char** username, char** password, char** domain,
                             rdp_auth_reason reason)
{
	rdpmcp_session* s = (rdpmcp_session*)instance->context;

	switch (reason)
	{
		case AUTH_NLA:
		case AUTH_TLS:
		case AUTH_RDP:
		case GW_AUTH_HTTP:
		case GW_AUTH_RDG:
		case GW_AUTH_RPC:
			if (s->cb.authenticate)
				break;
			/* fallthrough */
		default:
			return client_cli_authenticate_ex(instance, username, password, domain, reason);
	}

	char* u = dup_or_null(*username);
	char* p = dup_or_null(*password);
	char* d = dup_or_null(*domain);
	if (!s->cb.authenticate(s->cb.user, &u, &p, &d))
	{
		free(u);
		free(p);
		free(d);
		return FALSE;
	}
	free(*username);
	free(*password);
	free(*domain);
	*username = u;
	*password = p;
	*domain = d;
	return TRUE;
}

static DWORD mcp_verify_certificate(freerdp* instance, const char* host, UINT16 port,
                                    const char* common_name, const char* subject,
                                    const char* issuer, const char* fingerprint, DWORD flags)
{
	rdpmcp_session* s = (rdpmcp_session*)instance->context;
	if (!s->cb.verify_certificate)
		return client_cli_verify_certificate_ex(instance, host, port, common_name, subject, issuer,
		                                        fingerprint, flags);
	return (DWORD)s->cb.verify_certificate(s->cb.user, host, port, common_name, subject, issuer,
	                                       fingerprint, false);
}

static DWORD mcp_verify_changed_certificate(freerdp* instance, const char* host, UINT16 port,
                                            const char* common_name, const char* subject,
                                            const char* issuer, const char* new_fingerprint,
                                            const char* old_subject, const char* old_issuer,
                                            const char* old_fingerprint, DWORD flags)
{
	rdpmcp_session* s = (rdpmcp_session*)instance->context;
	if (!s->cb.verify_certificate)
		return client_cli_verify_changed_certificate_ex(instance, host, port, common_name, subject,
		                                                issuer, new_fingerprint, old_subject,
		                                                old_issuer, old_fingerprint, flags);
	return (DWORD)s->cb.verify_certificate(s->cb.user, host, port, common_name, subject, issuer,
	                                       new_fingerprint, true);
}

static BOOL mcp_client_new(freerdp* instance, rdpContext* context)
{
	rdpmcp_session* s = (rdpmcp_session*)context;

	instance->PreConnect = mcp_pre_connect;
	instance->PostConnect = mcp_post_connect;
	instance->PostDisconnect = mcp_post_disconnect;
	/* Wachtwoord en certificaat gaan via de frontend of de terminal, nooit via MCP */
	instance->AuthenticateEx = mcp_authenticate;
	instance->VerifyCertificateEx = mcp_verify_certificate;
	instance->VerifyChangedCertificateEx = mcp_verify_changed_certificate;
	instance->PresentGatewayMessage = client_cli_present_gateway_message;
	instance->LogonErrorInfo = client_cli_logon_error_info;
	instance->ChooseSmartcard = client_cli_choose_smartcard;
	instance->GetAccessToken = client_cli_get_access_token;

	pthread_mutex_init(&s->fb_lock, NULL);
	pthread_mutex_init(&s->input_lock, NULL);
	rdpmcp_clip_init(s);
	atomic_store(&s->ai_enabled, true);
	s->clipboard_enabled = true;
	s->listen_fd = -1;
	return TRUE;
}

static void mcp_client_free(freerdp* instance, rdpContext* context)
{
	rdpmcp_session* s = (rdpmcp_session*)context;
	WINPR_UNUSED(instance);

	free(s->fb);
	s->fb = NULL;
	pthread_mutex_destroy(&s->fb_lock);
	pthread_mutex_destroy(&s->input_lock);
	rdpmcp_clip_free(s);
}

static rdpmcp_session* session_alloc(const rdpmcp_callbacks* callbacks)
{
	RDP_CLIENT_ENTRY_POINTS ep;
	memset(&ep, 0, sizeof(ep));
	ep.Version = RDP_CLIENT_INTERFACE_VERSION;
	ep.Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
	ep.ContextSize = sizeof(rdpmcp_session);
	ep.ClientNew = mcp_client_new;
	ep.ClientFree = mcp_client_free;

	rdpmcp_session* s = (rdpmcp_session*)freerdp_client_context_new(&ep);
	if (s && callbacks)
		s->cb = *callbacks;
	return s;
}

#define SET_OR_FAIL(expr) \
	do                    \
	{                     \
		if (!(expr))      \
			goto fail;    \
	} while (0)

rdpmcp_session* rdpmcp_session_new(const rdpmcp_config* c, const rdpmcp_callbacks* callbacks)
{
	if (!c || !c->host || !*c->host)
		return NULL;
	rdpmcp_session* s = session_alloc(callbacks);
	if (!s)
		return NULL;
	rdpSettings* st = s->common.context.settings;

	/* Eerst de standaarden van de FreeRDP-opdrachtregel overnemen: zonder die stap staan
	 * o.a. de grafische pipeline (GFX/H.264) en bureaubladcompositie uit, en valt FreeRDP
	 * terug op trage bitmap-updates. */
	{
		char program[] = "rdp-mcp";
		char server[] = "/v:localhost";
		char* argv[] = { program, server, NULL };
		SET_OR_FAIL(freerdp_client_settings_parse_command_line(st, 2, argv, FALSE) == 0);
	}

	SET_OR_FAIL(freerdp_settings_set_string(st, FreeRDP_ServerHostname, c->host));
	SET_OR_FAIL(freerdp_settings_set_uint32(st, FreeRDP_ServerPort, c->port ? c->port : 3389));
	if (c->username && *c->username)
		SET_OR_FAIL(freerdp_settings_set_string(st, FreeRDP_Username, c->username));
	if (c->domain && *c->domain)
		SET_OR_FAIL(freerdp_settings_set_string(st, FreeRDP_Domain, c->domain));
	if (c->password && *c->password)
		SET_OR_FAIL(freerdp_settings_set_string(st, FreeRDP_Password, c->password));
	if (c->gateway && *c->gateway)
	{
		SET_OR_FAIL(freerdp_settings_set_string(st, FreeRDP_GatewayHostname, c->gateway));
		SET_OR_FAIL(freerdp_settings_set_bool(st, FreeRDP_GatewayUseSameCredentials, TRUE));
		SET_OR_FAIL(freerdp_set_gateway_usage_method(st, TSC_PROXY_MODE_DIRECT));
	}
	SET_OR_FAIL(freerdp_settings_set_uint32(st, FreeRDP_DesktopWidth, c->width ? c->width : 1024));
	SET_OR_FAIL(
	    freerdp_settings_set_uint32(st, FreeRDP_DesktopHeight, c->height ? c->height : 768));
	if (c->keyboard_layout)
		SET_OR_FAIL(freerdp_settings_set_uint32(st, FreeRDP_KeyboardLayout, c->keyboard_layout));
	SET_OR_FAIL(freerdp_settings_set_bool(st, FreeRDP_IgnoreCertificate, c->ignore_certificate));
	if (c->dynamic_resolution)
	{
		SET_OR_FAIL(freerdp_settings_set_bool(st, FreeRDP_DynamicResolutionUpdate, TRUE));
		SET_OR_FAIL(freerdp_settings_set_bool(st, FreeRDP_SupportDisplayControl, TRUE));
	}
	SET_OR_FAIL(freerdp_settings_set_bool(st, FreeRDP_RedirectClipboard, c->clipboard));
	s->clipboard_enabled = c->clipboard;
	atomic_store(&s->ai_enabled, c->ai_enabled);

	snprintf(s->host, sizeof(s->host), "%s", c->host);
	snprintf(s->name, sizeof(s->name), "%s", (c->name && *c->name) ? c->name : c->host);
	return s;

fail:
	freerdp_client_context_free(&s->common.context);
	return NULL;
}

rdpmcp_session* rdpmcp_session_new_from_args(int argc, char** argv, bool clipboard,
                                             bool ai_enabled, const rdpmcp_callbacks* callbacks,
                                             int* exit_code)
{
	*exit_code = 1;
	rdpmcp_session* s = session_alloc(callbacks);
	if (!s)
		return NULL;
	rdpSettings* st = s->common.context.settings;

	const int status = freerdp_client_settings_parse_command_line(st, argc, argv, FALSE);
	if (status)
	{
		*exit_code = freerdp_client_settings_command_line_status_print(st, status, argc, argv);
		if (status == COMMAND_LINE_STATUS_PRINT_HELP)
			*exit_code = -1; /* teken voor de frontend: eigen help tonen */
		freerdp_client_context_free(&s->common.context);
		return NULL;
	}

	s->clipboard_enabled = clipboard;
	atomic_store(&s->ai_enabled, ai_enabled);
	const char* host = freerdp_settings_get_string(st, FreeRDP_ServerHostname);
	snprintf(s->host, sizeof(s->host), "%s", host ? host : "?");
	snprintf(s->name, sizeof(s->name), "%s", s->host);
	*exit_code = 0;
	return s;
}

/* ---------- rdp-thread ---------- */

static void* rdp_thread(void* arg)
{
	rdpmcp_session* s = arg;
	rdpContext* context = &s->common.context;
	freerdp* instance = context->instance;

	set_state(s, RDPMCP_STATE_CONNECTING, NULL);
	if (!freerdp_connect(instance))
	{
		const UINT32 err = freerdp_get_last_error(context);
		set_state(s, err == FREERDP_ERROR_CONNECT_CANCELLED ? RDPMCP_STATE_DISCONNECTED
		                                                    : RDPMCP_STATE_FAILED,
		          freerdp_get_last_error_string(err));
		return NULL;
	}
	set_state(s, RDPMCP_STATE_CONNECTED, NULL);

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
			if (client_auto_reconnect_ex(instance, NULL))
				continue;
			break;
		}
	}

	atomic_store(&s->connected, false);
	const UINT32 err = freerdp_get_last_error(context);
	const UINT32 info = freerdp_error_info(instance);
	freerdp_disconnect(instance);

	if (err == FREERDP_ERROR_SUCCESS || err == FREERDP_ERROR_CONNECT_CANCELLED)
		set_state(s, RDPMCP_STATE_DISCONNECTED, info ? freerdp_get_error_info_string(info) : NULL);
	else
		set_state(s, RDPMCP_STATE_FAILED, freerdp_get_last_error_string(err));
	return NULL;
}

bool rdpmcp_session_start(rdpmcp_session* s)
{
	if (!rdpmcp_control_start(s))
		fprintf(stderr, "rdp-mcp: control-socket niet gestart, MCP werkt niet voor %s\n", s->name);
	if (pthread_create(&s->rdp_thread, NULL, rdp_thread, s) != 0)
	{
		rdpmcp_control_stop(s);
		return false;
	}
	s->rdp_thread_started = true;
	return true;
}

void rdpmcp_session_disconnect(rdpmcp_session* s)
{
	freerdp_abort_connect_context(&s->common.context);
}

void rdpmcp_session_free(rdpmcp_session* s)
{
	if (!s)
		return;
	rdpmcp_session_disconnect(s);
	if (s->rdp_thread_started)
		pthread_join(s->rdp_thread, NULL);
	rdpmcp_control_stop(s);
	freerdp_client_context_free(&s->common.context);
}

rdpmcp_state rdpmcp_session_state(rdpmcp_session* s)
{
	return (rdpmcp_state)atomic_load(&s->state);
}

const char* rdpmcp_session_socket_path(rdpmcp_session* s)
{
	return s->sock_path[0] ? s->sock_path : NULL;
}

/* ---------- framebuffer (frontend) ---------- */

const uint8_t* rdpmcp_frame_lock(rdpmcp_session* s, int* width, int* height, rdpmcp_rect* dirty,
                                 bool* resized)
{
	pthread_mutex_lock(&s->fb_lock);
	atomic_store(&s->frame_pending, false);
	*width = s->fb_w;
	*height = s->fb_h;
	if (resized)
		*resized = s->fb_resized;
	if (dirty)
	{
		if (s->fb_resized)
			*dirty = (rdpmcp_rect){ 0, 0, s->fb_w, s->fb_h };
		else if (s->fb_has_dirty)
			*dirty = (rdpmcp_rect){ s->dirty_x1, s->dirty_y1, s->dirty_x2 - s->dirty_x1,
				                    s->dirty_y2 - s->dirty_y1 };
		else
			*dirty = (rdpmcp_rect){ 0, 0, 0, 0 };
	}
	s->fb_resized = false;
	s->fb_has_dirty = false;
	return s->fb;
}

void rdpmcp_frame_unlock(rdpmcp_session* s)
{
	pthread_mutex_unlock(&s->fb_lock);
}

/* ---------- lokale invoer (frontend) ---------- */

static void clamp(rdpmcp_session* s, int* x, int* y)
{
	const int w = s->fb_w;
	const int h = s->fb_h;
	if (*x < 0) *x = 0;
	if (*y < 0) *y = 0;
	if (w > 0 && *x >= w) *x = w - 1;
	if (h > 0 && *y >= h) *y = h - 1;
}

static void send_mouse(rdpmcp_session* s, UINT16 flags, int x, int y)
{
	if (!atomic_load(&s->connected))
		return;
	clamp(s, &x, &y);
	pthread_mutex_lock(&s->input_lock);
	freerdp_input_send_mouse_event(s->common.context.input, flags, (UINT16)x, (UINT16)y);
	pthread_mutex_unlock(&s->input_lock);
}

void rdpmcp_input_key(rdpmcp_session* s, uint32_t scancode, bool down, bool repeat)
{
	if (!atomic_load(&s->connected) || scancode == 0 || scancode >= 512)
		return;
	pthread_mutex_lock(&s->input_lock);
	freerdp_input_send_keyboard_event_ex(s->common.context.input, down, repeat, scancode);
	s->key_down[scancode] = down;
	pthread_mutex_unlock(&s->input_lock);
	atomic_store(&s->user_last_ms, rdpmcp_now_ms());
}

void rdpmcp_input_move(rdpmcp_session* s, int x, int y)
{
	send_mouse(s, PTR_FLAGS_MOVE, x, y);
}

void rdpmcp_input_button(rdpmcp_session* s, rdpmcp_button button, bool down, int x, int y)
{
	atomic_store(&s->user_last_ms, rdpmcp_now_ms());
	switch (button)
	{
		case RDPMCP_BUTTON_LEFT:
			send_mouse(s, PTR_FLAGS_BUTTON1 | (down ? PTR_FLAGS_DOWN : 0), x, y);
			break;
		case RDPMCP_BUTTON_RIGHT:
			send_mouse(s, PTR_FLAGS_BUTTON2 | (down ? PTR_FLAGS_DOWN : 0), x, y);
			break;
		case RDPMCP_BUTTON_MIDDLE:
			send_mouse(s, PTR_FLAGS_BUTTON3 | (down ? PTR_FLAGS_DOWN : 0), x, y);
			break;
		case RDPMCP_BUTTON_X1:
		case RDPMCP_BUTTON_X2:
		{
			if (!atomic_load(&s->connected))
				return;
			clamp(s, &x, &y);
			const UINT16 flags = (button == RDPMCP_BUTTON_X1 ? PTR_XFLAGS_BUTTON1 : PTR_XFLAGS_BUTTON2) |
			                     (down ? PTR_XFLAGS_DOWN : 0);
			pthread_mutex_lock(&s->input_lock);
			freerdp_input_send_extended_mouse_event(s->common.context.input, flags, (UINT16)x,
			                                        (UINT16)y);
			pthread_mutex_unlock(&s->input_lock);
			break;
		}
	}
}

void rdpmcp_input_wheel(rdpmcp_session* s, bool horizontal, int notches, int x, int y)
{
	const UINT16 axis = horizontal ? PTR_FLAGS_HWHEEL : PTR_FLAGS_WHEEL;
	if (notches > 30) notches = 30;
	if (notches < -30) notches = -30;
	for (; notches > 0; notches--)
		send_mouse(s, axis | 0x0078, x, y);
	for (; notches < 0; notches++)
		send_mouse(s, axis | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088, x, y);
}

void rdpmcp_input_focus(rdpmcp_session* s, bool caps_lock, bool num_lock, bool scroll_lock)
{
	if (!atomic_load(&s->connected))
		return;
	UINT16 flags = 0;
	if (caps_lock)
		flags |= KBD_SYNC_CAPS_LOCK;
	if (num_lock)
		flags |= KBD_SYNC_NUM_LOCK;
	if (scroll_lock)
		flags |= KBD_SYNC_SCROLL_LOCK;
	pthread_mutex_lock(&s->input_lock);
	freerdp_input_send_focus_in_event(s->common.context.input, flags);
	pthread_mutex_unlock(&s->input_lock);
}

void rdpmcp_input_release_all(rdpmcp_session* s)
{
	for (uint32_t code = 1; code < 512; code++)
		if (s->key_down[code])
			rdpmcp_input_key(s, code, false, false);
}

void rdpmcp_set_ai_enabled(rdpmcp_session* s, bool enabled)
{
	atomic_store(&s->ai_enabled, enabled);
}

bool rdpmcp_get_ai_enabled(rdpmcp_session* s)
{
	return atomic_load(&s->ai_enabled);
}
