#pragma once
#include "../zonetool.hpp"

namespace zonetool::iw7
{
	// PlayerAnimScript (playeranim): the player's third-person animation script (human_playeranim_cp in common_cp,
	// human_playeranim_mp in common_core_mp). Layout read from the game's own loader (Load_PlayerAnimScript at
	// 0x140A63D00 of the shipped executable, docs/PLAYERANIM.md). Dumped and parsed as JSON so a creator can add
	// animations, script items and event entries (a gesture's third-person animation is an item of an event).
	class player_anim : public asset_interface
	{
	private:
		std::string name_;
		PlayerAnimScript* asset_ = nullptr;
		std::vector<std::pair<scr_string_t*, std::string>> script_strings;

	public:
		static constexpr unsigned int script_anim_count = 121;
		static constexpr unsigned int script_event_count = 55;

		PlayerAnimScript* parse(const std::string& name, zone_memory* mem);

		void init(const std::string& name, zone_memory* mem) override;
		void prepare(zone_buffer* buf, zone_memory* mem) override;
		void load_depending(zone_base* zone) override;

		void* pointer() override { return asset_; }
		bool referenced() override { return name_.starts_with(","); }
		std::string name() override;
		std::int32_t type() override;
		void write(zone_base* zone, zone_buffer* buffer) override;

		static void dump(PlayerAnimScript* asset);
	};
}
