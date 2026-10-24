// PS5SX2 (2.11, AI-assisted): the disc drive speed test payload. swordpdf: "we need to make elfs that test their drives max speeds (both internal and
// external)" -- the DVD / CD drives, not the storage.
//
// An ELF for the console's ELF loader (port 9021, like the disc helper). It finds every optical drive the console has, reads the disc in each the way
// PS5SX2 does (/dev/cdN), and asks each drive for more speed in every way it knows. What it found goes to /data/PCSX2/logs/disc-drive-speed.txt
// (appended: each run is added to the end) and the headline numbers come as notifications. Three builds of this one file:
//   SCOPE=1  internal: the PS5's own drive (SONY PS-SYSTEM: the built-in one, or the detachable one of a Digital Edition)
//   SCOPE=2  external: any other optical drive the console sees (a USB DVD / BD drive)
//   SCOPE=3  all of them
//
// What a drive is asked (about 3 minutes each; every phase is time-boxed; the disc is only READ, nothing is written to it or to the console):
//   identity   INQUIRY (make, model, firmware), the disc's profile and size, the speeds the drive says it has (MODE SENSE page 2A, GET PERFORMANCE)
//   speed map  the disc read 128 KiB at a time at 5 places from the inner edge to the outer edge, as the drive is (a CAV drive gets faster outwards)
//   read size  32 KiB, 128 KiB, 1 MiB, 4 MiB reads: does the size of a read matter (a USB drive can be slow with small ones)
//   sustained  20 s in one go, second by second: a drive that starts fast and slows down, or stalls, shows
//   seeks      random 32 KiB reads, and two places of the disc read in turn (32 KiB, 128 KiB, 512 KiB at a time): what a game gets when it loads many
//              small pieces or plays two things at once. A seek costs the drive 100-300 ms, so how much is read for each one decides the speed
//   stutter    how PS5SX2 really reads (a block of 32 KiB, 16 more read ahead, a pause, a drive check every 0.5 s) against the same bytes in
//              128 KiB reads (the 5S2CD fork's read-ahead, which people say stutters less): MB/s while reading, the SLOWEST single read, how long
//              the first read after a pause takes, and how many reads took over 100 ms (a read that long is a stall the game feels)
//   settings   Sony's own speed command (DB rr ss ss: every rotation 0..3 with 11 speeds; the PS5 drive takes 2.0x and 3.2x on every console tested
//              so far, the rest is refused), the standard SET CD SPEED (max) and SET STREAMING (max): each one the drive takes is timed at the inner
//              and outer part of the disc, so "max speed" is what the drive does when asked for everything
//   at the end the drive is put back to its default (Sony: DB 00 00 20)
//
// 1x = 150 KiB/s for a CD (0.1536 MB/s), 1.385 MB/s for a DVD, 4.5 MB/s for a Blu-ray.
//
// Built with the PS5 payload SDK (build-discspeed.sh); with -DHOST_TEST it runs on a PC against image files and a fake SCSI layer (test-discspeed.sh).
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HOST_TEST
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef HOST_TEST
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <ps5/kernel.h>

_Static_assert(sizeof(union ccb) == 0x4E0, "union ccb: SceShellCore's CAMIOCOMMAND (0xC4E01902) carries 1248 bytes");
#define D_NONE CAM_DIR_NONE
#define D_IN CAM_DIR_IN
#define D_OUT CAM_DIR_OUT
#else
#define D_NONE 0
#define D_IN 1
#define D_OUT 2
#endif

#ifndef SCOPE
#define SCOPE 3
#endif
#define BUILD_NAME "disc drive test 2"

#define MIB (1024ull * 1024ull)
#define MAX_DRIVES 8
#define MIN_DISC (32ull * MIB)

// ---------------------------------------------------------------------------------------------------------------------------------------------
// notifications and the log

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
	fflush(stdout);
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

