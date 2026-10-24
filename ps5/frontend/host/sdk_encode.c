/* PS5 port frontend: the LZMA SDK's own encoder as a command-line tool, for the 7z reader's tests (2026-10-16, review, AI-assisted).
 *
 * 7-Zip's LZMA2 and LZMA encoders are this SDK's Lzma2Enc.c and LzmaEnc.c, so the streams made here are the ones 7-Zip makes: with several threads LZMA2 cuts the data
 * into independent blocks that each start with a dictionary reset, incompressible stretches are stored in uncompressed chunks, and the chunk and state-reset
 * patterns are the encoder's own. The other test archives (make_7z_fixtures.py) are made by liblzma, which shares none of that code. sdk_corpus_check.py wraps
 * the raw streams made here in a 7z container and runs the reader over them.
 *
 *   sdk_encode <in> <out> lzma2 <dictionary bytes> <level 0-9> <threads> <block bytes | 0 (auto) | solid>
 *   sdk_encode <in> <out> lzma  <dictionary bytes> <level 0-9> <threads (1-2)> 0 [lc lp pb]
 *
 * Writes the raw stream to <out> and prints  PROP <byte>  (LZMA2) or  PROPS <10 hex digits>  (LZMA), then  SIZE <bytes>.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Precomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Lzma2Enc.h"
#include "LzmaEnc.h"

static void *MyAlloc(ISzAllocPtr p, size_t size)
{
	(void)p;
	return size ? malloc(size) : NULL;
}
static void MyFree(ISzAllocPtr p, void *address)
{
	(void)p;
	free(address);
}
static const ISzAlloc g_alloc = {MyAlloc, MyFree};

typedef struct
{
	ISeqInStream vt;
	const Byte *p;
	size_t left;
} MemIn;

static SRes MemIn_Read(ISeqInStreamPtr pp, void *buf, size_t *size)
{
	MemIn *s = Z7_CONTAINER_FROM_VTBL(pp, MemIn, vt);
	const size_t n = *size < s->left ? *size : s->left;
	memcpy(buf, s->p, n);
	s->p += n;
	s->left -= n;
	*size = n;
	return SZ_OK;
}

typedef struct
{
	ISeqOutStream vt;
	Byte *buf;
	size_t len, cap;
} MemOut;

static size_t MemOut_Write(ISeqOutStreamPtr pp, const void *data, size_t size)
{
	MemOut *s = Z7_CONTAINER_FROM_VTBL(pp, MemOut, vt);
	if (s->len + size > s->cap)
	{
		const size_t cap = (s->len + size) * 2 + 4096;
		Byte *nb = (Byte *)realloc(s->buf, cap);
		if (!nb)
			return 0;
		s->buf = nb;
		s->cap = cap;
	}
	memcpy(s->buf + s->len, data, size);
	s->len += size;
	return size;
}

int main(int argc, char **argv)
{
	FILE *f;
	long n;
	Byte *in;
	const char *kind;
	unsigned long long dict;
	int level, threads;
	MemIn mi;
	MemOut mo;
	if (argc < 8)
	{
		fprintf(stderr, "usage: sdk_encode <in> <out> lzma2|lzma <dict bytes> <level> <threads> <block bytes|0|solid> [lc lp pb]\n");
		return 64;
	}
	f = fopen(argv[1], "rb");
	if (!f)
		return 2;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	in = (Byte *)malloc(n ? (size_t)n : 1);
	if (!in || fread(in, 1, (size_t)n, f) != (size_t)n)
		return 3;
	fclose(f);
	kind = argv[3];
	dict = strtoull(argv[4], 0, 10);
	level = atoi(argv[5]);
	threads = atoi(argv[6]);
	mi.vt.Read = MemIn_Read;
	mi.p = in;
	mi.left = (size_t)n;
	mo.vt.Write = MemOut_Write;
	mo.buf = NULL;
	mo.len = mo.cap = 0;
	if (!strcmp(kind, "lzma2"))
	{
		CLzma2EncHandle h = Lzma2Enc_Create(&g_alloc, &g_alloc);
		CLzma2EncProps p;
		SRes r;
		Lzma2EncProps_Init(&p);
		p.lzmaProps.level = level;
		p.lzmaProps.dictSize = (UInt32)dict;
		p.numTotalThreads = threads;
		p.blockSize = !strcmp(argv[7], "solid") ? LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID : strtoull(argv[7], 0, 10);
		if (!h || Lzma2Enc_SetProps(h, &p) != SZ_OK)
		{
			fprintf(stderr, "can't set the properties\n");
			return 4;
		}
		r = Lzma2Enc_Encode2(h, &mo.vt, NULL, NULL, &mi.vt, NULL, 0, NULL);
		if (r != SZ_OK)
		{
			fprintf(stderr, "encoder error %d\n", (int)r);
			return 5;
		}
		printf("PROP %u\n", (unsigned)Lzma2Enc_WriteProperties(h));
		Lzma2Enc_Destroy(h);
	}
	else
	{
		CLzmaEncHandle h = LzmaEnc_Create(&g_alloc);
		CLzmaEncProps p;
		Byte props[5];
		SizeT ps = 5;
		SRes r;
		LzmaEncProps_Init(&p);
		p.level = level;
		p.dictSize = (UInt32)dict;
		p.numThreads = threads > 2 ? 2 : threads;
		if (argc >= 11)
		{
			p.lc = atoi(argv[8]);
			p.lp = atoi(argv[9]);
			p.pb = atoi(argv[10]);
		}
		if (!h || LzmaEnc_SetProps(h, &p) != SZ_OK)
		{
			fprintf(stderr, "can't set the properties\n");
			return 4;
		}
		LzmaEnc_WriteProperties(h, props, &ps);
		r = LzmaEnc_Encode(h, &mo.vt, &mi.vt, NULL, &g_alloc, &g_alloc);
		if (r != SZ_OK)
		{
			fprintf(stderr, "encoder error %d\n", (int)r);
			return 5;
		}
		printf("PROPS %02x%02x%02x%02x%02x\n", props[0], props[1], props[2], props[3], props[4]);
		LzmaEnc_Destroy(h, &g_alloc, &g_alloc);
	}
	f = fopen(argv[2], "wb");
	if (!f || fwrite(mo.buf, 1, mo.len, f) != mo.len)
		return 6;
	fclose(f);
	printf("SIZE %zu\n", mo.len);
	free(in);
	free(mo.buf);
	return 0;
}
