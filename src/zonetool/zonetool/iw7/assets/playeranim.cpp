#include "std_include.hpp"
#include "playeranim.hpp"

// Layout (from Load_PlayerAnimScript 0x140A63D00 and its helpers 0x140A63C20, 0x140A641A0, 0x140A643B0, 0x140A642F0):
//   header 0xB0 in the current block, then in the virtual block, each array only when its pointer is set:
//   name; animations (align 4, animationCount x 0x50, three script strings each); scriptAnims (align 8, 121 x 0x20)
//   and scriptEvents (align 8, 55 x 0x20), each entry with items (align 4, itemCount x 4) and transitions (align 4,
//   transitionCount x 4); scriptItems (align 8, scriptItemCount x 0x18) with conditions (align 4, x 20) and commands
//   (align 4, x 6); scriptTransitions (align 4, x 12); scriptIdleTurns (align 4, x 4); scriptIdleTwitches (align 8,
//   x 16) with twitches (align 2, x 2); scriptAimSets (align 4, x 16); scriptLeanSets (align 4, x 4); unk (align 4,
//   x 48); torsoAnimPackMap (align 2, animationCount x 2); torsoAnimUnpackMap (torsoAnimCount x 2); legsAnimPackMap
//   (animationCount x 2); legsAnimUnpackMap (legsAnimCount x 2); xAnims (align 8, xAnimCount pointers).
namespace zonetool::iw7
{
	namespace
	{
		std::string to_hex(const void* data, size_t size)
		{
			static const char* digits = "0123456789abcdef";
			std::string out;
			out.reserve(size * 2);
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; i++)
			{
				out.push_back(digits[bytes[i] >> 4]);
				out.push_back(digits[bytes[i] & 15]);
			}
			return out;
		}

		void from_hex(const std::string& text, void* data, size_t size)
		{
			if (text.size() != size * 2)
			{
				ZONETOOL_FATAL("playeranim: hex field has %zu digits, %zu expected", text.size(), size * 2);
			}
			auto* bytes = static_cast<unsigned char*>(data);
			for (size_t i = 0; i < size; i++)
			{
				bytes[i] = static_cast<unsigned char>(std::stoul(text.substr(i * 2, 2), nullptr, 16));
			}
		}

		template <typename T>
		T* parse_array(zone_memory* mem, const ordered_json& data, unsigned int expected, const char* what)
		{
			if (data.is_null()) return nullptr;
			if (data.size() != expected)
			{
				ZONETOOL_FATAL("playeranim: %s has %zu entries, %u expected", what, data.size(), expected);
			}
			return mem->allocate<T>(expected ? expected : 1);
		}

		void dump_entry(const PlayerAnimScriptEntry& e, ordered_json& out)
		{
			out["flags"] = e.flags;
			out["items"] = ordered_json::array();
			for (auto i = 0u; e.items && i < e.itemCount; i++) out["items"].push_back(e.items[i]);
			out["transitions"] = ordered_json::array();
			for (auto i = 0u; e.transitions && i < e.transitionCount; i++) out["transitions"].push_back(e.transitions[i]);
		}

		void parse_entry(PlayerAnimScriptEntry& e, const ordered_json& in, zone_memory* mem)
		{
			e.flags = in["flags"].get<unsigned int>();
			e.itemCount = static_cast<unsigned int>(in["items"].size());
			e.transitionCount = static_cast<unsigned int>(in["transitions"].size());
			e.items = nullptr;
			e.transitions = nullptr;
			if (e.itemCount)
			{
				e.items = mem->allocate<unsigned int>(e.itemCount);
				for (auto i = 0u; i < e.itemCount; i++) e.items[i] = in["items"][i].get<unsigned int>();
			}
			if (e.transitionCount)
			{
				e.transitions = mem->allocate<unsigned int>(e.transitionCount);
				for (auto i = 0u; i < e.transitionCount; i++) e.transitions[i] = in["transitions"][i].get<unsigned int>();
			}
		}

