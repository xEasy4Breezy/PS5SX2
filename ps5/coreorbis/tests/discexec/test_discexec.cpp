// PS5SX2 (2.11, AI-assisted): when the app honours the disc helper's exec signal (orbis-shims/OrbisDiscExec.h), on a PC.
//
// The decision table below is written out LITERALLY, one row per combination with the reason, and is not a second copy of the rule in the header, so
// a change of the rule (or of its order) has to change a row here. What this test does NOT run: the watcher thread (ProsperoDiscDump.cpp) and the
// disc helper; they need a console. The rows say what the console must show (see the checklist in the build's note).
//   ps5/coreorbis/tests/discexec/test-discexec.sh
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later
#include "OrbisDiscExec.h" // (the test script gives -I for orbis-shims; the mutant run gives -I for a changed copy)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/stat.h>

using orbis_disc::DecideExec;
using orbis_disc::ExecIn;
using orbis_disc::ExecVerdict;

static int fails = 0;
static int checks = 0;
static void Check(bool ok, const std::string& what)
{
	checks++;
	if (!ok)
	{
		fails++;
		std::printf("FAIL  %s\n", what.c_str());
	}
}

static const char* Name(ExecVerdict v)
{
	switch (v)
	{
		case ExecVerdict::Honor: return "Honor";
		case ExecVerdict::IgnoreTile: return "IgnoreTile";
		case ExecVerdict::IgnoreGameStarting: return "IgnoreGameStarting";
		case ExecVerdict::IgnoreFlag: return "IgnoreFlag";
	}
	return "?";
}

struct Row
{
	bool tile, starting, discexec, nodiscexec;
	ExecVerdict want;
	const char* why;
};

// tile, game starting, flag discexec, flag nodiscexec -> verdict. All 16 combinations, written out.
static const Row kRows[] = {
	// not a tile
	{false, false, false, false, ExecVerdict::Honor, "idle shelf / PS2 CD or USB DVD copied by the helper: the respawn is the way the game starts"},
	{false, true, false, false, ExecVerdict::IgnoreGameStarting, "end of a copy: the app started the game itself; a game that runs is not killed"},
	{false, false, true, false, ExecVerdict::Honor, "flag discexec with nothing starting: as before"},
	{false, true, true, false, ExecVerdict::Honor, "flag discexec: the old behaviour, a game that is starting is exec'd over (the A/B)"},
	{false, false, false, true, ExecVerdict::IgnoreFlag, "flag nodiscexec: ignored whatever"},
	{false, true, false, true, ExecVerdict::IgnoreFlag, "nodiscexec while a game starts: ignored; the flag is named as the reason"},
	{false, false, true, true, ExecVerdict::Honor, "both flags set: the old behaviour (discexec wins)"},
	{false, true, true, true, ExecVerdict::Honor, "both flags set while a game starts: the old behaviour (discexec wins)"},
	// a disc tile: never, with any flag (a cross-title exec of PPSA99203's eboot ends at the home screen)
	{true, false, false, false, ExecVerdict::IgnoreTile, "tile, nothing else"},
	{true, true, false, false, ExecVerdict::IgnoreTile, "tile, a game starting"},
	{true, false, true, false, ExecVerdict::IgnoreTile, "tile with discexec: still never"},
	{true, true, true, false, ExecVerdict::IgnoreTile, "tile with discexec and a game starting: still never"},
	{true, false, false, true, ExecVerdict::IgnoreTile, "tile with nodiscexec: the tile is the reason"},
	{true, true, false, true, ExecVerdict::IgnoreTile, "tile, game, nodiscexec"},
	{true, false, true, true, ExecVerdict::IgnoreTile, "tile, both flags"},
	{true, true, true, true, ExecVerdict::IgnoreTile, "tile, everything"},
};

static std::string Join(const std::string& dir, const char* name) { return dir + "/" + name; }

static bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

static void Touch(const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "w");
	if (f)
	{
		std::fputs("x\n", f);
		std::fclose(f);
	}
}

