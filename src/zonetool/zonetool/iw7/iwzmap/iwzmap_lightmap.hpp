#pragma once

namespace zonetool::iw7::iwzmap
{
	// Baked lightmap of a generated world (source\lightmap\, written by the SDK baker tools-source/iwzmap/bake):
	//   primary.dds      BC4_UNORM  W x H   sun visibility (0.5 = shadow edge)
	//   secondary.dds    BC6H_UF16  W x 2H  top half non-directional irradiance, bottom half directional colour
	//   secondunorm.dds  R8G8_UNORM W x H   dominant light direction (rotated octahedral, tangent space)
	// The images replace the template lightmap's images under the template names, so the GfxWorld keeps its
	// single GfxLightMap and every lightmapped surface (lightmapIndex 0) samples the baked atlas through the
	// lightmap coordinates of world.bin.
	struct baked_lightmap
	{
		bool active = false;
		GfxLightMap* lightmap = nullptr;
		GfxImage* images[3]{};
	};

	// call while the template fastfile is current (template image headers are read from it)
	baked_lightmap prepare_lightmap(GfxWorld* w, const std::string& source, zone_memory* mem);
	// call after filesystem::set_fastfile(<map>): writes images\*.iw7Image and lightmaps\*.json
	void dump_lightmap(const baked_lightmap& lm);
}