		void write_entries(zone_buffer* buf, PlayerAnimScriptEntry* data, PlayerAnimScriptEntry** dest_field, unsigned int count)
		{
			buf->align(7);
			auto* dest = buf->write(data, count);
			for (auto i = 0u; i < count; i++)
			{
				if (data[i].items)
				{
					buf->align(3);
					buf->write(data[i].items, data[i].itemCount);
					buf->clear_pointer(&dest[i].items);
				}
				if (data[i].transitions)
				{
					buf->align(3);
					buf->write(data[i].transitions, data[i].transitionCount);
					buf->clear_pointer(&dest[i].transitions);
				}
			}
			buf->clear_pointer(dest_field);
		}
	}

	PlayerAnimScript* player_anim::parse(const std::string& name, zone_memory* mem)
	{
		const auto path = "playeranim\\"s + name + ".json"s;
		auto file = filesystem::file(path);
		if (!file.exists())
		{
			return nullptr;
		}

		ZONETOOL_INFO("Parsing playeranim \"%s\"...", name.data());

		file.open("rb");
		ordered_json data = json::parse(file.read_bytes(file.size()));
		file.close();

		auto* asset = mem->allocate<PlayerAnimScript>();
		asset->scriptName = mem->duplicate_string(name);
		asset->torsoAnimCount = data["torsoAnimCount"].get<unsigned int>();
		asset->legsAnimCount = data["legsAnimCount"].get<unsigned int>();

		const auto& anims = data["animations"];
		asset->animationCount = static_cast<unsigned int>(anims.size());
		asset->animations = mem->allocate<PlayerAnimEntry>(asset->animationCount ? asset->animationCount : 1);
		for (auto i = 0u; i < asset->animationCount; i++)
		{
			auto& e = asset->animations[i];
			from_hex(anims[i]["raw"].get<std::string>(), e.__pad0, sizeof(e.__pad0));
			this->script_strings.emplace_back(&e.entryName, anims[i]["entry"].get<std::string>());
			this->script_strings.emplace_back(&e.animName, anims[i]["anim"].get<std::string>());
			this->script_strings.emplace_back(&e.shadowAnimName, anims[i]["shadow"].get<std::string>());
		}

		if (!data["scriptAnims"].is_null())
		{
			asset->scriptAnims = parse_array<PlayerAnimScriptEntry>(mem, data["scriptAnims"], script_anim_count, "scriptAnims");
			for (auto i = 0u; i < script_anim_count; i++) parse_entry(asset->scriptAnims[i], data["scriptAnims"][i], mem);
		}
		if (!data["scriptEvents"].is_null())
		{
			asset->scriptEvents = parse_array<PlayerAnimScriptEntry>(mem, data["scriptEvents"], script_event_count, "scriptEvents");
			for (auto i = 0u; i < script_event_count; i++) parse_entry(asset->scriptEvents[i], data["scriptEvents"][i], mem);
		}

		const auto& items = data["scriptItems"];
		asset->scriptItemCount = static_cast<unsigned int>(items.size());
		if (asset->scriptItemCount)
		{
			asset->scriptItems = mem->allocate<PlayerAnimScriptItem>(asset->scriptItemCount);
			for (auto i = 0u; i < asset->scriptItemCount; i++)
			{
				auto& item = asset->scriptItems[i];
				const auto& conds = items[i]["conditions"];
				const auto& cmds = items[i]["commands"];
				item.conditionCount = static_cast<unsigned int>(conds.size());
				item.commandCount = static_cast<unsigned int>(cmds.size());
				if (item.conditionCount)
				{
					item.conditions = mem->allocate<PlayerAnimScriptCondition>(item.conditionCount);
					for (auto c = 0u; c < item.conditionCount; c++)
					{
						item.conditions[c].index = conds[c]["index"].get<unsigned int>();
						for (auto v = 0; v < 4; v++) item.conditions[c].value[v] = conds[c]["value"][v].get<unsigned int>();
					}
				}
				if (item.commandCount)
				{
					item.commands = mem->allocate<PlayerAnimScriptCommand>(item.commandCount);
					for (auto c = 0u; c < item.commandCount; c++)
					{
						item.commands[c].bodyPart = cmds[c]["bodyPart"].get<unsigned short>();
						item.commands[c].animIndex = cmds[c]["animIndex"].get<unsigned short>();
						item.commands[c].animDuration = cmds[c]["animDuration"].get<unsigned short>();
					}
				}
			}
		}

		const auto& transitions = data["scriptTransitions"];
		asset->scriptTransitionCount = static_cast<unsigned int>(transitions.size());
		if (asset->scriptTransitionCount)
		{
			asset->scriptTransitions = mem->allocate<PlayerAnimScriptTransition>(asset->scriptTransitionCount);
			for (auto i = 0u; i < asset->scriptTransitionCount; i++)
			{
				asset->scriptTransitions[i].triggerType = static_cast<PlayerAnimScriptTransitionTriggerType>(transitions[i][0].get<int>());
				asset->scriptTransitions[i].triggerParam.moveType = static_cast<PlayerAnimScriptMoveType>(transitions[i][1].get<int>());
				asset->scriptTransitions[i].targetMoveType = static_cast<PlayerAnimScriptMoveType>(transitions[i][2].get<int>());
			}
		}

		const auto& turns = data["scriptIdleTurns"];
		asset->scriptIdleTurnCount = static_cast<unsigned int>(turns.size());
		if (asset->scriptIdleTurnCount)
		{
			asset->scriptIdleTurns = mem->allocate<PlayerAnimScriptIdleTurn>(asset->scriptIdleTurnCount);
			for (auto i = 0u; i < asset->scriptIdleTurnCount; i++)
			{
				asset->scriptIdleTurns[i].turnleft = turns[i][0].get<unsigned short>();
				asset->scriptIdleTurns[i].turnright = turns[i][1].get<unsigned short>();
			}
		}

		const auto& twitches = data["scriptIdleTwitches"];
		asset->scriptIdleTwitchCount = static_cast<unsigned int>(twitches.size());
		if (asset->scriptIdleTwitchCount)
		{
			asset->scriptIdleTwitches = mem->allocate<PlayerAnimScriptIdleTwitch>(asset->scriptIdleTwitchCount);
			for (auto i = 0u; i < asset->scriptIdleTwitchCount; i++)
			{
				auto& t = asset->scriptIdleTwitches[i];
				t.cycleLength = twitches[i]["cycleLength"].get<float>();
				const auto& list = twitches[i]["twitches"];
				t.twitchCount = static_cast<unsigned int>(list.size());
				if (t.twitchCount)
				{
					t.twitches = mem->allocate<unsigned short>(t.twitchCount);
					for (auto j = 0u; j < t.twitchCount; j++) t.twitches[j] = list[j].get<unsigned short>();
				}
			}
		}

		const auto& aims = data["scriptAimSets"];
		asset->scriptAimSetCount = static_cast<unsigned int>(aims.size());
		if (asset->scriptAimSetCount)
		{
			asset->scriptAimSets = mem->allocate<PlayerAnimScriptAimSet>(asset->scriptAimSetCount);
			for (auto i = 0u; i < asset->scriptAimSetCount; i++)
			{
				auto* v = reinterpret_cast<unsigned short*>(&asset->scriptAimSets[i]);
				for (auto j = 0; j < 8; j++) v[j] = aims[i][j].get<unsigned short>();
			}
		}

		const auto& leans = data["scriptLeanSets"];
		asset->scriptLeanSetCount = static_cast<unsigned int>(leans.size());
		if (asset->scriptLeanSetCount)
		{
			asset->scriptLeanSets = mem->allocate<PlayerAnimScriptLeanSet>(asset->scriptLeanSetCount);
			for (auto i = 0u; i < asset->scriptLeanSetCount; i++)
			{
				asset->scriptLeanSets[i].lean_left = leans[i][0].get<unsigned short>();
				asset->scriptLeanSets[i].lean_right = leans[i][1].get<unsigned short>();
			}
		}

		const auto& unk = data["unk"];
		asset->unkCount = static_cast<unsigned int>(unk.size());
		if (asset->unkCount)
		{
			asset->unk = mem->allocate<unk_1453E3788>(asset->unkCount);
			for (auto i = 0u; i < asset->unkCount; i++) from_hex(unk[i].get<std::string>(), asset->unk[i].__pad0, sizeof(asset->unk[i].__pad0));
		}

		const auto map = [&](const char* key, unsigned int expected) -> unsigned short*
		{
			const auto& list = data[key];
			if (list.is_null()) return nullptr;
			if (list.size() != expected) ZONETOOL_FATAL("playeranim: %s has %zu entries, %u expected", key, list.size(), expected);
			auto* out = mem->allocate<unsigned short>(expected ? expected : 1);
			for (auto i = 0u; i < expected; i++) out[i] = list[i].get<unsigned short>();
			return out;
		};
		asset->torsoAnimPackMap = map("torsoAnimPackMap", asset->animationCount);
		asset->torsoAnimUnpackMap = map("torsoAnimUnpackMap", asset->torsoAnimCount);
		asset->legsAnimPackMap = map("legsAnimPackMap", asset->animationCount);
		asset->legsAnimUnpackMap = map("legsAnimUnpackMap", asset->legsAnimCount);

		const auto& xanims = data["xAnims"];
		asset->xAnimCount = static_cast<unsigned int>(xanims.size());
		if (asset->xAnimCount)
		{
			asset->xAnims = mem->allocate<XAnimParts*>(asset->xAnimCount);
			for (auto i = 0u; i < asset->xAnimCount; i++)
			{
				const auto anim = xanims[i].get<std::string>();
				if (anim.empty()) continue;
				asset->xAnims[i] = mem->manual_allocate<XAnimParts>(sizeof(const char*));
				asset->xAnims[i]->name = mem->duplicate_string(anim);
			}
		}

		return asset;
	}

	void player_anim::init(const std::string& name, zone_memory* mem)
	{
		this->name_ = name;

		if (this->referenced())
		{
			this->asset_ = mem->allocate<PlayerAnimScript>();
			this->asset_->scriptName = mem->duplicate_string(name);
			return;
		}

		this->asset_ = this->parse(name, mem);
		if (!this->asset_)
		{
			ZONETOOL_FATAL("playeranim %s is missing (playeranim\\%s.json)", name.data(), name.data());
		}
	}

	void player_anim::prepare(zone_buffer* buf, zone_memory* mem)
	{
		for (const auto& [ptr, str] : this->script_strings)
		{
			*ptr = static_cast<scr_string_t>(buf->write_scriptstring(str.data()));
		}
	}

	void player_anim::load_depending(zone_base* zone)
	{
		auto* asset = this->asset_;
		for (auto i = 0u; asset->xAnims && i < asset->xAnimCount; i++)
		{
			if (asset->xAnims[i])
			{
				zone->add_asset_of_type(ASSET_TYPE_XANIMPARTS, asset->xAnims[i]->name);
			}
		}
	}

	std::string player_anim::name()
	{
		return this->name_;
	}

	std::int32_t player_anim::type()
	{
		return ASSET_TYPE_PLAYERANIM;
	}

	void player_anim::write(zone_base* zone, zone_buffer* buf)
	{
		auto* data = this->asset_;
		auto* dest = buf->write(data);

		buf->push_stream(XFILE_BLOCK_VIRTUAL);

		dest->scriptName = buf->write_str(this->name());

		if (data->animations)
		{
			buf->align(3);
			buf->write(data->animations, data->animationCount);
			buf->clear_pointer(&dest->animations);
		}
		if (data->scriptAnims) write_entries(buf, data->scriptAnims, &dest->scriptAnims, script_anim_count);
		if (data->scriptEvents) write_entries(buf, data->scriptEvents, &dest->scriptEvents, script_event_count);
		if (data->scriptItems)
		{
			buf->align(7);
			auto* ditems = buf->write(data->scriptItems, data->scriptItemCount);
			for (auto i = 0u; i < data->scriptItemCount; i++)
			{
				const auto& item = data->scriptItems[i];
				if (item.conditions)
				{
					buf->align(3);
					buf->write(item.conditions, item.conditionCount);
					buf->clear_pointer(&ditems[i].conditions);
				}
				if (item.commands)
				{
					buf->align(3);
					buf->write(item.commands, item.commandCount);
					buf->clear_pointer(&ditems[i].commands);
				}
			}
			buf->clear_pointer(&dest->scriptItems);
		}
		if (data->scriptTransitions)
		{
			buf->align(3);
			buf->write(data->scriptTransitions, data->scriptTransitionCount);
			buf->clear_pointer(&dest->scriptTransitions);
		}
		if (data->scriptIdleTurns)
		{
			buf->align(3);
			buf->write(data->scriptIdleTurns, data->scriptIdleTurnCount);
			buf->clear_pointer(&dest->scriptIdleTurns);
		}
		if (data->scriptIdleTwitches)
		{
			buf->align(7);
			auto* dtw = buf->write(data->scriptIdleTwitches, data->scriptIdleTwitchCount);
			for (auto i = 0u; i < data->scriptIdleTwitchCount; i++)
			{
				if (data->scriptIdleTwitches[i].twitches)
				{
					buf->align(1);
					buf->write(data->scriptIdleTwitches[i].twitches, data->scriptIdleTwitches[i].twitchCount);
					buf->clear_pointer(&dtw[i].twitches);
				}
			}
			buf->clear_pointer(&dest->scriptIdleTwitches);
		}
		if (data->scriptAimSets)
		{
			buf->align(3);
			buf->write(data->scriptAimSets, data->scriptAimSetCount);
			buf->clear_pointer(&dest->scriptAimSets);
		}
		if (data->scriptLeanSets)
		{
			buf->align(3);
			buf->write(data->scriptLeanSets, data->scriptLeanSetCount);
			buf->clear_pointer(&dest->scriptLeanSets);
		}
		if (data->unk)
		{
			buf->align(3);
			buf->write(data->unk, data->unkCount);
			buf->clear_pointer(&dest->unk);
		}
		const auto map = [&](unsigned short* list, unsigned short** dfield, unsigned int count)
		{
			if (!list) return;
			buf->align(1);
			buf->write(list, count);
			buf->clear_pointer(dfield);
		};
		map(data->torsoAnimPackMap, &dest->torsoAnimPackMap, data->animationCount);
		map(data->torsoAnimUnpackMap, &dest->torsoAnimUnpackMap, data->torsoAnimCount);
		map(data->legsAnimPackMap, &dest->legsAnimPackMap, data->animationCount);
		map(data->legsAnimUnpackMap, &dest->legsAnimUnpackMap, data->legsAnimCount);

		if (data->xAnims)
		{
			buf->align(7);
			auto* dxanims = buf->write(data->xAnims, data->xAnimCount);
			for (auto i = 0u; i < data->xAnimCount; i++)
			{
				if (data->xAnims[i])
				{
					dxanims[i] = reinterpret_cast<XAnimParts*>(zone->get_asset_pointer(ASSET_TYPE_XANIMPARTS, data->xAnims[i]->name));
				}
			}
			buf->clear_pointer(&dest->xAnims);
		}

		buf->pop_stream();
	}

	void player_anim::dump(PlayerAnimScript* asset)
	{
		const auto path = "playeranim\\"s + asset->scriptName + ".json"s;
		auto file = filesystem::file(path);
		file.open("wb");

		const auto script_string = [](scr_string_t value) -> std::string
		{
			if (!value) return "";
			const auto* str = SL_ConvertToString(value);
			return str ? str : "";
		};

		ordered_json data;
		data["torsoAnimCount"] = asset->torsoAnimCount;
		data["legsAnimCount"] = asset->legsAnimCount;

		data["animations"] = ordered_json::array();
		for (auto i = 0u; asset->animations && i < asset->animationCount; i++)
		{
			const auto& e = asset->animations[i];
			ordered_json entry;
			entry["entry"] = script_string(e.entryName);
			entry["anim"] = script_string(e.animName);
			entry["shadow"] = script_string(e.shadowAnimName);
			entry["raw"] = to_hex(e.__pad0, sizeof(e.__pad0));
			data["animations"].push_back(entry);
		}

		data["scriptAnims"] = nullptr;
		if (asset->scriptAnims)
		{
			data["scriptAnims"] = ordered_json::array();
			for (auto i = 0u; i < script_anim_count; i++)
			{
				ordered_json e;
				dump_entry(asset->scriptAnims[i], e);
				data["scriptAnims"].push_back(e);
			}
		}
		data["scriptEvents"] = nullptr;
		if (asset->scriptEvents)
		{
			data["scriptEvents"] = ordered_json::array();
			for (auto i = 0u; i < script_event_count; i++)
			{
				ordered_json e;
				dump_entry(asset->scriptEvents[i], e);
				data["scriptEvents"].push_back(e);
			}
		}

		data["scriptItems"] = ordered_json::array();
		for (auto i = 0u; asset->scriptItems && i < asset->scriptItemCount; i++)
		{
			const auto& item = asset->scriptItems[i];
			ordered_json out;
			out["conditions"] = ordered_json::array();
			for (auto c = 0u; item.conditions && c < item.conditionCount; c++)
			{
				ordered_json cond;
				cond["index"] = item.conditions[c].index;
				cond["value"] = { item.conditions[c].value[0], item.conditions[c].value[1], item.conditions[c].value[2], item.conditions[c].value[3] };
				out["conditions"].push_back(cond);
			}
			out["commands"] = ordered_json::array();
			for (auto c = 0u; item.commands && c < item.commandCount; c++)
			{
				ordered_json cmd;
				cmd["bodyPart"] = item.commands[c].bodyPart;
				cmd["animIndex"] = item.commands[c].animIndex;
				cmd["animDuration"] = item.commands[c].animDuration;
				out["commands"].push_back(cmd);
			}
			data["scriptItems"].push_back(out);
		}

		data["scriptTransitions"] = ordered_json::array();
		for (auto i = 0u; asset->scriptTransitions && i < asset->scriptTransitionCount; i++)
		{
			const auto& t = asset->scriptTransitions[i];
			data["scriptTransitions"].push_back({ static_cast<int>(t.triggerType), static_cast<int>(t.triggerParam.moveType), static_cast<int>(t.targetMoveType) });
		}

		data["scriptIdleTurns"] = ordered_json::array();
		for (auto i = 0u; asset->scriptIdleTurns && i < asset->scriptIdleTurnCount; i++)
		{
			data["scriptIdleTurns"].push_back({ asset->scriptIdleTurns[i].turnleft, asset->scriptIdleTurns[i].turnright });
		}

		data["scriptIdleTwitches"] = ordered_json::array();
		for (auto i = 0u; asset->scriptIdleTwitches && i < asset->scriptIdleTwitchCount; i++)
		{
			const auto& t = asset->scriptIdleTwitches[i];
			ordered_json out;
			out["cycleLength"] = t.cycleLength;
			out["twitches"] = ordered_json::array();
			for (auto j = 0u; t.twitches && j < t.twitchCount; j++) out["twitches"].push_back(t.twitches[j]);
			data["scriptIdleTwitches"].push_back(out);
		}

		data["scriptAimSets"] = ordered_json::array();
		for (auto i = 0u; asset->scriptAimSets && i < asset->scriptAimSetCount; i++)
		{
			const auto* v = reinterpret_cast<const unsigned short*>(&asset->scriptAimSets[i]);
			data["scriptAimSets"].push_back({ v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7] });
		}

		data["scriptLeanSets"] = ordered_json::array();
		for (auto i = 0u; asset->scriptLeanSets && i < asset->scriptLeanSetCount; i++)
		{
			data["scriptLeanSets"].push_back({ asset->scriptLeanSets[i].lean_left, asset->scriptLeanSets[i].lean_right });
		}

		data["unk"] = ordered_json::array();
		for (auto i = 0u; asset->unk && i < asset->unkCount; i++)
		{
			data["unk"].push_back(to_hex(asset->unk[i].__pad0, sizeof(asset->unk[i].__pad0)));
		}

		const auto map = [&](const char* key, const unsigned short* list, unsigned int count)
		{
			if (!list) { data[key] = nullptr; return; }
			data[key] = ordered_json::array();
			for (auto i = 0u; i < count; i++) data[key].push_back(list[i]);
		};
		map("torsoAnimPackMap", asset->torsoAnimPackMap, asset->animationCount);
		map("torsoAnimUnpackMap", asset->torsoAnimUnpackMap, asset->torsoAnimCount);
		map("legsAnimPackMap", asset->legsAnimPackMap, asset->animationCount);
		map("legsAnimUnpackMap", asset->legsAnimUnpackMap, asset->legsAnimCount);

		data["xAnims"] = ordered_json::array();
		for (auto i = 0u; asset->xAnims && i < asset->xAnimCount; i++)
		{
			data["xAnims"].push_back(asset->xAnims[i] && asset->xAnims[i]->name ? asset->xAnims[i]->name : "");
		}

		const auto str = data.dump(1);
		file.write(str);
		file.close();
	}
}
