#pragma once

// Rules for a generated static model whose XModel has no light grid lit instance in the template map ("no twin":
// a kit model of another shipped map, an authored model). Header only, so that tests/iwzmap_smodel_rules_tests.cpp
// checks them without game assets.
//
// Cause of the earlier crash (Station Zero g19-g21, access violation at iw7_ship+0xDCE6E7 in the first client frame):
// a twinless model copied the whole draw instance of the first light grid lit template instance,
// zmb_bumper_cars_building_signage_backdrop, whose firstMtlSkinIndex is 4. The draw pass reads
// model->materialHandles[lodInfo[0].surfIndex + firstMtlSkinIndex]; on a model with fewer than five surfaces that
// slot is past the material list (the crash read two floats as a Material pointer). In all 17,010 cp_zmb instances
// (916 models) firstMtlSkinIndex is a per-model constant below the model's surface count, 752 models use 0.
// Fields that belong to the model and not to the place, measured on the same 17,010 instances:
//   firstMtlSkinIndex  per model, < numsurfs              -> 0 (always a valid surface)
//   unk2 / unk3        per model: light grid sample tree depth (0..3) and node count (2..65), the length of the
//                      instance's own (unk1:unk0) range                      -> the single-sample form (0, 2)
//   flags 0x400 / 0x800 reactive motion (never set in cp_zmb) / animated vertices (13 models) -> cleared
// So a twinless instance starts from a neutral template instance (light grid lit, skin 0, single sample, neither
// motion flag) and is then made valid for its own model.

#include <cstdint>
#include <cstddef>

namespace zonetool::iw7::iwzmap::smodel_rules
{
	constexpr std::uint16_t flag_lightgrid = 0x40;
	constexpr std::uint16_t flag_reactive_motion = 0x400;
	constexpr std::uint16_t flag_animated_verts = 0x800;
	constexpr std::uint16_t neutral_sample_depth = 0;  // unk2
	constexpr std::uint16_t neutral_sample_nodes = 2;  // unk3

	// a template draw instance fit to lend its fields to a twinless model
	template <typename DrawInst> bool is_neutral_donor(const DrawInst& d)
	{
		return (d.flags & flag_lightgrid) && !(d.flags & (flag_reactive_motion | flag_animated_verts)) &&
			d.firstMtlSkinIndex == 0 && d.unk2 == neutral_sample_depth && d.unk3 == neutral_sample_nodes;
	}

	// index of the first neutral donor among count template instances, -1 when there is none
	template <typename DrawInst> int find_neutral_donor(const DrawInst* insts, std::size_t count)
	{
		for (std::size_t i = 0; i < count; i++)
			if (is_neutral_donor(insts[i])) return static_cast<int>(i);
		return -1;
	}

	// the model-dependent fields of a draw instance copied onto a twinless model
	template <typename DrawInst> void fit_to_model(DrawInst& d)
	{
		d.firstMtlSkinIndex = 0;
		d.unk2 = neutral_sample_depth;
		d.unk3 = neutral_sample_nodes;
		d.flags = static_cast<decltype(d.flags)>(d.flags & ~(flag_reactive_motion | flag_animated_verts));
	}

	// the earlier crash in one predicate: the surface the draw pass reads is past the model's material list
	inline bool skin_reads_past_materials(unsigned firstMtlSkinIndex, unsigned lod0SurfIndex, unsigned numsurfs)
	{
		return lod0SurfIndex + firstMtlSkinIndex >= numsurfs;
	}
}
