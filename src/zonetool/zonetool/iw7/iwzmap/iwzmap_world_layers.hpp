#pragma once

// world.bin version 2: the per-surface layer stream of layered (`*`) world materials (iwzmap.cpp read_world, build_gfx), header only so that
// tests/iwzmap_world_layers_tests.cpp checks the format without game assets. Format and reasons: iw7-creator-sdk
// docs/station-zero/redesign/BLEND-LAYERS.md section 4.1.
//
// Version 1 and 2 share header, materials, 44 byte vertices, u16 indices. A version 2 surface record is the 44 byte version 1 record plus
// `layer_offset` (bytes, relative to the start of the layer blob, 4 aligned) and `layer_stride` (bytes a vertex, 0 = the surface has no
// layer record). After the surfaces and before the 6 bounds floats: `u32 layer_size`, then `layer_size` bytes (the blob). The layer
// record of vertex `first_vertex + j` of a surface is at `layer_offset + j * layer_stride`, `vertex_count * layer_stride` bytes in all
// (the engine binds slot 1 at the surface's `vertexLayerData` and indexes it like slot 0, so one record per vertex of the surface's range).
// In the zone the blob is `tz->vld`; a surface with a layer record gets `vertexLayerData = base + layer_offset`, every other surface 0.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace zonetool::iw7::iwzmap::world_layers
{
	constexpr std::uint32_t version_plain = 1;
	constexpr std::uint32_t version_layers = 2;
	constexpr std::size_t surface_bytes_v1 = 44;
	constexpr std::size_t surface_bytes_v2 = 52;
	// the zone's layer stream of a world without layers: a non-empty (zeroed) stream, 16 bytes a vertex (the renderer creates a buffer from it)
	constexpr std::size_t zero_bytes_per_vertex = 16;

	struct surface
	{
		std::uint32_t material;
		std::uint32_t first_vertex;
		std::uint32_t vertex_count;
		std::uint32_t base_index;
		std::uint32_t tri_count;
		float mins[3];
		float maxs[3];
		std::uint32_t layer_offset;     // version 2 only, 0 in version 1
		std::uint32_t layer_stride;     // version 2 only, 0 in version 1 (no layer record)
	};

	static_assert(sizeof(surface) == surface_bytes_v2, "world.bin version 2 surface record");

	inline std::size_t surface_bytes(std::uint32_t version)
	{
		if (version == version_plain) return surface_bytes_v1;
		if (version == version_layers) return surface_bytes_v2;
		throw std::runtime_error("world.bin: unsupported version");
	}

	// one surface record as stored (surface_bytes(version) bytes); the version 1 record has no layer fields
	inline surface decode_surface(const unsigned char* p, std::uint32_t version)
	{
		surface s{};
		std::memcpy(&s, p, surface_bytes(version));
		return s;
	}

	// a layered surface's record must lie inside the blob and start 4 byte aligned
	inline void validate(const std::vector<surface>& surfaces, std::size_t layer_size)
	{
		for (std::size_t i = 0; i < surfaces.size(); i++)
		{
			const auto& s = surfaces[i];
			if (!s.layer_stride) continue;
			if (s.layer_offset % 4 != 0) throw std::runtime_error("world.bin: surface " + std::to_string(i) + " layer offset is not 4 byte aligned");
			const auto end = static_cast<std::uint64_t>(s.layer_offset) + static_cast<std::uint64_t>(s.vertex_count) * s.layer_stride;
			if (end > layer_size) throw std::runtime_error("world.bin: surface " + std::to_string(i) + " layer record is past the layer blob");
		}
	}

	// the zone's layer stream: the blob, then zeros up to the zeroed stream a world without layers has (16 bytes a vertex)
	inline std::vector<std::uint8_t> stream(const std::vector<std::uint8_t>& blob, std::size_t vertex_count)
	{
		std::vector<std::uint8_t> out(std::max(blob.size(), vertex_count * zero_bytes_per_vertex));
		if (!blob.empty()) std::memcpy(out.data(), blob.data(), blob.size());
		return out;
	}

	// the surface's `vertexLayerData`: base + offset for a layered surface, 0 for any other
	inline std::uint32_t vertex_layer_data(const surface& s, std::uint32_t base)
	{
		return s.layer_stride ? base + s.layer_offset : 0;
	}

	struct summary_t
	{
		std::size_t surfaces = 0;
		std::uint64_t bytes = 0;
		std::set<std::uint32_t> strides;
	};

	inline summary_t summarize(const std::vector<surface>& surfaces)
	{
		summary_t r;
		for (const auto& s : surfaces)
		{
			if (!s.layer_stride) continue;
			r.surfaces++;
			r.bytes += static_cast<std::uint64_t>(s.vertex_count) * s.layer_stride;
			r.strides.insert(s.layer_stride);
		}
		return r;
	}

	// "layer: N surfaces, M bytes, strides {a, b}"
	inline std::string summary_line(const summary_t& r)
	{
		std::string t;
		for (const auto st : r.strides) t += (t.empty() ? "" : ", ") + std::to_string(st);
		return "layer: " + std::to_string(r.surfaces) + " surfaces, " + std::to_string(r.bytes) + " bytes, strides {" + t + "}";
	}
}
