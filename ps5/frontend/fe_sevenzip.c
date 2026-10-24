/* PS5 port frontend: a streaming 7z reader for the HD texture packs (2026-10-16, AI-assisted). See fe_sevenzip.h.
 *
 * Built on the LZMA SDK in 3rdparty/lzma (26.02, public domain): SzArEx_Open reads the header with every allocation counted against one
 * budget, then each folder (solid block) is decoded as a stream with Lzma2Dec_DecodeToBuf / LzmaDec_DecodeToBuf into a 1 MiB chunk, with
 * a circular dictionary buffer of min(dict, unpack size). The packed input is read with pread() from the downloaded file. Files are cut
 * out of a block's byte stream in order and handed to the consumer.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Precomp.h"

#include "fe_sevenzip.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "7z.h"
#include "7zCrc.h"
#include "CpuArch.h"
#include "Lzma2Dec.h"
#include "LzmaDec.h"

#define LOOK_BUF (64u << 10)
#define IN_CHUNK (256u << 10)
#define OUT_CHUNK (1u << 20)
#define NAME_MAX_UNITS 1024u
#define NAME_BUF (4 * NAME_MAX_UNITS + 8)

const char *Sz7CodeName(Sz7Code code)
{
	static const char *const names[] = {"OK", "NOT_7Z", "HEADER_BAD", "HEADER_TOO_BIG", "TOO_MANY_FILES", "UNSUPPORTED", "ENCRYPTED", "DICT_TOO_BIG",
		"NO_MEMORY", "IO", "DATA", "CRC", "EOF", "ENTRY_TOO_BIG", "SINK", "CANCELLED", "TOO_BIG", "NO_CRC"};
	return (unsigned)code < sizeof(names) / sizeof(names[0]) ? names[code] : "?";
}

void Sz7LimitsDefault(Sz7Limits *l)
{
	l->max_dict = 64ull << 20;
	l->max_header_bytes = 16ull << 20;
	l->max_header_mem = 48ull << 20;
	l->max_files = 65536;
	l->max_entry = 256ull << 20;
	l->max_total = 12ull << 30;
}

/* ---------- one budget for every allocation of the header phase (both of the SDK's allocators draw on it) ---------- */
typedef struct
{
	size_t cap, used, peak;
} Sz7Budget;

typedef struct
{
	ISzAlloc vt;
	Sz7Budget *b;
} Sz7Alloc;

static void *Budget_Alloc(ISzAllocPtr pp, size_t size)
{
	Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(Sz7Alloc)
	unsigned char *m;
	if (size == 0)
		return NULL;
	if (size > p->b->cap - p->b->used) /* used <= cap always */
		return NULL;
	m = (unsigned char *)malloc(size + 16);
	if (!m)
		return NULL;
	*(size_t *)(void *)m = size;
	p->b->used += size;
	if (p->b->used > p->b->peak)
		p->b->peak = p->b->used;
	return m + 16;
}

static void Budget_Free(ISzAllocPtr pp, void *address)
{
	Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(Sz7Alloc)
	unsigned char *m;
	if (!address)
		return;
	m = (unsigned char *)address - 16;
	p->b->used -= *(size_t *)(void *)m;
	free(m);
}

static void Alloc_Init(Sz7Alloc *a, Sz7Budget *b)
{
	a->vt.Alloc = Budget_Alloc;
	a->vt.Free = Budget_Free;
	a->b = b;
}

/* ---------- pread-based input ---------- */
typedef struct
{
	ISeekInStream vt;
	int fd;
	uint64_t size, pos;
} PReadIn;

static SRes PRead_Read(ISeekInStreamPtr pp, void *buf, size_t *size)
{
	Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(PReadIn)
	size_t want = *size, got = 0;
	while (got < want)
	{
		const ssize_t r = pread(p->fd, (char *)buf + got, want - got, (off_t)(p->pos + got));
		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			return SZ_ERROR_READ;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	p->pos += got;
	*size = got;
	return SZ_OK;
}

static SRes PRead_Seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
	Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(PReadIn)
	Int64 np;
	switch (origin)
	{
		case SZ_SEEK_SET: np = *pos; break;
		case SZ_SEEK_CUR: np = (Int64)p->pos + *pos; break;
		case SZ_SEEK_END: np = (Int64)p->size + *pos; break;
		default: return SZ_ERROR_PARAM;
	}
	if (np < 0)
		return SZ_ERROR_PARAM;
	p->pos = (uint64_t)np;
	*pos = np;
	return SZ_OK;
}

