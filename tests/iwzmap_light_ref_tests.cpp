#include "../src/zonetool/zonetool/iw7/iwzmap/iwzmap_light_ref.hpp"
#include <cstdio>
#include <stdexcept>

namespace lr = zonetool::iw7::iwzmap::light_ref;

static void require(bool condition, const char* message)
{
	if (!condition) throw std::runtime_error(message);
}

static lr::candidate sphere(bool used)
{
	lr::candidate c;
	c.omni = true; c.radius = 100.0f; c.vertex_count = 32; c.index_count = 180; c.has_data = true; c.used = used;
	return c;
}

int main()
{
	try
	{
		// cp_zmb shaped: a few odd slots, 32 vertex spheres at 4..9, the generated lights took slots 6..9
		std::vector<lr::candidate> s(10);
		s[2] = sphere(false); s[2].vertex_count = 44;                   // another shape
		s[3] = sphere(false); s[3].plane_count = 6;                     // a spot style slot with a view frustum
		s[4] = sphere(false); s[4].omni = false;                        // a spot
		s[5] = sphere(false); s[5].radius = 0.0f;                       // no radius
		for (auto k = 6; k < 10; k++) s[k] = sphere(true);
		require(lr::pick(s) == 6, "all spheres used: the lowest used slot is the reference");
		// an unused sphere wins over a lower used one
		s[8].used = false;
		require(lr::pick(s) == 8, "an unused sphere is preferred over a used one");
		s[1] = sphere(false);
		require(lr::pick(s) == 1, "the lowest unused sphere");
		// 178 lights on cp_zmb: every sphere used, the old search (unused only) found none
		for (auto& c : s) c.used = true;
		require(lr::pick(s) == 1, "every sphere used: still a reference");
		// no sphere at all
		std::vector<lr::candidate> none(5);
		require(lr::pick(none) == lr::none, "no sphere: none (the compiler refuses)");
		std::vector<lr::candidate> empty;
		require(lr::pick(empty) == lr::none, "no slots: none");
		auto nodata = sphere(false); nodata.has_data = false;
		require(lr::pick({nodata}) == lr::none, "a slot without vertex data is no reference");
	}
	catch (const std::exception& e)
	{
		std::fputs(e.what(), stderr);
		std::fputc('\n', stderr);
		return 1;
	}
	return 0;
}
