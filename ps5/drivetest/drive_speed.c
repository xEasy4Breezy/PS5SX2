// PS5SX2 (2.11, AI-assisted): the drive speed test payload. swordpdf: "we need to make elfs that test their drives max speeds (both internal and external)".
//
// An ELF for the console's ELF loader (port 9021, like the disc helper). It looks for the storage the console has, runs the same short tests on each
// and writes what it found to /data/PCSX2/logs/drive-speed.txt (and says the headline numbers in notifications). Three builds of this one file:
//   SCOPE=1  internal: the console's own SSD (/data)
//   SCOPE=2  external: USB drives (/mnt/usb0..7) and extended M.2 storage (/mnt/ext0, /mnt/ext1)
//   SCOPE=3  all of them
//
// What a drive is asked (about 75 s each; every phase is time-boxed, so a slow drive costs time, never a hang; files only in <drive>/PS5SX2-speedtest,
// all removed at the end, and nothing is written where less than ~1 GiB is free):
//   write      one file written 8 MiB at a time for 12 s, the speed second by second (a drive that gets slower after some seconds shows it), then what the
//              flush at the end took
//   block size 64 KiB, 256 KiB, 1 MiB and 32 MiB writes for 4 s each; 8 MiB with a flush after every buffer; 8 MiB with O_DIRECT (no file cache)
//   read       the file just written, read back (the file cache may hold it: "buffered"), and read with O_DIRECT (the drive's own speed)
//   cold read  the largest file already on the drive (a game, a download), read from its middle: what a game start gets
//   random     4 KiB reads at random places of that file, one at a time: reads a second and how long each took (an SSD or a hard disk)
//   small      1,000 files of 16 KiB created, then deleted: files a second (what a pack of thousands of textures costs the file system)
//
// Why: the texture pack unpacking sat at a flat 2.0 MB/s on some consoles while a payload wrote 256 MB/s to /data (the same drive): this says what each
// drive can do from a payload, so the numbers can be set against what PS5SX2 itself gets.
//
// Built with the PS5 payload SDK (build-drivetest.sh); with -DHOST_TEST it runs on a PC against folders (test-drivetest.sh).
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HOST_TEST
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HOST_TEST
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef HOST_TEST
#include <sys/sysctl.h>
#endif

#ifndef SCOPE
#define SCOPE 3
#endif
#define BUILD_NAME "drive test 1"

#define MIB (1024ull * 1024ull)
#define GIB (1024ull * MIB)
#define TEST_DIR "PS5SX2-speedtest"
#define MAX_TARGETS 12

#ifndef O_DIRECT
#define O_DIRECT 0
#endif

#ifdef HOST_TEST
static void notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char* fmt, ...)
{
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("[popup] %s\n", buf);
}
#else
typedef struct
{
	char unused[45];
	char message[3075];
} notify_req_t;
int sceKernelSendNotificationRequest(int32_t device, void* req, size_t size, int32_t blocking);
static void notify(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char* fmt, ...)
{
	static notify_req_t req;
	memset(&req, 0, sizeof(req));
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(req.message, sizeof(req.message), fmt, ap);
	va_end(ap);
	sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
#endif

// ---------------------------------------------------------------------------------------------------------------------------------------------
// log

static FILE* g_log;
static char g_log_path[256] = "/data/PCSX2/logs/drive-speed.txt";
static double g_scale = 1.0; // DRIVETEST_SCALE: the phases' lengths times this (the PC test shortens them)

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...)
{
	char buf[700];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	fputc('\n', stdout);
	fflush(stdout);
	if (g_log)
	{
		fputs(buf, g_log);
		fputc('\n', g_log);
		fflush(g_log);
	}
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return g_rng;
}