/* ---------- the archive ---------- */
struct Sz7Archive
{
	int fd;
	PReadIn in;
	CLookToRead2 look;
	Byte *lookbuf;
	Sz7Budget budget;
	Sz7Alloc a_main, a_temp;
	CSzArEx db;
	int db_open, extracted;
	Sz7Limits lim;
	Sz7Info info;
	UInt16 *wname; /* NAME_MAX_UNITS */
	char *name;    /* NAME_BUF */
	char err[200];
};

static Sz7Code Fail(Sz7Archive *z, Sz7Code c, const char *what)
{
	snprintf(z->err, sizeof(z->err), "%s", what);
	return c;
}

/* The SDK's CRC table is built once, by whoever gets here first (no pthread_once: nothing outside libc's basics is imported); a second caller at the same moment waits. */
static int g_crc_state; /* 0 not built, 1 being built, 2 built */
static void CrcInit(void)
{
	int expected = 0;
	if (__atomic_load_n(&g_crc_state, __ATOMIC_ACQUIRE) == 2)
		return;
	if (__atomic_compare_exchange_n(&g_crc_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
	{
		CrcGenerateTable();
		__atomic_store_n(&g_crc_state, 2, __ATOMIC_RELEASE);
		return;
	}
	while (__atomic_load_n(&g_crc_state, __ATOMIC_ACQUIRE) != 2) /* (building the 8 KB table takes microseconds) */
		;
}

static uint64_t SatAdd(uint64_t a, uint64_t b)
{
	return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

static uint64_t Lzma2DictFromProp(unsigned prop)
{
	return prop == 40 ? 0xFFFFFFFFull : (uint64_t)(2 | (prop & 1)) << (prop / 2 + 11);
}

/* What a folder is: ONE coder, LZMA2 / LZMA / Copy, one pack stream (what 7-Zip makes for the texture packs). */
typedef struct
{
	int kind; /* 0 copy, 1 lzma, 2 lzma2 */
	const Byte *props;
	unsigned props_size;
	uint64_t unpack, pack, pack_pos, dict;
} FolderInfo;

static Sz7Code GetFolder(Sz7Archive *z, uint32_t fi, FolderInfo *out)
{
	CSzFolder f;
	CSzData sd;
	const Byte *data = z->db.db.CodersData + z->db.db.FoCodersOffsets[fi];
	memset(out, 0, sizeof(*out));
	sd.Data = data;
	sd.Size = z->db.db.FoCodersOffsets[(size_t)fi + 1] - z->db.db.FoCodersOffsets[fi];
	if (SzGetNextFolderItem(&f, &sd) != SZ_OK || sd.Size != 0 || f.UnpackStream != z->db.db.FoToMainUnpackSizeIndex[fi])
		return Fail(z, SZ7_HEADER_BAD, "a folder record is damaged");
	if (f.NumCoders != 1 || f.NumPackStreams != 1 || f.NumBonds != 0 || f.PackStreams[0] != 0)
	{
		snprintf(z->err, sizeof(z->err), "a block uses %u coders (filters, BCJ2 and chains are not read)", (unsigned)f.NumCoders);
		return SZ7_UNSUPPORTED;
	}
	{
		const CSzCoderInfo *c = &f.Coders[0];
		out->props = data + c->PropsOffset;
		out->props_size = c->PropsSize;
		if (c->MethodID == 0x21)
		{
			if (c->NumStreams != 1 || c->PropsSize != 1 || out->props[0] > 40)
				return Fail(z, SZ7_HEADER_BAD, "bad LZMA2 properties");
			out->kind = 2;
			out->dict = Lzma2DictFromProp(out->props[0]);
		}
		else if (c->MethodID == 0x30101)
		{
			unsigned lc, lp, d;
			if (c->NumStreams != 1 || c->PropsSize != 5 || out->props[0] >= 9 * 5 * 5)
				return Fail(z, SZ7_HEADER_BAD, "bad LZMA properties");
			d = out->props[0];
			lc = d % 9;
			d /= 9;
			lp = d % 5;
			if (lc + lp > 4) /* what LZMA2 and 7-Zip's own presets allow: the probability table stays at 28 KiB */
				return Fail(z, SZ7_UNSUPPORTED, "unusual LZMA properties (lc + lp over 4)");
			out->kind = 1;
			out->dict = (uint32_t)out->props[1] | ((uint32_t)out->props[2] << 8) | ((uint32_t)out->props[3] << 16) | ((uint32_t)out->props[4] << 24);
			if (out->dict < 4096)
				out->dict = 4096;
		}
		else if (c->MethodID == 0)
			out->kind = 0, out->dict = 0;
		else if (c->MethodID == 0x6F10701)
			return Fail(z, SZ7_ENCRYPTED, "the pack is password-protected");
		else
		{
			snprintf(z->err, sizeof(z->err), "7z method %llx is not read (LZMA2, LZMA and Copy are)", (unsigned long long)c->MethodID);
			return SZ7_UNSUPPORTED;
		}
	}
	out->unpack = SzAr_GetFolderUnpackSize(&z->db.db, fi);
	{
		/* SzArEx_GetFolderStreamPos / SzArEx_GetFolderFullPackSize are declared in 7z.h but defined nowhere in this SDK: the same sums, here */
		const UInt32 psi = z->db.db.FoStartPackStreamIndex[fi];
		const UInt64 ps = z->db.db.PackPositions[psi + 1] - z->db.db.PackPositions[psi];
		if (ps == 0 && out->unpack != 0)
			return Fail(z, SZ7_HEADER_BAD, "a block has no packed data");
		out->pack = ps;
		out->pack_pos = z->db.dataPos + z->db.db.PackPositions[psi];
	}
	if (out->pack_pos > z->in.size || out->pack > z->in.size - out->pack_pos)
		return Fail(z, SZ7_EOF, "a block's data lies outside the file");
	if (out->kind == 0 && out->pack != out->unpack)
		return Fail(z, SZ7_HEADER_BAD, "a stored block's sizes differ");
	return SZ7_OK;
}

static void Sz7_Close(Sz7Archive *z)
{
	if (z->db_open)
		SzArEx_Free(&z->db, &z->a_main.vt);
	z->db_open = 0;
	free(z->lookbuf);
	z->lookbuf = NULL;
	if (z->fd >= 0)
		close(z->fd);
	z->fd = -1;
}

Sz7Archive *Sz7New(void)
{
	Sz7Archive *z = (Sz7Archive *)calloc(1, sizeof(*z));
	if (!z)
		return NULL;
	z->fd = -1;
	Sz7LimitsDefault(&z->lim);
	return z;
}

void Sz7Free(Sz7Archive *z)
{
	if (!z)
		return;
	Sz7_Close(z);
	free(z->wname);
	free(z->name);
	free(z);
}

const char *Sz7Error(const Sz7Archive *z)
{
	return z ? z->err : "";
}

const Sz7Info *Sz7GetInfo(const Sz7Archive *z)
{
	return &z->info;
}

Sz7Code Sz7Open(Sz7Archive *z, const char *path, const Sz7Limits *lim)
{
	Byte start[k7zStartHeaderSize];
	struct stat st;
	SRes r;
	if (!z)
		return SZ7_NO_MEMORY;
	CrcInit();
	Sz7_Close(z);
	memset(&z->info, 0, sizeof(z->info));
	z->err[0] = 0;
	z->lim = *lim;
	z->fd = open(path, O_RDONLY);
	if (z->fd < 0 || fstat(z->fd, &st) != 0)
		return Fail(z, SZ7_IO, "can't open the file");
	z->in.fd = z->fd;
	z->in.size = (uint64_t)st.st_size;
	z->in.pos = 0;
	z->in.vt.Read = PRead_Read;
	z->in.vt.Seek = PRead_Seek;
	/* the start header first: refuse a header the SDK would allocate before it has checked anything but the file size */
	if (pread(z->fd, start, sizeof(start), 0) != (ssize_t)sizeof(start) || memcmp(start, k7zSignature, k7zSignatureSize) != 0)
		return Fail(z, SZ7_NOT_7Z, "not a 7z file");
	if (CrcCalc(start + 12, 20) != ((uint32_t)start[8] | ((uint32_t)start[9] << 8) | ((uint32_t)start[10] << 16) | ((uint32_t)start[11] << 24)))
		return Fail(z, SZ7_HEADER_BAD, "the start header is damaged");
	{
		uint64_t off = 0, size = 0;
		int k;
		for (k = 7; k >= 0; k--)
			off = (off << 8) | start[12 + k], size = (size << 8) | start[20 + k];
		if (size > lim->max_header_bytes)
			return Fail(z, SZ7_HEADER_TOO_BIG, "the header is too big");
		if (z->in.size < 32 || off > z->in.size - 32 || size > z->in.size - 32 - off)
			return Fail(z, SZ7_EOF, "the archive is incomplete");
	}
	z->budget.cap = lim->max_header_mem > (uint64_t)SIZE_MAX / 2 ? SIZE_MAX / 2 : (size_t)lim->max_header_mem;
	z->budget.used = z->budget.peak = 0;
	Alloc_Init(&z->a_main, &z->budget);
	Alloc_Init(&z->a_temp, &z->budget);
	if (!z->wname)
		z->wname = (UInt16 *)malloc(NAME_MAX_UNITS * sizeof(UInt16));
	if (!z->name)
		z->name = (char *)malloc(NAME_BUF);
	z->lookbuf = (Byte *)malloc(LOOK_BUF);
	if (!z->wname || !z->name || !z->lookbuf)
		return Fail(z, SZ7_NO_MEMORY, "out of memory");
	z->look.realStream = &z->in.vt;
	LookToRead2_CreateVTable(&z->look, False);
	z->look.buf = z->lookbuf;
	z->look.bufSize = LOOK_BUF;
	LookToRead2_INIT(&z->look)
	SzArEx_Init(&z->db);
	r = SzArEx_Open(&z->db, &z->look.vt, &z->a_main.vt, &z->a_temp.vt);
	if (r != SZ_OK)
	{
		const Sz7Code c = r == SZ_ERROR_MEM ? SZ7_NO_MEMORY : r == SZ_ERROR_UNSUPPORTED ? SZ7_UNSUPPORTED : r == SZ_ERROR_INPUT_EOF ? SZ7_EOF :
		                  r == SZ_ERROR_READ ? SZ7_IO : r == SZ_ERROR_NO_ARCHIVE ? SZ7_NOT_7Z : r == SZ_ERROR_CRC ? SZ7_CRC : SZ7_HEADER_BAD;
		snprintf(z->err, sizeof(z->err), "the header can't be read (code %d)%s", (int)r, r == SZ_ERROR_MEM ? ": it wants more memory than is allowed for it" : "");
		z->info.header_peak = z->budget.peak;
		/* the SDK frees its own partial state on a failed open */
		z->db_open = 0;
		return c;
	}
	z->db_open = 1;
	z->info.header_peak = z->budget.peak;
	z->info.header_live = z->budget.used;
	z->info.files = z->db.NumFiles;
	z->info.folders = z->db.db.NumFolders;
	if (z->db.NumFiles > lim->max_files || z->db.db.NumFolders > lim->max_files)
		return Fail(z, SZ7_TOO_MANY_FILES, "too many files in the pack");
	/* every block: its method, dictionary and sizes */
	{
		uint32_t fi;
		for (fi = 0; fi < z->db.db.NumFolders; fi++)
		{
			FolderInfo f;
			uint64_t need;
			const Sz7Code c = GetFolder(z, fi, &f);
			if (c != SZ7_OK)
				return c;
			need = f.dict < f.unpack ? f.dict : f.unpack;
			if (f.kind == 0)
				need = 0;
			z->info.pack_total = SatAdd(z->info.pack_total, f.pack);
			if (f.dict > z->info.dict_declared)
				z->info.dict_declared = f.dict;
			if (need > z->info.dict_need)
				z->info.dict_need = need;
			if (fi == 0)
				snprintf(z->info.method, sizeof(z->info.method), "%s dict=%llu", f.kind == 2 ? "LZMA2" : f.kind == 1 ? "LZMA" : "Copy", (unsigned long long)f.dict);
			if (need > lim->max_dict)
			{
				snprintf(z->err, sizeof(z->err), "the pack needs a %llu MiB dictionary to unpack; the limit is %llu MiB", (unsigned long long)((need + 1048575) >> 20),
					(unsigned long long)(lim->max_dict >> 20));
				return SZ7_DICT_TOO_BIG;
			}
		}
	}
	{
		uint32_t i;
		for (i = 0; i < z->db.NumFiles; i++)
		{
			if (SzArEx_IsDir(&z->db, i))
			{
				z->info.dirs++;
				continue;
			}
			{
				const uint64_t s = SzArEx_GetFileSize(&z->db, i);
				z->info.unpack_total = SatAdd(z->info.unpack_total, s);
				if (s > z->info.largest_file)
					z->info.largest_file = s;
			}
		}
	}
	if (z->info.unpack_total > lim->max_total)
	{
		snprintf(z->err, sizeof(z->err), "the pack unpacks to more than %llu MiB; the limit is %llu MiB", (unsigned long long)(z->info.unpack_total >> 20) /* may saturate */,
			(unsigned long long)(lim->max_total >> 20));
		return SZ7_TOO_BIG;
	}
	return SZ7_OK;
}

/* ---------- UTF-16LE -> UTF-8 (strict) ---------- */
static int Utf16ToUtf8(const UInt16 *w, size_t n, char *out, size_t out_cap)
{
	size_t o = 0, i;
	for (i = 0; i < n; i++)
	{
		uint32_t c = w[i];
		if (c >= 0xD800 && c < 0xDC00)
		{
			if (i + 1 >= n || w[i + 1] < 0xDC00 || w[i + 1] > 0xDFFF)
				return -1;
			c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
		}
		else if (c >= 0xDC00 && c <= 0xDFFF)
			return -1;
		if (o + 4 >= out_cap)
			return -1;
		if (c < 0x80)
			out[o++] = (char)c;
		else if (c < 0x800)
			out[o++] = (char)(0xC0 | (c >> 6)), out[o++] = (char)(0x80 | (c & 63));
		else if (c < 0x10000)
			out[o++] = (char)(0xE0 | (c >> 12)), out[o++] = (char)(0x80 | ((c >> 6) & 63)), out[o++] = (char)(0x80 | (c & 63));
		else
			out[o++] = (char)(0xF0 | (c >> 18)), out[o++] = (char)(0x80 | ((c >> 12) & 63)), out[o++] = (char)(0x80 | ((c >> 6) & 63)), out[o++] = (char)(0x80 | (c & 63));
	}
	out[o] = 0;
	return (int)o;
}

/* The file's name as UTF-8 into z->name. 0 when the entry can't be used: no readable name, a link, a device, a reparse point. */
static int FileName(Sz7Archive *z, uint32_t i)
{
	const size_t units = SzArEx_GetFileNameUtf16(&z->db, i, NULL); /* with the terminator */
	if (units < 2 || units > NAME_MAX_UNITS)
		return 0;
	SzArEx_GetFileNameUtf16(&z->db, i, z->wname);
	if (Utf16ToUtf8(z->wname, units - 1, z->name, NAME_BUF) < 0)
		return 0;
	/* an entry that is not a plain file: a Unix mode in the high 16 bits (0x8000 marks it) or a Windows reparse point */
	if (SzBitWithVals_Check(&z->db.Attribs, i))
	{
		const UInt32 a = z->db.Attribs.Vals[i];
		if ((a & 0x8000u) && (((a >> 16) & 0170000u) != 0100000u))
			return 0;
		if (a & 0x400u)
			return 0;
	}
	return 1;
}

Sz7Code Sz7ScanFiles(Sz7Archive *z, Sz7WantFn want, void *ctx, Sz7Scan *out)
{
	uint32_t i;
	memset(out, 0, sizeof(*out));
	if (!z || !z->db_open)
		return SZ7_HEADER_BAD;
	for (i = 0; i < z->db.NumFiles; i++)
	{
		uint64_t size;
		if (SzArEx_IsDir(&z->db, i))
			continue;
		size = SzArEx_GetFileSize(&z->db, i);
		if (!FileName(z, i) || !want(ctx, z->name, size))
		{
			out->skipped_files++;
			out->skipped_bytes = SatAdd(out->skipped_bytes, size);
			continue;
		}
		if (size > z->lim.max_entry)
		{
			snprintf(z->err, sizeof(z->err), "a file in the pack is %llu MiB; the limit is %llu MiB", (unsigned long long)(size >> 20), (unsigned long long)(z->lim.max_entry >> 20));
			return SZ7_ENTRY_TOO_BIG;
		}
		if (size > 0 && !SzBitWithVals_Check(&z->db.CRCs, i))
			return Fail(z, SZ7_NO_CRC, "a file in the pack has no checksum, so nothing could vouch for it");
		out->kept_files++;
		out->kept_bytes = SatAdd(out->kept_bytes, size);
		if (size > out->kept_largest)
			out->kept_largest = size;
	}
	return SZ7_OK;
}

/* ---------- one folder as a stream ---------- */
typedef struct
{
	FolderInfo f;
	CLzma2Dec l2;
	CLzmaDec l1;
	Byte *dic;
	size_t dic_size;
	Byte *in;
	size_t in_pos, in_len;
	uint64_t pack_left, out_left, file_pos;
	int started;
	uint64_t packed_read;
} FolderStream;

static Sz7Code FS_Open(Sz7Archive *z, FolderStream *s, uint32_t fi, Sz7Alloc *probs_alloc)
{
	Sz7Code c;
	memset(s, 0, sizeof(*s));
	Lzma2Dec_CONSTRUCT(&s->l2)
	LzmaDec_CONSTRUCT(&s->l1)
	c = GetFolder(z, fi, &s->f);
	if (c != SZ7_OK)
		return c;
	s->pack_left = s->f.pack;
	s->out_left = s->f.unpack;
	s->file_pos = s->f.pack_pos;
	s->in = (Byte *)malloc(IN_CHUNK);
	if (!s->in)
		return Fail(z, SZ7_NO_MEMORY, "out of memory");
	if (s->f.kind != 0)
	{
		uint64_t need = s->f.dict < s->f.unpack ? s->f.dict : s->f.unpack;
		if (need < 4096)
			need = 4096;
		if (need > z->lim.max_dict)
			return Fail(z, SZ7_DICT_TOO_BIG, "dictionary over the limit");
		s->dic_size = (size_t)need;
		s->dic = (Byte *)malloc(s->dic_size);
		if (!s->dic)
			return Fail(z, SZ7_NO_MEMORY, "out of memory (dictionary)");
		if (s->f.kind == 2)
		{
			if (Lzma2Dec_AllocateProbs(&s->l2, s->f.props[0], &probs_alloc->vt) != SZ_OK)
				return Fail(z, SZ7_NO_MEMORY, "out of memory (LZMA2 state)");
			s->l2.decoder.dic = s->dic;
			s->l2.decoder.dicBufSize = s->dic_size;
			Lzma2Dec_Init(&s->l2);
		}
		else
		{
			if (LzmaDec_AllocateProbs(&s->l1, s->f.props, s->f.props_size, &probs_alloc->vt) != SZ_OK)
				return Fail(z, SZ7_NO_MEMORY, "out of memory (LZMA state)");
			s->l1.dic = s->dic;
			s->l1.dicBufSize = s->dic_size;
			LzmaDec_Init(&s->l1);
		}
	}
	s->started = 1;
	return SZ7_OK;
}

static void FS_Free(FolderStream *s, Sz7Alloc *probs_alloc)
{
	if (s->started && s->f.kind == 2)
		Lzma2Dec_FreeProbs(&s->l2, &probs_alloc->vt);
	else if (s->started && s->f.kind == 1)
		LzmaDec_FreeProbs(&s->l1, &probs_alloc->vt);
	free(s->dic);
	free(s->in);
	memset(s, 0, sizeof(*s));
}

static Sz7Code FS_Fill(Sz7Archive *z, FolderStream *s)
{
	size_t n = IN_CHUNK;
	if (n > s->pack_left)
		n = (size_t)s->pack_left;
	if (n == 0)
		return SZ7_EOF;
	{
		size_t got = 0;
		while (got < n)
		{
			const ssize_t r = pread(z->fd, s->in + got, n - got, (off_t)(s->file_pos + got));
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0)
				return r < 0 ? Fail(z, SZ7_IO, "read error") : Fail(z, SZ7_EOF, "the archive is incomplete");
			got += (size_t)r;
		}
	}
	s->file_pos += n;
	s->pack_left -= n;
	s->packed_read += n;
	s->in_pos = 0;
	s->in_len = n;
	return SZ7_OK;
}

/* up to `want` (<= out_left) bytes into dst; at least one on success */
static Sz7Code FS_Read(Sz7Archive *z, FolderStream *s, Byte *dst, size_t want, size_t *got)
{
	if (want > s->out_left)
		want = (size_t)s->out_left;
	*got = 0;
	if (want == 0)
		return SZ7_OK;
	if (s->f.kind == 0)
	{
		/* stored */
		size_t n = want;
		if (s->in_pos == s->in_len)
		{
			const Sz7Code c = FS_Fill(z, s);
			if (c != SZ7_OK)
				return c;
		}
		if (n > s->in_len - s->in_pos)
			n = s->in_len - s->in_pos;
		memcpy(dst, s->in + s->in_pos, n);
		s->in_pos += n;
		s->out_left -= n;
		*got = n;
		return SZ7_OK;
	}
	for (;;)
	{
		SizeT in_cur = s->in_len - s->in_pos, out_cur = want;
		ELzmaStatus status;
		SRes r;
		if (in_cur == 0 && s->pack_left > 0)
		{
			const Sz7Code c = FS_Fill(z, s);
			if (c != SZ7_OK)
				return c;
			in_cur = s->in_len;
		}
		if (s->f.kind == 2)
			r = Lzma2Dec_DecodeToBuf(&s->l2, dst, &out_cur, s->in + s->in_pos, &in_cur, LZMA_FINISH_ANY, &status);
		else
			r = LzmaDec_DecodeToBuf(&s->l1, dst, &out_cur, s->in + s->in_pos, &in_cur, LZMA_FINISH_ANY, &status);
		s->in_pos += in_cur;
		if (r != SZ_OK)
			return Fail(z, SZ7_DATA, "the compressed data is damaged");
		if (out_cur > 0)
		{
			s->out_left -= out_cur;
			*got = out_cur;
			return SZ7_OK;
		}
		if (status == LZMA_STATUS_FINISHED_WITH_MARK)
			return Fail(z, SZ7_DATA, "the compressed data ends early");
		if (in_cur == 0 && s->in_pos == s->in_len && s->pack_left == 0)
			return Fail(z, SZ7_EOF, "the compressed data is incomplete");
		if (in_cur == 0 && s->in_pos != s->in_len)
			return Fail(z, SZ7_DATA, "the decoder stopped");
	}
}

/* after the last byte: the end marker is read and every packed byte used */
static Sz7Code FS_Finish(Sz7Archive *z, FolderStream *s)
{
	if (s->out_left != 0)
		return Fail(z, SZ7_DATA, "a block was not read to its end");
	if (s->f.kind == 0)
		return s->pack_left == 0 && s->in_pos == s->in_len ? SZ7_OK : Fail(z, SZ7_DATA, "stored block size");
	for (;;)
	{
		SizeT in_cur = s->in_len - s->in_pos, out_cur = 0;
		ELzmaStatus status;
		SRes r;
		Byte dummy;
		if (in_cur == 0 && s->pack_left > 0)
		{
			const Sz7Code c = FS_Fill(z, s);
			if (c != SZ7_OK)
				return c;
			in_cur = s->in_len;
		}
		if (s->f.kind == 2)
			r = Lzma2Dec_DecodeToBuf(&s->l2, &dummy, &out_cur, s->in + s->in_pos, &in_cur, LZMA_FINISH_END, &status);
		else
			r = LzmaDec_DecodeToBuf(&s->l1, &dummy, &out_cur, s->in + s->in_pos, &in_cur, LZMA_FINISH_END, &status);
		s->in_pos += in_cur;
		if (r != SZ_OK || out_cur != 0)
			return Fail(z, SZ7_DATA, "the compressed data is damaged at its end");
		if (status == LZMA_STATUS_FINISHED_WITH_MARK)
			break;
		if (status == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK && s->f.kind == 1 && s->pack_left == 0 && s->in_pos == s->in_len)
			return SZ7_OK; /* LZMA1 without an end marker */
		/* every pass must use input or end: a decoder that stands still with input left is damaged data, never a loop */
		if (in_cur == 0 && s->in_pos < s->in_len && status != LZMA_STATUS_NEEDS_MORE_INPUT)
			return Fail(z, SZ7_DATA, "the compressed data has more after its end");
		if (in_cur == 0 && s->pack_left == 0 && s->in_pos == s->in_len)
			return Fail(z, SZ7_EOF, "the compressed data is incomplete at its end");
	}
	if (s->pack_left != 0 || s->in_pos != s->in_len)
		return Fail(z, SZ7_DATA, "the packed data is longer than the block");
	return SZ7_OK;
}

Sz7Code Sz7Extract(Sz7Archive *z, const Sz7Sink *sink, Sz7Result *res)
{
	FolderStream fs;
	Sz7Budget probs_budget;
	Sz7Alloc probs;
	uint32_t cur_folder = (uint32_t)-1, i;
	uint64_t packed_base = 0; /* packed bytes of the blocks before the current one: progress counts the whole archive */
	Byte *chunk;
	Sz7Code c = SZ7_OK;
	memset(&fs, 0, sizeof(fs));
	memset(res, 0, sizeof(*res));
	if (!z || !z->db_open || z->extracted)
		return SZ7_HEADER_BAD;
	z->extracted = 1;
	probs_budget.cap = 1u << 20; /* the LZMA state is at most 28 KiB; the cap just proves it */
	probs_budget.used = probs_budget.peak = 0;
	Alloc_Init(&probs, &probs_budget);
	chunk = (Byte *)malloc(OUT_CHUNK);
	if (!chunk)
		return Fail(z, SZ7_NO_MEMORY, "out of memory");
	for (i = 0; i < z->db.NumFiles && c == SZ7_OK; i++)
	{
		const uint32_t fold = z->db.FileToFolder[i];
		const uint64_t size = SzArEx_GetFileSize(&z->db, i);
		int keep = 0;
		uint32_t crc = CRC_INIT_VAL;
		uint64_t left;
		char name_copy[64]; /* only for an error message */
		if (SzArEx_IsDir(&z->db, i))
			continue;
		if (fold != (uint32_t)-1 && fold != cur_folder)
		{
			if (cur_folder != (uint32_t)-1)
			{
				if (fold < cur_folder) /* the SDK numbers blocks in file order; anything else is a lie */
				{
					c = Fail(z, SZ7_HEADER_BAD, "the files and blocks are out of order");
					break;
				}
				c = FS_Finish(z, &fs);
				packed_base += fs.packed_read;
				FS_Free(&fs, &probs);
				cur_folder = (uint32_t)-1;
				if (c != SZ7_OK)
					break;
			}
			c = FS_Open(z, &fs, fold, &probs);
			if (c != SZ7_OK)
			{
				FS_Free(&fs, &probs);
				break;
			}
			cur_folder = fold;
		}
		if (FileName(z, i))
		{
			const int b = sink->begin(sink->ctx, z->name, size);
			if (b < 0)
			{
				c = Fail(z, SZ7_SINK, "the consumer stopped");
				break;
			}
			keep = b == 0;
			if (keep && size > z->lim.max_entry)
			{
				c = Fail(z, SZ7_ENTRY_TOO_BIG, "a file in the pack is too big");
				break;
			}
			if (keep && size > 0 && !SzBitWithVals_Check(&z->db.CRCs, i))
			{
				c = Fail(z, SZ7_NO_CRC, "a file in the pack has no checksum");
				break;
			}
		}
		snprintf(name_copy, sizeof(name_copy), "%.40s", keep ? z->name : "a file");
		if (keep)
			res->kept_files++;
		else
			res->skipped_files++;
		left = size;
		while (left > 0)
		{
			size_t n = left > OUT_CHUNK ? OUT_CHUNK : (size_t)left, got = 0;
			if (fold == (uint32_t)-1)
			{
				c = Fail(z, SZ7_HEADER_BAD, "file sizes disagree with the blocks");
				break;
			}
			c = FS_Read(z, &fs, chunk, n, &got);
			if (c != SZ7_OK)
				break;
			crc = CrcUpdate(crc, chunk, got);
			if (keep)
			{
				if (sink->data(sink->ctx, chunk, got) != 0)
				{
					c = Fail(z, SZ7_SINK, "the consumer failed");
					break;
				}
				res->kept_bytes += got;
			}
			left -= got;
			res->packed_done = packed_base + fs.packed_read;
			if (sink->cancelled && sink->cancelled(sink->ctx, res->packed_done))
			{
				c = Fail(z, SZ7_CANCELLED, "cancelled");
				break;
			}
		}
		if (c != SZ7_OK)
			break;
		if (SzBitWithVals_Check(&z->db.CRCs, i) && CRC_GET_DIGEST(crc) != z->db.CRCs.Vals[i])
		{
			snprintf(z->err, sizeof(z->err), "checksum mismatch in %s", name_copy);
			c = SZ7_CRC;
			break;
		}
		if (keep && sink->end(sink->ctx) != 0)
		{
			c = Fail(z, SZ7_SINK, "the consumer failed at the end of a file");
			break;
		}
	}
	if (c == SZ7_OK && cur_folder != (uint32_t)-1)
		c = FS_Finish(z, &fs);
	if (cur_folder != (uint32_t)-1)
	{
		res->packed_done = packed_base + fs.packed_read;
		FS_Free(&fs, &probs);
	}
	free(chunk);
	return c;
}
