#pragma once

namespace zonetool::iw7::iwzmap
{
	// Entity physics shapes for generated trigger models (info_volume spawn volumes, trigger_* brush triggers).
	//
	// IW7 script volume tests (ispointinvolume, istouching) and trigger contact use the entity's Havok shape:
	// MapEnts::havokEntsShapeData is an hk_2014 packfile whose HavokPhysicsShapeList holds one
	// hknpDynamicCompoundShape per brush/trigger model, selected by physicsShapeOverrideIdx (with a physics asset such
	// as triggermodeldummydefault). The TriggerHull bounds alone are never "inside" (engine run g5: no shape, every test
	// false). A box shape is an hknpConvexPolytopeShape (8 vertices, 6 planes) instanced once in the compound, with the
	// box bounds in the compound AABB and its bounding-volume tree; all values are in units / 32. Every one of them must
	// describe the same box: patching only the positive half extents (engine run g8) leaves the planes and the negative
	// bounds at the donor's size, so the volume is the intersection of both boxes.
	//
	// assign_volume_shapes gives every generated trigger model [first_generated, trigger.count) with a single centred hull
	// the shape of an unused template box (a donor no kept sub-model, client trigger or referenced template trigger
	// uses), rewritten completely to the generated hull: vertices, planes, compound AABB and tree node bounds. The shape
	// data is copied first (the template asset is not modified). Each written shape is decoded again and verified.
	// Opt out: IWZ_NO_VOLUME_SHAPES. Requires the template shape data (IWZ_KEEP_ENTSHAPES).
	// Returns the number of generated trigger models that received a verified shape.
	unsigned int assign_volume_shapes(MapEnts* e, unsigned int first_generated, const std::string& ents, zone_memory* mem);
}
