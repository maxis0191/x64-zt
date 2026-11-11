#include <std_include.hpp>
#include "zonetool/iw7/zonetool.hpp"
#include "iwzmap.hpp"
#include "iwzmap_lightmap.hpp"
#include "iwzmap_smodel_rules.hpp"

#include <array>
#include <bit>
#include <functional>
#include <cmath>
#include <limits>
#include <map>
#include <set>
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
#include "zonetool/iw7/assets/xmodel.hpp"

#include <utils/io.hpp>

#include "iwzmap_volumes.hpp"

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

		// Static models (smodels.txt): "<xmodel> x y z pitch yaw roll scale [noshadow] [nocollision] [cull=<units>]"
		// ('#' starts a comment). Each line becomes a GfxWorld static model (light grid lit), an Umbra object
		// (class 1) and, when the model has a collision shape, a clipmap static model.
		// The XModel is read from the template dump or, for a model the template map does not carry, from xmodel/ next to
		// smodels.txt (the zone must then carry the model: kit import). Engine-tested (iwz-stage2/smodels runs 3):
		// the clipmap static model blocks bullet traces only; physicstrace, playerphysicstrace, player movement (and
		// so AI) go through it, as for stock props, whose player blocking comes from clip geometry. The generator adds
		// that clip to the world Havok mesh and cuts the navmesh from smodels.bounds.json (written next to the
		// sources by every compile, or by -iwzbounds).
		struct smodel_source
		{
			std::string model;
			float origin[3]{};
			float angles[3]{};
			float scale = 1.0f;
			bool no_shadow = false;
			bool no_collision = false;
			int cull = -1;
			XModel* xmodel = nullptr; // parsed from the template dump (bounds, lods, collision)
			float axis[3][3]{};       // forward, left, up (unscaled)
			float mins[3]{};          // world-space bounds
			float maxs[3]{};
			bool collides = false;
		};

		void angles_to_axis(const float* angles, float axis[3][3])
		{
			constexpr auto rad = 3.14159265358979323846 / 180.0;
			const auto s_p = std::sin(angles[0] * rad), c_p = std::cos(angles[0] * rad);
			const auto s_y = std::sin(angles[1] * rad), c_y = std::cos(angles[1] * rad);
			const auto s_r = std::sin(angles[2] * rad), c_r = std::cos(angles[2] * rad);
			// forward, right (AngleVectors), left = -right, up
			const double f[3] = {c_p * c_y, c_p * s_y, -s_p};
			const double r[3] = {-s_r * s_p * c_y + c_r * s_y, -s_r * s_p * s_y - c_r * c_y, -s_r * c_p};
			const double u[3] = {c_r * s_p * c_y + s_r * s_y, c_r * s_p * s_y - s_r * c_y, c_r * c_p};
			for (auto a = 0; a < 3; a++)
			{
				axis[0][a] = static_cast<float>(f[a]);
				axis[1][a] = static_cast<float>(-r[a]);
				axis[2][a] = static_cast<float>(u[a]);
			}
		}

		// world bounds of a model-space box placed with axis * scale + origin
		void transform_bounds(const Bounds& local, const float axis[3][3], float scale, const float* origin, float* mins, float* maxs)
		{
			for (auto a = 0; a < 3; a++)
			{
				auto mid = origin[a], half = 0.0f;
				for (auto k = 0; k < 3; k++)
				{
					mid += axis[k][a] * scale * local.midPoint[k];
					half += std::fabs(axis[k][a] * scale) * local.halfSize[k];
				}
				mins[a] = mid - half;
				maxs[a] = mid + half;
			}
		}

		bool model_has_collision(const XModel* m)
		{
			return m && ((m->physicsLODData && m->physicsLODDataSize) || m->physicsAsset || m->numCollSurfs);
		}

		std::vector<smodel_source> read_smodels(const std::string& path, zone_memory* mem, bool required = false)
		{
			std::vector<smodel_source> list;
			std::string text;
			if (!utils::io::read_file(path, &text))
			{
				if (required) throw std::runtime_error("cannot read " + path);
				return list;
			}
			std::unordered_map<std::string, XModel*> models;
			// a model that is not in the template dump (kit model of another map, authored model) is read from
			// <folder of smodels.txt>/xmodel/<name>.xmb (+ <name>_lod*.hkx): the creator SDK links it there; the zone
			// step gets the model itself from the kit import. The search paths are restored on every exit.
			struct source_path
			{
				std::vector<std::string> saved = filesystem::get_search_paths();
				explicit source_path(const std::string& dir) { if (!dir.empty()) filesystem::add_path(dir, true); }
				~source_path() { filesystem::get_search_paths() = saved; }
			} scope(std::filesystem::path(path).parent_path().string());
			std::istringstream in(text);
			auto line_no = 0;
			for (std::string line; std::getline(in, line);)
			{
				line_no++;
				if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
				std::istringstream l(line);
				smodel_source s;
				if (!(l >> s.model)) continue;
				if (!(l >> s.origin[0] >> s.origin[1] >> s.origin[2] >> s.angles[0] >> s.angles[1] >> s.angles[2] >> s.scale))
					throw std::runtime_error("smodels.txt line " + std::to_string(line_no) + ": expected <xmodel> x y z pitch yaw roll scale");
				for (std::string flag; l >> flag;)
				{
					if (flag == "noshadow") s.no_shadow = true;
					else if (flag == "nocollision") s.no_collision = true;
					else if (flag.starts_with("cull=")) s.cull = std::stoi(flag.substr(5));
					else throw std::runtime_error("smodels.txt line " + std::to_string(line_no) + ": unknown flag " + flag);
				}
				if (!(s.scale > 0.0f)) throw std::runtime_error("smodels.txt line " + std::to_string(line_no) + ": scale must be positive");
				if (!models.contains(s.model))
				{
					xmodel parser;
					models[s.model] = parser.parse(s.model, mem);
					if (!models[s.model]) ZONETOOL_WARNING("iwzmap: static model %s is neither in the template dump nor in xmodel/ next to smodels.txt; skipped", s.model.data());
				}
				s.xmodel = models[s.model];
				if (!s.xmodel) continue;
				angles_to_axis(s.angles, s.axis);
				transform_bounds(s.xmodel->bounds, s.axis, s.scale, s.origin, s.mins, s.maxs);
				s.collides = !s.no_collision && model_has_collision(s.xmodel);
				list.push_back(s);
			}
			if (list.size() > 0xFFFF) throw std::runtime_error("more than 65535 static models");
			return list;
		}

		// footprints for the nav generator and for verification: per model its model-space bounds and collision,
		// per instance its world bounds
		std::string smodel_bounds_json(const std::vector<smodel_source>& list)
		{
			std::ostringstream o;
			o.setf(std::ios::fixed);
			o.precision(3);
			o << "{\n \"schema\": \"iw7.iwzmap-smodel-bounds/1\",\n \"models\": {";
			std::map<std::string, const XModel*> models;
			for (const auto& s : list) models[s.model] = s.xmodel;
			auto first = true;
			for (const auto& [name, m] : models)
			{
				const auto& b = m->bounds;
				o << (first ? "\n" : ",\n") << "  \"" << name << "\": {\"mins\": [" << b.midPoint[0] - b.halfSize[0] << ", " << b.midPoint[1] - b.halfSize[1]
					<< ", " << b.midPoint[2] - b.halfSize[2] << "], \"maxs\": [" << b.midPoint[0] + b.halfSize[0] << ", " << b.midPoint[1] + b.halfSize[1] << ", "
					<< b.midPoint[2] + b.halfSize[2] << "], \"radius\": " << m->radius << ", \"collision\": " << (model_has_collision(m) ? "true" : "false")
					<< ", \"physicsLOD\": " << m->physicsLODDataSize << ", \"collSurfs\": " << m->numCollSurfs << ", \"lods\": " << static_cast<int>(m->numLods) << "}";
				first = false;
			}
			o << "\n },\n \"instances\": [";
			for (auto i = 0u; i < list.size(); i++)
			{
				const auto& s = list[i];
				o << (i ? ",\n" : "\n") << "  {\"index\": " << i << ", \"model\": \"" << s.model << "\", \"origin\": [" << s.origin[0] << ", " << s.origin[1] << ", " << s.origin[2]
					<< "], \"angles\": [" << s.angles[0] << ", " << s.angles[1] << ", " << s.angles[2] << "], \"scale\": " << s.scale << ", \"mins\": [" << s.mins[0] << ", "
					<< s.mins[1] << ", " << s.mins[2] << "], \"maxs\": [" << s.maxs[0] << ", " << s.maxs[1] << ", " << s.maxs[2] << "], \"collision\": "
					<< (s.collides ? "true" : "false") << "}";
			}
			o << "\n ]\n}\n";
			return o.str();
		}

		// Primary lights (lights.txt): "omni x y z r g b intensity radius [shadow]" or
		// "spot x y z r g b intensity radius pitch yaw fov [shadow]" ('#' comment). The light colour is rgb * intensity
		// (stock IW7 lights store radiance this way; stock values are in the thousands). Each light takes the place of
		// a static template light of the same kind (its arrays stay coherent), moved and re-shaped to the new light.
		// cp_zmb offers about 860 static spot and 140 static omni slots. Lights reach shading through their class-4
		// tome object and the regenerated static light voxel tree (build_voxel_tree). "shadow" picks a
		// shadow-capable slot and lists shadow casters (surfaces/static models in the sphere): not engine-verified.
		struct light_source
		{
			bool spot = false;
			float origin[3]{};
			float rgb[3]{};
			float intensity = 0.0f;
			float radius = 0.0f;
			float pitch = 90.0f;
			float yaw = 0.0f;
			float fov = 90.0f;
			bool shadow = false;
			unsigned int slot = 0;
			// script-switchable light (name=<targetname> [group=<script_noteworthy>]): takes a scriptable slot and
			// gets a light_spot map entity, so level script can setlightintensity it (power-on lighting)
			std::string name;
			std::string group;
		};

		std::vector<light_source> read_lights(const std::string& path)
		{
			std::vector<light_source> list;
			std::string text;
			if (!utils::io::read_file(path, &text)) return list;
			std::istringstream in(text);
			auto line_no = 0;
			for (std::string line; std::getline(in, line);)
			{
				line_no++;
				if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
				std::istringstream l(line);
				std::string kind;
				if (!(l >> kind)) continue;
				light_source s;
				if (kind != "omni" && kind != "spot") throw std::runtime_error("lights.txt line " + std::to_string(line_no) + ": expected omni or spot");
				s.spot = kind == "spot";
				if (!(l >> s.origin[0] >> s.origin[1] >> s.origin[2] >> s.rgb[0] >> s.rgb[1] >> s.rgb[2] >> s.intensity >> s.radius))
					throw std::runtime_error("lights.txt line " + std::to_string(line_no) + ": expected x y z r g b intensity radius");
				if (s.spot && !(l >> s.pitch >> s.yaw >> s.fov))
					throw std::runtime_error("lights.txt line " + std::to_string(line_no) + ": a spot needs pitch yaw fov");
				// optional tail: [shadow] [name=<targetname>] [group=<script_noteworthy>]
				for (std::string tok; l >> tok;)
				{
					if (tok.rfind("name=", 0) == 0) s.name = tok.substr(5);
					else if (tok.rfind("group=", 0) == 0) s.group = tok.substr(6);
					else if (tok == "0" || tok == "1") s.shadow = tok == "1";
					else throw std::runtime_error("lights.txt line " + std::to_string(line_no) + ": unknown token " + tok);
				}
				if (!(s.radius > 0.0f)) throw std::runtime_error("lights.txt line " + std::to_string(line_no) + ": radius must be positive");
				list.push_back(s);
			}
			return list;
		}

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
					// every template probe: tetrahedra around the generated rooms may reach far outside them
					const auto margin = std::getenv("IWZ_AMBIENT_NEAR_ONLY") ? 512.0f : 1.0e7f;
					for (auto a = 0; a < 3; a++) { z.mins[a] = src.mins[a] - margin; z.maxs[a] = src.maxs[a] + margin; }
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

		// Reflection probes (owner/coordinator 2026-11-08: pale Spaceland glyphs on the generated ceilings). The world
		// keeps the template's probe instances and cube array, so generated surfaces reflect cp_zmb's signage. With
		// IWZ_PROBES=neutral every lighting.txt room gets its own probe instance (its box as the volume, its centre as
		// the capture point) whose cube is a constant colour: the room's ambient rgb times IWZ_PROBES_SCALE (default
		// 0.25), i.e. a dim neutral environment matching the room. Instances beyond the rooms are parked far outside the
		// world. Needs an unstreamed R16G16B16A16_FLOAT cube array (element-major subresources); anything else is logged
		// and left alone. IWZ_PROBES_NO_RELIGHT=1 also drops the runtime relighting table (template GBuffer relight).
		// BC6H_UF16 mode 11 (mode bits 00011): endpoints rw,gw,bw then rx,gx,bx (10 bits each), then 63 index bits.
		// Equal endpoints and zero indices give a constant colour. Unsigned 10-bit unquantisation: unq = (q << 16 |
		// 0x8000) >> 10, half = unq * 31 >> 6, so q = round((half - 15.5) / 31).
		std::array<std::uint8_t, 16> bc6h_constant(const std::uint16_t half_rgb[3])
		{
			std::array<std::uint8_t, 16> block{};
			unsigned int bit = 0;
			auto put = [&](std::uint32_t value, unsigned int count)
			{
				for (auto i = 0u; i < count; i++, bit++)
					if ((value >> i) & 1u) block[bit >> 3] |= static_cast<std::uint8_t>(1u << (bit & 7u));
			};
			put(0x03u, 5);
			std::uint32_t q[3];
			for (auto c = 0; c < 3; c++)
			{
				const auto h = static_cast<double>(half_rgb[c] & 0x7FFFu);
				q[c] = static_cast<std::uint32_t>(std::clamp(std::lround((h - 15.5) / 31.0), 0L, 1022L));
			}
			for (auto e = 0; e < 2; e++) for (auto c = 0; c < 3; c++) put(q[c], 10);
			return block;      // remaining 63 index bits stay 0
		}

		GfxImage* g_probe_image = nullptr;      // modified cube array, dumped into the new map (after set_fastfile)

		unsigned int neutralize_reflection_probes(GfxWorld* w, const std::vector<ambient_zone>& zones, const world_source& src, zone_memory* mem)
		{
			const auto* mode = std::getenv("IWZ_PROBES");
			if (!mode || std::string(mode) != "neutral") return 0;
			auto& rp = w->draw.reflectionProbeData;
			auto* img = rp.reflectionProbeArrayImage;
			if (img && !img->pixelData && img->name)
			{
				// the world only references the array by name: read the template image itself (template fastfile is
				// still the active one here), modify it and dump it into the new map with the world
				gfx_image parser;
				auto* full = parser.parse(img->name, mem);
				if (full) img = full;
			}
			std::vector<const ambient_zone*> rooms;
			for (const auto& z : zones) if (z.maxs[0] - z.mins[0] < 1.0e6f) rooms.push_back(&z);
			if (!img || !img->pixelData || img->streamed || rooms.empty())
			{
				ZONETOOL_WARNING("iwzmap: probes: no unstreamed cube array or no rooms (image %p, streamed %d, rooms %zu): left as is",
					static_cast<void*>(img), img ? img->streamed : -1, rooms.size());
				return 0;
			}
			const auto fmt = static_cast<int>(img->imageFormat);
			// R16G16B16A16_FLOAT arrays store one face per element; BC6H_UF16 (cp_zmb) one whole cube (6 faces with mips)
			const auto cubes = static_cast<unsigned int>(static_cast<int>(img->imageFormat) == 95 ? img->numElements : img->numElements / 6u);
			const auto total = static_cast<std::size_t>(img->dataLen1);
			ZONETOOL_INFO("iwzmap: probes: array %s %ux%u format %d elements %u levels %u bytes %zu; %u probes, %u instances, %u relight",
				img->name, img->width, img->height, fmt, img->numElements, img->levelCount, total, rp.reflectionProbeCount,
				rp.reflectionProbeInstanceCount, rp.probeRelightingCount);
			const auto per_check = img->numElements ? total / img->numElements : 0;
			if ((fmt != 10 && fmt != 95) || img->numElements == 0 || total % img->numElements != 0 || per_check % (fmt == 95 ? 16 : 8) != 0)
			{
				ZONETOOL_WARNING("iwzmap: probes: unexpected array layout (format %d, %zu bytes for %u elements): left as is", fmt, total, img->numElements);
				return 0;
			}
			const auto per = total / img->numElements;
			const auto scale = std::getenv("IWZ_PROBES_SCALE") ? static_cast<float>(std::atof(std::getenv("IWZ_PROBES_SCALE"))) : 0.25f;
			const auto n = std::min<std::size_t>({ rooms.size(), static_cast<std::size_t>(cubes), static_cast<std::size_t>(rp.reflectionProbeInstanceCount) });
			for (auto k = 0u; k < n; k++)
			{
				const auto& z = *rooms[k];
				std::uint16_t px[4];
				for (auto c = 0; c < 3; c++) px[c] = float_to_half(z.rgb[c] * scale);
				px[3] = float_to_half(1.0f);
				if (fmt == 95)
				{
					// one element = one cube (6 faces, all mips): every 16-byte block the same constant block
					const auto block = bc6h_constant(px);
					auto* base = img->pixelData + static_cast<std::size_t>(k) * per;
					for (std::size_t o = 0; o + 16 <= per; o += 16) std::memcpy(base + o, block.data(), 16);
				}
				else
				{
					for (auto f = 0u; f < 6u; f++)
					{
						auto* base = img->pixelData + (static_cast<std::size_t>(k) * 6u + f) * per;
						for (std::size_t o = 0; o + 8 <= per; o += 8) std::memcpy(base + o, px, 8);
					}
				}
				auto& inst = rp.reflectionProbeInstances[k];
				float size[3];
				for (auto a = 0; a < 3; a++)
				{
					inst.probePosition[a] = 0.5f * (z.mins[a] + z.maxs[a]);
					inst.volumeObb.center[a] = inst.probePosition[a];
					size[a] = z.maxs[a] - z.mins[a];
					inst.volumeObb.halfSize[a] = 0.5f * size[a];
					inst.volumeObb.xAxis[a] = a == 0 ? 1.0f : 0.0f;
					inst.volumeObb.yAxis[a] = a == 1 ? 1.0f : 0.0f;
					inst.volumeObb.zAxis[a] = a == 2 ? 1.0f : 0.0f;
					inst.feather[a] = 16.0f;
					inst.expandProjectionNeg[a] = 0.0f;
					inst.expandProjectionPos[a] = 0.0f;
				}
				inst.probeRotation[0] = inst.probeRotation[1] = inst.probeRotation[2] = 0.0f;
				inst.probeRotation[3] = 1.0f;
				inst.probeImageIndex = static_cast<unsigned short>(k);
				// smaller rooms win where boxes overlap (stairs inside the hall's margin)
				inst.priority = 1.0e9f / std::max(1.0f, size[0] * size[1] * size[2]) + 1.0f;
				ZONETOOL_INFO("iwzmap: probes: room %u box %.0f %.0f %.0f .. %.0f %.0f %.0f -> cube %u rgb %.4f %.4f %.4f", k,
					z.mins[0], z.mins[1], z.mins[2], z.maxs[0], z.maxs[1], z.maxs[2], k, z.rgb[0] * scale, z.rgb[1] * scale, z.rgb[2] * scale);
			}
			auto parked = 0u;
			for (auto i = static_cast<unsigned int>(n); i < rp.reflectionProbeInstanceCount; i++)
			{
				auto& inst = rp.reflectionProbeInstances[i];
				for (auto a = 0; a < 3; a++) { inst.volumeObb.center[a] = 1.0e6f + 256.0f * i; inst.volumeObb.halfSize[a] = 1.0f; }
				inst.priority = 0.0f;
				parked++;
			}
			// any template instance whose volume still touches the generated world (should be 0)
			auto overlapping = 0u;
			for (auto i = static_cast<unsigned int>(n); i < rp.reflectionProbeInstanceCount; i++)
			{
				const auto& o = rp.reflectionProbeInstances[i].volumeObb;
				bool hit = true;
				for (auto a = 0; a < 3; a++) if (o.center[a] + o.halfSize[a] < src.mins[a] || o.center[a] - o.halfSize[a] > src.maxs[a]) hit = false;
				if (hit) overlapping++;
			}
			if (std::getenv("IWZ_PROBES_NO_RELIGHT")) { rp.probeRelightingCount = 0; }
			g_probe_image = img;
			ZONETOOL_INFO("iwzmap: probes: neutral rooms %zu of %zu (cubes %u), parked %u template instances, template overlapping the world %u, relight %u",
				n, rooms.size(), cubes, parked, overlapping, rp.probeRelightingCount);
			return static_cast<unsigned int>(n);
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

		std::string hex_bytes(const void* data, std::size_t size)
		{
			std::string s;
			char b[4];
			for (std::size_t i = 0; i < size; i++)
			{
				std::snprintf(b, sizeof(b), "%02x", static_cast<const std::uint8_t*>(data)[i]);
				s += b;
				if ((i & 3) == 3) s += ' ';
			}
			return s;
		}

		// Static light voxel tree (Forward+ light binning of the static spot/omni primary lights), as read from cp_zmb:
		// - header: root node grid dimensions (x, y, z), node coordinate bit shifts {9, 7, 5} (512, 128, 32 units),
		//   bound min/max of the root grid;
		// - one top-down node per root column: first internal node index of the column's lowest root and its root
		//   z range (roots of one column are consecutive internal nodes; outside the range there is no light);
		// - internal node: 4x4x4 children as a 64-bit mask in two words; word w's children are numbered from
		//   firstNodeIndex[w] in bit order; an index with bit 31 set is a leaf, else an internal node (child size =
		//   parent size / 4). Internal node 0 and leaf 0 are reserved;
		// - leaf: u16 address into lightListArray; a list is a header (spot count << 7 | omni count) followed by the
		//   spot then the omni light indices. Address 2 is the empty list.
		// The column order and the bit -> axis order are verified against the template's light positions
		// (IWZ_LOG_STATICS: "voxel: hypothesis"); voxel_layout holds the verified one: on cp_zmb, x-major columns and
		// bit fields (x, y, z) put 433021 of 461483 listed light refs inside their light sphere (the stock baker bins a
		// little wider), y-major 0 and the other bit orders at most 285924.
		// The light AABB tree (lightAABB) holds exactly the lights with needsDynamicShadows (170 on cp_zmb); the
		// generated lights have it off, so it is left as is.
		struct voxel_layout
		{
			int column_x_major = 1; // top-down index = x + y * dimX
			int perm = 0;           // bit fields (b & 3, b >> 2 & 3, b >> 4) -> axes {perm table}
		};

		constexpr int voxel_perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

		template <typename F>
		void voxel_visit(const GfxVoxelTree& t, const voxel_layout& lay, F&& leaf)
		{
			const auto* h = t.voxelTreeHeader;
			const int dim[3] = {h->rootNodeDimension[0], h->rootNodeDimension[1], h->rootNodeDimension[2]};
			const auto root = static_cast<float>(1 << h->nodeCoordBitShift[0]);
			const auto* fields = voxel_perms[lay.perm];
			std::function<void(int, const float*, float)> walk = [&](int node, const float* org, float size)
			{
				if (node <= 0 || node >= t.voxelInternalNodeCount) return;
				const auto& in = t.voxelInternalNodeArray[node];
				const auto child = size / 4.0f;
				for (auto b = 0; b < 64; b++)
				{
					const auto w = b >> 5;
					const auto bit = static_cast<unsigned>(b & 31);
					const auto mask = static_cast<unsigned>(in.childNodeMask[w]);
					if (!(mask & (1u << bit))) continue;
					const auto rank = std::popcount(mask & ((1u << bit) - 1u));
					const auto first = static_cast<unsigned>(in.firstNodeIndex[w]);
					const int f[3] = {b & 3, (b >> 2) & 3, (b >> 4) & 3};
					float o[3] = {org[0], org[1], org[2]};
					for (auto k = 0; k < 3; k++) o[fields[k]] += f[k] * child;
					const auto idx = (first & 0x7FFFFFFFu) + rank;
					if (first & 0x80000000u) leaf(static_cast<int>(idx), o, child);
					else walk(static_cast<int>(idx), o, child);
				}
			};
			for (auto k = 0; k < t.voxelTopDownViewNodeCount; k++)
			{
				const auto& td = t.voxelTopDownViewNodeArray[k];
				if (td.zMax < td.zMin || (!td.firstNodeIndex && !td.zMin && !td.zMax)) continue;
				const auto tx = lay.column_x_major ? k % dim[0] : k / dim[1];
				const auto ty = lay.column_x_major ? k / dim[0] : k % dim[1];
				for (auto z = td.zMin; z <= td.zMax; z++)
				{
					const float org[3] = {h->boundMin[0] + tx * root, h->boundMin[1] + ty * root, h->boundMin[2] + z * root};
					walk(td.firstNodeIndex + (z - td.zMin), org, root);
				}
			}
		}

		bool box_touches_sphere(const float* mins, float size, const float* c, float r)
		{
			auto d2 = 0.0f;
			for (auto a = 0; a < 3; a++)
			{
				const auto v = std::clamp(c[a], mins[a], mins[a] + size);
				d2 += (v - c[a]) * (v - c[a]);
			}
			return d2 <= r * r;
		}

		// scores every layout hypothesis: share of (leaf, listed light) pairs whose leaf box touches the light sphere
		void voxel_hypotheses(const GfxVoxelTree& t, const ComWorld* cw)
		{
			for (auto major = 0; major < 2; major++)
			{
				for (auto p = 0; p < 6; p++)
				{
					voxel_layout lay{major, p};
					std::uint64_t ok = 0, total = 0, leaves = 0;
					voxel_visit(t, lay, [&](int leaf, const float* o, float size)
					{
						if (leaf <= 0 || leaf >= t.voxelLeafNodeCount) return;
						const auto addr = t.voxelLeafNodeArray[leaf].lightListAddress;
						if (addr >= static_cast<unsigned>(t.lightListArraySize)) return;
						const auto hdr = t.lightListArray[addr];
						const auto n = (hdr >> 7) + (hdr & 0x7F);
						leaves++;
						for (auto q = 0; q < n && addr + 1 + q < t.lightListArraySize; q++)
						{
							const auto li = t.lightListArray[addr + 1 + q];
							if (li >= cw->primaryLightCount) continue;
							const auto& l = cw->primaryLights[li];
							total++;
							if (box_touches_sphere(o, size, l.origin, l.radius + 1.0f)) ok++;
						}
					});
					ZONETOOL_INFO("voxel: hypothesis column %s perm %d%d%d: %llu / %llu light refs inside the light sphere (%llu leaves)",
						major ? "x-major" : "y-major", voxel_perms[p][0], voxel_perms[p][1], voxel_perms[p][2], ok, total, leaves);
				}
			}
		}

		// inspection of the template's static models, their collision and the primary lights (IWZ_LOG_STATICS)
		void log_statics(const GfxWorld* w, const GfxWorldTransientZone* tz, const clipMap_t* c, const ComWorld* cw)
		{
			const auto& d = w->dpvs;
			ZONETOOL_INFO("statics: smodels %u visData %u surfVis %u lightVis %u probeVis %u volVis %u decalVis %u usage %d sunOpt %u sunSurfVis %u",
				d.smodelCount, d.smodelVisDataCount, d.surfaceVisDataCount, d.primaryLightVisDataCount, d.reflectionProbeVisDataCount,
				d.volumetricVisDataCount, d.decalVisDataCount, d.usageCount, d.sunShadowOptCount, d.sunSurfVisDataCount);
			std::map<unsigned, unsigned> flag_hist, lod_hist;
			for (auto i = 0u; i < d.smodelCount; i++)
			{
				flag_hist[d.smodelDrawInsts[i].flags & 0xFFF0]++;
				if (d.lodData) lod_hist[d.lodData[i]]++;
			}
			for (const auto& [f, n] : flag_hist) ZONETOOL_INFO("statics: flags %04x x%u", f, n);
			auto lods = 0;
			for (const auto& [f, n] : lod_hist) if (lods++ < 12) ZONETOOL_INFO("statics: lodData %08x x%u", f, n);
			if (d.lodData) ZONETOOL_INFO("statics: lodData[count] %08x", d.lodData[d.smodelCount]);
			{
				std::string s;
				for (auto i = 0u; i < std::min(d.smodelCount, 24u) && d.sortedSmodelIndices; i++) s += std::to_string(d.sortedSmodelIndices[i]) + " ";
				ZONETOOL_INFO("statics: sortedSmodelIndices %s", s.data());
			}
			std::vector<unsigned> picks = {0, 1, 2, 3, 100, 1000, 5000, 10000};
			for (auto i = 0u, grid = 0u, vert = 0u; i < d.smodelCount; i++)
			{
				const auto f = d.smodelDrawInsts[i].flags;
				if ((f & STATIC_MODEL_FLAG_LIGHTGRID_LIGHTING) && grid++ < 4) picks.push_back(i);
				if ((f & STATIC_MODEL_FLAG_VERTEXLIT_LIGHTING) && vert++ < 2) picks.push_back(i);
			}
			for (const auto i : picks)
			{
				if (i >= d.smodelCount) continue;
				const auto& dinst = d.smodelDrawInsts[i];
				const auto& sinst = d.smodelInsts[i];
				const auto& p = dinst.placement;
				ZONETOOL_INFO("smodel %u %s at %.1f %.1f %.1f scale %.3f axis0 %.3f %.3f %.3f flags %04x cull %u lightingHandle %u lmIdx %d nLV %u vlFlags %x env %u probe %u skin %u sun %u tz %u",
					i, dinst.model ? dinst.model->name : "-", p.origin[0], p.origin[1], p.origin[2], p.scale, p.axis[0][0], p.axis[0][1], p.axis[0][2],
					dinst.flags, dinst.cullDist, dinst.lightingHandle, dinst.modelLightmapInfo.lightmapIndex, dinst.vertexLightingInfo.numLightingValues,
					dinst.vertexLightingInfo.flags, dinst.primaryLightEnvIndex, dinst.reflectionProbeIndex, dinst.firstMtlSkinIndex, dinst.sunShadowFlags, dinst.transientZone);
				ZONETOOL_INFO("smodel %u inst mid %.1f %.1f %.1f half %.1f %.1f %.1f lightingOrigin %.1f %.1f %.1f lod %08x", i, sinst.bounds.midPoint[0], sinst.bounds.midPoint[1],
					sinst.bounds.midPoint[2], sinst.bounds.halfSize[0], sinst.bounds.halfSize[1], sinst.bounds.halfSize[2], sinst.lightingOrigin[0], sinst.lightingOrigin[1], sinst.lightingOrigin[2],
					d.lodData ? d.lodData[i] : 0);
				ZONETOOL_INFO("smodel %u tail @%zu: %s", i, offsetof(GfxStaticModelDrawInst, modelLightmapInfo),
					hex_bytes(reinterpret_cast<const char*>(&dinst) + offsetof(GfxStaticModelDrawInst, modelLightmapInfo), sizeof(dinst) - offsetof(GfxStaticModelDrawInst, modelLightmapInfo)).data());
				ZONETOOL_INFO("smodel %u vli: %s", i, hex_bytes(&dinst.vertexLightingInfo, sizeof(dinst.vertexLightingInfo)).data());
			}
			// relations of the unnamed draw instance fields
			if (d.sortedSmodelIndices && d.lodData)
			{
				std::vector<unsigned> pos(d.smodelCount);
				for (auto k = 0u; k < d.smodelCount; k++) pos[d.sortedSmodelIndices[k]] = k;
				auto u9pos = 0u, lodpos = 0u, u9lod = 0u, lodlt = 0u;
				std::map<unsigned, unsigned> h2, h3, h10, h11, hsun, hprobe, hskin, h13, h14, hunk21;
				auto handle_nz = 0u, env_nz = 0u, u5_nz = 0u, u6_nz = 0u;
				std::vector<std::pair<unsigned, unsigned>> ranges;
				std::set<unsigned> lod_distinct, u15_distinct;
				for (auto i = 0u; i < d.smodelCount; i++)
				{
					const auto& dinst = d.smodelDrawInsts[i];
					if (dinst.unk9 == pos[i]) u9pos++;
					if (d.lodData[i] == pos[i]) lodpos++;
					if (d.lodData[i] == dinst.unk9) u9lod++;
					if (d.lodData[i] < d.lodData[d.smodelCount]) lodlt++;
					lod_distinct.insert(d.lodData[i]);
					std::uint32_t u15;
					std::memcpy(&u15, dinst.unk15, 4);
					u15_distinct.insert(u15);
					h2[dinst.unk2]++; h3[dinst.unk3]++; h10[dinst.unk10]++; h11[dinst.unk11]++; hsun[dinst.sunShadowFlags]++;
					hprobe[dinst.reflectionProbeIndex]++; hskin[dinst.firstMtlSkinIndex]++; h13[dinst.unk13]++; h14[dinst.unk14]++; hunk21[dinst.unk21]++;
					if (dinst.lightingHandle) handle_nz++;
					if (dinst.primaryLightEnvIndex) env_nz++;
					if (dinst.unk5) u5_nz++;
					if (dinst.unk6[0] || dinst.unk6[1] || dinst.unk6[2] || dinst.unk6[3]) u6_nz++;
					ranges.emplace_back(dinst.unk0 | (static_cast<unsigned>(dinst.unk1) << 16), i);
				}
				std::sort(ranges.begin(), ranges.end());
				auto contiguous = 0u, overlap = 0u;
				for (auto k = 0u; k + 1 < ranges.size(); k++)
				{
					const auto end = ranges[k].first + d.smodelDrawInsts[ranges[k].second].unk3;
					if (end == ranges[k + 1].first) contiguous++;
					if (end > ranges[k + 1].first) overlap++;
				}
				const auto& last = ranges.back();
				ZONETOOL_INFO("rel: unk9==sortedPos %u lod==sortedPos %u lod==unk9 %u lod<lod[n] %u lodDistinct %zu u15Distinct %zu; ranges first %u last end %u contiguous %u overlap %u",
					u9pos, lodpos, u9lod, lodlt, lod_distinct.size(), u15_distinct.size(), ranges.front().first,
					last.first + d.smodelDrawInsts[last.second].unk3, contiguous, overlap);
				ZONETOOL_INFO("rel: lightingHandle!=0 %u env!=0 %u unk5!=0 %u unk6!=0 %u", handle_nz, env_nz, u5_nz, u6_nz);
				auto dump_hist = [](const char* name, const std::map<unsigned, unsigned>& h)
				{
					std::string s;
					auto n = 0;
					for (const auto& [v, c] : h) { if (n++ < 16) s += std::to_string(v) + ":" + std::to_string(c) + " "; }
					ZONETOOL_INFO("rel: %s (%zu values) %s", name, h.size(), s.data());
				};
				dump_hist("unk2", h2); dump_hist("unk3", h3); dump_hist("unk10", h10); dump_hist("unk11", h11); dump_hist("sun", hsun);
				dump_hist("probe", hprobe); dump_hist("skin", hskin); dump_hist("unk13", h13); dump_hist("unk14", h14); dump_hist("unk21", hunk21);
				// per model: unk2/unk3/unk11 against the XModel lods
				std::set<std::string> seen;
				for (auto i = 0u; i < d.smodelCount && seen.size() < 12; i++)
				{
					const auto& dinst = d.smodelDrawInsts[i];
					if (!dinst.model || seen.contains(dinst.model->name)) continue;
					seen.insert(dinst.model->name);
					xmodel parser;
					zone_memory scratch(64ull * 1024 * 1024);
					const auto* m = parser.parse(dinst.model->name, &scratch);
					std::string lod_list;
					auto surfs = 0;
					if (m) for (auto l = 0; l < m->numLods; l++) { lod_list += std::to_string(m->lodInfo[l].numsurfs) + "@" + std::to_string(static_cast<int>(m->lodInfo[l].dist)) + " "; surfs += m->lodInfo[l].numsurfs; }
					ZONETOOL_INFO("rel: model %s numLods %d numsurfs %d lods [%s] sum %d | unk2 %u unk3 %u unk11 %u lod %u flags %04x cull %u", dinst.model->name,
						m ? m->numLods : -1, m ? m->numsurfs : -1, lod_list.data(), surfs, dinst.unk2, dinst.unk3, dinst.unk11, d.lodData[i], dinst.flags, dinst.cullDist);
					scratch.free();
				}
			}
			// full table of the template static models for offline analysis (IWZ_DUMP_SMODELS=<csv path>)
			if (const auto* path = std::getenv("IWZ_DUMP_SMODELS"))
			{
				std::vector<unsigned> pos(d.smodelCount);
				if (d.sortedSmodelIndices) for (auto k = 0u; k < d.smodelCount; k++) pos[d.sortedSmodelIndices[k]] = k;
				std::ostringstream o;
				o << "i,model,x,y,z,scale,flags,cull,lod,sortedPos,unk0,unk1,unk2,unk3,handle,unk5,unk9,unk10,unk11,unk13,unk14,unk15,env,probe,skin,sun,tz,unk21,lmIdx,nLV,numLods,numsurfs,radius\n";
				for (auto i = 0u; i < d.smodelCount; i++)
				{
					const auto& tdi = d.smodelDrawInsts[i];
					std::uint32_t u15;
					std::memcpy(&u15, tdi.unk15, 4);
					const auto* m = tdi.model;
					o << i << "," << (m && m->name ? m->name : "-") << "," << tdi.placement.origin[0] << "," << tdi.placement.origin[1] << "," << tdi.placement.origin[2] << ","
						<< tdi.placement.scale << "," << tdi.flags << "," << tdi.cullDist << "," << (d.lodData ? d.lodData[i] : 0) << "," << pos[i] << "," << tdi.unk0 << "," << tdi.unk1
						<< "," << tdi.unk2 << "," << tdi.unk3 << "," << tdi.lightingHandle << "," << tdi.unk5 << "," << tdi.unk9 << "," << tdi.unk10 << "," << tdi.unk11 << ","
						<< static_cast<int>(tdi.unk13) << "," << static_cast<int>(tdi.unk14) << "," << u15 << "," << tdi.primaryLightEnvIndex << "," << static_cast<int>(tdi.reflectionProbeIndex)
						<< "," << static_cast<int>(tdi.firstMtlSkinIndex) << "," << static_cast<int>(tdi.sunShadowFlags) << "," << static_cast<int>(tdi.transientZone) << ","
						<< static_cast<int>(tdi.unk21) << "," << tdi.modelLightmapInfo.lightmapIndex << "," << tdi.vertexLightingInfo.numLightingValues << ","
						<< (m ? static_cast<int>(m->numLods) : -1) << "," << (m ? static_cast<int>(m->numsurfs) : -1) << "," << (m ? m->radius : 0.0f) << "\n";
				}
				o << "lodData[count]," << (d.lodData ? d.lodData[d.smodelCount] : 0) << "\n";
				utils::io::write_file(path, o.str());
				ZONETOOL_INFO("statics: wrote %u static model rows to %s", d.smodelCount, path);
			}
			// aabb tree smodel lists
			if (tz && tz->aabbTrees)
			{
				auto nodes = 0u, refs = 0u;
				for (auto cell = 0u; cell < tz->cellCount; cell++)
					for (auto k = 0; k < tz->aabbTreeCounts[cell].aabbTreeCount; k++) { nodes++; refs += tz->aabbTrees[cell].aabbTree[k].smodelIndexCount; }
				ZONETOOL_INFO("statics: aabb trees %u nodes, %u smodel refs", nodes, refs);
			}
			// collision
			ZONETOOL_INFO("statics: clip numStaticModels %u collision list %u transient lists %u", c->numStaticModels,
				c->staticModelCollisionModelList.numModels, c->numStaticModelCollisionModelLists);
			for (auto k = 0u; k < c->numStaticModelCollisionModelLists && k < 4; k++)
				ZONETOOL_INFO("statics: transient collision list %u: %u models", k, c->staticModelCollisionModelLists[k].numModels);
			{
				std::string s;
				for (auto k = 0u; k < std::min(c->staticModelCollisionModelList.numModels, 24u); k++) s += std::to_string(c->staticModelCollisionModelList.staticModelIndex[k]) + " ";
				ZONETOOL_INFO("statics: collision list head %s", s.data());
			}
			std::map<std::string, unsigned> flag_bytes;
			for (auto k = 0u; k < c->numStaticModels; k++)
			{
				const auto& m = c->staticModelList[k];
				flag_bytes[hex_bytes(&m.unk1, 4) + " pad " + std::to_string(m.pad)]++;
			}
			for (const auto& [f, n] : flag_bytes) ZONETOOL_INFO("statics: clip model flag bytes %s x%u", f.data(), n);
			{
				std::map<std::string, std::pair<unsigned, unsigned>> per_model;
				for (auto k = 0u; k < c->numStaticModels; k++)
				{
					const auto& m = c->staticModelList[k];
					auto& e = per_model[m.xmodel && m.xmodel->name ? m.xmodel->name : "-"];
					(m.unk1 ? e.second : e.first)++;
				}
				std::string flagged;
				for (const auto& [name, e] : per_model) if (e.second) flagged += name + ":" + std::to_string(e.second) + "/" + std::to_string(e.first + e.second) + " ";
				ZONETOOL_INFO("statics: clip models with unk1 set: %s", flagged.data());
			}
			for (const auto k : {0u, 1u, 2u, 100u, 1000u})
			{
				if (k >= c->numStaticModels) continue;
				const auto& m = c->staticModelList[k];
				const auto* ix = m.invScaledAxis;
				ZONETOOL_INFO("clip smodel %u %s at %.1f %.1f %.1f inv %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f bytes %s",
					k, m.xmodel ? m.xmodel->name : "-", m.origin[0], m.origin[1], m.origin[2], ix[0][0], ix[0][1], ix[0][2], ix[1][0], ix[1][1], ix[1][2],
					ix[2][0], ix[2][1], ix[2][2], hex_bytes(&m.unk1, 8).data());
				// the gfx smodel at the same origin
				for (auto i = 0u; i < d.smodelCount; i++)
				{
					const auto* o = d.smodelDrawInsts[i].placement.origin;
					if (std::fabs(o[0] - m.origin[0]) < 0.01f && std::fabs(o[1] - m.origin[1]) < 0.01f && std::fabs(o[2] - m.origin[2]) < 0.01f)
					{
						const auto& p = d.smodelDrawInsts[i].placement;
						ZONETOOL_INFO("clip smodel %u = gfx smodel %u (%s) scale %.3f axis %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f", k, i,
							d.smodelDrawInsts[i].model ? d.smodelDrawInsts[i].model->name : "-", p.scale, p.axis[0][0], p.axis[0][1], p.axis[0][2],
							p.axis[1][0], p.axis[1][1], p.axis[1][2], p.axis[2][0], p.axis[2][1], p.axis[2][2]);
						break;
					}
				}
			}
			// lights
			ZONETOOL_INFO("lights: com %u (scriptable %u first %u envs %u fwd+ %d bake %u) gfx %u lastSun %u moving %u aligned %u motion %u ent %u entShadow %u dynShadow %u %u",
				cw->primaryLightCount, cw->scriptablePrimaryLightCount, cw->firstScriptablePrimaryLight, cw->primaryLightEnvCount, cw->useForwardPlus,
				cw->bakeQuality, w->primaryLightCount, w->lastSunPrimaryLightIndex, w->movingScriptablePrimaryLightCount, w->staticSpotOmniPrimaryLightCountAligned,
				w->primaryLightMotionDetectBitsEntries, w->entityMotionBitsEntries, w->numPrimaryLightEntityShadowVisEntries,
				w->numPrimaryLightDynEntShadowVisEntries[0], w->numPrimaryLightDynEntShadowVisEntries[1]);
			ZONETOOL_INFO("lights: frustum %p views %p voxelTrees %d lightLists size %u aabb nodes %u lights %u heroOnly %u region %p shadowGeom %p",
				w->frustumLights, w->lightViewFrustums, w->voxelTreeCount, w->lightLists.listsSize, w->lightAABB.nodeCount, w->lightAABB.lightCount,
				w->heroOnlyLightCount, w->lightRegion, w->shadowGeomOptimized);
			for (auto v = 0; v < w->voxelTreeCount; v++)
			{
				const auto& t = w->voxelTree[v];
				ZONETOOL_INFO("lights: voxel tree %d topdown %d internal %d leaf %d lightList %d bounds mid %.0f %.0f %.0f half %.0f %.0f %.0f", v,
					t.voxelTopDownViewNodeCount, t.voxelInternalNodeCount, t.voxelLeafNodeCount, t.lightListArraySize, t.zoneBound.midPoint[0],
					t.zoneBound.midPoint[1], t.zoneBound.midPoint[2], t.zoneBound.halfSize[0], t.zoneBound.halfSize[1], t.zoneBound.halfSize[2]);
				if (t.voxelTreeHeader)
					ZONETOOL_INFO("lights: voxel header %s", hex_bytes(t.voxelTreeHeader, sizeof(GfxVoxelTreeHeader)).data());
				std::string s;
				for (auto k = 0; k < std::min(t.lightListArraySize, 32); k++) s += std::to_string(t.lightListArray[k]) + " ";
				ZONETOOL_INFO("lights: voxel lightList head %s", s.data());
				// structure probes (voxel tree format inspection)
				auto pc = [](int v) { return static_cast<unsigned>(std::popcount(static_cast<unsigned>(v))); };
				std::uint64_t m0 = 0;
				for (auto k = 0; k < t.voxelInternalNodeCount; k++)
				{
					const auto& in = t.voxelInternalNodeArray[k];
					m0 += pc(in.childNodeMask[0]) + pc(in.childNodeMask[1]);
				}
				ZONETOOL_INFO("voxel: internal mask popcount total %llu (internal %d leaf %d)", m0, t.voxelInternalNodeCount, t.voxelLeafNodeCount);
				for (auto k = 0; k < std::min(t.voxelInternalNodeCount, 12); k++)
				{
					const auto& in = t.voxelInternalNodeArray[k];
					ZONETOOL_INFO("voxel: internal %d first %d %d mask %08x %08x", k, in.firstNodeIndex[0], in.firstNodeIndex[1], in.childNodeMask[0], in.childNodeMask[1]);
				}
				for (auto k = t.voxelInternalNodeCount - 4; k < t.voxelInternalNodeCount; k++)
				{
					if (k < 0) continue;
					const auto& in = t.voxelInternalNodeArray[k];
					ZONETOOL_INFO("voxel: internal %d first %d %d mask %08x %08x", k, in.firstNodeIndex[0], in.firstNodeIndex[1], in.childNodeMask[0], in.childNodeMask[1]);
				}
				auto shown = 0;
				std::uint64_t zsum = 0;
				for (auto k = 0; k < t.voxelTopDownViewNodeCount; k++)
				{
					const auto& td = t.voxelTopDownViewNodeArray[k];
					if (td.zMax >= td.zMin) zsum += td.zMax - td.zMin + 1;
					if (td.firstNodeIndex >= 0 && shown < 12 && (td.zMin || td.zMax || td.firstNodeIndex))
					{
						ZONETOOL_INFO("voxel: topdown %d (x %d y %d) first %d z %d..%d", k, k % t.voxelTreeHeader->rootNodeDimension[0],
							k / t.voxelTreeHeader->rootNodeDimension[0], td.firstNodeIndex, td.zMin, td.zMax);
						shown++;
					}
				}
				ZONETOOL_INFO("voxel: topdown z span sum %llu", zsum);
				std::map<unsigned, unsigned> addr_hist;
				for (auto k = 0; k < t.voxelLeafNodeCount; k++) addr_hist[t.voxelLeafNodeArray[k].lightListAddress]++;
				std::vector<std::pair<unsigned, unsigned>> top(addr_hist.begin(), addr_hist.end());
				std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
				ZONETOOL_INFO("voxel: %zu distinct leaf addresses", addr_hist.size());
				for (auto k = 0u; k < std::min<std::size_t>(top.size(), 12); k++)
				{
					std::string ls;
					for (auto q = top[k].first; q < std::min<unsigned>(top[k].first + 10, t.lightListArraySize); q++) ls += std::to_string(t.lightListArray[q]) + " ";
					ZONETOOL_INFO("voxel: leaf address %u x%u -> %s", top[k].first, top[k].second, ls.data());
				}
				std::string first_leaves;
				for (auto k = 0; k < std::min(t.voxelLeafNodeCount, 64); k++) first_leaves += std::to_string(t.voxelLeafNodeArray[k].lightListAddress) + " ";
				ZONETOOL_INFO("voxel: first leaves %s", first_leaves.data());
				if (t.voxelInternalNodeDynamicLightList)
					ZONETOOL_INFO("voxel: dynamic light list %u %u %u %u", t.voxelInternalNodeDynamicLightList[0], t.voxelInternalNodeDynamicLightList[1],
						t.voxelInternalNodeDynamicLightList[2], t.voxelInternalNodeDynamicLightList[3]);
				voxel_hypotheses(t, cw);
			}
			std::map<int, unsigned> type_hist;
			for (auto k = 0u; k < cw->primaryLightCount; k++) type_hist[cw->primaryLights[k].type]++;
			for (const auto& [t, n] : type_hist) ZONETOOL_INFO("lights: type %d x%u", t, n);
			std::vector<unsigned> lp = {0, 1, 2, 3, 4, 5};
			for (auto k = 0u, o = 0u, s = 0u; k < cw->primaryLightCount; k++)
			{
				const auto t = cw->primaryLights[k].type;
				if (t == GFX_LIGHT_TYPE_OMNI && o++ < 3) lp.push_back(k);
				if (t == GFX_LIGHT_TYPE_SPOT && s++ < 2) lp.push_back(k);
			}
			for (const auto k : lp)
			{
				if (k >= cw->primaryLightCount) continue;
				const auto& l = cw->primaryLights[k];
				ZONETOOL_INFO("light %u type %d shadow %u dyn %u vol %u exp %u tzl %x ent %u uv %.2f ir %.2f color %.3f %.3f %.3f dir %.2f %.2f %.2f origin %.0f %.0f %.0f radius %.0f fade %.2f %.2f bulb %.1f cos %.3f %.3f soft %.2f bias %.3f area %.2f falloff %.2f def %s",
					k, l.type, l.canUseShadowMap, l.needsDynamicShadows, l.isVolumetric, l.exponent, l.transientZoneList, l.entityId, l.uvIntensity, l.irIntensity,
					l.color[0], l.color[1], l.color[2], l.dir[0], l.dir[1], l.dir[2], l.origin[0], l.origin[1], l.origin[2], l.radius, l.fadeOffsetRt[0], l.fadeOffsetRt[1],
					l.bulbRadius, l.cosHalfFovOuter, l.cosHalfFovInner, l.shadowSoftness, l.shadowBias, l.shadowArea, l.distanceFalloff, l.defName ? l.defName : "-");
				if (k < w->primaryLightCount)
				{
					if (w->shadowGeomOptimized)
						ZONETOOL_INFO("light %u shadow geom: surfaces %u smodels %u", k, w->shadowGeomOptimized[k].surfaceCount, w->shadowGeomOptimized[k].smodelCount);
					if (w->lightRegion)
					{
						const auto& r = w->lightRegion[k];
						ZONETOOL_INFO("light %u region hulls %u%s", k, r.hullCount, r.hullCount ? "" : "");
						for (auto h = 0u; h < r.hullCount && h < 2; h++)
						{
							std::string mid, half;
							for (auto a = 0; a < 9; a++) { mid += std::to_string(r.hulls[h].kdopMidPoint[a]) + " "; half += std::to_string(r.hulls[h].kdopHalfSize[a]) + " "; }
							ZONETOOL_INFO("light %u hull %u axes %u kdop mid %s half %s", k, h, r.hulls[h].axisCount, mid.data(), half.data());
							for (auto a = 0u; a < r.hulls[h].axisCount && a < 4; a++)
							{
								const auto& hax = r.hulls[h].axis[a];
								ZONETOOL_INFO("light %u hull %u axis %u dir %.3f %.3f %.3f mid %.1f half %.1f", k, h, a, hax.dir[0], hax.dir[1], hax.dir[2], hax.midPoint, hax.halfSize);
							}
						}
					}
					if (w->frustumLights)
					{
						const auto& f = w->frustumLights[k];
						ZONETOOL_INFO("light %u frustum idx %u vtx %u", k, f.indexCount, f.vertexCount);
						for (auto v = 0u; v < f.vertexCount && v < 3 && f.vertices; v++)
						{
							const auto* fv = reinterpret_cast<const float*>(f.vertices + v * 32);
							ZONETOOL_INFO("light %u frustum v%u %f %f %f %f %f %f %f %f", k, v, fv[0], fv[1], fv[2], fv[3], fv[4], fv[5], fv[6], fv[7]);
						}
					}
					if (w->lightViewFrustums)
					{
						const auto& f = w->lightViewFrustums[k];
						ZONETOOL_INFO("light %u view frustum planes %u idx %u vtx %u", k, f.planeCount, f.indexCount, f.vertexCount);
						for (auto p = 0u; p < f.planeCount && p < 3; p++)
							ZONETOOL_INFO("light %u view plane %u %f %f %f %f", k, p, f.planes[p][0], f.planes[p][1], f.planes[p][2], f.planes[p][3]);
						for (auto v = 0u; v < f.vertexCount && v < 2; v++)
							ZONETOOL_INFO("light %u view vertex %u %f %f %f", k, v, f.vertices[v][0], f.vertices[v][1], f.vertices[v][2]);
					}
				}
			}
			for (auto k = 0u; k < cw->primaryLightEnvCount && k < 4; k++)
			{
				const auto& e = cw->primaryLightEnvs[k];
				ZONETOOL_INFO("light env %u: n %u [%u %u %u %u]", k, e.numIndices, e.primaryLightIndices[0], e.primaryLightIndices[1], e.primaryLightIndices[2], e.primaryLightIndices[3]);
			}
			if (w->lightAABB.nodeArray)
			{
				for (auto k = 0u; k < std::min<unsigned>(w->lightAABB.nodeCount, 4); k++)
				{
					const auto& n = w->lightAABB.nodeArray[k];
					ZONETOOL_INFO("light aabb node %u first %u count %u mid %.0f %.0f %.0f half %.0f %.0f %.0f", k, n.firstChild, n.childCount, n.bound.midPoint[0],
						n.bound.midPoint[1], n.bound.midPoint[2], n.bound.halfSize[0], n.bound.halfSize[1], n.bound.halfSize[2]);
				}
				std::string s;
				for (auto k = 0u; k < std::min<unsigned>(w->lightAABB.lightCount, 24); k++) s += std::to_string(w->lightAABB.lightArray[k]) + " ";
				ZONETOOL_INFO("light aabb lights head %s", s.data());
				// which lights the aabb tree holds
				std::map<std::string, unsigned> in_tree, all;
				std::set<unsigned> listed(w->lightAABB.lightArray, w->lightAABB.lightArray + w->lightAABB.lightCount);
				for (auto k = 0u; k < cw->primaryLightCount; k++)
				{
					const auto& l = cw->primaryLights[k];
					const auto key = "type " + std::to_string(l.type) + " shadow " + std::to_string(l.canUseShadowMap) + " dyn " + std::to_string(l.needsDynamicShadows) +
						" vol " + std::to_string(l.isVolumetric) + (k >= cw->firstScriptablePrimaryLight ? " scriptable" : "");
					all[key]++;
					if (listed.contains(k)) in_tree[key]++;
				}
				for (const auto& [key, n] : all) ZONETOOL_INFO("light aabb: %s: %u lights, %u in the tree", key.data(), n, in_tree.contains(key) ? in_tree[key] : 0u);
			}
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

		// Generated static models: each instance copies the draw instance of a template static model using the same
		// XModel for the fields that are not placement or lighting, then is set to light grid lighting at its own place.
		// A model with no light grid lit template instance (a kit model of another map, an authored model) starts from
		// a neutral template instance and gets its model-dependent fields fitted (iwzmap_smodel_rules.hpp, which also
		// names the cause of the earlier crash). IWZ_SMODEL_DONOR=<template model> copies that model's instance verbatim
		// instead: the earlier behaviour, kept to reproduce the crash.
		void build_smodels(GfxWorld* w, const std::vector<smodel_source>& list, zone_memory* mem)
		{
			namespace rules = smodel_rules;
			const auto n = static_cast<unsigned int>(list.size());
			auto& d = w->dpvs;
			std::unordered_map<std::string, unsigned int> by_model;
			for (auto i = 0u; i < d.smodelCount; i++)
			{
				const auto& dinst = d.smodelDrawInsts[i];
				const auto lit_by_grid = (dinst.flags & STATIC_MODEL_FLAG_LIGHTGRID_LIGHTING) != 0;
				if (lit_by_grid && dinst.model && dinst.model->name && !by_model.contains(dinst.model->name)) by_model[dinst.model->name] = i;
			}
			int forced_donor = -1;
			if (const auto* forced = std::getenv("IWZ_SMODEL_DONOR"))
			{
				if (!by_model.contains(forced)) throw std::runtime_error(std::string("IWZ_SMODEL_DONOR: ") + forced + " has no light grid lit template instance");
				forced_donor = static_cast<int>(by_model[forced]);
				ZONETOOL_WARNING("iwzmap: IWZ_SMODEL_DONOR: twinless static models copy the draw instance of %s verbatim (firstMtlSkinIndex %u; crash reproduction)",
					forced, d.smodelDrawInsts[forced_donor].firstMtlSkinIndex);
			}
			const auto neutral = rules::find_neutral_donor(d.smodelDrawInsts, d.smodelCount);
			auto twinless_needed = false;
			for (const auto& s : list) if (!by_model.contains(s.model)) twinless_needed = true;
			if (twinless_needed && forced_donor < 0 && neutral < 0) throw std::runtime_error("the template world has no neutral light grid lit static model");

			auto* insts = mem->allocate<GfxStaticModelInst>(std::max(n, 1u));
			auto* draws = mem->allocate<GfxStaticModelDrawInst>(std::max(n, 1u));
			auto* lod = mem->allocate<unsigned int>(n + 1);
			auto* sorted = mem->allocate<unsigned short>(std::max(n, 1u));
			std::unordered_map<std::string, XModel*> stubs;
			auto copied = 0u;
			auto twinless = 0u;
			auto range_end = 0u;
			const auto* unk_mode = std::getenv("IWZ_SMODEL_UNK");
			const bool zero_unk = unk_mode && std::string(unk_mode) == "zero";
			const auto* lod_mode = std::getenv("IWZ_SMODEL_LOD");
			const bool lod_index = !(lod_mode && std::string(lod_mode) == "zero");
			for (auto i = 0u; i < n; i++)
			{
				const auto& s = list[i];
				const auto twin = by_model.contains(s.model);
				const auto t = twin ? by_model[s.model] : static_cast<unsigned int>(forced_donor >= 0 ? forced_donor : neutral);
				if (twin) copied++;
				else
				{
					twinless++;
					if (!stubs.contains(s.model))
					{
						const auto& td = d.smodelDrawInsts[t];
						const auto* xm = s.xmodel;
						ZONETOOL_INFO("iwzmap: static model %s has no template instance: %s draw instance of %s (%u surfaces, lod0 surfIndex %u, skin %u%s)",
							s.model.data(), forced_donor >= 0 ? "verbatim" : "fitted neutral", td.model ? td.model->name : "-",
							xm ? xm->numsurfs : 0u, xm ? xm->lodInfo[0].surfIndex : 0u, forced_donor >= 0 ? td.firstMtlSkinIndex : 0u,
							forced_donor >= 0 && xm && rules::skin_reads_past_materials(td.firstMtlSkinIndex, xm->lodInfo[0].surfIndex, xm->numsurfs)
								? "; the draw pass will read past its materials" : "");
					}
				}
				auto& dinst = draws[i];
				dinst = d.smodelDrawInsts[t];
				if (!twin && forced_donor < 0) rules::fit_to_model(dinst);
				for (auto a = 0; a < 3; a++)
				{
					dinst.placement.origin[a] = s.origin[a];
					for (auto k = 0; k < 3; k++) dinst.placement.axis[k][a] = s.axis[k][a];
				}
				dinst.placement.scale = s.scale;
				// stock (unk1:unk0, unk3) ranges are contiguous and never overlap across all instances (0..92061 in
				// cp_zmb): give each generated instance its own range of its template's length
				dinst.unk0 = static_cast<unsigned short>(range_end & 0xFFFF);
				dinst.unk1 = static_cast<unsigned short>(range_end >> 16);
				range_end += dinst.unk3;
				// unk9: 1 + rank of the instance in (unk1:unk0) order (verified on all 17010 cp_zmb instances)
				dinst.unk9 = static_cast<unsigned short>(i + 1);
				if (zero_unk)
				{
					// like the H1 -> IW7 converter: every unnamed field zero
					dinst.unk0 = dinst.unk1 = dinst.unk2 = dinst.unk3 = 0;
					dinst.unk5 = 0;
					std::memset(dinst.unk6, 0, sizeof(dinst.unk6));
					dinst.unk9 = dinst.unk10 = dinst.unk11 = 0;
					dinst.unk13 = dinst.unk14 = 0;
					std::memset(dinst.unk15, 0, sizeof(dinst.unk15));
					dinst.unk21 = 0;
				}
				if (!stubs.contains(s.model)) stubs[s.model] = name_stub<XModel>(mem, s.model);
				dinst.model = stubs[s.model];
				std::memset(&dinst.vertexLightingInfo, 0, sizeof(dinst.vertexLightingInfo));
				dinst.modelLightmapInfo = {};
				dinst.modelLightmapInfo.lightmapIndex = -1;
				dinst.flags = static_cast<unsigned short>((dinst.flags & ~(STATIC_MODEL_FLAG_VERTEXLIT_LIGHTING | STATIC_MODEL_FLAG_LIGHTMAP_LIGHTING |
					STATIC_MODEL_FLAG_GROUND_LIGHTING | STATIC_MODEL_FLAG_NO_CAST_SHADOW)) | STATIC_MODEL_FLAG_LIGHTGRID_LIGHTING);
				if (s.no_shadow) dinst.flags |= STATIC_MODEL_FLAG_NO_CAST_SHADOW;
				if (s.cull >= 0) dinst.cullDist = static_cast<unsigned short>(std::min(s.cull, 0xFFFF));
				dinst.transientZone = 0;
				dinst.primaryLightEnvIndex = 0;
				dinst.reflectionProbeIndex = 0;

				auto& sinst = insts[i];
				sinst.bounds = make_bounds(s.mins, s.maxs);
				for (auto a = 0; a < 3; a++) sinst.lightingOrigin[a] = sinst.bounds.midPoint[a];
				// stock lodData: a distinct slot per instance (0..lodData[count]-1, one duplicate 0), like a filled
				// runtime cache; the generated world gives instance i slot i and lodData[count] = count
				// (IWZ_SMODEL_LOD=zero: all 0). Engine-tested: with all-zero lodData and template unk9 values no generated
				// static model was drawn; with slots + unk9 rank they render (iwz-stage2/smodels run-a-2 vs run-a-3).
				lod[i] = lod_index ? i : 0;
				sorted[i] = static_cast<unsigned short>(i);
			}
			lod[n] = lod_index ? n : 0;
			// consecutive draws of one model (instanced batches)
			std::stable_sort(sorted, sorted + n, [&](unsigned short a, unsigned short b) { return list[a].model < list[b].model; });

			d.smodelCount = n;
			d.smodelVisDataCount = (n + 31) >> 5;
			d.smodelInsts = n ? insts : nullptr;
			d.smodelDrawInsts = n ? draws : nullptr;
			d.sortedSmodelIndices = n ? sorted : nullptr;
			d.lodData = lod;
			ZONETOOL_INFO("iwzmap: %u static models (%u with a same-model template instance, %u without, %zu models, ranges %u)", n, copied, twinless, stubs.size(), range_end);
		}

		// Primary lights: each generated light takes a static template light slot of the same type (and shadow
		// capability when one is left), in the static range [lastSun + 1, firstScriptable). The slot keeps its
		// definition (light_point_linear) and entity id; position, shape and colour become the generated light's.
		// The GfxWorld per-light data (shadow geometry, light region, frustum meshes) of the slot is moved with it.
		// Returns the used slots (their Umbra objects are class 4).
		// Named lights (scripted) take slots of the scriptable range [firstScriptablePrimaryLight, primaryLightCount)
		// (cp_zmb: 25 spot slots 1014..1038, each linked to its light_spot map entity by the entity key "1" = light
		// index). When more lights are named than the template has scriptable slots, the range is extended
		// downwards over static template slots (firstScriptablePrimaryLight lowered; IWZ_NO_SCRIPT_LIGHT_EXTEND
		// refuses instead): every primary light array is per light index, so only the range bounds change.
		std::vector<unsigned int> assign_light_slots(ComWorld* cw, std::vector<light_source>& lights)
		{
			std::vector<unsigned int> slots;
			if (lights.empty()) return slots;
			std::vector<bool> used(cw->primaryLightCount);
			unsigned int last_sun = 0;
			for (auto k = 0u; k < cw->primaryLightCount; k++) if (cw->primaryLights[k].type == GFX_LIGHT_TYPE_DIR) last_sun = k;
			auto named = 0u;
			for (const auto& l : lights) if (!l.name.empty()) named++;
			if (named && cw->scriptablePrimaryLightCount < named)
			{
				if (std::getenv("IWZ_NO_SCRIPT_LIGHT_EXTEND") || !cw->scriptablePrimaryLightCount)
					throw std::runtime_error("lights.txt names " + std::to_string(named) + " lights, the template has " +
						std::to_string(cw->scriptablePrimaryLightCount) + " scriptable light slots");
				const auto first = cw->primaryLightCount - named;
				if (first <= last_sun + 1) throw std::runtime_error("not enough template light slots for the named lights");
				ZONETOOL_INFO("iwzmap: scriptable light range extended from %u..%u to %u..%u (%u named lights)",
					cw->firstScriptablePrimaryLight, cw->primaryLightCount - 1, first, cw->primaryLightCount - 1, named);
				cw->firstScriptablePrimaryLight = first;
				cw->scriptablePrimaryLightCount = named;
			}
			const auto end = cw->scriptablePrimaryLightCount ? cw->firstScriptablePrimaryLight : cw->primaryLightCount;
			// named lights first (scriptable range, same type preferred, any spot/omni slot otherwise)
			for (auto& l : lights)
			{
				if (l.name.empty()) continue;
				const auto want = l.spot ? GFX_LIGHT_TYPE_SPOT : GFX_LIGHT_TYPE_OMNI;
				int pick = -1;
				for (auto pass = 0; pass < 2 && pick < 0; pass++)
					for (auto k = end; k < cw->primaryLightCount && pick < 0; k++)
					{
						if (used[k]) continue;
						const auto t = cw->primaryLights[k].type;
						if (pass == 0 && t != want) continue;
						if (t != GFX_LIGHT_TYPE_SPOT && t != GFX_LIGHT_TYPE_OMNI) continue;
						pick = static_cast<int>(k);
					}
				if (pick < 0) throw std::runtime_error("no scriptable light slot left for light " + l.name);
				used[pick] = true;
				l.slot = static_cast<unsigned int>(pick);
				if (l.shadow && !cw->primaryLights[pick].canUseShadowMap) l.shadow = false; // slot has no shadow data
			}
			for (auto& l : lights)
			{
				if (!l.name.empty()) { slots.push_back(l.slot); continue; }
				const auto want = l.spot ? GFX_LIGHT_TYPE_SPOT : GFX_LIGHT_TYPE_OMNI;
				int pick = -1;
				for (auto pass = 0; pass < 3 && pick < 0; pass++)
				{
					for (auto k = last_sun + 1; k < end && pick < 0; k++)
					{
						if (used[k]) continue;
						const auto& t = cw->primaryLights[k];
						if (pass == 0 && (t.type != want || (t.canUseShadowMap != 0) != l.shadow || t.isVolumetric)) continue;
						if (pass == 1 && (t.type != want || t.isVolumetric)) continue;
						if (pass == 2 && t.type != GFX_LIGHT_TYPE_SPOT && t.type != GFX_LIGHT_TYPE_OMNI) continue;
						pick = static_cast<int>(k);
					}
				}
				if (pick < 0) throw std::runtime_error("no static template light slot left for a generated light");
				used[pick] = true;
				l.slot = static_cast<unsigned int>(pick);
				slots.push_back(l.slot);
			}
			return slots;
		}

		void spot_axes(const light_source& l, float* dir, float* up)
		{
			// dir points from the light along its beam (pitch down positive, like player angles)
			const float angles[3] = {l.pitch, l.yaw, 0.0f};
			float axis[3][3];
			angles_to_axis(angles, axis);
			for (auto a = 0; a < 3; a++) { dir[a] = axis[0][a]; up[a] = axis[2][a]; }
		}

		void build_com_lights(ComWorld* cw, const std::vector<light_source>& lights)
		{
			for (const auto& l : lights)
			{
				auto& p = cw->primaryLights[l.slot];
				const auto cos_outer = std::cos(l.fov * 0.5f * 3.14159265f / 180.0f);
				// keep the template's inner/outer cone relation (soft edge)
				const auto inner = std::cos(std::max(l.fov * 0.5f - 10.0f, l.fov * 0.25f) * 3.14159265f / 180.0f);
				p.type = l.spot ? GFX_LIGHT_TYPE_SPOT : GFX_LIGHT_TYPE_OMNI;
				p.canUseShadowMap = l.shadow ? 1 : 0;
				p.needsDynamicShadows = 0;
				p.isVolumetric = 0;
				p.transientZoneList = 0;
				p.uvIntensity = 0.0f;
				p.irIntensity = 0.0f;
				for (auto a = 0; a < 3; a++) { p.color[a] = l.rgb[a] * l.intensity; p.origin[a] = l.origin[a]; }
				p.radius = l.radius;
				if (l.spot)
				{
					spot_axes(l, p.dir, p.up);
					p.cosHalfFovOuter = cos_outer;
					p.cosHalfFovInner = std::max(inner, cos_outer);
				}
				else
				{
					p.dir[0] = 0.0f; p.dir[1] = 0.0f; p.dir[2] = 1.0f;
					p.up[0] = 0.0f; p.up[1] = 1.0f; p.up[2] = 0.0f;
				}
				p.bulbLength[0] = p.bulbLength[1] = p.bulbLength[2] = 0.0f;
				p.rotationLimit = 1.0f;
				p.translationLimit = 0.0f;
				ZONETOOL_INFO("iwzmap: light %s slot %u at %.0f %.0f %.0f radius %.0f color %.0f %.0f %.0f%s", l.spot ? "spot" : "omni", l.slot,
					l.origin[0], l.origin[1], l.origin[2], l.radius, p.color[0], p.color[1], p.color[2], l.shadow ? " shadow" : "");
			}
		}

		// light_spot map entities of the named lights (stock cp_zmb layout of a scriptable light entity; keys are the
		// template's entity string ids: 157 classname, 543 origin, 65 angles, 822 targetname, 638 script_noteworthy,
		// 1 primary light index, 599 radius, 4524 _color, 39409 intensity, 29508 / 29507 fov outer / inner,
		// 20434 light definition). The angles give the spot direction (pitch, yaw) like the light itself.
		std::string light_ents_text(const ComWorld* cw, const std::vector<light_source>& lights)
		{
			std::string out;
			char buf[512];
			for (const auto& l : lights)
			{
				if (l.name.empty()) continue;
				const auto& p = cw->primaryLights[l.slot];
				std::snprintf(buf, sizeof(buf), "{\n157 \"light_spot\"\n543 \"%g %g %g\"\n65 \"%g %g 0\"\n822 \"%s\"\n", l.origin[0], l.origin[1],
					l.origin[2], l.spot ? l.pitch : 0.0f, l.spot ? l.yaw : 0.0f, l.name.data());
				out += buf;
				if (!l.group.empty()) out += "638 \"" + l.group + "\"\n";
				std::snprintf(buf, sizeof(buf), "1 \"%u\"\n599 \"%g\"\n4524 \"%g %g %g\"\n39409 \"%g\"\n29508 \"%g\"\n29507 \"%g\"\n20434 \"%s\"\n}\n",
					l.slot, l.radius, l.rgb[0], l.rgb[1], l.rgb[2], l.intensity, l.spot ? l.fov : 90.0f,
					l.spot ? std::max(l.fov * 0.5f - 10.0f, l.fov * 0.25f) * 2.0f : 45.0f, p.defName ? p.defName : "light_point_linear");
				out += buf;
				ZONETOOL_INFO("iwzmap: scripted light %s (group %s) -> scriptable slot %u", l.name.data(), l.group.empty() ? "-" : l.group.data(), l.slot);
			}
			return out;
		}

		// light frame: dir, up, side columns
		void light_frame(const ComPrimaryLight& l, double f[3][3])
		{
			double d[3] = {l.dir[0], l.dir[1], l.dir[2]}, u[3] = {l.up[0], l.up[1], l.up[2]};
			auto norm = [](double* v) { const auto n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (n > 1e-9) for (auto a = 0; a < 3; a++) v[a] /= n; };
			norm(d);
			const auto du = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
			for (auto a = 0; a < 3; a++) u[a] -= du * d[a];
			norm(u);
			if (std::fabs(u[0]) + std::fabs(u[1]) + std::fabs(u[2]) < 1e-6) { u[0] = 0; u[1] = std::fabs(d[1]) < 0.9 ? 1 : 0; u[2] = std::fabs(d[1]) < 0.9 ? 0 : 1; }
			const double s[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
			for (auto a = 0; a < 3; a++) { f[a][0] = d[a]; f[a][1] = u[a]; f[a][2] = s[a]; }
		}

		// moves a template light's world-space shapes (frustum mesh, shadow view frustum) onto the generated light:
		// p' = o_new + scale * R (p - o_old), R = F_new F_old^T (omni lights only translate and scale)
		struct light_transform
		{
			double r[3][3]{};
			double scale = 1.0;
			double from[3]{}, to[3]{};

			void point(float* p) const
			{
				double v[3] = {p[0] - from[0], p[1] - from[1], p[2] - from[2]};
				for (auto a = 0; a < 3; a++) p[a] = static_cast<float>(to[a] + scale * (r[a][0] * v[0] + r[a][1] * v[1] + r[a][2] * v[2]));
			}

			void plane(float* pl) const
			{
				// n.x + d = 0: transform the normal and a point on the plane
				float p[3] = {-pl[3] * pl[0], -pl[3] * pl[1], -pl[3] * pl[2]};
				point(p);
				double n[3];
				for (auto a = 0; a < 3; a++) n[a] = r[a][0] * pl[0] + r[a][1] * pl[1] + r[a][2] * pl[2];
				for (auto a = 0; a < 3; a++) pl[a] = static_cast<float>(n[a]);
				pl[3] = static_cast<float>(-(n[0] * p[0] + n[1] * p[1] + n[2] * p[2]));
			}
		};

		void build_gfx_lights(GfxWorld* w, const std::vector<ComPrimaryLight>& before, const ComWorld* cw, const std::vector<light_source>& lights,
			const world_source& src, zone_memory* mem)
		{
			// the template lights that are not reused keep no shadow casters: their lists index template surfaces and
			// static models that the generated world does not have
			if (w->shadowGeomOptimized && !std::getenv("IWZ_KEEP_TEMPLATE_SHADOWGEOM"))
				for (auto k = 0u; k < w->primaryLightCount; k++) w->shadowGeomOptimized[k] = {};
			for (auto li = 0u; li < lights.size(); li++)
			{
				const auto& l = lights[li];
				const auto k = l.slot;
				const auto& o = before[li];
				const auto& n = cw->primaryLights[k];
				light_transform t;
				t.scale = o.radius > 0.0f ? static_cast<double>(n.radius) / o.radius : 1.0;
				for (auto a = 0; a < 3; a++) { t.from[a] = o.origin[a]; t.to[a] = n.origin[a]; }
				double fo[3][3], fn[3][3];
				light_frame(o, fo);
				light_frame(n, fn);
				for (auto a = 0; a < 3; a++)
					for (auto b = 0; b < 3; b++)
						t.r[a][b] = l.spot ? fn[a][0] * fo[b][0] + fn[a][1] * fo[b][1] + fn[a][2] * fo[b][2] : (a == b ? 1.0 : 0.0);

				auto moved_vertices = 0u;
				if (w->frustumLights)
				{
					auto& f = w->frustumLights[k];
					if (f.vertices && f.vertexCount)
					{
						auto* copy = mem->allocate<char>(32ull * f.vertexCount);
						std::memcpy(copy, f.vertices, 32ull * f.vertexCount);
						for (auto v = 0u; v < f.vertexCount; v++) t.point(reinterpret_cast<float*>(copy + 32ull * v));
						f.vertices = copy;
						moved_vertices += f.vertexCount;
					}
				}
				if (w->lightViewFrustums)
				{
					auto& f = w->lightViewFrustums[k];
					if (f.planes && f.planeCount)
					{
						auto* planes = mem->allocate<vec4_t>(f.planeCount);
						std::memcpy(planes, f.planes, sizeof(vec4_t) * f.planeCount);
						for (auto p = 0u; p < f.planeCount; p++) t.plane(planes[p]);
						f.planes = planes;
					}
					if (f.vertices && f.vertexCount)
					{
						auto* verts = mem->allocate<vec3_t>(f.vertexCount);
						std::memcpy(verts, f.vertices, sizeof(vec3_t) * f.vertexCount);
						for (auto v = 0u; v < f.vertexCount; v++) t.point(verts[v]);
						f.vertices = verts;
						moved_vertices += f.vertexCount;
					}
				}
				if (w->lightRegion) { w->lightRegion[k].hullCount = 0; w->lightRegion[k].hulls = nullptr; }
				if (w->shadowGeomOptimized)
				{
					// shadow casters: generated surfaces and static models inside the light's sphere
					auto& g = w->shadowGeomOptimized[k];
					g = {};
					if (l.shadow)
					{
						auto touches = [&](const float* mins, const float* maxs)
						{
							auto d2 = 0.0f;
							for (auto a = 0; a < 3; a++)
							{
								const auto c = std::clamp(l.origin[a], mins[a], maxs[a]);
								d2 += (c - l.origin[a]) * (c - l.origin[a]);
							}
							return d2 <= l.radius * l.radius;
						};
						std::vector<unsigned int> surfs;
						for (auto s = 0u; s < src.surfaces.size(); s++) if (touches(src.surfaces[s].mins, src.surfaces[s].maxs)) surfs.push_back(s);
						std::vector<unsigned short> models;
						for (auto s = 0u; s < w->dpvs.smodelCount; s++)
						{
							const auto& b = w->dpvs.smodelInsts[s].bounds;
							const float mins[3] = {b.midPoint[0] - b.halfSize[0], b.midPoint[1] - b.halfSize[1], b.midPoint[2] - b.halfSize[2]};
							const float maxs[3] = {b.midPoint[0] + b.halfSize[0], b.midPoint[1] + b.halfSize[1], b.midPoint[2] + b.halfSize[2]};
							if (touches(mins, maxs) && !(w->dpvs.smodelDrawInsts[s].flags & STATIC_MODEL_FLAG_NO_CAST_SHADOW)) models.push_back(static_cast<unsigned short>(s));
						}
						g.surfaceCount = static_cast<unsigned short>(surfs.size());
						g.smodelCount = static_cast<unsigned short>(models.size());
						if (!surfs.empty()) { g.sortedSurfIndex = mem->allocate<unsigned int>(surfs.size()); std::memcpy(g.sortedSurfIndex, surfs.data(), surfs.size() * 4); }
						if (!models.empty()) { g.smodelIndex = mem->allocate<unsigned short>(models.size()); std::memcpy(g.smodelIndex, models.data(), models.size() * 2); }
					}
				}
				ZONETOOL_INFO("iwzmap: light slot %u: moved %u shape vertices from %.0f %.0f %.0f (scale %.2f); shadow casters %u surfaces %u static models", k,
					moved_vertices, o.origin[0], o.origin[1], o.origin[2], t.scale, w->shadowGeomOptimized ? w->shadowGeomOptimized[k].surfaceCount : 0,
					w->shadowGeomOptimized ? w->shadowGeomOptimized[k].smodelCount : 0);
			}
		}

		// Generated static light voxel tree (format: see voxel_visit): roots of 512 units over the generated world,
		// 128-unit internal nodes, 32-unit leaves listing the generated lights whose sphere touches them. The template
		// tree binned the template lights at their cp_zmb places (which the generated world overlaps), so it is
		// always replaced: without lights.txt every leaf is empty.
		void build_voxel_tree(GfxWorld* w, const ComWorld* cw, const std::vector<light_source>& lights, const world_source& src,
			const voxel_layout& lay, zone_memory* mem)
		{
			if (!w->voxelTreeCount || !w->voxelTree || !w->voxelTree[0].voxelTreeHeader || std::getenv("IWZ_KEEP_VOXELS")) return;
			auto& t = w->voxelTree[0];
			const auto* old = t.voxelTreeHeader;
			constexpr auto root = 512.0f;
			float lo[3], hi[3];
			for (auto a = 0; a < 3; a++) { lo[a] = src.mins[a]; hi[a] = src.maxs[a]; }
			for (const auto& l : lights)
				for (auto a = 0; a < 3; a++) { lo[a] = std::min(lo[a], l.origin[a] - l.radius); hi[a] = std::max(hi[a], l.origin[a] + l.radius); }
			int dim[3];
			float base[3];
			for (auto a = 0; a < 3; a++)
			{
				base[a] = std::floor(lo[a] / root) * root;
				dim[a] = std::max(1, static_cast<int>(std::ceil((hi[a] - base[a]) / root)));
			}
			auto* h = mem->allocate<GfxVoxelTreeHeader>();
			*h = *old;
			for (auto a = 0; a < 3; a++)
			{
				h->rootNodeDimension[a] = dim[a];
				h->boundMin[a] = base[a];
				h->boundMax[a] = base[a] + dim[a] * root;
			}
			h->rootNodeDimension[3] = 0;
			h->nodeCoordBitShift[0] = 9; h->nodeCoordBitShift[1] = 7; h->nodeCoordBitShift[2] = 5; h->nodeCoordBitShift[3] = 0;
			h->boundMin[3] = h->boundMax[3] = 0.0f;

			// light lists
			std::vector<unsigned short> arr = {t.lightListArraySize > 1 ? t.lightListArray[0] : static_cast<unsigned short>(16384),
				t.lightListArraySize > 1 ? t.lightListArray[1] : static_cast<unsigned short>(1), 0};
			std::map<std::vector<unsigned short>, unsigned short> addresses;
			addresses[{}] = 2;
			auto touching = [&](const float* o, float size)
			{
				std::vector<unsigned short> spots, omnis;
				for (const auto& l : lights)
				{
					if (!box_touches_sphere(o, size, l.origin, l.radius)) continue;
					(cw->primaryLights[l.slot].type == GFX_LIGHT_TYPE_SPOT ? spots : omnis).push_back(static_cast<unsigned short>(l.slot));
				}
				std::sort(spots.begin(), spots.end());
				std::sort(omnis.begin(), omnis.end());
				std::vector<unsigned short> key;
				if (spots.empty() && omnis.empty()) return key;
				key.push_back(static_cast<unsigned short>((spots.size() << 7) | omnis.size()));
				key.insert(key.end(), spots.begin(), spots.end());
				key.insert(key.end(), omnis.begin(), omnis.end());
				return key;
			};
			auto address = [&](const std::vector<unsigned short>& key) -> unsigned short
			{
				if (const auto it = addresses.find(key); it != addresses.end()) return it->second;
				if (arr.size() + key.size() > 0xFFFF) throw std::runtime_error("voxel light lists exceed 65535 entries");
				const auto a = static_cast<unsigned short>(arr.size());
				arr.insert(arr.end(), key.begin(), key.end());
				addresses[key] = a;
				return a;
			};
			auto any_light = [&](const float* o, float size)
			{
				for (const auto& l : lights) if (box_touches_sphere(o, size, l.origin, l.radius)) return true;
				return false;
			};

			std::vector<GfxVoxelInternalNode> internal(1);
			std::vector<GfxVoxelLeafNode> leaves(1);
			leaves[0].lightListAddress = 0;
			const auto* fields = voxel_perms[lay.perm];
			auto child_origin = [&](const float* org, float child, int b, float* o)
			{
				const int f[3] = {b & 3, (b >> 2) & 3, (b >> 4) & 3};
				for (auto a = 0; a < 3; a++) o[a] = org[a];
				for (auto k = 0; k < 3; k++) o[fields[k]] += f[k] * child;
			};
			// fills internal node `node` (already reserved) covering org..org+size; children of 128-unit nodes are leaves
			std::function<void(std::size_t, const float*, float)> fill = [&](std::size_t node, const float* org, float size)
			{
				const auto child = size / 4.0f;
				const auto leaf_children = child <= 32.0f;
				GfxVoxelInternalNode in{};
				std::vector<std::pair<std::size_t, std::array<float, 3>>> recurse;
				for (auto word = 0; word < 2; word++)
				{
					unsigned mask = 0;
					const auto first = leaf_children ? leaves.size() : internal.size();
					std::vector<std::array<float, 3>> kids;
					for (auto bit = 0; bit < 32; bit++)
					{
						float o[3];
						child_origin(org, child, word * 32 + bit, o);
						if (!any_light(o, child)) continue;
						mask |= 1u << bit;
						kids.push_back({o[0], o[1], o[2]});
					}
					in.childNodeMask[word] = static_cast<int>(mask);
					in.firstNodeIndex[word] = static_cast<int>(leaf_children ? (0x80000000u | static_cast<unsigned>(first)) : static_cast<unsigned>(first));
					if (!mask) in.firstNodeIndex[word] = 0;
					for (const auto& o : kids)
					{
						if (leaf_children)
						{
							GfxVoxelLeafNode leaf{};
							leaf.lightListAddress = address(touching(o.data(), child));
							leaves.push_back(leaf);
						}
						else
						{
							recurse.emplace_back(internal.size(), o);
							internal.push_back({});
						}
					}
				}
				internal[node] = in;
				for (const auto& [idx, o] : recurse) fill(idx, o.data(), child);
			};

			std::vector<GfxVoxelTopDownViewNode> columns(static_cast<std::size_t>(dim[0]) * dim[1]);
			for (auto ty = 0; ty < dim[1]; ty++)
			{
				for (auto tx = 0; tx < dim[0]; tx++)
				{
					const auto k = lay.column_x_major ? tx + ty * dim[0] : ty + tx * dim[1];
					auto& td = columns[k];
					td = {};
					auto z0 = -1, z1 = -1;
					for (auto z = 0; z < dim[2]; z++)
					{
						const float o[3] = {base[0] + tx * root, base[1] + ty * root, base[2] + z * root};
						if (!any_light(o, root)) continue;
						if (z0 < 0) z0 = z;
						z1 = z;
					}
					if (z0 < 0) continue;
					td.firstNodeIndex = static_cast<int>(internal.size());
					td.zMin = z0;
					td.zMax = z1;
					const auto first = internal.size();
					internal.resize(internal.size() + (z1 - z0 + 1));
					for (auto z = z0; z <= z1; z++)
					{
						const float o[3] = {base[0] + tx * root, base[1] + ty * root, base[2] + z * root};
						fill(first + (z - z0), o, root);
					}
				}
			}

			t.voxelTreeHeader = h;
			t.voxelTopDownViewNodeCount = static_cast<int>(columns.size());
			t.voxelTopDownViewNodeArray = mem->allocate<GfxVoxelTopDownViewNode>(columns.size());
			std::memcpy(t.voxelTopDownViewNodeArray, columns.data(), columns.size() * sizeof(GfxVoxelTopDownViewNode));
			t.voxelInternalNodeCount = static_cast<int>(internal.size());
			t.voxelInternalNodeArray = mem->allocate<GfxVoxelInternalNode>(internal.size());
			std::memcpy(t.voxelInternalNodeArray, internal.data(), internal.size() * sizeof(GfxVoxelInternalNode));
			t.voxelLeafNodeCount = static_cast<int>(leaves.size());
			t.voxelLeafNodeArray = mem->allocate<GfxVoxelLeafNode>(leaves.size());
			std::memcpy(t.voxelLeafNodeArray, leaves.data(), leaves.size() * sizeof(GfxVoxelLeafNode));
			t.lightListArraySize = static_cast<int>(arr.size());
			t.lightListArray = mem->allocate<unsigned short>(arr.size());
			std::memcpy(t.lightListArray, arr.data(), arr.size() * sizeof(unsigned short));
			t.voxelInternalNodeDynamicLightList = mem->allocate<unsigned int>(2 * internal.size());
			float zmin[3], zmax[3];
			for (auto a = 0; a < 3; a++) { zmin[a] = std::min(src.mins[a], h->boundMin[a]); zmax[a] = std::max(src.maxs[a], h->boundMax[a]); }
			t.zoneBound = make_bounds(zmin, zmax);
			for (auto v = 1; v < w->voxelTreeCount; v++) w->voxelTree[v] = t;
			ZONETOOL_INFO("iwzmap: voxel light tree %dx%dx%d roots at %.0f %.0f %.0f: %zu internal nodes, %zu leaves, %zu light lists (%zu entries)",
				dim[0], dim[1], dim[2], base[0], base[1], base[2], internal.size(), leaves.size(), addresses.size(), arr.size());

			// verification: decode the generated tree with the same reader and check every listed light
			std::uint64_t ok = 0, total = 0;
			voxel_visit(t, lay, [&](int leaf, const float* o, float size)
			{
				const auto addr = t.voxelLeafNodeArray[leaf].lightListAddress;
				const auto hdr = t.lightListArray[addr];
				for (auto q = 0; q < (hdr >> 7) + (hdr & 0x7F); q++)
				{
					const auto& l = cw->primaryLights[t.lightListArray[addr + 1 + q]];
					total++;
					if (box_touches_sphere(o, size, l.origin, l.radius + 1.0f)) ok++;
				}
			});
			ZONETOOL_INFO("iwzmap: voxel light tree check: %llu / %llu light refs inside the light sphere", ok, total);
		}

		void build_gfx(GfxWorld* w, GfxWorldTransientZone* tz, const world_source& src, const std::vector<smodel_source>& smodels,
			const std::vector<unsigned int>& light_slots, const std::string& map, const std::string& bsp, zone_memory* mem)
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
				// generated static models (none without smodels.txt)
				build_smodels(w, smodels, mem);
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
					for (auto k = 0u; k < w->dpvs.smodelCount; k++) user_ids.push_back(0x10000000u + k);
					for (const auto slot : light_slots) user_ids.push_back(0x40000000u + slot);
					const auto probes = std::getenv("IWZ_TOME_NO_PROBES") ? 0u : w->draw.reflectionProbeData.reflectionProbeInstanceCount;
					for (auto p = 0u; p < probes; p++) user_ids.push_back(0x50000000u + p);
					const auto tome = build_conservative_tome(user_ids, src.mins, src.maxs);
					w->umbraTomeSize = static_cast<unsigned int>(tome.size());
					w->umbraTomeData = mem->allocate<char>(tome.size());
					std::memcpy(w->umbraTomeData, tome.data(), tome.size());
					ZONETOOL_INFO("iwzmap: generated conservative Umbra tome (%u bytes, %u surfaces, %u static models, %zu primary lights, %u probe instances)",
						w->umbraTomeSize, own, w->dpvs.smodelCount, light_slots.size(), probes);
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
					// the leaf lists every static model
					node.smodelIndexCount = keep_vis ? 0 : static_cast<unsigned short>(w->dpvs.smodelCount);
					node.smodelIndexes = nullptr;
					if (node.smodelIndexCount)
					{
						node.smodelIndexes = mem->allocate<unsigned short>(node.smodelIndexCount);
						for (auto k = 0u; k < node.smodelIndexCount; k++) node.smodelIndexes[k] = static_cast<unsigned short>(k);
					}
					node.childrenOffset = 0;
				}
				if (keep_vis)
				{
					for (auto c = 0u; c < cells; c++) w->cellTransientInfos[c].aabbTreeIndex = 0;
				}
			}
		}

		// Static model collision: one clipmap static model per generated static model (same order), the colliding
		// ones listed in the collision model list. Each entry copies the template entry of the same XModel (else the
		// first template entry) for its flag bytes.
		void build_clip_smodels(clipMap_t* c, const std::vector<smodel_source>& list, zone_memory* mem)
		{
			std::unordered_map<std::string, unsigned int> by_model;
			for (auto i = 0u; i < c->numStaticModels; i++)
			{
				const auto* m = c->staticModelList[i].xmodel;
				if (m && m->name && !by_model.contains(m->name)) by_model[m->name] = i;
			}
			const auto n = static_cast<unsigned int>(list.size());
			auto* models = n ? mem->allocate<cStaticModel_s>(n) : nullptr;
			std::vector<int> colliding;
			std::unordered_map<std::string, XModel*> stubs;
			for (auto i = 0u; i < n; i++)
			{
				const auto& s = list[i];
				auto& m = models[i];
				if (by_model.contains(s.model)) m = c->staticModelList[by_model[s.model]];
				else if (c->numStaticModels) m = c->staticModelList[0];
				else m = {};
				if (!stubs.contains(s.model)) stubs[s.model] = name_stub<XModel>(mem, s.model);
				m.xmodel = stubs[s.model];
				m.hasTransientModel = false;
				m.hasTransientPhysicsAsset = false;
				if (const auto* f = std::getenv("IWZ_CLIP_UNK1")) m.unk1 = std::atoi(f) != 0; // flag-byte experiments
				if (const auto* f = std::getenv("IWZ_CLIP_UNK2")) m.unk2 = std::atoi(f) != 0;
				for (auto a = 0; a < 3; a++)
				{
					m.origin[a] = s.origin[a];
					for (auto k = 0; k < 3; k++) m.invScaledAxis[a][k] = s.axis[k][a] / s.scale; // transpose (stock layout)
				}
				if (s.collides) colliding.push_back(static_cast<int>(i));
			}
			c->numStaticModels = n;
			c->staticModelList = models;
			c->staticModelCollisionModelList.numModels = static_cast<unsigned int>(colliding.size());
			c->staticModelCollisionModelList.staticModelIndex = nullptr;
			if (!colliding.empty())
			{
				c->staticModelCollisionModelList.staticModelIndex = mem->allocate<int>(colliding.size());
				std::memcpy(c->staticModelCollisionModelList.staticModelIndex, colliding.data(), colliding.size() * sizeof(int));
			}
			c->numStaticModelCollisionModelLists = 0;
			c->staticModelCollisionModelLists = nullptr;
			ZONETOOL_INFO("iwzmap: %u clipmap static models, %zu with collision", n, colliding.size());
		}

		void build_clip(clipMap_t* c, const std::string& bsp, const std::string& hkx, const std::vector<smodel_source>& smodels, zone_memory* mem)
		{
			c->name = mem->duplicate_string(bsp);
			build_clip_smodels(c, smodels, mem);
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
					// axes must differ so each float of the shape maps to one axis
					if (std::fabs(v[0] - v[1]) < 1e-2f || std::fabs(v[0] - v[2]) < 1e-2f || std::fabs(v[1] - v[2]) < 1e-2f) continue;
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
					// the donor is a convex box: AABB min/max (twice), 8 vertices (+-x, +-y, +-z) and 6 planes (n, -d), all
					// between the first and last max triplet: rescale every float of the region whose magnitude is a half extent
					const auto at = find_all(from);
					auto patched = 0u;
					if (!at.empty())
					{
						const auto r0 = at.front() >= 16 ? at.front() - 16 : 0, r1 = std::min(size, at.back() + 16);
						for (auto o = r0; o + 4 <= r1; o += 4)
						{
							float f;
							std::memcpy(&f, blob + o, 4);
							for (auto a = 0; a < 3; a++)
							{
								if (std::fabs(std::fabs(f) - from[a]) < 1e-3f)
								{
									const auto g2 = f < 0 ? -to[a] : to[a];
									std::memcpy(blob + o, &g2, 4);
									patched++;
									break;
								}
							}
						}
					}
					tm.physicsAsset = models[d].physicsAsset;
					tm.physicsShapeOverrideIdx = models[d].physicsShapeOverrideIdx;
					tm.flags = models[d].flags;
					ZONETOOL_INFO("iwzmap: generated trigger ?%u takes the entity shape of template ?%u (shape %u, half %.0f %.0f %.0f -> %.0f %.0f %.0f, %u floats)",
						base + k, d, tm.physicsShapeOverrideIdx, db.halfSize[0], db.halfSize[1], db.halfSize[2], g.halfSize[0], g.halfSize[1], g.halfSize[2], patched);
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
			const auto template_triggers = e->trigger.count;
			add_triggers(e, triggers, ents, mem);
			assign_volume_shapes(e, template_triggers, ents, mem); // iwzmap_volumes.cpp: entity shapes of generated triggers
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

			std::string hkx, graph, ents, triggers, relocations, light_ents;
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
			if (std::getenv("IWZ_LOG_STATICS")) log_statics(w, tz, c, cw);
			const auto smodels = read_smodels(source + "\\smodels.txt", mem.get());
			if (!smodels.empty()) utils::io::write_file(source + "\\smodels.bounds.json", smodel_bounds_json(smodels));

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
				auto lights = read_lights(source + "\\lights.txt");
				const auto slots = assign_light_slots(cw, lights);
				std::vector<ComPrimaryLight> before;
				for (const auto& l : lights) before.push_back(cw->primaryLights[l.slot]);
				build_com_lights(cw, lights);
				light_ents = light_ents_text(cw, lights);
				build_gfx(w, tz, src, smodels, slots, map, bsp, mem.get());
				build_gfx_lights(w, before, cw, lights, src, mem.get());
				voxel_layout lay;
				if (const auto* v = std::getenv("IWZ_VOXEL_LAYOUT")) std::sscanf(v, "%d,%d", &lay.column_x_major, &lay.perm);
				lay.perm = std::clamp(lay.perm, 0, 5);
				build_voxel_tree(w, cw, lights, src, lay, mem.get());
				const auto zones = read_lighting(source + "\\lighting.txt", src);
				if (!zones.empty())
				{
					auto& grid = w->lightGrid.probeData;
					const auto a = relight_probes(grid.probePositions, grid.probes, grid.probeCount, zones);
					const auto b = relight_probes(grid.gpuVisibleProbePositions, grid.gpuVisibleProbesData, grid.gpuVisibleProbesCount, zones);
					ZONETOOL_INFO("iwzmap: relit %u light grid probes (%u gpu visible) from %zu lighting zones", a, b, zones.size());
					{
						// probe rooms: the generator's zone boxes (probes.txt, lighting.txt format); a baked lighting.txt has
						// thousands of chart rooms
						const auto probe_rooms = read_lighting(source + "\\probes.txt", src);
						neutralize_reflection_probes(w, probe_rooms.empty() ? zones : probe_rooms, src, mem.get());
					}
					// points outside the template's probe tetrahedra (the generated rooms lie where the template has no
					// probes) take their light grid zone's fallback probe: give every fallback the generated ambient
					// (first lighting line) instead of the template's value (engine runs g2..g11: script models dark)
					for (auto zi = 0u; zi < grid.zoneCount && grid.zones; zi++)
					{
						auto& fb = grid.zones[zi].fallbackProbeData;
						ZONETOOL_INFO("iwzmap: light grid zone %u: %u probes, %u tetrahedrons, fallback c0 %.3f %.3f %.3f -> %.3f %.3f %.3f", zi,
							grid.zones[zi].numProbes, grid.zones[zi].numTetrahedrons, half_to_float(fb.coeffs[0]), half_to_float(fb.coeffs[9]),
							half_to_float(fb.coeffs[18]), zones[0].rgb[0], zones[0].rgb[1], zones[0].rgb[2]);
						if (std::getenv("IWZ_KEEP_FALLBACK_PROBE")) continue;
						for (auto chan_i = 0; chan_i < 3; chan_i++)
						{
							for (auto k = 0; k < 9; k++) fb.coeffs[chan_i * 9 + k] = 0;
							fb.coeffs[chan_i * 9 + 0] = float_to_half(zones[0].rgb[chan_i]);
							fb.coeffs[chan_i * 9 + 2] = float_to_half(zones[0].rgb[chan_i] * zones[0].up);
						}
						fb.coeffs[27] = float_to_half(1.0f);
					}
				}
			}
			build_clip(c, bsp, hkx, smodels, mem.get());
			build_ents(e, bsp, ents + light_ents, triggers, relocations, &me, src, mem.get());
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
			if (g_probe_image)
			{
				gfx_image::dump(g_probe_image);
				ZONETOOL_INFO("iwzmap: probes: wrote %s into the map", g_probe_image->name);
			}
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

	bool smodel_bounds(const std::string& template_zone, const std::string& smodels_path, const std::string& out_path)
	{
		try
		{
			auto mem = std::make_unique<zone_memory>(256ull * 1024 * 1024);
			filesystem::set_fastfile(template_zone);
			const auto list = read_smodels(smodels_path, mem.get(), true);
			utils::io::write_file(out_path, smodel_bounds_json(list));
			ZONETOOL_INFO("iwzmap: wrote %zu static model footprints to %s", list.size(), out_path.data());
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
