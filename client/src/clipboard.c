/* Klembord via het cliprdr-kanaal (enkel tekst, CF_UNICODETEXT).
 *
 * - Mac -> remote: bij een wijziging van het Mac-klembord kondigen we tekst aan;
 *   de server vraagt de inhoud pas op bij plakken.
 * - remote -> Mac: kondigt de server tekst aan, dan halen we die meteen op en
 *   zetten ze op het Mac-klembord.
 * - AI: kan het remote klembord lezen en zelf tekst aanbieden (CLIPGET/CLIPSET).
 *
 * Privacy: de AI krijgt het Mac-klembord nooit te zien. CLIPGET weigert zolang
 * de Mac-inhoud actief is, en als de laatste invoer van de AI kwam (bv. een
 * AI-ctrl+v), geven we de Mac-inhoud ook niet aan de server. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <freerdp/client/cliprdr.h>
#include <winpr/string.h>
#include <winpr/user.h>

#include "rdpmcp.h"

static char* dup_or_null(const char* s)
{
	return s ? strdup(s) : NULL;
}

/* Windows gebruikt CRLF, de Mac en de AI LF */
static void crlf_to_lf(char* s)
{
	if (!s)
		return;
	char* w = s;
	for (const char* r = s; *r; r++)
		if (!(r[0] == '\r' && r[1] == '\n'))
			*w++ = *r;
	*w = '\0';
}

static char* lf_to_crlf(const char* s)
{
	size_t extra = 0;
	for (const char* p = s; *p; p++)
		if (*p == '\n' && (p == s || p[-1] != '\r'))
			extra++;
	char* out = malloc(strlen(s) + extra + 1);
	if (!out)
		return NULL;
	char* w = out;
	for (const char* p = s; *p; p++)
	{
		if (*p == '\n' && (p == s || p[-1] != '\r'))
			*w++ = '\r';
		*w++ = *p;
	}
	*w = '\0';
	return out;
}

static UINT send_format_list(mcpContext* mc, bool has_text)
{
	CliprdrClientContext* cliprdr = mc->cliprdr;
	if (!cliprdr)
		return CHANNEL_RC_OK;

	CLIPRDR_FORMAT format = { .formatId = CF_UNICODETEXT, .formatName = NULL };
	CLIPRDR_FORMAT_LIST list = { 0 };
	list.common.msgType = CB_FORMAT_LIST;
	list.numFormats = has_text ? 1 : 0;
	list.formats = has_text ? &format : NULL;
	return cliprdr->ClientFormatList(cliprdr, &list);
}

/* ---------- callbacks van het cliprdr-kanaal ---------- */

static UINT cb_monitor_ready(CliprdrClientContext* cliprdr, const CLIPRDR_MONITOR_READY* ready)
{
	mcpContext* mc = cliprdr->custom;
	WINPR_UNUSED(ready);

	CLIPRDR_GENERAL_CAPABILITY_SET general = { 0 };
	general.capabilitySetType = CB_CAPSTYPE_GENERAL;
	general.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
	general.version = CB_CAPS_VERSION_2;
	general.generalFlags = CB_USE_LONG_FORMAT_NAMES;

	CLIPRDR_CAPABILITIES caps = { 0 };
	caps.cCapabilitiesSets = 1;
	caps.capabilitySets = (CLIPRDR_CAPABILITY_SET*)&general;

	const UINT rc = cliprdr->ClientCapabilities(cliprdr, &caps);
	if (rc != CHANNEL_RC_OK)
		return rc;

	pthread_mutex_lock(&mc->clip_lock);
	const bool has_text = (mc->clip_owner == CLIP_OWNER_LOCAL && mc->clip_local_text) ||
	                      (mc->clip_owner == CLIP_OWNER_AI && mc->clip_ai_text);
	pthread_mutex_unlock(&mc->clip_lock);
	return send_format_list(mc, has_text);
}

static UINT cb_server_capabilities(CliprdrClientContext* cliprdr,
                                   const CLIPRDR_CAPABILITIES* caps)
{
	WINPR_UNUSED(cliprdr);
	WINPR_UNUSED(caps);
	return CHANNEL_RC_OK;
}

