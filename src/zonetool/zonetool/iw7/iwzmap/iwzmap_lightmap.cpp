#include <std_include.hpp>
#include "zonetool/iw7/zonetool.hpp"
#include "iwzmap_lightmap.hpp"

#include "zonetool/iw7/assets/gfximage.hpp"
#include "zonetool/iw7/assets/gfxlightmap.hpp"

#include <utils/io.hpp>

// Baked lightmaps for generated worlds (see iwzmap_lightmap.hpp).
//
// How the IW7 world shader reads the lightmap (lit-surface pixel shaders of the lmap_* techniques, e.g.
// ps 71a6cf7234b8e2ca of w_l_sm_replace_i0c0s0o0n0p0, lightmap coordinates = vertex TEXCOORD0.zw):
//   A = secondary(u, v * 0.5)            top half, linear RGB
//   B = secondary(u, v * 0.5 + 0.5)      bottom half, linear RGB
//   D = octahedral(secondunorm(u, v))    dominant direction in the vertex tangent frame (x tangent,
//                                        y bitangent, z normal)
//   E = A + ssao * B * saturate(dot(N, D))   diffuse irradiance, multiplied by the albedo, no scale constant
//   S = primary(u, v): baked sun visibility, smoothstep((2S - 1 + k) / 2k) with k = sun softness constant,
//       min'ed with the dynamic sun shadow; the sun itself is evaluated at runtime.

namespace zonetool::iw7::iwzmap
{
	namespace
	{
		struct dds_image
		{
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			unsigned int width = 0;
			unsigned int height = 0;
			std::string data;
		};

		std::size_t surface_bytes(DXGI_FORMAT f, unsigned int w, unsigned int h)
		{
			switch (f)
			{
			case DXGI_FORMAT_BC4_UNORM: return ((w + 3) / 4) * ((h + 3) / 4) * 8ull;
			case DXGI_FORMAT_BC6H_UF16: return ((w + 3) / 4) * ((h + 3) / 4) * 16ull;
			case DXGI_FORMAT_R8G8_UNORM: return w * h * 2ull;
			default: throw std::runtime_error("lightmap: unsupported DDS format " + std::to_string(f));
			}
		}

		// single-surface DDS with the DX10 header (as texconv -dx10 and the SDK baker write it)
		dds_image read_dds(const std::string& path)
		{
			std::string b;
			if (!utils::io::read_file(path, &b)) throw std::runtime_error("lightmap: cannot read " + path);
			if (b.size() < 148 || std::memcmp(b.data(), "DDS ", 4) || std::memcmp(b.data() + 84, "DX10", 4))
			{
				throw std::runtime_error("lightmap: " + path + " is not a DX10 DDS file");
			}
			dds_image d;
			std::memcpy(&d.height, b.data() + 12, 4);
			std::memcpy(&d.width, b.data() + 16, 4);
			std::uint32_t fmt;
			std::memcpy(&fmt, b.data() + 128, 4);
			d.format = static_cast<DXGI_FORMAT>(fmt);
			const auto size = surface_bytes(d.format, d.width, d.height);
			if (b.size() < 148 + size) throw std::runtime_error("lightmap: " + path + " is truncated");
			d.data = b.substr(148, size);
			return d;
		}

		GfxImage* make_image(const GfxImage* tpl, const dds_image& d, zone_memory* mem)
		{
			auto* img = mem->allocate<GfxImage>();
			*img = *tpl; // flags, map type, semantic, category (lightmap), picmip, card memory, name
			std::memset(&img->texture, 0, sizeof(img->texture));
			std::memset(img->streams, 0, sizeof(img->streams));
			img->imageFormat = d.format;
			img->width = static_cast<unsigned short>(d.width);
			img->height = static_cast<unsigned short>(d.height);
			img->depth = 1;
			img->numElements = 1;
			img->levelCount = 1;
			img->streamed = 0;
			img->dataLen1 = static_cast<unsigned int>(d.data.size());
			img->dataLen2 = img->dataLen1;
			img->pixelData = mem->allocate<unsigned char>(d.data.size());
			std::memcpy(img->pixelData, d.data.data(), d.data.size());
			return img;
		}
	}

