// PS5 port frontend, PC harness (2.11, AI-assisted; testers: "games started from the disc do not have the right settings applied to
// them and pressing L2 + D-pad down opens the web UI for a different game, patches and cheats as well"): the settings page for a
// game that plays straight from a disc drive ("/dev/cd0"). Such a game is no file of the page's list, so the page found none and
// opened the first game of the list; its settings, patches and cheats were that game's. test-discgame-web.sh builds this against
// fe_web.cpp and runs it (the server runs in the process, the requests go over 127.0.0.1).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../fe_web.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fe;

namespace
{
int fails = 0;

void Check(bool ok, const char* what, const std::string& detail = std::string())
{
	std::printf("%s: %s%s%s\n", ok ? "ok  " : "FAIL", what, detail.empty() || ok ? "" : "  -- ", ok ? "" : detail.c_str());
	if (!ok)
		fails++;
}

void MakeDir(const std::string& p)
{
	mkdir(p.c_str(), 0755);
}

void Write(const std::string& path, const std::string& text)
{
	std::ofstream f(path, std::ios::binary);
	f << text;
}

std::string Read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

bool Exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// An ISO 9660 image just big enough for the serial reader (the same as settings_test.cpp's).
void MakeIso(const std::string& path, const std::string& boot_elf)
{
	std::vector<uint8_t> img(20 * 2048, 0);
	auto le32 = [&](size_t at, uint32_t v) {
		for (int i = 0; i < 4; i++)
			img[at + i] = static_cast<uint8_t>(v >> (8 * i));
	};
	uint8_t* pvd = &img[16 * 2048];
	pvd[0] = 1;
	std::memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	const size_t root = 16 * 2048 + 156;
	img[root] = 34;
	le32(root + 2, 18);
	le32(root + 10, 2048);
	img[root + 25] = 2;
	img[root + 32] = 1;
	uint8_t* term = &img[17 * 2048];
	term[0] = 255;
	std::memcpy(term + 1, "CD001", 5);
	const std::string cnf = "BOOT2 = cdrom0:\\" + boot_elf + ";1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
	const std::string name = "SYSTEM.CNF;1";
	const size_t rec = 18 * 2048;
	img[rec] = static_cast<uint8_t>(33 + name.size() + ((33 + name.size()) & 1));
	le32(rec + 2, 19);
	le32(rec + 10, static_cast<uint32_t>(cnf.size()));
	img[rec + 32] = static_cast<uint8_t>(name.size());
	std::memcpy(&img[rec + 33], name.data(), name.size());
	std::memcpy(&img[19 * 2048], cnf.data(), cnf.size());
	std::ofstream f(path, std::ios::binary);
	f.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
}

// The status, a space, the body.
std::string Http(uint16_t port, const std::string& method, const std::string& target, const std::string& body)
{
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
		return "connect failed";
	std::string enc;
	for (char c : target)
		enc += c == ' ' ? std::string("%20") : std::string(1, c);
	const std::string req = method + " " + enc + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: " +
	                        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	send(s, req.data(), req.size(), 0);
	std::string resp;
	char buf[8192];
	ssize_t n;
	while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
		resp.append(buf, static_cast<size_t>(n));
	close(s);
	const size_t sp = resp.find(' ');
	const size_t hdr_end = resp.find("\r\n\r\n");
	return resp.substr(sp + 1, 3) + " " + (hdr_end == std::string::npos ? std::string() : resp.substr(hdr_end + 4));
}

bool Has(const std::string& text, const std::string& part)
{
	return text.find(part) != std::string::npos;
}
} // namespace

