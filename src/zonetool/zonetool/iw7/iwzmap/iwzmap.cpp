#include <std_include.hpp>
#include "zonetool/iw7/zonetool.hpp"
#include "iwzmap.hpp"

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
			ZONETOOL_INFO("template gfxworld: surfaces %u cells %d planes %d nodes %d smodels %u staticSurf %u visData %u "
				"primaryLights %u lightmaps %u probes %u models %d skies %d umbra %u",
				w->surfaceCount, w->dpvsPlanes.cellCount, w->planeCount, w->nodeCount, w->dpvs.smodelCount,
				w->dpvs.staticSurfaceCount, w->dpvs.surfaceVisDataCount, w->primaryLightCount, w->draw.lightMapCount,
				w->draw.reflectionProbeData.reflectionProbeCount, w->modelCount, w->skyCount, w->umbraTomeSize);
		}

		void build_gfx(GfxWorld* w, GfxWorldTransientZone* tz, const world_source& src, const std::string& map,
			const std::string& bsp, zone_memory* mem)
		{
			log_template(w);

			// surface template per material (flags, sort key and the unknown bytes of a stock surface
			// using the same material), falling back to the first opaque template surface
			std::unordered_map<std::string, GfxSurface> by_material;
			for (auto i = 0u; i < w->surfaceCount; i++)
			{
				const auto& s = w->dpvs.surfaces[i];
				if (s.material && s.material->name && !by_material.contains(s.material->name))
				{
					by_material[s.material->name] = s;
				}
			}
			const auto fallback = w->dpvs.surfaces[w->dpvs.litOpaqueSurfsBegin];

			w->name = mem->duplicate_string(bsp);
			w->baseName = mem->duplicate_string(map);

			const auto count = static_cast<unsigned int>(src.surfaces.size());
			auto* surfaces = mem->allocate<GfxSurface>(count);
			auto* bounds = mem->allocate<GfxSurfaceBounds>(count);
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

			for (auto i = 0u; i < count; i++)
			{
				const auto& s = src.surfaces[i];
				const auto& mat_name = src.materials[s.material];
				surfaces[i] = by_material.contains(mat_name) ? by_material[mat_name] : fallback;
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
			w->dpvs.staticSurfaceCount = count;
			w->dpvs.sortedSurfIndex = mem->allocate<unsigned int>(count);
			for (auto i = 0u; i < count; i++) w->dpvs.sortedSurfIndex[i] = i;
			w->dpvs.litOpaqueSurfsBegin = 0;
			w->dpvs.litOpaqueSurfsEnd = count;
			w->dpvs.litDecalSurfsBegin = w->dpvs.litDecalSurfsEnd = count;
			w->dpvs.litTransSurfsBegin = w->dpvs.litTransSurfsEnd = count;
			w->dpvs.emissiveSurfsBegin = w->dpvs.emissiveSurfsEnd = count;
			w->dpvs.surfaceVisDataCount = count;
			w->dpvs.surfaceCastsSunShadow = mem->allocate<unsigned int>(count);
			for (auto i = 0u; i < count; i++) w->dpvs.surfaceCastsSunShadow[i] = 0xFFFFFFFF;
			w->dpvs.sunShadowOptCount = 0;
			w->dpvs.sunSurfVisDataCount = 0;
			w->dpvs.surfaceCastsSunShadowOpt = nullptr;

			// no static models
			w->dpvs.smodelCount = 0;
			w->dpvs.smodelVisDataCount = 0;
			w->dpvs.lodData = mem->allocate<unsigned int>(1);
			w->dpvs.smodelInsts = nullptr;
			w->dpvs.smodelDrawInsts = nullptr;
			w->dpvs.sortedSmodelIndices = nullptr;
			w->dpvs.decalVisDataCount = 0;
			w->dpvs.volumetricVisDataCount = 0;

			// one cell, one BSP leaf (node = {cellIndex + 1, rightChildOffset})
			const auto world_bounds = make_bounds(src.mins, src.maxs);
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

			// sky: keep the template image, no sky surfaces
			for (auto i = 0; i < w->skyCount; i++)
			{
				w->skies[i].skySurfCount = 0;
				w->skies[i].skyStartSurfs = nullptr;
			}

			// world brush model 0 covers every surface; no other brush models
			w->modelCount = 1;
			w->models[0].bounds = world_bounds;
			w->models[0].startSurfIndex = 0;
			w->models[0].surfaceCount = static_cast<unsigned short>(count);
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
			w->draw.decalVolumeCollectionCount = 0;
			w->draw.decalVolumeCollections = nullptr;
			w->draw.volumetrics.volumetricCount = 0;
			w->draw.volumetrics.volumetrics = nullptr;
			w->heightfieldCount = 0;
			w->heightfields = nullptr;
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

			// indices
			w->draw.indexCount = static_cast<unsigned int>(src.indices.size());
			w->draw.indices = mem->allocate<unsigned short>(src.indices.size());
			std::memcpy(w->draw.indices, src.indices.data(), src.indices.size() * sizeof(std::uint16_t));

			// single transient zone with the vertices and a one-node aabb tree per cell
			w->draw.transientZoneCount = 1;
			for (auto i = 1; i < 32; i++) w->draw.transientZones[i] = nullptr;
			w->draw.transientZones[0] = name_stub<GfxWorldTransientZone>(mem, map);

			tz->name = mem->duplicate_string(map);
			tz->transientZoneIndex = 0;
			tz->vertexCount = static_cast<unsigned int>(src.vertices.size());
			tz->vd.vertices = mem->allocate<GfxWorldVertex>(src.vertices.size());
			std::memcpy(tz->vd.vertices, src.vertices.data(), src.vertices.size() * sizeof(GfxWorldVertex));
			tz->vertexLayerDataSize = 0;
			tz->vld.data = nullptr;
			tz->cellCount = 1;
			tz->aabbTreeCounts = mem->allocate<GfxCellTreeCount>(1);
			tz->aabbTreeCounts[0].aabbTreeCount = 1;
			tz->aabbTrees = mem->allocate<GfxCellTree>(1);
			tz->aabbTrees[0].aabbTree = mem->allocate<GfxAabbTree>(1);
			auto& node = tz->aabbTrees[0].aabbTree[0];
			node.bounds = world_bounds;
			node.childCount = 0;
			node.surfaceCount = static_cast<unsigned short>(count);
			node.startSurfIndex = 0;
			node.smodelIndexCount = 0;
			node.smodelIndexes = nullptr;
			node.childrenOffset = 0;
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
			for (auto i = 0; i < c->stageCount; i++)
			{
				c->stages[i].name = mem->duplicate_string(c->stages[i].name ? c->stages[i].name : "");
			}
		}

		void build_ents(MapEnts* e, const std::string& bsp, const std::string& ents, zone_memory* mem)
		{
			e->name = mem->duplicate_string(bsp);
			e->numEntityChars = static_cast<int>(ents.size() + 1);
			e->entityString = mem->allocate<char>(ents.size() + 1);
			std::memcpy(e->entityString, ents.data(), ents.size());

			// nothing of the template may appear in the new world: clear each list with its pointers,
			// so that the writer and the engine loader agree on an empty list
			std::memset(&e->clientTrigger, 0, sizeof(e->clientTrigger));
			std::memset(&e->clientTriggerBlend, 0, sizeof(e->clientTriggerBlend));
			std::memset(&e->spawnList, 0, sizeof(e->spawnList));
			std::memset(&e->splineList, 0, sizeof(e->splineList));
			e->dynEntCount[0] = e->dynEntCount[1] = 0;
			e->dynEntCountTotal = 0;
			std::memset(e->dynEntDefList, 0, sizeof(e->dynEntDefList));
			std::memset(e->dynEntPoseList, 0, sizeof(e->dynEntPoseList));
			std::memset(e->dynEntClientList, 0, sizeof(e->dynEntClientList));
			std::memset(e->dynEntGlobalIdList, 0, sizeof(e->dynEntGlobalIdList));
			e->dynEntTransientGroupCount = 0;
			e->dynEntTransientGroups = nullptr;
			std::memset(e->dynEntTransientGroupRuntime, 0, sizeof(e->dynEntTransientGroupRuntime));
			std::memset(e->dynEntTransientGroupState, 0, sizeof(e->dynEntTransientGroupState));
			e->unk3Count = 0;
			e->unk3 = nullptr;
			e->clientEntAnchorCount = 0;
			e->clientEntAnchors = nullptr;
			std::memset(&e->scriptableMapEnts, 0, sizeof(e->scriptableMapEnts));
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
			n->numObstacleBounds = 0;
			n->obstacleBounds = nullptr;
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

			std::string hkx, graph, ents;
			if (!utils::io::read_file(source + "\\colmap.hkx", &hkx)) throw std::runtime_error("missing colmap.hkx");
			if (!utils::io::read_file(source + "\\navgraph.bin", &graph)) throw std::runtime_error("missing navgraph.bin");
			if (!utils::io::read_file(source + "\\ents.txt", &ents)) throw std::runtime_error("missing ents.txt");

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

			if (!w || !tz || !c || !cw || !e || !n || !gw || !fw)
			{
				throw std::runtime_error("template world assets are incomplete");
			}

			build_gfx(w, tz, src, map, bsp, mem.get());
			build_clip(c, bsp, hkx, mem.get());
			build_ents(e, bsp, ents, mem.get());
			build_nav(n, bsp, graph, mem.get());

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

			filesystem::set_fastfile(map);
			gfx_world::dump(w);
			gfx_world_tr::dump(tz);
			clip_map::dump(c);
			com_world::dump(cw);
			map_ents::dump(e);
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
