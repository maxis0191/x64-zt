#include "std_include.hpp"
#include "animclass.hpp"

// AnimationClass (animclass): AI animation state machines (zombie agents use e.g. zombie_asm_animclass).
// Binary dump through the asset manager; script strings are recorded at parse time and written in prepare().
namespace zonetool::iw7
{
	void anim_class::add_script_string(scr_string_t* ptr, const char* str)
	{
		for (const auto& entry : this->script_strings)
		{
			if (entry.first == ptr) return;
		}
		this->script_strings.emplace_back(ptr, str);
	}

	const char* anim_class::get_script_string(scr_string_t* ptr)
	{
		for (const auto& entry : this->script_strings)
		{
			if (entry.first == ptr) return entry.second;
		}
		return nullptr;
	}

	AnimationClass* anim_class::parse(const std::string& name, zone_memory* mem)
	{
		assetmanager::reader read(mem);
		const auto path = "animclass\\"s + name;
		if (!read.open(path))
		{
			return nullptr;
		}

		ZONETOOL_INFO("Parsing animclass \"%s\"...", name.data());

#define SS(ptr) this->add_script_string(ptr, read.read_string())

		auto* asset = read.read_single<AnimationClass>();
		asset->className = read.read_string();

		if (asset->stateMachine)
		{
			auto* sm = asset->stateMachine = read.read_single<AnimationStateMachine>();
			SS(&sm->name);
			if (sm->states)
			{
				sm->states = read.read_array<AnimationState>();
				for (auto i = 0; i < sm->stateCount; i++)
				{
					auto& st = sm->states[i];
					SS(&st.name);
					SS(&st.notify);
					if (st.animEntries)
					{
						st.animEntries = read.read_array<AnimationEntry>();
						for (auto e = 0; e < st.entryCount; e++) SS(&st.animEntries[e].animName);
					}
					if (st.animIndices)
					{
						st.animIndices = read.read_array<unsigned __int64>();
					}
					if (st.aliasList)
					{
						st.aliasList = read.read_array<AnimAlias>();
						for (auto a = 0; a < st.aliasCount; a++)
						{
							SS(&st.aliasList[a].aliasName);
							if (st.aliasList[a].aliasInfo) st.aliasList[a].aliasInfo = read.read_array<AnimAliasInfo>();
						}
					}
				}
			}
			if (sm->aimSets)
			{
				sm->aimSets = read.read_array<AnimationAimSet>();
				for (auto i = 0; i < sm->aimSetCount; i++)
				{
					auto& set = sm->aimSets[i];
					SS(&set.name);
					SS(&set.rootName);
					if (set.animName)
					{
						set.animName = read.read_array<scr_string_t>();
						for (auto a = 0; a < set.animCount; a++) SS(&set.animName[a]);
					}
					if (set.animIndices) set.animIndices = read.read_array<unsigned __int64>();
					if (set.aimNodeIndices) set.aimNodeIndices = read.read_array<unsigned __int64>();
				}
			}
		}

		SS(&asset->animTree);
		asset->scriptable = read.read_asset<ScriptableDef>();

#define STRINGS(list, count) if (list) { list = read.read_array<scr_string_t>(); for (auto i = 0; i < count; i++) SS(&list[i]); }
		STRINGS(asset->soundNotes, asset->soundCount);
		STRINGS(asset->soundNames, asset->soundCount);
		STRINGS(asset->soundOptions, asset->soundCount);
		STRINGS(asset->effectNotes, asset->effectCount);
		if (asset->effectDefs)
		{
			asset->effectDefs = read.read_array<FxCombinedDef>();
			for (auto i = 0; i < asset->effectCount; i++)
			{
				if (asset->effectDefs[i].u.data) asset->effectDefs[i].u.fx = read.read_asset<FxEffectDef>();
			}
		}
		STRINGS(asset->effectTags, asset->effectCount);
#undef STRINGS
#undef SS

		read.close();
		return asset;
	}

	void anim_class::init(const std::string& name, zone_memory* mem)
	{
		this->name_ = name;

		if (this->referenced())
		{
			this->asset_ = mem->allocate<AnimationClass>();
			this->asset_->className = mem->duplicate_string(name);
			return;
		}

		this->asset_ = this->parse(name, mem);
		if (!this->asset_)
		{
			ZONETOOL_FATAL("animclass %s is missing...", name.data());
		}
	}

