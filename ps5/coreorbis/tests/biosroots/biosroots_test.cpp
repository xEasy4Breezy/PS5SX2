// Host test for orbis-shims/OrbisBiosRoots.h (2026-10-16, AI-assisted): the folders PCSX2's BIOS lookup looks in after the BIOS
// folder's own files, with a fake /data/PCSX2 and /mnt in a temp folder. Run (test-biosroots.sh does it):
//   g++ -std=c++17 -I ps5/coreorbis/orbis-shims biosroots_test.cpp && ./a.out
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisBiosRoots.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int fails = 0;
static void Check(bool ok, const std::string& w)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", w.c_str());
	fails += !ok;
}
static std::string Join(const std::vector<std::string>& v, const std::string& strip)
{
	std::string s;
	for (const std::string& d : v)
		s += (s.empty() ? "" : " ") + d.substr(strip.size());
	return s;
}

int main(int argc, char** argv)
{
	const std::string root = argc > 1 ? argv[1] : "/tmp/biosroots-test";
	std::system(("rm -rf '" + root + "' && mkdir -p '" + root + "/data/PCSX2/bios' '" + root + "/data/PCSX2/BIOS2' '" + root +
		"/mnt/usb0/bios' '" + root + "/mnt/usb0/PCSX2/bios' '" + root + "/mnt/ext1/Stuff' '" + root + "/custom/pack/inner'").c_str());
	const std::string top = root + "/data/PCSX2", mnt = root + "/mnt";
	// default folder: the folders in it, then the top folder, then the drives
	const auto def = OrbisBios::SearchDirs(top + "/bios", false, true, top, mnt);
	Check(Join(def, root) == "/data/PCSX2 /mnt/usb0 /mnt/usb0/PCSX2 /mnt/usb0/PCSX2/bios /mnt/usb0/bios /mnt/ext1",
		"unset: /data/PCSX2, the drives, their bios/ and PCSX2/bios");
	// the default call with the switches left out is the same (ScanUsb on by default)
	Check(OrbisBios::SearchDirs(top + "/bios", false, true, top, mnt) == def, "unset, USB scan on: the same list");
	// Scan USB drives off: the same without any drive
	const auto nousb = OrbisBios::SearchDirs(top + "/bios", false, false, top, mnt);
	Check(Join(nousb, root) == "/data/PCSX2", "no USB scan: /data/PCSX2 only, no drive");
	// the player's folder: only the folders in it, whatever the USB switch says
	const auto mine = OrbisBios::SearchDirs(root + "/custom", true, true, top, mnt);
	Check(Join(mine, root) == "/custom/pack /custom/pack/inner", "set: only the folders inside it");
	Check(OrbisBios::SearchDirs(root + "/custom", true, false, top, mnt) == mine, "set, no USB scan: the same");
	// a folder that is not there: nothing at all
	Check(OrbisBios::SearchDirs(root + "/gone", true, true, top, mnt).empty(), "set but missing: nothing");
	// a BIOS folder on a drive that is set: its folders, not the drive's others
	const auto on_drive = OrbisBios::SearchDirs(mnt + "/usb0/PCSX2", true, true, top, mnt);
	Check(Join(on_drive, root) == "/mnt/usb0/PCSX2/bios", "set on a drive: the folders inside it only");
	std::system(("rm -rf '" + root + "'").c_str());
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
