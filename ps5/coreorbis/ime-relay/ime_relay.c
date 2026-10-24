// PS5SX2 (2.11, AI-assisted): the keyboard relay, a payload that opens the PS5's own on-screen keyboard (libSceImeDialog) for the app.
//
// PS5SX2's own process can't load libSceImeDialog on firmware 11.40 or later: sceKernelLoadStartModule answers 0x80020063 (rtld:
// "syscall load_prx failed due to 0x00000063"), as it does for libSceNotification, libSceKeyboard and libSceMouse. A payload started by the
// console's ELF loader runs in a system process, where such libraries load (notify-relay/notify_relay.c does it for notifications). So
// ProsperoImeRelay.cpp embeds this program, writes a request over g_request's marker and sends the whole file to the loader on
// 127.0.0.1:9021; this process opens the keyboard, waits for the player to close it and writes what was typed to the result file the request names.
//
// EXPERIMENT (needs a console to learn whether the dialog shows over a payload's request at all): the app uses it only when the flag file
// flags/imerelay is there, and falls back to the shelf's own keyboard when the dialog doesn't start within a few seconds. Everything that
// happens goes to the result file's neighbour "<result>.log" (stdout and stderr belong to the loader's connection, which the app does not keep).
//
// Request (the app writes it over the marker, NUL-terminated):
//   PS5SX2-IME-RELAY-REQUEST-V1\n user=<id>\n max=<chars>\n type=<0 default|1 basic latin|2 URL|3 mail|4 number>\n password=<0|1>\n
//   result=<path>\n title=<text>\n text=<text>\n
// Files: "<result>.started" appears when the dialog is up; "<result>" is written last: "status <n>\nend <n>\nrc <hex>\ntext <utf8>\n".
//
// Built with the ps5-payload-dev SDK by Makefile.vk (it links libSceImeDialog) and embedded with .incbin.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// The PS4 SDK's SceImeDialogParam as the console's library lays it out (96 bytes; the app's own check names the same fields).
typedef struct
{
	int32_t user_id;
	uint32_t type;
	uint64_t supported_languages;
	uint32_t enter_label;
	uint32_t input_method;
	void* filter;
	uint32_t option;
	uint32_t max_text_length;
	uint16_t* input_text_buffer;
	float posx, posy;
	uint32_t horizontal_alignment, vertical_alignment;
	const uint16_t* placeholder;
	const uint16_t* title;
	int8_t reserved[16];
} ImeParam;
typedef struct
{
	uint32_t end_status; // 0 OK, 1 cancelled, 2 aborted
	int32_t reserved[16];
} ImeResult;

int sceImeDialogInit(ImeParam* param, void* extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(ImeResult* result);
int sceImeDialogTerm(void);
int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, unsigned flags, void* opt, int* res);
int sceKernelDlsym(int handle, const char* symbol, void** addrp);

// The app finds the marker and writes the request over it; the array lies in .data with its whole size in the file.
__attribute__((used)) volatile char g_request[4096] = "PS5SX2-IME-RELAY-REQUEST-V1";

static char g_log_path[600];

static void note(const char* fmt, ...)
{
	if (!g_log_path[0])
		return;
	FILE* f = fopen(g_log_path, "a");
	if (!f)
		return;
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(f, "%ld %s\n", (long)time(NULL), line);
	fclose(f);
}

// "key=value" lines of the request: the value of `key` up to the end of its line (copied into out), or "".
static void field(const char* req, const char* key, char* out, size_t room)
{
	out[0] = '\0';
	char needle[32];
	snprintf(needle, sizeof(needle), "\n%s=", key);
	const char* at = strstr(req, needle);
	if (!at)
		return;
	at += strlen(needle);
	size_t n = 0;
	while (at[n] && at[n] != '\n' && n + 1 < room)
	{
		out[n] = at[n];
		n++;
	}
	out[n] = '\0';
}

static size_t utf8_to_16(const char* s, uint16_t* out, size_t room)
{
	size_t n = 0;
	while (*s && n + 2 < room)
	{
		const unsigned char c = (unsigned char)*s;
		uint32_t cp = c;
		int len = 1;
		if (c >= 0xF0 && s[1] && s[2] && s[3])
			cp = ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu), len = 4;
		else if (c >= 0xE0 && s[1] && s[2])
			cp = ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu), len = 3;
		else if (c >= 0xC0 && s[1])
			cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu), len = 2;
		s += len;
		if (cp >= 0x10000)
		{
			cp -= 0x10000;
			out[n++] = (uint16_t)(0xD800 + (cp >> 10));
			out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
		}
		else
			out[n++] = (uint16_t)cp;
	}
	out[n] = 0;
	return n;
}

