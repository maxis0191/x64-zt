#include "std_include.hpp"
#include "behavior_tree.hpp"

namespace zonetool::iw7
{
	// AI behavior tree: a named list of nodes; each node is a name plus 16 opaque data bytes
	// (kept verbatim as hex). Shipped in every zombies map zone; agents fail without it.
	namespace
	{
		std::string to_hex(const char* data, std::size_t size)
		{
			static const char* digits = "0123456789abcdef";
			std::string out;
			for (auto i = 0u; i < size; i++)
			{
				const auto b = static_cast<unsigned char>(data[i]);
				out.push_back(digits[b >> 4]);
				out.push_back(digits[b & 15]);
			}
			return out;
		}

		void from_hex(const std::string& hex, char* out, std::size_t size)
		{
			for (auto i = 0u; i < size && i * 2 + 1 < hex.size(); i++)
			{
				out[i] = static_cast<char>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
			}
		}
	}

	BehaviorTree* behavior_tree::parse(const std::string& name, zone_memory* mem)
	{
		const auto path = "behaviortree\\"s + name + ".json"s;

		auto file = filesystem::file(path);
		if (!file.exists())
		{
			return nullptr;
		}

		ZONETOOL_INFO("Parsing behaviortree \"%s\"...", name.data());

		file.open("rb");
		ordered_json data = json::parse(file.read_bytes(file.size()));
		file.close();

		auto asset = mem->allocate<BehaviorTree>();
		asset->name = mem->duplicate_string(name);
		asset->nodeCount = static_cast<unsigned short>(data["nodes"].size());
		asset->nodes = mem->allocate<BehaviorTreeNode>(asset->nodeCount);
		for (auto i = 0u; i < asset->nodeCount; i++)
		{
			const auto& node = data["nodes"][i];
			if (!node["name"].is_null())
			{
				asset->nodes[i].name = mem->duplicate_string(node["name"].get<std::string>());
			}
			from_hex(node["data"].get<std::string>(), asset->nodes[i].__pad0, sizeof(asset->nodes[i].__pad0));
		}

		return asset;
	}

	void behavior_tree::init(const std::string& name, zone_memory* mem)
	{
		this->name_ = name;

		if (this->referenced())
		{
			this->asset_ = mem->allocate<typename std::remove_reference<decltype(*this->asset_)>::type>();
			this->asset_->name = mem->duplicate_string(name);
			return;
		}

		this->asset_ = this->parse(name, mem);
		if (!this->asset_)
		{
			this->asset_ = db_find_x_asset_header_safe(XAssetType(this->type()), this->name().data()).behaviorTree;
		}
	}

	void behavior_tree::prepare(zone_buffer* buf, zone_memory* mem)
	{
	}

	void behavior_tree::load_depending(zone_base* zone)
	{
	}

	std::string behavior_tree::name()
	{
		return this->name_;
	}

	std::int32_t behavior_tree::type()
	{
		return ASSET_TYPE_BEHAVIOR_TREE;
	}

	void behavior_tree::write(zone_base* zone, zone_buffer* buf)
	{
		auto data = this->asset_;
		auto dest = buf->write(data);

		buf->push_stream(XFILE_BLOCK_VIRTUAL);

		dest->name = buf->write_str(this->name());

		if (data->nodes)
		{
			buf->align(7);
			auto* nodes = buf->write(data->nodes, data->nodeCount);
			for (auto i = 0u; i < data->nodeCount; i++)
			{
				if (data->nodes[i].name)
				{
					nodes[i].name = buf->write_str(data->nodes[i].name);
				}
			}
			buf->clear_pointer(&dest->nodes);
		}

		buf->pop_stream();
	}

	void behavior_tree::dump(BehaviorTree* asset)
	{
		const auto path = "behaviortree\\"s + asset->name + ".json"s;
		auto file = filesystem::file(path);
		file.open("wb");

		ordered_json data;
		data["nodes"] = ordered_json::array();
		for (auto i = 0u; i < asset->nodeCount; i++)
		{
			ordered_json node;
			node["name"] = asset->nodes[i].name ? ordered_json(asset->nodes[i].name) : ordered_json(nullptr);
			node["data"] = to_hex(asset->nodes[i].__pad0, sizeof(asset->nodes[i].__pad0));
			data["nodes"].push_back(node);
		}

		auto str = data.dump(4);
		data.clear();
		file.write(str);
		file.close();
	}
}