	void anim_class::prepare(zone_buffer* buf, zone_memory* mem)
	{
		for (const auto& [ptr, str] : this->script_strings)
		{
			*ptr = static_cast<scr_string_t>(buf->write_scriptstring(str));
		}
	}

	void anim_class::load_depending(zone_base* zone)
	{
		auto* asset = this->asset_;
		if (asset->scriptable)
		{
			zone->add_asset_of_type(ASSET_TYPE_SCRIPTABLE, asset->scriptable->name);
		}
		for (auto i = 0; asset->effectDefs && i < asset->effectCount; i++)
		{
			const auto& def = asset->effectDefs[i];
			if (!def.u.data) continue;
			if (def.type == FX_COMBINED_VFX) zone->add_asset_of_type(ASSET_TYPE_VFX, def.u.vfx->name);
			else zone->add_asset_of_type(ASSET_TYPE_FX, def.u.fx->name);
		}
	}

	std::string anim_class::name()
	{
		return this->name_;
	}

	std::int32_t anim_class::type()
	{
		return ASSET_TYPE_ANIMCLASS;
	}

	void anim_class::write(zone_base* zone, zone_buffer* buf)
	{
		auto* data = this->asset_;
		auto* dest = buf->write(data);

		buf->push_stream(XFILE_BLOCK_VIRTUAL);

		dest->className = buf->write_str(this->name());

		if (data->stateMachine)
		{
			buf->align(7);
			auto* dsm = buf->write(data->stateMachine);
			const auto* sm = data->stateMachine;
			if (sm->states)
			{
				buf->align(7);
				auto* dstates = buf->write(sm->states, sm->stateCount);
				for (auto i = 0; i < sm->stateCount; i++)
				{
					const auto& st = sm->states[i];
					if (st.animEntries)
					{
						buf->align(3);
						buf->write(st.animEntries, st.entryCount);
						buf->clear_pointer(&dstates[i].animEntries);
					}
					if (st.animIndices)
					{
						buf->push_stream(XFILE_BLOCK_RUNTIME);
						buf->align(7);
						buf->write(st.animIndices, st.entryCount);
						buf->clear_pointer(&dstates[i].animIndices);
						buf->pop_stream();
					}
					if (st.aliasList)
					{
						buf->align(7);
						auto* daliases = buf->write(st.aliasList, st.aliasCount);
						for (auto a = 0; a < st.aliasCount; a++)
						{
							if (st.aliasList[a].aliasInfo)
							{
								buf->align(3);
								buf->write(st.aliasList[a].aliasInfo, st.aliasList[a].animCount);
								buf->clear_pointer(&daliases[a].aliasInfo);
							}
						}
						buf->clear_pointer(&dstates[i].aliasList);
					}
				}
				buf->clear_pointer(&dsm->states);
			}
			if (sm->aimSets)
			{
				buf->align(7);
				auto* dsets = buf->write(sm->aimSets, sm->aimSetCount);
				for (auto i = 0; i < sm->aimSetCount; i++)
				{
					const auto& set = sm->aimSets[i];
					if (set.animName)
					{
						buf->align(3);
						buf->write(set.animName, set.animCount);
						buf->clear_pointer(&dsets[i].animName);
					}
					buf->push_stream(XFILE_BLOCK_RUNTIME);
					if (set.animIndices)
					{
						buf->align(7);
						buf->write(set.animIndices, set.animCount);
						buf->clear_pointer(&dsets[i].animIndices);
					}
					if (set.aimNodeIndices)
					{
						buf->align(7);
						buf->write(set.aimNodeIndices, set.animCount);
						buf->clear_pointer(&dsets[i].aimNodeIndices);
					}
					buf->pop_stream();
				}
				buf->clear_pointer(&dsm->aimSets);
			}
			buf->clear_pointer(&dest->stateMachine);
		}

		if (data->scriptable)
		{
			dest->scriptable = reinterpret_cast<ScriptableDef*>(zone->get_asset_pointer(ASSET_TYPE_SCRIPTABLE, data->scriptable->name));
		}

		const auto strings = [&](scr_string_t* list, scr_string_t** dlist, unsigned short count)
		{
			if (!list) return;
			buf->align(3);
			buf->write(list, count);
			buf->clear_pointer(dlist);
		};
		strings(data->soundNotes, &dest->soundNotes, data->soundCount);
		strings(data->soundNames, &dest->soundNames, data->soundCount);
		strings(data->soundOptions, &dest->soundOptions, data->soundCount);
		strings(data->effectNotes, &dest->effectNotes, data->effectCount);
		if (data->effectDefs)
		{
			buf->align(7);
			auto* ddefs = buf->write(data->effectDefs, data->effectCount);
			for (auto i = 0; i < data->effectCount; i++)
			{
				const auto& def = data->effectDefs[i];
				if (!def.u.data) continue;
				if (def.type == FX_COMBINED_VFX)
					ddefs[i].u.vfx = reinterpret_cast<ParticleSystemDef*>(zone->get_asset_pointer(ASSET_TYPE_VFX, def.u.vfx->name));
				else
					ddefs[i].u.fx = reinterpret_cast<FxEffectDef*>(zone->get_asset_pointer(ASSET_TYPE_FX, def.u.fx->name));
			}
			buf->clear_pointer(&dest->effectDefs);
		}
		strings(data->effectTags, &dest->effectTags, data->effectCount);

		buf->pop_stream();
	}

