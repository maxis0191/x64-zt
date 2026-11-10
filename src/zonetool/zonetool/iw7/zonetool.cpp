#include <std_include.hpp>
#include "zonetool.hpp"

#include "converter/converter.hpp"
#include "assets/physics_asset.hpp"
#include "assets/sound_bank_subset_rules.hpp"
#include "common/havok.hpp"
#include "iwzmap/iwzmap.hpp"

#include "../utils/gsc.hpp"
#include "../utils/csv_generator.hpp"

#include <utils/io.hpp>
#include <utils/flags.hpp>
#include <utils/cryptography.hpp>

namespace zonetool::iw7
{
	struct dump_params
	{
		game::game_mode target;
		std::string zone;
		bool valid;
		std::unordered_set<XAssetType> filter;
	};

	zonetool_globals_t globals{};
	std::vector<std::pair<XAssetType, std::string>> referenced_assets;
	std::unordered_set<XAssetType> asset_type_filter;
	std::unordered_set<std::string> enemy_image_names;
	std::unordered_set<std::string> enemy_images_dumped;
	std::map<std::string, std::filesystem::path> enemy_image_sources;
	size_t enemy_image_capture_count = 0;
	std::string enemy_image_pack;
	bool enemy_image_capture = false;
	std::unordered_set<std::string> enemy_physics_names;
	std::unordered_map<std::string, std::vector<std::filesystem::path>> enemy_physics_sources;
	size_t enemy_physics_capture_count = 0;
	std::string enemy_physics_pack;
	bool enemy_physics_capture = false;
	bool enemy_pack_build_active = false;

	std::unordered_set<std::pair<std::uint32_t, std::string>, pair_hash<std::uint32_t, std::string>> ignore_assets;

	const char* get_asset_name(XAssetType type, void* pointer)
	{
		XAssetHeader header{ .data = pointer };
		return DB_GetXAssetHeaderName(type, header);
	}

	const char* get_asset_name(XAsset* asset)
	{
		return DB_GetXAssetHeaderName(asset->type, asset->header);
	}

	void set_asset_name(XAsset* asset, const char* name)
	{
		DB_SetXAssetHeaderName(asset->type, asset->header, name);
	}

	const char* type_to_string(XAssetType type)
	{
		return g_assetNames[type];
	}

	std::int32_t type_to_int(std::string type)
	{
		for (std::int32_t i = 0; i < ASSET_TYPE_COUNT; i++)
		{
			if (g_assetNames[i] == type)
				return i;
		}

		return -1;
	}

	bool is_valid_asset_type(const std::string& type)
	{
		return type_to_int(type) >= 0;
	}

	bool zone_exists(const std::string& zone)
	{
		return DB_FileExists(zone.data(), 0);
	}

	bool is_referenced_asset(XAsset* asset)
	{
		if (get_asset_name(asset)[0] == ',')
		{
			return true;
		}
		return false;
	}

	void wait_for_database()
	{
		// wait for database to be ready
		while (!WaitForSingleObject(*reinterpret_cast<HANDLE*>(0x14602BD40), 0) == 0)
		{
			Sleep(5);
		}
	}

	XAssetEntry* db_find_x_asset_entry(XAssetType type, const char* name)
	{
		return utils::hook::invoke<XAssetEntry*>(0x1403B57F0, reinterpret_cast<void*>(0x1453E7370), name, type);
	}

	XAssetHeader db_find_x_asset_header(XAssetType type, const char* name, int create_default)
	{
		const auto header = zonetool::db_find_x_asset_header<XAssetHeader>(type, name, create_default);
		if (enemy_pack_build_active && (!header.data || DB_IsXAssetDefault(type, name)))
			throw std::runtime_error("Required IW7 enemy database asset is missing or default: " + std::string(name ? name : "<null>"));
		return header;
	}

	XAssetHeader db_find_x_asset_header_safe(XAssetType type, const std::string& name)
	{
		const auto header = zonetool::db_find_x_asset_header_safe<XAssetHeader, XAssetEntry>(type, name);
		if (enemy_pack_build_active && (!header.data || DB_IsXAssetDefault(type, name.c_str())))
			throw std::runtime_error("Required IW7 enemy database asset is missing or default: " + name);
		return header;
	}

	void DB_EnumXAssets(const XAssetType type,
		const std::function<void(XAssetHeader)>& callback, const bool includeOverride)
	{
		DB_EnumXAssets_Internal(type, static_cast<void(*)(XAssetHeader, void*)>([](XAssetHeader header, void* data)
		{
			const auto& cb = *static_cast<const std::function<void(XAssetHeader)>*>(data);
			cb(header);
		}), &callback, includeOverride);
	}

	bool Material_TechSetHasTechnique(const MaterialTechniqueSet* techSet, MaterialTechniqueType techType)
	{
		const int techniqueIndex = techType >> 6;
		const uint64_t techniqueBit = 1ULL << (static_cast<std::uint32_t>(techType) & TECHNIQUE_MASK);

		if (techType >= TECHNIQUE_COUNT)
		{
			printf("techType doesn't index TECHNIQUE_COUNT\n\t%i not in [0, %i)\n", techType, TECHNIQUE_COUNT);
			__debugbreak();
		}

		if (techniqueIndex >= NUM_TECHNIQUE_MASK_ELEMS)
		{
			__debugbreak();
		}

		return (techSet->techniqueMask[techniqueIndex] & techniqueBit) != 0;
	}

	void dump_asset_h1(XAsset* asset)
	{
		utils::memory::allocator allocator;

#define DUMP_ASSET_REGULAR(__type__,___,__struct__) \
		if (asset->type == __type__) \
		{ \
			if(IS_DEBUG) ZONETOOL_INFO("Dumping asset \"%s\" of type %s.", get_asset_name(asset), type_to_string(asset->type)); \
			auto asset_ptr = reinterpret_cast<__struct__*>(asset->header.data); \
			___::dump(asset_ptr); \
		}

#define DUMP_ASSET_NO_CONVERT(__type__,___,__struct__) \
		if (asset->type == __type__) \
		{ \
			if(IS_DEBUG) ZONETOOL_INFO("Dumping asset \"%s\" of type %s.", get_asset_name(asset), type_to_string(asset->type)); \
			auto asset_ptr = reinterpret_cast<zonetool::h1::__struct__*>(asset->header.data); \
			zonetool::h1::___::dump(asset_ptr); \
		}

#define DUMP_ASSET_CONVERT(__type__,__namespace__,__struct__) \
		if (asset->type == __type__) \
		{ \
			if(IS_DEBUG) ZONETOOL_INFO("Converting and dumping asset \"%s\" of type %s.", get_asset_name(asset), type_to_string(asset->type)); \
			auto asset_ptr = reinterpret_cast<__struct__*>(asset->header.data); \
			converter::h1::__namespace__::dump(asset_ptr); \
		}

		try
		{
			DUMP_ASSET_CONVERT(ASSET_TYPE_IMAGE, gfximage, GfxImage);
			DUMP_ASSET_CONVERT(ASSET_TYPE_MATERIAL, material, Material);
			//DUMP_ASSET_CONVERT(ASSET_TYPE_XANIMPARTS, xanim, XAnimParts);
			DUMP_ASSET_CONVERT(ASSET_TYPE_XMODEL, xmodel, XModel);
			DUMP_ASSET_CONVERT(ASSET_TYPE_XMODEL_SURFS, xsurface, XModelSurfs);
		}
		catch (std::exception& ex)
		{
			ZONETOOL_FATAL("A fatal exception occured while dumping zone \"%s\", exception was: \n%s", filesystem::get_fastfile().data(), ex.what());
		}

#undef DUMP_ASSET_CONVERT
#undef DUMP_ASSET_NO_CONVERT
#undef DUMP_ASSET_REGULAR
	}

	void dump_asset_iw7(XAsset* asset)
	{
#define DUMP_ASSET(__type__,___,__struct__) \
		if (asset->type == __type__) \
		{ \
			if(IS_DEBUG) ZONETOOL_INFO("Dumping asset \"%s\" of type %s.", get_asset_name(asset), type_to_string(asset->type)); \
			auto asset_ptr = reinterpret_cast<__struct__*>(asset->header.data); \
			___::dump(asset_ptr); \
		}

		try
		{
			// dump assets
			DUMP_ASSET(ASSET_TYPE_DDL, ddl, DDLFile);
			DUMP_ASSET(ASSET_TYPE_FX, fx_effect_def, FxEffectDef);
			DUMP_ASSET(ASSET_TYPE_PARTICLE_SIM_ANIMATION, fx_particle_sim_animation, FxParticleSimAnimation);
			DUMP_ASSET(ASSET_TYPE_GESTURE, gesture, Gesture);
			DUMP_ASSET(ASSET_TYPE_IMAGE, gfx_image, GfxImage);
			DUMP_ASSET(ASSET_TYPE_LIGHT_DEF, gfx_light_def, GfxLightDef);
			DUMP_ASSET(ASSET_TYPE_GFXLIGHTMAP, gfx_light_map, GfxLightMap);
			DUMP_ASSET(ASSET_TYPE_IMPACT_FX, impact_fx, FxImpactTable);
			DUMP_ASSET(ASSET_TYPE_LASER, laser, LaserDef);
			DUMP_ASSET(ASSET_TYPE_LOCALIZE_ENTRY, localize, LocalizeEntry);
			DUMP_ASSET(ASSET_TYPE_LUA_FILE, lua_file, LuaFile);
			DUMP_ASSET(ASSET_TYPE_MATERIAL, material, Material);
			DUMP_ASSET(ASSET_TYPE_NET_CONST_STRINGS, net_const_strings, NetConstStrings);
			DUMP_ASSET(ASSET_TYPE_VFX, particle_system, ParticleSystemDef);
			DUMP_ASSET(ASSET_TYPE_RAWFILE, rawfile, RawFile);
			DUMP_ASSET(ASSET_TYPE_RETICLE, reticle, ReticleDef);
			DUMP_ASSET(ASSET_TYPE_RUMBLE, rumble, RumbleInfo);
			DUMP_ASSET(ASSET_TYPE_RUMBLE_GRAPH, rumble_graph, RumbleGraph);
			DUMP_ASSET(ASSET_TYPE_BEHAVIOR_TREE, behavior_tree, BehaviorTree);
			DUMP_ASSET(ASSET_TYPE_ANIMCLASS, anim_class, AnimationClass);
			DUMP_ASSET(ASSET_TYPE_SCRIPTABLE, scriptable_def, ScriptableDef);
			DUMP_ASSET(ASSET_TYPE_SCRIPTFILE, scriptfile, ScriptFile);
			DUMP_ASSET(ASSET_TYPE_STREAMING_INFO, streaming_info, StreamingInfo);
			DUMP_ASSET(ASSET_TYPE_STRINGTABLE, string_table, StringTable);
			DUMP_ASSET(ASSET_TYPE_TRACER, tracer, TracerDef);
			DUMP_ASSET(ASSET_TYPE_TTF, ttf_def, TTFDef);
			DUMP_ASSET(ASSET_TYPE_VECTORFIELD, vector_field, VectorField);
			DUMP_ASSET(ASSET_TYPE_ATTACHMENT, weapon_attachment, WeaponAttachment);
			DUMP_ASSET(ASSET_TYPE_ANIM_PACKAGE, weapon_anim_package, WeaponAnimPackage);
			DUMP_ASSET(ASSET_TYPE_SFX_PACKAGE, weapon_sfx_package, WeaponSFXPackage);
			DUMP_ASSET(ASSET_TYPE_VFX_PACKAGE, weapon_vfx_package, WeaponVFXPackage);
			DUMP_ASSET(ASSET_TYPE_WEAPON, weapon_def, WeaponCompleteDef);
			DUMP_ASSET(ASSET_TYPE_XANIMPARTS, xanim_parts, XAnimParts);
			DUMP_ASSET(ASSET_TYPE_XMODEL, xmodel, XModel);
			DUMP_ASSET(ASSET_TYPE_XMODEL_SURFS, xsurface, XModelSurfs);

			DUMP_ASSET(ASSET_TYPE_SOUND_GLOBALS, sound_globals, SndGlobals);
			DUMP_ASSET(ASSET_TYPE_SOUND_BANK, sound_bank, SndBank);
			//DUMP_ASSET(ASSET_TYPE_SOUND_BANK_TRANSIENT, sound_bank_transient, SndBankTransient);

			DUMP_ASSET(ASSET_TYPE_PHYSICSASSET, physics_asset, PhysicsAsset);
			DUMP_ASSET(ASSET_TYPE_PHYSICS_FX_PIPELINE, physics_fx_pipeline, PhysicsFXPipeline);
			DUMP_ASSET(ASSET_TYPE_PHYSICS_FX_SHAPE, physics_fx_shape, PhysicsFXShape);
			DUMP_ASSET(ASSET_TYPE_PHYSICSLIBRARY, physics_library, PhysicsLibrary);
			DUMP_ASSET(ASSET_TYPE_PHYSICS_SFX_EVENT_ASSET, physics_sfx_event, PhysicsSFXEventAsset);
			DUMP_ASSET(ASSET_TYPE_PHYSICS_VFX_EVENT_ASSET, physics_vfx_event, PhysicsVFXEventAsset);

			DUMP_ASSET(ASSET_TYPE_COMPUTESHADER, compute_shader, ComputeShader);
			DUMP_ASSET(ASSET_TYPE_DOMAINSHADER, domain_shader, MaterialDomainShader);
			DUMP_ASSET(ASSET_TYPE_HULLSHADER, hull_shader, MaterialHullShader);
			DUMP_ASSET(ASSET_TYPE_PIXELSHADER, pixel_shader, MaterialPixelShader);
			//DUMP_ASSET(ASSET_TYPE_VERTEXDECL, vertex_decl, MaterialVertexDeclaration);
			DUMP_ASSET(ASSET_TYPE_VERTEXSHADER, vertex_shader, MaterialVertexShader);

			DUMP_ASSET(ASSET_TYPE_TECHNIQUE_SET, techset, MaterialTechniqueSet);

			DUMP_ASSET(ASSET_TYPE_PATHDATA, path_data, PathData);
			DUMP_ASSET(ASSET_TYPE_CLIPMAP, clip_map, clipMap_t);
			DUMP_ASSET(ASSET_TYPE_COMWORLD, com_world, ComWorld);
			DUMP_ASSET(ASSET_TYPE_FXWORLD, fx_world, FxWorld);
			DUMP_ASSET(ASSET_TYPE_GFXWORLD, gfx_world, GfxWorld);
			DUMP_ASSET(ASSET_TYPE_GFXWORLD_TRANSIENT_ZONE, gfx_world_tr, GfxWorldTransientZone);
			DUMP_ASSET(ASSET_TYPE_GLASSWORLD, glass_world, GlassWorld);
			DUMP_ASSET(ASSET_TYPE_MAP_ENTS, map_ents, MapEnts);
			DUMP_ASSET(ASSET_TYPE_NAVMESH, nav_mesh, NavMeshData);
		}
		catch (const std::exception& e)
		{
			ZONETOOL_FATAL("A fatal exception occured while dumping zone \"%s\", exception was: \n%s",
				filesystem::get_fastfile().data(), e.what());
		}

#undef DUMP_ASSET
	}

