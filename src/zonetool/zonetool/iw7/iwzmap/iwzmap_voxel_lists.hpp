#pragma once

// Static light voxel lists of the generated world (iwzmap.cpp build_voxel_tree), header only so that
// tests/iwzmap_voxel_lists_tests.cpp checks them without game assets.
//
// The engine side (GfxVoxelTree, structs.hpp; stock cp_zmb, cp_frontend and mp_frontend read with worldkit/iw7_dump.py, 26 Nov 2026):
//   leaf node        2 bytes: a u16 ADDRESS into lightListArray
//   lightListArray   unsigned short[lightListArraySize]; a list is a header (spot count << 7 | omni count) then the spot
//                    light indices then the omni light indices; the first two words are the template's, word 2 is the
//                    empty list. cp_zmb: 3,335,495 leaves, 3,712 distinct lists, 11,932 entries, longest list 6 lights.
// A u16 address reaches 65535 words, so the whole array is kept to 65535 entries (the compiler's old limit, kept: the
// last list could run past the address range, but nothing in the engine was read to allow it).
//
// A leaf's list is the lights whose sphere touches its 32-unit box. A list may name MORE lights than that: a light listed
// where its sphere does not reach adds nothing to the shading (outside its radius the light is zero) and costs one more
// light evaluated in that leaf; the stock lists already do it (the stock baker bins a little wider: about 6 % of the
// listed refs of cp_zmb are outside the sphere). So a leaf whose set is a subset of another set may point at that
// other list, which makes its own list unnecessary. `absorb` does it for every set that is a subset of a kept set with at
// most `slack` extra lights; slack 0 absorbs nothing (the lists of before this change, byte for byte).
// The same algorithm, with the same order and tie-breaks, is studio/voxellights.py (`studio check` predicts the count).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace zonetool::iw7::iwzmap::voxel_lists
{
	using light_set = std::vector<unsigned short>; // ascending light indices (positions in lights.txt), no duplicates, not empty

	constexpr std::size_t base_entries = 3;      // template word 0, template word 1, the empty list at address 2
	constexpr std::size_t entry_limit = 0xFFFF;  // u16 address
	constexpr std::size_t max_slack = 8;
	constexpr std::size_t max_omnis = 127;       // the list header holds the omni count in 7 bits
	constexpr std::size_t max_spots = 511;       // and the spot count in the 9 bits above

	// representative of every set: itself when its list is kept, else the kept set it is a subset of.
	// Order: size descending, then the sets themselves ascending; a kept set is the smallest candidate (the first of equals).
	inline std::vector<std::size_t> absorb(const std::vector<light_set>& sets, std::size_t slack)
	{
		std::vector<std::size_t> rep(sets.size());
		std::iota(rep.begin(), rep.end(), std::size_t{0});
		if (!slack || sets.empty()) return rep;

		std::vector<std::size_t> order(sets.size());
		std::iota(order.begin(), order.end(), std::size_t{0});
		std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
		{
			if (sets[a].size() != sets[b].size()) return sets[a].size() > sets[b].size();
			return sets[a] < sets[b];
		});

		std::vector<std::vector<std::size_t>> kept_with; // light index -> kept sets holding it, in the order they were kept
		for (const auto idx : order)
		{
			const auto& a = sets[idx];
			for (const auto l : a) if (l >= kept_with.size()) kept_with.resize(static_cast<std::size_t>(l) + 1);
			auto pivot = a.front(); // the light held by the fewest kept sets (the first of equals)
			for (const auto l : a) if (kept_with[l].size() < kept_with[pivot].size()) pivot = l;
			constexpr auto none = static_cast<std::size_t>(-1);
			auto best = none;
			for (const auto b : kept_with[pivot])
			{
				const auto& bs = sets[b];
				if (bs.size() - a.size() > slack) continue;
				if (best != none && bs.size() >= sets[best].size()) continue;
				if (std::includes(bs.begin(), bs.end(), a.begin(), a.end())) best = b;
			}
			if (best != none) { rep[idx] = best; continue; }
			for (const auto l : a) kept_with[l].push_back(idx);
		}
		return rep;
	}

	// entries of the light list array once only the kept sets have a list
	inline std::size_t entries(const std::vector<light_set>& sets, const std::vector<std::size_t>& rep)
	{
		auto n = base_entries;
		for (std::size_t i = 0; i < sets.size(); i++) if (rep[i] == i) n += 1 + sets[i].size();
		return n;
	}

	struct plan
	{
		std::size_t slack = 0;
		std::size_t entries_at_zero = 0;
		std::size_t entries = 0;
		bool fits = false;
		std::vector<std::size_t> rep;
	};

	// forced_slack > 0: that slack only; 0: no absorbing (the lists as before); negative: the smallest slack up to max_slack
	// whose lists fit, 0 when they already do.
	inline plan choose(const std::vector<light_set>& sets, int forced_slack)
	{
		plan p;
		p.rep = absorb(sets, 0);
		p.entries_at_zero = p.entries = entries(sets, p.rep);
		p.fits = p.entries <= entry_limit;
		if (forced_slack == 0 || (forced_slack < 0 && p.fits)) return p;
		const auto first = forced_slack > 0 ? static_cast<std::size_t>(forced_slack) : std::size_t{1};
		const auto last = forced_slack > 0 ? first : max_slack;
		for (auto s = first; s <= last; s++)
		{
			p.slack = s;
			p.rep = absorb(sets, s);
			p.entries = entries(sets, p.rep);
			p.fits = p.entries <= entry_limit;
			if (p.fits) break;
		}
		return p;
	}
}
