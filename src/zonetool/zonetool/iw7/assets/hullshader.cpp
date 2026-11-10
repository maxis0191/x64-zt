#include <std_include.hpp>
#include <utils/cryptography.hpp>
#include "hullshader.hpp"

namespace zonetool::iw7
{
	namespace
	{
		// A shader deduplicated into a zone that ZoneTool loaded at startup has no dump file.
		// Reuse the loaded program when it is real DXBC bytecode (or the engine's empty null shader).
		template <typename T>
		T* shader_from_database(XAssetType type, const std::string& name, zone_memory* mem)
		{
			if (const auto* bytes = find_captured_shader(type, name))
			{
				auto* asset = mem->allocate<T>();
				asset->name = mem->duplicate_string(name);
				asset->prog.loadDef.programSize = static_cast<unsigned int>(bytes->size());
				asset->prog.loadDef.program = mem->allocate<unsigned char>(bytes->size());
				std::memcpy(asset->prog.loadDef.program, bytes->data(), bytes->size());
				if constexpr (requires { asset->prog.loadDef.microCodeCrc; })
				{
					asset->prog.loadDef.microCodeCrc = utils::cryptography::crc32::compute(asset->prog.loadDef.program, asset->prog.loadDef.programSize);
				}
				asset->debugName = shader::parse_debug_name(asset->name, mem);
				ZONETOOL_INFO("Using captured %s \"%s\" (%zu bytes)", type_to_string(type), name.data(), bytes->size());
				return asset;
			}

			if (DB_IsXAssetDefault(type, name.data()))
			{
				return nullptr;
			}

			auto* loaded = reinterpret_cast<T*>(db_find_x_asset_header_safe(type, name).data);
			if (!loaded)
			{
				return nullptr;
			}

			const auto& def = loaded->prog.loadDef;
			const auto is_dxbc = def.program && def.programSize >= 4 && !std::memcmp(def.program, "DXBC", 4);
			if (!is_dxbc && def.programSize != 0)
			{
				return nullptr;
			}

			auto* asset = mem->allocate<T>();
			std::memcpy(asset, loaded, sizeof(T));
			asset->name = mem->duplicate_string(name);
			asset->debugName = loaded->debugName ? mem->duplicate_string(loaded->debugName) : nullptr;
			asset->prog.loadDef.program = def.programSize ? mem->allocate<unsigned char>(def.programSize) : nullptr;
			if (def.programSize)
			{
				std::memcpy(asset->prog.loadDef.program, def.program, def.programSize);
			}
			ZONETOOL_INFO("Using loaded %s \"%s\" (%u bytes)", type_to_string(type), name.data(), def.programSize);
			return asset;
		}
	}

	MaterialHullShader* hull_shader::parse(const std::string& name, zone_memory* mem)
	{
		const auto path = get_shader_path(name, hullshader);

		filesystem::file file(path);
		file.open("rb");
		if (!file.get_fp())
		{
			return nullptr;
		}

		const auto buffer_size = file.size();
		const auto buffer = file.read_bytes(buffer_size);

		auto* asset = mem->allocate<MaterialHullShader>();
		asset->name = mem->duplicate_string(name);
		asset->prog.loadDef.programSize = static_cast<unsigned int>(buffer_size);
		asset->prog.loadDef.program = mem->allocate<unsigned char>(buffer_size);

		std::memcpy(asset->prog.loadDef.program, buffer.data(), buffer_size);

		file.close();

		asset->debugName = shader::parse_debug_name(asset->name, mem);

		return asset;
	}

	void hull_shader::init(const std::string& name, zone_memory* mem)
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
			this->asset_ = shader_from_database<MaterialHullShader>(XAssetType(this->type()), name, mem);
		}

		if (!this->asset_)
		{
			ZONETOOL_FATAL("hullshader \"%s\" not found.", name.data());
		}
	}

	void hull_shader::prepare(zone_buffer* buf, zone_memory* mem)
	{
	}

	void hull_shader::load_depending(zone_base* zone)
	{
	}

	std::string hull_shader::name()
	{
		return this->name_;
	}

	std::int32_t hull_shader::type()
	{
		return ASSET_TYPE_HULLSHADER;
	}

	void hull_shader::write(zone_base* zone, zone_buffer* buf)
	{
		auto data = this->asset_;
		auto dest = buf->write(data);

		buf->push_stream(XFILE_BLOCK_VIRTUAL);
		dest->name = buf->write_str(this->name());

		if (data->debugName)
		{
			dest->debugName = buf->write_str(data->debugName);
		}

		buf->push_stream(XFILE_BLOCK_TEMP);
		if (data->prog.loadDef.program)
		{
			buf->align(3);
			buf->write(data->prog.loadDef.program, data->prog.loadDef.programSize);
			buf->clear_pointer(&dest->prog.loadDef.program);
		}
		buf->pop_stream();

		buf->pop_stream();
	}

	void hull_shader::dump(MaterialHullShader* asset)
	{
		const auto path = get_shader_path(asset->name, hullshader);

		filesystem::file file(path);
		file.open("wb");
		file.write(asset->prog.loadDef.program, asset->prog.loadDef.programSize);
		file.close();

		if (asset->debugName)
		{
			shader::dump_debug_name(asset->name, asset->debugName);
		}
	}
}