	std::unordered_map<game::game_mode, std::function<void(XAsset*)>> dump_functions =
	{
		{game::iw7, dump_asset_iw7},
		{game::h1, dump_asset_h1},
	};

	void capture_enemy_shader(XAsset* asset)
	{
		if (!utils::flags::has_flag("enemy-extract") || !asset) return;
		const auto capture = [](const auto* shader, const char* stage, bool allow_null = false)
		{
			if (shader && shader->name)
			{
				const auto name = std::string(shader->name);
				// A leading comma denotes a reference, whose program is owned by an earlier zone.
				if (name.starts_with(",")) return;
				if (name.empty() || name.size() > 240 || name == "." || name == ".." ||
					!std::all_of(name.begin(), name.end(), [](unsigned char c)
						{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
							(c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; }))
				{
					ZONETOOL_WARNING("Skipping nonportable early shader name: %s", name.c_str());
					return;
				}
				const auto size = shader->prog.loadDef.programSize;
				const auto* program = shader->prog.loadDef.program;
				const bool empty_null = allow_null && name == "null.hlsl" && size == 0 && !program;
				const bool valid = program && size >= 32 && size <= 64 * 1024 * 1024 && std::memcmp(program, "DXBC", 4) == 0;
				if (empty_null || valid)
				{
					const auto relative = std::filesystem::path("techsets") / stage / (name + ".cso");
					const auto destination = std::filesystem::path("dump") / "paris_enemy_shader_bootstrap" / relative;
					bool keep_existing = false;
					if (!empty_null && std::filesystem::exists(destination))
					{
						const auto existing = utils::io::read_file(destination.string());
						keep_existing = existing.size() >= 32 && existing.compare(0, 4, "DXBC") == 0;
					}
					if (!keep_existing)
					{
						std::filesystem::create_directories(destination.parent_path());
						const auto bytes = size ? std::string(reinterpret_cast<const char*>(program), size) : std::string();
						utils::io::write_file(destination.string(), bytes);
						if (utils::io::read_file(destination.string()) != bytes)
							throw std::runtime_error("Could not preserve early IW7 enemy shader: " + name);
					}
				}
			}
		};
		switch (asset->type)
		{
		case ASSET_TYPE_VERTEXSHADER: capture(reinterpret_cast<MaterialVertexShader*>(asset->header.data), "vs"); break;
		case ASSET_TYPE_HULLSHADER: capture(reinterpret_cast<MaterialHullShader*>(asset->header.data), "hs"); break;
		case ASSET_TYPE_DOMAINSHADER: capture(reinterpret_cast<MaterialDomainShader*>(asset->header.data), "ds"); break;
		case ASSET_TYPE_PIXELSHADER: capture(reinterpret_cast<MaterialPixelShader*>(asset->header.data), "ps", true); break;
		default: break;
		}
	}

	void dump_asset(XAsset* asset)
	{
		// Initial database registration still has the shader program storage intact.
		// Preserve every graphics stage before later technique dumps can see reused storage.
		capture_enemy_shader(asset);
		if (enemy_image_capture && asset->type == ASSET_TYPE_IMAGE)
		{
			auto* image = asset->header.image;
			if (image && image->name && enemy_image_names.contains(image->name))
			{
				const auto previous_fastfile = filesystem::get_fastfile();
				const auto restore_fastfile = gsl::finally([&] { filesystem::set_fastfile(previous_fastfile); });
				// Retain every registration, including a later patch-zone replacement.
				// Select the last complete capture only after all source zones finish.
				filesystem::set_fastfile(enemy_image_pack + "/capture_" + std::to_string(enemy_image_capture_count++));
				// The stream cursor belongs to this registration, not a later DB lookup.
				// Reuse the upstream stream decompressor while that cursor is still valid.
				if (!image->streamed && image->dataLen1 && !image->pixelData)
					throw std::runtime_error("Requested enemy resident image has no pixel data: " + std::string(image->name));
				if (image->imageFormat == DXGI_FORMAT_R8G8B8A8_UNORM && image->category == IMG_CATEGORY_AUTO_GENERATED &&
					image->width == 1 && image->height == 1 && image->dataLen1 == 4 && image->pixelData &&
					image->pixelData[0] == 255 && image->pixelData[1] == 0 && image->pixelData[2] == 0 && image->pixelData[3] == 255)
					throw std::runtime_error("Requested enemy image is the generated red placeholder: " + std::string(image->name));
				gfx_image::dump(image, g_load->file->name);
				const auto folder = image->streamed ? "streamed_images" : "images";
				const auto root = std::filesystem::path(filesystem::get_dump_path()) / folder;
				const auto metadata = root / (std::string(image->name) + ".iw7Image");
				if (!std::filesystem::is_regular_file(metadata))
					throw std::runtime_error("Requested enemy image metadata was not written: " + std::string(image->name));
				if (image->streamed)
				{
					for (size_t i = 0; i < 4; ++i)
					{
						const auto cumulative = image->streams[i].levelCountAndSize.pixelSize;
						const auto previous = i ? image->streams[i - 1].levelCountAndSize.pixelSize : 0;
						// Retail images may use fewer than four streams; unused tail
						// descriptors are zero, not a copy of the last cumulative size.
						if (!cumulative)
						{
							for (size_t tail = i + 1; tail < 4; ++tail)
								if (image->streams[tail].levelCountAndSize.pixelSize)
									throw std::runtime_error("Enemy image has a hole in its stream descriptors: " + std::string(image->name));
							continue;
						}
						if (cumulative < previous) throw std::runtime_error("Enemy image stream sizes are not cumulative: " + std::string(image->name));
						if (cumulative == previous) continue;
						const auto pixels = root / (std::string(image->name) + "_stream" + std::to_string(i) + ".pixels");
						if (!std::filesystem::is_regular_file(pixels) || std::filesystem::file_size(pixels) != cumulative - previous)
							throw std::runtime_error("Requested enemy image stream was not written: " + pixels.string());
					}
				}
				ZONETOOL_INFO("Captured donor image %s: %ux%u, streamed=%u", image->name, image->width, image->height, image->streamed);
				enemy_images_dumped.insert(image->name);
				enemy_image_sources[image->name] = root;
			}
		}

		if (globals.verify)
		{
			ZONETOOL_INFO("Loading asset \"%s\" of type %s.", get_asset_name(asset), type_to_string(asset->type));
		}

		if (globals.dump_csv)
		{
			if (globals.csv_file.get_fp() == nullptr)
			{
				globals.csv_file = filesystem::file(filesystem::get_fastfile() + ".csv");
				globals.csv_file.open("wb");
			}

			// dump assets to disk
			if (globals.csv_file.get_fp())
			{
				std::fprintf(globals.csv_file.get_fp(), "%s,%s\n", type_to_string(asset->type), get_asset_name(asset));
			}
		}

		if (!globals.dump)
		{
			return;
		}

		if (asset_type_filter.size() > 0 && !asset_type_filter.contains(asset->type))
		{
			return;
		}

		// dump referenced later
		if (is_referenced_asset(asset))
		{
			// -dumpzonetypes: a requested type that this zone only references (",name") is defined in another
			// zone (a map's materials live in techsets_<map>): dump it from the loaded definition afterwards
			if (asset_type_filter.size() > 0)
			{
				referenced_assets.emplace_back(asset->type, get_asset_name(asset));
			}
			return;
		}

		const auto dump_func = dump_functions.find(globals.target_game);
		if (dump_func == dump_functions.end())
		{
			const auto name = game::get_mode_as_string(globals.target_game);
			ZONETOOL_ERROR("Dump mode \"%s\" is not supported", name.data());
			return;
		}

		dump_func->second(asset);
	}

	void dump_enemy_pack_asset(XAssetType type, void* data)
	{
		if (!filesystem::get_fastfile().starts_with("paris_enemy_") || !data) return;

		static const std::unordered_set<XAssetType> enemy_source_types{
			// Images must be captured at DB registration. Parsed portable images
			// have no live stream cursor; re-dumping them here can dereference an
			// unrelated or null g_streamZoneMem and invalidate source provenance.
			ASSET_TYPE_VERTEXSHADER, ASSET_TYPE_HULLSHADER,
			ASSET_TYPE_DOMAINSHADER, ASSET_TYPE_PIXELSHADER, ASSET_TYPE_COMPUTESHADER,
			ASSET_TYPE_SCRIPTABLE, ASSET_TYPE_TECHNIQUE_SET,
		};
		if (!enemy_source_types.contains(type)) return;

		XAsset asset{};
		asset.type = type;
		asset.header.data = data;
		const auto previous_dump = globals.dump;
		const auto previous_dump_csv = globals.dump_csv;
		const auto previous_verify = globals.verify;
		const auto previous_target = globals.target_game;
		const auto previous_filter = asset_type_filter;
		const auto restore = gsl::finally([&]
		{
			globals.dump = previous_dump;
			globals.dump_csv = previous_dump_csv;
			globals.verify = previous_verify;
			globals.target_game = previous_target;
			asset_type_filter = previous_filter;
		});
		globals.dump = true;
		globals.dump_csv = false;
		globals.verify = false;
		globals.target_game = game::iw7;
		asset_type_filter = enemy_source_types;
		dump_asset(&asset);
	}

	void dump_refs()
	{
		// remove duplicates
		std::sort(referenced_assets.begin(), referenced_assets.end());
		referenced_assets.erase(std::unique(referenced_assets.begin(),
			referenced_assets.end()), referenced_assets.end());

		std::size_t refs_dumped = 0, refs_missing = 0, refs_images = 0;
		for (auto& asset : referenced_assets)
		{
			if (asset.second.length() <= 1)
			{
				continue;
			}

			const auto asset_name = &asset.second[1];

			if (asset.first == ASSET_TYPE_IMAGE)
			{
				refs_images++;
				if (asset_type_filter.empty())
				{
					ZONETOOL_WARNING("Not dumping referenced asset \"%s\" of type \"%s\"", asset_name, type_to_string(asset.first));
				}
				continue;
			}

			const auto& asset_header = db_find_x_asset_header_safe(asset.first, asset_name);

			if (!asset_header.data || DB_IsXAssetDefault(asset.first, asset_name))
			{
				ZONETOOL_ERROR("Could not find referenced asset \"%s\" of type \"%s\"", asset_name, type_to_string(asset.first));
				refs_missing++;
				continue;
			}
			refs_dumped++;

			//ZONETOOL_INFO("Dumping additional asset \"%s\" of type \"%s\"", asset_name, type_to_string(asset.first));

			XAsset referenced_asset =
			{
				asset.first,
				asset_header
			};

			dump_asset(&referenced_asset);
		}

		if (!referenced_assets.empty())
		{
			ZONETOOL_INFO("Referenced assets of the requested types: %zu dumped from their defining zone, %zu not loaded, %zu images skipped",
				refs_dumped, refs_missing, refs_images);
		}

		referenced_assets.clear();
	}

	void stop_dumping()
	{
		globals.verify = false;

		if (globals.dump_csv)
		{
			globals.csv_file.close();
			globals.csv_file = {};
			globals.dump_csv = false;
		}

		if (!globals.dump)
		{
			return;
		}

		dump_refs();

		ZONETOOL_INFO("Zone \"%s\" dumped.", filesystem::get_fastfile().data());

		globals.dump = false;

		zonetool::taskbar::clear();
	}

	void capture_enemy_physics_asset(PhysicsAsset* asset)
	{
		if (!enemy_physics_capture || !asset || !asset->name || !enemy_physics_names.contains(asset->name)) return;
		if (!asset->havokData || !asset->havokDataSize || asset->havokDataSize > 128 * 1024 * 1024 ||
			asset->numSFXEventAssets < 0 || asset->numSFXEventAssets > 256 ||
			asset->numVFXEventAssets < 0 || asset->numVFXEventAssets > 256 ||
			(asset->numSFXEventAssets && !asset->sfxEventAssets) ||
			(asset->numVFXEventAssets && !asset->vfxEventAssets))
			throw std::runtime_error("Requested donor PhysicsAsset has invalid Havok or event-asset data: " + std::string(asset->name));

		const auto previous_fastfile = filesystem::get_fastfile();
		const auto capture_fastfile = enemy_physics_pack + "/capture_" + std::to_string(enemy_physics_capture_count++);
		filesystem::set_fastfile(capture_fastfile);
		const auto restore_fastfile = gsl::finally([&] { filesystem::set_fastfile(previous_fastfile); });

		physics_asset::dump(asset);
		const auto capture_root = std::filesystem::path(filesystem::get_dump_path());
		const auto source = capture_root / "physicsasset" / asset->name;
		const auto havok_file = std::filesystem::path(source.string() + havok::binary::havok_file_ext);
		if (!std::filesystem::is_regular_file(source) || !std::filesystem::is_regular_file(havok_file) ||
			std::filesystem::file_size(source) == 0 || std::filesystem::file_size(havok_file) != asset->havokDataSize)
			throw std::runtime_error("Requested donor PhysicsAsset capture is incomplete: " + std::string(asset->name));

		const auto bytes = utils::io::read_file(havok_file.string());
		if (bytes.size() != asset->havokDataSize ||
			std::memcmp(bytes.data(), asset->havokData, asset->havokDataSize) != 0)
			throw std::runtime_error("Requested donor Havok capture differs from its pre-registration bytes: " + std::string(asset->name));

		enemy_physics_sources[asset->name].push_back(capture_root);
		const auto hash = utils::cryptography::sha256::compute(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true);
		ZONETOOL_INFO("Captured pre-registration donor PhysicsAsset %s (%u bytes, %s)", asset->name, asset->havokDataSize, hash.c_str());
	}

	namespace
	{
		std::mutex captured_shader_mutex;
		std::unordered_map<std::string, std::string> captured_shaders[ASSET_TYPE_COUNT];

		template <typename T>
		void capture_shader(XAssetType type, T* shader)
		{
			if (!shader || !shader->name) return;
			const auto& def = shader->prog.loadDef;
			if (!def.program || def.programSize < 4 || std::memcmp(def.program, "DXBC", 4)) return;
			std::lock_guard _(captured_shader_mutex);
			captured_shaders[type].try_emplace(shader->name, reinterpret_cast<const char*>(def.program), def.programSize);
		}
	}

	const std::string* find_captured_shader(XAssetType type, const std::string& name)
	{
		std::lock_guard _(captured_shader_mutex);
		const auto it = captured_shaders[type].find(name);
		return it == captured_shaders[type].end() ? nullptr : &it->second;
	}

	utils::hook::detour db_add_xasset_hook;
	XAssetHeader db_add_xasset_stub(XAssetType type, XAssetHeader* header)
	{
		if (enemy_physics_capture && type == ASSET_TYPE_PHYSICSASSET && header)
			capture_enemy_physics_asset(header->physicsAsset);

		// keep shader bytecode: the engine drops it once the D3D object exists, and shaders are
		// deduplicated into shared techset zones, so a new zone may need bytes it has no dump of
		switch (type)
		{
		case ASSET_TYPE_PIXELSHADER: capture_shader(type, header->pixelShader); break;
		case ASSET_TYPE_VERTEXSHADER: capture_shader(type, header->vertexShader); break;
		case ASSET_TYPE_HULLSHADER: capture_shader(type, header->hullShader); break;
		case ASSET_TYPE_DOMAINSHADER: capture_shader(type, header->domainShader); break;
		default: break;
		}

		XAsset xasset =
		{
			type,
			*header
		};

		dump_asset(&xasset);
		return db_add_xasset_hook.invoke<XAssetHeader>(type, header);
	}

	utils::hook::detour db_finish_load_x_file_hook;
	void db_finish_load_x_file_stub()
	{
		if (std::string(g_load->file->name) == filesystem::get_fastfile() &&
			reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == 0x1409E7745ui64)
		{
			stop_dumping();
		}
		return db_finish_load_x_file_hook.invoke<void>();
	}

	utils::hook::detour load_x_gfx_globals_hook;
	void load_x_gfx_globals_stub(bool atStreamStart)
	{
		load_x_gfx_globals_hook.invoke<void>(atStreamStart);

		const auto var_x_gfx_globals = *reinterpret_cast<XGfxGlobals**>(0x1453E4490);
		insert_x_gfx_globals_for_zone(*g_zoneIndex, var_x_gfx_globals);
	}

	void reallocate_asset_pool(const XAssetType type, const unsigned int new_size)
	{
		const size_t element_size = DB_GetXAssetTypeSize(type);

		auto* new_pool = utils::memory::get_allocator()->allocate(new_size * element_size);
		std::memmove(new_pool, g_assetPool[type], g_poolSize[type] * element_size);

		g_assetPool[type] = new_pool;
		g_poolSize[type] = new_size;
	}

	void reallocate_asset_pool_multiplier(const XAssetType type, unsigned int multiplier)
	{
		const auto new_size = multiplier * g_poolSize[type];
		reallocate_asset_pool(type, multiplier * new_size);
	}

	bool is_zone_loaded(const std::string& name)
	{
		if (DB_Zones_GetZoneIndexFromName(name.data()) != 0xFFFF)
		{
			return true;
		}

		return false;
	}

	bool load_zone(const std::string& name, DBSyncMode mode = DB_LOAD_SYNC, bool inform = true)
	{
		if (!zone_exists(name.data()))
		{
			ZONETOOL_INFO("Zone \"%s\" could not be found!", name.data());
			return false;
		}

		wait_for_database();

		if (is_zone_loaded(name))
		{
			if (inform)
			{
				ZONETOOL_INFO("zone \"%s\" is already loaded...", name.data());
			}

			return false;
		}

		if (inform)
		{
			ZONETOOL_INFO("Loading zone \"%s\"...", name.data());
		}

		zonetool::taskbar::set_indeterminate();
		XZoneInfo zone = { name.data(), DB_ZONE_GAME | DB_ZONE_CUSTOM };
		DB_LoadXAssets(&zone, 1, mode);
		return true;
	}

	void unload_zones()
	{
		DB_UnloadFastfilesByZoneFlags(DB_ZONE_CUSTOM);
		ZONETOOL_INFO("Unloaded loaded zones...");
	}

	void dump_zone(const std::string& name, const game::game_mode target, const std::optional<std::string> fastfile = {})
	{
		if (!zone_exists(name.data()))
		{
			ZONETOOL_INFO("Zone \"%s\" could not be found!", name.data());
			return;
		}

		wait_for_database();

		globals.target_game = target;
		ZONETOOL_INFO("Dumping zone \"%s\"...", name.data());

		if (fastfile.has_value())
		{
			filesystem::set_fastfile(fastfile.value());
		}
		else
		{
			filesystem::set_fastfile(name);
		}

		globals.dump = true;
		globals.dump_csv = true;
		if (!load_zone(name, DB_LOAD_ASYNC, false))
		{
			globals.dump = false;
			globals.dump_csv = false;
			return;
		}

		while (globals.dump)
		{
			Sleep(1);
		}
	}

	void dump_csv(const std::string& name)
	{
		if (!zone_exists(name.data()))
		{
			ZONETOOL_INFO("Zone \"%s\" could not be found!", name.data());
			return;
		}

		wait_for_database();

		ZONETOOL_INFO("Dumping csv \"%s\"...", name.data());

		filesystem::set_fastfile(name);

		globals.dump_csv = true;
		if (!load_zone(name, DB_LOAD_ASYNC, true))
		{
			globals.dump_csv = false;
			return;
		}

		while (globals.dump_csv)
		{
			Sleep(1);
		}

		ZONETOOL_INFO("Csv \"%s\" dumped...", name.data());
	}

	void verify_zone(const std::string& name)
	{
		if (!zone_exists(name.data()))
		{
			ZONETOOL_INFO("Zone \"%s\" could not be found!", name.data());
			return;
		}

		wait_for_database();

		filesystem::set_fastfile(name);
		globals.verify = true;
		if (!load_zone(name, DB_LOAD_ASYNC, true))
		{
			globals.verify = false;
		}

		while (globals.verify)
		{
			Sleep(1);
		}
	}

	void add_assets_using_iterator(const std::string& fastfile, const std::string& type, const std::string& folder,
		const std::string& extension, bool skip_reference, zone_base* zone)
	{
		const auto path = "zonetool\\" + fastfile + "\\" + folder;
		if (!std::filesystem::is_directory(path))
		{
			return;
		}

		const auto iter = std::filesystem::recursive_directory_iterator(path);
		for (auto& file : iter)
		{
			if (!is_regular_file(file))
			{
				continue;
			}

			const auto filename = file.path().filename().string();

			if (skip_reference && filename[0] == ',')
			{
				continue;
			}

			if (!extension.empty() && filename.ends_with(extension))
			{
				const auto base_name = filename.substr(0, filename.length() - extension.length());
				zone->add_asset_of_type(type, base_name);
			}
			else if (file.path().extension().empty())
			{
				zone->add_asset_of_type(type, filename);
			}
		}
	}

	void parse_csv_file_ignore(const std::string& fastfile, const std::string& csv)
	{
		auto path = "zone_source\\" + csv + ".csv";
		auto parser = csv::parser(path.data(), ',');

		if (!parser.valid())
		{
			throw std::runtime_error(utils::string::va("Could not find csv file \"%s\"", csv.data()));
		}

		auto rows = parser.get_rows();
		if (rows == nullptr)
		{
			return;
		}

		for (auto row_index = 0; row_index < parser.get_num_rows(); row_index++)
		{
			auto* row = rows[row_index];
			if (row == nullptr)
			{
				continue;
			}

			if (!row->fields)
			{
				continue;
			}

			if ((strlen(row->fields[0]) >= 1 && row->fields[0][0] == '#') || (strlen(row->fields[0]) >= 2 && row->
				fields[0][0] == '/' && row->fields[0][1] == '/'))
			{
				// comment line, go to next line.
				continue;
			}
			if (!strlen(row->fields[0]))
			{
				// empty line, go to next line.
				continue;
			}

			if (row->num_fields < 2 || !is_valid_asset_type(row->fields[0]))
			{
				continue;
			}

			std::string name;
			if ((!row->fields[1] || !strlen(row->fields[1]) && row->fields[2] && strlen(row->fields[2])))
			{
				continue;
			}
			else
			{
				name = row->fields[1];
			}

			const auto type = static_cast<std::uint32_t>(type_to_int(row->fields[0]));
			ignore_assets.insert(std::make_pair(type, name));
		}
	}

	void parse_csv_file(zone_base* zone, const std::string& fastfile, const std::string& csv)
	{
		auto path = "zone_source\\" + csv + ".csv";
		auto parser = csv::parser(path.data(), ',');

		if (!parser.valid())
		{
			throw std::runtime_error(utils::string::va("Could not find csv file \"%s\" to build zone!", csv.data()));
		}

		auto is_referencing = false;
		auto rows = parser.get_rows();
		if (rows == nullptr)
		{
			return;
		}

		for (auto row_index = 0; row_index < parser.get_num_rows(); row_index++)
		{
			auto* row = rows[row_index];
			if (row == nullptr)
			{
				continue;
			}

			if (!row->fields)
			{
				continue;
			}

			if ((strlen(row->fields[0]) >= 1 && row->fields[0][0] == '#') || (strlen(row->fields[0]) >= 2 && row->
				fields[0][0] == '/' && row->fields[0][1] == '/'))
			{
				// comment line, go to next line.
				continue;
			}
			if (!strlen(row->fields[0]))
			{
				// empty line, go to next line.
				continue;
			}
			if (row->fields[0] == "require"s)
			{
				load_zone(row->fields[1], DB_LOAD_ASYNC);
				wait_for_database();
			}
			else if (row->fields[0] == "include"s)
			{
				filesystem::get_search_paths().push_back("zonetool\\"s + row->fields[1] + "\\");
				parse_csv_file(zone, fastfile, row->fields[1]);
				filesystem::get_search_paths().pop_back();
			}
			else if (row->fields[0] == "ignore"s)
			{
				parse_csv_file_ignore(fastfile, row->fields[1]);
			}
			// this allows us to reference assets instead of rewriting them
			else if (row->fields[0] == "reference"s)
			{
				if (row->num_fields >= 2)
				{
					is_referencing = row->fields[1] == "true"s;
				}
			}
			// this will use a directory iterator to automatically add assets
			else if (row->fields[0] == "iterate"s)
			{
				if (row->num_fields >= 2)
				{
					auto type = row->fields[1];
					auto iterate_all = row->fields[1] == "true"s;

					try
					{
						if (type == "vfx"s || iterate_all)
						{
							add_assets_using_iterator(fastfile, type, "particlesystem", ".iw7VFX", true, zone);
						}
						if (type == "material"s || iterate_all)
						{
							add_assets_using_iterator(fastfile, type, "materials", ".json", true, zone);
						}
						if (type == "xmodel"s || iterate_all)
						{
							add_assets_using_iterator(fastfile, type, "xmodel", ".xmb", true, zone);
						}
						if (type == "xanim"s || iterate_all)
						{
							add_assets_using_iterator(fastfile, type, "xanim", ".xab", true, zone);
						}
					}
					catch (const std::exception& e)
					{
						ZONETOOL_FATAL("A fatal exception occured while building zone \"%s\", exception was: \n%s", fastfile.data(), e.what());
					}
				}
			}
			// add paths
			else if ((row->fields[0] == "addpath"s || row->fields[0] == "addpaths"s) && row->num_fields >= 2)
			{
				bool insert_at_beginning = row->num_fields >= 3 && row->fields[2] == "true"s;

				if (row->fields[0] == "addpath"s)
					filesystem::add_path(row->fields[1], insert_at_beginning);
				else
					filesystem::add_paths_from_directory(row->fields[1], insert_at_beginning);
			}
			// if entry is not an option, it should be an asset.
			else
			{
				if (row->fields[0] == "localize"s && row->num_fields >= 2 &&
					filesystem::file("localizedstrings/"s + row->fields[1] + ".str").exists())
				{
					localize::parse_localizedstrings_file(zone, row->fields[1]);
				}
				else if (row->fields[0] == "localize"s && row->num_fields >= 2 &&
					filesystem::file("localizedstrings/"s + row->fields[1] + ".json").exists())
				{
					localize::parse_localizedstrings_json(zone, row->fields[1]);
				}
				else
				{
					if (row->num_fields < 2 || !is_valid_asset_type(row->fields[0]))
					{
						continue;
					}

					std::string name;
					if ((!row->fields[1] || !strlen(row->fields[1]) && row->fields[2] && strlen(row->fields[2])))
					{
						name = ","s + row->fields[2];
					}
					else
					{
						name = ((is_referencing) ? ","s : ""s) + row->fields[1];
					}

					try
					{
						zone->add_asset_of_type(
							row->fields[0],
							name
						);
					}
					catch (std::exception& ex)
					{
						ZONETOOL_FATAL("A fatal exception occured while building zone \"%s\", exception was: \n%s", fastfile.data(), ex.what());
					}
				}
			}
		}
	}

	std::shared_ptr<zone_base> alloc_zone(const std::string& zone)
	{
		auto ptr = std::make_shared<zone_interface>(zone);
		return ptr;
	}

	std::shared_ptr<zone_buffer> alloc_buffer()
	{
		auto ptr = std::make_shared<zone_buffer>();
		ptr->init_streams(MAX_XFILE_COUNT);

		return ptr;
	}

	dump_params get_dump_params(const ::iw7::command::params& params)
	{
		dump_params dump_params{};
		dump_params.target = game::iw7;

		const auto parse_params = [&]()
		{
			if (params.size() < 3)
			{
				dump_params.zone = params.get(1);
				return true;
			}

			const auto mode = params.get(1);
			dump_params.zone = params.get(2);
			dump_params.target = game::get_mode_from_string(mode);

			if (dump_params.target == game::none)
			{
				ZONETOOL_ERROR("Invalid dump target \"%s\"", mode);
				return false;
			}

			if (!dump_functions.contains(dump_params.target))
			{
				ZONETOOL_ERROR("Unsupported dump target \"%s\" (%i)", mode, dump_params.target);
				return false;
			}

			if (params.size() >= 4)
			{
				const auto asset_types_str = params.get(3);
				if (asset_types_str == "_"s)
				{
					return true;
				}

				const auto asset_types = utils::string::split(asset_types_str, ',');

				for (const auto& type_str : asset_types)
				{
					const auto type = type_to_int(type_str);
					if (type == -1)
					{
						ZONETOOL_ERROR("Asset type \"%s\" does not exist", type_str.data());
						return false;
					}

					dump_params.filter.insert(static_cast<XAssetType>(type));
				}
			}

			return true;
		};

		dump_params.valid = parse_params();
		return dump_params;
	}

	void clear_asset_fields()
	{
		material::fixed_nml_images_map.clear();
		techset::vertexdecl_pointers.clear();
		//xanim_parts::secondary_anims.clear();
	}

	void build_zone(const std::string& fastfile)
	{
		const auto previous_enemy_build = enemy_pack_build_active;
		enemy_pack_build_active = fastfile.starts_with("paris_enemy_");
		const auto restore_enemy_build = gsl::finally([&] { enemy_pack_build_active = previous_enemy_build; });
		// make sure FS is correct.
		filesystem::set_fastfile(fastfile);

		ZONETOOL_INFO("Building fastfile \"%s\"", fastfile.data());

		auto zone = alloc_zone(fastfile);
		if (zone == nullptr)
		{
			ZONETOOL_ERROR("An error occured while building fastfile \"%s\": Are you out of memory?", fastfile.data());
			return;
		}

		ignore_assets.clear();
		clear_asset_fields();

		zonetool::taskbar::set_indeterminate();

		try
		{
			parse_csv_file(zone.get(), fastfile, fastfile);
		}
		catch (std::exception& ex)
		{
			ZONETOOL_ERROR("%s", ex.what());
			return;
		}

		// allocate zone buffer
		auto buffer = alloc_buffer();

		// set game specific zone type info
		buffer->set_fields(XFILE_BLOCK_RUNTIME,
			0xFFFFFFF000000000,
			static_cast<std::uint32_t>(-2),
			static_cast<std::uint32_t>(-4),
			static_cast<std::uint32_t>(-3),
			static_cast<std::uint32_t>(-1));

		// add branding asset
		zone->add_asset_of_type("rawfile", fastfile);

		// compile zone
		zone->build(buffer.get());

		zonetool::taskbar::clear();

		ignore_assets.clear();
		clear_asset_fields();
	}

	void iterate_zones()
	{
		const auto iterate_zones_internal = [](const std::string& path)
		{
			for (auto const& dir_entry : std::filesystem::directory_iterator{ path })
			{
				if (dir_entry.is_regular_file() && dir_entry.path().extension() == ".ff")
				{
					const auto zone = dir_entry.path().stem().string();

					load_zone(zone);

					wait_for_database();
					unload_zones();
				}
			}
		};

		const auto zone_path = utils::io::directory_exists("zone") ? "zone/" : "";
		iterate_zones_internal(zone_path);

		const auto lang_path = utils::io::directory_exists("zone") ? "zone/english/" : "english/";
		iterate_zones_internal(lang_path);
	}

	void register_commands()
	{
		::iw7::command::add("quit", []()
		{
			std::quick_exit(EXIT_SUCCESS);
		});

		::iw7::command::add("iwzcompile", [](const ::iw7::command::params& params)
		{
			if (params.size() < 5)
			{
				ZONETOOL_ERROR("usage: iwzcompile <template zone> <template map> <new map> <source dir>");
				return;
			}

			iwzmap::compile(params.get(1), params.get(2), params.get(3), params.get(4));
		});

		::iw7::command::add("iwzbounds", [](const ::iw7::command::params& params)
		{
			if (params.size() < 4)
			{
				ZONETOOL_ERROR("usage: iwzbounds <template zone> <smodels.txt> <out.json>");
				return;
			}

			iwzmap::smodel_bounds(params.get(1), params.get(2), params.get(3));
		});

		::iw7::command::add("buildzone", [](const ::iw7::command::params& params)
		{
			if (params.size() != 2)
			{
				ZONETOOL_ERROR("usage: buildzone <zone>");
				return;
			}

			build_zone(params.get(1));
		});

		::iw7::command::add("loadzone", [](const ::iw7::command::params& params)
		{
			if (params.size() != 2)
			{
				ZONETOOL_ERROR("usage: loadzone <zone>");
				return;
			}

			load_zone(params.get(1));
		});

		::iw7::command::add("unloadzones", []()
		{
			unload_zones();
		});

		::iw7::command::add("dumpzone", [](const ::iw7::command::params& params)
		{
			if (params.size() < 2)
			{
				ZONETOOL_ERROR("usage: dumpzone <zone>");
				return;
			}

			asset_type_filter.clear();

			if (params.size() >= 3)
			{
				const auto mode = params.get(1);
				const auto dump_target = game::get_mode_from_string(mode);

				if (dump_target == game::none)
				{
					ZONETOOL_ERROR("Invalid dump target \"%s\"", mode);
					return;
				}

				if (!dump_functions.contains(dump_target))
				{
					ZONETOOL_ERROR("Unsupported dump target \"%s\" (%i)", mode, dump_target);
					return;
				}

				if (params.size() >= 4)
				{
					const auto asset_types_str = params.get(3);
					const auto asset_types = utils::string::split(asset_types_str, ',');

					for (const auto& type_str : asset_types)
					{
						const auto type = type_to_int(type_str);
						if (type == -1)
						{
							ZONETOOL_ERROR("Asset type \"%s\" does not exist", type_str.data());
							return;
						}

						asset_type_filter.insert(static_cast<XAssetType>(type));
					}
				}

				dump_zone(params.get(2), dump_target);
			}
			else
			{
				dump_zone(params.get(1), game::iw7);
			}
		});

		::iw7::command::add("dumpasset", [](const ::iw7::command::params& params)
		{
			const auto type = XAssetType(type_to_int(params.get(1)));
			const auto name = params.get(2);

			XAsset asset{};
			asset.type = type;

			const auto header = db_find_x_asset_header(type, name, false);
			if (!header.data)
			{
				ZONETOOL_INFO("Asset not found\n");
				return;
			}

			globals.dump = true;
			const auto _0 = gsl::finally([]
			{
				globals.dump = false;
			});

			filesystem::set_fastfile("assets");
			asset.header = header;
			globals.target_game = game::iw7;
			dump_asset(&asset);

			ZONETOOL_INFO("Dumped to dump/assets");
		});

		::iw7::command::add("dumpcsv", [](const ::iw7::command::params& params)
		{
			if (params.size() != 2)
			{
				ZONETOOL_ERROR("usage: dumpcsv <zone>");
				return;
			}

			dump_csv(params.get(1));
		});

		::iw7::command::add("verifyzone", [](const ::iw7::command::params& params)
		{
			if (params.size() != 2)
			{
				ZONETOOL_ERROR("usage: verifyzone <zone>");
				return;
			}

			verify_zone(params.get(1));
		});

		::iw7::command::add("generatecsv", csv_generator::create_command
			<::iw7::command::params>([](const uint32_t id)
		{
			return gsc::iw7::gsc_ctx->token_name(id);
		}));

		::iw7::command::add("iteratezones", []()
		{
			iterate_zones();
		});
	}

	std::vector<std::string> get_command_line_arguments()
	{
		LPWSTR* szArglist;
		int nArgs;

		szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);

		std::vector<std::string> args;
		args.resize(nArgs);

		// convert all args to std::string
		for (int i = 0; i < nArgs; i++)
		{
			auto curArg = std::wstring(szArglist[i]);
			args[i] = std::string(curArg.begin(), curArg.end());
		}

		// return arguments
		return args;
	}