static UINT cb_server_format_list(CliprdrClientContext* cliprdr, const CLIPRDR_FORMAT_LIST* list)
{
	mcpContext* mc = cliprdr->custom;

	CLIPRDR_FORMAT_LIST_RESPONSE response = { 0 };
	response.common.msgType = CB_FORMAT_LIST_RESPONSE;
	response.common.msgFlags = CB_RESPONSE_OK;
	UINT rc = cliprdr->ClientFormatListResponse(cliprdr, &response);
	if (rc != CHANNEL_RC_OK)
		return rc;

	bool has_text = false;
	for (UINT32 i = 0; i < list->numFormats; i++)
		if (list->formats[i].formatId == CF_UNICODETEXT)
			has_text = true;

	pthread_mutex_lock(&mc->clip_lock);
	mc->clip_owner = CLIP_OWNER_REMOTE;
	free(mc->clip_remote_text);
	mc->clip_remote_text = NULL;
	mc->clip_pending = has_text;
	pthread_cond_broadcast(&mc->clip_cond);
	pthread_mutex_unlock(&mc->clip_lock);

	if (has_text)
	{
		CLIPRDR_FORMAT_DATA_REQUEST request = { 0 };
		request.common.msgType = CB_FORMAT_DATA_REQUEST;
		request.requestedFormatId = CF_UNICODETEXT;
		rc = cliprdr->ClientFormatDataRequest(cliprdr, &request);
		if (rc != CHANNEL_RC_OK)
		{
			pthread_mutex_lock(&mc->clip_lock);
			mc->clip_pending = false;
			pthread_cond_broadcast(&mc->clip_cond);
			pthread_mutex_unlock(&mc->clip_lock);
		}
	}
	return rc;
}

static UINT cb_server_format_list_response(CliprdrClientContext* cliprdr,
                                           const CLIPRDR_FORMAT_LIST_RESPONSE* response)
{
	WINPR_UNUSED(cliprdr);
	WINPR_UNUSED(response);
	return CHANNEL_RC_OK;
}

static UINT cb_server_format_data_request(CliprdrClientContext* cliprdr,
                                          const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	mcpContext* mc = cliprdr->custom;
	WCHAR* wtext = NULL;
	size_t wlen = 0;
	bool blocked = false;

	if (request->requestedFormatId == CF_UNICODETEXT)
	{
		pthread_mutex_lock(&mc->clip_lock);
		const char* src = NULL;
		if (mc->clip_owner == CLIP_OWNER_AI)
			src = mc->clip_ai_text;
		else if (mc->clip_owner == CLIP_OWNER_LOCAL)
		{
			/* Kwam de laatste invoer van de AI, dan is dit vermoedelijk een AI-plakactie */
			if (atomic_load(&mc->ai_last_ms) > atomic_load(&mc->user_last_ms))
				blocked = true;
			else
				src = mc->clip_local_text;
		}
		char* crlf = src ? lf_to_crlf(src) : NULL;
		pthread_mutex_unlock(&mc->clip_lock);

		if (crlf)
		{
			wtext = ConvertUtf8ToWCharAlloc(crlf, &wlen);
			free(crlf);
		}
	}

	if (blocked)
	{
		printf("Mac-klembord niet gedeeld: de plakopdracht kwam van de AI\n");
		fflush(stdout);
	}

	CLIPRDR_FORMAT_DATA_RESPONSE response = { 0 };
	response.common.msgType = CB_FORMAT_DATA_RESPONSE;
	if (wtext)
	{
		response.common.msgFlags = CB_RESPONSE_OK;
		response.common.dataLen = (UINT32)((wlen + 1) * sizeof(WCHAR));
		response.requestedFormatData = (const BYTE*)wtext;
	}
	else
		response.common.msgFlags = CB_RESPONSE_FAIL;

	const UINT rc = cliprdr->ClientFormatDataResponse(cliprdr, &response);
	free(wtext);
	return rc;
}

static UINT cb_server_format_data_response(CliprdrClientContext* cliprdr,
                                           const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	mcpContext* mc = cliprdr->custom;
	char* text = NULL;

	if ((response->common.msgFlags & CB_RESPONSE_OK) && response->requestedFormatData &&
	    response->common.dataLen >= sizeof(WCHAR))
	{
		text = ConvertWCharNToUtf8Alloc((const WCHAR*)response->requestedFormatData,
		                                response->common.dataLen / sizeof(WCHAR), NULL);
		crlf_to_lf(text);
	}

	bool stored = false;
	pthread_mutex_lock(&mc->clip_lock);
	if (mc->clip_owner == CLIP_OWNER_REMOTE)
	{
		free(mc->clip_remote_text);
		mc->clip_remote_text = text;
		text = NULL;
		stored = mc->clip_remote_text != NULL;
	}
	mc->clip_pending = false;
	pthread_cond_broadcast(&mc->clip_cond);
	pthread_mutex_unlock(&mc->clip_lock);
	free(text);

	if (stored)
		rdpmcp_push_event(RDPMCP_EVENT_CLIPBOARD);
	return CHANNEL_RC_OK;
}

/* ---------- levenscyclus ---------- */

void rdpmcp_clip_init(mcpContext* mc)
{
	pthread_mutex_init(&mc->clip_lock, NULL);
	pthread_cond_init(&mc->clip_cond, NULL);
	mc->clip_owner = CLIP_OWNER_NONE;
}

