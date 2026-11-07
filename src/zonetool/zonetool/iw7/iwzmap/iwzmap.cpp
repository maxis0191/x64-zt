#include <std_include.hpp>
#include "zonetool/iw7/zonetool.hpp"
#include "iwzmap.hpp"
#include "iwzmap_lightmap.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <sstream>

#include "zonetool/iw7/assets/gfxworld.hpp"
#include "zonetool/iw7/assets/gfxworld_tr.hpp"
#include "zonetool/iw7/assets/clipmap.hpp"
#include "zonetool/iw7/assets/comworld.hpp"
#include "zonetool/iw7/assets/mapents.hpp"
#include "zonetool/iw7/assets/navmesh.hpp"
#include "zonetool/iw7/assets/aipaths.hpp"
#include "zonetool/iw7/assets/glassworld.hpp"
#include "zonetool/iw7/assets/fxworld.hpp"

#include <utils/io.hpp>

// IW7 map compiler ("iwzmap").
//
// IW7 has no map compiler. This command takes the world assets of a stock map as a template
// (sun, sky, light grid, reflection probes, lightmap atlas, primary lights, physics capacities)
// and replaces everything spatial with generated data:
//   world.bin     render geometry: materials, GfxWorldVertex array, u16 indices, surfaces
//   colmap.hkx    Havok hk_2014.2.5 packfile (HavokPhysicsShapeList -> hknpCompressedMeshShape)
//   navgraph.bin  bfx NavGraph resource (graph version 0x2B)
//   ents.txt      entity string
// Everything spatial that is not replaced (static models, umbra, decals, glass, dyn ents, scriptables,
// client triggers, shadow caster lists) is emptied so that no template content appears in the new map.
// The result is written in the regular dump layout (dump/<map>/...), ready for buildzone.

namespace zonetool::iw7::iwzmap
{
	namespace
	{
		// Conservative Umbra tome (format 20, the version IW7 and IW8 ship): one tile holding one cell that lists
		// every object, each object's bounds spanning the whole scene, so the Umbra query never culls a generated
		// object; the renderer still frustum culls. Object user ids carry the class in bits 28-31
		// (0 world surface, 5 reflection probe instance).
		// Layout: header 0x170 (version 0xD6000014, CRC32C of [8, end) at +4, size at +8), 16-byte aligned blocks.
		class tome_writer
		{
		public:
			std::uint32_t block(std::size_t size)
			{
				align();
				const auto offset = static_cast<std::uint32_t>(data_.size());
				data_.resize(data_.size() + size);
				return offset;
			}

			void align() { data_.resize((data_.size() + 15) & ~static_cast<std::size_t>(15)); }

			template <typename T> void put(std::uint32_t offset, T value)
			{
				std::memcpy(data_.data() + offset, &value, sizeof(T));
			}

			void put_bounds(std::uint32_t offset, const float* mins, const float* maxs)
			{
				std::memcpy(data_.data() + offset, mins, 12);
				std::memcpy(data_.data() + offset + 12, maxs, 12);
			}

			std::uint32_t size() const { return static_cast<std::uint32_t>(data_.size()); }
			std::string& data() { return data_; }

		private:
			std::string data_;
		};

		std::uint32_t crc32c(const char* data, std::size_t size)
		{
			auto crc = 0xFFFFFFFFu;
			for (std::size_t i = 0; i < size; i++)
			{
				crc ^= static_cast<std::uint8_t>(data[i]);
				for (auto bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
			}
			return ~crc;
		}

		std::string build_conservative_tome(const std::vector<std::uint32_t>& user_ids, const float* scene_mins, const float* scene_maxs)
		{
			const auto count = static_cast<std::uint32_t>(user_ids.size());
			if (!count) throw std::runtime_error("an Umbra tome needs at least one object");
			auto width = 1u;
			while (width < 31 && (1u << width) < count) width++;

			// pad the scene a little so no surface sits on the tile border
			float mins[3], maxs[3];
			for (auto a = 0; a < 3; a++) { mins[a] = scene_mins[a] - 64.0f; maxs[a] = scene_maxs[a] + 64.0f; }

			tome_writer t;
			t.block(0x170);
			t.put<std::uint32_t>(0x00, 0xD6000014);
			t.put<float>(0x0C, 512.0f);
			std::memcpy(t.data().data() + 0x14, mins, 12);
			std::memcpy(t.data().data() + 0x20, maxs, 12);
			std::memcpy(t.data().data() + 0x150, mins, 12);
			std::memcpy(t.data().data() + 0x15C, maxs, 12);
			t.put<std::uint32_t>(0x40, count);
			t.put<std::uint32_t>(0x54, width);
			t.put<std::uint32_t>(0x5C, count);
			t.put<std::uint32_t>(0x8C, 1); // leaf tiles
			t.put<std::uint32_t>(0x90, 1); // tiles

			// top-level tile tree: a single leaf
			const auto top_tree = t.block(4);
			t.put<std::uint32_t>(top_tree, 3);
			const auto top_map = t.block(4);
			t.put<std::uint32_t>(0x2C, 33);
			t.put<std::uint32_t>(0x30, top_tree);
			t.put<std::uint32_t>(0x34, top_map);

			// per-object bounds (min, max) and distance boxes (min, 0, max, +inf), all scene sized
			const auto bounds = t.block(static_cast<std::size_t>(count) * 24);
			const auto distances = t.block(static_cast<std::size_t>(count) * 32);
			for (auto i = 0u; i < count; i++)
			{
				t.put_bounds(bounds + i * 24, mins, maxs);
				std::memcpy(t.data().data() + distances + i * 32, mins, 12);
				t.put<float>(distances + i * 32 + 12, 0.0f);
				std::memcpy(t.data().data() + distances + i * 32 + 16, maxs, 12);
				t.put<float>(distances + i * 32 + 28, std::numeric_limits<float>::infinity());
			}
			t.put<std::uint32_t>(0x44, bounds);
			t.put<std::uint32_t>(0x48, distances);

			const auto ids = t.block(static_cast<std::size_t>(count) * 4);
			for (auto i = 0u; i < count; i++) t.put<std::uint32_t>(ids + i * 4, user_ids[i]);
			t.put<std::uint32_t>(0x50, ids);

			// the cell's object list: indices 0..count-1, bit packed with the index width
			const auto list_bytes = (static_cast<std::size_t>(count) * width + 31) / 32 * 4;
			const auto list = t.block(list_bytes);
			for (auto i = 0u; i < count; i++)
			{
				const auto bit0 = static_cast<std::size_t>(i) * width;
				for (auto b = 0u; b < width; b++)
				{
					if (i & (1u << b))
					{
						t.data()[list + ((bit0 + b) >> 3)] |= static_cast<char>(1u << ((bit0 + b) & 7));
					}
				}
			}
			t.put<std::uint32_t>(0x58, list);

			const auto cell_starts = t.block(8);
			t.put<std::uint32_t>(cell_starts, 0);
			t.put<std::uint32_t>(cell_starts + 4, 1);
			t.put<std::uint32_t>(0x88, cell_starts);
			const auto lod = t.block(4);
			t.put<float>(lod, 1.0f);
			t.put<std::uint32_t>(0x9C, lod);
			const auto tile_table = t.block(4);
			t.put<std::uint32_t>(0xA0, tile_table);
			const auto expand = t.block(4);
			t.put<float>(expand, 0.0f);
			t.put<std::uint32_t>(0x14C, expand);

			// the tile: bounds, quantization scale (largest extent over 16 bits), one leaf, one cell
			const auto tile = t.block(0x60);
			t.put_bounds(tile, mins, maxs);
			t.put<std::uint32_t>(tile + 0x18, 33);
			t.put<float>(tile + 0x30, 0.0f);
			t.put<std::uint32_t>(tile + 0x34, 1);
			auto extent = 0.0;
			for (auto a = 0; a < 3; a++) extent = std::max(extent, static_cast<double>(maxs[a]) - mins[a]);
			auto scale = static_cast<float>(extent / 65536.0);
			if (static_cast<double>(scale) < extent / 65536.0) scale = std::nextafter(scale, std::numeric_limits<float>::infinity());
			t.put<float>(tile + 0x44, scale);
			t.put<float>(tile + 0x48, 1.0f / scale);
			t.put<float>(0x168, scale); // the header repeats the root tile scale (object contribution cull)

			const auto tile_tree = t.block(4);
			t.put<std::uint32_t>(tile_tree, 3);
			t.put<std::uint32_t>(tile + 0x1C, tile_tree - tile);
			const auto tile_map = t.block(8);
			t.put<std::uint32_t>(tile + 0x20, tile_map - tile);
			const auto cell_index = t.block(8);
			t.put<std::uint32_t>(cell_index, 0);
			t.put<std::uint32_t>(cell_index + 4, 0xFFFFFFFF);
			t.put<std::uint32_t>(tile + 0x40, cell_index - tile);
			const auto cell = t.block(36);
			t.put<std::uint32_t>(cell + 0x0C, count);
			t.put<std::uint32_t>(cell + 0x10, 0xFFFFFFFF);
			t.put<std::uint32_t>(cell + 0x14, 0x80000000);
			t.put<std::uint32_t>(cell + 0x1C, 0x0000FFFF);
			t.put<std::uint32_t>(cell + 0x20, 0xFFFFFFFF);
			t.put<std::uint32_t>(tile + 0x38, cell - tile);
			t.align();
			t.put<std::uint32_t>(tile + 0x2C, ((t.size() - tile) << 8) | 3u);
			t.put<std::uint32_t>(tile_table, tile);

			t.put<std::uint32_t>(0x08, t.size());
			t.put<std::uint32_t>(0x04, crc32c(t.data().data() + 8, t.size() - 8));
			return t.data();
		}

		struct src_surface
		{
			std::uint32_t material;
			std::uint32_t first_vertex;
			std::uint32_t vertex_count;
			std::uint32_t base_index;
			std::uint32_t tri_count;
			float mins[3];
			float maxs[3];
		};

		struct world_source
		{
			std::vector<std::string> materials;
			std::vector<GfxWorldVertex> vertices;
			std::vector<std::uint16_t> indices;
			std::vector<src_surface> surfaces;
			float mins[3]{};
			float maxs[3]{};
		};

		class reader
		{
		public:
			explicit reader(const std::string& data) : data_(data) {}

			template <typename T> T get()
			{
				T v{};
				need(sizeof(T));
				std::memcpy(&v, data_.data() + pos_, sizeof(T));
				pos_ += sizeof(T);
				return v;
			}

			void get_bytes(void* out, std::size_t n)
			{
				need(n);
				std::memcpy(out, data_.data() + pos_, n);
				pos_ += n;
			}

			std::string get_string()
			{
				const auto len = get<std::uint16_t>();
				need(len);
				std::string s(data_.data() + pos_, len);
				pos_ += len;
				return s;
			}

		private:
			void need(std::size_t n) const
			{
				if (pos_ + n > data_.size()) throw std::runtime_error("world.bin is truncated");
			}

			const std::string& data_;
			std::size_t pos_ = 0;
		};

		world_source read_world(const std::string& path)
		{
			std::string data;
			if (!utils::io::read_file(path, &data)) throw std::runtime_error("cannot read " + path);

			reader r(data);
			if (r.get<std::uint32_t>() != 0x575A5749) throw std::runtime_error("world.bin: bad magic");
			if (r.get<std::uint32_t>() != 1) throw std::runtime_error("world.bin: unsupported version");

			world_source w;
			w.materials.resize(r.get<std::uint32_t>());
			for (auto& m : w.materials) m = r.get_string();

			w.vertices.resize(r.get<std::uint32_t>());
			r.get_bytes(w.vertices.data(), w.vertices.size() * sizeof(GfxWorldVertex));

			w.indices.resize(r.get<std::uint32_t>());
			r.get_bytes(w.indices.data(), w.indices.size() * sizeof(std::uint16_t));

			w.surfaces.resize(r.get<std::uint32_t>());
			r.get_bytes(w.surfaces.data(), w.surfaces.size() * sizeof(src_surface));

			r.get_bytes(w.mins, sizeof(w.mins));
			r.get_bytes(w.maxs, sizeof(w.maxs));
			return w;
		}

		Bounds make_bounds(const float* mins, const float* maxs)
		{
			Bounds b{};
			for (auto a = 0; a < 3; a++)
			{
				b.midPoint[a] = (mins[a] + maxs[a]) * 0.5f;
				b.halfSize[a] = (maxs[a] - mins[a]) * 0.5f;
			}
			return b;
		}

		template <typename T> T* name_stub(zone_memory* mem, const std::string& name)
		{
			auto* asset = mem->allocate<T>();
			asset->name = mem->duplicate_string(name);
			return asset;
		}

		void log_template(const GfxWorld* w)
		{
			ZONETOOL_INFO("offsets: draw=%zu draw.lightmapReindexData=%zu draw.lightMaps=%zu draw.transientZones=%zu draw.indices=%zu draw.volumetrics=%zu lightGrid=%zu",
				offsetof(GfxWorld, draw), offsetof(GfxWorld, draw) + offsetof(GfxWorldDraw, lightmapReindexData),
				offsetof(GfxWorld, draw) + offsetof(GfxWorldDraw, lightMaps), offsetof(GfxWorld, draw) + offsetof(GfxWorldDraw, transientZones),
				offsetof(GfxWorld, draw) + offsetof(GfxWorldDraw, indices), offsetof(GfxWorld, draw) + offsetof(GfxWorldDraw, volumetrics),
				offsetof(GfxWorld, lightGrid));
			ZONETOOL_INFO("template gfxworld: surfaces %u cells %d planes %d nodes %d smodels %u staticSurf %u visData %u "
				"primaryLights %u lightmaps %u probes %u models %d skies %d umbra %u tz %u [%s %s %s]",
				w->surfaceCount, w->dpvsPlanes.cellCount, w->planeCount, w->nodeCount, w->dpvs.smodelCount,
				w->dpvs.staticSurfaceCount, w->dpvs.surfaceVisDataCount, w->primaryLightCount, w->draw.lightMapCount,
				w->draw.reflectionProbeData.reflectionProbeCount, w->modelCount, w->skyCount, w->umbraTomeSize, w->draw.transientZoneCount, w->draw.transientZones[0] ? w->draw.transientZones[0]->name : "-", w->draw.transientZones[1] ? w->draw.transientZones[1]->name : "-", w->draw.transientZones[2] ? w->draw.transientZones[2]->name : "-");
		}

		float half_to_float(std::uint16_t h)
		{
			const auto sign = (h >> 15) & 1u, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
			float v;
			if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
			else if (exponent == 31) v = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
			else v = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
			return sign ? -v : v;
		}

		std::uint16_t float_to_half(float f)
		{
			std::uint32_t x;
			std::memcpy(&x, &f, 4);
			const std::uint16_t sign = static_cast<std::uint16_t>((x >> 16) & 0x8000u);
			const auto exponent = static_cast<int>((x >> 23) & 0xFFu) - 127 + 15;
			const auto mantissa = x & 0x7FFFFFu;
			if (exponent <= 0) return sign;
			if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);
			auto h = static_cast<std::uint32_t>((exponent << 10) | (mantissa >> 13));
			if (mantissa & 0x1000u) h++; // round half up
			return static_cast<std::uint16_t>(sign | h);
		}