	std::string enemy_definition_table_for_map(const std::string& donor_zone)
	{
		if (donor_zone == "cp_zmb") return "mp/default_agent_definition.csv";
		if (donor_zone == "cp_rave") return "mp/dlc1_agent_definition.csv";
		if (donor_zone == "cp_disco") return "mp/dlc2_agent_definition.csv";
		if (donor_zone == "cp_town") return "mp/dlc3_agent_definition.csv";
		if (donor_zone == "cp_final") return "mp/dlc4_agent_definition.csv";
		throw std::runtime_error("Unsupported Zombies donor map for enemy extraction");
	}

	bool is_safe_enemy_token(const std::string& value)
	{
		return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c)
			{ return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
	}

	struct enemy_reference_request
	{
		XAssetType type;
		std::string type_name;
		std::string name;
	};

	struct enemy_reference_result
	{
		std::string type_name;
		std::string name;
		std::string status;
	};

	bool is_safe_enemy_reference_name(const std::string& name)
	{
		if (name.empty() || name.size() > 512 || name.front() == ',' ||
			name.front() == ' ' || name.back() == ' ' || name.find_first_of(",\"\r\n") != std::string::npos)
			return false;

		return std::all_of(name.begin(), name.end(), [](unsigned char c)
			{ return c >= 0x20 && c <= 0x7E; });
	}

