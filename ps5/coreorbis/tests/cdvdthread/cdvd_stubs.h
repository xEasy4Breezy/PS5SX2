// PS5SX2 (2.11, AI-assisted): what pcsx2/CDVD/CDVDdiscThread.cpp needs from the rest of PCSX2 and from the drive layer, as stand-ins for the PC tests
// (test_cdvdthread.cpp: functional; bench_cdvdthread.cpp: the simulation of the CPU thread's waits). The REAL thread file is compiled and linked with
// this; only the drive (IOCtlSrc) and the few globals CDVDdiscReader.cpp and CDVDcommon.cpp own are faked.
//
// Include it from exactly one translation unit of a program: it DEFINES the globals.
//
// The test program defines the drive: `FakeDrive* g_drive` points at an object implementing Ready() (IOCtlSrc::DiscReady) and Read2048().
// The switch files (the app's OrbisFlag) come from `g_flags`.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "CDVD/CDVDdiscReader.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>

struct FakeDrive
{
	virtual ~FakeDrive() = default;
	virtual bool Ready() { return true; }
	virtual bool Read2048(u32 sector, u32 count, u8* out) = 0;
	u32 total = 2000000; // sectors
};
extern FakeDrive* g_drive;

// The switch files that are "there" (read by the thread through the weak OrbisFlag in cdvdStartThread).
static std::mutex g_flags_lock;
static std::set<std::string> g_flags;
bool OrbisFlag(const char* name)
{
	std::lock_guard<std::mutex> lock(g_flags_lock);
	return g_flags.count(name) != 0;
}
static void SetFlags(std::initializer_list<const char*> names)
{
	std::lock_guard<std::mutex> lock(g_flags_lock);
	g_flags.clear();
	for (const char* n : names)
		g_flags.insert(n);
}

// Globals CDVDdiscReader.cpp / CDVDcommon.cpp own.
int curDiskType = 0;
int curTrayStatus = 0;
bool disc_has_changed = false;
bool weAreInNewDiskCB = false;
void (*newDiscCB)() = nullptr;
u8 strack = 1, etrack = 1;
std::unique_ptr<IOCtlSrc> src;
void cdvdParseTOC() {}

// IOCtlSrc (the non-PS5 layout of CDVDdiscReader.h) on top of the fake drive.
IOCtlSrc::IOCtlSrc(std::string f)
	: m_filename(std::move(f))
{
}
IOCtlSrc::~IOCtlSrc() {}
bool IOCtlSrc::Reopen(Error*)
{
	m_sectors = g_drive->total;
	m_media_type = 0;
	return true;
}
u32 IOCtlSrc::GetSectorCount() const { return m_sectors; }
s32 IOCtlSrc::GetMediaType() const { return m_media_type; }
const std::vector<toc_entry>& IOCtlSrc::ReadTOC() const
{
	static std::vector<toc_entry> e;
	return e;
}
u32 IOCtlSrc::GetLayerBreakAddress() const { return 0; }
void IOCtlSrc::SetSpindleSpeed(bool) const {}
bool IOCtlSrc::ReadTrackSubQ(cdvdSubQ*) const { return false; }
bool IOCtlSrc::DiscReady() { return g_drive->Ready(); }
bool IOCtlSrc::ReadSectors2352(u32, u32, u8*) const { return false; }
bool IOCtlSrc::ReadSectors2048(u32 sector, u32 count, u8* buffer) const { return g_drive->Read2048(sector, count, buffer); }

// The data of sector `lsn` is a function of the sector, so any copy can be checked.
static inline uint8_t SectorByte(uint32_t lsn, uint32_t i)
{
	return static_cast<uint8_t>((lsn * 2654435761u + i * 40503u) >> 13);
}
static inline void FillSectors(uint32_t sector, uint32_t count, uint8_t* out)
{
	for (uint32_t s = 0; s < count; s++)
		for (uint32_t i = 0; i < 2048; i++)
			out[s * 2048 + i] = SectorByte(sector + s, i);
}
static inline bool SectorMatches(uint32_t sector, const uint8_t* p)
{
	for (uint32_t i = 0; i < 2048; i += 7)
		if (p[i] != SectorByte(sector, i))
			return false;
	return p[2047] == SectorByte(sector, 2047);
}
