/* Unix-socket waarmee de MCP-server de sessie bekijkt en bestuurt.
 * Protocol: zie PROTOCOL.md. Eén thread per verbonden client. */

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <freerdp/input.h>

#include "internal.h"

typedef struct
{
	rdpmcp_session* s;
	int fd;
} conn_t;

static bool write_all(int fd, const void* buf, size_t len)
{
	const uint8_t* p = buf;
	while (len > 0)
	{
		const ssize_t n = write(fd, p, len);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static bool reply(int fd, const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return false;
	size_t len = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 2;
	buf[len++] = '\n';
	return write_all(fd, buf, len);
}

void rdpmcp_ai_touch(rdpmcp_session* s, int x, int y)
{
	if (x >= 0)
	{
		atomic_store(&s->ai_x, x);
		atomic_store(&s->ai_y, y);
	}
	atomic_store(&s->ai_last_ms, rdpmcp_now_ms());
	if (s->cb.ai_activity)
		s->cb.ai_activity(s->cb.user, x, y);
}

static bool clamp_xy(rdpmcp_session* s, int* x, int* y)
{
	pthread_mutex_lock(&s->fb_lock);
	const int w = s->fb_w;
	const int h = s->fb_h;
	pthread_mutex_unlock(&s->fb_lock);
	if (w <= 0 || h <= 0)
		return false;
	if (*x < 0) *x = 0;
	if (*y < 0) *y = 0;
	if (*x >= w) *x = w - 1;
	if (*y >= h) *y = h - 1;
	return true;
}

static void mouse(rdpmcp_session* s, UINT16 flags, int x, int y)
{
	pthread_mutex_lock(&s->input_lock);
	freerdp_input_send_mouse_event(s->common.context.input, flags, (UINT16)x, (UINT16)y);
	pthread_mutex_unlock(&s->input_lock);
}

static UINT16 button_flag(const char* name)
{
	if (strcmp(name, "left") == 0)
		return PTR_FLAGS_BUTTON1;
	if (strcmp(name, "right") == 0)
		return PTR_FLAGS_BUTTON2;
	if (strcmp(name, "middle") == 0)
		return PTR_FLAGS_BUTTON3;
	return 0;
}

static bool cmd_screenshot(rdpmcp_session* s, int fd)
{
	pthread_mutex_lock(&s->fb_lock);
	const int w = s->fb_w;
	const int h = s->fb_h;
	const size_t len = (size_t)w * h * 4;
	uint8_t* copy = (s->fb && len) ? malloc(len) : NULL;
	if (copy)
		memcpy(copy, s->fb, len);
	pthread_mutex_unlock(&s->fb_lock);

	if (!copy)
		return reply(fd, "ERR no-frame no image received yet");
	const bool ok = reply(fd, "OK %d %d %zu", w, h, len) && write_all(fd, copy, len);
	free(copy);
	return ok;
}

#define CLIP_MAX_BYTES (4u * 1024u * 1024u)

static bool clip_error(int fd, int rc)
{
	switch (rc)
	{
		case CLIP_DISABLED:
			return reply(fd, "ERR clipboard-disabled clipboard is off (--no-clipboard or -clipboard)");
		case CLIP_LOCAL_PRIVATE:
			return reply(fd, "ERR clipboard-local the clipboard holds content from the user's Mac, "
			                 "which is not shared with the AI");
		default:
			return reply(fd, "ERR clipboard-not-ready the clipboard channel is not active (yet)");
	}
}

static bool cmd_clipget(rdpmcp_session* s, int fd)
{
	char* text = NULL;
	const int rc = rdpmcp_clip_get(s, &text, 2000);
	if (rc != CLIP_OK)
		return clip_error(fd, rc);
	const size_t len = text ? strlen(text) : 0;
	const bool ok = reply(fd, "OK %zu", len) && (len == 0 || write_all(fd, text, len));
	free(text);
	return ok;
}

static bool cmd_clipset(rdpmcp_session* s, int fd, FILE* in, const char* line)
{
	size_t len = 0;
	if (sscanf(line, "%*s %zu", &len) != 1 || len > CLIP_MAX_BYTES)
	{
		/* payload niet betrouwbaar over te slaan: melden en verbinding sluiten */
		reply(fd, "ERR syntax CLIPSET <bytes> followed by the UTF-8 text (max 4 MiB)");
		return false;
	}

	char* text = malloc(len + 1);
	if (!text)
		return false;
	if (len > 0 && fread(text, 1, len, in) != len)
	{
		free(text);
		return false;
	}
	text[len] = '\0';

	/* payload altijd inlezen, ook bij een weigering, zodat het protocol synchroon blijft */
	bool ok;
	if (!atomic_load(&s->connected))
		ok = reply(fd, "ERR not-connected session is not connected");
	else if (!atomic_load(&s->ai_enabled))
		ok = reply(fd, "ERR ai-disabled the user has turned AI control off");
	else if (memchr(text, '\0', len))
		ok = reply(fd, "ERR syntax text contains a NUL character");
	else
	{
		const int rc = rdpmcp_clip_set_ai(s, text);
		if (rc == CLIP_OK)
			rdpmcp_ai_touch(s, -1, -1);
		ok = rc == CLIP_OK ? reply(fd, "OK") : clip_error(fd, rc);
	}
	free(text);
	return ok;
}

/* Bits in s->ai_refused: knoppen en toetsen waarvan het indrukken geweigerd werd */
enum
{
	GUARD_BIT_LEFT = 1u << 0,
	GUARD_BIT_RIGHT = 1u << 1,
	GUARD_BIT_MIDDLE = 1u << 2,
	GUARD_BIT_ENTER = 1u << 3,
	GUARD_BIT_KP_ENTER = 1u << 4,
	GUARD_BIT_SPACE = 1u << 5
};

static unsigned button_bit(UINT16 flag)
{
	switch (flag)
	{
		case PTR_FLAGS_BUTTON1: return GUARD_BIT_LEFT;
		case PTR_FLAGS_BUTTON2: return GUARD_BIT_RIGHT;
		default: return GUARD_BIT_MIDDLE;
	}
}

/* Enter en spatie bevestigen een geselecteerd menu-item of een knop */
static unsigned key_bit(unsigned code)
{
	switch (code)
	{
		case 0x1C: return GUARD_BIT_ENTER;
		case 0x11C: return GUARD_BIT_KP_ENTER;
		case 0x39: return GUARD_BIT_SPACE;
		default: return 0;
	}
}

/* De client sloot de verbinding (bv. een time-out terwijl de gebruiker nog moest beslissen) */
static bool peer_gone(int fd)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	if (poll(&p, 1, 0) <= 0)
		return false;
	if (p.revents & (POLLHUP | POLLERR))
		return true;
	char c;
	return recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

/* Vraagt de frontend of de AI dit mag; true = doorlaten */
static bool guard_allows(rdpmcp_session* s, int fd, rdpmcp_guard_kind kind, int x, int y)
{
	if (!s->cb.ai_guard)
		return true;
	pthread_mutex_lock(&s->fb_lock);
	const int w = s->fb_w;
	const int h = s->fb_h;
	const size_t len = (size_t)w * h * 4;
	uint8_t* copy = (s->fb && len) ? malloc(len) : NULL;
	if (copy)
		memcpy(copy, s->fb, len);
	pthread_mutex_unlock(&s->fb_lock);
	if (!copy)
		return false; /* zonder beeld valt er niets te controleren */
	const bool ok = s->cb.ai_guard(s->cb.user, kind, x, y, copy, w, h);
	free(copy);
	/* wie niet meer wacht op het antwoord, krijgt de actie ook niet uitgevoerd */
	return ok && !peer_gone(fd);
}

static bool reply_refused(int fd)
{
	return reply(fd, "ERR refused the user did not allow this action (it looks like it would "
	                 "restart, shut down or sign out Windows). Do not try another way; ask the user.");
}

/* Verwerkt één commandoregel; false = verbinding sluiten */
static bool handle_line(rdpmcp_session* s, int fd, FILE* in, char* line)
{
	char cmd[16] = { 0 };
	char a1[16] = { 0 };
	char a2[16] = { 0 };
	int x = 0;
	int y = 0;

	if (sscanf(line, "%15s", cmd) != 1)
		return reply(fd, "ERR syntax empty line");

	if (strcmp(cmd, "INFO") == 0)
	{
		pthread_mutex_lock(&s->fb_lock);
		const int w = s->fb_w;
		const int h = s->fb_h;
		pthread_mutex_unlock(&s->fb_lock);
		const bool unicode =
		    freerdp_settings_get_bool(s->common.context.settings, FreeRDP_UnicodeInput);
		return reply(fd, "OK %d %d %d %d %s %s", w, h, atomic_load(&s->ai_enabled) ? 1 : 0,
		             unicode ? 1 : 0, s->host[0] ? s->host : "?", s->name);
	}

	if (strcmp(cmd, "SCREENSHOT") == 0)
		return cmd_screenshot(s, fd);

	/* CLIPSET leest eerst zijn payload en controleert daarna zelf */
	if (strcmp(cmd, "CLIPSET") == 0)
		return cmd_clipset(s, fd, in, line);

	/* Alles hieronder is invoer: enkel als de gebruiker AI-besturing toelaat */
	if (!atomic_load(&s->connected))
		return reply(fd, "ERR not-connected session is not connected");
	if (!atomic_load(&s->ai_enabled))
		return reply(fd, "ERR ai-disabled the user has turned AI control off");

	if (strcmp(cmd, "CLIPGET") == 0)
		return cmd_clipget(s, fd);

	if (strcmp(cmd, "MOVE") == 0)
	{
		if (sscanf(line, "%*s %d %d", &x, &y) != 2 || !clamp_xy(s, &x, &y))
			return reply(fd, "ERR syntax MOVE <x> <y>");
		mouse(s, PTR_FLAGS_MOVE, x, y);
		rdpmcp_ai_touch(s, x, y);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "BUTTON") == 0)
	{
		if (sscanf(line, "%*s %15s %15s %d %d", a1, a2, &x, &y) != 4 || !clamp_xy(s, &x, &y))
			return reply(fd, "ERR syntax BUTTON <left|right|middle> <down|up> <x> <y>");
		const UINT16 flag = button_flag(a1);
		const bool down = strcmp(a2, "down") == 0;
		if (!flag || (!down && strcmp(a2, "up") != 0))
			return reply(fd, "ERR syntax BUTTON <left|right|middle> <down|up> <x> <y>");
		const unsigned bit = button_bit(flag);
		if (down)
		{
			if (!guard_allows(s, fd, RDPMCP_GUARD_CLICK, x, y))
			{
				atomic_fetch_or(&s->ai_refused, bit);
				return reply_refused(fd);
			}
			atomic_fetch_and(&s->ai_refused, ~bit);
			atomic_store(&s->ai_down_x, x);
			atomic_store(&s->ai_down_y, y);
		}
		else
		{
			/* het indrukken werd geweigerd: dan ook niet loslaten */
			if (atomic_fetch_and(&s->ai_refused, ~bit) & bit)
				return reply_refused(fd);
			/* Een menu-item reageert op het loslaten. Elders loslaten dan waar de knop
			 * neerging (slepen) wordt dus apart gecontroleerd; bij een weigering laat de
			 * knop los waar hij neerging. */
			const int dx = x - atomic_load(&s->ai_down_x);
			const int dy = y - atomic_load(&s->ai_down_y);
			if ((dx * dx + dy * dy > 16) && !guard_allows(s, fd, RDPMCP_GUARD_CLICK, x, y))
			{
				const int ox = atomic_load(&s->ai_down_x);
				const int oy = atomic_load(&s->ai_down_y);
				mouse(s, PTR_FLAGS_MOVE, ox, oy);
				mouse(s, flag, ox, oy);
				return reply_refused(fd);
			}
		}
		mouse(s, flag | (down ? PTR_FLAGS_DOWN : 0), x, y);
		rdpmcp_ai_touch(s, x, y);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "WHEEL") == 0)
	{
		int notches = 0;
		if (sscanf(line, "%*s %15s %d %d %d", a1, &notches, &x, &y) != 4 ||
		    !clamp_xy(s, &x, &y) || (strcmp(a1, "v") != 0 && strcmp(a1, "h") != 0))
			return reply(fd, "ERR syntax WHEEL <v|h> <notches> <x> <y>");
		const UINT16 axis = strcmp(a1, "v") == 0 ? PTR_FLAGS_WHEEL : PTR_FLAGS_HWHEEL;
		if (notches > 30) notches = 30;
		if (notches < -30) notches = -30;
		for (; notches > 0; notches--)
			mouse(s, axis | 0x0078, x, y);
		for (; notches < 0; notches++)
			mouse(s, axis | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088, x, y);
		rdpmcp_ai_touch(s, x, y);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "KEY") == 0)
	{
		unsigned code = 0;
		if (sscanf(line, "%*s %15s %x", a1, &code) != 2 || code == 0 || code > 0x1FF ||
		    (strcmp(a1, "down") != 0 && strcmp(a1, "up") != 0))
			return reply(fd, "ERR syntax KEY <down|up> <scancode-hex>");
		const bool down = strcmp(a1, "down") == 0;
		const unsigned bit = key_bit(code);
		if (bit && down)
		{
			if (!guard_allows(s, fd, RDPMCP_GUARD_KEY, -1, -1))
			{
				atomic_fetch_or(&s->ai_refused, bit);
				return reply_refused(fd);
			}
			atomic_fetch_and(&s->ai_refused, ~bit);
		}
		else if (bit && (atomic_fetch_and(&s->ai_refused, ~bit) & bit))
			return reply_refused(fd);
		pthread_mutex_lock(&s->input_lock);
		freerdp_input_send_keyboard_event_ex(s->common.context.input, down, FALSE, code);
		pthread_mutex_unlock(&s->input_lock);
		rdpmcp_ai_touch(s, -1, -1);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "UNICODE") == 0)
	{
		unsigned code = 0;
		if (sscanf(line, "%*s %x", &code) != 1 || code == 0 || code > 0xFFFF)
			return reply(fd, "ERR syntax UNICODE <utf16-codeunit-hex>");
		if (!freerdp_settings_get_bool(s->common.context.settings, FreeRDP_UnicodeInput))
			return reply(fd, "ERR unicode-unsupported the server does not support Unicode input");
		if ((code == '\r' || code == '\n' || code == ' ') &&
		    !guard_allows(s, fd, RDPMCP_GUARD_KEY, -1, -1))
			return reply_refused(fd);
		pthread_mutex_lock(&s->input_lock);
		freerdp_input_send_unicode_keyboard_event(s->common.context.input, 0, (UINT16)code);
		freerdp_input_send_unicode_keyboard_event(s->common.context.input, KBD_FLAGS_RELEASE,
		                                          (UINT16)code);
		pthread_mutex_unlock(&s->input_lock);
		rdpmcp_ai_touch(s, -1, -1);
		return reply(fd, "OK");
	}

	return reply(fd, "ERR unknown-command unknown command '%s'", cmd);
}