static void mkdirs(const char* path)
{
	char tmp[300];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char* p = tmp + 1; *p; p++)
		if (*p == '/')
		{
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	mkdir(tmp, 0777);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// the drives

struct target
{
	char root[64];  // the mount point
	char label[96]; // for the log
	char fstype[24];
	uint64_t total, avail;
	int external;
};

static int g_n;
static struct target g_t[MAX_TARGETS];

static int add_target(const char* root, const char* label, int external, int must_be_mount)
{
	struct statfs sf;
	if (g_n >= MAX_TARGETS || statfs(root, &sf) != 0)
		return 0;
#ifdef __FreeBSD__
	if (must_be_mount && strcmp(sf.f_mntonname, root) != 0)
		return 0; // the folder exists but is no drive of its own
#else
	(void)must_be_mount;
#endif
	if (sf.f_blocks == 0)
		return 0;
	struct target* t = &g_t[g_n++];
	snprintf(t->root, sizeof(t->root), "%s", root);
	snprintf(t->label, sizeof(t->label), "%s", label);
#ifdef __FreeBSD__
	snprintf(t->fstype, sizeof(t->fstype), "%s", sf.f_fstypename);
#else
	snprintf(t->fstype, sizeof(t->fstype), "?");
#endif
	t->total = (uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize;
	t->avail = (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
	t->external = external;
	return 1;
}

static void discover(void)
{
#ifdef HOST_TEST
	// PC test: DRIVETEST_INTERNAL=<folder>, DRIVETEST_EXTERNAL=<folder>,<folder>...
	const char* in = getenv("DRIVETEST_INTERNAL");
	const char* ex = getenv("DRIVETEST_EXTERNAL");
	if (SCOPE & 1 && in && *in)
		add_target(in, "internal (test folder)", 0, 0);
	if (SCOPE & 2 && ex && *ex)
	{
		char list[512];
		snprintf(list, sizeof(list), "%s", ex);
		int k = 0;
		for (char* tok = strtok(list, ","); tok; tok = strtok(NULL, ","))
		{
			char label[96];
			snprintf(label, sizeof(label), "external %d (test folder)", k++);
			add_target(tok, label, 1, 0);
		}
	}
#else
	if (SCOPE & 1)
		add_target("/data", "internal storage (/data)", 0, 0);
	if (SCOPE & 2)
	{
		for (int i = 0; i < 8; i++)
		{
			char root[32], label[64];
			snprintf(root, sizeof(root), "/mnt/usb%d", i);
			snprintf(label, sizeof(label), "USB drive %d (%s)", i, root);
			add_target(root, label, 1, 1);
		}
		for (int i = 0; i < 2; i++)
		{
			char root[32], label[64];
			snprintf(root, sizeof(root), "/mnt/ext%d", i);
			snprintf(label, sizeof(label), "extended storage %d (%s)", i, root);
			add_target(root, label, 1, 1);
		}
	}
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// measuring

struct run
{
	uint64_t bytes;
	double secs;       // writing or reading
	double sync_secs;  // the flush after a write
	double mbps;       // bytes / secs
	double mbps_synced; // bytes / (secs + flush)
	double min_mbps, max_mbps; // over the whole seconds
	double first2, last3;      // the speed over the first two and the last three seconds (the slow-down check)
	int err;           // errno of the first failure, 0 when none
};

static uint8_t* alloc_buffer(size_t bytes)
{
	void* p = NULL;
	if (posix_memalign(&p, 4096, bytes) != 0)
		return NULL;
	uint64_t* w = (uint64_t*)p;
	for (size_t i = 0; i < bytes / 8; i++)
		w[i] = rnd(); // not zeros: a drive that squeezes would otherwise flatter itself
	return (uint8_t*)p;
}

// Per-second speed bookkeeping shared by the loops.
struct ticks
{
	double t0, last_t;
	uint64_t last_bytes;
	double sum_first2, sum_last3[3];
	int n;
	double min_mbps, max_mbps;
	int timeline;
	const char* name;
};

static void ticks_start(struct ticks* k, const char* name, int timeline)
{
	memset(k, 0, sizeof(*k));
	k->t0 = k->last_t = now();
	k->min_mbps = 1e18;
	k->timeline = timeline;
	k->name = name;
}

static void ticks_add(struct ticks* k, uint64_t bytes_now)
{
	const double t = now();
	if (t - k->last_t < (g_scale < 1.0 ? (g_scale < 0.05 ? 0.05 : g_scale) : 1.0)) // a second (the PC test shortens it with the phases)
		return;
	const double mbps = (double)(bytes_now - k->last_bytes) / 1e6 / (t - k->last_t);
	k->n++;
	if (mbps < k->min_mbps)
		k->min_mbps = mbps;
	if (mbps > k->max_mbps)
		k->max_mbps = mbps;
	if (k->n <= 2)
		k->sum_first2 += mbps;
	k->sum_last3[(k->n - 1) % 3] = mbps;
	if (k->timeline)
		say("      %s second %2d: %8.1f MB/s", k->name, k->n, mbps);
	k->last_t = t;
	k->last_bytes = bytes_now;
}

static void ticks_finish(const struct ticks* k, struct run* r)
{
	r->min_mbps = k->n ? k->min_mbps : r->mbps;
	r->max_mbps = k->n ? k->max_mbps : r->mbps;
	r->first2 = k->n >= 2 ? k->sum_first2 / 2.0 : r->mbps;
	r->last3 = 0;
	const int m = k->n < 3 ? k->n : 3;
	for (int i = 0; i < m; i++)
		r->last3 += k->sum_last3[i];
	r->last3 = m ? r->last3 / m : r->mbps;
}

// One file written `bs` bytes at a time for `secs` seconds (or `cap` bytes). `extra`: more open flags (O_DIRECT); sync_each: fdatasync after each buffer.
static void seq_write(const char* path, size_t bs, double secs, uint64_t cap, int extra, int sync_each, int timeline, struct run* r)
{
	memset(r, 0, sizeof(*r));
	uint8_t* buf = alloc_buffer(bs);
	if (!buf)
	{
		r->err = ENOMEM;
		return;
	}
	const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | extra, 0666);
	if (fd < 0)
	{
		r->err = errno;
		free(buf);
		return;
	}
	struct ticks k;
	ticks_start(&k, "write", timeline);
	const double limit = secs * g_scale;
	uint64_t done = 0;
	while (now() - k.t0 < limit && done < cap)
	{
		size_t off = 0;
		while (off < bs)
		{
			const ssize_t w = write(fd, buf + off, bs - off);
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
			{
				r->err = w < 0 ? errno : EIO;
				break;
			}
			off += (size_t)w;
			done += (uint64_t)w;
		}
		if (r->err)
			break;
		if (sync_each && fdatasync(fd) != 0 && errno != EINVAL)
		{
			r->err = errno;
			break;
		}
		ticks_add(&k, done);
	}
	r->secs = now() - k.t0;
	const double s0 = now();
	fsync(fd);
	r->sync_secs = now() - s0;
	close(fd);
	free(buf);
	r->bytes = done;
	r->mbps = r->secs > 0 ? (double)done / 1e6 / r->secs : 0;
	r->mbps_synced = (r->secs + r->sync_secs) > 0 ? (double)done / 1e6 / (r->secs + r->sync_secs) : 0;
	ticks_finish(&k, r);
}

// A file read `bs` bytes at a time from `start` for `secs` seconds (or `cap` bytes, or its end).
static void seq_read(const char* path, size_t bs, uint64_t start, double secs, uint64_t cap, int extra, int timeline, struct run* r)
{
	memset(r, 0, sizeof(*r));
	uint8_t* buf = alloc_buffer(bs);
	if (!buf)
	{
		r->err = ENOMEM;
		return;
	}
	const int fd = open(path, O_RDONLY | extra);
	if (fd < 0)
	{
		r->err = errno;
		free(buf);
		return;
	}
	if (start && lseek(fd, (off_t)start, SEEK_SET) < 0)
	{
		r->err = errno;
		close(fd);
		free(buf);
		return;
	}
	struct ticks k;
	ticks_start(&k, "read", timeline);
	const double limit = secs * g_scale;
	uint64_t done = 0;
	while (now() - k.t0 < limit && done < cap)
	{
		const ssize_t n = read(fd, buf, bs);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
		{
			r->err = errno;
			break;
		}
		if (n == 0)
			break; // the end of the file
		done += (uint64_t)n;
		ticks_add(&k, done);
	}
	r->secs = now() - k.t0;
	close(fd);
	free(buf);
	r->bytes = done;
	r->mbps = r->secs > 0 ? (double)done / 1e6 / r->secs : 0;
	r->mbps_synced = r->mbps;
	ticks_finish(&k, r);
}

static int cmp_double(const void* a, const void* b)
{
	const double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y;
}

// 4 KiB reads at random aligned places of `path`, one at a time, for `secs` seconds.
static void rand_read(const char* path, uint64_t size, double secs, double* iops, double* avg_us, double* p99_us, int* err)
{
	*iops = *avg_us = *p99_us = 0;
	*err = 0;
	const int fd = open(path, O_RDONLY);
	if (fd < 0)
	{
		*err = errno;
		return;
	}
	uint8_t* buf = alloc_buffer(4096);
	const uint64_t blocks = size / 4096;
	enum { kMax = 200000 };
	double* lat = (double*)malloc(sizeof(double) * kMax);
	if (!buf || !lat || blocks < 16)
	{
		*err = ENOMEM;
		close(fd);
		free(buf);
		free(lat);
		return;
	}
	size_t n = 0;
	double total = 0;
	const double t0 = now(), limit = secs * g_scale;
	while (now() - t0 < limit && n < kMax)
	{
		const off_t off = (off_t)((rnd() % blocks) * 4096);
		const double a = now();
		const ssize_t got = pread(fd, buf, 4096, off);
		const double us = (now() - a) * 1e6;
		if (got != 4096)
		{
			*err = got < 0 ? errno : EIO;
			break;
		}
		lat[n++] = us;
		total += us;
	}
	const double span = now() - t0;
	if (n > 0)
	{
		*iops = (double)n / span;
		*avg_us = total / (double)n;
		qsort(lat, n, sizeof(double), cmp_double);
		*p99_us = lat[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1];
	}
	close(fd);
	free(buf);
	free(lat);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// what is on the drive already

struct biggest
{
	char path[300];
	uint64_t size;
	int seen;
};

static void find_biggest(const char* dir, int depth, const char* skip, struct biggest* b)
{
	if (depth > 3 || b->seen > 4000)
		return;
	DIR* d = opendir(dir);
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)) != NULL && b->seen <= 4000)
	{
		if (e->d_name[0] == '.' || strcmp(e->d_name, TEST_DIR) == 0 || strcmp(e->d_name, "System Volume Information") == 0 ||
			strncmp(e->d_name, "$RECYCLE", 8) == 0 || strcmp(e->d_name, "lost+found") == 0)
			continue;
		char path[300];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (skip && strcmp(path, skip) == 0)
			continue;
		struct stat st;
		if (stat(path, &st) != 0)
			continue;
		b->seen++;
		if (S_ISDIR(st.st_mode))
			find_biggest(path, depth + 1, skip, b);
		else if (S_ISREG(st.st_mode) && (uint64_t)st.st_size > b->size && (uint64_t)st.st_size >= 256 * MIB)
		{
			b->size = (uint64_t)st.st_size;
			snprintf(b->path, sizeof(b->path), "%s", path);
		}
	}
	closedir(d);
}

static void remove_tree(const char* dir)
{
	DIR* d = opendir(dir);
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)) != NULL)
	{
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		char path[300];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		struct stat st;
		if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode))
			remove_tree(path);
		else
			unlink(path);
	}
	closedir(d);
	rmdir(dir);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// one drive

