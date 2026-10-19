/* Klembord via het cliprdr-kanaal: tekst (CF_UNICODETEXT) en afbeeldingen (CF_DIB).
 *
 * - Mac -> remote: bij een wijziging van het Mac-klembord kondigen we de formaten
 *   aan; de server vraagt de inhoud pas op bij plakken.
 * - remote -> Mac: kondigt de server tekst en/of een afbeelding aan, dan halen we
 *   die meteen op (eerst tekst, dan afbeelding) en geven ze aan de frontend.
 * - AI: kan de remote tekst lezen en zelf tekst aanbieden (CLIPGET/CLIPSET).
 *
 * Privacy: de AI krijgt het Mac-klembord nooit te zien. CLIPGET weigert zolang
 * de lokale inhoud actief is, en als de laatste invoer van de AI kwam (bv. een
 * AI-ctrl+v), geven we de Mac-inhoud ook niet aan de server. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <freerdp/client/cliprdr.h>
#include <winpr/string.h>
#include <winpr/user.h>

#include "internal.h"

#define CLIP_MAX_DIB (64u * 1024u * 1024u)

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

static UINT send_format_list(rdpmcp_session* s, bool has_text, bool has_dib)
{
	CliprdrClientContext* cliprdr = s->cliprdr;
	if (!cliprdr)
		return CHANNEL_RC_OK;

	CLIPRDR_FORMAT formats[2] = { 0 };
	UINT32 n = 0;
	if (has_text)
		formats[n++].formatId = CF_UNICODETEXT;
	if (has_dib)
		formats[n++].formatId = CF_DIB;
	CLIPRDR_FORMAT_LIST list = { 0 };
	list.common.msgType = CB_FORMAT_LIST;
	list.numFormats = n;
	list.formats = n ? formats : NULL;
	return cliprdr->ClientFormatList(cliprdr, &list);
}

static UINT request_format(CliprdrClientContext* cliprdr, UINT32 format)
{
	CLIPRDR_FORMAT_DATA_REQUEST request = { 0 };
	request.common.msgType = CB_FORMAT_DATA_REQUEST;
	request.requestedFormatId = format;
	return cliprdr->ClientFormatDataRequest(cliprdr, &request);
}

/* Aanvragen afgerond: wachtenden wekken en de frontend de nieuwe inhoud geven */
static void finish_remote(rdpmcp_session* s)
{
	char* text = NULL;
	uint8_t* dib = NULL;
	size_t dib_len = 0;

	pthread_mutex_lock(&s->clip_lock);
	s->clip_pending = false;
	s->clip_requested = 0;
	s->clip_want_dib = false;
	if (s->clip_owner == CLIP_OWNER_REMOTE)
	{
		text = dup_or_null(s->clip_remote_text);
		if (s->clip_remote_dib && (dib = malloc(s->clip_remote_dib_len)))
		{
			memcpy(dib, s->clip_remote_dib, s->clip_remote_dib_len);
			dib_len = s->clip_remote_dib_len;
		}
	}
	pthread_cond_broadcast(&s->clip_cond);
	pthread_mutex_unlock(&s->clip_lock);

	/* de frontend zet dit op het lokale klembord */
	if (s->cb.remote_clipboard_data)
	{
		if (text || dib)
			s->cb.remote_clipboard_data(s->cb.user, text, dib, dib_len);
	}
	else if (text && s->cb.remote_clipboard)
		s->cb.remote_clipboard(s->cb.user, text);
	free(text);
	free(dib);
}

/* ---------- callbacks van het cliprdr-kanaal ---------- */

