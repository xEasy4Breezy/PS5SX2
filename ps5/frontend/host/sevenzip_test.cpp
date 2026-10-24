// PS5 port frontend: a command-line driver for the streaming 7z reader (fe_sevenzip.c), on a PC (2026-10-16, AI-assisted).
// test-sevenzip.sh builds it with the LZMA SDK's files and the sanitizers; sevenzip_check.py runs it over a corpus of generated
// archives (make_7z_fixtures.py), a mutation fuzz, and the memory-limited runs.
//
//   sevenzip_test <archive.7z> [--max-dict-mib N] [--max-hdr-mib N] [--max-files N] [--max-entry-mib N] [--max-total-mib N]
//                 [--out DIR] [--quiet] [--cancel-after-bytes N] [--sink-fail-after-bytes N] [--skip SUBSTRING] [--scan-only] [--names N]
//
// Prints   INFO ...                       one line: files, blocks, sizes, dictionary, header memory
//          F <size> <crc32 hex> <name>    for every kept file (unless --quiet), the CRC from zlib, not the SDK
//          NAME <name>                    the first N names, with --names N (the audit tool: tools/audit_7z_headers.py)
//          SCAN kept=N bytes=B skipped=S ...   the pass over the file list
//          SERIALS SLUS-20776,SLUS-21362  every serial-looking folder name in the archive's paths ("-" for none)
//          REPL files=N bytes=B           the files under a replacements/ folder
//          TEXTURES serials=SLUS-20776 files=N bytes=B noserial=M   what the app takes as textures: <first serial-looking folder>/replacements/<file> (the serials), or
//                                         a replacements/<file> with no serial-looking folder at all (noserial: installed under the game's own serial)
//          OK files=N bytes=B skipped=S packed_done=P   |   ERR <CODE NAME> <text>        (exit status 0 / 2)
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_sevenzip.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>
#include <zlib.h>

namespace
{
struct Driver
{
	bool quiet = false;
	std::string out_dir, skip;
	FILE* f = nullptr;
	uLong crc = 0;
	uint64_t size = 0;
	std::string cur;
	uint64_t cancel_after = 0, fail_after = 0, bytes_given = 0;
	uint64_t files = 0, bytes = 0;
	// what the file list says (every file, wanted or not)
	int names = 0;
	std::set<std::string> serials;
	uint64_t repl_files = 0, repl_bytes = 0;
	// (review) what the APP would take as a texture (fe_texpacks.cpp TexturePackTarget + SplitReplacement): <first serial-looking folder>/replacements/<something>; with
	// no serial-looking folder at all, any replacements/<something> (it goes under the game the pack is installed for)
	std::set<std::string> tex_serials;
	uint64_t tex_files = 0, tex_bytes = 0, tex_noserial = 0;

	static bool IsSerial(const std::string& s)
	{
		if (s.size() != 10 || s[4] != '-')
			return false;
		for (size_t i = 0; i < 10; i++)
			if (i != 4 && !(i < 4 ? ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) : (s[i] >= '0' && s[i] <= '9')))
				return false;
		return true;
	}

	void Textures(const char* name, uint64_t sz)
	{
		std::vector<std::string> parts;
		for (const char* q = name; *q;)
		{
			const char* e = std::strchr(q, '/');
			const size_t l = e ? static_cast<size_t>(e - q) : std::strlen(q);
			if (l > 0 && !(l == 1 && q[0] == '.'))
				parts.emplace_back(q, l);
			if (!e)
				break;
			q = e + 1;
		}
		size_t first = parts.size();
		for (size_t i = 0; i < parts.size() && first == parts.size(); i++)
			if (IsSerial(parts[i]))
				first = i;
		if (first < parts.size())
		{
			if (first + 2 < parts.size() && strcasecmp(parts[first + 1].c_str(), "replacements") == 0)
			{
				std::string s = parts[first];
				for (char& c : s)
					c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
				tex_serials.insert(s);
				tex_files++;
				tex_bytes += sz;
			}
			return;
		}
		for (size_t i = 0; i + 1 < parts.size(); i++)
			if (strcasecmp(parts[i].c_str(), "replacements") == 0)
			{
				tex_noserial++;
				tex_files++;
				tex_bytes += sz;
				return;
			}
	}

