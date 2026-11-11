#pragma once
// Serializer extension for pinned Joelrau/x64-zt IW7 layouts (GPL-3.0 upstream).
// Asset data is read from the owner's local installation and stays outside Git.
namespace zonetool::iw7
{
    class iw7_procedural_bones final : public asset_interface
    {
        std::string name_;
        XAnimProceduralBones* asset_{};
        std::map<scr_string_t*, const char*> strings_;

        template<class T> static T* clone(const T* source, size_t count, zone_memory* memory)
        {
            if (count > 4096) throw std::runtime_error("IW7 procedural bone array count out of bounds");
            if (!count) return nullptr;
            if (!source) throw std::runtime_error("IW7 procedural bone array is missing");
            auto* result = memory->allocate<T>(count);
            std::memcpy(result, source, sizeof(T) * count);
            return result;
        }
        void strings(scr_string_t* source, size_t count, zone_memory* memory)
        {
            for (size_t i = 0; i < count; ++i)
            {
                const auto* value = source[i] ? SL_ConvertToString(source[i]) : nullptr;
                if (source[i] && !value) throw std::runtime_error("IW7 procedural bone string is missing");
                strings_[source + i] = value ? memory->duplicate_string(value) : nullptr;
            }
        }
        template<class T> static void array(zone_buffer* buffer, const T* source, T** destination, size_t count)
        {
            if (!count) { *destination = nullptr; return; }
            if (!source) throw std::runtime_error("IW7 procedural bone array disappeared before serialization");
            buffer->align(3);
            buffer->write(source, count);
            buffer->clear_pointer(destination);
        }
        // Script strings of a parsed asset: the slots of the arrays this asset owns, filled from the dump's text.
        void read_strings(assetmanager::reader& read, scr_string_t* slots, size_t count, zone_memory* memory)
        {
            for (size_t i = 0; i < count; ++i)
            {
                const char* value = read.read_string();
                strings_[slots + i] = value ? memory->duplicate_string(value) : nullptr;
            }
        }
        static void dump_strings(assetmanager::dumper& dump, const scr_string_t* slots, size_t count)
        {
            for (size_t i = 0; i < count; ++i)
                dump.dump_string(slots[i] ? SL_ConvertToString(slots[i]) : nullptr);
        }
        static void check_counts(const XAnimProceduralBones* asset)
        {
            if (asset->numConstraints > 4096 || asset->numTargetBones > 4096 || asset->unk01_count > 4096 ||
                asset->unk02_count > 4096 || asset->unk03_count > 4096)
                throw std::runtime_error("IW7 procedural bone array count out of bounds");
        }

