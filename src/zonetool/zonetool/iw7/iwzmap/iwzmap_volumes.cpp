#include <std_include.hpp>
#include "zonetool/iw7/zonetool.hpp"
#include "iwzmap_volumes.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <regex>
#include <set>
#include <unordered_map>

namespace zonetool::iw7::iwzmap
{
	namespace
	{
		constexpr float shape_scale = 32.0f; // entity shapes are stored in units / 32
		constexpr float shape_tol = 1e-3f;   // shape space

		// hk_2014 binary packfile, 64-bit layout rules (8 1 0 1), read in place: fixups resolve pointers, virtual fixups
		// name object classes. Only float values are rewritten, so every fixup stays valid.
		struct packfile
		{
			unsigned char* d = nullptr;
			std::size_t size = 0;
			std::uint32_t st = 0, data_size = 0, contents = 0;
			std::unordered_map<std::uint32_t, std::uint32_t> local, global;
			std::unordered_map<std::uint32_t, std::string> classes;

			bool in(std::uint32_t off, std::uint32_t n) const { return off <= data_size && n <= data_size - off; }
			template <typename T> T get(std::uint32_t off) const
			{
				T v{};
				if (in(off, sizeof(T))) std::memcpy(&v, d + st + off, sizeof(T));
				return v;
			}
			float* f(std::uint32_t off) const { return in(off, 16) ? reinterpret_cast<float*>(d + st + off) : nullptr; }
			const std::string& cls(std::uint32_t off) const
			{
				static const std::string none;
				const auto it = classes.find(off);
				return it == classes.end() ? none : it->second;
			}
			bool ptr(const std::unordered_map<std::uint32_t, std::uint32_t>& m, std::uint32_t at, std::uint32_t* out) const
			{
				const auto it = m.find(at);
				if (it == m.end()) return false;
				*out = it->second;
				return true;
			}

			bool parse(unsigned char* data, std::size_t len)
			{
				d = data;
				size = len;
				static const unsigned char magic[8] = {0x57, 0xe0, 0xe0, 0x57, 0x10, 0xc0, 0xc0, 0x10};
				if (len < 0x40 || std::memcmp(d, magic, 8) || d[0x10] != 8) return false;
				std::int32_t nsec, csec, coff;
				std::memcpy(&nsec, d + 0x14, 4);
				std::memcpy(&csec, d + 0x18, 4);
				std::memcpy(&coff, d + 0x1c, 4);
				std::int16_t pad;
				std::memcpy(&pad, d + 0x3e, 2);
				const std::size_t hdr = 0x40 + (pad > 0 ? pad : 0);
				if (nsec < 3 || csec < 0 || csec >= nsec || hdr + 0x40ull * nsec > len) return false;
				struct section { std::uint32_t start, local, glob, virt, exp, imp, end; };
				std::vector<section> secs(nsec);
				for (auto i = 0; i < nsec; i++) std::memcpy(&secs[i], d + hdr + 0x40ull * i + 20, sizeof(section));
				for (const auto& s : secs)
					if (s.start > len || s.end > len - s.start) return false;
				// class names: [u32 signature][0x09][name\0] ...
				std::unordered_map<std::uint32_t, std::string> names;
				const auto& cn = secs[0];
				for (std::size_t p = cn.start; p + 5 < cn.start + cn.local && d[p + 4] == 9;)
				{
					const auto* s = reinterpret_cast<const char*>(d + p + 5);
					const auto n = strnlen(s, cn.start + cn.local - p - 5);
					names[static_cast<std::uint32_t>(p + 5 - cn.start)] = std::string(s, n);
					p += 5 + n + 1;
				}
				const auto& ds = secs[csec];
				st = ds.start;
				data_size = ds.local;
				contents = static_cast<std::uint32_t>(coff);
				auto read = [&](std::uint32_t from, std::uint32_t to, std::uint32_t stride, auto&& fn) {
					for (std::size_t o = st + from; o + stride <= st + to; o += stride)
					{
						std::int32_t v[3]{};
						std::memcpy(v, d + o, stride);
						if (v[0] != -1) fn(v);
					}
				};
				read(ds.local, ds.glob, 8, [&](const std::int32_t* v) { local[v[0]] = v[1]; });
				read(ds.glob, ds.virt, 12, [&](const std::int32_t* v) { if (v[1] == csec) global[v[0]] = v[2]; });
				read(ds.virt, ds.exp, 12, [&](const std::int32_t* v) {
					const auto it = names.find(v[2]);
					classes[v[0]] = it == names.end() ? "?" : it->second;
				});
				return cls(contents) == "HavokPhysicsShapeList";
			}