static UINT cb_monitor_ready(CliprdrClientContext* cliprdr, const CLIPRDR_MONITOR_READY* ready)
{
	rdpmcp_session* s = cliprdr->custom;
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

	pthread_mutex_lock(&s->clip_lock);
	const bool has_text = (s->clip_owner == CLIP_OWNER_LOCAL && s->clip_local_text) ||
	                      (s->clip_owner == CLIP_OWNER_AI && s->clip_ai_text);
	const bool has_dib = s->clip_owner == CLIP_OWNER_LOCAL && s->clip_local_dib;
	pthread_mutex_unlock(&s->clip_lock);
	return send_format_list(s, has_text, has_dib);
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
	rdpmcp_session* s = cliprdr->custom;

	CLIPRDR_FORMAT_LIST_RESPONSE response = { 0 };
	response.common.msgType = CB_FORMAT_LIST_RESPONSE;
	response.common.msgFlags = CB_RESPONSE_OK;
	UINT rc = cliprdr->ClientFormatListResponse(cliprdr, &response);
	if (rc != CHANNEL_RC_OK)
		return rc;

	bool has_text = false;
	bool has_dib = false;
	for (UINT32 i = 0; i < list->numFormats; i++)
	{
		if (list->formats[i].formatId == CF_UNICODETEXT)
			has_text = true;
		else if (list->formats[i].formatId == CF_DIB)
			has_dib = true;
	}

	/* eerst tekst, daarna de afbeelding: er kan maar één aanvraag tegelijk lopen */
	const UINT32 first = has_text ? CF_UNICODETEXT : (has_dib ? CF_DIB : 0);
	pthread_mutex_lock(&s->clip_lock);
	s->clip_owner = CLIP_OWNER_REMOTE;
	free(s->clip_remote_text);
	s->clip_remote_text = NULL;
	free(s->clip_remote_dib);
	s->clip_remote_dib = NULL;
	s->clip_remote_dib_len = 0;
	s->clip_pending = first != 0;
	s->clip_requested = first;
	s->clip_want_dib = has_text && has_dib;
	pthread_cond_broadcast(&s->clip_cond);
	pthread_mutex_unlock(&s->clip_lock);

	if (first && (rc = request_format(cliprdr, first)) != CHANNEL_RC_OK)
		finish_remote(s);
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
	rdpmcp_session* s = cliprdr->custom;
	WCHAR* wtext = NULL;
	size_t wlen = 0;
	uint8_t* dib = NULL;
	size_t dib_len = 0;
	bool blocked = false;

	if (request->requestedFormatId == CF_DIB)
	{
		pthread_mutex_lock(&s->clip_lock);
		if (s->clip_owner == CLIP_OWNER_LOCAL && s->clip_local_dib)
		{
			if (atomic_load(&s->ai_last_ms) > atomic_load(&s->user_last_ms))
				blocked = true;
			else if ((dib = malloc(s->clip_local_dib_len)))
			{
				memcpy(dib, s->clip_local_dib, s->clip_local_dib_len);
				dib_len = s->clip_local_dib_len;
			}
		}
		pthread_mutex_unlock(&s->clip_lock);
	}
	else if (request->requestedFormatId == CF_UNICODETEXT)
	{
		pthread_mutex_lock(&s->clip_lock);
		const char* src = NULL;
		if (s->clip_owner == CLIP_OWNER_AI)
			src = s->clip_ai_text;
		else if (s->clip_owner == CLIP_OWNER_LOCAL)
		{
			/* Kwam de laatste invoer van de AI, dan is dit vermoedelijk een AI-plakactie */
			if (atomic_load(&s->ai_last_ms) > atomic_load(&s->user_last_ms))
				blocked = true;
			else
				src = s->clip_local_text;
		}
		char* crlf = src ? lf_to_crlf(src) : NULL;
		pthread_mutex_unlock(&s->clip_lock);

		if (crlf)
		{
			wtext = ConvertUtf8ToWCharAlloc(crlf, &wlen);
			free(crlf);
		}
	}

	if (blocked)
	{
		static const char* msg = "Mac-klembord niet gedeeld: de plakopdracht kwam van de AI";
		if (s->cb.notice)
			s->cb.notice(s->cb.user, msg);
		else
		{
			printf("%s\n", msg);
			fflush(stdout);
		}
	}

	CLIPRDR_FORMAT_DATA_RESPONSE response = { 0 };
	response.common.msgType = CB_FORMAT_DATA_RESPONSE;
	if (wtext)
	{
		response.common.msgFlags = CB_RESPONSE_OK;
		response.common.dataLen = (UINT32)((wlen + 1) * sizeof(WCHAR));
		response.requestedFormatData = (const BYTE*)wtext;
	}
	else if (dib)
	{
		response.common.msgFlags = CB_RESPONSE_OK;
		response.common.dataLen = (UINT32)dib_len;
		response.requestedFormatData = dib;
	}
	else
		response.common.msgFlags = CB_RESPONSE_FAIL;

	const UINT rc = cliprdr->ClientFormatDataResponse(cliprdr, &response);
	free(wtext);
	free(dib);
	return rc;
}

