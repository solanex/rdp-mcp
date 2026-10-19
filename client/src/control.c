/* Unix-socket waarmee de MCP-server de sessie bekijkt en bestuurt.
 * Protocol: zie PROTOCOL.md. Eén thread per verbonden client. */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <freerdp/input.h>

#include "rdpmcp.h"

typedef struct
{
	mcpContext* mc;
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

static void ai_touch(mcpContext* mc, int x, int y)
{
	if (x >= 0)
	{
		atomic_store(&mc->ai_x, x);
		atomic_store(&mc->ai_y, y);
	}
	atomic_store(&mc->ai_last_ms, SDL_GetTicks());
	rdpmcp_push_event(RDPMCP_EVENT_AI);
}

static bool clamp_xy(mcpContext* mc, int* x, int* y)
{
	pthread_mutex_lock(&mc->fb_lock);
	const int w = mc->fb_w;
	const int h = mc->fb_h;
	pthread_mutex_unlock(&mc->fb_lock);
	if (w <= 0 || h <= 0)
		return false;
	if (*x < 0) *x = 0;
	if (*y < 0) *y = 0;
	if (*x >= w) *x = w - 1;
	if (*y >= h) *y = h - 1;
	return true;
}

static void mouse(mcpContext* mc, UINT16 flags, int x, int y)
{
	pthread_mutex_lock(&mc->input_lock);
	freerdp_input_send_mouse_event(mc->common.context.input, flags, (UINT16)x, (UINT16)y);
	pthread_mutex_unlock(&mc->input_lock);
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

static bool cmd_screenshot(mcpContext* mc, int fd)
{
	pthread_mutex_lock(&mc->fb_lock);
	const int w = mc->fb_w;
	const int h = mc->fb_h;
	const size_t len = (size_t)w * h * 4;
	uint8_t* copy = (mc->fb && len) ? malloc(len) : NULL;
	if (copy)
		memcpy(copy, mc->fb, len);
	pthread_mutex_unlock(&mc->fb_lock);

	if (!copy)
		return reply(fd, "ERR no-frame nog geen beeld ontvangen");
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
			return reply(fd, "ERR clipboard-disabled klembord staat uit (--no-clipboard of -clipboard)");
		case CLIP_LOCAL_PRIVATE:
			return reply(fd, "ERR clipboard-local het klembord bevat inhoud van de Mac van de "
			                 "gebruiker; die wordt niet gedeeld met de AI");
		default:
			return reply(fd, "ERR clipboard-not-ready het klembordkanaal is (nog) niet actief");
	}
}

static bool cmd_clipget(mcpContext* mc, int fd)
{
	char* text = NULL;
	const int rc = rdpmcp_clip_get(mc, &text, 2000);
	if (rc != CLIP_OK)
		return clip_error(fd, rc);
	const size_t len = text ? strlen(text) : 0;
	const bool ok = reply(fd, "OK %zu", len) && (len == 0 || write_all(fd, text, len));
	free(text);
	return ok;
}

static bool cmd_clipset(mcpContext* mc, int fd, FILE* in, const char* line)
{
	size_t len = 0;
	if (sscanf(line, "%*s %zu", &len) != 1 || len > CLIP_MAX_BYTES)
	{
		/* payload niet betrouwbaar over te slaan: melden en verbinding sluiten */
		reply(fd, "ERR syntax CLIPSET <bytes> gevolgd door de UTF-8-tekst (max 4 MiB)");
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
	if (!atomic_load(&mc->connected))
		ok = reply(fd, "ERR not-connected sessie is niet verbonden");
	else if (!atomic_load(&mc->ai_enabled))
		ok = reply(fd, "ERR ai-disabled de gebruiker heeft AI-besturing uitgeschakeld");
	else if (memchr(text, '\0', len))
		ok = reply(fd, "ERR syntax tekst bevat een NUL-teken");
	else
	{
		const int rc = rdpmcp_clip_set_ai(mc, text);
		if (rc == CLIP_OK)
			ai_touch(mc, -1, -1);
		ok = rc == CLIP_OK ? reply(fd, "OK") : clip_error(fd, rc);
	}
	free(text);
	return ok;
}

/* Verwerkt één commandoregel; false = verbinding sluiten */
static bool handle_line(mcpContext* mc, int fd, FILE* in, char* line)
{
	char cmd[16] = { 0 };
	char a1[16] = { 0 };
	char a2[16] = { 0 };
	int x = 0;
	int y = 0;

	if (sscanf(line, "%15s", cmd) != 1)
		return reply(fd, "ERR syntax lege regel");

	if (strcmp(cmd, "INFO") == 0)
	{
		pthread_mutex_lock(&mc->fb_lock);
		const int w = mc->fb_w;
		const int h = mc->fb_h;
		pthread_mutex_unlock(&mc->fb_lock);
		const bool unicode =
		    freerdp_settings_get_bool(mc->common.context.settings, FreeRDP_UnicodeInput);
		return reply(fd, "OK %d %d %d %d %s", w, h, atomic_load(&mc->ai_enabled) ? 1 : 0,
		             unicode ? 1 : 0, mc->host[0] ? mc->host : "?");
	}

	if (strcmp(cmd, "SCREENSHOT") == 0)
		return cmd_screenshot(mc, fd);

	/* CLIPSET leest eerst zijn payload en controleert daarna zelf */
	if (strcmp(cmd, "CLIPSET") == 0)
		return cmd_clipset(mc, fd, in, line);

	/* Alles hieronder is invoer: enkel als de gebruiker AI-besturing toelaat */
	if (!atomic_load(&mc->connected))
		return reply(fd, "ERR not-connected sessie is niet verbonden");
	if (!atomic_load(&mc->ai_enabled))
		return reply(fd, "ERR ai-disabled de gebruiker heeft AI-besturing uitgeschakeld");

	if (strcmp(cmd, "CLIPGET") == 0)
		return cmd_clipget(mc, fd);

	if (strcmp(cmd, "MOVE") == 0)
	{
		if (sscanf(line, "%*s %d %d", &x, &y) != 2 || !clamp_xy(mc, &x, &y))
			return reply(fd, "ERR syntax MOVE <x> <y>");
		mouse(mc, PTR_FLAGS_MOVE, x, y);
		ai_touch(mc, x, y);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "BUTTON") == 0)
	{
		if (sscanf(line, "%*s %15s %15s %d %d", a1, a2, &x, &y) != 4 || !clamp_xy(mc, &x, &y))
			return reply(fd, "ERR syntax BUTTON <left|right|middle> <down|up> <x> <y>");
		const UINT16 flag = button_flag(a1);
		const bool down = strcmp(a2, "down") == 0;
		if (!flag || (!down && strcmp(a2, "up") != 0))
			return reply(fd, "ERR syntax BUTTON <left|right|middle> <down|up> <x> <y>");
		mouse(mc, flag | (down ? PTR_FLAGS_DOWN : 0), x, y);
		ai_touch(mc, x, y);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "WHEEL") == 0)
	{
		int notches = 0;
		if (sscanf(line, "%*s %15s %d %d %d", a1, &notches, &x, &y) != 4 ||
		    !clamp_xy(mc, &x, &y) || (strcmp(a1, "v") != 0 && strcmp(a1, "h") != 0))
			return reply(fd, "ERR syntax WHEEL <v|h> <notches> <x> <y>");
		const UINT16 axis = strcmp(a1, "v") == 0 ? PTR_FLAGS_WHEEL : PTR_FLAGS_HWHEEL;
		if (notches > 30) notches = 30;
		if (notches < -30) notches = -30;
		for (; notches > 0; notches--)
			mouse(mc, axis | 0x0078, x, y);
		for (; notches < 0; notches++)
			mouse(mc, axis | PTR_FLAGS_WHEEL_NEGATIVE | 0x0088, x, y);
		ai_touch(mc, x, y);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "KEY") == 0)
	{
		unsigned code = 0;
		if (sscanf(line, "%*s %15s %x", a1, &code) != 2 || code == 0 || code > 0x1FF ||
		    (strcmp(a1, "down") != 0 && strcmp(a1, "up") != 0))
			return reply(fd, "ERR syntax KEY <down|up> <scancode-hex>");
		pthread_mutex_lock(&mc->input_lock);
		freerdp_input_send_keyboard_event_ex(mc->common.context.input, strcmp(a1, "down") == 0,
		                                     FALSE, code);
		pthread_mutex_unlock(&mc->input_lock);
		ai_touch(mc, -1, -1);
		return reply(fd, "OK");
	}

	if (strcmp(cmd, "UNICODE") == 0)
	{
		unsigned code = 0;
		if (sscanf(line, "%*s %x", &code) != 1 || code == 0 || code > 0xFFFF)
			return reply(fd, "ERR syntax UNICODE <utf16-codeunit-hex>");
		if (!freerdp_settings_get_bool(mc->common.context.settings, FreeRDP_UnicodeInput))
			return reply(fd, "ERR unicode-unsupported de server ondersteunt geen Unicode-invoer");
		pthread_mutex_lock(&mc->input_lock);
		freerdp_input_send_unicode_keyboard_event(mc->common.context.input, 0, (UINT16)code);
		freerdp_input_send_unicode_keyboard_event(mc->common.context.input, KBD_FLAGS_RELEASE,
		                                          (UINT16)code);
		pthread_mutex_unlock(&mc->input_lock);
		ai_touch(mc, -1, -1);
		return reply(fd, "OK");
	}

	return reply(fd, "ERR unknown-command onbekend commando '%s'", cmd);
}