	baked_lightmap prepare_lightmap(GfxWorld* w, const std::string& source, zone_memory* mem)
	{
		baked_lightmap out;
		const auto dir = source + "\\lightmap\\";
		if (!utils::io::file_exists(dir + "secondary.dds")) return out;

		const auto& r = w->draw.lightmapReindexData;
		ZONETOOL_INFO("template lightmaps: %u (override %s %s), reindex pixel size %u, %u elements, %u packed",
			w->draw.lightMapCount, w->draw.lightmapOverridePrimary ? w->draw.lightmapOverridePrimary->name : "-",
			w->draw.lightmapOverrideSecondary ? w->draw.lightmapOverrideSecondary->name : "-",
			r.imagePixelSize, r.reindexCount, r.packedLightmapCount);
		for (auto i = 0u; i < r.packedLightmapCount && r.packedLightmap; i++)
			ZONETOOL_INFO("template packed lightmap %u: %u x %u", i, r.packedLightmap[i].imageWidth, r.packedLightmap[i].imageHeight);
		for (auto i = 0u; i < r.reindexCount && r.reindexElement; i++)
			ZONETOOL_INFO("template reindex %u: index %u pos %u %u size %u %u", i, r.reindexElement[i].index,
				r.reindexElement[i].posX, r.reindexElement[i].posY, r.reindexElement[i].width, r.reindexElement[i].height);
		if (w->draw.lightMapCount < 1 || !w->draw.lightMaps || !w->draw.lightMaps[0] || !w->draw.lightMaps[0]->name)
		{
			throw std::runtime_error("lightmap: the template world has no lightmap to replace");
		}

		// the template lightmap asset names its three images (json in the template zone source)
		gfx_light_map lm_parser;
		const auto* tpl_lm = lm_parser.parse(w->draw.lightMaps[0]->name, mem);
		if (!tpl_lm) throw std::runtime_error(std::string("lightmap: template lightmap source missing for ") + w->draw.lightMaps[0]->name);

		const char* files[3] = {"primary.dds", "secondary.dds", "secondunorm.dds"};
		const DXGI_FORMAT formats[3] = {DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_R8G8_UNORM};
		dds_image dds[3];
		for (auto i = 0; i < 3; i++)
		{
			dds[i] = read_dds(dir + files[i]);
			if (dds[i].format != formats[i]) throw std::runtime_error(std::string("lightmap: ") + files[i] + " has the wrong format");
		}
		const auto width = dds[0].width, height = dds[0].height;
		if (dds[2].width != width || dds[2].height != height || dds[1].width != width || dds[1].height != height * 2 || (height & 3) || (width & 3))
		{
			throw std::runtime_error("lightmap: expected primary/secondunorm W x H and secondary W x 2H (multiples of 4)");
		}

		out.lightmap = mem->allocate<GfxLightMap>();
		out.lightmap->name = tpl_lm->name;
		for (auto i = 0; i < 3; i++)
		{
			if (!tpl_lm->textures[i]) throw std::runtime_error("lightmap: template lightmap image missing");
			gfx_image img_parser;
			const auto* tpl_img = img_parser.parse(tpl_lm->textures[i]->name, mem);
			if (!tpl_img || tpl_img->streamed) throw std::runtime_error(std::string("lightmap: template image unusable: ") + tpl_lm->textures[i]->name);
			out.images[i] = make_image(tpl_img, dds[i], mem);
			out.lightmap->textures[i] = out.images[i];
			ZONETOOL_INFO("lightmap: %s <- %s (%u x %u, format %u, %u bytes; template %u x %u format %u)", out.images[i]->name, files[i],
				out.images[i]->width, out.images[i]->height, out.images[i]->imageFormat, out.images[i]->dataLen1,
				tpl_img->width, tpl_img->height, tpl_img->imageFormat);
		}

		// lightmap reindex data: the template describes its 4096 x 4096 secondary as 2048 x 2048 tiles (packed size
		// 4096 x 4096, imagePixelSize 8). Engine-tested default (run iwz-stage2/lightmaps/run_cal): template reindex
		// rectangles kept, packed size = atlas W x H. IWZ_LM_SCALE_REINDEX scales every rectangle to the atlas instead
		// (not engine-tested yet).
		const double tpl_w = (r.packedLightmap && r.packedLightmapCount) ? r.packedLightmap[0].imageWidth : 0.0;
		if (!std::getenv("IWZ_LM_SCALE_REINDEX") && r.packedLightmap && r.packedLightmapCount)
		{
			auto* packed = mem->allocate<GfxWorldPackedLightmap>(r.packedLightmapCount);
			for (auto i = 0u; i < r.packedLightmapCount; i++) { packed[i].imageWidth = width; packed[i].imageHeight = height; }
			w->draw.lightmapReindexData.packedLightmap = packed;
		}
		else if (r.packedLightmap && r.packedLightmapCount && tpl_w > 0)
		{
			const auto sx = width / tpl_w;
			const auto sy = (height * 2.0) / r.packedLightmap[0].imageHeight;
			auto* packed = mem->allocate<GfxWorldPackedLightmap>(r.packedLightmapCount);
			for (auto i = 0u; i < r.packedLightmapCount; i++)
			{
				packed[i].imageWidth = static_cast<unsigned int>(r.packedLightmap[i].imageWidth * sx + 0.5);
				packed[i].imageHeight = static_cast<unsigned int>(r.packedLightmap[i].imageHeight * sy + 0.5);
			}
			auto* elems = mem->allocate<GfxWorldLightmapReindexDataElement>(std::max(1u, r.reindexCount));
			for (auto i = 0u; i < r.reindexCount && r.reindexElement; i++)
			{
				elems[i] = r.reindexElement[i];
				elems[i].posX = static_cast<unsigned int>(elems[i].posX * sx + 0.5);
				elems[i].width = static_cast<unsigned int>(elems[i].width * sx + 0.5);
				elems[i].posY = static_cast<unsigned int>(elems[i].posY * sy + 0.5);
				elems[i].height = static_cast<unsigned int>(elems[i].height * sy + 0.5);
			}
			w->draw.lightmapReindexData.packedLightmap = packed;
			if (r.reindexElement) w->draw.lightmapReindexData.reindexElement = elems;
			ZONETOOL_INFO("lightmap: reindex data scaled by %.4f x %.4f (packed %u x %u)", sx, sy, packed[0].imageWidth, packed[0].imageHeight);
		}
		if (std::getenv("IWZ_LM_CLEAR_REINDEX"))
		{
			w->draw.lightmapReindexData.reindexCount = 0;
			w->draw.lightmapReindexData.reindexElement = nullptr;
		}
		out.active = true;
		ZONETOOL_INFO("iwzmap: baked lightmap %u x %u installed (%s)", width, height, dir.data());
		return out;
	}

	void dump_lightmap(const baked_lightmap& lm)
	{
		if (!lm.active) return;
		for (auto* img : lm.images) gfx_image::dump(img);
		gfx_light_map::dump(lm.lightmap);
	}
}
