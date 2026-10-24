// PS5SX2 (2.11, AI-assisted): stand-ins for libSceImeDialog / libSceCommonDialog so ime_relay.c runs on a PC (test-ime-payload.sh).
// The "keyboard" appends an umlaut and an emoji to what it was given, as if the player had typed them, and closes after three status
// polls. STUB_MODE (environment): "cancel" ends with Circle (end_status 1), "fail" makes sceImeDialogInit fail.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	uint32_t end_status;
	int32_t reserved[16];
} ImeResult;

static ImeParam g_p;
static int g_polls;

int sceKernelLoadStartModule(const char* path, size_t args, const void* argp, unsigned flags, void* opt, int* res)
{
	(void)path;
	(void)args;
	(void)argp;
	(void)flags;
	(void)opt;
	if (res)
		*res = 0;
	return 7;
}

static int fake_common_init(void)
{
	return 0;
}

int sceKernelDlsym(int handle, const char* symbol, void** addrp)
{
	(void)handle;
	if (strcmp(symbol, "sceCommonDialogInitialize") != 0)
		return -1;
	*addrp = (void*)&fake_common_init;
	return 0;
}

int sceImeDialogInit(ImeParam* param, void* extended)
{
	(void)extended;
	const char* mode = getenv("STUB_MODE");
	if (mode && strcmp(mode, "fail") == 0)
		return (int)0x80020063;
	g_p = *param;
	// the facts the test checks go to a file (STUB_REPORT)
	const char* rep = getenv("STUB_REPORT");
	if (rep)
	{
		FILE* f = fopen(rep, "w");
		if (f)
		{
			fprintf(f, "user %d\ntype %u\noption %#x\nmax %u\ntitle16", param->user_id, param->type, param->option, param->max_text_length);
			for (const uint16_t* t = param->title; *t; t++)
				fprintf(f, " %04x", *t);
			fprintf(f, "\ntext16");
			for (const uint16_t* t = param->input_text_buffer; *t; t++)
				fprintf(f, " %04x", *t);
			fprintf(f, "\n");
			fclose(f);
		}
	}
	// "typed": an umlaut and a smiling emoji (a surrogate pair) added
	size_t n = 0;
	while (g_p.input_text_buffer[n])
		n++;
	g_p.input_text_buffer[n++] = 0x00FC;
	g_p.input_text_buffer[n++] = 0xD83D;
	g_p.input_text_buffer[n++] = 0xDE00;
	g_p.input_text_buffer[n] = 0;
	return 0;
}

int sceImeDialogGetStatus(void)
{
	return ++g_polls < 3 ? 1 : 2;
}

int sceImeDialogGetResult(ImeResult* r)
{
	const char* mode = getenv("STUB_MODE");
	r->end_status = mode && strcmp(mode, "cancel") == 0 ? 1 : 0;
	return 0;
}

int sceImeDialogTerm(void)
{
	return 0;
}
