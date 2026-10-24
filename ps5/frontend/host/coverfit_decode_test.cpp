// PS5 port frontend: the cover fit in the cover service (2.11, AI-assisted; swordpdf: "the covers need to be proportionate to the case"):
// CoverService::Decode with a CoverFit and DecodeWithAlpha on generated pictures, then the service end to end (a cover, a back and a spine from the
// covers folder and a cover from a download) with the fit on and off (CoverConfig::fit_covers, the flag file flags/nocoverfit).
//
//   coverfit_decode_test <fixture dir> <work dir>      (test-coverfit.sh makes the pictures with make_fit_fixtures.py; FE_RES: bin/resources)
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_covers.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace fe;

static int g_failures = 0;
#define CHECK(cond)                                                       \
	do                                                                    \
	{                                                                     \
		if (!(cond))                                                      \
		{                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			g_failures++;                                                 \
		}                                                                 \
	} while (0)

static std::vector<uint8_t> Read(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	std::vector<uint8_t> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (v.empty())
		std::printf("missing fixture %s\n", path.c_str());
	return v;
}

static const uint8_t* Px(const CoverImage& c, int x, int y)
{
	return &c.rgba[(static_cast<size_t>(y) * c.width + x) * 4];
}

// The bounding box of the pixels that are yellow (the fixtures' circle), or false.
static bool Yellow(const CoverImage& c, int& w, int& h)
{
	int x0 = c.width, y0 = c.height, x1 = -1, y1 = -1;
	for (int y = 0; y < c.height; y++)
		for (int x = 0; x < c.width; x++)
		{
			const uint8_t* p = Px(c, x, y);
			if (p[0] > 200 && p[1] > 200 && p[2] < 90)
			{
				x0 = std::min(x0, x);
				x1 = std::max(x1, x);
				y0 = std::min(y0, y);
				y1 = std::max(y1, y);
			}
		}
	if (x1 < x0)
		return false;
	w = x1 - x0 + 1;
	h = y1 - y0 + 1;
	return true;
}

static bool IsBar(const CoverImage& c, int x, int y)
{
	const uint8_t* p = Px(c, x, y);
	return p[0] == 12 && p[1] == 12 && p[2] == 15 && p[3] == 255;
}

static void Face(const std::string& dir, const char* file, int max_h, int want_w, int want_h, bool want_bars_lr, bool want_bars_tb, bool want_round)
{
	CoverImage c;
	const std::vector<uint8_t> bytes = Read(dir + "/" + file);
	const bool ok = CoverService::Decode(bytes, max_h, c, 4096, CoverFit::Face);
	std::printf("%-18s -> %dx%d\n", file, c.width, c.height);
	CHECK(ok);
	CHECK(std::abs(c.width - want_w) <= 1 && std::abs(c.height - want_h) <= 1);
	CHECK(c.rgba.size() == static_cast<size_t>(c.width) * c.height * 4);
	const bool lr = IsBar(c, 1, c.height / 2) && IsBar(c, c.width - 2, c.height / 2);
	const bool tb = IsBar(c, c.width / 2, 1) && IsBar(c, c.width / 2, c.height - 2);
	CHECK(lr == want_bars_lr);
	CHECK(tb == want_bars_tb);
	int w = 0, h = 0;
	if (want_round)
	{
		CHECK(Yellow(c, w, h));
		std::printf("    circle %dx%d\n", w, h);
		CHECK(std::abs(w - h) <= 1 + std::max(w, h) / 60); // round to within about a percent
	}
	// the picture is never stretched: for a picture that is made the face's shape the texture is 135:190
	if (want_w > 0 && std::abs(static_cast<double>(c.width) / c.height - kFaceAspectFront) > 0.01)
		CHECK(!want_bars_lr && !want_bars_tb && std::string(file).find("ok") != std::string::npos); // only a nearly-right one may be off the face's shape
}

static void Spine(const std::string& dir, const char* file, int want_w, int want_h)
{
	CoverImage c;
	const bool ok = CoverService::Decode(Read(dir + "/" + file), 1304, c, 4096, CoverFit::Spine);
	std::printf("%-18s -> %dx%d\n", file, c.width, c.height);
	CHECK(ok);
	CHECK(std::abs(c.width - want_w) <= 1 && std::abs(c.height - want_h) <= 1);
}