	std::vector<enemy_reference_request> read_enemy_reference_list(const std::string& path)
	{
		constexpr size_t max_input_bytes = 2 * 1024 * 1024;
		constexpr size_t max_rows = 100000;

		std::error_code file_error;
		if (!std::filesystem::is_regular_file(std::filesystem::path(path), file_error) || file_error)
			throw std::runtime_error("Enemy reference input is not a readable regular file: " + path);

		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
			throw std::runtime_error("Could not open enemy reference input: " + path);

		std::string data(max_input_bytes + 1, '\0');
		input.read(data.data(), static_cast<std::streamsize>(data.size()));
		const auto bytes_read = input.gcount();
		if (bytes_read < 0 || static_cast<size_t>(bytes_read) > max_input_bytes)
			throw std::runtime_error("Enemy reference input exceeds the 2 MiB limit");
		if (input.bad())
			throw std::runtime_error("Could not read enemy reference input: " + path);
		data.resize(static_cast<size_t>(bytes_read));
		if (data.empty())
			throw std::runtime_error("Enemy reference input is empty");

		std::vector<enemy_reference_request> requests;
		std::set<std::pair<std::string, std::string>> unique_rows;
		for (size_t offset = 0; offset < data.size();)
		{
			const auto newline = data.find('\n', offset);
			const bool line_terminated = newline != std::string::npos;
			auto row = data.substr(offset, line_terminated ? newline - offset : std::string::npos);
			offset = line_terminated ? newline + 1 : data.size();
			if (line_terminated && !row.empty() && row.back() == '\r') row.pop_back();

			if (row.empty() || row.front() == ',')
				throw std::runtime_error("Enemy reference input contains an empty or leading-comma row");
			const auto comma = row.find(',');
			if (comma == std::string::npos || row.find(',', comma + 1) != std::string::npos)
				throw std::runtime_error("Each enemy reference row must contain exactly two unquoted CSV fields");

			auto type_name = row.substr(0, comma);
			auto asset_name = row.substr(comma + 1);
			const auto type_value = type_to_int(type_name);
			if (type_value < 0)
				throw std::runtime_error("Unknown IW7 asset type in enemy reference input: " + type_name);
			if (!is_safe_enemy_reference_name(asset_name))
				throw std::runtime_error("Unsafe or invalid IW7 asset name in enemy reference input");
			if (requests.size() >= max_rows)
				throw std::runtime_error("Enemy reference input exceeds the 100,000 row limit");
			if (!unique_rows.emplace(type_name, asset_name).second)
				throw std::runtime_error("Duplicate type/name pair in enemy reference input: " + type_name + "," + asset_name);

			requests.push_back({static_cast<XAssetType>(type_value), std::move(type_name), std::move(asset_name)});
		}

		if (requests.empty())
			throw std::runtime_error("Enemy reference input contains no rows");
		return requests;
	}

	std::filesystem::path prepare_enemy_reference_output(const std::string& output_name)
	{
		if (!is_safe_enemy_token(output_name) || output_name.size() > 128)
			throw std::runtime_error("Enemy reference output name must contain 1-128 lowercase letters, digits, or underscores");

		const std::filesystem::path directory(L"enemy-catalog");
		auto attributes = GetFileAttributesW(directory.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES)
		{
			const auto error = GetLastError();
			if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
				throw std::runtime_error("Could not inspect enemy-catalog output directory (Windows error " + std::to_string(error) + ")");
			if (!CreateDirectoryW(directory.c_str(), nullptr))
			{
				const auto create_error = GetLastError();
				if (create_error != ERROR_ALREADY_EXISTS)
					throw std::runtime_error("Could not create enemy-catalog output directory (Windows error " + std::to_string(create_error) + ")");
			}
			attributes = GetFileAttributesW(directory.c_str());
		}

		if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
			(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
			throw std::runtime_error("enemy-catalog must be a real directory, not a file, link, or reparse point");

		const auto output = directory / (output_name + ".csv");
		if (GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES)
			throw std::runtime_error("Enemy reference output already exists; choose a new safe output name");
		return output;
	}

	void write_enemy_reference_output(const std::filesystem::path& path, const std::string& csv)
	{
		const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
		{
			const auto error = GetLastError();
			if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
				throw std::runtime_error("Enemy reference output already exists; choose a new safe output name");
			throw std::runtime_error("Could not create enemy reference output (Windows error " + std::to_string(error) + ")");
		}

		try
		{
			size_t written_total = 0;
			while (written_total < csv.size())
			{
				const auto bytes_to_write = static_cast<DWORD>(std::min<size_t>(csv.size() - written_total, MAXDWORD));
				DWORD bytes_written = 0;
				if (!WriteFile(handle, csv.data() + written_total, bytes_to_write, &bytes_written, nullptr) || bytes_written != bytes_to_write)
					throw std::runtime_error("Could not write complete enemy reference output");
				written_total += bytes_written;
			}
			if (!FlushFileBuffers(handle))
				throw std::runtime_error("Could not flush enemy reference output");

			LARGE_INTEGER beginning{};
			if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN))
				throw std::runtime_error("Could not seek for enemy reference output readback");
			std::string readback(csv.size(), '\0');
			size_t read_total = 0;
			while (read_total < readback.size())
			{
				const auto bytes_to_read = static_cast<DWORD>(std::min<size_t>(readback.size() - read_total, MAXDWORD));
				DWORD bytes_read = 0;
				if (!ReadFile(handle, readback.data() + read_total, bytes_to_read, &bytes_read, nullptr) || bytes_read == 0)
					throw std::runtime_error("Could not read back complete enemy reference output");
				read_total += bytes_read;
			}
			char extra_byte{};
			DWORD extra_bytes_read = 0;
			if (!ReadFile(handle, &extra_byte, 1, &extra_bytes_read, nullptr) || extra_bytes_read != 0 || readback != csv)
				throw std::runtime_error("Enemy reference output failed exact readback verification");
		}
		catch (...)
		{
			CloseHandle(handle);
			DeleteFileW(path.c_str());
			throw;
		}