			// HavokPhysicsShapeList: first member is hkArray<hknpShape*> (one entry per physicsShapeOverrideIdx)
			std::vector<std::int64_t> shapes() const
			{
				std::vector<std::int64_t> out;
				std::uint32_t arr = 0;
				if (!ptr(local, contents, &arr)) return out;
				const auto n = get<std::uint32_t>(contents + 8);
				for (auto i = 0u; i < n; i++)
				{
					std::uint32_t s = 0;
					out.push_back(ptr(global, arr + 8 * i, &s) ? static_cast<std::int64_t>(s) : -1);
				}
				return out;
			}
		};

		// one hknpDynamicCompoundShape instancing one axis-aligned hknpConvexPolytopeShape box at the origin
		struct box_shape
		{
			std::uint32_t compound = 0, poly = 0, verts = 0, planes = 0;
			std::vector<std::uint32_t> bounds; // min vector offsets (max = +16): compound AABB, tree nodes
			std::array<float, 3> half{};       // shape space
		};

		bool near(float a, float b) { return std::fabs(a - b) <= shape_tol * std::max(1.0f, std::fabs(b)); }

		bool decode_box(const packfile& pk, std::int64_t shape, box_shape* out, std::string* why)
		{
			box_shape b{};
			if (shape < 0 || pk.cls(static_cast<std::uint32_t>(shape)) != "hknpDynamicCompoundShape") { *why = "not a dynamic compound"; return false; }
			b.compound = static_cast<std::uint32_t>(shape);
			std::uint32_t inst = 0;
			if (!pk.ptr(pk.local, b.compound + 0x60, &inst) || pk.get<std::uint32_t>(b.compound + 0x68) != 1) { *why = "not one instance"; return false; }
			for (auto r = 0; r < 3; r++)
			{
				const auto* row = pk.f(inst + 16 * r);
				if (!row) { *why = "instance out of range"; return false; }
				for (auto c = 0; c < 3; c++)
					if (std::fabs(row[c] - (r == c ? 1.0f : 0.0f)) > 1e-5f) { *why = "rotated instance"; return false; }
			}
			const auto* t = pk.f(inst + 0x30);
			const auto* s = pk.f(inst + 0x40);
			if (!t || !s) { *why = "instance out of range"; return false; }
			for (auto a = 0; a < 3; a++)
				if (t[a] != 0.0f || std::fabs(s[a] - 1.0f) > 1e-5f) { *why = "offset or scaled instance"; return false; }
			if (!pk.ptr(pk.global, inst + 0x50, &b.poly) || pk.cls(b.poly) != "hknpConvexPolytopeShape") { *why = "instance is not a polytope"; return false; }
			if (pk.get<float>(b.poly + 0x14) != 0.0f) { *why = "convex radius"; return false; }
			if (pk.get<std::uint16_t>(b.poly + 0x30) != 8 || pk.get<std::uint16_t>(b.poly + 0x40) != 6) { *why = "not 8 vertices / 6 planes"; return false; }
			b.verts = b.poly + 0x30 + pk.get<std::uint16_t>(b.poly + 0x32);
			b.planes = b.poly + 0x40 + pk.get<std::uint16_t>(b.poly + 0x42);
			if (!pk.in(b.verts, 8 * 16) || !pk.in(b.planes, 6 * 16)) { *why = "polytope arrays out of range"; return false; }
			for (auto i = 0; i < 8; i++)
				for (auto a = 0; a < 3; a++) b.half[a] = std::max(b.half[a], std::fabs(pk.f(b.verts + 16 * i)[a]));
			for (auto i = 0; i < 8; i++)
				for (auto a = 0; a < 3; a++)
					if (!near(std::fabs(pk.f(b.verts + 16 * i)[a]), b.half[a])) { *why = "vertices are not one box"; return false; }
			for (auto i = 0; i < 6; i++)
			{
				const auto* p = pk.f(b.planes + 16 * i);
				auto axis = -1, axes = 0;
				for (auto a = 0; a < 3; a++)
					if (std::fabs(std::fabs(p[a]) - 1.0f) < 1e-4f) { axis = a; axes++; }
					else if (std::fabs(p[a]) > 1e-4f) axes = 9;
				if (axes != 1 || !near(-p[3], b.half[axis])) { *why = "planes are not the box faces"; return false; }
			}
			auto is_box_bounds = [&](std::uint32_t at) {
				const auto* lo = pk.f(at);
				const auto* hi = pk.f(at + 16);
				if (!lo || !hi) return false;
				for (auto a = 0; a < 3; a++)
					if (!near(lo[a], -b.half[a]) || !near(hi[a], b.half[a])) return false;
				return true;
			};
			if (!is_box_bounds(b.compound + 0x80)) { *why = "compound aabb is not the box"; return false; }
			b.bounds.push_back(b.compound + 0x80);
			std::uint32_t data = 0, nodes = 0;
			if (!pk.ptr(pk.global, b.compound + 0xc0, &data) || pk.cls(data) != "hknpDynamicCompoundShapeData" || !pk.ptr(pk.local, data + 0x10, &nodes))
			{
				*why = "no compound tree";
				return false;
			}
			const auto nn = pk.get<std::uint32_t>(data + 0x18);
			auto tree = 0;
			for (auto i = 0u; i < nn && i < 64; i++)
			{
				const auto at = nodes + 32 * i;
				if (!pk.in(at, 32)) { *why = "tree out of range"; return false; }
				const auto* v = pk.f(at);
				const auto* w = pk.f(at + 16);
				auto zero = true;
				for (auto a = 0; a < 3; a++) zero = zero && v[a] == 0.0f && w[a] == 0.0f;
				if (zero) continue;
				if (!is_box_bounds(at)) { *why = "tree node is not the box"; return false; }
				b.bounds.push_back(at);
				tree++;
			}
			if (!tree) { *why = "tree has no box node"; return false; }
			*out = b;
			return true;
		}