static UINT cb_server_format_data_response(CliprdrClientContext* cliprdr,
                                           const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	rdpmcp_session* s = cliprdr->custom;
	const bool ok = (response->common.msgFlags & CB_RESPONSE_OK) && response->requestedFormatData;
	const UINT32 len = response->common.dataLen;

	pthread_mutex_lock(&s->clip_lock);
	const UINT32 format = s->clip_requested;
	const bool want_dib = s->clip_want_dib;
	pthread_mutex_unlock(&s->clip_lock);

	if (format == CF_UNICODETEXT)
	{
		char* text = NULL;
		if (ok && len >= sizeof(WCHAR))
		{
			text = ConvertWCharNToUtf8Alloc((const WCHAR*)response->requestedFormatData,
			                                len / sizeof(WCHAR), NULL);
			crlf_to_lf(text);
		}
		pthread_mutex_lock(&s->clip_lock);
		if (s->clip_owner == CLIP_OWNER_REMOTE)
		{
			free(s->clip_remote_text);
			s->clip_remote_text = text;
			text = NULL;
		}
		if (want_dib)
		{
			s->clip_requested = CF_DIB;
			s->clip_want_dib = false;
		}
		pthread_mutex_unlock(&s->clip_lock);
		free(text);

		if (want_dib && request_format(cliprdr, CF_DIB) == CHANNEL_RC_OK)
			return CHANNEL_RC_OK; /* finish_remote volgt na de afbeelding */
	}
	else if (format == CF_DIB && ok && len > 40 && len <= CLIP_MAX_DIB)
	{
		uint8_t* dib = malloc(len);
		if (dib)
			memcpy(dib, response->requestedFormatData, len);
		pthread_mutex_lock(&s->clip_lock);
		if (s->clip_owner == CLIP_OWNER_REMOTE && dib)
		{
			free(s->clip_remote_dib);
			s->clip_remote_dib = dib;
			s->clip_remote_dib_len = len;
			dib = NULL;
		}
		pthread_mutex_unlock(&s->clip_lock);
		free(dib);
	}

	finish_remote(s);
	return CHANNEL_RC_OK;
}

/* ---------- levenscyclus ---------- */

void rdpmcp_clip_init(rdpmcp_session* s)
{
	pthread_mutex_init(&s->clip_lock, NULL);
	pthread_cond_init(&s->clip_cond, NULL);
	s->clip_owner = CLIP_OWNER_NONE;
}

void rdpmcp_clip_free(rdpmcp_session* s)
{
	free(s->clip_local_text);
	free(s->clip_remote_text);
	free(s->clip_ai_text);
	free(s->clip_local_dib);
	free(s->clip_remote_dib);
	s->clip_local_text = s->clip_remote_text = s->clip_ai_text = NULL;
	s->clip_local_dib = s->clip_remote_dib = NULL;
	pthread_cond_destroy(&s->clip_cond);
	pthread_mutex_destroy(&s->clip_lock);
}

void rdpmcp_clip_connected(rdpmcp_session* s, CliprdrClientContext* cliprdr)
{
	cliprdr->custom = s;
	cliprdr->MonitorReady = cb_monitor_ready;
	cliprdr->ServerCapabilities = cb_server_capabilities;
	cliprdr->ServerFormatList = cb_server_format_list;
	cliprdr->ServerFormatListResponse = cb_server_format_list_response;
	cliprdr->ServerFormatDataRequest = cb_server_format_data_request;
	cliprdr->ServerFormatDataResponse = cb_server_format_data_response;

	pthread_mutex_lock(&s->clip_lock);
	s->cliprdr = cliprdr;
	pthread_mutex_unlock(&s->clip_lock);
}

