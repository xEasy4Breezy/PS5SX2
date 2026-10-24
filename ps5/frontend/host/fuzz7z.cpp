// PS5 port frontend: a libFuzzer harness for the 7z reader (fe_sevenzip.c), on a PC (2026-10-16, AI-assisted). test-sevenzip.sh builds and runs it when SEVENZIP_LIBFUZZER=<seconds>
// is set (clang with libFuzzer, the sanitizers on). The input is a whole .7z. The two CRCs of the start header are repaired first (its own, and the end header's when that
// lies inside the input as a PLAIN header), so mutated bytes reach the SDK's parser instead of stopping at its CRC gate. Every input is opened, scanned and extracted with a
// consumer that takes everything (so the decoders run), under the production limits but a smaller header budget and dictionary (oversize claims are cheap to refuse).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_sevenzip.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>
#include <zlib.h>

static int Begin(void*, const char*, uint64_t) { return 0; }
static int Data(void*, const uint8_t*, size_t) { return 0; }
static int End(void*) { return 0; }
static int Want(void*, const char*, uint64_t) { return 1; }

static uint32_t Crc(const uint8_t* p, size_t n)
{
	return static_cast<uint32_t>(crc32(0, p, static_cast<uInt>(n)));
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size < 32 || size > (8u << 20))
		return 0;
	std::vector<uint8_t> a(data, data + size);
	static const uint8_t sig[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C}; // (the signature stays: otherwise nothing is parsed)
	std::memcpy(a.data(), sig, 6);
	uint64_t off = 0, hsize = 0;
	for (int k = 7; k >= 0; k--)
		off = (off << 8) | a[12 + k], hsize = (hsize << 8) | a[20 + k];
	if (off <= size - 32 && hsize <= size - 32 - off)
	{
		const uint32_t c = Crc(a.data() + 32 + off, static_cast<size_t>(hsize));
		std::memcpy(a.data() + 28, &c, 4); // (little endian host)
	}
	const uint32_t sc = Crc(a.data() + 12, 20);
	std::memcpy(a.data() + 8, &sc, 4);
	char path[64];
	std::snprintf(path, sizeof(path), "/dev/shm/fuzz7z-%d.7z", static_cast<int>(getpid()));
	FILE* f = std::fopen(path, "wb");
	if (!f)
		return 0;
	std::fwrite(a.data(), 1, a.size(), f);
	std::fclose(f);
	Sz7Limits lim;
	Sz7LimitsDefault(&lim);
	lim.max_header_mem = 8u << 20;
	lim.max_total = 1ull << 30;
	lim.max_dict = 16u << 20;
	Sz7Archive* z = Sz7New();
	if (Sz7Open(z, path, &lim) == SZ7_OK)
	{
		Sz7Scan scan;
		Sz7ScanFiles(z, Want, nullptr, &scan);
		Sz7Sink sink = {Begin, Data, End, nullptr, nullptr};
		Sz7Result res;
		Sz7Extract(z, &sink, &res);
	}
	Sz7Free(z);
	unlink(path);
	return 0;
}
