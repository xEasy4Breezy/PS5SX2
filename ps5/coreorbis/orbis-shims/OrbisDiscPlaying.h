// PS5SX2 (2.11, AI-assisted): logs/disc-playing.txt, the note the app leaves for the disc helper (the payload that runs beside it and polls the disc drives)
// while a game plays from a drive: one line "<unix time> <device> play\n", for example "1760611200 /dev/cd0 play". The helper leaves that drive alone while the
// time in it is fresh (the app rewrites it every 2 s; a few seconds old at most) and treats a missing or stale file as "nothing plays".
//
// The app writes it to "<path>.tmp" and renames that over the note, so a reader never sees half a line. Plain functions on std::string and stdio, so the PC
// test (tests/discready) can check the format and the write; the app's part (when it is written, ProsperoDiscDump.cpp) is the claim of the drive.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace orbis_disc
{
inline std::string PlayingLine(long long now, const std::string& device)
{
	return std::to_string(now) + " " + device + " play\n";
}

// The note's text: its time and device. False for anything but exactly "<digits> <device> play\n" (no spaces in the device).
inline bool ParsePlayingLine(const std::string& text, long long* when, std::string* device)
{
	const size_t a = text.find(' ');
	if (a == std::string::npos || a == 0 || text.size() < a + 2 || text.back() != '\n')
		return false;
	for (size_t i = 0; i < a; i++)
		if (text[i] < '0' || text[i] > '9')
			return false;
	const size_t b = text.find(' ', a + 1);
	if (b == std::string::npos || b == a + 1 || text.compare(b, std::string::npos, " play\n") != 0)
		return false;
	if (when)
		*when = std::atoll(text.substr(0, a).c_str());
	if (device)
		*device = text.substr(a + 1, b - a - 1);
	return true;
}

// Writes the note at `path` (through "<path>.tmp" and a rename). True when it is there.
inline bool WritePlayingStamp(const std::string& path, long long now, const std::string& device)
{
	const std::string tmp = path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "w");
	if (!f)
		return false;
	const std::string line = PlayingLine(now, device);
	const bool wrote = std::fwrite(line.data(), 1, line.size(), f) == line.size();
	if (std::fclose(f) == 0 && wrote && std::rename(tmp.c_str(), path.c_str()) == 0)
		return true;
	std::remove(tmp.c_str());
	return false;
}
} // namespace orbis_disc
