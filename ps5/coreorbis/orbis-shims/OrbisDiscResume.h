// PS5SX2 (2.11, AI-assisted): taking a disc copy up again after the app restarted, kept apart from the PS5 calls so a PC test can run it.
//
// swordpdf: "if you're copying a disc, start a game, and go back, it asks you again to play from disc or copy to storage ... the disk copying only
// works if you don't change games or stay on the dashboard". Going back to the menu from a game starts the app afresh (the console only lets it
// come back to the shelf by re-executing itself), and that ends the copy's thread. The copy goes to games/<name>.iso.part; beside it
// games/<name>.iso.part.state says how far the writing had come ("<serial> <disc bytes> <bytes written> <unreadable sectors>"), rewritten about
// every 1%. The next start finds the pair, checks it names the disc in the drive, and carries on a little before that point.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace orbis_disc
{
// How far before the state's point a copy carries on (a restart may have cut through a write; the unreadable sectors in that stretch are counted again).
constexpr uint64_t kResumeRewind = 16ull << 20;

struct ResumeState
{
	std::string serial;
	uint64_t total = 0; // the disc's size
	uint64_t done = 0;  // bytes written to the .part when the state was kept
	uint64_t bad = 0;   // sectors written as zeros BELOW the resume point (done - kResumeRewind): the ones after it are counted again when they are re-read
};

inline std::string FormatResumeState(const ResumeState& s)
{
	char buf[160];
	std::snprintf(buf, sizeof(buf), "%s %llu %llu %llu\n", s.serial.c_str(), static_cast<unsigned long long>(s.total),
		static_cast<unsigned long long>(s.done), static_cast<unsigned long long>(s.bad));
	return buf;
}

inline bool ParseResumeState(const std::string& text, ResumeState& out)
{
	char serial[64] = {};
	unsigned long long total = 0, done = 0, bad = 0;
	if (std::sscanf(text.c_str(), "%63s %llu %llu %llu", serial, &total, &done, &bad) != 4)
		return false;
	out.serial = serial;
	out.total = total;
	out.done = done;
	out.bad = bad;
	return true;
}

// Where to carry a cut-short copy on from: the offset to read from, or 0 to start over. The state has to name this disc (serial and size) and the
// .part has to hold at least what the state says (a power cut can leave less than it); the last `rewind` bytes are written again, as a write the
// restart cut through may have left a torn end. Under that much done it isn't worth carrying on.
inline uint64_t ResumeOffset(const ResumeState& st, const std::string& serial, uint64_t total, uint64_t part_size, uint64_t rewind = kResumeRewind)
{
	if (st.serial != serial || st.total != total || total == 0 || st.done > total || part_size < st.done || st.done <= rewind)
		return 0;
	return (st.done - rewind) / 2048 * 2048;
}
} // namespace orbis_disc
