#pragma once

namespace zonetool::iw7::iwzmap
{
	// Builds the world assets of a new map from a stock template map and generated sources.
	//   template_zone: a zone dumped with dumpzone, moved to zonetool/<template_zone>/
	//   map: new map name (cp_*), written to dump/<map>/ in the regular dump layout
	//   source: directory with world.bin, colmap.hkx, navgraph.bin and ents.txt (see iwzmap.cpp)
	bool compile(const std::string& template_zone, const std::string& template_map,
		const std::string& map, const std::string& source);
}