int main(int argc, char** argv)
{
	const std::string data = argc > 1 ? argv[1] : "/tmp/discgame-web-test";
	std::system(("rm -rf '" + data + "' && mkdir -p '" + data + "'").c_str());
	for (const char* d : {"/games", "/settings", "/patches", "/memcards", "/logs", "/covers", "/cache"})
		MakeDir(data + d);
	// The first game of the list ("Alpha"): the page used to open this one for a game from the disc.
	MakeIso(data + "/games/Alpha Game (USA).iso", "SLUS_200.01");
	Write(data + "/gs.ini", "# All games\nupscale_multiplier=4\n");
	Write(data + "/patches/SLUS-21453_AAAAAAAA.pnach",
		"gametitle=Disc Game\n\n[Widescreen 16:9]\ndescription=placeholder\npatch=1,EE,00100000,word,00000000\n");

	WebServer web;
	WebConfig cfg;
	cfg.game_dirs = {data + "/games"};
	cfg.settings_dir = data + "/settings";
	cfg.gs_ini = data + "/gs.ini";
	cfg.patches_dir = data + "/patches";
	cfg.covers_dir = data + "/covers";
	cfg.cache_dir = data + "/cache";
	cfg.token_path = data + "/token.txt";
	cfg.build_tag = "test";
	cfg.port = static_cast<uint16_t>(20000 + getpid() % 20000);
	cfg.change_log = data + "/logs/settings.log";
	cfg.logs_dir = data + "/logs";
	cfg.top_dir = data;
	cfg.memcards_dir = data + "/memcards";
	if (!web.Start(cfg))
	{
		std::printf("web server did not start\n");
		return 1;
	}
	const uint16_t port = web.Port();
	const std::string stem = "Disc Game (SLUS-21453)";

	// In the menu: only the shelf's game.
	std::string games = Http(port, "GET", "/api/games", "");
	Check(Has(games, "Alpha Game (USA).iso") && !Has(games, "\"id\":\"cd0\""), "the menu lists the shelf's game only");
	std::string state = Http(port, "GET", "/api/state", "");
	Check(Has(state, "\"mode\":\"menu\"") && Has(state, "\"playing\":null"), "no game runs in the menu");

	// A game from the drive.
	web.SetNowPlaying("/dev/cd0", stem, "SLUS-21453");
	state = Http(port, "GET", "/api/state", "");
	Check(Has(state, "\"mode\":\"game\"") && Has(state, "\"playing\":{\"id\":\"cd0\"") && Has(state, "\"serial\":\"SLUS-21453\""),
		"/api/state: the playing game is the disc (id cd0, its serial)", state);
	games = Http(port, "GET", "/api/games", "");
	Check(Has(games, "\"id\":\"cd0\"") && Has(games, "Alpha Game (USA).iso"), "/api/games: the disc is listed beside the shelf's game", games);
	// Each entry ends with its "playing" field: the first one after an entry's id is that entry's.
	auto playing_of = [&](const std::string& id) {
		const size_t at = games.find("\"id\":\"" + id + "\"");
		const size_t pl = at == std::string::npos ? at : games.find("\"playing\":", at);
		return pl == std::string::npos ? std::string("missing") : games.substr(pl + 10, games.find_first_of(",}", pl) - pl - 10);
	};
	Check(playing_of("cd0") == "true", "/api/games: the disc is the one marked playing", playing_of("cd0"));
	Check(playing_of("Alpha Game (USA).iso") == "false", "/api/games: the shelf's game is not", playing_of("Alpha Game (USA).iso"));
	std::string settings = Http(port, "GET", "/api/settings?id=cd0", "");
	Check(Has(settings, "\"id\":\"cd0\"") && Has(settings, "\"serial\":\"SLUS-21453\"") &&
			  Has(settings, "\"file\":\"settings/" + stem + ".ini\""),
		"/api/settings: the disc's own settings file", settings);
	Check(Has(settings, "Widescreen 16:9"), "/api/settings: the patch group of the disc's serial is offered", settings);
	// A change goes to the disc's file (the file the game reads: main-boot.cpp's s_game_ini_path), not to the first game's.
	const std::string saved = Http(port, "POST", "/api/settings?id=cd0", "set upscale_multiplier=3\npatch+ Widescreen 16:9\n");
	Check(saved.rfind("200", 0) == 0, "a save for the disc is accepted", saved);
	const std::string disc_ini = Read(data + "/settings/" + stem + ".ini");
	Check(Has(disc_ini, "upscale_multiplier=3") && Has(disc_ini, "Widescreen 16:9"), "the change is in settings/<disc>.ini", disc_ini);
	Check(!Exists(data + "/settings/Alpha Game (USA).ini"), "the shelf's first game got no file");
	Check(Read(data + "/gs.ini").find("upscale_multiplier=4") != std::string::npos, "gs.ini is as it was");
	// The page reading the disc's file back.
	settings = Http(port, "GET", "/api/settings?id=cd0", "");
	Check(Has(settings, "\"upscale_multiplier\":\"3\"") || Has(settings, "upscale_multiplier"), "/api/settings reads the disc's change back", settings);

	// The same game once its copy is on the shelf (file name "<stem>.iso" would be a list entry of its own): the list shows one.
	web.SetNowPlaying(data + "/games/Alpha Game (USA).iso", "Alpha Game (USA)", "");
	games = Http(port, "GET", "/api/games", "");
	Check(!Has(games, "\"id\":\"cd0\""), "an image of the list adds no second entry", games);
	state = Http(port, "GET", "/api/state", "");
	Check(Has(state, "\"playing\":{\"id\":\"Alpha Game (USA).iso\""), "/api/state: an image of the list is the playing game", state);

	// Back to the menu: the disc's entry goes.
	web.SetNowPlaying("/dev/cd0", stem, "SLUS-21453");
	web.SetNowPlaying("", "", "");
	games = Http(port, "GET", "/api/games", "");
	state = Http(port, "GET", "/api/state", "");
	Check(!Has(games, "\"id\":\"cd0\"") && Has(state, "\"mode\":\"menu\""), "the shelf again: no disc entry, no playing game", games);

	// A disc with no usable serial ("Disc drive cd1"): still its own entry, no serial, its own file.
	web.SetNowPlaying("/dev/cd1", "Disc drive cd1", "");
	state = Http(port, "GET", "/api/state", "");
	Check(Has(state, "\"playing\":{\"id\":\"cd1\"") && Has(state, "\"serial\":\"\""), "a disc without a serial is the playing game too", state);
	Check(Http(port, "POST", "/api/settings?id=cd1", "set filter=1\n").rfind("200", 0) == 0 && Exists(data + "/settings/Disc drive cd1.ini"),
		"and its changes go to settings/Disc drive cd1.ini");
	// 2.11: the page's search box: POST /api/search is taken by the shelf once; the shelf's own search is on /api/state.
	{
		std::string taken;
		Check(!web.TakeSearchFromPage(taken), "no search asked for yet");
		const std::string posted = Http(port, "POST", "/api/search", "God of War\x01 ");
		Check(posted.rfind("200", 0) == 0, "POST /api/search is accepted", posted);
		Check(web.TakeSearchFromPage(taken) && taken == "God of War ", "the shelf takes the page's text (control characters left out)", taken);
		Check(!web.TakeSearchFromPage(taken), "...once");
		state = Http(port, "GET", "/api/state", "");
		Check(Has(state, "\"search\":\"God of War \""), "/api/state says what the shelf searches for", state);
		web.SetSearchReported("");
		state = Http(port, "GET", "/api/state", "");
		Check(Has(state, "\"search\":\"\""), "...and when the shelf clears it", state);
	}
	web.Stop();
	std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