void rdpmcp_clip_disconnected(rdpmcp_session* s)
{
	pthread_mutex_lock(&s->clip_lock);
	s->cliprdr = NULL;
	s->clip_pending = false;
	s->clip_requested = 0;
	s->clip_want_dib = false;
	pthread_cond_broadcast(&s->clip_cond);
	pthread_mutex_unlock(&s->clip_lock);
}

/* ---------- frontend ---------- */

void rdpmcp_local_clipboard_changed(rdpmcp_session* s, const char* text)
{
	rdpmcp_local_clipboard_set(s, text, NULL, 0);
}

void rdpmcp_local_clipboard_set(rdpmcp_session* s, const char* text, const uint8_t* dib,
                                size_t dib_len)
{
	if (!s->clipboard_enabled)
		return;
	if (!dib || dib_len <= 40 || dib_len > CLIP_MAX_DIB)
	{
		dib = NULL;
		dib_len = 0;
	}

	pthread_mutex_lock(&s->clip_lock);
	/* Door ons gezet vanuit de remote sessie: geen nieuwe lokale inhoud */
	const bool same_text = !text || (s->clip_remote_text && strcmp(text, s->clip_remote_text) == 0);
	const bool same_dib = !dib || (s->clip_remote_dib && s->clip_remote_dib_len == dib_len &&
	                               memcmp(dib, s->clip_remote_dib, dib_len) == 0);
	if (s->clip_owner == CLIP_OWNER_REMOTE && (text || dib) && same_text && same_dib)
	{
		pthread_mutex_unlock(&s->clip_lock);
		return;
	}
	free(s->clip_local_text);
	s->clip_local_text = (text && *text) ? strdup(text) : NULL;
	crlf_to_lf(s->clip_local_text);
	free(s->clip_local_dib);
	s->clip_local_dib = NULL;
	s->clip_local_dib_len = 0;
	if (dib && (s->clip_local_dib = malloc(dib_len)))
	{
		memcpy(s->clip_local_dib, dib, dib_len);
		s->clip_local_dib_len = dib_len;
	}
	s->clip_owner = CLIP_OWNER_LOCAL;
	const bool has_text = s->clip_local_text != NULL;
	const bool has_dib = s->clip_local_dib != NULL;
	const bool ready = s->cliprdr != NULL;
	pthread_mutex_unlock(&s->clip_lock);

	if (ready)
		send_format_list(s, has_text, has_dib);
}

/* ---------- control-thread ---------- */

int rdpmcp_clip_get(rdpmcp_session* s, char** text, int timeout_ms)
{
	*text = NULL;
	if (!s->clipboard_enabled)
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

	pthread_mutex_lock(&s->clip_lock);
	while (s->clip_pending)
		if (pthread_cond_timedwait(&s->clip_cond, &s->clip_lock, &deadline) != 0)
			break;

	int rc = CLIP_OK;
	if (!s->cliprdr)
		rc = CLIP_NOT_READY;
	else if (s->clip_owner == CLIP_OWNER_LOCAL)
		rc = CLIP_LOCAL_PRIVATE;
	else if (s->clip_owner == CLIP_OWNER_REMOTE)
		*text = dup_or_null(s->clip_remote_text);
	else if (s->clip_owner == CLIP_OWNER_AI)
		*text = dup_or_null(s->clip_ai_text);
	pthread_mutex_unlock(&s->clip_lock);
	return rc;
}

int rdpmcp_clip_set_ai(rdpmcp_session* s, const char* text)
{
	if (!s->clipboard_enabled)
		return CLIP_DISABLED;

	char* copy = strdup(text);
	if (!copy)
		return CLIP_NOT_READY;
	crlf_to_lf(copy);

	pthread_mutex_lock(&s->clip_lock);
	if (!s->cliprdr)
	{
		pthread_mutex_unlock(&s->clip_lock);
		free(copy);
		return CLIP_NOT_READY;
	}
	const bool has_text = *copy != '\0';
	free(s->clip_ai_text);
	s->clip_ai_text = copy;
	s->clip_owner = CLIP_OWNER_AI;
	pthread_mutex_unlock(&s->clip_lock);

	return send_format_list(s, has_text, false) == CHANNEL_RC_OK ? CLIP_OK : CLIP_NOT_READY;
}