void rdpmcp_clip_free(mcpContext* mc)
{
	free(mc->clip_local_text);
	free(mc->clip_remote_text);
	free(mc->clip_ai_text);
	mc->clip_local_text = mc->clip_remote_text = mc->clip_ai_text = NULL;
	pthread_cond_destroy(&mc->clip_cond);
	pthread_mutex_destroy(&mc->clip_lock);
}

void rdpmcp_clip_connected(mcpContext* mc, CliprdrClientContext* cliprdr)
{
	cliprdr->custom = mc;
	cliprdr->MonitorReady = cb_monitor_ready;
	cliprdr->ServerCapabilities = cb_server_capabilities;
	cliprdr->ServerFormatList = cb_server_format_list;
	cliprdr->ServerFormatListResponse = cb_server_format_list_response;
	cliprdr->ServerFormatDataRequest = cb_server_format_data_request;
	cliprdr->ServerFormatDataResponse = cb_server_format_data_response;

	pthread_mutex_lock(&mc->clip_lock);
	mc->cliprdr = cliprdr;
	pthread_mutex_unlock(&mc->clip_lock);
}

void rdpmcp_clip_disconnected(mcpContext* mc)
{
	pthread_mutex_lock(&mc->clip_lock);
	mc->cliprdr = NULL;
	mc->clip_pending = false;
	pthread_cond_broadcast(&mc->clip_cond);
	pthread_mutex_unlock(&mc->clip_lock);
}

/* ---------- main-thread ---------- */

void rdpmcp_clip_local_changed(mcpContext* mc)
{
	if (!mc->clipboard_enabled)
		return;

	char* text = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;

	pthread_mutex_lock(&mc->clip_lock);
	/* Zelf gezet vanuit de remote sessie: geen nieuwe Mac-inhoud */
	if (text && mc->clip_remote_text && strcmp(text, mc->clip_remote_text) == 0)
	{
		pthread_mutex_unlock(&mc->clip_lock);
		SDL_free(text);
		return;
	}
	free(mc->clip_local_text);
	mc->clip_local_text = (text && *text) ? strdup(text) : NULL;
	crlf_to_lf(mc->clip_local_text);
	mc->clip_owner = CLIP_OWNER_LOCAL;
	const bool has_text = mc->clip_local_text != NULL;
	const bool ready = mc->cliprdr != NULL;
	pthread_mutex_unlock(&mc->clip_lock);
	SDL_free(text);

	if (ready)
		send_format_list(mc, has_text);
}

void rdpmcp_clip_apply_remote(mcpContext* mc)
{
	pthread_mutex_lock(&mc->clip_lock);
	char* text = mc->clip_owner == CLIP_OWNER_REMOTE ? dup_or_null(mc->clip_remote_text) : NULL;
	pthread_mutex_unlock(&mc->clip_lock);

	if (text)
		SDL_SetClipboardText(text);
	free(text);
}

/* ---------- control-thread ---------- */

int rdpmcp_clip_get(mcpContext* mc, char** text, int timeout_ms)
{
	*text = NULL;
	if (!mc->clipboard_enabled)
		return CLIP_DISABLED;

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L)
	{
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&mc->clip_lock);
	while (mc->clip_pending)
		if (pthread_cond_timedwait(&mc->clip_cond, &mc->clip_lock, &deadline) != 0)
			break;

	int rc = CLIP_OK;
	if (!mc->cliprdr)
		rc = CLIP_NOT_READY;
	else if (mc->clip_owner == CLIP_OWNER_LOCAL)
		rc = CLIP_LOCAL_PRIVATE;
	else if (mc->clip_owner == CLIP_OWNER_REMOTE)
		*text = dup_or_null(mc->clip_remote_text);
	else if (mc->clip_owner == CLIP_OWNER_AI)
		*text = dup_or_null(mc->clip_ai_text);
	pthread_mutex_unlock(&mc->clip_lock);
	return rc;
}

int rdpmcp_clip_set_ai(mcpContext* mc, const char* text)
{
	if (!mc->clipboard_enabled)
		return CLIP_DISABLED;

	char* copy = strdup(text);
	if (!copy)
		return CLIP_NOT_READY;
	crlf_to_lf(copy);

	pthread_mutex_lock(&mc->clip_lock);
	if (!mc->cliprdr)
	{
		pthread_mutex_unlock(&mc->clip_lock);
		free(copy);
		return CLIP_NOT_READY;
	}
	const bool has_text = *copy != '\0';
	free(mc->clip_ai_text);
	mc->clip_ai_text = copy;
	mc->clip_owner = CLIP_OWNER_AI;
	pthread_mutex_unlock(&mc->clip_lock);

	return send_format_list(mc, has_text) == CHANNEL_RC_OK ? CLIP_OK : CLIP_NOT_READY;
}
