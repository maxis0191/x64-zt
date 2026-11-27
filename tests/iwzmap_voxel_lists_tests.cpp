#include "../src/zonetool/zonetool/iw7/iwzmap/iwzmap_voxel_lists.hpp"
#include <cstdio>
#include <set>
#include <stdexcept>

namespace vl = zonetool::iw7::iwzmap::voxel_lists;

void require(bool condition, const char* message)
{
	if (!condition) throw std::runtime_error(message);
}

static std::uint64_t lcg_state = 12345;
static std::uint64_t lcg()
{
	lcg_state = (lcg_state * 1103515245ull + 12345ull) & 0x7fffffffull;
	return lcg_state;
}

int main()
{
	try
	{
		// hand made: {1,2,3} {1,2} {1,2,4} {5} {1,2,3,4}
		const std::vector<vl::light_set> sets = {{1, 2, 3}, {1, 2}, {1, 2, 4}, {5}, {1, 2, 3, 4}};
		auto rep = vl::absorb(sets, 0);
		for (std::size_t i = 0; i < sets.size(); i++) require(rep[i] == i, "slack 0 absorbed a set");
		require(vl::entries(sets, rep) == 3 + 4 + 3 + 4 + 2 + 5, "entries at slack 0");

		rep = vl::absorb(sets, 1);
		require(rep[4] == 4, "the largest set is kept");
		require(rep[0] == 4 && rep[2] == 4, "{1,2,3} and {1,2,4} point at {1,2,3,4}");
		require(rep[1] == 1, "{1,2} is two lights short of {1,2,3,4}: kept at slack 1");
		require(rep[3] == 3, "{5} is a subset of nothing");
		rep = vl::absorb(sets, 2);
		require(rep[1] == 4, "{1,2} points at {1,2,3,4} at slack 2");
		require(vl::entries(sets, rep) == 3 + 5 + 2, "entries at slack 2");

		// a set points at a KEPT set (never at an absorbed one): {1,2} and {1} are subsets of {1,2,3}; {1} is two lights short of it
		const std::vector<vl::light_set> chain = {{1, 2, 3}, {1, 2}, {1}};
		rep = vl::absorb(chain, 2);
		require(rep[0] == 0 && rep[1] == 0 && rep[2] == 0, "slack 2: both subsets point at {1,2,3}");
		rep = vl::absorb(chain, 1);
		require(rep[0] == 0 && rep[1] == 0 && rep[2] == 2, "slack 1: {1} is kept (its only superset {1,2} was absorbed)");

		// the shared fixture of studio/voxellights.py (`python -m pytest tests/test_voxellights.py`): 392 sets from the LCG above
		std::vector<vl::light_set> fixture;
		std::set<vl::light_set> seen;
		for (auto n = 0; n < 400; n++)
		{
			const auto size = 1 + lcg() % 12;
			std::set<unsigned short> s;
			while (s.size() < size) s.insert(static_cast<unsigned short>((lcg() >> 8) % 60));
			const vl::light_set v(s.begin(), s.end());
			if (seen.insert(v).second) fixture.push_back(v);
		}
		require(fixture.size() == 392, "fixture size");
		const std::size_t expected_entries[5] = {3097, 3076, 3044, 3038, 3018};
		const std::size_t expected_kept[5] = {392, 382, 366, 364, 359};
		for (std::size_t k = 0; k < 5; k++)
		{
			rep = vl::absorb(fixture, k);
			std::size_t kept = 0;
			for (std::size_t i = 0; i < rep.size(); i++) kept += rep[i] == i;
			require(vl::entries(fixture, rep) == expected_entries[k], "fixture entries differ from the Python count");
			require(kept == expected_kept[k], "fixture kept sets differ from the Python count");
		}

		// choose: 1600 big sets of 21 lights and one subset of each (20 lights): 68803 entries as they are, 35203 at slack 1
		std::vector<vl::light_set> many;
		for (unsigned short b = 0; b < 1600; b++)
		{
			vl::light_set big;
			for (unsigned short l = 0; l < 21; l++) big.push_back(static_cast<unsigned short>(b * 21 + l));
			many.push_back(big);
			big.pop_back();
			many.push_back(big);
		}
		auto plan = vl::choose(many, -1);
		require(plan.entries_at_zero == 68803 && !(plan.entries_at_zero <= vl::entry_limit), "entries without absorbing");
		require(plan.slack == 1 && plan.fits && plan.entries == 35203, "the smallest slack that fits is 1");
		plan = vl::choose(many, 0);
		require(plan.slack == 0 && !plan.fits && plan.entries == 68803, "slack forced to 0 does not absorb");
		plan = vl::choose(fixture, -1);
		require(plan.slack == 0 && plan.fits && plan.entries == 3097, "a map that fits keeps every list");
		plan = vl::choose(fixture, 3);
		require(plan.slack == 3 && plan.entries == 3038, "a forced slack is used as it is");

		// budget policy: a set held by n leaves takes up to budget / n extra lights (studio/voxellights.py `budget_slack`, `choose_budget`, the same numbers)
		const std::vector<std::size_t> leaves = {3, 1, 2, 1, 40};
		require(vl::budget_slack(leaves, 0) == std::vector<std::size_t>({0, 0, 0, 0, 0}), "budget 0 gives no slack");
		require(vl::budget_slack(leaves, 4) == std::vector<std::size_t>({1, 4, 2, 4, 0}), "slack of budget 4");
		require(vl::budget_slack({1, 1}, 1000) == std::vector<std::size_t>({vl::max_slack, vl::max_slack}), "slack is capped");
		rep = vl::absorb(sets, vl::budget_slack(leaves, 1));
		require(rep == std::vector<std::size_t>({0, 0, 2, 3, 4}), "budget 1: only {1,2}, held by one leaf, may take a light ({1,2,3})");
		require(vl::entries(sets, rep) == 18, "entries at budget 1");
		rep = vl::absorb(sets, vl::budget_slack(leaves, 3));
		require(rep == std::vector<std::size_t>({4, 4, 4, 3, 4}), "budget 3: {1,2,3} (3 leaves) and {1,2} (1 leaf) both reach {1,2,3,4}");
		require(vl::entries(sets, rep) == 10, "entries at budget 3");
		require(vl::absorb(sets, std::vector<std::size_t>(sets.size(), 2)) == vl::absorb(sets, 2), "a list of equal slacks is the uniform slack");
		const std::size_t steps[12] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
		for (std::size_t k = 0; k + 1 < 12; k++) require(vl::next_budget(steps[k]) == steps[k + 1], "budget ladder");

		std::vector<std::size_t> fixture_leaves;
		for (std::size_t i = 0; i < fixture.size(); i++) fixture_leaves.push_back(1 + (i * 37) % 23);
		const std::size_t budgets[5] = {1, 4, 32, 64, 100};
		const std::size_t budget_entries[5] = {3097, 3093, 3032, 2963, 2940};
		const std::size_t budget_kept[5] = {392, 390, 366, 345, 339};
		for (std::size_t k = 0; k < 5; k++)
		{
			rep = vl::absorb(fixture, vl::budget_slack(fixture_leaves, budgets[k]));
			std::size_t kept = 0;
			for (std::size_t i = 0; i < rep.size(); i++) kept += rep[i] == i;
			require(vl::entries(fixture, rep) == budget_entries[k], "fixture entries at a budget differ from the Python count");
			require(kept == budget_kept[k], "fixture kept sets at a budget differ from the Python count");
		}
		auto bp = vl::choose_budget(fixture, fixture_leaves, -1);
		require(bp.budget == 0 && bp.fits && bp.entries == 3097, "a map that fits keeps every list");
		bp = vl::choose_budget(fixture, fixture_leaves, 4);
		require(bp.budget == 4 && bp.slack == 4 && bp.entries == 3093, "a forced budget is used as it is");

		// the 1600 big sets (4 leaves each) and one subset of each (1 leaf): budget 1 lets the subsets (1 leaf, slack 1) point at the big sets: 35203 entries
		std::vector<std::size_t> many_leaves;
		for (std::size_t k = 0; k < many.size(); k += 2) { many_leaves.push_back(4); many_leaves.push_back(1); }
		bp = vl::choose_budget(many, many_leaves, -1);
		require(bp.budget == 1 && bp.slack == 1 && bp.fits && bp.entries == 35203 && bp.entries_at_zero == 68803, "the smallest budget that fits is 1");
		bp = vl::choose_budget(many, many_leaves, 0);
		require(bp.budget == 0 && !bp.fits && bp.entries == 68803, "budget forced to 0 does not absorb");
	}
	catch (const std::exception& ex)
	{
		std::printf("FAIL: %s\n", ex.what());
		return 1;
	}
	std::printf("iwzmap voxel light lists: all checks passed\n");
	return 0;
}
