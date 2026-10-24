/* PS5 port frontend: a streaming 7z reader for the HD texture packs (2026-10-16, AI-assisted).
 *
 * swordpdf: "we forgot to add the new texture packs links": 192 of the community's Google Drive packs (Bl4ckH4nd's) are .7z files, and the
 * vendored libarchive has no 7z reader. This reads them with the LZMA SDK already vendored in 3rdparty/lzma (public domain), one solid
 * block ("folder") at a time AS A STREAM: the unpacked block is never held whole (the SDK's own SzArEx_Extract would allocate all of it,
 * gigabytes). What the console needs for a pack, at most:
 *
 *   dictionary   min(the stream's declared dictionary, the block's unpacked size), refused above Sz7Limits::max_dict (64 MiB)
 *   chunks       1 MiB of output + 256 KiB of packed input
 *   LZMA state   at most 28 KiB (LZMA2 and LZMA with lc + lp <= 4)
 *   the header   what the SDK holds for the file table, all of it under Sz7Limits::max_header_mem (48 MiB; about 0.6 to 0.8 times the
 *                decoded header while it is kept, about 1.8 times while it is read; the biggest header seen decodes to 7.2 MB)
 *
 * Only LZMA2, LZMA and stored blocks, one coder a block (what 7-Zip makes for these packs). Filters (BCJ ...), PPMd, bzip2, a second
 * coder and encryption are refused with a message, never guessed at. A 7z from the internet is untrusted: the header's sizes and
 * counts are bounded before anything is allocated for them, names are checked, every file's CRC is checked, and a pack whose wanted
 * files carry no CRC is refused.
 *
 * Plain C (built with the SDK's files by Makefile.vk and the host test scripts); the C++ caller is fe_texpacks.cpp (Unpack7z).
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FE_SEVENZIP_H
#define FE_SEVENZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	SZ7_OK = 0,
	SZ7_NOT_7Z,
	SZ7_HEADER_BAD,       /* damaged header */
	SZ7_HEADER_TOO_BIG,   /* the end header is bigger than Sz7Limits::max_header_bytes */
	SZ7_TOO_MANY_FILES,   /* more entries than Sz7Limits::max_files */
	SZ7_UNSUPPORTED,      /* a method that isn't read (filters, PPMd, bzip2, a second coder ...) */
	SZ7_ENCRYPTED,        /* a password-protected block */
	SZ7_DICT_TOO_BIG,     /* a block needs a bigger dictionary buffer than Sz7Limits::max_dict */
	SZ7_NO_MEMORY,        /* out of memory, or the header wants more than Sz7Limits::max_header_mem */
	SZ7_IO,               /* a read error */
	SZ7_DATA,             /* the compressed data is damaged */
	SZ7_CRC,              /* a file's CRC is wrong */
	SZ7_EOF,              /* the archive is incomplete */
	SZ7_ENTRY_TOO_BIG,    /* a wanted file is bigger than Sz7Limits::max_entry */
	SZ7_SINK,             /* the consumer refused or failed (disk full ...) */
	SZ7_CANCELLED,        /* the consumer's cancelled() said stop */
	SZ7_TOO_BIG,          /* the files add up to more than Sz7Limits::max_total */
	SZ7_NO_CRC            /* a wanted file with data has no CRC */
} Sz7Code;

/* "NOT_7Z", "DATA" ...: for logs and tests. */
const char *Sz7CodeName(Sz7Code code);

typedef struct
{
	uint64_t max_dict;         /* the dictionary buffer a block may need: min(declared, unpacked size) */
	uint64_t max_header_bytes; /* the end header as stored (packed) */
	uint64_t max_header_mem;   /* everything the SDK allocates while it reads the header, and the file table kept afterwards */
	uint32_t max_files;        /* entries, and blocks */
	uint64_t max_entry;        /* one wanted file */
	uint64_t max_total;        /* all the files together, wanted or not: the decoder's work, bounded (summed saturating) */
} Sz7Limits;

/* 64 MiB, 16 MiB, 48 MiB, 65536, 256 MiB, 12 GiB. */
void Sz7LimitsDefault(Sz7Limits *limits);

typedef struct
{
	uint32_t files, dirs, folders;
	uint64_t pack_total;       /* the blocks' packed bytes together */
	uint64_t unpack_total;     /* every file's bytes together (saturating) */
	uint64_t largest_file;
	uint64_t dict_declared;    /* the biggest dictionary any block declares */
	uint64_t dict_need;        /* the biggest dictionary BUFFER any block needs: min(declared, unpacked) */
	uint64_t header_peak;      /* the most the header phase held at once */
	uint64_t header_live;      /* what stays allocated for the file table while the archive is open */
	char method[64];           /* the first block's coder: "LZMA2 dict=16777216" */
} Sz7Info;

typedef struct Sz7Archive Sz7Archive;

/* Always returns a handle (NULL only when out of memory); Sz7Free closes whatever is open. */
Sz7Archive *Sz7New(void);
void Sz7Free(Sz7Archive *z);

/* Opens the archive at `path` and reads its header under `limits`: SZ7_OK, or what is wrong (Sz7Error says it in words). The file stays open until Sz7Free. */
Sz7Code Sz7Open(Sz7Archive *z, const char *path, const Sz7Limits *limits);
const char *Sz7Error(const Sz7Archive *z);
const Sz7Info *Sz7GetInfo(const Sz7Archive *z);

/* The files the consumer wants: called once per file (not folders) whose name is a readable relative path, utf8 and its size; 1 keeps it, 0 leaves it out. */
typedef int (*Sz7WantFn)(void *ctx, const char *utf8, uint64_t size);

typedef struct
{
	uint64_t kept_files, kept_bytes, skipped_files, skipped_bytes;
	uint64_t kept_largest;
} Sz7Scan;

/* A pass over the file list with no decoding: what Sz7Extract will be asked for. SZ7_ENTRY_TOO_BIG for a wanted file over max_entry, SZ7_NO_CRC for a wanted
 * file with data and no CRC (nothing could vouch for it). */
Sz7Code Sz7ScanFiles(Sz7Archive *z, Sz7WantFn want, void *ctx, Sz7Scan *out);

typedef struct
{
	/* a file starts: 0 keep it, 1 leave it out (it is still decoded: the block is one stream, and checked), < 0 abort */
	int (*begin)(void *ctx, const char *utf8, uint64_t size);
	/* the kept file's next bytes, in order; nonzero aborts (the writer failed) */
	int (*data)(void *ctx, const uint8_t *p, size_t n);
	/* the kept file is complete and its CRC matched; nonzero aborts */
	int (*end)(void *ctx);
	/* after every chunk, with the packed bytes read so far in all; nonzero cancels. May be NULL. */
	int (*cancelled)(void *ctx, uint64_t packed_done);
	void *ctx;
} Sz7Sink;

typedef struct
{
	uint64_t kept_files, kept_bytes, skipped_files;
	uint64_t packed_done;
} Sz7Result;

/* Decodes every block in order, cuts the files out, checks each file's CRC and each block's end. Once per handle. */
Sz7Code Sz7Extract(Sz7Archive *z, const Sz7Sink *sink, Sz7Result *result);

#ifdef __cplusplus
}
#endif

#endif