int main()
{
	// the two flow-defining cases at compile time (the end of a copy; the idle shelf). The mutant run switches these off, so that the table below has to
	// catch every wrong version by itself.
#ifndef DISCEXEC_NO_STATIC_ASSERT
	static_assert(DecideExec(ExecIn{false, false, false, false}) == ExecVerdict::Honor, "an idle shelf honours the signal");
	static_assert(DecideExec(ExecIn{false, true, false, false}) == ExecVerdict::IgnoreGameStarting, "a game that starts is not exec'd over");
	static_assert(orbis_disc::StaleLaunchFile(1000, 100), "constexpr");
#endif

	// ---- the table
	for (const Row& r : kRows)
	{
		const ExecIn in = {r.tile, r.starting, r.discexec, r.nodiscexec};
		const ExecVerdict got = DecideExec(in);
		char label[300];
		std::snprintf(label, sizeof(label), "tile=%d starting=%d discexec=%d nodiscexec=%d -> %s (wanted %s): %s", r.tile, r.starting, r.discexec, r.nodiscexec,
			Name(got), Name(r.want), r.why);
		Check(got == r.want, label);
	}
	Check(sizeof(kRows) / sizeof(kRows[0]) == 16, "the table has all 16 combinations");

	// ---- the invariants, on their own (a different way to say the same thing, so one wrong row cannot hide)
	for (int bits = 0; bits < 16; bits++)
	{
		const ExecIn in = {(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0};
		const ExecVerdict got = DecideExec(in);
		char label[120];
		std::snprintf(label, sizeof(label), "tile=%d starting=%d discexec=%d nodiscexec=%d", in.is_tile, in.game_starting, in.flag_discexec, in.flag_nodiscexec);
		if (in.is_tile)
			Check(got == ExecVerdict::IgnoreTile, std::string("a tile never execs: ") + label);
		if (got == ExecVerdict::Honor)
			Check(!in.is_tile, std::string("Honor never in a tile: ") + label);
		if (got == ExecVerdict::Honor && in.game_starting)
			Check(in.flag_discexec, std::string("a starting game is exec'd over only with discexec: ") + label);
		if (!in.is_tile && !in.game_starting && !in.flag_nodiscexec)
			Check(got == ExecVerdict::Honor, std::string("nothing in the way: honoured: ") + label);
		if (!in.is_tile && in.flag_discexec)
			Check(got == ExecVerdict::Honor, std::string("discexec outside a tile is the old behaviour: ") + label);
	}

	// ---- the texts: four different, non-empty lines (the log is read for them)
	{
		std::set<std::string> seen;
		for (ExecVerdict v : {ExecVerdict::Honor, ExecVerdict::IgnoreTile, ExecVerdict::IgnoreGameStarting, ExecVerdict::IgnoreFlag})
		{
			const char* t = orbis_disc::ExecVerdictText(v);
			Check(t && *t, std::string("a text for ") + Name(v));
			seen.insert(t ? t : "");
		}
		Check(seen.size() == 4, "four different texts");
		Check(std::strstr(orbis_disc::ExecVerdictText(ExecVerdict::Honor), "honoured") != nullptr, "the Honor text says honoured");
		Check(std::strstr(orbis_disc::ExecVerdictText(ExecVerdict::IgnoreGameStarting), "ignored") != nullptr, "the Ignore texts say ignored");
		Check(std::strstr(orbis_disc::ExecVerdictText(ExecVerdict::IgnoreTile), "tile") != nullptr, "the tile text names the tile");
		Check(std::strstr(orbis_disc::ExecVerdictText(ExecVerdict::IgnoreFlag), "nodiscexec") != nullptr, "the flag text names the flag");
	}

	// ---- StaleLaunchFile: older than 600 s is stale, 600 is not; no time (0) or a time in the future is trusted as fresh
	{
		const long long now = 1800000000LL;
		Check(!orbis_disc::StaleLaunchFile(now, now), "age 0: fresh");
		Check(!orbis_disc::StaleLaunchFile(now, now - 1), "age 1: fresh");
		Check(!orbis_disc::StaleLaunchFile(now, now - 599), "age 599: fresh");
		Check(!orbis_disc::StaleLaunchFile(now, now - 600), "age 600: fresh (the limit itself is still allowed)");
		Check(orbis_disc::StaleLaunchFile(now, now - 601), "age 601: stale");
		Check(orbis_disc::StaleLaunchFile(now, now - 86400), "a day old: stale");
		Check(!orbis_disc::StaleLaunchFile(now, 0), "mtime 0 (no time known): fresh");
		Check(!orbis_disc::StaleLaunchFile(now, now + 5), "mtime in the future: fresh");
		Check(!orbis_disc::StaleLaunchFile(now, now + 100000), "mtime far in the future: fresh");
		Check(!orbis_disc::StaleLaunchFile(0, 0), "both 0: fresh");
		Check(orbis_disc::StaleLaunchFile(1000, 900, 50), "a different limit: age 100 > 50: stale");
		Check(!orbis_disc::StaleLaunchFile(1000, 950, 50), "a different limit: age 50 is the limit: fresh");
	}

	// ---- a real file, aged the way main-boot reads it (fstat st_mtime)
	{
		char tmpl[] = "/tmp/discexec-XXXXXX";
		char* made = mkdtemp(tmpl);
		Check(made != nullptr, "mkdtemp");
		if (made)
		{
			const std::string dir = made;
			const std::string f = Join(dir, "disc-launch.txt");
			Touch(f);
			struct stat st = {};
			Check(stat(f.c_str(), &st) == 0, "stat of the fresh file");
			const long long now = static_cast<long long>(st.st_mtime);
			Check(!orbis_disc::StaleLaunchFile(now + 3, static_cast<long long>(st.st_mtime)), "a file just written: fresh");
			struct timespec ts[2];
			ts[0].tv_sec = ts[1].tv_sec = static_cast<time_t>(now - 700);
			ts[0].tv_nsec = ts[1].tv_nsec = 0;
			Check(utimensat(AT_FDCWD, f.c_str(), ts, 0) == 0, "set the file's time back by 700 s");
			Check(stat(f.c_str(), &st) == 0, "stat of the aged file");
			Check(orbis_disc::StaleLaunchFile(now, static_cast<long long>(st.st_mtime)), "the file aged 700 s: stale");
			ts[0].tv_sec = ts[1].tv_sec = static_cast<time_t>(now - 100);
			Check(utimensat(AT_FDCWD, f.c_str(), ts, 0) == 0, "set the file's time back by 100 s");
			Check(stat(f.c_str(), &st) == 0, "stat of the 100 s file");
			Check(!orbis_disc::StaleLaunchFile(now, static_cast<long long>(st.st_mtime)), "the file aged 100 s: fresh");
			unlink(f.c_str());
			rmdir(dir.c_str());
		}
	}

	// ---- RemoveSignalFiles: both signal files go, nothing else does, a missing file is no problem
	{
		char tmpl[] = "/tmp/discexec-XXXXXX";
		char* made = mkdtemp(tmpl);
		Check(made != nullptr, "mkdtemp (2)");
		if (made)
		{
			const std::string dir = made;
			const std::string launch = Join(dir, "disc-launch.txt"), exec = Join(dir, "disc-launch-exec.txt"), other = Join(dir, "disc-autostart.txt"),
			                  other2 = Join(dir, "disc-launch.txt.keep");
			Touch(launch);
			Touch(exec);
			Touch(other);
			Touch(other2);
			orbis_disc::RemoveSignalFiles(launch, exec);
			Check(!Exists(launch), "disc-launch.txt is gone (an ignored signal takes the launch note with it)");
			Check(!Exists(exec), "disc-launch-exec.txt is gone");
			Check(Exists(other), "disc-autostart.txt is untouched");
			Check(Exists(other2), "a file with a similar name is untouched");
			orbis_disc::RemoveSignalFiles(launch, exec); // both missing: no crash, nothing else happens
			Check(Exists(other) && Exists(other2), "a second call with the files missing touches nothing else");
			Touch(exec); // only the exec file there
			orbis_disc::RemoveSignalFiles(launch, exec);
			Check(!Exists(exec), "only the exec file there: it goes");
			Touch(launch); // only the launch note there
			orbis_disc::RemoveSignalFiles(launch, exec);
			Check(!Exists(launch), "only the launch note there: it goes");
			orbis_disc::RemoveSignalFiles(Join(dir, "no-such-dir/x"), Join(dir, "no-such-dir/y")); // a path that cannot exist
			Check(Exists(other), "paths that cannot exist: no crash");
			unlink(other.c_str());
			unlink(other2.c_str());
			rmdir(dir.c_str());
		}
	}

	std::printf("discexec test: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
	return fails ? 1 : 0;
}