        // proceduralbones\<name>: the asset, its name, then each array followed by the text of its script strings
        // (constraint source bones, target bones, the three strings of each unk01 entry, the one of each unk02 entry).
        XAnimProceduralBones* parse(const std::string& name, zone_memory* memory)
        {
            assetmanager::reader read(memory);
            if (!read.open("proceduralbones\\"s + name))
                return nullptr;
            ZONETOOL_INFO("Parsing proceduralbones \"%s\"...", name.data());
            auto* asset = read.read_single<XAnimProceduralBones>();
            asset->name = read.read_string();
            check_counts(asset);
            asset->constraints = read.read_array<XAnimConstraint>();
            for (size_t i = 0; i < asset->numConstraints; ++i)
                read_strings(read, asset->constraints[i].sourceBoneNames, 2, memory);
            asset->targetBoneNames = asset->numTargetBones ? memory->allocate<scr_string_t>(asset->numTargetBones) : nullptr;
            read_strings(read, asset->targetBoneNames, asset->numTargetBones, memory);
            asset->unk01 = read.read_array<unk_1453E1E68>();
            for (size_t i = 0; i < asset->unk01_count; ++i)
            {
                read_strings(read, &asset->unk01[i].unk01, 1, memory);
                read_strings(read, &asset->unk01[i].unk02, 1, memory);
                read_strings(read, &asset->unk01[i].unk03, 1, memory);
            }
            asset->unk02 = read.read_array<unk_1453E1E30>();
            for (size_t i = 0; i < asset->unk02_count; ++i)
                read_strings(read, &asset->unk02[i].unk01, 1, memory);
            asset->unk03 = read.read_array<unk_1453E1EA8>();
            read.close();
            if ((asset->numConstraints && !asset->constraints) || (asset->unk01_count && !asset->unk01) ||
                (asset->unk02_count && !asset->unk02) || (asset->unk03_count && !asset->unk03))
                throw std::runtime_error("IW7 procedural bone dump is missing an array: " + name);
            return asset;
        }
    public:
        static void dump(XAnimProceduralBones* asset)
        {
            assetmanager::dumper dump;
            if (!dump.open("proceduralbones\\"s + asset->name))
                return;
            dump.dump_single(asset);
            dump.dump_string(asset->name);
            dump.dump_array(asset->constraints, asset->numConstraints);
            for (size_t i = 0; i < asset->numConstraints; ++i)
                dump_strings(dump, asset->constraints[i].sourceBoneNames, 2);
            dump_strings(dump, asset->targetBoneNames, asset->numTargetBones);
            dump.dump_array(asset->unk01, asset->unk01_count);
            for (size_t i = 0; i < asset->unk01_count; ++i)
            {
                dump_strings(dump, &asset->unk01[i].unk01, 1);
                dump_strings(dump, &asset->unk01[i].unk02, 1);
                dump_strings(dump, &asset->unk01[i].unk03, 1);
            }
            dump.dump_array(asset->unk02, asset->unk02_count);
            for (size_t i = 0; i < asset->unk02_count; ++i)
                dump_strings(dump, &asset->unk02[i].unk01, 1);
            dump.dump_array(asset->unk03, asset->unk03_count);
            dump.close();
        }

        // Any zone: a reference, a dump on disk (proceduralbones\<name>), or the live asset of a loaded donor zone.
        void init(const std::string& name, zone_memory* memory) override
        {
            name_ = name;
            if (referenced())
            {
                asset_ = memory->allocate<XAnimProceduralBones>();
                asset_->name = memory->duplicate_string(name_);
                return;
            }
            if (!filesystem::get_fastfile().starts_with("paris_enemy_"))
            {
                asset_ = parse(name, memory);
                if (asset_)
                {
                    asset_->name = memory->duplicate_string(name_);
                    return;
                }
            }
            const auto* source = db_find_x_asset_header_safe(ASSET_TYPE_XANIM_PROCEDURALBONES, name).proceduralBones;
            if (!source || !source->name || name != source->name || DB_IsXAssetDefault(ASSET_TYPE_XANIM_PROCEDURALBONES, name.c_str()))
                throw std::runtime_error("IW7 procedural bones have no source: no proceduralbones\\" + name +
                    " dump and no loaded donor zone defines them");
            asset_ = clone(source, 1, memory);
            asset_->name = memory->duplicate_string(name_);
            asset_->constraints = clone(source->constraints, source->numConstraints, memory);
            asset_->targetBoneNames = clone(source->targetBoneNames, source->numTargetBones, memory);
            asset_->unk01 = clone(source->unk01, source->unk01_count, memory);
            asset_->unk02 = clone(source->unk02, source->unk02_count, memory);
            asset_->unk03 = clone(source->unk03, source->unk03_count, memory);
            for (size_t i = 0; i < asset_->numConstraints; ++i)
                strings(asset_->constraints[i].sourceBoneNames, 2, memory);
            strings(asset_->targetBoneNames, asset_->numTargetBones, memory);
            for (size_t i = 0; i < asset_->unk01_count; ++i)
            {
                strings(&asset_->unk01[i].unk01, 1, memory);
                strings(&asset_->unk01[i].unk02, 1, memory);
                strings(&asset_->unk01[i].unk03, 1, memory);
            }
            for (size_t i = 0; i < asset_->unk02_count; ++i)
                strings(&asset_->unk02[i].unk01, 1, memory);
            ZONETOOL_INFO("Cloned procedural bones %s: constraints=%u targets=%u arrays=%u/%u/%u", name.c_str(),
                asset_->numConstraints, asset_->numTargetBones, asset_->unk01_count, asset_->unk02_count, asset_->unk03_count);
        }
        void prepare(zone_buffer* buffer, zone_memory*) override
        {
            for (const auto& [pointer, value] : strings_)
                *pointer = static_cast<scr_string_t>(buffer->write_scriptstring(value));
        }
        void load_depending(zone_base*) override {}
        void* pointer() override { return asset_; }
        bool referenced() override { return name_.starts_with(","); }
        std::string name() override { return name_; }
        int type() override { return ASSET_TYPE_XANIM_PROCEDURALBONES; }
        void write(zone_base*, zone_buffer* buffer) override
        {
            auto* result = buffer->write(asset_);
            buffer->push_stream(XFILE_BLOCK_VIRTUAL);
            result->name = buffer->write_str(name_);
            array(buffer, asset_->constraints, &result->constraints, asset_->numConstraints);
            array(buffer, asset_->targetBoneNames, &result->targetBoneNames, asset_->numTargetBones);
            array(buffer, asset_->unk01, &result->unk01, asset_->unk01_count);
            array(buffer, asset_->unk02, &result->unk02, asset_->unk02_count);
            array(buffer, asset_->unk03, &result->unk03, asset_->unk03_count);
            buffer->pop_stream();
        }
    };