static bool conn_register(rdpmcp_session* s, int fd)
{
	pthread_mutex_lock(&s->conn_lock);
	const bool ok = s->listen_fd >= 0 && s->conn_count < RDPMCP_MAX_CONNS;
	if (ok)
		s->conn_fds[s->conn_count++] = fd;
	pthread_mutex_unlock(&s->conn_lock);
	return ok;
}

static void conn_unregister(rdpmcp_session* s, int fd)
{
	pthread_mutex_lock(&s->conn_lock);
	for (int i = 0; i < s->conn_count; i++)
	{
		if (s->conn_fds[i] == fd)
		{
			s->conn_fds[i] = s->conn_fds[--s->conn_count];
			break;
		}
	}
	pthread_cond_broadcast(&s->conn_cond);
	pthread_mutex_unlock(&s->conn_lock);
}

static void* conn_thread(void* arg)
{
	conn_t* c = arg;
	rdpmcp_session* s = c->s;
	const int fd = c->fd;
	FILE* in = fdopen(c->fd, "r");
	if (in)
	{
		char line[1024];
		while (fgets(line, sizeof(line), in))
		{
			line[strcspn(line, "\r\n")] = '\0';
			if (!handle_line(c->s, c->fd, in, line))
				break;
		}
		/* eerst afmelden: na fclose kan het fd-nummer hergebruikt worden */
		conn_unregister(s, fd);
		fclose(in); /* sluit ook fd */
	}
	else
	{
		conn_unregister(s, fd);
		close(fd);
	}
	free(c);
	return NULL;
}

