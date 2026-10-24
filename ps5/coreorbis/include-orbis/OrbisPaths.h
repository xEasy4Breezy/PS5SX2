// PS5 port (vk-285-33): where things live in /data/PCSX2.
//
//   games/       disc images (the game selector also still looks in the top folder)
//   bios/        the BIOS and its .mec/.nvm
//   memcards/    Mcd001.ps2, Mcd002.ps2
//   savestates/  *.p2s
//   settings/    per-game settings (<disc image name>.ini)
//   patches/     *.pnach
//   textures/    replacement textures
//   resources/   GameIndex.yaml, shaders/
//   cache/       PCSX2's and the Vulkan driver's shader caches
//   flags/       the on/off switch files (vk_async, swjit, ...)
//   logs/        boot.log, emulog.txt, stderr.log and diagnostic output
//   gs.ini, live.ini, lastgame.txt, playtime.dat, pid.txt stay in the top folder.
//
// Each folder is used when it exists; without it the top folder is, as in the old flat layout, so
// an older console setup keeps working and a new build can go on before the files move.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

// "/data/PCSX2/<sub>" when that folder exists, else "/data/PCSX2".
std::string OrbisDir(const char* sub);
// 2026-10-16 (AI-assisted; PS5SX2/GameFolders, ps5/frontend/fe_roots.h): where a copied PS2 disc goes and where a copy of it is looked
// for. No games folder set: games/ (OrbisDir("games")) for both, as before. Set: the folders games are listed from (search), and the
// first of them that is no NFS share (copy_to). main-boot.cpp tells these once the folders are looked up (OrbisSetGameDirs).
void OrbisSetGameDirs(bool only_set, const std::vector<std::string>& search, const std::string& copy_to);
// The folder a disc is copied into; "" when games folders are set and none can be written to (the caller says so).
std::string OrbisGamesDestDir();
// The folders a copy of a disc is looked for in (each flat).
std::vector<std::string> OrbisGameSearchDirs();
// A switch file: "/data/PCSX2/flags/<name>" when it's there, else "/data/PCSX2/<name>" (the old place).
std::string OrbisFlagPath(const char* name);
// Whether that switch is on (the file exists in either place). vk-285-105: from a snapshot once
// OrbisFlagsRefresh has run (main-boot's ticker, once a second), before that from the files.
bool OrbisFlag(const char* name);
// vk-285-105: the snapshot's refresh (the ticker thread), and a small file's contents from the same
// refresh (the first call for a path reads it directly and registers it). False when the file isn't there.
void OrbisFlagsRefresh();
bool OrbisCachedRead(const char* path, std::string& out);
// Where a log or diagnostic file goes: logs/<name>, or the top folder without logs/.
std::string OrbisLogPath(const char* name);
// pf.log's path for the page-fault handler (no allocation there); main-boot sets it at start.
extern "C" char g_orbis_pf_log[160];
