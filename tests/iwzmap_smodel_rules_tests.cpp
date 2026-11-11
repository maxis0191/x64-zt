#include "../src/zonetool/zonetool/iw7/iwzmap/iwzmap_smodel_rules.hpp"
#include <cstdio>
#include <stdexcept>

namespace rules = zonetool::iw7::iwzmap::smodel_rules;

// the fields of GfxStaticModelDrawInst the rules read and write, with the values of real cp_zmb instances
struct draw_inst
{
	unsigned short unk2;
	unsigned short unk3;
	unsigned short flags;
	unsigned char firstMtlSkinIndex;
};

void require(bool condition, const char* message)
{
	if (!condition) throw std::runtime_error(message);
}

int main()
{
	try
	{
		// cp_zmb instance 0 (zmb_bumper_cars_building_signage_backdrop): flags 582, unk2 3, unk3 65, skin 4
		const draw_inst signage{3, 65, 582, 4};
		// an animated-vertex instance (flags 2118 = 0x846) and a neutral one (flags 582, single sample, skin 0)
		const draw_inst animated{0, 2, 2118, 0};
		const draw_inst vertex_lit{0, 2, 0x86, 0};
		const draw_inst neutral{0, 2, 582, 0};
		const draw_inst world[] = {signage, animated, vertex_lit, neutral};

		require(!rules::is_neutral_donor(signage), "a skin 4 / 65-node instance taken as neutral");
		require(!rules::is_neutral_donor(animated), "an animated-vertex instance taken as neutral");
		require(!rules::is_neutral_donor(vertex_lit), "an instance without light grid lighting taken as neutral");
		require(rules::find_neutral_donor(world, 4) == 3, "neutral donor not found");
		require(rules::find_neutral_donor(world, 3) == -1, "a neutral donor invented");

		// the earlier crash: skin 4 of the signage copied onto debris_concrete_rubble_pile_01_dark (3 lods x 1 surface)
		require(rules::skin_reads_past_materials(4, 0, 3), "skin 4 on a 3-surface model not flagged");
		require(!rules::skin_reads_past_materials(4, 0, 15), "the signage's own skin flagged");
		require(!rules::skin_reads_past_materials(0, 0, 1), "skin 0 on a 1-surface model flagged");

		auto fitted = signage;
		fitted.flags = 2630; // 0xA46: animated vertices + fx marks + light grid
		rules::fit_to_model(fitted);
		require(fitted.firstMtlSkinIndex == 0 && fitted.unk2 == 0 && fitted.unk3 == 2, "model fields not fitted");
		require(fitted.flags == 0x246, "flags other than the motion flags changed, or motion flags kept");
		require(!rules::skin_reads_past_materials(fitted.firstMtlSkinIndex, 0, 1), "a fitted instance reads past a 1-surface model");
	}
	catch (const std::exception& ex)
	{
		std::printf("FAIL: %s\n", ex.what());
		return 1;
	}
	std::printf("iwzmap static model rules: 11 checks passed\n");
	return 0;
}