	void anim_class::dump(AnimationClass* asset)
	{
		const auto path = "animclass\\"s + asset->className;

		assetmanager::dumper dumper;
		if (!dumper.open(path))
		{
			return;
		}

		const auto dump_ss = [&](scr_string_t value) { dumper.dump_string(SL_ConvertToString(value)); };

		dumper.dump_single(asset);
		dumper.dump_string(asset->className);

		if (auto* sm = asset->stateMachine)
		{
			dumper.dump_single(sm);
			dump_ss(sm->name);
			if (sm->states)
			{
				dumper.dump_array(sm->states, sm->stateCount);
				for (auto i = 0; i < sm->stateCount; i++)
				{
					const auto& st = sm->states[i];
					dump_ss(st.name);
					dump_ss(st.notify);
					if (st.animEntries)
					{
						dumper.dump_array(st.animEntries, st.entryCount);
						for (auto e = 0; e < st.entryCount; e++) dump_ss(st.animEntries[e].animName);
					}
					if (st.animIndices) dumper.dump_array(st.animIndices, st.entryCount);
					if (st.aliasList)
					{
						dumper.dump_array(st.aliasList, st.aliasCount);
						for (auto a = 0; a < st.aliasCount; a++)
						{
							dump_ss(st.aliasList[a].aliasName);
							if (st.aliasList[a].aliasInfo) dumper.dump_array(st.aliasList[a].aliasInfo, st.aliasList[a].animCount);
						}
					}
				}
			}
			if (sm->aimSets)
			{
				dumper.dump_array(sm->aimSets, sm->aimSetCount);
				for (auto i = 0; i < sm->aimSetCount; i++)
				{
					const auto& set = sm->aimSets[i];
					dump_ss(set.name);
					dump_ss(set.rootName);
					if (set.animName)
					{
						dumper.dump_array(set.animName, set.animCount);
						for (auto a = 0; a < set.animCount; a++) dump_ss(set.animName[a]);
					}
					if (set.animIndices) dumper.dump_array(set.animIndices, set.animCount);
					if (set.aimNodeIndices) dumper.dump_array(set.aimNodeIndices, set.animCount);
				}
			}
		}

		dump_ss(asset->animTree);
		dumper.dump_asset(asset->scriptable);

		const auto strings = [&](scr_string_t* list, unsigned short count)
		{
			if (!list) return;
			dumper.dump_array(list, count);
			for (auto i = 0; i < count; i++) dump_ss(list[i]);
		};
		strings(asset->soundNotes, asset->soundCount);
		strings(asset->soundNames, asset->soundCount);
		strings(asset->soundOptions, asset->soundCount);
		strings(asset->effectNotes, asset->effectCount);
		if (asset->effectDefs)
		{
			dumper.dump_array(asset->effectDefs, asset->effectCount);
			for (auto i = 0; i < asset->effectCount; i++)
			{
				if (asset->effectDefs[i].u.data) dumper.dump_asset(asset->effectDefs[i].u.fx);
			}
		}
		strings(asset->effectTags, asset->effectCount);

		dumper.close();
	}
}