    template<class T> static void require_iw7_array(const T* source, size_t count, const char* field)
    {
        if (count && !source) throw std::runtime_error(std::string("IW7 enemy asset has a missing array: ") + field);
    }

    static unsigned char iw7_byte_count(unsigned int count, const char* field)
    {
        if (count > 255) throw std::runtime_error(std::string("IW7 enemy asset count exceeds its serialized width: ") + field);
        return static_cast<unsigned char>(count);
    }

    class iw7_anim_class final : public asset_interface
    {
        std::string name_;
        AnimationClass* asset_{};
        std::map<scr_string_t*, const char*> strings_;

        template<class T> static T* clone(T* source, size_t count, zone_memory* memory)
        {
            if (!source || !count) return nullptr;
            auto* result = memory->allocate<T>(count);
            std::memcpy(result, source, sizeof(T) * count);
            return result;
        }
        void strings(scr_string_t* source, size_t count)
        {
            if (source) for (size_t i = 0; i < count; ++i)
                strings_[source + i] = source[i] ? SL_ConvertToString(source[i]) : nullptr;
        }
        template<class T> static void array(zone_buffer* buffer, T* source, T** destination, size_t count, int alignment = 3)
        {
            if (!count)
            {
                buffer->clear_pointer(destination);
                return;
            }
            if (!source) throw std::runtime_error("IW7 enemy asset array disappeared before serialization");
            buffer->align(alignment);
            buffer->write(source, count);
            buffer->clear_pointer(destination);
        }
        static void runtime_array(zone_buffer* buffer, unsigned __int64* source, unsigned __int64** destination, size_t count)
        {
            buffer->push_stream(XFILE_BLOCK_RUNTIME);
            array(buffer, source, destination, count, 7);
            buffer->pop_stream();
        }
        static void runtime_optional_array(zone_buffer* buffer, unsigned __int64* source, unsigned __int64** destination, size_t count)
        {
            if (!source)
            {
                *destination = nullptr;
                return;
            }
            runtime_array(buffer, source, destination, count);
        }
    public:
        void init(const std::string& name, zone_memory* memory) override
        {
            name_ = name;
            asset_ = clone(db_find_x_asset_header_safe(ASSET_TYPE_ANIMCLASS, name).animClass, 1, memory);
            if (!asset_ || !asset_->stateMachine) throw std::runtime_error("IW7 animation class/state machine missing");
            asset_->stateMachine = clone(asset_->stateMachine, 1, memory);
            auto* machine = asset_->stateMachine;
            if (machine->stateCount > 2048 || machine->aimSetCount > 1024 || asset_->soundCount > 4096 || asset_->effectCount > 4096)
                throw std::runtime_error("IW7 animation class counts out of bounds");
            require_iw7_array(machine->states, machine->stateCount, "stateMachine.states");
            require_iw7_array(machine->aimSets, machine->aimSetCount, "stateMachine.aimSets");
            require_iw7_array(asset_->soundNotes, asset_->soundCount, "soundNotes");
            require_iw7_array(asset_->soundNames, asset_->soundCount, "soundNames");
            require_iw7_array(asset_->soundOptions, asset_->soundCount, "soundOptions");
            require_iw7_array(asset_->effectNotes, asset_->effectCount, "effectNotes");
            require_iw7_array(asset_->effectTags, asset_->effectCount, "effectTags");
            require_iw7_array(asset_->effectDefs, asset_->effectCount, "effectDefs");
            strings(&machine->name, 1);
            strings(&asset_->animTree, 1);
            machine->states = clone(machine->states, machine->stateCount, memory);
            for (size_t i = 0; i < machine->stateCount; ++i)
            {
                auto& state = machine->states[i];
                strings(&state.name, 1); strings(&state.notify, 1);
                const auto entries = iw7_byte_count(static_cast<unsigned char>(state.entryCount), "state.animEntries");
                const auto aliases = iw7_byte_count(static_cast<unsigned char>(state.aliasCount), "state.aliasList");
                require_iw7_array(state.animEntries, entries, "state.animEntries");
                require_iw7_array(state.animIndices, entries, "state.animIndices");
                require_iw7_array(state.aliasList, aliases, "state.aliasList");
                state.animEntries = clone(state.animEntries, entries, memory);
                for (size_t j = 0; j < entries; ++j) strings(&state.animEntries[j].animName, 1);
                state.aliasList = clone(state.aliasList, aliases, memory);
                for (size_t j = 0; j < aliases; ++j)
                {
                    strings(&state.aliasList[j].aliasName, 1);
                    const auto anim_count = iw7_byte_count(static_cast<unsigned char>(state.aliasList[j].animCount), "state.aliasList.animInfo");
                    require_iw7_array(state.aliasList[j].aliasInfo, anim_count, "state.aliasList.animInfo");
                    state.aliasList[j].aliasInfo = clone(state.aliasList[j].aliasInfo, anim_count, memory);
                }
            }
            machine->aimSets = clone(machine->aimSets, machine->aimSetCount, memory);
            for (size_t i = 0; i < machine->aimSetCount; ++i)
            {
                auto& aim = machine->aimSets[i];
                if (aim.animCount < 0 || aim.animCount > 4096) throw std::runtime_error("IW7 aim animation count out of bounds");
                if (aim.animCount && (!aim.animName || !aim.animIndices))
                {
                    ZONETOOL_WARNING("IW7 enemy animclass %s aimSet[%zu] is missing required arrays: count=%d animName=%p animIndices=%p optionalAimNodeIndices=%p",
                        name.c_str(), i, aim.animCount, static_cast<const void*>(aim.animName),
                        static_cast<const void*>(aim.animIndices), static_cast<const void*>(aim.aimNodeIndices));
                }
                require_iw7_array(aim.animName, static_cast<size_t>(aim.animCount), "aimSet.animName");
                require_iw7_array(aim.animIndices, static_cast<size_t>(aim.animCount), "aimSet.animIndices");
                if (aim.animCount && !aim.aimNodeIndices)
                    ZONETOOL_INFO("IW7 enemy animclass %s aimSet[%zu] preserves nullable aimNodeIndices (count=%d)", name.c_str(), i, aim.animCount);
                strings(&aim.name, 1); strings(&aim.rootName, 1);
                aim.animName = clone(aim.animName, aim.animCount, memory);
                strings(aim.animName, aim.animCount);
            }
#define IW7_STRINGS(field, count) asset_->field = clone(asset_->field, asset_->count, memory); strings(asset_->field, asset_->count)
            IW7_STRINGS(soundNotes, soundCount); IW7_STRINGS(soundNames, soundCount); IW7_STRINGS(soundOptions, soundCount);
            IW7_STRINGS(effectNotes, effectCount); IW7_STRINGS(effectTags, effectCount);
#undef IW7_STRINGS
            asset_->effectDefs = clone(asset_->effectDefs, asset_->effectCount, memory);
        }
        void prepare(zone_buffer* buffer, zone_memory*) override
        {
            for (const auto& [pointer, value] : strings_) *pointer = static_cast<scr_string_t>(buffer->write_scriptstring(value));
        }
        void load_depending(zone_base* zone) override
        {
            const auto* tree = strings_.at(&asset_->animTree);
            if (!tree || !*tree) throw std::runtime_error("IW7 animation tree name missing");
            auto tree_file = std::string(tree);
            if (!tree_file.starts_with("animtrees/")) tree_file = "animtrees/" + tree_file;
            if (!tree_file.ends_with(".atr")) tree_file += ".atr";
            if (DB_IsXAssetDefault(ASSET_TYPE_RAWFILE, tree_file.data())) throw std::runtime_error("IW7 animation tree rawfile missing");
            rawfile::dump(db_find_x_asset_header_safe(ASSET_TYPE_RAWFILE, tree_file).rawfile);
            zone->add_asset_of_type(ASSET_TYPE_RAWFILE, tree_file);
            auto* machine = asset_->stateMachine;
            for (size_t i = 0; i < machine->stateCount; ++i)
                for (size_t j = 0; j < static_cast<unsigned char>(machine->states[i].entryCount); ++j)
                {
                    auto* name = strings_.at(&machine->states[i].animEntries[j].animName);
                    if (name && *name) zone->add_asset_of_type(ASSET_TYPE_XANIMPARTS, name);
                }
            for (size_t i = 0; i < machine->aimSetCount; ++i)
                for (int j = 0; j < machine->aimSets[i].animCount; ++j)
                {
                    auto* name = strings_.at(machine->aimSets[i].animName + j);
                    if (name && *name) zone->add_asset_of_type(ASSET_TYPE_XANIMPARTS, name);
                }
            if (asset_->scriptable)
            {
                if (!asset_->scriptable->name) throw std::runtime_error("IW7 animation class has an unnamed scriptable dependency");
                zone->add_asset_of_type(ASSET_TYPE_SCRIPTABLE, asset_->scriptable->name);
            }
            for (size_t i = 0; asset_->effectDefs && i < asset_->effectCount; ++i)
            {
                auto& effect = asset_->effectDefs[i];
                if (!effect.u.data) continue;
                const auto* effect_name = effect.type == FX_COMBINED_VFX
                    ? (effect.u.vfx ? effect.u.vfx->name : nullptr)
                    : (effect.u.fx ? effect.u.fx->name : nullptr);
                if (!effect_name || !*effect_name) throw std::runtime_error("IW7 animation class has an invalid effect dependency");
                zone->add_asset_of_type(effect.type == FX_COMBINED_VFX ? ASSET_TYPE_VFX : ASSET_TYPE_FX, effect_name);
            }
        }
        void* pointer() override { return asset_; }
        std::string name() override { return name_; }
        int type() override { return ASSET_TYPE_ANIMCLASS; }
        void write(zone_base* zone, zone_buffer* buffer) override
        {
            auto* result = buffer->write(asset_);
            buffer->push_stream(XFILE_BLOCK_VIRTUAL);
            result->className = buffer->write_str(name_);
            auto* machine = asset_->stateMachine;
            buffer->align(7);
            auto* machine_result = buffer->write(machine);
            if (machine->stateCount)
            {
                require_iw7_array(machine->states, machine->stateCount, "stateMachine.states");
                buffer->align(7);
                auto* states = buffer->write(machine->states, machine->stateCount);
                for (size_t i = 0; i < machine->stateCount; ++i)
                {
                    auto& state = machine->states[i];
                    const auto entries = iw7_byte_count(static_cast<unsigned char>(state.entryCount), "state.animEntries");
                    const auto aliases_count = iw7_byte_count(static_cast<unsigned char>(state.aliasCount), "state.aliasList");
                    array(buffer, state.animEntries, &states[i].animEntries, entries);
                    runtime_array(buffer, state.animIndices, &states[i].animIndices, entries);
                    if (aliases_count)
                    {
                        require_iw7_array(state.aliasList, aliases_count, "state.aliasList");
                        buffer->align(7);
                        auto* aliases = buffer->write(state.aliasList, aliases_count);
                        for (size_t j = 0; j < aliases_count; ++j)
                            array(buffer, state.aliasList[j].aliasInfo, &aliases[j].aliasInfo,
                                iw7_byte_count(static_cast<unsigned char>(state.aliasList[j].animCount), "state.aliasList.animInfo"));
                        buffer->clear_pointer(&states[i].aliasList);
                    }
                    else
                    {
                        buffer->clear_pointer(&states[i].aliasList);
                    }
                }
                buffer->clear_pointer(&machine_result->states);
            }
            if (machine->aimSetCount)
            {
                require_iw7_array(machine->aimSets, machine->aimSetCount, "stateMachine.aimSets");
                buffer->align(7);
                auto* aims = buffer->write(machine->aimSets, machine->aimSetCount);
                for (size_t i = 0; i < machine->aimSetCount; ++i)
                {
                    auto& aim = machine->aimSets[i];
                    array(buffer, aim.animName, &aims[i].animName, aim.animCount);
                    runtime_array(buffer, aim.animIndices, &aims[i].animIndices, aim.animCount);
                    runtime_optional_array(buffer, aim.aimNodeIndices, &aims[i].aimNodeIndices, aim.animCount);
                }
                buffer->clear_pointer(&machine_result->aimSets);
            }
            else
            {
                buffer->clear_pointer(&machine_result->aimSets);
            }
            buffer->clear_pointer(&result->stateMachine);
            if (asset_->scriptable) result->scriptable = reinterpret_cast<ScriptableDef*>(zone->get_asset_pointer(ASSET_TYPE_SCRIPTABLE, asset_->scriptable->name));
            array(buffer, asset_->soundNotes, &result->soundNotes, asset_->soundCount);
            array(buffer, asset_->soundNames, &result->soundNames, asset_->soundCount);
            array(buffer, asset_->soundOptions, &result->soundOptions, asset_->soundCount);
            array(buffer, asset_->effectNotes, &result->effectNotes, asset_->effectCount);
            if (asset_->effectDefs)
            {
                buffer->align(7);
                auto* effects = buffer->write(asset_->effectDefs, asset_->effectCount);
                for (size_t i = 0; i < asset_->effectCount; ++i)
                {
                    auto& source = asset_->effectDefs[i];
                    if (source.u.data) effects[i].u.data = zone->get_asset_pointer(source.type == FX_COMBINED_VFX ? ASSET_TYPE_VFX : ASSET_TYPE_FX,
                        source.type == FX_COMBINED_VFX ? source.u.vfx->name : source.u.fx->name);
                }
                buffer->clear_pointer(&result->effectDefs);
            }
            array(buffer, asset_->effectTags, &result->effectTags, asset_->effectCount);
            buffer->pop_stream();
        }
    };