static void* accept_thread(void* arg)
{
	rdpmcp_session* s = arg;
	for (;;)
	{
		const int fd = accept(s->listen_fd, NULL, NULL);
		if (fd < 0)
		{
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			break; /* listen-socket gesloten: stoppen */
		}
#ifdef SO_NOSIGPIPE
		/* schrijven naar een client die al weg is, mag de app niet stoppen (SIGPIPE) */
		const int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		conn_t* c = malloc(sizeof(*c));
		pthread_t t;
		if (!c)
		{
			close(fd);
			continue;
		}
		c->s = s;
		c->fd = fd;
		if (!conn_register(s, fd))
		{
			reply(fd, "ERR busy too many connections");
			close(fd);
			free(c);
			continue;
		}
		if (pthread_create(&t, NULL, conn_thread, c) != 0)
		{
			conn_unregister(s, fd);
			close(fd);
			free(c);
			continue;
		}
		pthread_detach(t);
	}
	return NULL;
}

bool rdpmcp_control_start(rdpmcp_session* s)
{
	const char* dir_env = getenv("RDP_MCP_DIR");
	char dir[PATH_MAX];
	if (dir_env && *dir_env)
		snprintf(dir, sizeof(dir), "%s", dir_env);
	else
	{
		const char* home = getenv("HOME");
		if (!home)
			return false;
		snprintf(dir, sizeof(dir), "%s/.rdp-mcp", home);
	}
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return false;
	chmod(dir, 0700);

	struct sockaddr_un addr = { 0 };
	addr.sun_family = AF_UNIX;
	static atomic_int counter;
	const int n = snprintf(s->sock_path, sizeof(s->sock_path), "%s/%d-%d.sock", dir, (int)getpid(),
	                       atomic_fetch_add(&counter, 1) + 1);
	if (n < 0 || (size_t)n >= sizeof(addr.sun_path))
		return false;
	memcpy(addr.sun_path, s->sock_path, (size_t)n + 1);
	unlink(s->sock_path);

	s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s->listen_fd < 0)
		return false;

	const mode_t old = umask(0077); /* socket enkel voor de eigen gebruiker */
	const int rc = bind(s->listen_fd, (struct sockaddr*)&addr, sizeof(addr));
	umask(old);
	if (rc != 0 || listen(s->listen_fd, 4) != 0)
	{
		close(s->listen_fd);
		s->listen_fd = -1;
		s->sock_path[0] = '\0';
		return false;
	}

	if (pthread_create(&s->control_thread, NULL, accept_thread, s) != 0)
	{
		rdpmcp_control_stop(s);
		return false;
	}
	return true;
}

void rdpmcp_control_stop(rdpmcp_session* s)
{
	if (s->listen_fd >= 0)
	{
		pthread_mutex_lock(&s->conn_lock);
		shutdown(s->listen_fd, SHUT_RDWR);
		close(s->listen_fd);
		s->listen_fd = -1; /* conn_register laat geen nieuwe verbindingen meer toe */
		pthread_mutex_unlock(&s->conn_lock);
		pthread_join(s->control_thread, NULL);

		pthread_mutex_lock(&s->conn_lock);
		/* Open verbindingen afbreken en wachten tot hun threads de sessie loslaten. Een
		 * thread in ai_guard keert terug zodra de frontend de vraag annuleert. */
		for (int i = 0; i < s->conn_count; i++)
			shutdown(s->conn_fds[i], SHUT_RDWR);
		while (s->conn_count > 0)
			pthread_cond_wait(&s->conn_cond, &s->conn_lock);
		pthread_mutex_unlock(&s->conn_lock);
	}
	if (s->sock_path[0])
	{
		unlink(s->sock_path);
		s->sock_path[0] = '\0';
	}
}
