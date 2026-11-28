#include "../src/zonetool/zonetool/iw7/iwzmap/iwzmap_world_layers.hpp"
#include <cstdio>
#include <stdexcept>

namespace wl = zonetool::iw7::iwzmap::world_layers;

static void require(bool condition, const char* message)
{
	if (!condition) throw std::runtime_error(message);
}

static void put_u32(std::vector<unsigned char>& b, std::uint32_t v) { b.insert(b.end(), reinterpret_cast<unsigned char*>(&v), reinterpret_cast<unsigned char*>(&v) + 4); }
static void put_f32(std::vector<unsigned char>& b, float v) { b.insert(b.end(), reinterpret_cast<unsigned char*>(&v), reinterpret_cast<unsigned char*>(&v) + 4); }

// a record laid out field by field the way the SDK writer does (python struct '<5I6f' then, in version 2, 'II')
static std::vector<unsigned char> record(std::uint32_t version, std::uint32_t layer_offset, std::uint32_t layer_stride, std::uint32_t vertex_count = 45)
{
	std::vector<unsigned char> b;
	for (const auto v : {3u, 24239u, vertex_count, 90u, 30u}) put_u32(b, v);
	for (const auto f : {-1.0f, -2.0f, -3.0f, 4.0f, 5.0f, 6.0f}) put_f32(b, f);
	if (version == wl::version_layers) { put_u32(b, layer_offset); put_u32(b, layer_stride); }
	return b;
}

static bool throws(const std::vector<wl::surface>& s, std::size_t layer_size)
{
	try { wl::validate(s, layer_size); }
	catch (const std::runtime_error&) { return true; }
	return false;
}

int main()
{
	try
	{
		require(wl::surface_bytes(1) == 44 && wl::surface_bytes(2) == 52, "record sizes");
		require(sizeof(wl::surface) == 52, "struct size");
		auto unsupported = false;
		try { wl::surface_bytes(3); } catch (const std::runtime_error&) { unsupported = true; }
		require(unsupported, "version 3 refused");

		// version 1: 44 byte record, no layer fields
		const auto r1 = record(1, 0, 0);
		require(r1.size() == 44, "v1 record size");
		const auto s1 = wl::decode_surface(r1.data(), 1);
		require(s1.material == 3 && s1.first_vertex == 24239 && s1.vertex_count == 45 && s1.base_index == 90 && s1.tri_count == 30, "v1 fields");
		require(s1.mins[0] == -1.0f && s1.mins[2] == -3.0f && s1.maxs[0] == 4.0f && s1.maxs[2] == 6.0f, "v1 bounds");
		require(s1.layer_offset == 0 && s1.layer_stride == 0, "v1 has no layer record");

		// version 2: the same fields, then offset and stride
		const auto r2 = record(2, 1176, 36);
		require(r2.size() == 52, "v2 record size");
		const auto s2 = wl::decode_surface(r2.data(), 2);
		require(s2.material == 3 && s2.first_vertex == 24239 && s2.vertex_count == 45 && s2.tri_count == 30 && s2.maxs[2] == 6.0f, "v2 fields");
		require(s2.layer_offset == 1176 && s2.layer_stride == 36, "v2 layer fields");

		// validation: inside the blob, 4 aligned; a surface without a layer record is never checked
		auto ok = s2;                                                            // 1176 + 45 * 36 = 2796
		require(!throws({ok}, 2796), "record ends exactly at the blob end");
		require(throws({ok}, 2795), "record past the blob end");
		auto odd = s2; odd.layer_offset = 1177;
		require(throws({odd}, 100000), "unaligned offset");
		auto none = s2; none.layer_stride = 0; none.layer_offset = 999999;
		require(!throws({none}, 0), "stride 0: offset not read");
		auto wrap = s2; wrap.layer_offset = 0xFFFFFFF0u; wrap.vertex_count = 60000;
		require(throws({wrap}, 100000), "no 32 bit wrap");

		// the zone stream: version 1 is the zeroed 16 bytes a vertex, as before; a blob larger than that wins; a smaller one is padded with zeros
		const auto z = wl::stream({}, 1000);
		require(z.size() == 16000, "v1 stream size");
		for (const auto b : z) require(b == 0, "v1 stream is zero");
		std::vector<std::uint8_t> blob(40000);
		for (std::size_t i = 0; i < blob.size(); i++) blob[i] = static_cast<std::uint8_t>(i * 7 + 1);
		const auto big = wl::stream(blob, 1000);
		require(big.size() == 40000 && big[0] == 1 && big[39999] == blob[39999], "blob larger than the zeroed stream");
		std::vector<std::uint8_t> small(100, 0xAB);
		const auto pad = wl::stream(small, 1000);
		require(pad.size() == 16000 && pad[99] == 0xAB && pad[100] == 0 && pad[15999] == 0, "blob padded with zeros");

		// a layered surface points at base + offset, any other at 0
		require(wl::vertex_layer_data(s2, 0) == 1176 && wl::vertex_layer_data(s2, 2000) == 3176, "layered surface offset");
		require(wl::vertex_layer_data(s1, 2000) == 0 && wl::vertex_layer_data(none, 2000) == 0, "plain surface offset");

		// the log line
		auto a = s2; auto b2 = s2; b2.layer_stride = 28; b2.vertex_count = 10;
		const auto sm = wl::summarize({a, b2, s1});
		require(sm.surfaces == 2 && sm.bytes == 45u * 36 + 10u * 28, "summary counts");
		require(wl::summary_line(sm) == "layer: 2 surfaces, 1900 bytes, strides {28, 36}", "summary line");
		require(wl::summary_line(wl::summarize({s1})) == "layer: 0 surfaces, 0 bytes, strides {}", "summary line without layers");
	}
	catch (const std::exception& e)
	{
		std::fputs(e.what(), stderr);
		std::fputc('\n', stderr);
		return 1;
	}
	return 0;
}