    class iw7_behavior_tree final : public asset_interface
    {
        std::string name_;
        BehaviorTree* asset_{};
    public:
        void init(const std::string& name, zone_memory*) override
        {
            name_ = name;
            asset_ = db_find_x_asset_header_safe(ASSET_TYPE_BEHAVIOR_TREE, name).behaviorTree;
            if (!asset_ || !asset_->name || asset_->nodeCount > 8192)
                throw std::runtime_error("IW7 behavior tree unavailable or invalid");
        }
        void* pointer() override { return asset_; }
        std::string name() override { return name_; }
        int type() override { return ASSET_TYPE_BEHAVIOR_TREE; }
        void write(zone_base*, zone_buffer* buffer) override
        {
            auto* result = buffer->write(asset_);
            buffer->push_stream(XFILE_BLOCK_VIRTUAL);
            result->name = buffer->write_str(name_);
            if (asset_->nodeCount)
            {
                require_iw7_array(asset_->nodes, asset_->nodeCount, "behaviorTree.nodes");
                buffer->align(7);
                auto* nodes = buffer->write(asset_->nodes, asset_->nodeCount);
                for (size_t i = 0; i < asset_->nodeCount; ++i)
                    nodes[i].name = asset_->nodes[i].name ? buffer->write_str(asset_->nodes[i].name) : nullptr;
                buffer->clear_pointer(&result->nodes);
            }
            else
            {
                buffer->clear_pointer(&result->nodes);
            }
            buffer->pop_stream();
        }
    };
}