		// Light grid probes hold RGB order-2 spherical harmonics as halves: coefficients 0-8 red, 9-17 green,
		// 18-26 blue (0 = constant term, 2 = the vertical term, positive for light from above), then 27 sky
		// visibility. The probes around the generated world were baked for the template's geometry there; the
		// lighting file (lighting.txt in the source, "ambient r g b up" lines with an optional room box
		// "box x0 y0 z0 x1 y1 z1") replaces them with the generated world's own ambient.
		struct ambient_zone
		{
			float mins[3];
			float maxs[3];
			float rgb[3];
			float up;
		};

		std::vector<ambient_zone> read_lighting(const std::string& path, const world_source& src)
		{
			std::vector<ambient_zone> zones;
			std::string text;
			if (!utils::io::read_file(path, &text)) return zones;
			std::istringstream in(text);
			std::string line;
			while (std::getline(in, line))
			{
				std::istringstream l(line);
				std::string kind;
				l >> kind;
				ambient_zone z{};
				if (kind == "ambient")
				{
					l >> z.rgb[0] >> z.rgb[1] >> z.rgb[2] >> z.up;
					for (auto a = 0; a < 3; a++) { z.mins[a] = src.mins[a] - 512.0f; z.maxs[a] = src.maxs[a] + 512.0f; }
				}
				else if (kind == "room")
				{
					l >> z.mins[0] >> z.mins[1] >> z.mins[2] >> z.maxs[0] >> z.maxs[1] >> z.maxs[2] >> z.rgb[0] >> z.rgb[1] >> z.rgb[2] >> z.up;
				}
				else continue;
				zones.push_back(z);
			}
			return zones;
		}

		unsigned int relight_probes(GfxGpuLightGridProbePosition* positions, GfxSHProbeData* probes, unsigned int count,
			const std::vector<ambient_zone>& zones)
		{
			auto changed = 0u;
			for (auto i = 0u; i < count && positions && probes; i++)
			{
				const auto* p = positions[i].origin;
				// the last zone containing the probe wins (rooms after the global ambient)
				const ambient_zone* pick = nullptr;
				for (const auto& z : zones)
				{
					if (p[0] >= z.mins[0] && p[0] <= z.maxs[0] && p[1] >= z.mins[1] && p[1] <= z.maxs[1] && p[2] >= z.mins[2] && p[2] <= z.maxs[2]) pick = &z;
				}
				if (!pick) continue;
				for (auto c = 0; c < 3; c++)
				{
					for (auto k = 0; k < 9; k++) probes[i].coeffs[c * 9 + k] = 0;
					probes[i].coeffs[c * 9 + 0] = float_to_half(pick->rgb[c]);
					probes[i].coeffs[c * 9 + 2] = float_to_half(pick->rgb[c] * pick->up);
				}
				probes[i].coeffs[27] = float_to_half(1.0f);
				changed++;
			}
			return changed;
		}