static void* conn_thread(void* arg)
{
	conn_t* c = arg;
	FILE* in = fdopen(c->fd, "r");
	if (in)
	{
		char line[1024];
		while (fgets(line, sizeof(line), in))
		{
			line[strcspn(line, "\r\n")] = '\0';
			if (!handle_line(c->mc, c->fd, in, line))
				break;
		}
		fclose(in); /* sluit ook c->fd */
	}
	else
		close(c->fd);
	free(c);
	return NULL;
}

static void* accept_thread(void* arg)
{
	mcpContext* mc = arg;
	for (;;)
	{
		const int fd = accept(mc->listen_fd, NULL, NULL);
		if (fd < 0)
		{
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			break; /* listen-socket gesloten: stoppen */
		}
		conn_t* c = malloc(sizeof(*c));
		pthread_t t;
		if (!c)
		{
			close(fd);
			continue;
		}
		c->mc = mc;
		c->fd = fd;
		if (pthread_create(&t, NULL, conn_thread, c) != 0)
		{
			close(fd);
			free(c);
			continue;
		}
		pthread_detach(t);
	}
	return NULL;
}

bool rdpmcp_control_start(mcpContext* mc)
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
	const int n = snprintf(mc->sock_path, sizeof(mc->sock_path), "%s/%d.sock", dir, (int)getpid());
	if (n < 0 || (size_t)n >= sizeof(addr.sun_path))
		return false;
	memcpy(addr.sun_path, mc->sock_path, (size_t)n + 1);
	unlink(mc->sock_path);

	mc->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (mc->listen_fd < 0)
		return false;

	const mode_t old = umask(0077); /* socket enkel voor de eigen gebruiker */
	const int rc = bind(mc->listen_fd, (struct sockaddr*)&addr, sizeof(addr));
	umask(old);
	if (rc != 0 || listen(mc->listen_fd, 4) != 0)
	{
		close(mc->listen_fd);
		mc->listen_fd = -1;
		mc->sock_path[0] = '\0';
		return false;
	}

	if (pthread_create(&mc->control_thread, NULL, accept_thread, mc) != 0)
	{
		rdpmcp_control_stop(mc);
		return false;
	}
	return true;
}

void rdpmcp_control_stop(mcpContext* mc)
{
	if (mc->listen_fd >= 0)
	{
		shutdown(mc->listen_fd, SHUT_RDWR);
		close(mc->listen_fd);
		mc->listen_fd = -1;
		pthread_join(mc->control_thread, NULL);
	}
	if (mc->sock_path[0])
	{
		unlink(mc->sock_path);
		mc->sock_path[0] = '\0';
	}
}
