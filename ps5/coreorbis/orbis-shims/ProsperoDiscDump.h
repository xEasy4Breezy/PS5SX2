// PS5SX2 (vk-285-144, AI-assisted): a PS2 DVD in the PS5's drive, copied to /data/PCSX2/games as an .iso. See
// ProsperoDiscDump.cpp.
#pragma once

#include <string>

// Starts the thread that watches the drive (once; the flag nodiscdump keeps it off).
void OrbisDiscDumpStart();

// A copy is running now.
bool OrbisDiscDumpBusy();

// PR #34 port (AI-assisted): a game is about to play straight from drive `device` ("/dev/cd1", OrbisIOCtlSrc.cpp). From
// now on the watcher leaves that drive alone: it doesn't look at it, copy its disc or start its copy. False (and nothing
// changes) when that drive's disc is being copied right now. OrbisDiscDumpRelease gives the drive back to the watcher.
bool OrbisDiscDumpClaim(const std::string& device);

// 2.04 (AI-assisted): the copy of the PS2 disc `serial` in drive `device` that games/ holds (a whole one, with no ".incomplete" mark next to
// it), or empty. For a disc tile: the game starts from the copy when there is one, else from the drive.
std::string OrbisFindDiscCopy(const std::string& serial, const std::string& device);
void OrbisDiscDumpRelease(const std::string& device);

// 2.11 (AI-assisted): the name a copy of disc `serial` gets in games/ without its ".iso" ("Gran Turismo 4 (SLUS-21453)"). A game that plays
// from the drive keeps its settings in settings/<this>.ini, so the copy, once made, starts with the settings the disc had and the shelf's
// entry for it shows the same file.
std::string OrbisDiscSettingsStem(const std::string& serial);

// 2.11 (dumpxmb): main-boot says this process is a disc tile (a copy of the app under another title ID): the disc helper's exec signal is never honoured there.
void OrbisDiscNoteTile(bool tile);
// 2.11 (dumpxmb): a game is being started in this process (main-boot chose one, or the watcher's launch request was taken). From now on the helper's
// disc-launch-exec.txt is ignored (it would kill the game that is booting) unless the flag discexec is set.
void OrbisDiscNoteGameStarting(const char* how);
// 2.11 (dumpxmb): one boot.log line: which signal files between the app and the disc helper exist and how old they are.
void OrbisDiscLogSignalFiles(const char* when);
