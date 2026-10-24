// PS5 port frontend: the cover fit's plans (fe_coverfit.h, 2.11, AI-assisted; swordpdf: "the covers need to be proportionate to the case"). Checks PlanFit's
// invariants on a grid of sizes and prints a table of typical covers. No pictures and no GPU: test-coverfit.sh builds and runs it.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_coverfit.h"

#include <cstdio>
#include <cstdlib>

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

static const char* ModeName(FitPlan::Mode m)
{
	return m == FitPlan::Stretch ? "stretch" : m == FitPlan::Crop ? "crop" : "pad";
}

static void Row(int w, int h, int max_h, CoverFit fit, const char* what)
{
	const FitPlan p = PlanFit(w, h, max_h, fit);
	std::printf("%-22s %5dx%-5d a=%.3f  %-7s src(%d,%d %dx%d) tex %dx%d dst(%d,%d %dx%d)\n", what, w, h, static_cast<double>(w) / h, ModeName(p.mode), p.sx,
		p.sy, p.sw, p.sh, p.cw, p.ch, p.dx, p.dy, p.dw, p.dh);
}

int main()
{
	std::printf("-- typical fronts (max_h 1024)\n");
	Row(512, 736, 1024, CoverFit::Face, "xlenore default");
	Row(200, 280, 1024, CoverFit::Face, "LaunchBox 200x280");
	Row(712, 1000, 1024, CoverFit::Face, "LaunchBox 712x1000");
	Row(704, 1000, 1024, CoverFit::Face, "LaunchBox 704x1000");
	Row(2000, 2800, 1024, CoverFit::Face, "big scan 2000x2800");
	Row(600, 900, 1024, CoverFit::Face, "SteamGridDB 600x900");
	Row(640, 1000, 1024, CoverFit::Face, "0.64");
	Row(800, 1000, 1024, CoverFit::Face, "0.80 (banner side)");
	Row(1000, 1000, 1024, CoverFit::Face, "square");
	Row(500, 1000, 1024, CoverFit::Face, "0.50 tall");
	Row(1600, 900, 1024, CoverFit::Face, "16:9");
	Row(4096, 1024, 1024, CoverFit::Face, "4096x1024 banner");
	Row(100, 1000, 1024, CoverFit::Face, "thin strip");
	std::printf("-- backs (max_h 768)\n");
	Row(1500, 2100, 768, CoverFit::Face, "big back");
	Row(900, 1000, 768, CoverFit::Face, "0.9 back");
	std::printf("-- spines (max_h 1304)\n");
	Row(49, 640, 1304, CoverFit::Spine, "FFX spine");
	Row(175, 2151, 1304, CoverFit::Spine, "KH2 spine");
	Row(73, 900, 1304, CoverFit::Spine, "KH2 small");
	Row(120, 1000, 1304, CoverFit::Spine, "0.12 wide spine");
	Row(300, 1000, 1304, CoverFit::Spine, "0.30 not a spine");
	Row(40, 1000, 1304, CoverFit::Spine, "0.04 thin");
	Row(1000, 100, 1304, CoverFit::Spine, "landscape strip");
	std::printf("-- badges\n");
	Row(64, 64, 64, CoverFit::None, "badge");

	// Invariants over a grid.
	int plans = 0;
	for (CoverFit fit : {CoverFit::Face, CoverFit::Spine})
		for (int max_h : {256, 768, 1024, 1304})
			for (int h = 16; h <= 4096; h = h < 64 ? h + 7 : static_cast<int>(h * 1.19))
				for (double a = 0.02; a < 12.0; a *= 1.037)
				{
					const int w = std::max(16, static_cast<int>(h * a));
					if (w > 4096)
						continue;
					const FitPlan p = PlanFit(w, h, max_h, fit);
					const double face = fit == CoverFit::Spine ? kFaceAspectSpine : kFaceAspectFront;
					plans++;
					// everything inside its bounds
					CHECK(p.cw >= 1 && p.ch >= 1 && p.ch <= std::max(max_h, h <= max_h ? h : max_h));
					CHECK(p.ch <= max_h || p.ch <= h);
					CHECK(p.sx >= 0 && p.sy >= 0 && p.sx + p.sw <= w && p.sy + p.sh <= h && p.sw >= 1 && p.sh >= 1);
					CHECK(p.dx >= 0 && p.dy >= 0 && p.dx + p.dw <= p.cw && p.dy + p.dh <= p.ch && p.dw >= 1 && p.dh >= 1);
					// the used part is never stretched: the source rect and the destination rect have the same shape (within the rounding of a pixel)
					const double src = static_cast<double>(p.sw) / p.sh, dst = static_cast<double>(p.dw) / p.dh;
					if (p.mode != FitPlan::Stretch)
						CHECK(std::fabs(dst - src) <= src * (1.5 / std::min(p.dw, p.dh)) + 1e-9);
					// Crop and Pad make the face's shape (within a pixel)
					if (p.mode != FitPlan::Stretch)
						CHECK(std::fabs(static_cast<double>(p.cw) / p.ch - face) <= face * (1.5 / std::min(p.cw, p.ch)) + 1e-9 || p.cw <= 2);
					// Stretch is only for a picture nearly the face's shape
					const double offs = std::max(static_cast<double>(w) / h / face, face / (static_cast<double>(w) / h));
					const double lim_stretch = fit == CoverFit::Spine ? kStretchSpine : kStretchFront, lim_crop = fit == CoverFit::Spine ? kCropSpine : kCropFront;
					if (p.mode == FitPlan::Stretch)
						CHECK(offs <= lim_stretch + 1e-9);
					if (p.mode == FitPlan::Crop)
						CHECK(offs > lim_stretch - 1e-9 && offs <= lim_crop + 1e-9);
					// a cover is cropped by at most 15 % of its area
					if (p.mode == FitPlan::Crop && fit == CoverFit::Face)
						CHECK(static_cast<double>(p.sw) * p.sh >= 0.85 * w * h);
					// Pad of a picture that already fits the face's shape never happens
					if (p.mode == FitPlan::Pad)
						CHECK(offs > lim_crop - 1e-9);
				}
	std::printf("%d plans checked, %d failures\n", plans, g_failures);
	return g_failures ? 1 : 0;
}