		if (!CloseHandle(handle))
		{
			DeleteFileW(path.c_str());
			throw std::runtime_error("Could not close enemy reference output after readback");
		}
	}

	void load_enemy_source_zones(const std::string& donor_zone);

	bool check_enemy_references(const std::string& donor_zone, const std::string& input_path, const std::string& output_name)
	{
		if (donor_zone != "cp_zmb")
			throw std::runtime_error("Enemy reference check currently allows only cp_zmb");
		const auto requests = read_enemy_reference_list(input_path);
		const auto output_path = prepare_enemy_reference_output(output_name);
		if (!zone_exists("cp_zmb"))
			throw std::runtime_error("Spaceland fastfile cp_zmb is missing");

		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		asset_type_filter.clear();
		globals.target_game = game::iw7;
		load_enemy_source_zones("cp_zmb");

		std::vector<enemy_reference_result> results;
		results.reserve(requests.size());
		size_t verified_count = 0;
		size_t missing_count = 0;
		size_t default_count = 0;
		std::string csv = "type,name,status\n";
		for (const auto& request : requests)
		{
			// The existing safe header helper can create a default asset on a miss. Direct entry lookup
			// returns only an already-registered database header and cannot manufacture that default.
			const auto* entry = db_find_x_asset_entry(request.type, request.name.c_str());
			const bool type_matches = entry && entry->type == static_cast<unsigned char>(request.type);
			const auto header = type_matches ? entry->header : XAssetHeader{};
			std::string status;
			if (!header.data)
			{
				status = "missing";
				++missing_count;
			}
			else if (DB_IsXAssetDefault(request.type, request.name.c_str()))
			{
				status = "default";
				++default_count;
			}
			else
			{
				status = "verified";
				++verified_count;
			}

			results.push_back({request.type_name, request.name, status});
			csv += request.type_name + "," + request.name + "," + status + "\n";
		}

		write_enemy_reference_output(output_path, csv);
		printf("ENEMY_REFERENCE_CHECK map=cp_zmb rows=%zu verified=%zu missing=%zu default=%zu output=%s\n",
			results.size(), verified_count, missing_count, default_count, output_path.string().c_str());
		return missing_count == 0 && default_count == 0;
	}

	static std::string enemy_audit_csv_field(const std::string& value)
	{
		if (value.find_first_of("\r\n") != std::string::npos)
			throw std::runtime_error("Enemy animclass audit string contains a line break");
		std::string result = "\"";
		for (const auto character : value)
		{
			if (character == '"') result += '"';
			result += character;
		}
		result += '"';
		return result;
	}

	static std::string enemy_audit_script_string(scr_string_t value)
	{
		if (!value) return {};
		const auto* text = SL_ConvertToString(value);
		if (!text) return "<unresolved>";
		const std::string result(text);
		if (result.size() > 512 || std::any_of(result.begin(), result.end(), [](unsigned char c) { return c < 0x20 || c > 0x7E; }))
			return "<invalid>";
		return result;
	}

	bool audit_enemy_animclasses(const std::string& donor_zone, const std::string& input_path, const std::string& output_name)
	{
		(void)enemy_definition_table_for_map(donor_zone); // exact five-map allowlist
		const auto requests = read_enemy_reference_list(input_path);
		if (requests.size() > 128)
			throw std::runtime_error("Enemy animclass audit is limited to 128 assets");
		if (std::any_of(requests.begin(), requests.end(), [](const auto& request) { return request.type != ASSET_TYPE_ANIMCLASS; }))
			throw std::runtime_error("Enemy animclass audit input may contain only animclass rows");
		const auto output_path = prepare_enemy_reference_output(output_name);
		if (!zone_exists(donor_zone))
			throw std::runtime_error("Requested Zombies donor fastfile is missing: " + donor_zone);

		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		asset_type_filter.clear();
		globals.target_game = game::iw7;
		load_enemy_source_zones(donor_zone);

		std::string csv = "map,name,status,state_count,aim_set_count,aim_set_index,aim_set_name,root_name,anim_count,anim_names_present,anim_indices_present,aim_node_indices_present\n";
		size_t verified = 0;
		size_t with_null_node_arrays = 0;
		for (const auto& request : requests)
		{
			const auto* entry = db_find_x_asset_entry(ASSET_TYPE_ANIMCLASS, request.name.c_str());
			const bool type_matches = entry && entry->type == static_cast<unsigned char>(ASSET_TYPE_ANIMCLASS);
			const auto* asset = type_matches ? entry->header.animClass : nullptr;
			const std::string status = !asset ? "missing" : DB_IsXAssetDefault(ASSET_TYPE_ANIMCLASS, request.name.c_str()) ? "default" : "verified";
			if (status == "verified") ++verified;
			if (!asset || !asset->stateMachine)
			{
				csv += enemy_audit_csv_field(donor_zone) + "," + enemy_audit_csv_field(request.name) + "," + status + ",-1,-1,-1,,,,,,\n";
				continue;
			}

			const auto* machine = asset->stateMachine;
			if (machine->stateCount > 2048 || machine->aimSetCount > 1024 || (machine->aimSetCount && !machine->aimSets))
				throw std::runtime_error("Enemy animclass audit found invalid state-machine dimensions: " + request.name);
			if (!machine->aimSetCount)
			{
				csv += enemy_audit_csv_field(donor_zone) + "," + enemy_audit_csv_field(request.name) + "," + status + "," +
					std::to_string(machine->stateCount) + ",0,-1,,,,,,\n";
				continue;
			}

			for (size_t i = 0; i < machine->aimSetCount; ++i)
			{
				const auto& aim = machine->aimSets[i];
				if (aim.animCount < 0 || aim.animCount > 4096)
					throw std::runtime_error("Enemy animclass audit found invalid aim-set count: " + request.name);
				const bool names = aim.animName != nullptr;
				const bool indices = aim.animIndices != nullptr;
				const bool node_indices = aim.aimNodeIndices != nullptr;
				if (status == "verified" && aim.animCount > 0 && !node_indices) ++with_null_node_arrays;
				csv += enemy_audit_csv_field(donor_zone) + "," + enemy_audit_csv_field(request.name) + "," + status + "," +
					std::to_string(machine->stateCount) + "," + std::to_string(machine->aimSetCount) + "," + std::to_string(i) + "," +
					enemy_audit_csv_field(enemy_audit_script_string(aim.name)) + "," + enemy_audit_csv_field(enemy_audit_script_string(aim.rootName)) + "," +
					std::to_string(aim.animCount) + "," + (names ? "present" : "null") + "," + (indices ? "present" : "null") + "," +
					(node_indices ? "present" : "null") + "\n";
			}
		}

		write_enemy_reference_output(output_path, csv);
		printf("ENEMY_ANIMCLASS_AUDIT map=%s requested=%zu verified=%zu verified_aimsets_with_null_aim_node_indices=%zu output=%s\n",
			donor_zone.c_str(), requests.size(), verified, with_null_node_arrays, output_path.string().c_str());
		fflush(stdout);
		return verified == requests.size();
	}

	bool audit_enemy_model_surfaces(const std::string& donor_zone, const std::string& input_path, const std::string& output_name)
	{
		(void)enemy_definition_table_for_map(donor_zone); // exact five-map allowlist
		const auto requests = read_enemy_reference_list(input_path);
		if (requests.size() > 128)
			throw std::runtime_error("Enemy model-surface audit is limited to 128 models");
		if (std::any_of(requests.begin(), requests.end(), [](const auto& request) { return request.type != ASSET_TYPE_XMODEL; }))
			throw std::runtime_error("Enemy model-surface audit input may contain only xmodel rows");
		const auto output_path = prepare_enemy_reference_output(output_name);
		if (!zone_exists(donor_zone))
			throw std::runtime_error("Requested Zombies donor fastfile is missing: " + donor_zone);

		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		asset_type_filter.clear();
		globals.target_game = game::iw7;
		load_enemy_source_zones(donor_zone);

		std::string csv = "map,model,status,lod_index,xmodelsurfs_name,surface_status,physicsasset_name,physicsfxshape_name\n";
		size_t verified_models = 0;
		size_t verified_surfaces = 0;
		size_t missing_or_default_surfaces = 0;
		for (const auto& request : requests)
		{
			const auto* entry = db_find_x_asset_entry(ASSET_TYPE_XMODEL, request.name.c_str());
			const bool type_matches = entry && entry->type == static_cast<unsigned char>(ASSET_TYPE_XMODEL);
			const auto* model = type_matches ? entry->header.model : nullptr;
			const std::string status = !model ? "missing" : DB_IsXAssetDefault(ASSET_TYPE_XMODEL, request.name.c_str()) ? "default" : "verified";
			if (status == "verified") ++verified_models;
			if (!model)
			{
				csv += enemy_audit_csv_field(donor_zone) + "," + enemy_audit_csv_field(request.name) + "," + status + ",-1,,,,,\n";
				continue;
			}
			if (!model->name || request.name != model->name || model->numLods > 6 || model->numsurfs > 4096)
				throw std::runtime_error("Enemy model-surface audit found invalid model metadata: " + request.name);
			const std::string physics_asset_name = model->physicsAsset ? (model->physicsAsset->name ? model->physicsAsset->name : "") : "";
			const std::string physics_fx_shape_name = model->physicsFXShape ? (model->physicsFXShape->name ? model->physicsFXShape->name : "") : "";
			if ((model->physicsAsset && (physics_asset_name.empty() || !is_safe_enemy_reference_name(physics_asset_name))) ||
				(model->physicsFXShape && (physics_fx_shape_name.empty() || !is_safe_enemy_reference_name(physics_fx_shape_name))))
				throw std::runtime_error("Enemy model-surface audit found an invalid physics reference: " + request.name);
			bool has_surfaces = false;
			for (size_t i = 0; i < 6; ++i)
			{
				const auto* model_surfs = model->lodInfo[i].modelSurfs;
				if (!model_surfs) continue;
				has_surfaces = true;
				if (!model_surfs->name)
					throw std::runtime_error("Enemy model-surface audit found an unnamed XModelSurfs reference: " + request.name);
				const std::string surf_name(model_surfs->name);
				if (!is_safe_enemy_reference_name(surf_name))
					throw std::runtime_error("Enemy model-surface audit found an unsafe XModelSurfs name");
				const auto* surf_entry = db_find_x_asset_entry(ASSET_TYPE_XMODEL_SURFS, surf_name.c_str());
				const bool surf_type_matches = surf_entry && surf_entry->type == static_cast<unsigned char>(ASSET_TYPE_XMODEL_SURFS);
				const auto* surf_header = surf_type_matches ? surf_entry->header.modelSurfs : nullptr;
				const std::string surf_status = !surf_header ? "missing" :
					DB_IsXAssetDefault(ASSET_TYPE_XMODEL_SURFS, surf_name.c_str()) ? "default" : "verified";
				if (surf_status == "verified") ++verified_surfaces;
				else ++missing_or_default_surfaces;
				csv += enemy_audit_csv_field(donor_zone) + "," + enemy_audit_csv_field(request.name) + "," + status + "," +
					std::to_string(i) + "," + enemy_audit_csv_field(surf_name) + "," + surf_status + "," +
					enemy_audit_csv_field(physics_asset_name) + "," + enemy_audit_csv_field(physics_fx_shape_name) + "\n";
			}
			if (!has_surfaces)
				csv += enemy_audit_csv_field(donor_zone) + "," + enemy_audit_csv_field(request.name) + "," + status + ",-1,,," +
					enemy_audit_csv_field(physics_asset_name) + "," + enemy_audit_csv_field(physics_fx_shape_name) + "\n";
		}

		write_enemy_reference_output(output_path, csv);
		printf("ENEMY_XMODEL_SURFACE_AUDIT map=%s requested=%zu verified_models=%zu verified_surfaces=%zu missing_or_default_surfaces=%zu output=%s\n",
			donor_zone.c_str(), requests.size(), verified_models, verified_surfaces, missing_or_default_surfaces, output_path.string().c_str());
		fflush(stdout);
		return verified_models == requests.size() && missing_or_default_surfaces == 0;
	}

	void dump_enemy_weapon_defs(const std::string& donor_zone, const std::string& input_path, const std::string& pack_name)
	{
		(void)enemy_definition_table_for_map(donor_zone); // exact five-map allowlist
		const auto requests = read_enemy_reference_list(input_path);
		if (requests.empty() || requests.size() > 64)
			throw std::runtime_error("Enemy weapon dump requires 1-64 explicit weapon assets");
		if (!is_safe_enemy_token(pack_name) || pack_name.size() > 128 ||
			!pack_name.starts_with("paris_enemy_" + donor_zone + "_") || !pack_name.ends_with("_weapon_source"))
			throw std::runtime_error("Enemy weapon dump pack name must be a safe donor-scoped *_weapon_source token");
		if (std::any_of(requests.begin(), requests.end(), [](const auto& request)
			{ return request.type != ASSET_TYPE_WEAPON || !is_safe_enemy_token(request.name); }))
			throw std::runtime_error("Enemy weapon dump input may contain only safe weapon,name rows");

		const auto output = std::filesystem::path("dump") / pack_name;
		if (std::filesystem::exists(output))
			throw std::runtime_error("Enemy weapon source output must be fresh; choose a new pack name");
		if (!zone_exists(donor_zone))
			throw std::runtime_error("Requested Zombies donor fastfile is missing: " + donor_zone);

		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		globals.target_game = game::iw7;
		asset_type_filter.clear();
		load_enemy_source_zones(donor_zone);

		std::vector<XAsset> assets;
		assets.reserve(requests.size());
		for (const auto& request : requests)
		{
			const auto* entry = db_find_x_asset_entry(ASSET_TYPE_WEAPON, request.name.c_str());
			if (!entry || entry->type != static_cast<unsigned char>(ASSET_TYPE_WEAPON) || !entry->header.data ||
				DB_IsXAssetDefault(ASSET_TYPE_WEAPON, request.name.c_str()))
				throw std::runtime_error("Requested donor weapon is missing or default: " + request.name);
			XAsset asset{};
			asset.type = ASSET_TYPE_WEAPON;
			asset.header = entry->header;
			const auto* registered_name = get_asset_name(&asset);
			if (!registered_name || request.name != registered_name)
				throw std::runtime_error("Donor database returned a mismatched weapon name: " + request.name);
			assets.push_back(asset);
		}

		std::filesystem::create_directories(output / "weapons");
		filesystem::set_fastfile(pack_name);
		globals.dump = true;
		asset_type_filter.insert(ASSET_TYPE_WEAPON);
		const auto restore = gsl::finally([]
		{
			globals.dump = false;
			asset_type_filter.clear();
		});
		std::string manifest = "type,name,status,path,size_bytes,sha256\n";
		for (auto& asset : assets)
		{
			const auto name = std::string(get_asset_name(&asset));
			dump_asset(&asset);
			const auto relative = std::filesystem::path("weapons") / (name + ".json");
			const auto path = output / relative;
			if (!std::filesystem::is_regular_file(path))
				throw std::runtime_error("Donor weapon JSON was not written: " + name);
			const auto bytes = utils::io::read_file(path.string());
			if (bytes.empty()) throw std::runtime_error("Donor weapon JSON is empty: " + name);
			(void)ordered_json::parse(bytes);
			const auto hash = utils::cryptography::sha256::compute(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true);
			manifest += "weapon," + name + ",dumped," + relative.generic_string() + "," + std::to_string(bytes.size()) + "," + hash + "\n";
		}
		const auto manifest_path = output / "weapon-manifest.csv";
		utils::io::write_file(manifest_path.string(), manifest);
		if (utils::io::read_file(manifest_path.string()) != manifest)
			throw std::runtime_error("Enemy weapon manifest failed exact readback verification");
		printf("ENEMY_WEAPON_DUMP map=%s count=%zu output=%s\\weapon-manifest.csv\n",
			donor_zone.c_str(), assets.size(), output.string().c_str());
		fflush(stdout);
	}

	void load_enemy_source_zones(const std::string& donor_zone)
	{
		// Some donor techniques reference graphics stages in these shared shader zones.
		// The full MP core asset zones are unnecessary for that dependency and are not loaded.
		for (const auto* zone : {"techsets_global_core_mp", "techsets_common_core_mp"})
			if (zone_exists(zone)) load_zone(zone, DB_LOAD_SYNC);
		for (const auto* zone : {"code_post_gfx", "global", "common", "global_mp", "global_cp",
			"common_mp", "common_cp", donor_zone.c_str()})
		{
			for (const auto& candidate : {std::string("techsets_") + zone, std::string(zone), std::string("patch_") + zone})
				if (zone_exists(candidate)) load_zone(candidate, DB_LOAD_SYNC);
		}
	}

	std::string string_table_csv(const StringTable* table)
	{
		if (!table || table->columnCount <= 0 || table->columnCount > 256 || table->rowCount < 0 || table->rowCount > 100000 ||
			(table->rowCount && !table->values))
			throw std::runtime_error("Enemy definition table has invalid dimensions or storage");

		std::string csv;
		for (int row = 0; row < table->rowCount; ++row)
		{
			const auto* cells = table->values + static_cast<size_t>(row) * table->columnCount;
			for (int column = 0; column < table->columnCount; ++column)
			{
				if (column) csv += ',';
				csv += '"';
				for (const char* value = cells[column].string ? cells[column].string : ""; *value; ++value)
				{
					if (*value == '"') csv += '"';
					csv += *value;
				}
				csv += '"';
			}
			csv += '\n';
		}
		return csv;
	}

	void dump_enemy_definition_table(const std::string& donor_zone)
	{
		const auto table_name = enemy_definition_table_for_map(donor_zone);
		globals.target_game = game::iw7;
		load_enemy_source_zones(donor_zone);
		auto* table = db_find_x_asset_header_safe(ASSET_TYPE_STRINGTABLE, table_name).stringTable;
		const auto csv = string_table_csv(table);
		std::filesystem::create_directories("enemy-catalog");
		utils::io::write_file("enemy-catalog/" + donor_zone + ".csv", csv);
		printf("ENEMY_TABLE map=%s name=%s rows=%d columns=%d bytes=%zu\n", donor_zone.c_str(), table_name.c_str(),
			table->rowCount, table->columnCount, csv.size());
		fflush(stdout);
	}

	void dump_enemy_zone_inventory(const std::string& zone)
	{
		if (zone != "cp_zmb" && zone != "cp_rave" && zone != "cp_disco" && zone != "cp_town" && zone != "cp_final")
			throw std::runtime_error("Unsupported Zombies zone for enemy asset inventory");
		dump_csv(zone);
	}

	bool is_safe_enemy_script_name(const std::string& name)
	{
		if (name.empty() || name.size() > 240 || name.front() == '/' || name.back() == '/') return false;

		size_t component_start = 0;
		for (size_t i = 0; i <= name.size(); ++i)
		{
			if (i == name.size() || name[i] == '/')
			{
				if (i == component_start) return false;
				const auto component = name.substr(component_start, i - component_start);
				if (component == "." || component == ".." ||
					!std::all_of(component.begin(), component.end(), [](unsigned char c)
						{ return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
					return false;
				component_start = i + 1;
			}
		}
		return true;
	}

	bool is_safe_enemy_script_request_name(const std::string& name)
	{
		if (is_safe_enemy_script_name(name)) return true;
		return !name.empty() && name.size() <= 10 && std::all_of(name.begin(), name.end(), [](unsigned char c)
			{ return c >= '0' && c <= '9'; });
	}

	static std::unordered_map<std::string, ScriptFile*> index_enemy_scriptfiles_by_content_name(
		const std::unordered_set<std::string>& requested_names)
	{
		std::unordered_map<std::string, ScriptFile*> scripts;
		std::unordered_set<std::string> ambiguous_names;
		DB_EnumXAssets(ASSET_TYPE_SCRIPTFILE, [&](XAssetHeader header)
		{
			auto* script = header.scriptfile;
			if (!script || !script->name) return;
			const std::string name(script->name);
			if (!requested_names.contains(name)) return;
			const auto [entry, inserted] = scripts.emplace(name, script);
			if (!inserted && entry->second != script) ambiguous_names.insert(name);
		}, false);
		for (const auto& name : ambiguous_names)
			throw std::runtime_error("Loaded IW7 database has multiple effective ScriptFiles with the same content name: " + name);
		return scripts;
	}

	void dump_enemy_script_catalog(const std::string& donor_zone, const std::string& output_name)
	{
		(void)enemy_definition_table_for_map(donor_zone);
		const auto output_path = prepare_enemy_reference_output(output_name);
		if (!zone_exists(donor_zone))
			throw std::runtime_error("Requested Zombies fastfile is missing: " + donor_zone);

		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		asset_type_filter.clear();
		globals.target_game = game::iw7;
		load_enemy_source_zones(donor_zone);

		struct catalog_row
		{
			std::string database_name;
			std::string content_name;
			std::string status;
			std::string payload_sha256;
			int compressed_len{};
			int source_len{};
			int bytecode_len{};
		};
		std::vector<catalog_row> rows;
		bool invalid_asset = false;
		std::string invalid_name;
		DB_EnumXAssets(ASSET_TYPE_SCRIPTFILE, [&](XAssetHeader header)
		{
			if (rows.size() >= 4096)
			{
				invalid_asset = true;
				invalid_name = "loaded ScriptFile count exceeds 4096";
				return;
			}
			auto* script = header.scriptfile;
			const auto* database_name = script ? get_asset_name(ASSET_TYPE_SCRIPTFILE, script) : nullptr;
			if (!script || !script->name || !database_name)
			{
				invalid_asset = true;
				invalid_name = "loaded ScriptFile has no header or name";
				return;
			}

			const std::string content_name(script->name);
			const std::string db_name(database_name);
			constexpr int max_script_bytes = 64 * 1024 * 1024;
			const bool valid_content_name = content_name == "$default" || is_safe_enemy_script_request_name(content_name);
			if (!valid_content_name || db_name.empty() || db_name.size() > 240 ||
				std::any_of(db_name.begin(), db_name.end(), [](unsigned char c) { return c < 0x20 || c > 0x7E; }) ||
				script->compressedLen < 0 || script->len < 0 || script->bytecodeLen < 0 ||
				script->compressedLen > max_script_bytes || script->len > max_script_bytes || script->bytecodeLen > max_script_bytes ||
				(script->compressedLen > 0 && !script->buffer) || (script->bytecodeLen > 0 && !script->bytecode))
			{
				invalid_asset = true;
				invalid_name = content_name;
				return;
			}

			const auto status = content_name == "$default" || db_name == "$default" ? "sentinel" :
				DB_IsXAssetDefault(ASSET_TYPE_SCRIPTFILE, db_name.c_str()) ? "default" : "registered";
			std::string payload;
			const auto payload_size = sizeof(script->compressedLen) + sizeof(script->len) + sizeof(script->bytecodeLen) +
				static_cast<size_t>(script->compressedLen) + static_cast<size_t>(script->bytecodeLen);
			payload.reserve(payload_size);
			payload.append(reinterpret_cast<const char*>(&script->compressedLen), sizeof(script->compressedLen));
			payload.append(reinterpret_cast<const char*>(&script->len), sizeof(script->len));
			payload.append(reinterpret_cast<const char*>(&script->bytecodeLen), sizeof(script->bytecodeLen));
			if (script->compressedLen) payload.append(script->buffer, static_cast<size_t>(script->compressedLen));
			if (script->bytecodeLen) payload.append(script->bytecode, static_cast<size_t>(script->bytecodeLen));
			const auto payload_sha256 = utils::cryptography::sha256::compute(
				reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), true);
			rows.push_back({db_name, content_name, status, payload_sha256,
				script->compressedLen, script->len, script->bytecodeLen});
		}, false);
		if (invalid_asset)
			throw std::runtime_error("IW7 ScriptFile catalog is incomplete or invalid: " + invalid_name);
		std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right)
			{ return std::tie(left.database_name, left.content_name) < std::tie(right.database_name, right.content_name); });

		std::string csv = "map,database_name,content_name,status,compressed_len,source_len,bytecode_len,payload_sha256\n";
		for (const auto& row : rows)
			csv += enemy_audit_csv_field(donor_zone) + "," + enemy_audit_csv_field(row.database_name) + "," +
				enemy_audit_csv_field(row.content_name) + "," + row.status + "," + std::to_string(row.compressed_len) + "," +
				std::to_string(row.source_len) + "," + std::to_string(row.bytecode_len) + "," + row.payload_sha256 + "\n";
		write_enemy_reference_output(output_path, csv);
		printf("ENEMY_SCRIPT_CATALOG map=%s rows=%zu output=%s\n",
			donor_zone.c_str(), rows.size(), output_path.string().c_str());
		fflush(stdout);
	}

	void dump_enemy_scripts(const std::string& donor_zone, const std::string& list_path, const std::string& pack_name)
	{
		// Reuse the map allowlist; this command exports only compiled assets from one supported Zombies donor.
		(void)enemy_definition_table_for_map(donor_zone);
		const auto pack_prefix = "paris_enemy_" + donor_zone + "_";
		if (!is_safe_enemy_token(pack_name) || pack_name.size() > 128 || !pack_name.starts_with(pack_prefix) ||
			!pack_name.ends_with("_scripts") || pack_name.size() <= pack_prefix.size() + std::string("_scripts").size())
			throw std::runtime_error("Enemy script pack must be paris_enemy_<donor-map>_<name>_scripts");

		std::string list_data;
		if (!utils::io::read_file(list_path, &list_data))
			throw std::runtime_error("Could not read enemy script list: " + list_path);
		if (list_data.size() > 64 * 1024)
			throw std::runtime_error("Enemy script list exceeds the 64 KiB limit");
		if (list_data.starts_with("\xEF\xBB\xBF")) list_data.erase(0, 3);

		std::vector<std::string> script_names;
		std::unordered_set<std::string> unique_names;
		for (size_t offset = 0; offset < list_data.size();)
		{
			const auto newline = list_data.find('\n', offset);
			auto line = list_data.substr(offset, newline == std::string::npos ? std::string::npos : newline - offset);
			offset = newline == std::string::npos ? list_data.size() : newline + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const auto first = line.find_first_not_of(" \t");
			if (first == std::string::npos) continue;
			const auto last = line.find_last_not_of(" \t");
			line = line.substr(first, last - first + 1);
			if (line.front() == '#') continue;
			if (!is_safe_enemy_script_request_name(line))
				throw std::runtime_error("Unsafe IW7 script module path or numeric ScriptFile id in list: " + line);
			if (!unique_names.insert(line).second)
				throw std::runtime_error("Duplicate IW7 script module in list: " + line);
			if (script_names.size() >= 512)
				throw std::runtime_error("Enemy script list exceeds the 512 module limit");
			script_names.push_back(std::move(line));
		}
		if (script_names.empty())
			throw std::runtime_error("Enemy script list contains no module names");
		if (!zone_exists(donor_zone))
			throw std::runtime_error("Donor Zombies fastfile is missing: " + donor_zone);

		// Loading source zones must not enable the global database-add dump hook. Export only the
		// exact ScriptFile headers requested below, after the database is ready.
		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		asset_type_filter.clear();
		globals.target_game = game::iw7;
		filesystem::set_fastfile(pack_name);
		load_enemy_source_zones(donor_zone);
		// IW7's ScriptFile database keys can be numeric even though ScriptFile::name contains the
		// module path. Enumerate effective registered assets and index that field instead of asking
		// DB_FindXAssetEntry to hash the path as if it were the database key.
		const auto scripts_by_content_name = index_enemy_scriptfiles_by_content_name(unique_names);
		printf("ENEMY_SCRIPT_INDEX map=%s available=%zu\n", donor_zone.c_str(), scripts_by_content_name.size());

		struct dump_result
		{
			std::string name;
			std::string status;
			std::string path;
			uintmax_t size{};
			std::string sha256;
		};
		std::vector<dump_result> results;
		results.reserve(script_names.size());
		bool incomplete = false;
		const auto output_root = std::filesystem::path(filesystem::get_dump_path());
		std::filesystem::create_directories(output_root);

		globals.dump = true;
		asset_type_filter = {ASSET_TYPE_SCRIPTFILE};
		try
		{
			for (const auto& name : script_names)
			{
				dump_result result{name, "missing_asset", (std::filesystem::path("dump") / pack_name / (name + ".gscbin")).generic_string(), 0, {}};
				const auto found = scripts_by_content_name.find(name);
				if (found == scripts_by_content_name.end())
				{
					incomplete = true;
					ZONETOOL_ERROR("Requested donor scriptfile was not found: %s", name.c_str());
					results.push_back(std::move(result));
					continue;
				}
				auto* script = found->second;
				XAssetHeader header{};
				header.scriptfile = script;

				const auto asset_name = script->name ? std::string(script->name) : std::string();
				constexpr int max_script_bytes = 64 * 1024 * 1024;
				if (asset_name != name || script->compressedLen < 0 || script->len < 0 || script->bytecodeLen < 0 ||
					script->compressedLen > max_script_bytes || script->len > max_script_bytes || script->bytecodeLen > max_script_bytes ||
					(script->compressedLen > 0 && !script->buffer) || (script->bytecodeLen > 0 && !script->bytecode))
				{
					incomplete = true;
					result.status = "invalid_asset";
					ZONETOOL_ERROR("Requested donor scriptfile has invalid metadata: %s", name.c_str());
					results.push_back(std::move(result));
					continue;
				}

				const auto output_path = output_root / (name + ".gscbin");
				std::error_code remove_error;
				std::filesystem::remove(output_path, remove_error);
				if (remove_error)
				{
					incomplete = true;
					result.status = "remove_failed";
					ZONETOOL_ERROR("Could not remove stale compiled donor scriptfile: %s", output_path.string().c_str());
					results.push_back(std::move(result));
					continue;
				}

				XAsset asset{ASSET_TYPE_SCRIPTFILE, header};
				dump_asset(&asset);
				if (!std::filesystem::is_regular_file(output_path))
				{
					incomplete = true;
					result.status = "write_failed";
					ZONETOOL_ERROR("Could not write compiled donor scriptfile: %s", output_path.string().c_str());
					results.push_back(std::move(result));
					continue;
				}

				result.size = std::filesystem::file_size(output_path);
				const auto expected_size = name.size() + 1 + sizeof(int) * 3 +
					static_cast<uintmax_t>(script->compressedLen) + static_cast<uintmax_t>(script->bytecodeLen);
				if (result.size != expected_size || result.size == 0)
				{
					incomplete = true;
					result.status = "size_mismatch";
					ZONETOOL_ERROR("Compiled donor scriptfile size mismatch for %s (actual=%llu expected=%llu)", name.c_str(),
						static_cast<unsigned long long>(result.size), static_cast<unsigned long long>(expected_size));
					results.push_back(std::move(result));
					continue;
				}

				std::string expected;
				expected.reserve(static_cast<size_t>(expected_size));
				expected.append(name.data(), name.size());
				expected.push_back('\0');
				expected.append(reinterpret_cast<const char*>(&script->compressedLen), sizeof(script->compressedLen));
				expected.append(reinterpret_cast<const char*>(&script->len), sizeof(script->len));
				expected.append(reinterpret_cast<const char*>(&script->bytecodeLen), sizeof(script->bytecodeLen));
				if (script->compressedLen) expected.append(script->buffer, static_cast<size_t>(script->compressedLen));
				if (script->bytecodeLen) expected.append(script->bytecode, static_cast<size_t>(script->bytecodeLen));
				const auto dumped = utils::io::read_file(output_path.string());
				if (dumped != expected)
				{
					incomplete = true;
					result.status = "content_mismatch";
					ZONETOOL_ERROR("Compiled donor ScriptFile content mismatch for %s", name.c_str());
					results.push_back(std::move(result));
					continue;
				}
				result.sha256 = utils::cryptography::sha256::compute(
					reinterpret_cast<const uint8_t*>(dumped.data()), dumped.size(), true);
				result.status = "dumped";
				results.push_back(std::move(result));
			}
		}
		catch (...)
		{
			globals.dump = false;
			asset_type_filter.clear();
			throw;
		}
		globals.dump = false;
		asset_type_filter.clear();

		std::string manifest = "donor_map,pack_name,module,status,path,size_bytes,sha256\n";
		for (const auto& result : results)
			manifest += donor_zone + "," + pack_name + "," + result.name + "," + result.status + "," + result.path + "," +
				std::to_string(result.size) + "," + result.sha256 + "\n";
		const auto manifest_path = output_root / "scriptfile-manifest.csv";
		utils::io::write_file(manifest_path.string(), manifest);
		if (!std::filesystem::is_regular_file(manifest_path) || std::filesystem::file_size(manifest_path) != manifest.size())
			throw std::runtime_error("Could not write complete enemy script dump manifest: " + manifest_path.string());
		ZONETOOL_INFO("Enemy script dump manifest: %s", manifest_path.string().c_str());
		if (incomplete)
			throw std::runtime_error("Enemy script dump incomplete; inspect scriptfile-manifest.csv and the errors above");
	}

	void dump_enemy_images(const std::string& donor_zone, const std::string& list_path, const std::string& pack_name)
	{
		(void)enemy_definition_table_for_map(donor_zone);
		if (!utils::flags::has_flag("dump_streamed_image") || utils::flags::has_flag("dds"))
			throw std::runtime_error("Enemy image capture requires -dump_streamed_image and raw pixels (no -dds)");
		if (!is_safe_enemy_token(pack_name) || pack_name.size() > 128 ||
			!pack_name.starts_with("paris_enemy_" + donor_zone + "_") || !pack_name.ends_with("_images"))
			throw std::runtime_error("Enemy image source pack must be paris_enemy_<donor-map>_<name>_images");
		std::string list;
		if (!utils::io::read_file(list_path, &list) || list.size() > 64 * 1024)
			throw std::runtime_error("Enemy image list is missing or exceeds 64 KiB");
		if (list.starts_with("\xEF\xBB\xBF")) list.erase(0, 3);
		enemy_image_names.clear();
		enemy_images_dumped.clear();
		enemy_image_sources.clear();
		enemy_image_capture_count = 0;
		for (size_t offset = 0; offset < list.size();)
		{
			const auto end = list.find('\n', offset);
			auto name = list.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
			offset = end == std::string::npos ? list.size() : end + 1;
			const auto first = name.find_first_not_of(" \t\r");
			if (first == std::string::npos) continue;
			name = name.substr(first, name.find_last_not_of(" \t\r") - first + 1);
			if (name.starts_with("#")) continue;
			if (name.empty() || name.size() > 240 || name == "." || name == ".." ||
				!std::all_of(name.begin(), name.end(), [](unsigned char c)
					{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
						(c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '&' || c == '$'; }))
				throw std::runtime_error("Unsafe enemy image name in explicit list");
			if (!enemy_image_names.insert(name).second || enemy_image_names.size() > 512)
				throw std::runtime_error("Duplicate enemy image name or more than 512 requested images");
		}
		if (enemy_image_names.empty()) throw std::runtime_error("Enemy image list is empty");
		const auto output = std::filesystem::path("dump") / pack_name;
		if (std::filesystem::exists(output)) throw std::runtime_error("Enemy image output must be fresh; choose a new source pack name");
		std::filesystem::create_directories(output);
		enemy_image_pack = pack_name;
		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		enemy_image_capture = true;
		const auto restore = gsl::finally([] { enemy_image_capture = false; });
		load_enemy_source_zones(donor_zone);
		for (const auto& [name, source] : enemy_image_sources)
		{
			const auto destination = output / source.filename();
			std::filesystem::create_directories(destination);
			for (const auto& file : std::filesystem::directory_iterator(source))
			{
				if (!file.is_regular_file()) continue;
				const auto filename = file.path().filename().string();
				if (filename != name + ".iw7Image" && !filename.starts_with(name + "_stream")) continue;
				std::filesystem::copy_file(file.path(), destination / filename);
				if (utils::io::read_file(file.path().string()) != utils::io::read_file((destination / filename).string()))
					throw std::runtime_error("Enemy image selected-source copy mismatch");
			}
		}
		std::string manifest = "donor_map,image,status,path,size_bytes,sha256\n";
		for (const auto& name : enemy_image_names)
		{
			if (!enemy_images_dumped.contains(name))
			{
				manifest += donor_zone + "," + name + ",missing_registration,,,\n";
				continue;
			}
			const auto selected = output / enemy_image_sources.at(name).filename();
			for (const auto& file : std::filesystem::directory_iterator(selected))
			{
				if (!file.is_regular_file()) continue;
				const auto filename = file.path().filename().string();
				if (filename != name + ".iw7Image" && !filename.starts_with(name + "_stream")) continue;
				const auto bytes = utils::io::read_file(file.path().string());
				const auto hash = utils::cryptography::sha256::compute(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true);
				manifest += donor_zone + "," + name + ",dumped," + file.path().generic_string() + "," + std::to_string(bytes.size()) + "," + hash + "\n";
			}
		}
		utils::io::write_file((output / "image-manifest.csv").string(), manifest);
		if (utils::io::read_file((output / "image-manifest.csv").string()) != manifest)
			throw std::runtime_error("Could not write enemy image manifest");
		if (enemy_images_dumped.size() != enemy_image_names.size())
			throw std::runtime_error("Enemy image registration capture is incomplete; inspect image-manifest.csv");
	}

	void dump_enemy_physics_assets(const std::string& donor_zone, const std::string& input_path, const std::string& pack_name)
	{
		(void)enemy_definition_table_for_map(donor_zone);
		if (!is_safe_enemy_token(pack_name) || pack_name.size() > 128 ||
			!pack_name.starts_with("paris_enemy_" + donor_zone + "_") || !pack_name.ends_with("_physics"))
			throw std::runtime_error("Enemy physics source pack must be paris_enemy_<donor-map>_<name>_physics");

		const auto requests = read_enemy_reference_list(input_path);
		if (requests.size() > 128)
			throw std::runtime_error("Enemy physics capture is limited to 128 assets");
		for (const auto& request : requests)
		{
			if (request.type != ASSET_TYPE_PHYSICSASSET || !is_safe_enemy_token(request.name))
				throw std::runtime_error("Enemy physics input accepts only safe physicsasset names");
		}

		const auto output = std::filesystem::path("dump") / pack_name;
		if (std::filesystem::exists(output))
			throw std::runtime_error("Enemy physics output must be fresh; choose a new source pack name");
		std::filesystem::create_directories(output);

		enemy_physics_names.clear();
		enemy_physics_sources.clear();
		enemy_physics_capture_count = 0;
		enemy_physics_pack = pack_name;
		for (const auto& request : requests) enemy_physics_names.insert(request.name);
		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		asset_type_filter.clear();
		globals.target_game = game::iw7;
		enemy_physics_capture = true;
		const auto restore_capture = gsl::finally([] { enemy_physics_capture = false; });
		load_enemy_source_zones(donor_zone);
		enemy_physics_capture = false;

		std::string manifest = "donor_map,physicsasset,status,registration_count,havok_size_bytes,havok_sha256\n";
		bool complete = true;
		for (const auto& request : requests)
		{
			const auto captures = enemy_physics_sources.find(request.name);
			if (captures == enemy_physics_sources.end() || captures->second.empty())
			{
				manifest += donor_zone + "," + request.name + ",missing_registration,0,,\n";
				complete = false;
				continue;
			}

			// Zone load order ends with the donor map and its patch; use the last
			// pre-registration copy, which is the version left active in the DB.
			const auto& selected_root = captures->second.back();
			for (const auto& entry : std::filesystem::recursive_directory_iterator(selected_root))
			{
				if (!entry.is_regular_file()) continue;
				const auto relative = entry.path().lexically_relative(selected_root);
				const auto destination = output / relative;
				std::filesystem::create_directories(destination.parent_path());
				const auto source_bytes = utils::io::read_file(entry.path().string());
				if (std::filesystem::exists(destination))
				{
					if (utils::io::read_file(destination.string()) != source_bytes)
						throw std::runtime_error("Conflicting PhysicsAsset dependency source bytes: " + relative.generic_string());
				}
				else
				{
					std::filesystem::copy_file(entry.path(), destination);
					if (utils::io::read_file(destination.string()) != source_bytes)
						throw std::runtime_error("PhysicsAsset dependency source copy failed exact readback: " + relative.generic_string());
				}
			}

			const auto source = output / "physicsasset" / request.name;
			const auto havok_file = std::filesystem::path(source.string() + havok::binary::havok_file_ext);
			if (!std::filesystem::is_regular_file(source) || !std::filesystem::is_regular_file(havok_file) ||
				std::filesystem::file_size(source) == 0 || std::filesystem::file_size(havok_file) == 0)
				throw std::runtime_error("Selected enemy PhysicsAsset source pair is incomplete: " + request.name);
			const auto bytes = utils::io::read_file(havok_file.string());
			const auto hash = utils::cryptography::sha256::compute(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), true);
			manifest += donor_zone + "," + request.name + ",captured," + std::to_string(captures->second.size()) + "," +
				std::to_string(bytes.size()) + "," + hash + "\n";
		}

		const auto manifest_path = output / "physicsasset-manifest.csv";
		utils::io::write_file(manifest_path.string(), manifest);
		if (utils::io::read_file(manifest_path.string()) != manifest)
			throw std::runtime_error("Enemy physics manifest failed exact readback verification");
		if (!complete)
			throw std::runtime_error("Enemy PhysicsAsset capture is incomplete; inspect physicsasset-manifest.csv");
		ZONETOOL_INFO("ENEMY_PHYSICS_DUMP map=%s assets=%zu registrations=%zu output=%s\\physicsasset-manifest.csv",
			donor_zone.c_str(), requests.size(), enemy_physics_capture_count, output.string().c_str());
		fflush(stdout);
		enemy_physics_names.clear();
		enemy_physics_sources.clear();
	}

	void extract_enemy_definition(const std::string& donor_zone, const std::string& table_name,
		const std::string& agent_type, size_t type_column, const std::string& pack_name)
	{
		if (!is_safe_enemy_token(agent_type) || table_name != enemy_definition_table_for_map(donor_zone) ||
			pack_name != "paris_enemy_" + donor_zone + "_" + agent_type)
			throw std::runtime_error("Unsafe or inconsistent IW7 enemy extraction arguments");

		filesystem::set_fastfile(pack_name);
		globals.target_game = game::iw7;
		globals.dump = false;
		globals.dump_csv = false;
		globals.verify = false;
		asset_type_filter.clear();
		load_enemy_source_zones(donor_zone);

		auto* table = db_find_x_asset_header_safe(ASSET_TYPE_STRINGTABLE, table_name).stringTable;
		if (!table || table->columnCount <= 0 || table->columnCount > 256 || table->rowCount < 0 || table->rowCount > 100000 ||
			type_column >= static_cast<size_t>(table->columnCount) || (table->rowCount && !table->values))
			throw std::runtime_error("Enemy definition table is missing the requested agent type column");
		std::string row;
		int matches = 0;
		for (int source_row = 0; source_row < table->rowCount; ++source_row)
		{
			const auto* cells = table->values + static_cast<size_t>(source_row) * table->columnCount;
			if (!cells[type_column].string || agent_type != cells[type_column].string) continue;
			++matches;
			std::string filtered;
			for (int column = 0; column < table->columnCount; ++column)
			{
				if (column) filtered += ',';
				filtered += '"';
				for (const char* value = cells[column].string ? cells[column].string : ""; *value; ++value)
				{
					if (*value == '"') filtered += '"';
					filtered += *value;
				}
				filtered += '"';
			}
			filtered += '\n';
			row += filtered;
		}
		if (matches != 1) throw std::runtime_error("Expected exactly one matching enemy definition row");
		const auto definition_path = "zonetool/" + pack_name + "/mp/" + pack_name + "_definition.csv";
		std::filesystem::create_directories(std::filesystem::path(definition_path).parent_path());
		utils::io::write_file(definition_path, row);
	}

	void catalog_soundbank(const std::string& donor_zone, const std::string& bank_name, const std::string& output_name)
	{
		// Reject before creating output or loading any requested zone.
		if (!sound_bank_subset::is_catalog_donor(donor_zone))
			throw std::runtime_error("Soundbank catalog donor must be cp_zmb, cp_rave, cp_disco, cp_town, or cp_final");
		if (!is_safe_enemy_reference_name(bank_name)) throw std::runtime_error("Invalid soundbank name");
		const auto output = prepare_enemy_reference_output(output_name);
		globals.target_game = game::iw7;
		globals.dump = globals.dump_csv = globals.verify = false;
		asset_type_filter.clear();
		load_enemy_source_zones(donor_zone);
		const auto* bank = db_find_x_asset_header_safe(ASSET_TYPE_SOUND_BANK, bank_name).soundBank;
		if (!bank || DB_IsXAssetDefault(ASSET_TYPE_SOUND_BANK, bank_name.c_str()) ||
			!bank->alias || !bank->aliasCount || bank->aliasCount > 65535)
			throw std::runtime_error("Soundbank catalog donor is missing or invalid");
		const auto quote = [](const char* value)
		{
			std::string text = "\"";
			for (const char* p = value ? value : ""; *p; ++p) { if (*p == '"') text += '"'; text += *p; }
			return text + '"';
		};
		std::string csv = "alias,id,head,asset_id,secondary,secondary_id,stop,stop_id,duck,load_type,channel\n";
		for (unsigned int i = 0; i < bank->aliasCount; ++i)
		{
			const auto& list = bank->alias[i];
			if (!list.aliasName || !list.head || list.count <= 0 || list.count > 65535)
				throw std::runtime_error("Invalid soundbank catalog alias list");
			for (int h = 0; h < list.count; ++h)
			{
				const auto& alias = list.head[h];
				csv += quote(list.aliasName) + ',' + std::to_string(list.id) + ',' + std::to_string(h) + ',' +
					std::to_string(alias.assetId) + ',' + quote(alias.secondaryAliasName) + ',' + std::to_string(alias.secondaryId) + ',' +
					quote(alias.stopAliasName) + ',' + std::to_string(alias.stopAliasID) + ',' + std::to_string(alias.duck) + ',' +
					std::to_string(alias.flags.type) + ',' + std::to_string(alias.flags.channel) + '\n';
			}
		}
		write_enemy_reference_output(output, csv);
		ZONETOOL_INFO("Soundbank catalog: %s; alias lists=%u; metadata only; output=%s", bank_name.c_str(), bank->aliasCount, output.string().c_str());
	}

	void handle_params()
	{
		// Execute command line commands
		auto args = get_command_line_arguments();
		if (args.size() > 1)
		{
			bool do_exit = false;

			for (std::size_t i = 0; i < args.size(); i++)
			{
				if (args[i] == "-soundbank-catalog")
				{
					try
					{
						if (i + 3 >= args.size()) throw std::runtime_error("usage: -soundbank-catalog <Zombies-map> <bank> <safe-output-name>");
						catalog_soundbank(args[i + 1], args[i + 2], args[i + 3]);
						fflush(nullptr);
						std::quick_exit(EXIT_SUCCESS);
					}
					catch (const std::exception& error) { ZONETOOL_ERROR("Soundbank catalog failed: %s", error.what()); }
					catch (...) { ZONETOOL_ERROR("Soundbank catalog failed with an unknown error"); }
					fflush(nullptr);
					std::quick_exit(EXIT_FAILURE);
				}
				if (args[i] == "-enemy-script-catalog")
				{
					try
					{
						if (i + 2 >= args.size())
							throw std::runtime_error("usage: -enemy-script-catalog <cp_zmb|cp_rave|cp_disco|cp_town|cp_final> <safe-output-name>");
						dump_enemy_script_catalog(args[i + 1], args[i + 2]);
						fflush(nullptr);
						std::quick_exit(EXIT_SUCCESS);
					}
					catch (const std::exception& error)
					{
						ZONETOOL_ERROR("Enemy script catalog failed: %s", error.what());
					}
					catch (...)
					{
						ZONETOOL_ERROR("Enemy script catalog failed with an unknown error");
					}
					fflush(nullptr);
					std::quick_exit(EXIT_FAILURE);
				}

				if (args[i] == "-enemy-xmodel-surface-audit")
				{
					try
					{
						if (i + 3 >= args.size())
							throw std::runtime_error("usage: -enemy-xmodel-surface-audit <cp_zmb|cp_rave|cp_disco|cp_town|cp_final> <xmodel-list.csv> <safe-output-name>");
						const auto complete = audit_enemy_model_surfaces(args[i + 1], args[i + 2], args[i + 3]);
						fflush(nullptr);
						std::quick_exit(complete ? EXIT_SUCCESS : EXIT_FAILURE);
					}
					catch (const std::exception& error)
					{
						ZONETOOL_ERROR("Enemy XModel surface audit failed: %s", error.what());
					}
					catch (...)
					{
						ZONETOOL_ERROR("Enemy XModel surface audit failed with an unknown error");
					}
					fflush(nullptr);
					std::quick_exit(EXIT_FAILURE);
				}

				if (args[i] == "-enemy-weapon-dump")
				{
					try
					{
						if (i + 3 >= args.size())
							throw std::runtime_error("usage: -enemy-weapon-dump <cp_zmb|cp_rave|cp_disco|cp_town|cp_final> <weapon-list.csv> <paris_enemy_<map>_<name>_weapon_source>");
						dump_enemy_weapon_defs(args[i + 1], args[i + 2], args[i + 3]);
						fflush(nullptr);
						std::quick_exit(EXIT_SUCCESS);
					}
					catch (const std::exception& error)
					{
						ZONETOOL_ERROR("Enemy weapon dump failed: %s", error.what());
					}
					catch (...)
					{
						ZONETOOL_ERROR("Enemy weapon dump failed with an unknown error");
					}
					fflush(nullptr);
					std::quick_exit(EXIT_FAILURE);
				}

				if (args[i] == "-enemy-animclass-audit")
				{
					try
					{
						if (i + 3 >= args.size())
							throw std::runtime_error("usage: -enemy-animclass-audit <cp_zmb|cp_rave|cp_disco|cp_town|cp_final> <animclass-list.csv> <safe-output-name>");
						const auto complete = audit_enemy_animclasses(args[i + 1], args[i + 2], args[i + 3]);
						fflush(nullptr);
						std::quick_exit(complete ? EXIT_SUCCESS : EXIT_FAILURE);
					}
					catch (const std::exception& error)
					{
						ZONETOOL_ERROR("Enemy animclass audit failed: %s", error.what());
					}
					catch (...)
					{
						ZONETOOL_ERROR("Enemy animclass audit failed with an unknown error");
					}
					fflush(nullptr);
					std::quick_exit(EXIT_FAILURE);
				}

				if (args[i] == "-enemy-physics-dump")
				{
					try
					{
						if (i + 3 >= args.size())
							throw std::runtime_error("usage: -enemy-physics-dump <donor-map> <physicsasset-list.csv> <paris_enemy_<donor-map>_<name>_physics>");
						dump_enemy_physics_assets(args[i + 1], args[i + 2], args[i + 3]);
						fflush(nullptr);
						std::quick_exit(EXIT_SUCCESS);
					}
					catch (const std::exception& error)
					{
						ZONETOOL_ERROR("Enemy physics dump failed: %s", error.what());
					}
					catch (...)
					{
						ZONETOOL_ERROR("Enemy physics dump failed with an unknown error");
					}
					fflush(nullptr);
					std::quick_exit(EXIT_FAILURE);
				}

				if (args[i] == "-enemy-reference-check")
				{
					try
					{
						if (i + 3 >= args.size())
							throw std::runtime_error("usage: -enemy-reference-check <cp_zmb> <type,name-list.csv> <safe-output-name>");
						const auto all_verified = check_enemy_references(args[i + 1], args[i + 2], args[i + 3]);
						fflush(nullptr);
						std::quick_exit(all_verified ? EXIT_SUCCESS : EXIT_FAILURE);
					}
					catch (const std::exception& error)
					{
						ZONETOOL_ERROR("Enemy reference check failed: %s", error.what());
					}
					catch (...)
					{
						ZONETOOL_ERROR("Enemy reference check failed with an unknown error");
					}
					fflush(nullptr);
					std::quick_exit(EXIT_FAILURE);
				}

				if (i < args.size() - 1 && i + 1 < args.size())
				{
					if (args[i] == "-enemy-image-dump")
					{
						if (i + 3 >= args.size())
							throw std::runtime_error("usage: -dump_streamed_image -enemy-image-dump <donor-map> <image-list> <source-pack>");
						dump_enemy_images(args[i + 1], args[i + 2], args[i + 3]);
						i += 3;
						do_exit = true;
					}
					else if (args[i] == "-enemy-script-dump")
					{
						if (i + 3 >= args.size())
							throw std::runtime_error("usage: -enemy-script-dump <donor-map> <script-list-file> <pack-name>");
						dump_enemy_scripts(args[i + 1], args[i + 2], args[i + 3]);
						i += 3;
						do_exit = true;
					}
					else if (args[i] == "-enemy-zone-dump")
					{
						dump_enemy_zone_inventory(args[i + 1]);
						i++;
						do_exit = true;
					}
					else if (args[i] == "-enemy-table-dump")
					{
						dump_enemy_definition_table(args[i + 1]);
						i++;
						do_exit = true;
					}
					else if (args[i] == "-enemy-extract")
					{
						if (i + 5 >= args.size())
							throw std::runtime_error("usage: -enemy-extract <donor-map> <definition-table> <agent-type> <agent-type-column> <pack-name>");
						size_t parsed_length = 0;
						const auto type_column_value = std::stoul(args[i + 4], &parsed_length);
						if (parsed_length != args[i + 4].size() || type_column_value > 255)
							throw std::runtime_error("Invalid enemy definition agent-type column");
						extract_enemy_definition(args[i + 1], args[i + 2], args[i + 3], type_column_value, args[i + 5]);
						i += 5;
						do_exit = true;
					}
					else if (args[i] == "-loadzone")
					{
						load_zone(args[i + 1]);
						i++;

						do_exit = true;
					}
					else if (args[i] == "-iwzcompile" && i + 4 < args.size())
					{
						iwzmap::compile(args[i + 1], args[i + 2], args[i + 3], args[i + 4]);
						i += 4;

						do_exit = true;
					}
					else if (args[i] == "-iwzbounds" && i + 3 < args.size())
					{
						iwzmap::smodel_bounds(args[i + 1], args[i + 2], args[i + 3]);
						i += 3;

						do_exit = true;
					}
					else if (args[i] == "-buildzone")
					{
						build_zone(args[i + 1]);
						i++;

						do_exit = true;
					}
					else if (args[i] == "-buildzones")
					{
						const auto& filename = args[i + 1];
						std::string data{};
						if (!utils::io::read_file(filename, &data))
						{
							return;
						}

						const auto zones = utils::string::split(data, '\n');
						for (auto zone : zones)
						{
							if (zone.ends_with("\r"))
							{
								zone.pop_back();
							}

							build_zone(zone);
						}

						i++;

						do_exit = true;
					}
					else if (args[i] == "-verifyzone")
					{
						verify_zone(args[i + 1]);
						i++;

						do_exit = true;
					}
					else if (args[i] == "-dumpzonetypes" && i + 2 < args.size())
					{
						// -dumpzonetypes <zone> <type,type,...>: dump only the listed asset types
						asset_type_filter.clear();
						for (const auto& type_str : utils::string::split(args[i + 2], ','))
						{
							const auto type = type_to_int(type_str);
							if (type != -1) asset_type_filter.insert(static_cast<XAssetType>(type));
							else ZONETOOL_ERROR("Asset type \"%s\" does not exist", type_str.data());
						}
						// a map zone only references its materials and techsets (",name"): their definitions are in
						// techsets_<zone>; load that zone first so the references resolve and can be dumped
						const auto companion = "techsets_"s + args[i + 1];
						if (!args[i + 1].starts_with("techsets_") && zone_exists(companion) && !is_zone_loaded(companion))
						{
							ZONETOOL_INFO("Loading \"%s\" first (definitions of the zone's referenced materials/techsets)", companion.data());
							load_zone(companion, DB_LOAD_ASYNC, false);
							wait_for_database();
						}
						dump_zone(args[i + 1], game::iw7);
						i += 2;

						do_exit = true;
					}
					else if (args[i] == "-dumpzone")
					{
						dump_zone(args[i + 1], game::iw7);
						i++;

						do_exit = true;
					}
				}
			}

			if (do_exit)
			{
				fflush(nullptr);
				std::quick_exit(EXIT_SUCCESS);
			}
		}
	}

	void on_exit(void)
	{
		globals.verify = false;
		globals.dump = false;
		globals.dump_csv = false;
		globals.csv_file.close();
	}

	utils::hook::detour doexit_hook;
	void doexit(unsigned int a1, int a2, int a3)
	{
		on_exit();
		doexit_hook.invoke<void>(a1, a2, a3);
	}

	void init_zonetool()
	{
		if (utils::flags::has_flag("headless")) setvbuf(stdout, nullptr, _IONBF, 0);
		static bool initialized = false;
		if (initialized) return;
		initialized = true;

		ZONETOOL_INFO("ZoneTool is initializing...");

		// reallocs
		reallocate_asset_pool_multiplier(ASSET_TYPE_LUA_FILE, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_WEAPON, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_LOCALIZE_ENTRY, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_XANIMPARTS, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_ATTACHMENT, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_TTF, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_SOUND_GLOBALS, 4);
		reallocate_asset_pool_multiplier(ASSET_TYPE_EQUIPMENT_SND_TABLE, 4);
		reallocate_asset_pool_multiplier(ASSET_TYPE_SOUND_BANK, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_LEADERBOARD, 2);
		reallocate_asset_pool_multiplier(ASSET_TYPE_VERTEXDECL, 6);
		reallocate_asset_pool_multiplier(ASSET_TYPE_COMPUTESHADER, 4);
		reallocate_asset_pool_multiplier(ASSET_TYPE_IMPACT_FX, 2);

		// enable dumping
		db_add_xasset_hook.create(0x140A76520, &db_add_xasset_stub);

		// stop dumping
		db_finish_load_x_file_hook.create(0x1409E8B20, &db_finish_load_x_file_stub);

		// store xGfxGlobals pointers
		load_x_gfx_globals_hook.create(0x140A16710, &load_x_gfx_globals_stub);

		doexit_hook.create(0x1412D7348, doexit);
		atexit(on_exit);

		DB_FindXAssetEntry.set((std::uintptr_t)db_find_x_asset_entry);
	}

	void finalize()
	{
		ZONETOOL_INFO("ZoneTool initialization complete!");
	}

	void initialize()
	{
		init_zonetool();
	}

	void start()
	{
		initialize();
		finalize();

		branding();
		register_commands();

		handle_params();
	}
}