static void utf16_to_8(const uint16_t* s, char* out, size_t room)
{
	size_t n = 0;
	for (size_t i = 0; s[i]; i++)
	{
		uint32_t cp = s[i];
		if (cp >= 0xD800 && cp < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
			cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
		char tmp[4];
		int len;
		if (cp < 0x80)
			tmp[0] = (char)cp, len = 1;
		else if (cp < 0x800)
			tmp[0] = (char)(0xC0 | (cp >> 6)), tmp[1] = (char)(0x80 | (cp & 0x3F)), len = 2;
		else if (cp < 0x10000)
			tmp[0] = (char)(0xE0 | (cp >> 12)), tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)), tmp[2] = (char)(0x80 | (cp & 0x3F)), len = 3;
		else
			tmp[0] = (char)(0xF0 | (cp >> 18)), tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)), tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)),
			tmp[3] = (char)(0x80 | (cp & 0x3F)), len = 4;
		if (n + (size_t)len + 1 > room)
			break;
		memcpy(out + n, tmp, (size_t)len);
		n += (size_t)len;
	}
	out[n] = '\0';
}

static void write_file(const char* path, const char* text)
{
	char tmp[640];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(text, f);
	fclose(f);
	if (rename(tmp, path) != 0)
		unlink(tmp);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	// stdout and stderr go to the loader's connection, which the app doesn't keep: nothing is written there.
	const int nul = open("/dev/null", O_WRONLY);
	if (nul >= 0)
	{
		dup2(nul, 1);
		dup2(nul, 2);
	}
	const char* req = (const char*)g_request;
	if (strncmp(req, "PS5SX2-IME-RELAY-REQUEST-V1\n", 28) != 0)
		return 1; // sent without a request: nothing to do
	char user[24], max[24], type[24], password[8], result[560], title[200], text[600];
	field(req, "user", user, sizeof(user));
	field(req, "max", max, sizeof(max));
	field(req, "type", type, sizeof(type));
	field(req, "password", password, sizeof(password));
	field(req, "result", result, sizeof(result));
	field(req, "title", title, sizeof(title));
	field(req, "text", text, sizeof(text));
	if (!result[0])
		return 2;
	snprintf(g_log_path, sizeof(g_log_path), "%s.log", result);
	note("request: user %s, max %s, type %s, password %s, title \"%s\"", user, max, type, password, title);

	// libSceCommonDialog's own start-up, which the dialogs need first (the payload SDK has no stub for it: looked up by name).
	{
		int res = 0;
		const int mod = sceKernelLoadStartModule("/system/common/lib/libSceCommonDialog.sprx", 0, NULL, 0, NULL, &res);
		note("libSceCommonDialog.sprx: load %#x", (unsigned)mod);
		if (mod >= 0)
		{
			int (*init)(void) = NULL;
			if (sceKernelDlsym(mod, "sceCommonDialogInitialize", (void**)&init) == 0 && init)
				note("sceCommonDialogInitialize: %#x", (unsigned)init());
			else
				note("sceCommonDialogInitialize: not found");
		}
	}

	static uint16_t buffer[520], title16[128];
	utf8_to_16(text, buffer, 513);
	utf8_to_16(title, title16, 128);
	ImeParam p;
	memset(&p, 0, sizeof(p));
	p.user_id = atoi(user);
	p.type = (uint32_t)atoi(type);
	p.option = 0x2 | 0x20 | (atoi(password) ? 0x4 : 0); // no auto capitalization, no learning, a password hidden
	p.max_text_length = (uint32_t)(atoi(max) > 0 && atoi(max) <= 512 ? atoi(max) : 64);
	p.input_text_buffer = buffer;
	p.posx = 960.0f;
	p.posy = 300.0f;
	p.horizontal_alignment = 1;
	p.vertical_alignment = 1;
	p.title = title16;
	const int rc = sceImeDialogInit(&p, NULL);
	note("sceImeDialogInit: %#x", (unsigned)rc);
	if (rc < 0)
	{
		char out[200];
		snprintf(out, sizeof(out), "status -1\nend 0\nrc %x\ntext \n", (unsigned)rc);
		write_file(result, out);
		sleep(1);
		_exit(3);
	}
	char started[600];
	snprintf(started, sizeof(started), "%s.started", result);
	write_file(started, "1\n");
	int status = sceImeDialogGetStatus();
	const time_t t0 = time(NULL);
	time_t beat = 0;
	char alive[600];
	snprintf(alive, sizeof(alive), "%s.alive", result);
	while (status == 1 && time(NULL) - t0 < 600)
	{
		// A heartbeat for the app (it gives up on a payload that has stopped without an answer).
		if (time(NULL) != beat)
		{
			beat = time(NULL);
			write_file(alive, "1\n");
		}
		usleep(100000);
		status = sceImeDialogGetStatus();
	}
	note("status %d after %ld s", status, (long)(time(NULL) - t0));
	ImeResult r;
	memset(&r, 0, sizeof(r));
	int rrc = -1;
	if (status == 2)
		rrc = sceImeDialogGetResult(&r);
	note("sceImeDialogGetResult: %#x, end %u", (unsigned)rrc, (unsigned)r.end_status);
	sceImeDialogTerm();
	static char typed[2200], out[2400];
	if (status == 2 && rrc == 0 && r.end_status == 0)
		utf16_to_8(buffer, typed, sizeof(typed));
	else
		typed[0] = '\0';
	snprintf(out, sizeof(out), "status %d\nend %u\nrc %x\ntext %s\n", status, (unsigned)r.end_status, (unsigned)rrc, typed);
	write_file(result, out);
	// The library may still be winding down on its own thread: _exit() stops every thread before anything is unloaded.
	sleep(1);
	_exit(0);
}