	void Note(const char* name, uint64_t sz)
	{
		if (names > 0)
		{
			std::printf("NAME %s\n", name);
			names--;
		}
		Textures(name, sz);
		bool repl = false;
		const char* p = name;
		while (*p)
		{
			const char* e = std::strchr(p, '/');
			const size_t l = e ? static_cast<size_t>(e - p) : std::strlen(p);
			if (l == 10 && p[4] == '-')
			{
				bool ok = true;
				for (size_t i = 0; i < 10; i++)
					ok = ok && (i == 4 || (i < 4 ? ((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= 'a' && p[i] <= 'z')) : (p[i] >= '0' && p[i] <= '9')));
				if (ok)
				{
					std::string s(p, 10);
					for (char& c : s)
						c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
					serials.insert(s);
				}
			}
			if (l == 12 && strncasecmp(p, "replacements", 12) == 0)
				repl = true;
			if (!e)
				break;
			p = e + 1;
		}
		if (repl)
		{
			repl_files++;
			repl_bytes += sz;
		}
	}
};

bool SafeName(const char* n)
{
	const char* p = n;
	if (!*n || *n == '/')
		return false;
	while (*p)
	{
		const char* e = std::strchr(p, '/');
		const size_t l = e ? static_cast<size_t>(e - p) : std::strlen(p);
		if (l == 2 && p[0] == '.' && p[1] == '.')
			return false;
		for (size_t k = 0; k < l; k++)
			if (static_cast<unsigned char>(p[k]) < 0x20 || p[k] == '\\')
				return false;
		if (!e)
			break;
		p = e + 1;
	}
	return true;
}

// the file system's name limit is the driver's problem, not the pack's
bool Wanted(const Driver& d, const char* name)
{
	if (!SafeName(name))
		return false;
	if (!d.skip.empty() && std::strstr(name, d.skip.c_str()))
		return false;
	return !(!d.out_dir.empty() && std::strlen(name) > 200);
}

int Begin(void* ctx, const char* name, uint64_t size)
{
	Driver* d = static_cast<Driver*>(ctx);
	if (!Wanted(*d, name))
		return 1;
	d->cur = name;
	d->crc = crc32(0, nullptr, 0);
	d->size = 0;
	(void)size;
	d->f = nullptr;
	if (!d->out_dir.empty())
	{
		std::string path = d->out_dir + "/" + name;
		for (size_t i = d->out_dir.size() + 1; i < path.size(); i++)
			if (path[i] == '/')
			{
				path[i] = 0;
				mkdir(path.c_str(), 0777);
				path[i] = '/';
			}
		d->f = std::fopen(path.c_str(), "wb");
		if (!d->f)
			return -1;
	}
	return 0;
}

int Data(void* ctx, const uint8_t* p, size_t n)
{
	Driver* d = static_cast<Driver*>(ctx);
	d->crc = crc32(d->crc, p, static_cast<uInt>(n));
	d->size += n;
	d->bytes_given += n;
	if (d->fail_after && d->bytes_given >= d->fail_after)
		return 1;
	return d->f && std::fwrite(p, 1, n, d->f) != n ? 1 : 0;
}

int End(void* ctx)
{
	Driver* d = static_cast<Driver*>(ctx);
	if (d->f)
		std::fclose(d->f);
	d->f = nullptr;
	if (!d->quiet)
		std::printf("F %llu %08x %s\n", static_cast<unsigned long long>(d->size), static_cast<unsigned>(d->crc), d->cur.c_str());
	d->files++;
	d->bytes += d->size;
	return 0;
}

int Cancelled(void* ctx, uint64_t packed)
{
	Driver* d = static_cast<Driver*>(ctx);
	return d->cancel_after && packed >= d->cancel_after;
}