static bool AlphaBox(const CoverImage& c, int& w, int& h)
{
	int x0 = c.width, y0 = c.height, x1 = -1, y1 = -1;
	for (int y = 0; y < c.height; y++)
		for (int x = 0; x < c.width; x++)
			if (Px(c, x, y)[3] >= 128)
			{
				x0 = std::min(x0, x);
				x1 = std::max(x1, x);
				y0 = std::min(y0, y);
				y1 = std::max(y1, y);
			}
	if (x1 < x0)
		return false;
	w = x1 - x0 + 1;
	h = y1 - y0 + 1;
	return true;
}

static void Disc(const std::string& dir, const char* file, int want_bw, int want_bh)
{
	CoverImage c;
	const bool ok = CoverService::DecodeWithAlpha(Read(dir + "/" + file), 512, c, 2600);
	int bw = 0, bh = 0;
	CHECK(ok && AlphaBox(c, bw, bh));
	std::printf("%-18s -> %dx%d, opaque part %dx%d\n", file, c.width, c.height, bw, bh);
	CHECK(c.width == c.height); // always a square picture: the shelf's circle is the disc
	CHECK(std::abs(bw - want_bw) <= 3 && std::abs(bh - want_bh) <= 3);
}

static GameInfo MakeGame(const std::string& serial, const std::string& title)
{
	GameInfo g;
	g.path = "/games/" + serial + ".iso";
	g.stem = serial;
	g.title = title;
	g.serial = serial;
	g.region = "USA";
	return g;
}

