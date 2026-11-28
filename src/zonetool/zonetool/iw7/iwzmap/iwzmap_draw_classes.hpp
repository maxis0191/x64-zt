#pragma once

// Draw classes of generated world surfaces (decal, translucent, emissive), header only so that
// tests/iwzmap_draw_classes_tests.cpp checks the rules without game assets. Reasons and measurements: iw7-creator-sdk
// docs/station-zero/redesign/R2-PARITY.md section 14.
//
// IW7 draws a static world surface in the pass of the dpvs range its index lies in (litOpaque, litDecal, litTrans, emissive). The template world
// states the class bands as material sort keys, both ends inclusive (cp_zmb: opaque 1..6, decal 7..17, trans 18..34, emissive 35..40). The map
// source may carry `draw_classes.txt` next to world.bin: one `<material>\t<sortKey>` line per drawn material whose sort key is not opaque
// ('#' starts a comment). The compiler groups the surfaces by class with one stable partition, so a world without such a file, or one whose
// listed materials are all opaque, keeps its surface order and its ranges exactly.

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace zonetool::iw7::iwzmap::draw_classes
{
	enum kind : int
	{
		opaque = 0,
		decal = 1,
		trans = 2,
		emissive = 3,
		kind_count = 4,
	};

	inline const char* kind_name(int k)
	{
		switch (k)
		{
		case opaque: return "opaque";
		case decal: return "decal";
		case trans: return "trans";
		case emissive: return "emissive";
		default: return "?";
		}
	}

	// sort key bands of the template world, both ends inclusive (index = kind)
	struct bands
	{
		unsigned int begin[kind_count]{};
		unsigned int end[kind_count]{};
	};

	// the class a sort key falls in; outside every non opaque band (and for an unset band) it is opaque
	inline int classify(unsigned int sort_key, const bands& b)
	{
		for (auto k = static_cast<int>(decal); k < kind_count; k++)
		{
			if (b.end[k] >= b.begin[k] && b.end[k] > 0 && sort_key >= b.begin[k] && sort_key <= b.end[k]) return k;
		}
		return opaque;
	}

	// draw_classes.txt -> {material: sort key}. A malformed line is refused with its number (a silently skipped line would draw the surface in the wrong pass)
	inline std::unordered_map<std::string, unsigned int> parse(const std::string& text)
	{
		std::unordered_map<std::string, unsigned int> out;
		std::istringstream in(text);
		auto line_no = 0;
		for (std::string line; std::getline(in, line);)
		{
			line_no++;
			if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
			if (line.empty()) continue;
			const auto tab = line.find('\t');
			if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
				throw std::runtime_error("draw_classes.txt line " + std::to_string(line_no) + ": expected <material>\\t<sortKey>");
			const auto name = line.substr(0, tab);
			const auto key = line.substr(tab + 1);
			if (key.find_first_not_of("0123456789") != std::string::npos || key.size() > 3)
				throw std::runtime_error("draw_classes.txt line " + std::to_string(line_no) + ": the sort key is not a number of at most 3 digits");
			const auto value = static_cast<unsigned int>(std::stoul(key));
			if (value > 255) throw std::runtime_error("draw_classes.txt line " + std::to_string(line_no) + ": the sort key is over 255");
			if (out.contains(name) && out[name] != value)
				throw std::runtime_error("draw_classes.txt line " + std::to_string(line_no) + ": " + name + " is listed with two sort keys");
			out[name] = value;
		}
		return out;
	}

	// the stable partition of surfaces by class: order[new position] = old index, count[kind]
	struct partition_result
	{
		std::vector<std::size_t> order;
		unsigned int count[kind_count]{};
		bool identity = true;
	};

	inline partition_result partition(const std::vector<int>& kinds)
	{
		partition_result r;
		for (const auto k : kinds)
		{
			if (k < 0 || k >= kind_count) throw std::runtime_error("draw class out of range");
			r.count[k]++;
		}
		for (auto k = 0; k < kind_count; k++)
		{
			for (std::size_t i = 0; i < kinds.size(); i++)
			{
				if (kinds[i] == k) r.order.push_back(i);
			}
		}
		for (std::size_t i = 0; i < r.order.size(); i++)
		{
			if (r.order[i] != i) { r.identity = false; break; }
		}
		return r;
	}

	// the dpvs ranges of a world with `leading` template surfaces (kept degenerate in the opaque range) followed by the partitioned own surfaces:
	// opaque 0..leading + opaque count, then decal, trans, emissive each right after the one before
	struct ranges
	{
		unsigned int begin[kind_count]{};
		unsigned int end[kind_count]{};
	};

	inline ranges make_ranges(unsigned int leading, const unsigned int (&count)[kind_count])
	{
		ranges r;
		r.begin[opaque] = 0;
		r.end[opaque] = leading + count[opaque];
		for (auto k = static_cast<int>(decal); k < kind_count; k++)
		{
			r.begin[k] = r.end[k - 1];
			r.end[k] = r.begin[k] + count[k];
		}
		return r;
	}

	// the class of a template surface index by the template's own dpvs ranges (opaque when in none)
	inline int template_kind(unsigned int index, const unsigned int (&begin)[kind_count], const unsigned int (&end)[kind_count])
	{
		for (auto k = static_cast<int>(decal); k < kind_count; k++)
		{
			if (index >= begin[k] && index < end[k]) return k;
		}
		return opaque;
	}
}