		void write_box(const packfile& pk, const box_shape& b, const std::array<float, 3>& half)
		{
			for (auto i = 0; i < 8; i++)
			{
				auto* v = pk.f(b.verts + 16 * i);
				for (auto a = 0; a < 3; a++) v[a] = v[a] < 0.0f ? -half[a] : half[a];
			}
			for (auto i = 0; i < 6; i++)
			{
				auto* p = pk.f(b.planes + 16 * i);
				for (auto a = 0; a < 3; a++)
					if (std::fabs(std::fabs(p[a]) - 1.0f) < 1e-4f) p[3] = -half[a];
			}
			for (const auto at : b.bounds)
			{
				auto* lo = pk.f(at);
				auto* hi = pk.f(at + 16);
				for (auto a = 0; a < 3; a++) { lo[a] = -half[a]; hi[a] = half[a]; }
			}
		}
	}

	unsigned int assign_volume_shapes(MapEnts* e, unsigned int first_generated, const std::string& ents, zone_memory* mem)
	{
		if (!e || e->trigger.count <= first_generated) return 0;
		const auto generated = e->trigger.count - first_generated;
		if (std::getenv("IWZ_NO_VOLUME_SHAPES"))
		{
			ZONETOOL_WARNING("iwzmap: IWZ_NO_VOLUME_SHAPES: %u generated trigger models keep no entity shape (ispointinvolume/istouching false)", generated);
			return 0;
		}
		if (!e->havokEntsShapeData || !e->havokEntsShapeDataSize)
		{
			ZONETOOL_WARNING("iwzmap: no entity shape data: %u generated trigger models have no entity shape (ispointinvolume/istouching false)", generated);
			return 0;
		}
		if (!std::getenv("IWZ_KEEP_ENTSHAPES"))
			ZONETOOL_WARNING("iwzmap: volume shapes are written but IWZ_KEEP_ENTSHAPES is not set: the entity shape data will be cleared");

		// own copy: the template asset stays intact
		const auto size = static_cast<std::size_t>(e->havokEntsShapeDataSize);
		auto* blob = mem->allocate<char>(size);
		std::memcpy(blob, e->havokEntsShapeData, size);
		packfile pk;
		if (!pk.parse(reinterpret_cast<unsigned char*>(blob), size))
		{
			ZONETOOL_ERROR("iwzmap: entity shape data is not a HavokPhysicsShapeList packfile; generated volumes get no shape");
			return 0;
		}
		const auto shapes = pk.shapes();

		// shapes still in use: sub-models (brush models, door blockers), client triggers, template triggers the ents name
		std::set<unsigned int> busy;
		for (auto i = 0u; i < e->numSubModels && e->cmodels; i++) busy.insert(e->cmodels[i].physicsShapeOverrideIdx);
		for (auto i = 0u; i < e->clientTrigger.trigger.count && e->clientTrigger.trigger.models; i++)
			if (e->clientTrigger.trigger.models[i].physicsAsset) busy.insert(e->clientTrigger.trigger.models[i].physicsShapeOverrideIdx);
		static const std::regex ref("\"\\?([0-9]+)\"");
		for (std::sregex_iterator it(ents.begin(), ents.end(), ref), end; it != end; ++it)
		{
			const auto n = static_cast<unsigned int>(std::stoul((*it)[1].str()));
			if (n < first_generated && e->trigger.models[n].physicsAsset) busy.insert(e->trigger.models[n].physicsShapeOverrideIdx);
		}

		struct donor { unsigned int model; box_shape box; };
		std::vector<donor> donors;
		std::set<unsigned int> taken_shapes;
		for (auto m = 0u; m < first_generated; m++)
		{
			const auto& tm = e->trigger.models[m];
			if (!tm.physicsAsset || tm.hullCount != 1 || busy.count(tm.physicsShapeOverrideIdx) || taken_shapes.count(tm.physicsShapeOverrideIdx)) continue;
			if (tm.physicsShapeOverrideIdx >= shapes.size()) continue;
			const auto& hb = e->trigger.hulls[tm.firstHull].bounds;
			if (hb.midPoint[0] != 0.0f || hb.midPoint[1] != 0.0f || hb.midPoint[2] != 0.0f) continue;
			donor dn{m, {}};
			std::string why;
			if (!decode_box(pk, shapes[tm.physicsShapeOverrideIdx], &dn.box, &why)) continue;
			auto same = true;
			for (auto a = 0; a < 3; a++) same = same && std::fabs(dn.box.half[a] * shape_scale - hb.halfSize[a]) <= 0.5f;
			if (!same) continue;
			taken_shapes.insert(tm.physicsShapeOverrideIdx);
			donors.push_back(dn);
		}

		auto shaped = 0u, next = 0u;
		for (auto k = first_generated; k < e->trigger.count; k++)
		{
			auto& tm = e->trigger.models[k];
			const auto& hull = e->trigger.hulls[tm.firstHull];
			if (tm.hullCount != 1 || hull.bounds.midPoint[0] != 0.0f || hull.bounds.midPoint[1] != 0.0f || hull.bounds.midPoint[2] != 0.0f)
			{
				ZONETOOL_WARNING("iwzmap: generated trigger ?%u has %u hulls or an offset hull: no box entity shape", k, tm.hullCount);
				continue;
			}
			if (next >= donors.size())
			{
				ZONETOOL_ERROR("iwzmap: generated trigger ?%u: no free template box shape left (%zu donors)", k, donors.size());
				continue;
			}
			const auto& dn = donors[next++];
			const auto& dm = e->trigger.models[dn.model];
			const std::array<float, 3> half = {hull.bounds.halfSize[0] / shape_scale, hull.bounds.halfSize[1] / shape_scale, hull.bounds.halfSize[2] / shape_scale};
			write_box(pk, dn.box, half);
			box_shape check{};
			std::string why;
			const auto ok = decode_box(pk, shapes[dm.physicsShapeOverrideIdx], &check, &why) && near(check.half[0], half[0]) &&
				near(check.half[1], half[1]) && near(check.half[2], half[2]);
			if (!ok)
			{
				ZONETOOL_ERROR("iwzmap: generated trigger ?%u: rewritten shape %u does not verify (%s)", k, dm.physicsShapeOverrideIdx, why.data());
				continue;
			}
			tm.physicsAsset = dm.physicsAsset;
			tm.physicsShapeOverrideIdx = dm.physicsShapeOverrideIdx;
			tm.flags = dm.flags;
			shaped++;
			ZONETOOL_INFO("iwzmap: volume shape ?%u <- template ?%u shape %u (%s): box half %.1f %.1f %.1f (was %.1f %.1f %.1f), 8 vertices 6 planes %zu bounds verified",
				k, dn.model, tm.physicsShapeOverrideIdx, tm.physicsAsset->name ? tm.physicsAsset->name : "-",
				hull.bounds.halfSize[0], hull.bounds.halfSize[1], hull.bounds.halfSize[2],
				dn.box.half[0] * shape_scale, dn.box.half[1] * shape_scale, dn.box.half[2] * shape_scale, check.bounds.size());
		}
		e->havokEntsShapeData = blob;
		ZONETOOL_INFO("iwzmap: volume shapes: %u of %u generated trigger models shaped (%zu free template box shapes, %zu busy shapes, %zu entity shapes)",
			shaped, generated, donors.size(), busy.size(), shapes.size());
		return shaped;
	}
}
