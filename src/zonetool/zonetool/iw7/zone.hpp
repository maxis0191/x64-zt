#pragma once

namespace zonetool::iw7
{
	class zone_interface : public zone_base
	{
	private:
		std::uintptr_t m_assetbase;
		std::string name_;
		std::vector<std::shared_ptr<asset_interface>> m_assets;
		// (type, name) -> position of the first asset with that type and name in m_assets: the lookups below ran a
		// linear scan with two string copies per asset, which made adding N assets cost O(N^2) (minutes on a map zone)
		std::unordered_map<std::string, std::size_t> m_asset_index;
		std::shared_ptr<zone_memory> m_zonemem;

		static std::string asset_key(std::int32_t type, const std::string& name);
		void push_asset(const std::shared_ptr<asset_interface>& asset);
		std::size_t find_asset_index(std::int32_t type, const std::string& name);

	public:
		zone_interface(std::string name);
		~zone_interface();

		asset_interface* find_asset(std::int32_t type, const std::string& name) override;

		void* get_asset_pointer(std::int32_t type, const std::string& name) override;

		void add_asset_of_type_by_pointer(std::int32_t type, void* pointer) override;

		void add_asset_of_type(std::int32_t type, const std::string& name) override;
		void add_asset_of_type(const std::string& type, const std::string& name) override;
		std::int32_t get_type_by_name(const std::string& type) override;

		void build(zone_buffer* buf) override;
	};
}