struct summary
{
	char label[96], fstype[24];
	double write_mbps, write_synced, read_direct, cold_read, iops, small_create, small_delete;
	int have_write, have_read_direct, have_cold, have_iops, have_small;
};

static const char* err_text(int e)
{
	return e ? strerror(e) : "";
}

static void say_run(const char* what, const struct run* r)
{
	if (r->err)
		say("    %-34s failed: %s (after %.0f MB)", what, err_text(r->err), (double)r->bytes / 1e6);
	else
		say("    %-34s %8.1f MB/s   (with the flush %.1f; seconds: slowest %.1f, fastest %.1f)", what, r->mbps, r->mbps_synced, r->min_mbps, r->max_mbps);
}

static void test_target(const struct target* t, struct summary* sm)
{
	memset(sm, 0, sizeof(*sm));
	snprintf(sm->label, sizeof(sm->label), "%s", t->label);
	snprintf(sm->fstype, sizeof(sm->fstype), "%s", t->fstype);
	say("");
	say("=== %s", t->label);
	say("    file system %s, %.1f GB, %.1f GB free", t->fstype, (double)t->total / 1e9, (double)t->avail / 1e9);
	char dir[200], file[260], sdir[260];
	snprintf(dir, sizeof(dir), "%s/%s", t->root, TEST_DIR);
	snprintf(file, sizeof(file), "%s/big.tmp", dir);
	snprintf(sdir, sizeof(sdir), "%s/small", dir);
	remove_tree(dir); // what a run that was cut short left
	const double t_start = now();
	int can_write = t->avail >= 1 * GIB + 256 * MIB;
	if (!can_write)
		say("    not enough free space for the write tests (under 1.3 GB): reading only");
	if (can_write && mkdir(dir, 0777) != 0 && errno != EEXIST)
	{
		say("    can't make %s: %s: reading only", dir, strerror(errno));
		can_write = 0;
	}
	uint64_t cap = 0;
	if (can_write)
	{
		cap = t->avail / 4 < 3 * GIB ? t->avail / 4 : 3 * GIB; // at most 3 GB, and a quarter of what is free
		if (cap > t->avail - 256 * MIB)
			cap = t->avail - 256 * MIB;
	}
	struct run r;
	if (can_write)
	{
		say("  writing (8 MiB at a time, up to 12 s, at most %.1f GB):", (double)cap / 1e9);
		seq_write(file, 8 * MIB, 12, cap, 0, 0, 1, &r);
		say_run("write 8 MiB", &r);
		if (r.err)
			can_write = 0;
		else
		{
			sm->write_mbps = r.mbps;
			sm->write_synced = r.mbps_synced;
			sm->have_write = 1;
			say("    the flush at the end took %.2f s; %.0f MB written in %.1f s", r.sync_secs, (double)r.bytes / 1e6, r.secs);
			if (r.first2 > 20 && r.last3 < r.first2 * 0.6)
				say("    NOTE: the speed fell from %.0f to %.0f MB/s during the write (the drive's cache fills, or something throttles writes)", r.first2, r.last3);
		}
	}
	if (can_write)
	{
		say("  block sizes (writing for 4 s each, flushed at the end):");
		static const size_t sizes[] = {64 * 1024, 256 * 1024, 1 * MIB, 32 * MIB};
		static const char* const names[] = {"write 64 KiB", "write 256 KiB", "write 1 MiB", "write 32 MiB"};
		char other[260];
		snprintf(other, sizeof(other), "%s/blocks.tmp", dir);
		for (int i = 0; i < 4; i++)
		{
			seq_write(other, sizes[i], 4, cap / 2, 0, 0, 0, &r);
			// the flushed speed is what a program that closes its file gets
			if (r.err)
				say("    %-34s failed: %s", names[i], err_text(r.err));
			else
				say("    %-34s %8.1f MB/s   (flushed: %.1f)", names[i], r.mbps, r.mbps_synced);
		}
		seq_write(other, 8 * MIB, 4, cap / 2, 0, 1, 0, &r);
		if (r.err)
			say("    %-34s failed: %s", "8 MiB, flushed every buffer", err_text(r.err));
		else
			say("    %-34s %8.1f MB/s", "8 MiB, flushed every buffer", r.mbps_synced);
		seq_write(other, 8 * MIB, 4, cap / 2, O_DIRECT, 0, 0, &r);
		if (r.err)
			say("    %-34s not possible here (%s)", "8 MiB, O_DIRECT (no file cache)", err_text(r.err));
		else
			say("    %-34s %8.1f MB/s", "8 MiB, O_DIRECT (no file cache)", r.mbps_synced);
		unlink(other);
	}
	if (can_write)
	{
		say("  reading back the file just written (12 s at most):");
		seq_read(file, 8 * MIB, 0, 12, cap, 0, 0, &r);
		say_run("read, buffered (may come from RAM)", &r);
		seq_read(file, 8 * MIB, 0, 6, cap, O_DIRECT, 0, &r);
		if (r.err)
			say("    %-34s not possible here (%s)", "read, O_DIRECT (no file cache)", err_text(r.err));
		else
		{
			say_run("read, O_DIRECT (the drive's own)", &r);
			sm->read_direct = r.mbps;
			sm->have_read_direct = 1;
		}
	}
	// what the drive already holds: the largest file, read from its middle
	struct biggest big;
	memset(&big, 0, sizeof(big));
	char skip[300];
	snprintf(skip, sizeof(skip), "%s", file);
	find_biggest(t->root, 0, skip, &big);
	if (big.size > 0)
	{
		say("  an existing file, read from its middle (%s, %.1f GB):", big.path, (double)big.size / 1e9);
		uint64_t start = big.size > 1 * GIB ? (big.size / 2) / (8 * MIB) * (8 * MIB) : 0;
		seq_read(big.path, 8 * MIB, start, 12, 2 * GIB, 0, 1, &r);
		say_run("cold read 8 MiB", &r);
		if (!r.err)
		{
			sm->cold_read = r.mbps;
			sm->have_cold = 1;
		}
		double iops, avg, p99;
		int err;
		rand_read(big.path, big.size, 5, &iops, &avg, &p99, &err);
		if (err)
			say("    %-34s failed: %s", "random 4 KiB reads", err_text(err));
		else
		{
			say("    %-34s %8.0f reads/s   (average %.0f us, slowest 1%% over %.0f us)", "random 4 KiB reads, one at a time", iops, avg, p99);
			sm->iops = iops;
			sm->have_iops = 1;
		}
	}
	else
		say("  (no file of 256 MB or more on this drive to read: the cold-read and random tests are skipped)");
	if (can_write)
	{
		// the small files
		mkdir(sdir, 0777);
		say("  1,000 small files (16 KiB each):");
		uint8_t* buf = alloc_buffer(16 * 1024);
		int made = 0;
		const double c0 = now();
		const double limit = 10 * g_scale;
		for (int i = 0; i < 1000 && buf && now() - c0 < limit; i++)
		{
			char path[300];
			snprintf(path, sizeof(path), "%s/f%04d.bin", sdir, i);
			const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
			if (fd < 0)
				break;
			const ssize_t w = write(fd, buf, 16 * 1024);
			close(fd);
			if (w != 16 * 1024)
				break;
			made++;
		}
		const double c1 = now();
		for (int i = 0; i < made; i++)
		{
			char path[300];
			snprintf(path, sizeof(path), "%s/f%04d.bin", sdir, i);
			unlink(path);
		}
		const double c2 = now();
		free(buf);
		if (made > 0 && c1 > c0 && c2 > c1)
		{
			sm->small_create = (double)made / (c1 - c0);
			sm->small_delete = (double)made / (c2 - c1);
			sm->have_small = 1;
			say("    %-34s %8.0f files/s   (%d files in %.2f s)", "created", sm->small_create, made, c1 - c0);
			say("    %-34s %8.0f files/s", "deleted", sm->small_delete);
		}
		else
			say("    the small files could not be made (%s)", strerror(errno));
	}
	remove_tree(dir);
	struct stat st;
	say("  cleaned up: %s %s; this drive took %.0f s", dir, stat(dir, &st) == 0 ? "IS STILL THERE" : "removed", now() - t_start);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------

static void headline(const struct summary* s, char* out, size_t room)
{
	char a[160] = "", b[80] = "", c[80] = "", d[80] = "";
	if (s->have_write)
		snprintf(a, sizeof(a), "write %.0f MB/s", s->write_mbps);
	if (s->have_read_direct)
		snprintf(b, sizeof(b), ", read %.0f", s->read_direct);
	if (s->have_cold)
		snprintf(c, sizeof(c), ", existing file %.0f", s->cold_read);
	if (s->have_iops)
		snprintf(d, sizeof(d), ", %.0f reads/s", s->iops);
	snprintf(out, room, "%s%s%s%s", a[0] ? a : "no write test", b, c, d);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
#ifdef HOST_TEST
	if (getenv("DRIVETEST_LOG"))
		snprintf(g_log_path, sizeof(g_log_path), "%s", getenv("DRIVETEST_LOG"));
#endif
	if (getenv("DRIVETEST_SCALE"))
		g_scale = atof(getenv("DRIVETEST_SCALE"));
	if (g_scale <= 0 || g_scale > 10)
		g_scale = 1.0;
	{
		char dir[260];
		snprintf(dir, sizeof(dir), "%s", g_log_path);
		char* slash = strrchr(dir, '/');
		if (slash)
		{
			*slash = 0;
			mkdirs(dir);
		}
	}
	// One run at a time: a second ELF loaded while the first is working would measure the first. The lock is a file with the time in it; one older
	// than 20 minutes (a run that was killed) is taken over.
	{
		char lock[300];
		snprintf(lock, sizeof(lock), "%s.lock", g_log_path);
		FILE* lf = fopen(lock, "r");
		long long at = 0;
		if (lf)
		{
			if (fscanf(lf, "%lld", &at) != 1)
				at = 0;
			fclose(lf);
		}
		const long long t_now = (long long)time(NULL);
		if (at > 0 && t_now >= at && t_now - at < 20 * 60)
		{
			notify("PS5SX2 drive test is already running: wait for its last popup, then load it again");
			printf("another drive test is running (lock %s)\n", lock);
			return 1;
		}
		lf = fopen(lock, "w");
		if (lf)
		{
			fprintf(lf, "%lld\n", t_now);
			fclose(lf);
		}
	}
	g_log = fopen(g_log_path, "a");
	const time_t when = time(NULL);
	say("");
	say("############################################################################");
	say("PS5SX2 %s (%s) %s", BUILD_NAME, SCOPE == 1 ? "internal storage" : SCOPE == 2 ? "external storage" : "internal and external storage", ctime(&when));
#ifndef HOST_TEST
	{
		unsigned int sdk = 0;
		size_t len = sizeof(sdk);
		unsigned long long mem = 0;
		size_t mlen = sizeof(mem);
		if (sysctlbyname("kern.sdk_version", &sdk, &len, NULL, 0) == 0)
			say("console SDK version %#x", sdk);
		if (sysctlbyname("hw.physmem", &mem, &mlen, NULL, 0) == 0)
			say("memory %.1f GB", (double)mem / 1e9);
	}
#endif
	discover();
	if (g_n == 0)
	{
		say("no drive found (%s)", SCOPE == 2 ? "no USB drive or extended storage is plugged in and mounted" : "nothing to test");
		notify("PS5SX2 drive test: no %s found", SCOPE == 2 ? "USB drive or extended storage" : "drive");
		if (g_log)
			fclose(g_log);
		{
			char lock[300];
			snprintf(lock, sizeof(lock), "%s.lock", g_log_path);
			unlink(lock);
		}
		return 1;
	}
	say("%d drive%s to test:", g_n, g_n == 1 ? "" : "s");
	for (int i = 0; i < g_n; i++)
		say("  %s: %s, %.1f GB (%.1f GB free)", g_t[i].label, g_t[i].fstype, (double)g_t[i].total / 1e9, (double)g_t[i].avail / 1e9);
	notify("PS5SX2 drive test: %d drive%s, about %d min. Keep the console on; nothing is changed on the drives.", g_n, g_n == 1 ? "" : "s",
		(int)(g_n * 90 * g_scale / 60 + 1));
	struct summary sums[MAX_TARGETS];
	for (int i = 0; i < g_n; i++)
	{
		test_target(&g_t[i], &sums[i]);
		char h[260];
		headline(&sums[i], h, sizeof(h));
		notify("PS5SX2 %s (%s): %s", g_t[i].label, g_t[i].fstype, h);
	}
	say("");
	say("=== summary");
	for (int i = 0; i < g_n; i++)
	{
		char h[260];
		headline(&sums[i], h, sizeof(h));
		say("RESULT | %s | %s | %s", sums[i].label, sums[i].fstype, h);
		if (sums[i].have_small)
			say("RESULT |   small files: %.0f created/s, %.0f deleted/s", sums[i].small_create, sums[i].small_delete);
	}
	say("log: %s", g_log_path);
	{
		char lock[300];
		snprintf(lock, sizeof(lock), "%s.lock", g_log_path);
		unlink(lock);
	}
	notify("PS5SX2 drive test done. Send %s", g_log_path);
	if (g_log)
		fclose(g_log);
	sleep(1);
	return 0;
}