static void CopyFile(const std::string& from, const std::string& to)
{
	const std::vector<uint8_t> bytes = Read(from);
	std::ofstream(to, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// The service end to end. Game 0 has a cover, a back and a spine in the covers folder (a square cover, a square back and a 300x1000 spine: none of them
// the shape of its face); game 1 has nothing and a download answers with a 16:9 picture. With the fit every picture comes out the shape of its face
// (the picture inside on bars); without it (the flag file nocoverfit) each is as it was before 2.11: whole, as decoded.
static void Service(const std::string& fix, const std::string& work, const Fonts& fonts, bool fit)
{
	std::printf("-- service, fit %s\n", fit ? "on" : "off");
	const std::string covers = work + (fit ? "/fit-on/covers" : "/fit-off/covers"), cache = work + (fit ? "/fit-on/cache" : "/fit-off/cache");
	CHECK(system(("mkdir -p '" + covers + "' '" + cache + "'").c_str()) == 0);
	CopyFile(fix + "/front_square.png", covers + "/SLUS-00001.png");
	CopyFile(fix + "/front_square.png", covers + "/SLUS-00001_back.png");
	CopyFile(fix + "/spine_wide.png", covers + "/SLUS-00001_spine.png");
	const std::vector<uint8_t> wide = Read(fix + "/front_wide.png");

	const std::vector<GameInfo> games = {MakeGame("SLUS-00001", "One"), MakeGame("SLUS-00002", "Two")};
	CoverConfig cfg;
	cfg.manual_dir = covers;
	cfg.cache_dir = cache;
	cfg.url_template = "https://example.test/covers/${serial}.jpg";
	cfg.allow_download = true;
	cfg.fit_covers = fit;
	CoverService cs;
	cs.Start(games, &fonts, cfg, [&wide](const std::string& url, std::vector<uint8_t>& out) {
		out.clear();
		if (url.find("SLUS-00002") == std::string::npos)
			return 404;
		out = wide;
		return 200;
	});
	cs.Want(0);
	cs.Want(1);
	cs.WantBack(0);

	std::map<std::string, CoverImage> got; // "cover0", "cover1", "back0", "spine0": the real pictures (not the painted ones)
	const auto t0 = std::chrono::steady_clock::now();
	while (got.size() < 4 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 15.0)
	{
		CoverImage img;
		while (cs.Poll(img))
		{
			const std::string src = img.source;
			if (img.kind == CoverImage::Cover)
				got[img.game == 0 ? "cover0" : "cover1"] = img;
			else if (img.kind == CoverImage::RealBack)
				got["back0"] = img;
			else if (img.kind == CoverImage::Spine && src == "manual")
				got["spine0"] = img;
		}
		usleep(20000);
	}
	CHECK(cs.Stop(5000));
	CHECK(got.size() == 4);
	for (const auto& kv : got)
		std::printf("%-8s -> %dx%d\n", kv.first.c_str(), kv.second.width, kv.second.height);
	if (got.size() != 4)
		return;
	const auto aspect = [&](const char* key) { return static_cast<double>(got[key].width) / got[key].height; };
	if (fit)
	{
		CHECK(std::abs(aspect("cover0") - kFaceAspectFront) < 0.01);
		CHECK(std::abs(aspect("cover1") - kFaceAspectFront) < 0.01);
		CHECK(std::abs(aspect("back0") - kFaceAspectFront) < 0.01);
		CHECK(std::abs(aspect("spine0") - kFaceAspectSpine) < 0.005);
		CHECK(IsBar(got["cover0"], got["cover0"].width / 2, 1)); // the square cover is letter-boxed in the face's shape (bars above and below) ...
		CHECK(got["cover0"].height == 1024);                     // ... as large as the texture may be
		CHECK(got["spine0"].height == 1304);
	}
	else
	{
		CHECK(got["cover0"].width == 1000 && got["cover0"].height == 1000);
		CHECK(got["cover1"].width == 1600 && got["cover1"].height == 900);
		CHECK(got["back0"].width == 768 && got["back0"].height == 768);
		CHECK(got["spine0"].width == 300 && got["spine0"].height == 1000);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: coverfit_decode_test <fixture dir> <work dir>\n");
		return 2;
	}
	const std::string d = argv[1], work = argv[2];
	//   file                 max_h  want_w want_h  bars lr   bars tb   circle round
	Face(d, "front_ok.jpg", 1024, 712, 1000, false, false, false);      // 0.712: left as it is
	Face(d, "front_big_ok.jpg", 1024, 731, 1024, false, false, false);  // 2000x2800: only scaled down
	Face(d, "front_tall.png", 1024, 711, 1000, true, false, true);      // 500x1000: pillarbox, no resample
	Face(d, "front_square.png", 1024, 728, 1024, false, true, true);    // 1000x1000: letterbox
	Face(d, "front_wide.png", 1024, 728, 1024, false, true, true);      // 1600x900
	Face(d, "front_banner.png", 1024, 728, 1024, false, true, true);    // 4096x1024: bars, a thin strip
	Face(d, "front_a064.png", 1024, 711, 1000, true, false, true);      // 640x1000: 0.90 of the face's width: padded (crop is for <= 10 %)
	Face(d, "front_crop.png", 1024, 711, 1000, false, false, true);     // 760x1000: 7 % wide: the middle of it fills the face
	Face(d, "back_big.jpg", 768, 549, 768, false, false, false);        // a back: 1500x2100 at 768 rows
	Spine(d, "spine_ok.png", 106, 1304);                               // 175x2151: stretched a little (10 %): as it was
	Spine(d, "spine_wide.png", 96, 1304);                              // 300x1000: padded
	Disc(d, "disc_round.png", 512, 512);                               // 700x700 round
	Disc(d, "disc_ellipse.png", 512, 512);                             // 900x700, touches all sides: stretched round
	Disc(d, "disc_margin.png", 512, 512);                              // 1000x1000 with a 15 % margin: cut to the disc
	Disc(d, "disc_pair.png", 512, 256);                                // 1400x700 (not one disc): proportions kept
	Disc(d, "disc_rect.png", 512, 398);                                // 900x700, opaque all over (no surround): not an ellipse, proportions kept
	// no fit (a badge): as it was
	{
		CoverImage c;
		const std::vector<uint8_t> bytes = Read(d + "/front_square.png");
		CHECK(CoverService::Decode(bytes, 64, c, 512, CoverFit::None) == false); // 1000 > 512 limit: refused as before
		CHECK(CoverService::Decode(bytes, 64, c, 0) && c.width == 64 && c.height == 64);
	}

	// The service itself, with the fit on and off.
	std::vector<uint8_t> text_font, icon_font;
	const std::string res = getenv("FE_RES") ? getenv("FE_RES") : "";
	text_font = Read(res + "/fonts/Roboto-Regular.ttf");
	icon_font = Read(res + "/fonts/promptfont.otf");
	if (text_font.empty() || icon_font.empty())
	{
		std::printf("FAIL: fonts not found under FE_RES=%s\n", res.c_str());
		return 2;
	}
	Fonts fonts;
	CHECK(fonts.Init(text_font.data(), text_font.size(), icon_font.data(), icon_font.size()));
	Service(d, work, fonts, true);
	Service(d, work, fonts, false);

	std::printf("%s\n", g_failures ? "FAILED" : "all passed");
	return g_failures ? 1 : 0;
}