int Want(void* ctx, const char* name, uint64_t size)
{
	Driver* d = static_cast<Driver*>(ctx);
	d->Note(name, size);
	return Wanted(*d, name) ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
	Sz7Limits lim;
	Sz7LimitsDefault(&lim);
	Driver d;
	const char* path = nullptr;
	bool scan_only = false;
	for (int i = 1; i < argc; i++)
	{
		const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
		if (!std::strcmp(argv[i], "--max-dict-mib"))
			lim.max_dict = std::strtoull(next(), nullptr, 10) << 20;
		else if (!std::strcmp(argv[i], "--max-hdr-mib"))
			lim.max_header_mem = std::strtoull(next(), nullptr, 10) << 20;
		else if (!std::strcmp(argv[i], "--max-files"))
			lim.max_files = static_cast<uint32_t>(std::strtoul(next(), nullptr, 10));
		else if (!std::strcmp(argv[i], "--max-entry-mib"))
			lim.max_entry = std::strtoull(next(), nullptr, 10) << 20;
		else if (!std::strcmp(argv[i], "--max-total-mib"))
			lim.max_total = std::strtoull(next(), nullptr, 10) << 20;
		else if (!std::strcmp(argv[i], "--out"))
			d.out_dir = next();
		else if (!std::strcmp(argv[i], "--quiet"))
			d.quiet = true;
		else if (!std::strcmp(argv[i], "--cancel-after-bytes"))
			d.cancel_after = std::strtoull(next(), nullptr, 10);
		else if (!std::strcmp(argv[i], "--sink-fail-after-bytes"))
			d.fail_after = std::strtoull(next(), nullptr, 10);
		else if (!std::strcmp(argv[i], "--skip"))
			d.skip = next();
		else if (!std::strcmp(argv[i], "--scan-only"))
			scan_only = true;
		else if (!std::strcmp(argv[i], "--names"))
			d.names = std::atoi(next());
		else
			path = argv[i];
	}
	if (!path)
	{
		std::fprintf(stderr, "usage: sevenzip_test <archive.7z> [options]\n");
		return 64;
	}
	Sz7Archive* z = Sz7New();
	if (!z)
		return 3;
	const Sz7Code open = Sz7Open(z, path, &lim);
	const Sz7Info* info = Sz7GetInfo(z);
	if (open != SZ7_OK)
	{
		std::printf("ERR %s %s\n", Sz7CodeName(open), Sz7Error(z));
		Sz7Free(z);
		return 2;
	}
	std::printf("INFO files=%u dirs=%u folders=%u pack=%llu unpack=%llu largest=%llu dict_declared=%llu dict_need=%llu header_peak=%llu header_live=%llu method=\"%s\"\n",
		info->files, info->dirs, info->folders, static_cast<unsigned long long>(info->pack_total), static_cast<unsigned long long>(info->unpack_total),
		static_cast<unsigned long long>(info->largest_file), static_cast<unsigned long long>(info->dict_declared), static_cast<unsigned long long>(info->dict_need),
		static_cast<unsigned long long>(info->header_peak), static_cast<unsigned long long>(info->header_live), info->method);
	std::fflush(stdout);
	Sz7Scan scan;
	const Sz7Code sc = Sz7ScanFiles(z, Want, &d, &scan);
	if (sc != SZ7_OK)
	{
		std::printf("ERR %s %s\n", Sz7CodeName(sc), Sz7Error(z));
		Sz7Free(z);
		return 2;
	}
	std::printf("SCAN kept=%llu bytes=%llu skipped=%llu skipped_bytes=%llu largest=%llu\n", static_cast<unsigned long long>(scan.kept_files),
		static_cast<unsigned long long>(scan.kept_bytes), static_cast<unsigned long long>(scan.skipped_files),
		static_cast<unsigned long long>(scan.skipped_bytes), static_cast<unsigned long long>(scan.kept_largest));
	{
		std::string list;
		for (const std::string& sv : d.serials)
			list += (list.empty() ? "" : ",") + sv;
		std::printf("SERIALS %s\nREPL files=%llu bytes=%llu\n", list.empty() ? "-" : list.c_str(), static_cast<unsigned long long>(d.repl_files),
			static_cast<unsigned long long>(d.repl_bytes));
		std::string tex;
		for (const std::string& sv : d.tex_serials)
			tex += (tex.empty() ? "" : ",") + sv;
		std::printf("TEXTURES serials=%s files=%llu bytes=%llu noserial=%llu\n", tex.empty() ? "-" : tex.c_str(), static_cast<unsigned long long>(d.tex_files),
			static_cast<unsigned long long>(d.tex_bytes), static_cast<unsigned long long>(d.tex_noserial));
	}
	if (scan_only)
	{
		Sz7Free(z);
		return 0;
	}
	Sz7Sink sink;
	sink.begin = Begin;
	sink.data = Data;
	sink.end = End;
	sink.cancelled = Cancelled;
	sink.ctx = &d;
	Sz7Result res;
	const Sz7Code c = Sz7Extract(z, &sink, &res);
	if (d.f)
		std::fclose(d.f);
	if (c != SZ7_OK)
	{
		std::printf("ERR %s %s\n", Sz7CodeName(c), Sz7Error(z));
		Sz7Free(z);
		return 2;
	}
	std::printf("OK files=%llu bytes=%llu skipped=%llu packed_done=%llu\n", static_cast<unsigned long long>(res.kept_files),
		static_cast<unsigned long long>(res.kept_bytes), static_cast<unsigned long long>(res.skipped_files), static_cast<unsigned long long>(res.packed_done));
	Sz7Free(z);
	return 0;
}
