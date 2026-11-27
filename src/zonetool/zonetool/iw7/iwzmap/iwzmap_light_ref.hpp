#pragma once

// Reference culling sphere of the generated lights (iwzmap.cpp build_gfx_lights), header only so that
// tests/iwzmap_light_ref_tests.cpp checks the choice without game assets.
//
// Every generated light gets its own culling mesh: the 32 vertex sphere of one template omni slot, moved and scaled onto
// the light. The reference is chosen among the template slots as the template holds them (before the generated lights
// overwrite theirs). An unused slot is preferred (the choice of builds before 27 Nov 2026), a slot used by a generated
// light is the fallback: choosing among unused slots only lost the reference when the lights took every 32 vertex omni
// slot (cp_zmb, 178 or more named lights: the scriptable range grows downwards over the static slots, first = 1039 - named),
// and each light kept its own template shape moved onto it (clipped to the template's rooms, a cone on a spot slot):
// black stair-stepped patches of 8x8 pixel light tiles in the lit frame (Studio fault FE66).

#include <cstddef>
#include <vector>

namespace zonetool::iw7::iwzmap::light_ref
{
	constexpr unsigned none = ~0u;

	struct candidate
	{
		bool omni = false;               // template light type is omni
		float radius = 0.0f;             // template light radius
		unsigned vertex_count = 0;       // frustumLights[k].vertexCount
		unsigned index_count = 0;        // frustumLights[k].indexCount
		bool has_data = false;           // vertices and indices present
		unsigned plane_count = 0;        // lightViewFrustums[k].planeCount (0 when the array is absent)
		bool used = false;               // a generated light takes this slot
	};

	// the lowest unused slot that is a plain 32 vertex omni sphere, else the lowest used one, else none
	inline unsigned pick(const std::vector<candidate>& slots)
	{
		for (auto pass = 0; pass < 2; pass++)
			for (std::size_t k = 0; k < slots.size(); k++)
			{
				const auto& c = slots[k];
				if (c.used != (pass == 1) || !c.omni || !(c.radius > 0.0f)) continue;
				if (c.vertex_count != 32 || !c.has_data || !c.index_count || c.plane_count) continue;
				return static_cast<unsigned>(k);
			}
		return none;
	}
}