static FILE* g_log;
static char g_log_path[256] = "/data/PCSX2/logs/disc-drive-speed.txt";
static double g_scale = 1.0; // DISCTEST_SCALE: the phases' lengths times this (the PC test shortens them)

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...)
{
	char buf[720];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
	va_end(ap);
	size_t n = strlen(buf);
	buf[n++] = '\n';
	buf[n] = 0;
	fwrite(buf, 1, n, stdout);
	fflush(stdout);
	if (g_log)
	{
		fwrite(buf, 1, n, g_log);
		fflush(g_log);
	}
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// For the watchdog thread: what is going on, and a counter that moves every time a read or a drive command comes back.
static volatile uint64_t g_progress;
static volatile uint64_t g_where; // offset of the read in flight
static volatile unsigned g_cmd;   // opcode of the last drive command
static const char* volatile g_phase = "starting";
static volatile int g_done;
static double g_wd_warn = 15, g_wd_giveup = 240; // seconds without progress: a line in the log; the run is given up

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

static uint32_t be32(const uint8_t* p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const uint8_t* p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// the platform: the console's CAM layer, or on a PC image files and a fake SCSI drive

#ifndef HOST_TEST

// Opens a device with the shell's credentials when this process may not (as PS5SX2 does for /dev/cd0).
static int dev_open(const char* path, int flags)
{
	int fd = open(path, flags);
	if (fd < 0 && (errno == EPERM || errno == EACCES))
	{
		const pid_t pid = getpid();
		const uint64_t authid = kernel_get_ucred_authid(pid);
		uint8_t caps[16], all[16];
		memset(all, 0xff, sizeof(all));
		kernel_get_ucred_caps(pid, caps);
		kernel_set_ucred_authid(pid, 0x4800000000010003ull);
		kernel_set_ucred_caps(pid, all);
		fd = open(path, flags);
		const int e = errno;
		kernel_set_ucred_authid(pid, authid);
		kernel_set_ucred_caps(pid, caps);
		errno = e;
	}
	return fd;
}

static ssize_t dev_pread_raw(int fd, void* buf, size_t n, off_t off)
{
	return pread(fd, buf, n, off);
}

// The disc's size and sector size; -1 when the drive has no disc (ready) yet.
static int dev_size(int fd, uint64_t* size, unsigned* sector)
{
	off_t media = 0;
	unsigned sec = 0;
	if (ioctl(fd, DIOCGMEDIASIZE, &media) != 0 || media <= 0)
		return -1;
	if (ioctl(fd, DIOCGSECTORSIZE, &sec) != 0 || sec == 0)
		sec = 2048;
	*size = (uint64_t)media;
	*sector = sec;
	return 0;
}

// "/dev/passN" of a cd device.
static int dev_passthru(int fd, char* out, size_t n)
{
	union ccb ccb;
	memset(&ccb, 0, sizeof(ccb));
	ccb.ccb_h.func_code = XPT_GDEVLIST;
	if (ioctl(fd, CAMGETPASSTHRU, &ccb) != 0)
		return -1;
	snprintf(out, n, "/dev/%.*s%u", (int)sizeof(ccb.cgdl.periph_name), ccb.cgdl.periph_name, ccb.cgdl.unit_number);
	return 0;
}

// One command: 0 when done, else why = "errno N" or "sense K/ASC/ASCQ (cam S)".
static int scsi_raw(int pass, const uint8_t* cdb, int cdb_len, uint32_t dir, uint8_t* data, uint32_t len, char* why, size_t wmax)
{
	union ccb ccb;
	memset(&ccb, 0, sizeof(ccb));
	cam_fill_csio(&ccb.csio, 1, NULL, dir | CAM_DEV_QFRZDIS | (dir == CAM_DIR_NONE ? CAM_PASS_ERR_RECOVER : 0), MSG_SIMPLE_Q_TAG, data, len,
		SSD_FULL_SIZE, (uint8_t)cdb_len, 15000);
	memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
	if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0)
	{
		snprintf(why, wmax, "errno %d", errno);
		return -1;
	}
	if ((ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
		return 0;
	const uint8_t* s = (const uint8_t*)&ccb.csio.sense_data;
	snprintf(why, wmax, "sense %X/%02X/%02X (cam %#x)", s[2] & 15, s[12], s[13], ccb.ccb_h.status & CAM_STATUS_MASK);
	return -1;
}

static int list_cd_devices(char names[][16], int max)
{
	int n = 0;
	DIR* d = opendir("/dev");
	if (!d)
		return 0;
	struct dirent* e;
	while ((e = readdir(d)) != NULL && n < max)
		if (!strncmp(e->d_name, "cd", 2) && e->d_name[2] >= '0' && e->d_name[2] <= '9')
			snprintf(names[n++], 16, "%s", e->d_name);
	closedir(d);
	// cd0, cd1, ... in order
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++)
			if (strcmp(names[j], names[i]) < 0)
			{
				char t[16];
				memcpy(t, names[i], 16);
				memcpy(names[i], names[j], 16);
				memcpy(names[j], t, 16);
			}
	return n;
}

__attribute__((unused)) static void inquiry_text(int pass, char* out, size_t n, int* type);

// What the console has that could be a drive or a USB device, and every pass device with what it says it is: if an external drive is not used, this
// says whether the console saw it at all.
static void survey_devices(void)
{
	char list[1500] = "";
	int at = 0;
	DIR* d = opendir("/dev");
	if (d)
	{
		struct dirent* e;
		while ((e = readdir(d)) != NULL)
		{
			const char* nm = e->d_name;
			const int interesting = !strncmp(nm, "cd", 2) || !strncmp(nm, "pass", 4) || !strncmp(nm, "da", 2) || !strncmp(nm, "ugen", 4) ||
									!strncmp(nm, "usb", 3) || !strncmp(nm, "umass", 5) || !strncmp(nm, "acd", 3) || !strncmp(nm, "xpt", 3) ||
									!strncmp(nm, "bd", 2);
			if (interesting && at < (int)sizeof(list) - 20)
				at += snprintf(list + at, sizeof(list) - (size_t)at, " %s", nm);
		}
		closedir(d);
	}
	say("/dev:%s", list);
	d = opendir("/dev/usb");
	if (d)
	{
		char ulist[1500] = "";
		int ua = 0;
		struct dirent* e;
		while ((e = readdir(d)) != NULL)
			if (e->d_name[0] != '.' && ua < (int)sizeof(ulist) - 20)
				ua += snprintf(ulist + ua, sizeof(ulist) - (size_t)ua, " %s", e->d_name);
		closedir(d);
		say("/dev/usb:%s", ulist);
	}
	for (int i = 0; i < 16; i++)
	{
		char p[32];
		snprintf(p, sizeof(p), "/dev/pass%d", i);
		if (access(p, F_OK) != 0)
			continue;
		const int pass = dev_open(p, O_RDWR);
		if (pass < 0)
		{
			say("%s: open errno %d", p, errno);
			continue;
		}
		char what[96];
		int type = -1;
		inquiry_text(pass, what, sizeof(what), &type);
		say("%s: type %d%s: %s", p, type, type == 5 ? " (CD/DVD/BD)" : type == 0 ? " (disk)" : "", what);
		close(pass);
	}
}

static void console_info(void)
{
	const uint32_t v = kernel_get_fw_version();
	unsigned int sdk = 0;
	size_t len = sizeof(sdk);
	say("console firmware %x.%02x (kernel %#x)", v >> 24, (v >> 16) & 0xff, v);
	if (sysctlbyname("kern.sdk_version", &sdk, &len, NULL, 0) == 0)
		say("console SDK version %#x", sdk);
}

#else // HOST_TEST: image files and a fake drive

struct fake_drive
{
	char name[16];
	char path[200]; // the image file ("-" = no disc)
	int sony;
	unsigned profile;
	double x;       // the speed setting now, in x
	int bb_taken;   // SET CD SPEED taken
	int has_disc;
};
static struct fake_drive g_fake[MAX_DRIVES];
static int g_nfake;
static int g_cd_idx[256], g_pass_idx[256];
static double g_fake_k = 40; // DISCTEST_FAKE_K: how many times faster the fake drives are than real ones (the PC test would take hours otherwise)
static double g_fake_seek_ms = 4;                 // DISCTEST_FAKE_SEEK_MS: a read that does not carry on from the last one waits this long
static long long g_hang_from = -1;                // DISCTEST_FAKE_HANG=<MiB>: a read from there on never comes back (30 s)
static uint64_t g_fake_last_end[MAX_DRIVES];
static long long g_bad_from = -1, g_bad_to = -1; // DISCTEST_FAKE_BAD="from-to" (MiB): reads that touch it fail

static void fake_init(void)
{
	for (int i = 0; i < 256; i++)
		g_cd_idx[i] = g_pass_idx[i] = -1;
	if (getenv("DISCTEST_FAKE_K"))
		g_fake_k = atof(getenv("DISCTEST_FAKE_K"));
	if (getenv("DISCTEST_FAKE_SEEK_MS"))
		g_fake_seek_ms = atof(getenv("DISCTEST_FAKE_SEEK_MS"));
	if (getenv("DISCTEST_FAKE_HANG"))
		g_hang_from = atoll(getenv("DISCTEST_FAKE_HANG"));
	if (getenv("DISCTEST_WD_WARN"))
		g_wd_warn = atof(getenv("DISCTEST_WD_WARN"));
	if (getenv("DISCTEST_WD_GIVEUP"))
		g_wd_giveup = atof(getenv("DISCTEST_WD_GIVEUP"));
	if (getenv("DISCTEST_FAKE_BAD"))
		sscanf(getenv("DISCTEST_FAKE_BAD"), "%lld-%lld", &g_bad_from, &g_bad_to);
	const char* env = getenv("DISCTEST_FAKE"); // "cd0=/path/img:sony,cd1=/path/img2:pc:08"
	if (!env)
		return;
	char tmp[1000];
	snprintf(tmp, sizeof(tmp), "%s", env);
	for (char* tok = strtok(tmp, ","); tok && g_nfake < MAX_DRIVES; tok = strtok(NULL, ","))
	{
		struct fake_drive* f = &g_fake[g_nfake];
		char* eq = strchr(tok, '=');
		if (!eq)
			continue;
		*eq = 0;
		snprintf(f->name, sizeof(f->name), "%s", tok);
		char* path = eq + 1;
		char* c1 = strchr(path, ':');
		char kind[16] = "pc";
		f->profile = 0x10;
		if (c1)
		{
			*c1 = 0;
			char* c2 = strchr(c1 + 1, ':');
			if (c2)
			{
				*c2 = 0;
				f->profile = (unsigned)strtoul(c2 + 1, NULL, 16);
			}
			snprintf(kind, sizeof(kind), "%s", c1 + 1);
		}
		snprintf(f->path, sizeof(f->path), "%s", path);
		f->sony = !strcmp(kind, "sony");
		f->has_disc = strcmp(path, "-") != 0;
		f->x = f->sony ? 2.0 : 4.0;
		g_nfake++;
	}
}

static int dev_open(const char* path, int flags)
{
	if (!strncmp(path, "/dev/pass", 9))
	{
		const int i = atoi(path + 9);
		if (i < 0 || i >= g_nfake)
		{
			errno = ENOENT;
			return -1;
		}
		const int fd = open("/dev/null", O_RDWR);
		if (fd >= 0 && fd < 256)
			g_pass_idx[fd] = i;
		return fd;
	}
	if (!strncmp(path, "/dev/cd", 7))
	{
		const int i = atoi(path + 7);
		if (i < 0 || i >= g_nfake)
		{
			errno = ENOENT;
			return -1;
		}
		const int fd = open(g_fake[i].has_disc ? g_fake[i].path : "/dev/null", flags & O_ACCMODE);
		if (fd >= 0 && fd < 256)
			g_cd_idx[fd] = i;
		return fd;
	}
	return open(path, flags);
}

static ssize_t dev_pread_raw(int fd, void* buf, size_t n, off_t off)
{
	const int i = (fd >= 0 && fd < 256) ? g_cd_idx[fd] : -1;
	if (g_bad_from >= 0 && (long long)off < g_bad_to * (long long)MIB && (long long)(off + (off_t)n) > g_bad_from * (long long)MIB)
	{
		errno = EIO;
		return -1;
	}
	if (g_hang_from >= 0 && (long long)off >= g_hang_from * (long long)MIB)
		sleep(30);
	const ssize_t r = pread(fd, buf, n, off);
	if (r > 0 && i >= 0 && g_fake_seek_ms > 0 && ((uint64_t)off + MIB < g_fake_last_end[i] || (uint64_t)off > g_fake_last_end[i] + MIB))
	{
		struct timespec sk;
		sk.tv_sec = (time_t)(g_fake_seek_ms / 1000);
		sk.tv_nsec = (long)((g_fake_seek_ms - (double)sk.tv_sec * 1000) * 1e6);
		nanosleep(&sk, NULL);
	}
	if (r > 0 && i >= 0)
		g_fake_last_end[i] = (uint64_t)off + (uint64_t)r;
	if (r > 0 && i >= 0)
	{
		struct stat st;
		fstat(fd, &st);
		const double pos = st.st_size > 0 ? (double)off / (double)st.st_size : 0;
		const double rate = g_fake[i].x * 1.385 * g_fake_k * (g_fake[i].sony ? 1.0 : 0.5 + 0.5 * pos); // MB/s
		const double secs = (double)r / (rate * 1e6);
		struct timespec ts;
		ts.tv_sec = (time_t)secs;
		ts.tv_nsec = (long)((secs - (double)ts.tv_sec) * 1e9);
		nanosleep(&ts, NULL);
	}
	return r;
}

static int dev_size(int fd, uint64_t* size, unsigned* sector)
{
	const int i = (fd >= 0 && fd < 256) ? g_cd_idx[fd] : -1;
	struct stat st;
	if (i < 0 || !g_fake[i].has_disc || fstat(fd, &st) != 0 || st.st_size <= 0)
		return -1;
	*size = (uint64_t)st.st_size;
	*sector = 2048;
	return 0;
}

static int dev_passthru(int fd, char* out, size_t n)
{
	const int i = (fd >= 0 && fd < 256) ? g_cd_idx[fd] : -1;
	if (i < 0)
		return -1;
	snprintf(out, n, "/dev/pass%d", i);
	return 0;
}

static int refuse(char* why, size_t wmax, int key, int asc)
{
	snprintf(why, wmax, "sense %X/%02X/00 (cam 0x4)", key, asc);
	return -1;
}

static int scsi_raw(int pass, const uint8_t* cdb, int cdb_len, uint32_t dir, uint8_t* data, uint32_t len, char* why, size_t wmax)
{
	(void)cdb_len;
	(void)dir;
	const int i = (pass >= 0 && pass < 256) ? g_pass_idx[pass] : -1;
	if (i < 0)
	{
		snprintf(why, wmax, "errno 9");
		return -1;
	}
	struct fake_drive* f = &g_fake[i];
	switch (cdb[0])
	{
		case 0x00: // TEST UNIT READY
			return f->has_disc ? 0 : refuse(why, wmax, 2, 0x3A);
		case 0x12: // INQUIRY
			memset(data, 0, len);
			data[0] = 5;
			memcpy(data + 8, f->sony ? "SONY    " : "HL-DT-ST", 8);
			memcpy(data + 16, f->sony ? "PS-SYSTEM 503R  " : "DVDRAM GP65NB60 ", 16);
			memcpy(data + 32, f->sony ? "2305" : "1.00", 4);
			return 0;
		case 0x46: // GET CONFIGURATION
			memset(data, 0, len);
			data[6] = (uint8_t)(f->profile >> 8);
			data[7] = (uint8_t)f->profile;
			return 0;
		case 0x5A: // MODE SENSE(10)
		{
			memset(data, 0, len);
			uint8_t* p = data + 8;
			p[0] = 0x2A;
			p[1] = 0x16;
			const unsigned maxkb = f->sony ? 4432 : 22160;
			p[8] = (uint8_t)(maxkb >> 8);
			p[9] = (uint8_t)maxkb;
			p[14] = (uint8_t)(maxkb >> 8);
			p[15] = (uint8_t)maxkb;
			data[1] = 0x1E;
			return 0;
		}
		case 0xAC: // GET PERFORMANCE
		{
			memset(data, 0, len);
			if (cdb[10] == 0x03)
				return refuse(why, wmax, 5, 0x24);
			data[3] = 20 - 4 + 4;
			const unsigned s = f->sony ? 2770 : 5540, e = f->sony ? 4432 : 22160;
			data[12] = (uint8_t)(s >> 24), data[13] = (uint8_t)(s >> 16), data[14] = (uint8_t)(s >> 8), data[15] = (uint8_t)s;
			data[20] = (uint8_t)(e >> 24), data[21] = (uint8_t)(e >> 16), data[22] = (uint8_t)(e >> 8), data[23] = (uint8_t)e;
			return 0;
		}
		case 0xDB: // Sony speed
		{
			const unsigned rot = cdb[1], sp = (unsigned)(cdb[2] << 8 | cdb[3]);
			if (!f->sony)
				return refuse(why, wmax, 5, 0x20);
			if (rot == 0 && sp == 0x20)
				f->x = 2.0;
			else if (rot == 0 && sp == 0x32)
				f->x = 3.2;
			else
				return refuse(why, wmax, 5, 0x24);
			return 0;
		}
		case 0xBB: // SET CD SPEED
			if (f->sony)
				return refuse(why, wmax, 5, 0x20);
			f->x = 8.0;
			f->bb_taken = 1;
			return 0;
		case 0xB6: // SET STREAMING
			if (data && len > 0 && (data[0] & 0x04))
				return 0; // RDD
			return refuse(why, wmax, 5, 0x20);
		default:
			return refuse(why, wmax, 5, 0x20);
	}
}

static int list_cd_devices(char names[][16], int max)
{
	int n = 0;
	for (; n < g_nfake && n < max; n++)
		snprintf(names[n], 16, "%s", g_fake[n].name);
	return n;
}

static void survey_devices(void)
{
	say("/dev: (PC test: %d fake drive%s)", g_nfake, g_nfake == 1 ? "" : "s");
}

static void console_info(void)
{
	say("console firmware (PC test)");
}

#endif

// ---------------------------------------------------------------------------------------------------------------------------------------------
// the watched calls

static ssize_t dev_pread(int fd, void* buf, size_t n, off_t off)
{
	g_where = (uint64_t)off;
	const ssize_t r = dev_pread_raw(fd, buf, n, off);
	g_progress++;
	return r;
}

static int scsi(int pass, const uint8_t* cdb, int cdb_len, uint32_t dir, uint8_t* data, uint32_t len, char* why, size_t wmax)
{
	g_cmd = cdb[0];
	const int r = scsi_raw(pass, cdb, cdb_len, dir, data, len, why, wmax);
	g_progress++;
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// the drives

typedef struct
{
	char name[16];      // "cd0"
	char path[32];      // "/dev/cd0"
	char pass_path[32]; // "/dev/pass0"
	int fd, pass;
	char vendor[9], model[17], rev[5];
	int sony;           // the PS5's own drive
	int disc;           // a disc is in and ready
	int tested;
	uint64_t size;
	unsigned sector;
	unsigned profile;
	unsigned claim_kbps; // the fastest speed the drive says it has
	// results
	double curve_min, curve_max;
	double best_mbps;
	char best_what[96];
	int errs;
	int have_seek;
	double rand_avg_ms, rand_max_ms, alt32, alt128, alt512;
	int have_app;
	double app_mbps, app_max_ms, nocheck_mbps, nocheck_max_ms, fork_mbps, fork_max_ms;
} drive_t;

static drive_t g_d[MAX_DRIVES];
static int g_nd;
static uint8_t* g_buf;

static const char* profile_name(unsigned p)
{
	switch (p)
	{
		case 0x08: return "CD-ROM";
		case 0x09: return "CD-R";
		case 0x0A: return "CD-RW";
		case 0x10: return "DVD-ROM";
		case 0x11: return "DVD-R";
		case 0x12: return "DVD-RAM";
		case 0x13: case 0x14: return "DVD-RW";
		case 0x15: case 0x16: return "DVD-R DL";
		case 0x1A: return "DVD+RW";
		case 0x1B: return "DVD+R";
		case 0x2B: return "DVD+R DL";
		case 0x40: return "BD-ROM";
		case 0x41: case 0x42: return "BD-R";
		case 0x43: return "BD-RE";
		default: return "unknown disc type";
	}
}

// 0 CD, 1 DVD, 2 BD: from the profile, else from the size.
static int disc_kind(const drive_t* d)
{
	if (d->profile >= 0x08 && d->profile <= 0x0A)
		return 0;
	if (d->profile >= 0x10 && d->profile <= 0x2F)
		return 1;
	if (d->profile >= 0x40 && d->profile <= 0x43)
		return 2;
	return d->size < 900ull * 1000 * 1000 ? 0 : d->size < 9ull * 1000 * 1000 * 1000 ? 1 : 2;
}
static const char* kind_name(const drive_t* d)
{
	static const char* const k[] = {"CD", "DVD", "BD"};
	return k[disc_kind(d)];
}
// MB/s of 1x
static double unit_mbps(const drive_t* d)
{
	static const double u[] = {0.1536, 1.385, 4.5};
	return u[disc_kind(d)];
}

__attribute__((unused)) static void inquiry_text(int pass, char* out, size_t n, int* type)
{
	uint8_t inq[36] = {0};
	char why[96];
	const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
	if (scsi(pass, c, 6, D_IN, inq, sizeof(inq), why, sizeof(why)) != 0)
	{
		snprintf(out, n, "INQUIRY %s", why);
		*type = -1;
		return;
	}
	*type = inq[0] & 0x1f;
	snprintf(out, n, "%.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
}

static void trim_copy(char* dst, size_t dn, const uint8_t* src, size_t sn)
{
	size_t n = sn < dn - 1 ? sn : dn - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
	while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0))
		dst[--n] = 0;
}

static void check_disc(drive_t* d)
{
	uint64_t size = 0;
	unsigned sector = 0;
	d->disc = dev_size(d->fd, &size, &sector) == 0 && size >= MIN_DISC;
	if (d->disc)
	{
		d->size = size;
		d->sector = sector;
	}
}

static int add_drive(const char* name)
{
	if (g_nd >= MAX_DRIVES)
		return 0;
	drive_t* d = &g_d[g_nd];
	memset(d, 0, sizeof(*d));
	d->fd = d->pass = -1;
	snprintf(d->name, sizeof(d->name), "%s", name);
	snprintf(d->path, sizeof(d->path), "/dev/%s", name);
	d->fd = dev_open(d->path, O_RDONLY);
	if (d->fd < 0)
	{
		say("%s: can't open (errno %d)", d->path, errno);
		return 0;
	}
	if (dev_passthru(d->fd, d->pass_path, sizeof(d->pass_path)) == 0)
		d->pass = dev_open(d->pass_path, O_RDWR);
	if (d->pass < 0)
	{
		say("%s: no pass device (%s, errno %d): its commands can't be sent, skipped", d->path, d->pass_path[0] ? d->pass_path : "none", errno);
		close(d->fd);
		return 0;
	}
	uint8_t inq[36] = {0};
	char why[96];
	const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
	if (scsi(d->pass, c, 6, D_IN, inq, sizeof(inq), why, sizeof(why)) != 0)
	{
		say("%s: INQUIRY failed (%s), skipped", d->path, why);
		close(d->pass);
		close(d->fd);
		return 0;
	}
	trim_copy(d->vendor, sizeof(d->vendor), inq + 8, 8);
	trim_copy(d->model, sizeof(d->model), inq + 16, 16);
	trim_copy(d->rev, sizeof(d->rev), inq + 32, 4);
	d->sony = strstr(d->model, "PS-SYSTEM") != NULL;
	if (((SCOPE == 1) && !d->sony) || ((SCOPE == 2) && d->sony))
	{
		say("%s (%s %s %s): %s, not tested by this ELF", d->path, d->vendor, d->model, d->rev,
			d->sony ? "the PS5's own drive" : "an external drive");
		close(d->pass);
		close(d->fd);
		return 0;
	}
	check_disc(d);
	say("%s (%s %s %s, pass %s): %s, %s", d->path, d->vendor, d->model, d->rev, d->pass_path,
		d->sony ? "the PS5's own drive (built in, or the detachable drive of a Digital Edition)" : "an EXTERNAL drive (not the PS5's own)",
		d->disc ? "disc in" : "no disc");
	g_nd++;
	return 1;
}

static const char* label_of(const drive_t* d, char* out, size_t n)
{
	snprintf(out, n, "%s %s %s %s (%s)", d->sony ? "PS5 drive" : "external", d->vendor, d->model, d->rev, d->name);
	return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// timed reads

typedef struct
{
	uint64_t bytes;
	double secs, mbps, first_ms, min_s, max_s;
	int errs, err_no;
} run_t;

// Reads `chunk` bytes at a time from `off` for `limit` seconds (times the scale) or `cap` bytes. The first read is not counted (the drive spins up and
// seeks): its time is first_ms. ticks: a line for every second.
static void read_run(drive_t* d, uint64_t off, uint32_t chunk, double limit, uint64_t cap, int ticks, run_t* r)
{
	memset(r, 0, sizeof(*r));
	r->min_s = 1e18;
	const uint64_t sec = d->sector ? d->sector : 2048;
	off -= off % sec;
	if (off + 2 * (uint64_t)chunk >= d->size)
	{
		r->errs = 1;
		r->err_no = EINVAL;
		return;
	}
	if (cap > d->size - off - chunk)
		cap = d->size - off - chunk;
	const double t_first = now();
	ssize_t n = dev_pread(d->fd, g_buf, chunk, (off_t)off);
	r->first_ms = (now() - t_first) * 1000.0;
	if (n != (ssize_t)chunk)
	{
		r->errs = 1;
		r->err_no = n < 0 ? errno : EIO;
		d->errs++;
		return;
	}
	off += chunk;
	const double lim = limit * g_scale;
	const double t0 = now();
	double last_t = t0;
	uint64_t last_b = 0;
	while (r->bytes < cap)
	{
		const double t = now();
		if (t - t0 >= lim)
			break;
		n = dev_pread(d->fd, g_buf, chunk, (off_t)off);
		if (n <= 0)
		{
			if (!r->err_no)
				r->err_no = n < 0 ? errno : EIO;
			r->errs++;
			d->errs++;
			if (n == 0 || r->errs >= 5)
				break;
			off += chunk; // skip the bad place
			continue;
		}
		r->bytes += (uint64_t)n;
		off += (uint64_t)n;
		off = (off + sec - 1) / sec * sec;
		const double t2 = now();
		if (t2 - last_t >= 1.0 * g_scale)
		{
			const double s = (double)(r->bytes - last_b) / (t2 - last_t) / 1e6;
			if (s < r->min_s)
				r->min_s = s;
			if (s > r->max_s)
				r->max_s = s;
			if (ticks)
				say("      second %4.1f: %7.2f MB/s (%.2fx %s)", t2 - t0, s, s / unit_mbps(d), kind_name(d));
			last_t = t2;
			last_b = r->bytes;
		}
	}
	r->secs = now() - t0;
	r->mbps = r->secs > 0 ? (double)r->bytes / r->secs / 1e6 : 0;
	if (r->min_s > 1e17)
		r->min_s = r->max_s = r->mbps;
}

// Where on the disc: pct percent in, at 1 MiB, leaving room before the end.
static uint64_t pos_off(const drive_t* d, int pct)
{
	uint64_t o = d->size / 100 * (uint64_t)pct;
	if (o + 80 * MIB > d->size)
		o = d->size > 80 * MIB ? d->size - 80 * MIB : 0;
	return o - o % MIB;
}

static void say_run(const drive_t* d, const char* what, const run_t* r)
{
	if (r->bytes == 0)
		say("    %-34s FAILED: %s (%d error%s)", what, strerror(r->err_no ? r->err_no : EIO), r->errs, r->errs == 1 ? "" : "s");
	else
		say("    %-34s %8.2f MB/s  %6.2fx %-3s (first read %.0f ms%s)", what, r->mbps, r->mbps / unit_mbps(d), kind_name(d), r->first_ms,
			r->errs ? ", read errors" : "");
}

static void note_best(drive_t* d, double mbps, const char* what)
{
	if (mbps > d->best_mbps)
	{
		d->best_mbps = mbps;
		snprintf(d->best_what, sizeof(d->best_what), "%s", what);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// the phases

static void phase_identity(drive_t* d)
{
	g_phase = "who the drive is";
	char why[96];
	uint8_t gc[8] = {0};
	const uint8_t c_gc[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
	if (scsi(d->pass, c_gc, 10, D_IN, gc, sizeof(gc), why, sizeof(why)) == 0)
		d->profile = be16(gc + 6);
	say("  disc: %s (profile %#06x), %.2f GB (%llu MiB), sector %u, %s", profile_name(d->profile), d->profile, (double)d->size / 1e9,
		(unsigned long long)(d->size / MIB), d->sector, d->size > 4700000000ull && disc_kind(d) == 1 ? "dual layer" : "one layer or not a DVD");
	uint8_t ms[256] = {0};
	const uint8_t c_ms[10] = {0x5A, 0x08, 0x2A, 0, 0, 0, 0, (uint8_t)(sizeof(ms) >> 8), (uint8_t)sizeof(ms), 0};
	if (scsi(d->pass, c_ms, 10, D_IN, ms, sizeof(ms), why, sizeof(why)) == 0)
	{
		const uint8_t* p = ms + 8 + be16(ms + 6);
		if ((p[0] & 0x3F) == 0x2A)
		{
			say("  MODE SENSE page 2A: max read %u kB/s, current read %u kB/s%s", be16(p + 8), be16(p + 14), be16(p + 8) ? "" : " (0: newer drives leave these fields empty)");
			if (be16(p + 8) > d->claim_kbps)
				d->claim_kbps = be16(p + 8);
		}
	}
	else
		say("  MODE SENSE page 2A: %s", why);
	uint8_t pf[1024] = {0};
	const uint8_t c_pf[12] = {0xAC, 0x00, 0, 0, 0, 0, 0, 0, 0, 32, 0x00, 0};
	if (scsi(d->pass, c_pf, 12, D_IN, pf, sizeof(pf), why, sizeof(why)) == 0)
	{
		const uint32_t n = (be32(pf) > 4 ? be32(pf) - 4 : 0) / 16;
		for (uint32_t i = 0; i < n && i < 8; i++)
		{
			const uint8_t* e = pf + 8 + 16 * i;
			say("  GET PERFORMANCE read %u: LBA %u at %u kB/s .. LBA %u at %u kB/s", i, be32(e), be32(e + 4), be32(e + 8), be32(e + 12));
			if (be32(e + 4) > d->claim_kbps)
				d->claim_kbps = be32(e + 4);
			if (be32(e + 12) > d->claim_kbps)
				d->claim_kbps = be32(e + 12);
		}
	}
	else
		say("  GET PERFORMANCE: %s", why);
	if (d->claim_kbps)
		say("  the drive says it can read up to %u kB/s = %.1fx %s", d->claim_kbps, (double)d->claim_kbps / 1000.0 / unit_mbps(d), kind_name(d));
}

static void phase_curve(drive_t* d)
{
	g_phase = "the speed map";
	say("  speed map, 128 KiB reads, the drive's default speed (inner edge -> outer edge; a CAV drive is faster outwards):");
	static const int kPct[] = {3, 25, 50, 75, 97};
	double sum = 0;
	int ok = 0;
	d->curve_min = 1e18;
	for (int i = 0; i < 5; i++)
	{
		run_t r;
		char what[48];
		snprintf(what, sizeof(what), "at %d%% of the disc", kPct[i]);
		read_run(d, pos_off(d, kPct[i]), 128 * 1024, 5, 64 * MIB, 0, &r);
		say_run(d, what, &r);
		if (r.bytes)
		{
			sum += r.mbps;
			ok++;
			if (r.mbps < d->curve_min)
				d->curve_min = r.mbps;
			if (r.mbps > d->curve_max)
				d->curve_max = r.mbps;
			note_best(d, r.mbps, "the drive's default speed");
		}
	}
	if (!ok)
		d->curve_min = 0;
	else
		say("  average %.2f MB/s (%.2fx %s)", sum / ok, sum / ok / unit_mbps(d), kind_name(d));
}

static void phase_chunks(drive_t* d)
{
	g_phase = "the read sizes";
	say("  read size (does a bigger read help?):");
	static const uint32_t kSizes[] = {32 * 1024, 128 * 1024, 1024 * 1024, 4 * 1024 * 1024};
	static const char* const kNames[] = {"32 KiB reads", "128 KiB reads", "1 MiB reads", "4 MiB reads"};
	for (int i = 0; i < 4; i++)
	{
		run_t r;
		read_run(d, pos_off(d, 8) + (uint64_t)i * 64 * MIB, kSizes[i], 4, 48 * MIB, 0, &r);
		say_run(d, kNames[i], &r);
		if (r.bytes)
			note_best(d, r.mbps, kNames[i]);
	}
}

typedef struct
{
	unsigned bursts, reads, over100;
	uint64_t bytes;
	double read_secs, wall_secs, mbps, max_ms, resume_sum, resume_max;
	int errs;
} burst_t;

// A TEST UNIT READY: what PS5SX2's reader sends the drive (through its pass device) every half second to see whether the disc is still there.
static void drive_check(drive_t* d)
{
	char why[96];
	const uint8_t c[6] = {0, 0, 0, 0, 0, 0};
	scsi(d->pass, c, 6, D_NONE, NULL, 0, why, sizeof(why));
}

// PS5SX2 reads a disc in bursts: the block the game asked for and up to 16 after it (blocks of `block` bytes, one read each), then nothing until the
// game asks again. This does the same for `limit` seconds: `blocks` reads of `block` bytes, `pause_s` of rest, and every `check_s` seconds (0 = never)
// the drive check. mbps is bytes over the time the reads took.
static void burst_run(drive_t* d, uint64_t off, uint32_t block, unsigned blocks, double pause_s, double check_s, double limit, burst_t* b)
{
	memset(b, 0, sizeof(*b));
	off -= off % MIB;
	const double lim = limit * g_scale;
	const double t0 = now();
	double last_check = t0;
	while (now() - t0 < lim && b->errs < 5)
	{
		if (off + (uint64_t)block * blocks + MIB >= d->size)
			break;
		b->bursts++;
		for (unsigned i = 0; i < blocks; i++)
		{
			const double a = now();
			const ssize_t n = dev_pread(d->fd, g_buf, block, (off_t)off);
			const double ms = (now() - a) * 1000.0;
			off += block;
			if (n != (ssize_t)block)
			{
				b->errs++;
				d->errs++;
				continue;
			}
			b->reads++;
			b->bytes += block;
			b->read_secs += ms / 1000.0;
			if (ms > b->max_ms)
				b->max_ms = ms;
			if (ms > 100.0)
				b->over100++;
			if (i == 0)
			{
				b->resume_sum += ms;
				if (ms > b->resume_max)
					b->resume_max = ms;
			}
		}
		if (check_s > 0 && now() - last_check >= check_s * g_scale)
		{
			drive_check(d);
			last_check = now();
		}
		const double rest = pause_s * g_scale;
		struct timespec ts;
		ts.tv_sec = (time_t)rest;
		ts.tv_nsec = (long)((rest - (double)ts.tv_sec) * 1e9);
		nanosleep(&ts, NULL);
	}
	b->wall_secs = now() - t0;
	b->mbps = b->read_secs > 0 ? (double)b->bytes / b->read_secs / 1e6 : 0;
}

static void say_burst(const drive_t* d, const char* what, const burst_t* b)
{
	if (!b->reads)
	{
		say("    %-44s FAILED (%d errors)", what, b->errs);
		return;
	}
	say("    %-44s %7.2f MB/s (%.2fx %s) | slowest read %6.0f ms | first read after a pause: avg %.0f ms, worst %.0f ms | %u of %u reads over 100 ms%s", what,
		b->mbps, b->mbps / unit_mbps(d), kind_name(d), b->max_ms, b->bursts ? b->resume_sum / b->bursts : 0.0, b->resume_max, b->over100, b->reads,
		b->errs ? " | read errors" : "");
}

static void phase_stutter(drive_t* d)
{
	g_phase = "how PS5SX2 reads";
	say("  how PS5SX2 reads (a burst of 17 x 32 KiB = the block asked for and 16 read ahead, 150 ms pause, 10 s each; the slowest read is what stutters):");
	burst_t a, b, c;
	burst_run(d, pos_off(d, 20), 32 * 1024, 17, 0.15, 0.5, 10, &a);
	say_burst(d, "PS5SX2 now: 32 KiB reads + drive check every 0.5 s", &a);
	burst_run(d, pos_off(d, 40), 32 * 1024, 17, 0.15, 0, 10, &b);
	say_burst(d, "32 KiB reads, no drive check", &b);
	burst_run(d, pos_off(d, 60), 128 * 1024, 4, 0.15, 0, 10, &c);
	say_burst(d, "128 KiB reads (the 5S2CD fork), no drive check", &c);
	d->have_app = a.reads && b.reads && c.reads;
	d->app_mbps = a.mbps;
	d->app_max_ms = a.max_ms;
	d->nocheck_mbps = b.mbps;
	d->nocheck_max_ms = b.max_ms;
	d->fork_mbps = c.mbps;
	d->fork_max_ms = c.max_ms;
	if (a.reads)
		note_best(d, a.mbps, "PS5SX2's own reads");
	if (b.reads)
		note_best(d, b.mbps, "32 KiB reads");
	if (c.reads)
		note_best(d, c.mbps, "128 KiB reads");
}

typedef struct
{
	unsigned visits, reads;
	uint64_t bytes;
	double wall, mbps, first_sum, first_max, max_ms;
	int errs;
} alt_t;

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 7;
	g_rng ^= g_rng << 17;
	return g_rng;
}

// 32 KiB reads at random places, one at a time, for `limit` seconds: every one is a seek.
static void rand_run(drive_t* d, double limit, alt_t* r)
{
	memset(r, 0, sizeof(*r));
	const double lim = limit * g_scale;
	const double t0 = now();
	const uint64_t slots = d->size > 4 * MIB ? (d->size - 4 * MIB) / 2048 : 0;
	while (slots && now() - t0 < lim && r->visits < 4000 && r->errs < 5)
	{
		const uint64_t off = (rnd() % slots) * 2048;
		const double a = now();
		const ssize_t n = dev_pread(d->fd, g_buf, 32 * 1024, (off_t)off);
		const double ms = (now() - a) * 1000.0;
		if (n != 32 * 1024)
		{
			r->errs++;
			d->errs++;
			continue;
		}
		r->visits++;
		r->reads++;
		r->bytes += 32 * 1024;
		r->first_sum += ms;
		if (ms > r->max_ms)
			r->max_ms = ms;
	}
	r->wall = now() - t0;
	r->mbps = r->wall > 0 ? (double)r->bytes / r->wall / 1e6 : 0;
}

// Two places of the disc read in turn, `visit` bytes (32 KiB reads) from one, then from the other, and so on: every change is a seek.
static void alt_run(drive_t* d, uint64_t off_a, uint64_t off_b, uint32_t visit, double limit, alt_t* r)
{
	memset(r, 0, sizeof(*r));
	const uint32_t block = 32 * 1024;
	const double lim = limit * g_scale;
	const double t0 = now();
	uint64_t off[2] = {off_a - off_a % MIB, off_b - off_b % MIB};
	int which = 0;
	while (now() - t0 < lim && r->errs < 5)
	{
		if (off[which] + visit + MIB >= d->size)
			break;
		for (uint32_t done = 0; done < visit; done += block)
		{
			const double a = now();
			const ssize_t n = dev_pread(d->fd, g_buf, block, (off_t)off[which]);
			const double ms = (now() - a) * 1000.0;
			off[which] += block;
			if (n != (ssize_t)block)
			{
				r->errs++;
				d->errs++;
				continue;
			}
			r->reads++;
			r->bytes += block;
			if (ms > r->max_ms)
				r->max_ms = ms;
			if (done == 0)
			{
				r->visits++;
				r->first_sum += ms;
				if (ms > r->first_max)
					r->first_max = ms;
			}
		}
		which ^= 1;
	}
	r->wall = now() - t0;
	r->mbps = r->wall > 0 ? (double)r->bytes / r->wall / 1e6 : 0;
}

static void phase_seeks(drive_t* d)
{
	g_phase = "seeks";
	say("  seeks (a seek costs the drive 100-300 ms; what is read for each one decides what a game gets):");
	alt_t r;
	rand_run(d, 8, &r);
	if (r.visits)
	{
		say("    random 32 KiB reads, one at a time: %u reads in %.1f s = %.2f MB/s | average %.0f ms a read, slowest %.0f ms", r.visits, r.wall, r.mbps,
			r.first_sum / r.visits, r.max_ms);
		d->rand_avg_ms = r.first_sum / r.visits;
		d->rand_max_ms = r.max_ms;
	}
	else
		say("    random 32 KiB reads: FAILED (%d errors)", r.errs);
	static const uint32_t kVisit[] = {32 * 1024, 128 * 1024, 512 * 1024};
	static const char* const kName[] = {"32 KiB at a time (a game asking for one block, then another place)",
		"128 KiB at a time (the 5S2CD fork's read-ahead window)", "512 KiB at a time (PS5SX2's block plus its 16 read ahead)"};
	double got[3] = {0, 0, 0};
	for (int i = 0; i < 3; i++)
	{
		alt_t a;
		alt_run(d, pos_off(d, 15), pos_off(d, 65), kVisit[i], 6, &a);
		if (a.reads)
		{
			say("    two streams in turn, %-62s %7.2f MB/s | seek avg %.0f ms, slowest read %.0f ms", kName[i], a.mbps, a.visits ? a.first_sum / a.visits : 0.0,
				a.max_ms);
			got[i] = a.mbps;
		}
		else
			say("    two streams in turn, %s: FAILED (%d errors)", kName[i], a.errs);
	}
	d->alt32 = got[0];
	d->alt128 = got[1];
	d->alt512 = got[2];
	d->have_seek = r.visits > 0 && got[0] > 0 && got[1] > 0 && got[2] > 0;
}

static void phase_sustained(drive_t* d)
{
	g_phase = "20 seconds in one go";
	say("  20 seconds in one go (128 KiB reads), second by second:");
	run_t r;
	read_run(d, pos_off(d, 15), 128 * 1024, 20, 400 * MIB, 1, &r);
	say_run(d, "20 s sustained", &r);
	if (r.bytes && r.min_s < 1e17)
		say("    slowest second %.2f MB/s, fastest %.2f MB/s", r.min_s, r.max_s);
}

// Times two places with the setting `what` in force; the speed maps are already above, this says what the SETTING changed.
static void time_setting(drive_t* d, const char* what)
{
	run_t a, b;
	read_run(d, pos_off(d, 10), 128 * 1024, 4, 48 * MIB, 0, &a);
	read_run(d, pos_off(d, 90), 128 * 1024, 4, 48 * MIB, 0, &b);
	char la[96], lb[96];
	snprintf(la, sizeof(la), "%s, inner", what);
	snprintf(lb, sizeof(lb), "%s, outer", what);
	say_run(d, la, &a);
	say_run(d, lb, &b);
	if (a.bytes)
		note_best(d, a.mbps, what);
	if (b.bytes)
		note_best(d, b.mbps, what);
}

static int sony_speed(drive_t* d, unsigned rot, unsigned speed, char* why, size_t wmax)
{
	const uint8_t c[12] = {0xDB, (uint8_t)rot, (uint8_t)(speed >> 8), (uint8_t)speed, 0, 0, 0, 0, 0, 0, 0, 0};
	return scsi(d->pass, c, 12, D_NONE, NULL, 0, why, wmax);
}

static void phase_settings(drive_t* d)
{
	g_phase = "the speed settings";
	char why[96];
	if (d->sony)
	{
		say("  Sony's speed command DB (rotation 0..3 x 11 speeds; a refused one costs nothing, a taken one is timed):");
		static const uint16_t kSpeeds[] = {0x0020, 0x0032, 0x0040, 0x0050, 0x0060, 0x0080, 0x00A0, 0x00C0, 0x0100, 0x0180, 0xFFFF};
		int taken = 0, refused = 0;
		for (unsigned rot = 0; rot < 4; rot++)
			for (size_t i = 0; i < sizeof(kSpeeds) / sizeof(kSpeeds[0]); i++)
			{
				const unsigned sp = kSpeeds[i];
				if (sony_speed(d, rot, sp, why, sizeof(why)) != 0)
				{
					refused++;
					say("    DB %02x %02x %02x: refused, %s", rot, sp >> 8, sp & 0xff, why);
					continue;
				}
				taken++;
				char what[64];
				snprintf(what, sizeof(what), "Sony DB %02x %02x %02x", rot, sp >> 8, sp & 0xff);
				say("    %s: TAKEN", what);
				time_setting(d, what);
			}
		say("  Sony DB: %d taken, %d refused", taken, refused);
		sony_speed(d, 0, 0x20, why, sizeof(why)); // back to the default
	}
	say("  SET CD SPEED (standard, read speed = maximum):");
	const uint8_t c_bb[12] = {0xBB, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
	if (scsi(d->pass, c_bb, 12, D_NONE, NULL, 0, why, sizeof(why)) != 0)
		say("    SET CD SPEED max: refused, %s", why);
	else
	{
		say("    SET CD SPEED max: TAKEN");
		time_setting(d, "SET CD SPEED max");
	}
	say("  SET STREAMING (standard, the whole disc as fast as the drive can):");
	uint8_t p[28] = {0};
	const uint32_t end = (uint32_t)(d->size / (d->sector ? d->sector : 2048) - 1);
	p[8] = (uint8_t)(end >> 24), p[9] = (uint8_t)(end >> 16), p[10] = (uint8_t)(end >> 8), p[11] = (uint8_t)end;
	p[12] = 0x10, p[13] = 0, p[14] = 0, p[15] = 0; // read size 0x10000000 kB ...
	p[18] = 1000 >> 8, p[19] = 1000 & 0xff;       // ... per 1000 ms
	p[20] = 0x10, p[21] = 0, p[22] = 0, p[23] = 0;
	p[26] = 1000 >> 8, p[27] = 1000 & 0xff;
	const uint8_t c_ss[12] = {0xB6, 0, 0, 0, 0, 0, 0, 0, 0, 0, sizeof(p), 0};
	if (scsi(d->pass, c_ss, 12, D_OUT, p, sizeof(p), why, sizeof(why)) != 0)
		say("    SET STREAMING max: refused, %s", why);
	else
	{
		say("    SET STREAMING max: TAKEN");
		time_setting(d, "SET STREAMING max");
		memset(p, 0, sizeof(p));
		p[0] = 0x04; // RDD: the drive's defaults back
		scsi(d->pass, c_ss, 12, D_OUT, p, sizeof(p), why, sizeof(why));
	}
	if (d->sony)
	{
		sony_speed(d, 0, 0x20, why, sizeof(why));
		say("  Sony drive back to its default (DB 00 00 20)");
	}
}

static void headline(const drive_t* d, char* out, size_t n)
{
	char lab[160];
	if (!d->tested)
	{
		snprintf(out, n, "%s: no disc, not tested", label_of(d, lab, sizeof(lab)));
		return;
	}
	size_t at = (size_t)snprintf(out, n, "%s: %s %.1f GB, default speed %.1f-%.1f MB/s (%.1f-%.1fx %s), best %.1f MB/s (%.1fx) with %s", label_of(d, lab, sizeof(lab)),
		profile_name(d->profile), (double)d->size / 1e9, d->curve_min, d->curve_max, d->curve_min / unit_mbps(d), d->curve_max / unit_mbps(d),
		kind_name(d), d->best_mbps, d->best_mbps / unit_mbps(d), d->best_what[0] ? d->best_what : "-");
	if (d->claim_kbps && at < n)
		at += (size_t)snprintf(out + at, n - at, ", the drive claims up to %.1fx", (double)d->claim_kbps / 1000.0 / unit_mbps(d));
	if (d->errs && at < n)
		snprintf(out + at, n - at, " (%d read errors)", d->errs);
}

static void test_drive(drive_t* d)
{
	char lab[160];
	say("");
	say("=== %s", label_of(d, lab, sizeof(lab)));
	d->tested = 1;
	phase_identity(d);
	phase_curve(d);
	phase_chunks(d);
	phase_stutter(d);
	phase_seeks(d);
	phase_sustained(d);
	phase_settings(d);
	char h[400];
	headline(d, h, sizeof(h));
	say("  => %s", h);
	notify("PS5SX2 %s", h);
}

// Drives without a disc: waits for one (a minute) so the person can put it in after loading the ELF.
static void wait_for_discs(void)
{
	char names[200] = "";
	int missing = 0;
	for (int i = 0; i < g_nd; i++)
		if (!g_d[i].disc)
		{
			missing++;
			const size_t at = strlen(names);
			snprintf(names + at, sizeof(names) - at, "%s%s", at ? ", " : "", g_d[i].name);
		}
	if (!missing)
		return;
	say("no disc in: %s. Waiting up to a minute for one to be put in.", names);
	notify("PS5SX2 disc drive test: no disc in %s. Put one in within a minute (a DVD, CD or Blu-ray).", names);
	const double t0 = now();
	g_phase = "waiting for a disc";
	while (now() - t0 < 60.0 * g_scale)
	{
		g_progress++;
		missing = 0;
		for (int i = 0; i < g_nd; i++)
		{
			if (!g_d[i].disc)
				check_disc(&g_d[i]);
			if (!g_d[i].disc)
				missing++;
		}
		if (!missing)
			break;
		struct timespec ts = {0, 500 * 1000 * 1000};
		nanosleep(&ts, NULL);
	}
	if (!missing)
	{
		say("all drives have a disc now; letting them settle");
		struct timespec ts = {(time_t)(3 * g_scale) + 1, 0};
		nanosleep(&ts, NULL);
	}
}

static void unlock(void)
{
	char lock[300];
	snprintf(lock, sizeof(lock), "%s.lock", g_log_path);
	unlink(lock);
}

// The watchdog: a read or a drive command that does not come back (a scratched disc, a drive that hangs) used to end the log with no word. It says
// where, every 30 s; after g_wd_giveup seconds it gives the run up so the next load is not refused.
static void* watchdog(void* arg)
{
	(void)arg;
	uint64_t last = g_progress;
	double last_t = now(), said_at = 0;
	while (!g_done)
	{
		struct timespec ts = {1, 0};
		nanosleep(&ts, NULL);
		if (g_progress != last)
		{
			last = g_progress;
			last_t = now();
			said_at = 0;
			continue;
		}
		const double stuck = now() - last_t;
		if (stuck >= g_wd_warn && stuck - said_at >= (said_at == 0 ? 0 : 30))
		{
			said_at = stuck;
			say("!! nothing came back for %.0f s while \"%s\" (the read at %llu MiB, or the drive command %#04x, is stuck)", stuck, g_phase,
				(unsigned long long)(g_where / MIB), g_cmd);
		}
		if (stuck >= g_wd_giveup)
		{
			say("!! giving up after %.0f s: the drive does not answer. The log above is all there is; restart the console before the next test.", stuck);
			notify("PS5SX2 disc drive test: the drive stopped answering (%s). Send %s, then restart the console.", g_phase, g_log_path);
			unlock();
#ifdef HOST_TEST
			_exit(2);
#else
			kill(getpid(), SIGKILL); // (not _exit: the helper that runs on consoles imports kill, not _exit)
			for (;;)
				sleep(1);
#endif
		}
	}
	return NULL;
}

int main(void)
{
#ifdef HOST_TEST
	fake_init();
	if (getenv("DISCTEST_LOG"))
		snprintf(g_log_path, sizeof(g_log_path), "%s", getenv("DISCTEST_LOG"));
#endif
	if (getenv("DISCTEST_SCALE"))
		g_scale = atof(getenv("DISCTEST_SCALE"));
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
	// One run at a time: a second ELF loaded while the first is working would measure the first. The lock is a file with the time and the process id
	// in it; one older than 20 minutes, or of a process that is gone (a run that was killed), is taken over.
	int stale_lock = 0;
	{
		char lock[300];
		snprintf(lock, sizeof(lock), "%s.lock", g_log_path);
		FILE* lf = fopen(lock, "r");
		long long at = 0;
		int pid = 0;
		if (lf)
		{
			if (fscanf(lf, "%lld %d", &at, &pid) < 1)
				at = 0;
			fclose(lf);
		}
		const long long t_now = (long long)time(NULL);
		int alive = 1;
		if (pid > 0)
			alive = kill((pid_t)pid, 0) == 0 || errno == EPERM;
		if (at > 0 && t_now >= at && t_now - at < 20 * 60 && alive)
		{
			notify("PS5SX2 disc drive test is already running (it started %lld s ago): wait for its \"done\" popup. If it never comes, restart the console.",
				t_now - at);
			printf("another disc drive test is running (lock %s)\n", lock);
			return 1;
		}
		stale_lock = at > 0;
		lf = fopen(lock, "w");
		if (lf)
		{
			fprintf(lf, "%lld %d\n", t_now, (int)getpid());
			fclose(lf);
		}
	}
	g_log = fopen(g_log_path, "a");
	if (posix_memalign((void**)&g_buf, 4096, 4 * MIB) != 0)
	{
		say("out of memory");
		unlock();
		return 1;
	}
	const time_t when = time(NULL);
	say("");
	say("############################################################################");
	say("PS5SX2 %s (%s) %s", BUILD_NAME, SCOPE == 1 ? "the PS5's own drive" : SCOPE == 2 ? "external drives" : "the PS5's own drive and external drives", ctime(&when));
	say("(a complete log ends with the line \"=== run finished\"; one that does not is a run still going or one that died: wait for the \"done\" popup)");
	if (stale_lock)
		say("(the lock of an earlier run was found: that run did not finish, or was stopped; its log ends without \"=== run finished\")");
	{
		pthread_t wd;
		if (pthread_create(&wd, NULL, watchdog, NULL) == 0)
			pthread_detach(wd);
		else
			say("(no watchdog thread)");
	}
	console_info();
	survey_devices();

	char names[MAX_DRIVES * 2][16];
	const int ncd = list_cd_devices(names, MAX_DRIVES * 2);
	for (int i = 0; i < ncd; i++)
		add_drive(names[i]);
	if (g_nd == 0)
	{
		say("no optical drive to test (%s)", SCOPE == 2 ? "no external DVD/CD/BD drive is seen by the console: see the /dev lines above" : "none found");
		notify("PS5SX2 disc drive test: no %s found", SCOPE == 2 ? "external disc drive" : "disc drive");
		if (g_log)
			fclose(g_log);
		unlock();
		return 1;
	}
	notify("PS5SX2 disc drive test: %d drive%s, about 3 min each. Keep the disc in (it is only read) and wait for the \"done\" popup before taking the log.", g_nd,
		g_nd == 1 ? "" : "s");
	wait_for_discs();

	int tested = 0;
	for (int i = 0; i < g_nd; i++)
	{
		if (!g_d[i].disc)
		{
			char lab[160];
			say("");
			say("=== %s: no disc, skipped", label_of(&g_d[i], lab, sizeof(lab)));
			continue;
		}
		test_drive(&g_d[i]);
		tested++;
	}
	say("");
	say("=== summary");
	for (int i = 0; i < g_nd; i++)
	{
		char h[400];
		headline(&g_d[i], h, sizeof(h));
		say("RESULT | %s", h);
		if (g_d[i].have_app)
			say("RESULT |   reads: PS5SX2 now %.2f MB/s (slowest read %.0f ms), 32 KiB without the drive check %.2f MB/s (%.0f ms), 128 KiB reads %.2f MB/s (%.0f ms)",
				g_d[i].app_mbps, g_d[i].app_max_ms, g_d[i].nocheck_mbps, g_d[i].nocheck_max_ms, g_d[i].fork_mbps, g_d[i].fork_max_ms);
		if (g_d[i].have_seek)
			say("RESULT |   seeks: random 32 KiB read %.0f ms (slowest %.0f ms); two streams in turn: 32 KiB at a time %.2f MB/s, 128 KiB %.2f MB/s, 512 KiB %.2f MB/s",
				g_d[i].rand_avg_ms, g_d[i].rand_max_ms, g_d[i].alt32, g_d[i].alt128, g_d[i].alt512);
	}
	say("log: %s", g_log_path);
	say("=== run finished");
	g_done = 1;
	unlock();
	if (tested)
		notify("PS5SX2 disc drive test done. Send %s", g_log_path);
	else
		notify("PS5SX2 disc drive test: nothing was tested (no disc). Put a disc in and load it again.");
	if (g_log)
		fclose(g_log);
	for (int i = 0; i < g_nd; i++)
	{
		if (g_d[i].pass >= 0)
			close(g_d[i].pass);
		if (g_d[i].fd >= 0)
			close(g_d[i].fd);
	}
	sleep(1);
	return tested ? 0 : 1;
}