		// light grid probes around the generated world (inspection: IWZ_LOG_PROBES)
		void log_probes(const GfxWorld* w, const world_source& src)
		{
			const auto& pd = w->lightGrid.probeData;
			float lo[3] = {3.4e38f, 3.4e38f, 3.4e38f}, hi[3] = {-3.4e38f, -3.4e38f, -3.4e38f};
			auto inside = 0u;
			for (auto i = 0u; i < pd.probeCount && pd.probePositions; i++)
			{
				const auto* p = pd.probePositions[i].origin;
				auto in = true;
				for (auto a = 0; a < 3; a++)
				{
					lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]);
					if (p[a] < src.mins[a] - 256.0f || p[a] > src.maxs[a] + 256.0f) in = false;
				}
				if (!in) continue;
				if (inside++ < 4 && pd.probes)
				{
					std::string c;
					for (auto k = 0; k < 29; k++) c += std::to_string(half_to_float(pd.probes[i].coeffs[k])).substr(0, 6) + " ";
					ZONETOOL_INFO("probe %u at %.0f %.0f %.0f: %s", i, p[0], p[1], p[2], c.data());
				}
			}
			ZONETOOL_INFO("light grid: probes %u (gpu visible %u) zones %u tetrahedrons %u (visible %u); bounds %.0f %.0f %.0f .. %.0f %.0f %.0f; %u near the generated world",
				pd.probeCount, pd.gpuVisibleProbesCount, pd.zoneCount, pd.tetrahedronCount, pd.tetrahedronCountVisible,
				lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], inside);
		}

		void build_gfx(GfxWorld* w, GfxWorldTransientZone* tz, const world_source& src, const std::string& map,
			const std::string& bsp, zone_memory* mem)
		{
			log_template(w);
			if (std::getenv("IWZ_LOG_PROBES")) log_probes(w, src);
			{
				std::string nodes;
				for (auto i = 0; i < std::min(w->nodeCount, 16); i++) nodes += std::to_string(w->dpvsPlanes.nodes[i]) + " ";
				ZONETOOL_INFO("template dpvs: planes %d nodes %d [%s] cells %d cell0 portals %u tinfo0 tree %u tz %u",
					w->planeCount, w->nodeCount, nodes.data(), w->dpvsPlanes.cellCount, w->cells[0].portalCount,
					w->cellTransientInfos[0].aabbTreeIndex, w->cellTransientInfos[0].transientZone);
				{
					std::map<int, int> low; int above = 0;
					for (auto i = 0; i < w->nodeCount; i++)
					{
						const int v = w->dpvsPlanes.nodes[i];
						if (v <= 4) low[v]++;
						if (v > w->planeCount) above++;
					}
					ZONETOOL_INFO("template nodes: v0 %d v1 %d v2 %d v3 %d v4 %d; above planeCount %d", low[0], low[1], low[2], low[3], low[4], above);
				}
				if (tz->aabbTreeCounts && tz->aabbTrees)
				{
					const auto& t = tz->aabbTrees[0].aabbTree[0];
					ZONETOOL_INFO("template tz: cells %u trees(cell0) %u root children %u surfs %u start %u smodels %u childOffset %d",
						tz->cellCount, tz->aabbTreeCounts[0].aabbTreeCount, t.childCount, t.surfaceCount, t.startSurfIndex, t.smodelIndexCount, t.childrenOffset);
				}
				const auto& s0 = w->dpvs.surfaces[w->dpvs.litOpaqueSurfsBegin];
				ZONETOOL_INFO("template surface: lm %u flags %02x sort %u unk %u %u %u %u tz %u layer %u; lit opaque %u..%u of %u",
					s0.lightmapIndex, s0.flags, s0.sortKey, s0.unk1, s0.unk2, s0.unk3, s0.unk4, s0.transientZone, s0.tris.vertexLayerData,
					w->dpvs.litOpaqueSurfsBegin, w->dpvs.litOpaqueSurfsEnd, w->surfaceCount);
			}

			// surface template per material (flags, sort key and the unknown bytes of a stock surface
			// using the same material), falling back to the first opaque template surface
			std::unordered_map<std::string, unsigned int> by_material;
			for (auto i = 0u; i < w->surfaceCount; i++)
			{
				const auto& s = w->dpvs.surfaces[i];
				if (s.material && s.material->name && !by_material.contains(s.material->name))
				{
					by_material[s.material->name] = i;
				}
			}
			const auto fallback = w->dpvs.litOpaqueSurfsBegin;
			for (auto i : {0u, 1u, 2u, 100u, 1000u})
			{
				if (i < w->surfaceCount) ZONETOOL_INFO("template drawsurf %u: objectId %u packed %016llx %016llx", i,
					static_cast<unsigned>(w->dpvs.surfaceMaterials[i].fields.objectId),
					w->dpvs.surfaceMaterials[i].packed.p0, w->dpvs.surfaceMaterials[i].packed.p1);
			}

			w->name = mem->duplicate_string(bsp);
			w->baseName = mem->duplicate_string(map);

			// Umbra (the shipping IW7 visibility path) needs a tome whose object ids match the world's surfaces and
			// static models. Without a generated tome, keep the template visibility system: the template surfaces
			// stay (degenerate, nothing drawn) at their indices, the template static models stay (hidden), and the
			// generated surfaces follow them.
			const bool keep_vis = std::getenv("IWZ_KEEP_UMBRA") != nullptr;
			const bool slot_mode = keep_vis && std::getenv("IWZ_SLOT_SURFACES") != nullptr;
			const auto own = static_cast<unsigned int>(src.surfaces.size());
			const auto tpl_count = keep_vis ? w->surfaceCount : 0u;
			const auto count = slot_mode ? tpl_count : tpl_count + own;
			// slot k receives generated surface k: template opaque surfaces whose bounds contain the generated world,
			// smallest first (their Umbra objects are visible from inside the generated world)
			std::vector<unsigned int> slots;
			if (slot_mode)
			{
				// per generated surface: the smallest unused opaque template surface containing it, else the one it
				// overlaps most (that template surface's Umbra object then governs its visibility)
				std::vector<bool> taken(w->surfaceCount);
				auto contained = 0u;
				for (auto k = 0u; k < own; k++)
				{
					const auto& g = src.surfaces[k];
					float best_contain = 3.4e38f, best_overlap = -1.0f;
					int contain_i = -1, overlap_i = -1;
					for (auto i = w->dpvs.litOpaqueSurfsBegin; i < w->dpvs.litOpaqueSurfsEnd; i++)
					{
						if (taken[i]) continue;
						const auto& b = w->dpvs.surfacesBounds[i].bounds;
						bool contains = true;
						float overlap = 1.0f;
						for (auto a = 0; a < 3; a++)
						{
							const auto lo = b.midPoint[a] - b.halfSize[a], hi = b.midPoint[a] + b.halfSize[a];
							if (lo > g.mins[a] || hi < g.maxs[a]) contains = false;
							overlap *= std::max(0.0f, std::min(hi, g.maxs[a] + 1.0f) - std::max(lo, g.mins[a] - 1.0f));
						}
						const auto volume = b.halfSize[0] * b.halfSize[1] * std::max(b.halfSize[2], 1.0f);
						if (contains && volume < best_contain) { best_contain = volume; contain_i = static_cast<int>(i); }
						if (overlap > best_overlap) { best_overlap = overlap; overlap_i = static_cast<int>(i); }
					}
					const auto pick = contain_i >= 0 ? contain_i : overlap_i;
					if (pick < 0) throw std::runtime_error("no template surface slot left for a generated surface");
					if (contain_i >= 0) contained++;
					taken[pick] = true;
					slots.push_back(static_cast<unsigned int>(pick));
				}
				ZONETOOL_INFO("iwzmap: %u generated surfaces in template slots (%u contained by their slot surface)", own, contained);
			}
			auto* surfaces = mem->allocate<GfxSurface>(count);
			auto* bounds = mem->allocate<GfxSurfaceBounds>(count);
			// per-surface draw keys are runtime data (zero in the zone); keep the array sized to the new surfaces
			auto* draw_surfs = mem->allocate<GfxDrawSurf>(count);
			std::vector<Material*> materials(src.materials.size());
			for (auto i = 0u; i < src.materials.size(); i++)
			{
				materials[i] = name_stub<Material>(mem, src.materials[i]);
				if (!by_material.contains(src.materials[i]))
				{
					ZONETOOL_WARNING("material \"%s\" is not used by the template world; using template surface defaults",
						src.materials[i].data());
				}
			}

			for (auto i = 0u; i < tpl_count; i++)
			{
				surfaces[i] = w->dpvs.surfaces[i];
				surfaces[i].tris.triCount = 0;
				surfaces[i].tris.vertexCount = 0;
				surfaces[i].tris.firstVertex = 0;
				surfaces[i].tris.baseIndex = 0;
				surfaces[i].transientZone = 0;
				bounds[i] = w->dpvs.surfacesBounds[i];
				draw_surfs[i] = w->dpvs.surfaceMaterials[i];
			}
			for (auto k = 0u; k < own; k++)
			{
				const auto i = slot_mode ? slots[k] : tpl_count + k;
				const auto& s = src.surfaces[k];
				const auto& mat_name = src.materials[s.material];
				const auto tpl = by_material.contains(mat_name) ? by_material[mat_name] : fallback;
				surfaces[i] = w->dpvs.surfaces[tpl];
				draw_surfs[i] = w->dpvs.surfaceMaterials[tpl];
				surfaces[i].material = materials[s.material];
				surfaces[i].tris.vertexLayerData = 0;
				surfaces[i].tris.firstVertex = s.first_vertex;
				surfaces[i].tris.vertexCount = static_cast<unsigned short>(s.vertex_count);
				surfaces[i].tris.triCount = static_cast<unsigned short>(s.tri_count);
				surfaces[i].tris.baseIndex = s.base_index;
				surfaces[i].transientZone = 0;

				float max_edge = 0.0f;
				for (auto t = 0u; t < s.tri_count * 3; t++)
				{
					const auto& a = src.vertices[s.first_vertex + src.indices[s.base_index + t]].xyz;
					const auto& b = src.vertices[s.first_vertex + src.indices[s.base_index + (t / 3) * 3 + (t + 1) % 3]].xyz;
					const auto d = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
					max_edge = std::max(max_edge, d);
				}
				surfaces[i].tris.maxEdgeLength = max_edge;

				bounds[i] = w->dpvs.surfacesBounds[0];
				bounds[i].bounds = make_bounds(s.mins, s.maxs);
			}

			w->surfaceCount = count;
			w->dpvs.surfaces = surfaces;
			w->dpvs.surfacesBounds = bounds;
			w->dpvs.surfaceMaterials = draw_surfs;
			w->dpvs.staticSurfaceCount = count;
			if (!slot_mode)
			{
				w->dpvs.sortedSurfIndex = mem->allocate<unsigned int>(count);
				for (auto i = 0u; i < count; i++) w->dpvs.sortedSurfIndex[i] = i;
				if (slot_mode)
				{
					std::vector<bool> used(count);
					auto pos = 0u;
					for (const auto slot : slots) { w->dpvs.sortedSurfIndex[pos++] = slot; used[slot] = true; }
					for (auto i = 0u; i < count; i++) if (!used[i]) w->dpvs.sortedSurfIndex[pos++] = i;
				}
				w->dpvs.litOpaqueSurfsBegin = 0;
				w->dpvs.litOpaqueSurfsEnd = count;
				w->dpvs.litDecalSurfsBegin = w->dpvs.litDecalSurfsEnd = count;
				w->dpvs.litTransSurfsBegin = w->dpvs.litTransSurfsEnd = count;
				w->dpvs.emissiveSurfsBegin = w->dpvs.emissiveSurfsEnd = count;
			}
			w->dpvs.surfaceVisDataCount = count;
			w->dpvs.surfaceCastsSunShadow = mem->allocate<unsigned int>(count);
			for (auto i = 0u; i < count; i++) w->dpvs.surfaceCastsSunShadow[i] = 0xFFFFFFFF;
			w->dpvs.sunShadowOptCount = 0;
			w->dpvs.sunSurfVisDataCount = 0;
			w->dpvs.surfaceCastsSunShadowOpt = nullptr;

			if (keep_vis)
			{
				for (auto i = 0u; i < w->dpvs.smodelCount; i++) w->dpvs.smodelDrawInsts[i].placement.scale = 0.0f;
				ZONETOOL_INFO("iwzmap: kept template visibility (%u surfaces, %u hidden static models)", tpl_count, w->dpvs.smodelCount);
			}
			else
			{
			// no static models
			w->dpvs.smodelCount = 0;
			w->dpvs.smodelVisDataCount = 0;
			w->dpvs.lodData = mem->allocate<unsigned int>(1);
			w->dpvs.smodelInsts = nullptr;
			w->dpvs.smodelDrawInsts = nullptr;
			w->dpvs.sortedSmodelIndices = nullptr;
			}
			if (!keep_vis)
			{
				w->dpvs.decalVisDataCount = 0;
				w->dpvs.volumetricVisDataCount = 0;
			}

			// one cell, one BSP leaf (node = {cellIndex + 1, rightChildOffset})
			const auto world_bounds = make_bounds(src.mins, src.maxs);
			if (!keep_vis)
			{
			w->planeCount = 0;
			w->dpvsPlanes.planes = nullptr;
			w->nodeCount = 2;
			w->dpvsPlanes.nodes = mem->allocate<unsigned short>(2);
			w->dpvsPlanes.nodes[0] = 1;
			w->dpvsPlanes.nodes[1] = 0;
			w->dpvsPlanes.cellCount = 1;
			w->cells = mem->allocate<GfxCell>(1);
			w->cells[0].bounds = world_bounds;
			w->cells[0].portalCount = 0;
			w->cells[0].portals = nullptr;
			w->cellTransientInfos = mem->allocate<GfxCellTransientInfo>(1);
			w->cellTransientInfos[0].aabbTreeIndex = 0;
			w->cellTransientInfos[0].transientZone = 0;
			}

			// sky: keep the template image, no sky surfaces
			for (auto i = 0; i < w->skyCount; i++)
			{
				w->skies[i].skySurfCount = 0;
				w->skies[i].skyStartSurfs = nullptr;
			}

			// world brush model 0 covers every surface. IWZ_KEEP_SUBMODELS keeps the template brush sub-model slots
			// (the collision of script_brushmodel entities, e.g. door blockers) without any render surfaces
			const auto keep_sub = std::getenv("IWZ_KEEP_SUBMODELS") != nullptr;
			ZONETOOL_INFO("iwzmap: template gfx brush models %u (%s)", w->modelCount, keep_sub ? "kept without surfaces" : "trimmed to the world");
			if (keep_sub)
			{
				for (auto i = 1; i < static_cast<int>(w->modelCount); i++)
				{
					w->models[i].startSurfIndex = 0;
					w->models[i].surfaceCount = 0;
				}
			}
			else
			{
				w->modelCount = 1;
			}
			w->models[0].bounds = world_bounds;
			w->models[0].startSurfIndex = slot_mode ? 0 : tpl_count;
			w->models[0].surfaceCount = static_cast<unsigned short>(own);
			w->models[0].radius = std::sqrt(world_bounds.halfSize[0] * world_bounds.halfSize[0] +
				world_bounds.halfSize[1] * world_bounds.halfSize[1] + world_bounds.halfSize[2] * world_bounds.halfSize[2]);
			w->bounds = world_bounds;

			// materials used by the world
			w->materialMemoryCount = static_cast<int>(materials.size());
			w->materialMemory = mem->allocate<MaterialMemory>(materials.size());
			for (auto i = 0u; i < materials.size(); i++)
			{
				w->materialMemory[i].material = materials[i];
				w->materialMemory[i].memory = 0;
			}

			// lights keep their template data, but nothing may reference template surfaces or models
			if (w->shadowGeomOptimized)
			{
				for (auto i = 0u; i < w->primaryLightCount; i++)
				{
					w->shadowGeomOptimized[i].surfaceCount = 0;
					w->shadowGeomOptimized[i].smodelCount = 0;
					w->shadowGeomOptimized[i].sortedSurfIndex = nullptr;
					w->shadowGeomOptimized[i].smodelIndex = nullptr;
				}
			}

			// no decal volumes, volumetrics, heightfields or occlusion
			// (kept with the template visibility: Umbra objects of these types set their vis bits)
			if (!std::getenv("IWZ_KEEP_UMBRA"))
			{
				w->draw.decalVolumeCollectionCount = 0;
				w->draw.decalVolumeCollections = nullptr;
				w->draw.volumetrics.volumetricCount = 0;
				w->draw.volumetrics.volumetrics = nullptr;
			}
			w->heightfieldCount = 0;
			w->heightfields = nullptr;
			if (!std::getenv("IWZ_KEEP_UMBRA"))
			{
				w->numUmbraGates = 0;
				w->umbraGates = nullptr;
				w->umbraTomeSize = 0;
				w->umbraTomeData = nullptr;
				w->umbraTomePtr = nullptr;
				w->numUmbraGates2 = 0;
				w->umbraGates2 = nullptr;
				w->umbraTomeSize2 = 0;
				w->umbraTomeData2 = nullptr;
				w->umbraTomePtr2 = nullptr;
				w->umbraUnkSize = 0;
				w->umbraUnkData = nullptr;

				// generated visibility: a conservative tome over the generated surfaces (and the reflection probe
				// instances, whose vis bits come from the query as well)
				if (std::getenv("IWZ_GEN_TOME"))
				{
					std::vector<std::uint32_t> user_ids;
					for (auto k = 0u; k < own; k++) user_ids.push_back(k);
					const auto probes = std::getenv("IWZ_TOME_NO_PROBES") ? 0u : w->draw.reflectionProbeData.reflectionProbeInstanceCount;
					for (auto p = 0u; p < probes; p++) user_ids.push_back(0x50000000u + p);
					const auto tome = build_conservative_tome(user_ids, src.mins, src.maxs);
					w->umbraTomeSize = static_cast<unsigned int>(tome.size());
					w->umbraTomeData = mem->allocate<char>(tome.size());
					std::memcpy(w->umbraTomeData, tome.data(), tome.size());
					ZONETOOL_INFO("iwzmap: generated conservative Umbra tome (%u bytes, %u surfaces, %u probe instances)",
						w->umbraTomeSize, own, probes);
					if (const auto* out = std::getenv("IWZ_DUMP_TOME")) utils::io::write_file(std::string(out) + ".gen.tome", tome);
				}
			}

			ZONETOOL_INFO("template dpvsDyn: clients %u %u words %u %u", w->dpvsDyn.dynEntClientCount[0], w->dpvsDyn.dynEntClientCount[1],
				w->dpvsDyn.dynEntClientWordCount[0], w->dpvsDyn.dynEntClientWordCount[1]);
			// no dyn entities (MapEnts has none): the renderer walks dynEntClientCount client entries
			for (auto a = 0; a < 2 && std::getenv("IWZ_CLEAR_DYNENTS"); a++)
			{
				w->dpvsDyn.dynEntClientCount[a] = 0;
				w->dpvsDyn.dynEntClientWordCount[a] = 0;
			}

			// indices
			w->draw.indexCount = static_cast<unsigned int>(src.indices.size());
			w->draw.indices = mem->allocate<unsigned short>(src.indices.size());
			std::memcpy(w->draw.indices, src.indices.data(), src.indices.size() * sizeof(std::uint16_t));

			// single transient zone with the vertices and a one-node aabb tree per cell
			w->draw.transientZoneCount = 1;
			w->draw.transientZones[0] = name_stub<GfxWorldTransientZone>(mem, map);
			// the client reads transient zone slots beyond the count; point every slot at zone 0
			for (auto i = 1; i < 32; i++) w->draw.transientZones[i] = std::getenv("IWZ_TZ_NULL") ? nullptr : w->draw.transientZones[0];

			tz->name = mem->duplicate_string(map);
			tz->transientZoneIndex = 0;
			tz->vertexCount = static_cast<unsigned int>(src.vertices.size());
			tz->vd.vertices = mem->allocate<GfxWorldVertex>(src.vertices.size());
			std::memcpy(tz->vd.vertices, src.vertices.data(), src.vertices.size() * sizeof(GfxWorldVertex));
			// a non-empty (zeroed) vertex layer stream: the renderer creates a buffer from it
			tz->vertexLayerDataSize = static_cast<unsigned int>(src.vertices.size() * 16);
			tz->vld.data = mem->allocate<unsigned char>(tz->vertexLayerDataSize);
			// slot mode keeps the template aabb trees (Umbra returns their nodes)
			if (!slot_mode)
			{
				// one aabb tree per cell, each a single leaf over the generated surfaces
				const auto cells = static_cast<unsigned int>(w->dpvsPlanes.cellCount);
				tz->cellCount = cells;
				tz->aabbTreeCounts = mem->allocate<GfxCellTreeCount>(cells);
				tz->aabbTrees = mem->allocate<GfxCellTree>(cells);
				for (auto c = 0u; c < cells; c++)
				{
					tz->aabbTreeCounts[c].aabbTreeCount = 1;
					tz->aabbTrees[c].aabbTree = mem->allocate<GfxAabbTree>(1);
					auto& node = tz->aabbTrees[c].aabbTree[0];
					node.bounds = world_bounds;
					node.childCount = 0;
					node.surfaceCount = static_cast<unsigned short>(own);
					node.startSurfIndex = slot_mode ? 0 : tpl_count;
					node.smodelIndexCount = 0;
					node.smodelIndexes = nullptr;
					node.childrenOffset = 0;
				}
				if (keep_vis)
				{
					for (auto c = 0u; c < cells; c++) w->cellTransientInfos[c].aabbTreeIndex = 0;
				}
			}
		}

		void build_clip(clipMap_t* c, const std::string& bsp, const std::string& hkx, zone_memory* mem)
		{
			c->name = mem->duplicate_string(bsp);
			c->numStaticModels = 0;
			c->staticModelList = nullptr;
			c->staticModelCollisionModelList.numModels = 0;
			c->staticModelCollisionModelList.staticModelIndex = nullptr;
			c->numStaticModelCollisionModelLists = 0;
			c->staticModelCollisionModelLists = nullptr;
			c->mapEnts = name_stub<MapEnts>(mem, bsp);
			c->havokWorldShapeDataSize = static_cast<unsigned int>(hkx.size());
			c->havokWorldShapeData = mem->allocate<char>(hkx.size());
			std::memcpy(c->havokWorldShapeData, hkx.data(), hkx.size());
			c->numCollisionHeatmapEntries = 0;
			c->collisionHeatmap = nullptr;
			// keep the template stages: the client reads stage 0 for scene lighting (a null list crashes it)
		}

		float park(float z) { return std::getenv("IWZ_NO_PARK") ? z : -60000.0f; }

		void add_triggers(MapEnts* e, const std::string& text, std::string& ents, zone_memory* mem)
		{
			struct gen { unsigned int contents; std::vector<std::array<float, 3>> halves; };
			std::vector<gen> list;
			std::istringstream in(text);
			for (std::string line; std::getline(in, line);)
			{
				std::istringstream ls(line);
				gen g{};
				if (!(ls >> std::hex >> g.contents >> std::dec)) continue;
				for (std::array<float, 3> h{}; ls >> h[0] >> h[1] >> h[2];) g.halves.push_back(h);
				if (!g.halves.empty()) list.push_back(g);
			}
			if (list.empty()) return;
			const auto base = e->trigger.count, hull_base = e->trigger.hullCount;
			auto hulls_total = hull_base;
			for (const auto& g : list) hulls_total += static_cast<unsigned int>(g.halves.size());
			auto* models = mem->allocate<TriggerModel>(base + list.size());
			std::memcpy(models, e->trigger.models, base * sizeof(TriggerModel));
			auto* hulls = mem->allocate<TriggerHull>(hulls_total);
			std::memcpy(hulls, e->trigger.hulls, hull_base * sizeof(TriggerHull));
			auto h = hull_base;
			for (auto k = 0u; k < list.size(); k++)
			{
				auto& tm = models[base + k];
				tm = {};
				tm.contents = list[k].contents;
				tm.hullCount = static_cast<unsigned short>(list[k].halves.size());
				tm.firstHull = static_cast<unsigned short>(h);
				for (const auto& half : list[k].halves)
				{
					auto& hull = hulls[h++];
					hull = {};
					for (auto a = 0; a < 3; a++) hull.bounds.halfSize[a] = half[a];
					hull.contents = list[k].contents;
				}
				const auto tag = "\"?IWZ" + std::to_string(k) + "\"";
				const auto real = "\"?" + std::to_string(base + k) + "\"";
				for (auto pos = ents.find(tag); pos != std::string::npos; pos = ents.find(tag, pos + real.size()))
					ents.replace(pos, tag.size(), real);
			}
			// IWZ_TRIGGER_SHAPES: script volume tests (ispointinvolume / istouching) use the trigger model's entity physics
			// shape, which generated models lack. Give each generated model the shape of an unused single-hull template box
			// trigger (unique box size in the entity shape data) and resize that box (its half extents are stored / 32,
			// three times per box shape) to the generated hull.
			if (std::getenv("IWZ_TRIGGER_SHAPES") && e->havokEntsShapeData && e->havokEntsShapeDataSize)
			{
				auto* blob = reinterpret_cast<unsigned char*>(e->havokEntsShapeData);
				const auto size = static_cast<size_t>(e->havokEntsShapeDataSize);
				auto find_all = [&](const float* v) {
					std::vector<size_t> at;
					for (size_t o = 0; o + 12 <= size; o += 4)
					{
						float f[3];
						std::memcpy(f, blob + o, 12);
						if (std::fabs(f[0] - v[0]) < 1e-3f && std::fabs(f[1] - v[1]) < 1e-3f && std::fabs(f[2] - v[2]) < 1e-3f) at.push_back(o);
					}
					return at;
				};
				std::vector<std::pair<float, unsigned int>> donors;
				for (auto m = 0u; m < base; m++)
				{
					const auto& tm = models[m];
					if (tm.hullCount != 1 || (tm.contents & 0xFF000000) != 0x28000000) continue;
					const auto& b = hulls[tm.firstHull].bounds;
					if (b.midPoint[0] != 0.0f || b.midPoint[1] != 0.0f || b.midPoint[2] != 0.0f) continue;
					const float v[3] = {b.halfSize[0] / 32.0f, b.halfSize[1] / 32.0f, b.halfSize[2] / 32.0f};
					if (find_all(v).size() != 3) continue;
					donors.emplace_back(-(b.halfSize[0] * b.halfSize[1] * b.halfSize[2]), m);
				}
				// the engine keeps a broadphase box of the donor's original size (engine run g8: points beyond it fail), so
				// each generated volume takes the smallest unused donor that covers it on every axis, largest volumes first
				std::sort(donors.begin(), donors.end());
				std::vector<unsigned int> order(list.size());
				for (auto k = 0u; k < list.size(); k++) order[k] = k;
				std::sort(order.begin(), order.end(), [&](unsigned int a, unsigned int b) {
					const auto& ha = hulls[models[base + a].firstHull].bounds.halfSize;
					const auto& hb = hulls[models[base + b].firstHull].bounds.halfSize;
					return std::max({ha[0], ha[1], ha[2]}) > std::max({hb[0], hb[1], hb[2]});
				});
				std::vector<bool> taken(donors.size(), false);
				for (const auto k : order)
				{
					const auto& gen_half = hulls[models[base + k].firstHull].bounds.halfSize;
					auto pick = -1;
					for (auto i = static_cast<int>(donors.size()) - 1; i >= 0; i--)     // ascending volume
					{
						if (taken[i]) continue;
						const auto& donor_half = hulls[models[donors[i].second].firstHull].bounds.halfSize;
						if (donor_half[0] >= gen_half[0] && donor_half[1] >= gen_half[1] && donor_half[2] >= gen_half[2]) { pick = i; break; }
					}
					if (pick < 0)
						for (auto i = 0; i < static_cast<int>(donors.size()); i++) if (!taken[i]) { pick = i; break; }
					if (pick < 0) break;
					taken[pick] = true;
					const auto d = donors[pick].second;
					auto& tm = models[base + k];
					const auto& db = hulls[models[d].firstHull].bounds;
					const float from[3] = {db.halfSize[0] / 32.0f, db.halfSize[1] / 32.0f, db.halfSize[2] / 32.0f};
					const auto& g = hulls[tm.firstHull].bounds;
					const float to[3] = {g.halfSize[0] / 32.0f, g.halfSize[1] / 32.0f, g.halfSize[2] / 32.0f};
					const auto at = find_all(from);
					for (const auto o : at) std::memcpy(blob + o, to, 12);
					tm.physicsAsset = models[d].physicsAsset;
					tm.physicsShapeOverrideIdx = models[d].physicsShapeOverrideIdx;
					tm.flags = models[d].flags;
					ZONETOOL_INFO("iwzmap: generated trigger ?%u takes the entity shape of template ?%u (shape %u, half %.0f %.0f %.0f -> %.0f %.0f %.0f, %zu patches)",
						base + k, d, tm.physicsShapeOverrideIdx, db.halfSize[0], db.halfSize[1], db.halfSize[2], g.halfSize[0], g.halfSize[1], g.halfSize[2], at.size());
				}
				if (donors.size() < list.size()) ZONETOOL_WARNING("iwzmap: only %zu template box shapes for %zu generated triggers", donors.size(), list.size());
			}
			e->trigger.models = models;
			e->trigger.count = base + static_cast<unsigned int>(list.size());
			e->trigger.hulls = hulls;
			e->trigger.hullCount = hulls_total;
			ZONETOOL_INFO("iwzmap: %zu generated trigger models (?%u..?%u)", list.size(), base, base + static_cast<unsigned int>(list.size()) - 1);
		}

		void build_ents(MapEnts* e, const std::string& bsp, std::string ents, const std::string& triggers, const std::string& relocations,
			map_ents* strings, const world_source& src, zone_memory* mem)
		{
			add_triggers(e, triggers, ents, mem);
			ZONETOOL_INFO("template triggers: %u models %u hulls %u slabs %u windings; client %u models", e->trigger.count,
				e->trigger.hullCount, e->trigger.slabCount, e->trigger.windingCount, e->clientTrigger.trigger.count);
			for (auto i = 0u; i < e->trigger.count; i++)
			{
				if (!std::getenv("IWZ_LIST_TRIGGERS") && i != 21) continue;
				if (i >= e->trigger.count) continue;
				const auto& tm = e->trigger.models[i];
				const auto& hb = e->trigger.hulls[tm.firstHull].bounds;
				ZONETOOL_INFO("trigger model %u: contents %x hulls %u first %u windings %u flags %x hull0 mid %.0f %.0f %.0f half %.0f %.0f %.0f slabs %u",
					i, tm.contents, tm.hullCount, tm.firstHull, tm.windingCount, tm.flags, hb.midPoint[0], hb.midPoint[1], hb.midPoint[2],
					hb.halfSize[0], hb.halfSize[1], hb.halfSize[2], e->trigger.hulls[tm.firstHull].slabCount);
			}
			// one sub-model (the world, *0) to match the single GfxWorld brush model, unless IWZ_KEEP_SUBMODELS keeps the
			// template sub-models (collision-only script_brushmodels; see build_gfx). Their template nav obstacle index is
			// kept with the template obstacle hulls (IWZ_SUBMODEL_NAVOBST=keep) or cleared (default: no obstacle).
			if (e->numSubModels > 1 && !std::getenv("IWZ_KEEP_ENTS") && !std::getenv("IWZ_KEEP_SUBMODELS"))
			{
				e->numSubModels = 1;
			}
			if (std::getenv("IWZ_KEEP_SUBMODELS") && e->cmodels)
			{
				const auto* mode = std::getenv("IWZ_SUBMODEL_NAVOBST");
				const auto keep_obst = mode && std::string(mode) == "keep";
				for (auto i = 0u; i < e->numSubModels; i++)
				{
					auto& cm = e->cmodels[i];
					ZONETOOL_INFO("iwzmap: sub-model *%u mid %.1f %.1f %.1f half %.1f %.1f %.1f radius %.1f shape %u navobst %u edge %u info %p physics %s",
						i, cm.bounds.midPoint[0], cm.bounds.midPoint[1], cm.bounds.midPoint[2], cm.bounds.halfSize[0], cm.bounds.halfSize[1],
						cm.bounds.halfSize[2], cm.radius, cm.physicsShapeOverrideIdx, cm.navObstacleIdx, cm.edgeFirstIndex,
						static_cast<void*>(cm.info), cm.physicsAsset && cm.physicsAsset->name ? cm.physicsAsset->name : "-");
					if (i && !keep_obst) cm.navObstacleIdx = 0xFFFF;
				}
			}
			if (e->cmodels)
			{
				e->cmodels[0].bounds = make_bounds(src.mins, src.maxs);
				e->cmodels[0].radius = std::sqrt(e->cmodels[0].bounds.halfSize[0] * e->cmodels[0].bounds.halfSize[0] +
					e->cmodels[0].bounds.halfSize[1] * e->cmodels[0].bounds.halfSize[1] +
					e->cmodels[0].bounds.halfSize[2] * e->cmodels[0].bounds.halfSize[2]);
			}

			e->name = mem->duplicate_string(bsp);
			e->numEntityChars = static_cast<int>(ents.size() + 1);
			e->entityString = mem->allocate<char>(ents.size() + 1);
			std::memcpy(e->entityString, ents.data(), ents.size());

			if (std::getenv("IWZ_KEEP_ENTS"))
			{
				return; // bisection aid: template lists untouched
			}

			// nothing of the template may appear in the new world: clear each list with its pointers,
			// so that the writer and the engine loader agree on an empty list
			// template script triggers are kept: clipmap stages index them
			if (!std::getenv("IWZ_KEEP_CLIENTTRIGGERS")) std::memset(&e->clientTrigger, 0, sizeof(e->clientTrigger));
			std::memset(&e->clientTriggerBlend, 0, sizeof(e->clientTriggerBlend));
			if (!std::getenv("IWZ_KEEP_SPAWNLIST")) std::memset(&e->spawnList, 0, sizeof(e->spawnList));
			std::memset(&e->splineList, 0, sizeof(e->splineList));
			ZONETOOL_INFO("template mapents: dynEnt %u %u total %u reserved %u %u (free %d %d) scriptables %u/%u/%u transientGroups %u",
				e->dynEntCount[0], e->dynEntCount[1], e->dynEntCountTotal,
				e->scriptableMapEnts.reservedDynents[0].numReservedDynents, e->scriptableMapEnts.reservedDynents[1].numReservedDynents,
				e->scriptableMapEnts.reservedDynents[0].freeHead, e->scriptableMapEnts.reservedDynents[1].freeHead,
				e->scriptableMapEnts.totalInstanceCount, e->scriptableMapEnts.runtimeInstanceCount, e->scriptableMapEnts.reservedInstanceCount,
				e->dynEntTransientGroupCount);
			// template entity physics shapes describe the template brush sub-models (trimmed above)
			if (!std::getenv("IWZ_KEEP_ENTSHAPES"))
			{
				e->havokEntsShapeDataSize = 0;
				e->havokEntsShapeData = nullptr;
			}
			if (std::getenv("IWZ_CLEAR_DYNENTS"))
			{
				e->dynEntCount[0] = e->dynEntCount[1] = 0;
				e->dynEntCountTotal = 0;
				std::memset(e->dynEntDefList, 0, sizeof(e->dynEntDefList));
				std::memset(e->dynEntPoseList, 0, sizeof(e->dynEntPoseList));
				// the client reads entry 0 of each runtime client list even with no dyn entities: keep one inactive entry
				for (auto a = 0; a < 2; a++) for (auto b = 0; b < 2; b++) e->dynEntClientList[a][b] = mem->allocate<DynEntityClient>(1);
				std::memset(e->dynEntGlobalIdList, 0, sizeof(e->dynEntGlobalIdList));
				e->dynEntTransientGroupCount = 0;
				e->dynEntTransientGroups = nullptr;
				std::memset(e->dynEntTransientGroupRuntime, 0, sizeof(e->dynEntTransientGroupRuntime));
				std::memset(e->dynEntTransientGroupState, 0, sizeof(e->dynEntTransientGroupState));
			}
			e->unk3Count = 0;
			e->unk3 = nullptr;
			e->clientEntAnchorCount = 0;
			e->clientEntAnchors = nullptr;
			// the engine pools runtime dyn entities (scriptable parts, debris) out of the map's dyn entity set:
			// the render world counts, the MapEnts lists and the scriptable reservations must stay coherent.
			// keep the template set, parked far below the playable space (spawn flags kept: the enabled ones serve the reservations)
			ScriptableReservedDynents reserved[2] = {e->scriptableMapEnts.reservedDynents[0], e->scriptableMapEnts.reservedDynents[1]};
			const auto scriptables = e->scriptableMapEnts;
			std::memset(&e->scriptableMapEnts, 0, sizeof(e->scriptableMapEnts));
			// the scriptable instance pool is required (runtime scriptables are allocated from it; without it the
			// client stops taking snapshots after the first one). Keep it; park the template's placed instances
			// out of sight and unnamed so no Spaceland prop appears or answers a script lookup.
			if (!std::getenv("IWZ_CLEAR_SCRIPTABLES"))
			{
				e->scriptableMapEnts = scriptables;
				// scriptables.txt: "<targetname> <n-th instance of that name> x y z yaw" moves a template scriptable
				// (perk machine, upgrade station...) into the generated world with its name and behaviour
				struct move { std::string name; unsigned int nth; float origin[3]; float yaw; bool used; };
				std::vector<move> moves;
				{
					std::istringstream in(relocations);
					for (std::string line; std::getline(in, line);)
					{
						std::istringstream ls(line);
						move mv{};
						if (ls >> mv.name >> mv.nth >> mv.origin[0] >> mv.origin[1] >> mv.origin[2] >> mv.yaw) moves.push_back(mv);
					}
				}
				std::unordered_map<std::string, unsigned int> seen_names;
				auto placed = 0u, relocated = 0u;
				for (auto i = 0u; i < e->scriptableMapEnts.totalInstanceCount && e->scriptableMapEnts.instances; i++)
				{
					auto& inst = e->scriptableMapEnts.instances[i];
					if (!inst.contextHeader.context.def)
					{
						if (std::getenv("IWZ_LIST_SCRIPTABLES"))
						{
							const auto* n = strings ? strings->get_script_string(&inst.targetname) : nullptr;
							ZONETOOL_INFO("template scriptable without def #%u \"%s\" at %.0f %.0f %.0f", i, n ? n : "",
								inst.contextHeader.context.origin[0], inst.contextHeader.context.origin[1], inst.contextHeader.context.origin[2]);
						}
						continue;
					}
					placed++;
					const auto* tn = strings ? strings->get_script_string(&inst.targetname) : nullptr;
					std::string name = tn ? tn : "";
					if (std::getenv("IWZ_LIST_SCRIPTABLES"))
					{
						const auto* dn = strings ? strings->get_script_string(&inst.debugNameScr) : nullptr;
						ZONETOOL_INFO("template scriptable #%u tn \"%s\" debug \"%s\" / \"%s\" def %s at %.0f %.0f %.0f", i, name.data(),
							inst.debugName ? inst.debugName : "", dn ? dn : "", inst.contextHeader.context.def->name,
							inst.contextHeader.context.origin[0], inst.contextHeader.context.origin[1], inst.contextHeader.context.origin[2]);
					}
					const auto nth = name.empty() ? 0u : seen_names[name]++;
					move* mv = nullptr;
					for (auto& m : moves) if (!m.used && m.name == name && m.nth == nth) { mv = &m; break; }
					auto place = [&](ScriptableInstanceContext& c)
					{
						if (mv)
						{
							for (auto a = 0; a < 3; a++) c.initialOrigin[a] = c.origin[a] = mv->origin[a];
							c.initialAngles[0] = c.angles[0] = 0.0f;
							c.initialAngles[1] = c.angles[1] = mv->yaw;
							c.initialAngles[2] = c.angles[2] = 0.0f;
						}
						else c.initialOrigin[2] = c.origin[2] = -60000.0f;
					};
					place(inst.contextHeader.context);
					place(inst.contextHeaderLocalClient[0].context);
					place(inst.contextHeaderLocalClient[1].context);
					if (mv) { mv->used = true; relocated++; }
					else if (strings) strings->set_script_string(&inst.targetname, "");
				}
				for (const auto& m : moves)
					if (!m.used) ZONETOOL_WARNING("iwzmap: scriptable \"%s\" #%u not found in the template", m.name.data(), m.nth);
				ZONETOOL_INFO("iwzmap: relocated %u template scriptables", relocated);
				if (std::getenv("IWZ_LIST_SCRIPTABLES"))
				{
					for (const auto& [n, c] : seen_names) ZONETOOL_INFO("template scriptable \"%s\" x%u", n.data(), c);
				}
				ZONETOOL_INFO("iwzmap: scriptables total %u runtime %u reserved %u; parked %u placed template instances",
					e->scriptableMapEnts.totalInstanceCount, e->scriptableMapEnts.runtimeInstanceCount,
					e->scriptableMapEnts.reservedInstanceCount, placed);
			}
			if (!std::getenv("IWZ_CLEAR_DYNENTS"))
			{
				// keep only the dyn entities the scriptable pool reserves, renumbered, parked out of sight,
				// with no transient group: the rest of the template set describes Spaceland props
				// the disabled defs form the pool runtime dyn entities (scriptable parts, debris) are taken from
				std::vector<unsigned int> keep;
				std::unordered_map<unsigned int, unsigned int> remap;
				auto transient = 0, active = 0;
				for (auto i = 0u; i < e->dynEntCount[0]; i++)
				{
					const auto& d = e->dynEntDefList[0][i];
					if (d.isTransient) transient++;
					if (d.spawnEnabled || d.spawnActive) active++;
					remap[i] = static_cast<unsigned int>(keep.size());
					keep.push_back(i);
				}
				ZONETOOL_INFO("iwzmap: template dyn defs %u (transient %d, active %d)", e->dynEntCount[0], transient, active);
				const auto n = static_cast<unsigned short>(keep.size());
				auto* defs = mem->allocate<DynEntityDef>(n);
				DynEntityPose* poses[2] = {mem->allocate<DynEntityPose>(n), mem->allocate<DynEntityPose>(n)};
				DynEntityGlobalId* ids[2] = {mem->allocate<DynEntityGlobalId>(n), mem->allocate<DynEntityGlobalId>(n)};
				for (auto i = 0u; i < n; i++)
				{
					defs[i] = e->dynEntDefList[0][keep[i]];
					defs[i].initialPose.origin[2] = park(defs[i].initialPose.origin[2]);
					defs[i].pose.origin[2] = park(defs[i].pose.origin[2]);
					defs[i].brushModel = 0;
					defs[i].linkTo = nullptr;
					defs[i].isTransient = false;
					defs[i].transientZoneLoaded = false;
					defs[i].transientIndexStored = 0;
					defs[i].distantShadows = false;
					for (auto p = 0; p < 2; p++)
					{
						if (e->dynEntPoseList[p][0]) poses[p][i] = e->dynEntPoseList[p][0][keep[i]];
						poses[p][i].pose.origin[2] = park(poses[p][i].pose.origin[2]);
						if (poses[p][i].poses)
						{
							auto* copy = mem->allocate<GfxPlacement>(poses[p][i].numPoses);
							for (auto k = 0u; k < poses[p][i].numPoses; k++) { copy[k] = poses[p][i].poses[k]; copy[k].origin[2] = park(copy[k].origin[2]); }
							poses[p][i].poses = copy;
						}
						if (e->dynEntGlobalIdList[p]) ids[p][i] = e->dynEntGlobalIdList[p][keep[i]];
					}
				}
				for (auto c = 0; c < 2; c++)
				{
					auto& r = e->scriptableMapEnts.reservedDynents[c];
					r = reserved[c];
					if (!r.reservedDynents) continue;
					auto* list = mem->allocate<ScriptableReservedDynent>(r.numReservedDynents);
					for (auto k = 0u; k < r.numReservedDynents; k++)
					{
						list[k] = reserved[c].reservedDynents[k];
						list[k].dynentId = remap.contains(list[k].dynentId) ? remap[list[k].dynentId] : 0;
					}
					r.reservedDynents = list;
				}
				e->dynEntCount[0] = n;
				e->dynEntCount[1] = 0;
				e->dynEntCountTotal = n;
				e->dynEntDefList[0] = defs;
				e->dynEntDefList[1] = nullptr;
				e->dynEntPoseList[0][0] = poses[0];
				e->dynEntPoseList[1][0] = poses[1];
				e->dynEntPoseList[0][1] = e->dynEntPoseList[1][1] = nullptr;
				for (auto a = 0; a < 2; a++) for (auto c = 0; c < 2; c++) e->dynEntClientList[a][c] = mem->allocate<DynEntityClient>(c ? 1 : std::max<unsigned short>(n, 1));
				e->dynEntGlobalIdList[0] = ids[0];
				e->dynEntGlobalIdList[1] = ids[1];
				e->dynEntTransientGroupCount = 0;
				e->dynEntTransientGroups = nullptr;
				std::memset(e->dynEntTransientGroupRuntime, 0, sizeof(e->dynEntTransientGroupRuntime));
				std::memset(e->dynEntTransientGroupState, 0, sizeof(e->dynEntTransientGroupState));
				ZONETOOL_INFO("iwzmap: kept %u reserved dyn entities (of the template set)", n);
			}
			e->numMayhemScenes = 0;
			e->mayhemScenes = nullptr;
			std::memset(&e->spawners, 0, sizeof(e->spawners));
			e->audioPASpeakerCount = 0;
			e->audioPASpeakers = nullptr;
		}

		void build_nav(NavMeshData* n, const std::string& bsp, const std::string& graph, zone_memory* mem)
		{
			n->name = mem->duplicate_string(bsp);
			n->numNavResources = 1;
			auto& r = n->navResources[0];
			r.graphSize = static_cast<int>(graph.size());
			r.pGraphBuffer = mem->allocate<char>(graph.size());
			std::memcpy(r.pGraphBuffer, graph.data(), graph.size());
			r.bDockable = false;
			const auto* mode = std::getenv("IWZ_SUBMODEL_NAVOBST");
			ZONETOOL_INFO("iwzmap: template nav obstacle bounds %d (%s)", n->numObstacleBounds,
				std::getenv("IWZ_KEEP_SUBMODELS") && mode && std::string(mode) == "keep" ? "kept" : "cleared");
			for (auto i = 0; i < n->numObstacleBounds && std::getenv("IWZ_KEEP_SUBMODELS"); i++)
			{
				const auto& ob = n->obstacleBounds[i];
				for (auto h = 0; h < ob.m_NumHulls; h++)
					ZONETOOL_INFO("iwzmap: nav obstacle %d hull %d: %d planes, bounds %.1f %.1f %.1f offset %.1f %.1f %.1f use %d", i, h,
						ob.m_Hulls[h].m_NumBoundaries, ob.m_Hulls[h].m_BoundsX, ob.m_Hulls[h].m_BoundsY, ob.m_Hulls[h].m_BoundsZ,
						ob.m_Hulls[h].m_Offset[0], ob.m_Hulls[h].m_Offset[1], ob.m_Hulls[h].m_Offset[2], ob.m_Hulls[h].m_bUseBounds);
			}
			if (!(std::getenv("IWZ_KEEP_SUBMODELS") && mode && std::string(mode) == "keep"))
			{
				n->numObstacleBounds = 0;
				n->obstacleBounds = nullptr;
			}
			n->numGlassBounds = 0;
			n->glassBounds = nullptr;
			n->numModifiers = 0;
			n->modifiers = nullptr;
			n->numLinkCreationData = 0;
			n->linkCreationData = nullptr;
		}
	}

	bool compile(const std::string& template_zone, const std::string& template_map, const std::string& map,
		const std::string& source)
	{
		try
		{
			const auto tpl_bsp = "maps/cp/" + template_map + ".d3dbsp";
			const auto bsp = "maps/cp/" + map + ".d3dbsp";
			const auto src = read_world(source + "\\world.bin");

			std::string hkx, graph, ents, triggers, relocations;
			if (!utils::io::read_file(source + "\\colmap.hkx", &hkx)) throw std::runtime_error("missing colmap.hkx");
			if (!utils::io::read_file(source + "\\navgraph.bin", &graph)) throw std::runtime_error("missing navgraph.bin");
			if (!utils::io::read_file(source + "\\ents.txt", &ents)) throw std::runtime_error("missing ents.txt");
			utils::io::read_file(source + "\\triggers.txt", &triggers);
			utils::io::read_file(source + "\\scriptables.txt", &relocations);

			auto mem = std::make_unique<zone_memory>(1024ull * 1024 * 1024);
			filesystem::set_fastfile(template_zone);

			gfx_world gfx;
			auto* w = gfx.parse(tpl_bsp, mem.get());
			gfx_world_tr gfx_tr;
			auto* tz = gfx_tr.parse(template_map, mem.get());
			clip_map clip;
			auto* c = clip.parse(tpl_bsp, mem.get());
			com_world com;
			auto* cw = com.parse(tpl_bsp, mem.get());
			map_ents me;
			auto* e = me.parse(tpl_bsp, mem.get());
			nav_mesh nav;
			auto* n = nav.parse(tpl_bsp, mem.get());
			glass_world glass;
			auto* gw = glass.parse(tpl_bsp, mem.get());
			fx_world fx;
			auto* fw = fx.parse(tpl_bsp, mem.get());
			path_data paths;
			auto* pd = paths.parse(tpl_bsp, mem.get());

			if (w) log_template(w); // also for an inspection-only template (e.g. a world without nav)
			if (w && std::getenv("IWZ_DUMP_TOME"))
			{
				// inspection: raw visibility data of the template (tome, second tome, extra block)
				const std::string base = std::getenv("IWZ_DUMP_TOME");
				ZONETOOL_INFO("template umbra: gates %u tome %u tome2 %u (gates2 %u) unk %u", w->numUmbraGates, w->umbraTomeSize,
					w->umbraTomeSize2, w->numUmbraGates2, w->umbraUnkSize);
				if (w->umbraTomeData) utils::io::write_file(base + ".tome", std::string(w->umbraTomeData, w->umbraTomeSize));
				if (w->umbraTomeData2) utils::io::write_file(base + ".tome2", std::string(w->umbraTomeData2, w->umbraTomeSize2));
				if (w->umbraUnkData) utils::io::write_file(base + ".unk", std::string(w->umbraUnkData, w->umbraUnkSize));
			}
			if (!w || !tz || !c || !cw || !e || !n || !gw || !fw)
			{
				throw std::runtime_error("template world assets are incomplete");
			}

			if (std::getenv("IWZ_KEEP_GFX"))
			{
				// bisection aid: template render world, renamed only
				log_template(w);
				w->name = mem->duplicate_string(bsp);
				w->baseName = mem->duplicate_string(map);
				for (auto i = 0u; i < w->draw.transientZoneCount; i++)
				{
					w->draw.transientZones[i] = name_stub<GfxWorldTransientZone>(mem.get(), map);
				}
				tz->name = mem->duplicate_string(map);
				if (std::getenv("IWZ_INJECT_SURFACES"))
				{
					// bisection aid: the stock render world plus the generated geometry written into template
					// surface slots over the generated world (vertices/indices appended to the stock buffers)
					const auto tv = tz->vertexCount, ti = w->draw.indexCount;
					ZONETOOL_INFO("template buffers: %u vertices, %u indices, %u layer bytes (%.2f per vertex)", tv, ti,
						tz->vertexLayerDataSize, tv ? double(tz->vertexLayerDataSize) / tv : 0.0);
					auto* verts = mem->allocate<GfxWorldVertex>(tv + src.vertices.size());
					std::memcpy(verts, tz->vd.vertices, tv * sizeof(GfxWorldVertex));
					std::memcpy(verts + tv, src.vertices.data(), src.vertices.size() * sizeof(GfxWorldVertex));
					tz->vd.vertices = verts;
					tz->vertexCount = tv + static_cast<unsigned int>(src.vertices.size());
					auto* idx = mem->allocate<unsigned short>(ti + src.indices.size());
					std::memcpy(idx, w->draw.indices, ti * sizeof(unsigned short));
					std::memcpy(idx + ti, src.indices.data(), src.indices.size() * sizeof(unsigned short));
					w->draw.indices = idx;
					w->draw.indexCount = ti + static_cast<unsigned int>(src.indices.size());
					std::vector<std::pair<float, unsigned int>> cand;
					for (auto i = w->dpvs.litOpaqueSurfsBegin; i < w->dpvs.litOpaqueSurfsEnd; i++)
					{
						const auto& b = w->dpvs.surfacesBounds[i].bounds;
						bool overlaps = true;
						for (auto a = 0; a < 3; a++)
							if (b.midPoint[a] + b.halfSize[a] < src.mins[a] || b.midPoint[a] - b.halfSize[a] > src.maxs[a]) overlaps = false;
						if (overlaps) cand.emplace_back(-(b.halfSize[0] * b.halfSize[1]), i);
					}
					std::sort(cand.begin(), cand.end());
					for (auto k = 0u; k < src.surfaces.size() && k < cand.size(); k++)
					{
						const auto i = cand[k].second;
						const auto& g = src.surfaces[k];
						auto& surf = w->dpvs.surfaces[i];
						surf.material = name_stub<Material>(mem.get(), src.materials[g.material]);
						surf.tris.firstVertex = tv + g.first_vertex;
						surf.tris.vertexCount = static_cast<unsigned short>(g.vertex_count);
						surf.tris.triCount = static_cast<unsigned short>(g.tri_count);
						surf.tris.baseIndex = ti + g.base_index;
						ZONETOOL_INFO("slot %u had lightmap %u flags %02x", i, surf.lightmapIndex, surf.flags);
						surf.lightmapIndex = 0;
						ZONETOOL_INFO("injected generated surface %u into template slot %u (%s)", k, i, src.materials[g.material].data());
					}
				}
			}
			else
			{
				build_gfx(w, tz, src, map, bsp, mem.get());
				const auto zones = read_lighting(source + "\\lighting.txt", src);
				if (!zones.empty())
				{
					auto& grid = w->lightGrid.probeData;
					const auto a = relight_probes(grid.probePositions, grid.probes, grid.probeCount, zones);
					const auto b = relight_probes(grid.gpuVisibleProbePositions, grid.gpuVisibleProbesData, grid.gpuVisibleProbesCount, zones);
					ZONETOOL_INFO("iwzmap: relit %u light grid probes (%u gpu visible) from %zu lighting zones", a, b, zones.size());
				}
			}
			build_clip(c, bsp, hkx, mem.get());
			build_ents(e, bsp, ents, triggers, relocations, &me, src, mem.get());
			build_nav(n, bsp, graph, mem.get());
			if (!std::getenv("IWZ_CLEAR_DYNENTS"))
			{
				w->dpvsDyn.dynEntClientCount[0] = e->dynEntCount[0];
				w->dpvsDyn.dynEntClientWordCount[0] = (e->dynEntCount[0] + 31u) / 32u;
				w->dpvsDyn.dynEntClientCount[1] = 0;
				w->dpvsDyn.dynEntClientWordCount[1] = 0;
			}

			cw->name = mem->duplicate_string(bsp);
			gw->name = mem->duplicate_string(bsp);
			if (gw->g_glassData)
			{
				gw->g_glassData->pieceCount = 0;
				gw->g_glassData->glassPieces = nullptr;
				gw->g_glassData->glassNameCount = 0;
				gw->g_glassData->glassNames = nullptr;
			}
			fw->name = mem->duplicate_string(bsp);
			fw->glassSys.defCount = 0;
			fw->glassSys.defs = nullptr;
			fw->glassSys.initPieceCount = 0;
			fw->glassSys.initGeoDataCount = 0;
			fw->glassSys.lightingHandles = nullptr;
			fw->glassSys.initPieceStates = nullptr;
			fw->glassSys.initGeoData = nullptr;
			if (pd) pd->name = mem->duplicate_string(bsp);

			// baked lightmap (source\lightmap\): template image headers are read before switching to the new map
			const auto baked = std::getenv("IWZ_KEEP_GFX") ? baked_lightmap{} : prepare_lightmap(w, source, mem.get());
			filesystem::set_fastfile(map);
			gfx_world::dump(w);
			dump_lightmap(baked);
			gfx_world_tr::dump(tz);
			clip_map::dump(c);
			com_world::dump(cw);
			map_ents::dump_string_source = &me;
			map_ents::dump(e);
			map_ents::dump_string_source = nullptr;
			nav_mesh::dump(n);
			glass_world::dump(gw);
			fx_world::dump(fw);
			if (pd) path_data::dump(pd);

			ZONETOOL_INFO("iwzmap: wrote %s (%zu surfaces, %zu vertices, %zu indices, %zu-byte collision, %zu-byte nav graph)",
				bsp.data(), src.surfaces.size(), src.vertices.size(), src.indices.size(), hkx.size(), graph.size());
			mem->free();
			return true;
		}
		catch (const std::exception& ex)
		{
			ZONETOOL_ERROR("iwzmap: %s", ex.what());
			return false;
		}
	}
}
