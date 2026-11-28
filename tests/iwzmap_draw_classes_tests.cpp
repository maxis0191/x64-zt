#include "../src/zonetool/zonetool/iw7/iwzmap/iwzmap_draw_classes.hpp"
#include <cstdio>
#include <stdexcept>

namespace dc = zonetool::iw7::iwzmap::draw_classes;

static void require(bool condition, const char* message)
{
	if (!condition) throw std::runtime_error(message);
}

static bool throws(const char* text)
{
	try { dc::parse(text); }
	catch (const std::runtime_error&) { return true; }
	return false;
}

// the bands of cp_zmb's world header (read from cp_zmb.d3dbsp.gfxmap, R2-PARITY section 14)
static dc::bands cp_zmb()
{
	dc::bands b;
	b.begin[dc::opaque] = 1; b.end[dc::opaque] = 6;
	b.begin[dc::decal] = 7; b.end[dc::decal] = 17;
	b.begin[dc::trans] = 18; b.end[dc::trans] = 34;
	b.begin[dc::emissive] = 35; b.end[dc::emissive] = 40;
	return b;
}

int main()
{
	try
	{
		const auto b = cp_zmb();
		// sort keys of the stock materials of the control room: plain 2 and 3, graffiti 10 and 15, wall stains 13, glass 25 and 30, the emissive band
		require(dc::classify(2, b) == dc::opaque && dc::classify(3, b) == dc::opaque && dc::classify(6, b) == dc::opaque, "opaque band");
		require(dc::classify(7, b) == dc::decal && dc::classify(10, b) == dc::decal && dc::classify(15, b) == dc::decal && dc::classify(17, b) == dc::decal, "decal band, ends included");
		require(dc::classify(18, b) == dc::trans && dc::classify(25, b) == dc::trans && dc::classify(30, b) == dc::trans && dc::classify(34, b) == dc::trans, "trans band, ends included");
		require(dc::classify(35, b) == dc::emissive && dc::classify(40, b) == dc::emissive, "emissive band");
		require(dc::classify(0, b) == dc::opaque && dc::classify(41, b) == dc::opaque && dc::classify(255, b) == dc::opaque, "outside every band is opaque");
		dc::bands unset;
		require(dc::classify(0, unset) == dc::opaque && dc::classify(10, unset) == dc::opaque, "a template without bands classifies nothing");

		// the file
		const auto m = dc::parse("# comment\nw/cp_disco_graffiti_01\t15\r\nwc/glass_cracked_01\t25 # glass\n\nw/glass_clear_solid_01\t30\n");
		require(m.size() == 3 && m.at("w/cp_disco_graffiti_01") == 15 && m.at("wc/glass_cracked_01") == 25 && m.at("w/glass_clear_solid_01") == 30, "parse");
		require(dc::parse("").empty() && dc::parse("# only a comment\n\n").empty(), "an empty file lists nothing");
		require(throws("w/a 15\n"), "no tab");
		require(throws("w/a\t\n"), "no key");
		require(throws("\t15\n"), "no name");
		require(throws("w/a\t1x\n"), "not a number");
		require(throws("w/a\t256\n"), "over 255");
		require(throws("w/a\t1000\n"), "four digits");
		require(throws("w/a\t7\nw/a\t8\n"), "two keys for one material");
		require(dc::parse("w/a\t7\nw/a\t7\n").size() == 1, "the same key twice is one entry");

		// the partition: stable, and the identity when every surface is opaque (a world without decals keeps its order)
		const std::vector<int> all_opaque(50, dc::opaque);
		const auto id = dc::partition(all_opaque);
		require(id.identity && id.order.size() == 50 && id.count[dc::opaque] == 50 && !id.count[dc::decal] && !id.count[dc::trans] && !id.count[dc::emissive], "identity");
		for (std::size_t i = 0; i < id.order.size(); i++) require(id.order[i] == i, "identity order");
		require(dc::partition({}).identity && dc::partition({}).order.empty(), "no surfaces");

		const std::vector<int> mixed = {dc::trans, dc::opaque, dc::decal, dc::opaque, dc::emissive, dc::decal, dc::trans, dc::opaque};
		const auto p = dc::partition(mixed);
		const std::vector<std::size_t> want = {1, 3, 7, 2, 5, 0, 6, 4};
		require(!p.identity && p.order == want, "stable by class");
		require(p.count[dc::opaque] == 3 && p.count[dc::decal] == 2 && p.count[dc::trans] == 2 && p.count[dc::emissive] == 1, "counts");
		require(dc::partition(std::vector<int>{dc::decal, dc::trans}).identity, "already grouped stays the identity");
		auto bad = false;
		try { dc::partition({0, 7}); } catch (const std::runtime_error&) { bad = true; }
		require(bad, "a class out of range is refused");

		// the ranges: without classes exactly what build_gfx wrote before (opaque 0..count, the others empty at count)
		const unsigned int plain[dc::kind_count] = {50, 0, 0, 0};
		const auto r0 = dc::make_ranges(0, plain);
		require(r0.begin[0] == 0 && r0.end[0] == 50 && r0.begin[1] == 50 && r0.end[1] == 50 && r0.begin[2] == 50 && r0.end[2] == 50 && r0.begin[3] == 50 && r0.end[3] == 50, "ranges of a plain world");
		const auto r1 = dc::make_ranges(10, plain);                  // 10 kept template surfaces and 50 own
		require(r1.end[0] == 60 && r1.begin[1] == 60 && r1.end[3] == 60, "ranges with leading template surfaces");
		const unsigned int cls[dc::kind_count] = {3, 2, 2, 1};
		const auto r2 = dc::make_ranges(0, cls);
		require(r2.begin[0] == 0 && r2.end[0] == 3 && r2.begin[1] == 3 && r2.end[1] == 5 && r2.begin[2] == 5 && r2.end[2] == 7 && r2.begin[3] == 7 && r2.end[3] == 8, "ranges of a classed world");
		// the ranges cover the partitioned surfaces: position i has the kind template_kind says
		for (std::size_t pos = 0; pos < p.order.size(); pos++)
			require(dc::template_kind(static_cast<unsigned int>(pos), r2.begin, r2.end) == mixed[p.order[pos]], "a surface lies in the range of its class");
		require(dc::template_kind(10, r2.begin, r2.end) == dc::opaque, "outside the ranges is opaque");

		std::puts("iwzmap_draw_classes_tests: all passed");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::printf("iwzmap_draw_classes_tests: FAILED: %s\n", e.what());
		return 1;
	}
}
