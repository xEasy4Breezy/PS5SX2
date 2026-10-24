// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "CDVDcommon.h"
#include "common/Pcsx2Defs.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Error;
#ifdef __PROSPERO__
namespace orbis_disc
{
class ReadAhead;
class ReadyPolicy;
}
#endif

extern int curDiskType;
extern int curTrayStatus;

struct toc_entry
{
	u32 lba;
	u8 track;
	u8 adr : 4;
	u8 control : 4;
};

class IOCtlSrc
{
	IOCtlSrc(const IOCtlSrc&) = delete;
	IOCtlSrc& operator=(const IOCtlSrc&) = delete;

	std::string m_filename;

#if defined(_WIN32)
	HANDLE m_device = INVALID_HANDLE_VALUE;
	mutable std::mutex m_lock;
#else
	int m_device = -1;
#endif
#ifdef __PROSPERO__
	// PS5SX2 (ps5/coreorbis/orbis-shims/OrbisIOCtlSrc.cpp): the drive's CAM pass-through device (raw CD sectors, the
	// sub-channel, TEST UNIT READY), the lock that keeps Reopen from closing the descriptors under a read on another
	// thread (the CDVD thread, its keep-alive thread, the CPU thread's sub-channel reads), and DiscReady's last answer.
	int m_pass = -1;
	mutable std::mutex m_lock;
	bool m_ready = false;
	bool m_said_unreadable = false;
	mutable bool m_raw_off = false; // READ CD didn't work on this disc: its sectors are made from their 2048 bytes
	bool m_fast = false;            // the PS5's own drive was set to 3.2x (on by default, switch file nodisc32): put back at the end
	// 2.11: read-ahead windows in front of ReadSectors2048 (ps5/coreorbis/orbis-shims/OrbisDiscReadAhead.h).
	mutable std::unique_ptr<orbis_disc::ReadAhead> m_ahead;
	// 2.11: when DiscReady asks the drive, and what it makes of the answer (ps5/coreorbis/orbis-shims/OrbisDiscReady.h).
	std::unique_ptr<orbis_disc::ReadyPolicy> m_ready_policy;
#endif

	s32 m_media_type = 0;
	u32 m_sectors = 0;
	u32 m_layer_break = 0;
	std::vector<toc_entry> m_toc;

	bool ReadDVDInfo();
	bool ReadCDInfo();

public:
	IOCtlSrc(std::string filename);
	~IOCtlSrc();

	bool Reopen(Error* error);

	u32 GetSectorCount() const;
	const std::vector<toc_entry>& ReadTOC() const;
	bool ReadSectors2048(u32 sector, u32 count, u8* buffer) const;
	bool ReadSectors2352(u32 sector, u32 count, u8* buffer) const;
	bool ReadTrackSubQ(cdvdSubQ* subq) const;
	u32 GetLayerBreakAddress() const;
	s32 GetMediaType() const;
	void SetSpindleSpeed(bool restore_defaults) const;
	bool DiscReady();
#ifdef __PROSPERO__
	// 2.11: the drive's part of the CDVD thread's 10 s line in boot.log ("windows W, hits H | speed ..."): up to `size` bytes into `out`, the
	// length written (0 when the drive is busy: the line goes without it).
	size_t DriveStats(char* out, size_t size) const;
#endif
};

#ifdef __PROSPERO__
// 2.11: the time (steady clock, ms) of the last read that really reached the drive, -1 for none. A read the read-ahead windows served from memory
// doesn't count: it proves nothing about the drive. DiscReady and the keep-alive thread use it (OrbisIOCtlSrc.cpp).
s64 OrbisDiscLastReadMs();
#endif

extern std::unique_ptr<IOCtlSrc> src;

std::vector<std::string> GetOpticalDriveList();
void GetValidDrive(std::string& drive);

extern bool disc_has_changed;
extern bool weAreInNewDiskCB;

extern void (*newDiscCB)();

void cdvdStartThread();
void cdvdStopThread();
void cdvdRequestSector(u32 sector, s32 mode);
u8* cdvdGetSector(u32 sector, s32 mode);
s32 cdvdDirectReadSector(u32 sector, s32 mode, u8* buffer);
void cdvdRefreshData();
void cdvdParseTOC();

// PS5SX2 (2.11, AI-assisted; CDVDdiscThread.cpp). cdvdSetWaitCapMs: how long the CPU thread waits in cdvdGetSector for the CDVD thread's read of a block
// before it reads the block itself (3000 ms; a setter for the test). cdvdQuiesce: the CDVD thread starts no new drive read and the call waits (at most
// `max_ms`) for the one in flight; cdvdResume undoes it (the app is about to re-execute itself and the drive should not be mid-read; it never takes
// the drive's lock and sends no command).
void cdvdSetWaitCapMs(u32 ms);
void cdvdQuiesce(int max_ms);
void cdvdResume();
// The switch file discasync (off): cdvdAsyncOn says it is there (read in cdvdStartThread); cdvdTryGetSector is cdvdGetSector that answers -2 ("not yet, ask
// again") instead of waiting, for cdvdReadInterrupt only (CDVD.cpp, the PS5SX2 hunk; see DISCgetBuffer).
bool cdvdAsyncOn();
s32 cdvdTryGetSector(u32 sector, s32 mode, u8** out);
extern bool g_cdvd_async_ok;
