#include <std_include.hpp>
#include "zonetool.hpp"
#include "zonetool_enemy.hpp"
#include "zone.hpp"
#include "zonetool/utils/utils.hpp"

#include <utils/io.hpp>
#include <utils/cryptography.hpp>

#include <zlib.h>
#include <lz4.h>
#include <lz4hc.h>
#include <chrono>
#include <map>
#include <atomic>
#include <thread>
#include <functional>
#include <unordered_set>
#include <utils/hook.hpp>
#include <cwctype>

#define FF_MAGIC_SIGNED "IWff0100"
#define FF_MAGIC_UNSIGNED "IWffu100"

//#define FF_SIGNED
#ifdef FF_SIGNED
#define FF_MAGIC FF_MAGIC_SIGNED
#else
#define FF_MAGIC FF_MAGIC_UNSIGNED
#endif

#define FF_VERSION 1619

#define CUSTOM_IMAGEFILE_INDEX 431

#define COMPRESS_BLOCK_TYPE_ZLIB 1
#define COMPRESS_BLOCK_TYPE_LZ4 4
#define COMPRESS_BLOCK_TYPE COMPRESS_BLOCK_TYPE_LZ4

#define COMPRESSOR_BLOCK 2
#define COMPRESSOR_PASSTHROUGH 1
#define COMPRESSOR COMPRESSOR_BLOCK

#ifdef FF_SIGNED
static_assert(COMPRESS_BLOCK_TYPE == COMPRESS_BLOCK_TYPE_LZ4, "LZ4 is the only compression type supported with signed fastfiles currently");
static_assert(COMPRESSOR == COMPRESSOR_BLOCK, "Block compressor is the only supported type with signed fastfiles currently");
#else
static_assert(COMPRESSOR == COMPRESSOR_BLOCK || COMPRESSOR == COMPRESSOR_PASSTHROUGH, "Unknown compressor type");
static_assert(COMPRESS_BLOCK_TYPE == COMPRESS_BLOCK_TYPE_LZ4 || COMPRESSOR == COMPRESS_BLOCK_TYPE_ZLIB, "Unsupported block compression type");
#endif

namespace zonetool::iw7
{
	namespace
	{
		// wall clock of the zone step's parts (the lines "[ TIMING ]" of the log; the table of docs/BUILD-SERVER.md)
		struct phase_clock
		{
			std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
			double lap() { const auto n = std::chrono::steady_clock::now(); const auto d = std::chrono::duration<double>(n - t).count(); t = n; return d; }
		};
	}


	// ------------------------------------------------------------------------------------------------------------
	// The parsed-asset cache of the build server (-serve). A zone build parses every asset of its csv from files
	// (about 18 s for a map's 135,000 stock assets) and the result is the same from build to build. In the server the
	// parsed asset objects live on between builds, keyed by (type, name):
	//  * an entry holds the asset, the ordered (type, name) list of the dependencies its parse added to the zone and
	//    the files its parse opened or probed (hooked at CreateFileW / GetFileAttributes*W / FindFirstFileExW), each
	//    with the size and time seen;
	//  * a hit replays the dependency additions through add_asset_of_type (so the zone's asset order, which pointers
	//    encode, is the one a fresh parse gives) and pushes the cached asset; no file is read;
	//  * an entry is valid when none of its files changed (all distinct files are stat'ed by 16 threads at the start of
	//    a build) and every dependency's entry is valid: a changed source file re-parses that asset and, through the
	//    dependency rule, the assets that were parsed with it;
	//  * the objects are allocated in an arena of their own that outlives the per-build zone; when it is 85 % used the
	//    cache is flushed.
	// Not cached: assets whose parse adds a localize entry by pointer, builds of the paris_enemy_ packs.
	namespace asset_cache
	{
		struct stamp
		{
			bool exists = false;
			unsigned long long size = 0;
			unsigned long long time = 0;
			bool operator==(const stamp& o) const { return exists == o.exists && size == o.size && time == o.time; }
		};

		struct entry
		{
			std::shared_ptr<asset_interface> asset;
			std::vector<std::pair<std::int32_t, std::string>> deps;
			std::vector<std::pair<int, stamp>> files; // (path id, stamp seen)
		};

		struct frame
		{
			std::vector<std::pair<std::int32_t, std::string>> deps;
			std::unordered_set<std::wstring> files;
			bool unsafe = false;
			frame* parent = nullptr;
		};

		bool g_enabled = false;
		thread_local frame* t_frame = nullptr;
		thread_local bool t_quiet = false;
		std::unordered_map<std::string, entry> g_table;
		std::vector<std::wstring> g_paths;
		std::unordered_map<std::wstring, int> g_path_id;
		std::vector<stamp> g_current;                      // current stamp of every path, computed at the start of a build
		std::unordered_map<std::string, signed char> g_ok; // per build: 1 valid, 0 invalid, -1 being checked
		std::shared_ptr<zone_memory> g_arena;
		std::size_t g_hits = 0, g_misses = 0, g_invalid = 0, g_uncacheable = 0;
		bool g_hooked = false;
		utils::hook::detour h_create, h_attr, h_attr_ex, h_find;

		void record(const wchar_t* path)
		{
			if (t_frame && !t_quiet && path)
			{
				std::wstring p(path);
				for (auto& c : p) { c = static_cast<wchar_t>(towlower(c)); if (c == L'/') c = L'\\'; }
				t_frame->files.insert(std::move(p));
			}
		}

		HANDLE WINAPI create_file_stub(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD d, DWORD f, HANDLE t)
		{
			record(n);
			return h_create.invoke<HANDLE>(n, a, s, sa, d, f, t);
		}

		DWORD WINAPI get_attr_stub(LPCWSTR n)
		{
			record(n);
			return h_attr.invoke<DWORD>(n);
		}

		BOOL WINAPI get_attr_ex_stub(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID d)
		{
			record(n);
			return h_attr_ex.invoke<BOOL>(n, l, d);
		}

		HANDLE WINAPI find_first_stub(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD fl)
		{
			record(n);
			return h_find.invoke<HANDLE>(n, l, d, o, f, fl);
		}

		void install_hooks()
		{
			if (g_hooked) return;
			g_hooked = true;
			const auto kb = GetModuleHandleA("kernelbase.dll");
			const auto k32 = GetModuleHandleA("kernel32.dll");
			const auto get = [&](const char* name) -> void*
			{
				auto p = kb ? GetProcAddress(kb, name) : nullptr;
				if (!p && k32) p = GetProcAddress(k32, name);
				return reinterpret_cast<void*>(p);
			};
			h_create.create(get("CreateFileW"), create_file_stub);
			h_attr.create(get("GetFileAttributesW"), get_attr_stub);
			h_attr_ex.create(get("GetFileAttributesExW"), get_attr_ex_stub);
			h_find.create(get("FindFirstFileExW"), find_first_stub);
		}

		stamp stat_of(const std::wstring& path)
		{
			t_quiet = true;
			stamp s;
			WIN32_FILE_ATTRIBUTE_DATA d{};
			if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d))
			{
				s.exists = true;
				s.size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
				s.time = (static_cast<unsigned long long>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
			}
			t_quiet = false;
			return s;
		}

		bool active() { return g_enabled; }

		void set_enabled(bool on)
		{
			g_enabled = on;
			if (on) install_hooks();
		}

		void flush()
		{
			g_table.clear();
			g_paths.clear();
			g_path_id.clear();
			g_current.clear();
			g_ok.clear();
			g_arena.reset();
			g_hits = g_misses = g_invalid = g_uncacheable = 0;
			material::fixed_nml_images_map.clear();
		}

		// zone_memory keeps its position private and the header is included by every asset file: read it through the
		// explicit-instantiation access idiom instead of changing the header (which would rebuild the whole project)
		template <class Tag, typename Tag::type M>
		struct member_ref { friend typename Tag::type member_of(Tag) { return M; } };
		struct mem_pos_tag { using type = std::size_t zone_memory::*; friend type member_of(mem_pos_tag); };
		template struct member_ref<mem_pos_tag, &zone_memory::mem_pos_>;
		std::size_t arena_used() { return g_arena ? (g_arena.get()->*member_of(mem_pos_tag{})) : 0; }

		struct mem_pool_tag { using type = LPVOID zone_memory::*; friend type member_of(mem_pool_tag); };
		template struct member_ref<mem_pool_tag, &zone_memory::memory_pool_>;
		// is the address inside the cache's own arena (and not in the memory of a loaded game zone, which a zone unload frees)?
		bool in_arena(const void* p)
		{
			if (!g_arena || !p) return false;
			const auto base = reinterpret_cast<std::uintptr_t>(g_arena.get()->*member_of(mem_pool_tag{}));
			const auto at = reinterpret_cast<std::uintptr_t>(p);
			return at >= base && at < base + MAX_ZONE_SIZE;
		}

		std::string stats()
		{
			return utils::string::va("%zu entries, %zu files, arena %.0f MB, hits %zu, misses %zu (of which stale %zu), not cacheable %zu", g_table.size(),
				g_paths.size(), arena_used() / 1048576.0, g_hits, g_misses, g_invalid, g_uncacheable);
		}

		zone_memory* arena()
		{
			if (!g_arena) g_arena = std::make_shared<zone_memory>(MAX_ZONE_SIZE);
			return g_arena.get();
		}

		// the start of a build: the current stamp of every file any entry has seen
		void begin_build()
		{
			g_hits = g_misses = g_invalid = g_uncacheable = 0;
			g_ok.clear();
			if (g_arena && arena_used() > MAX_ZONE_SIZE / 100 * 85)
			{
				ZONETOOL_INFO("asset cache: arena %.0f of %.0f MB used, flushing the cache", arena_used() / 1048576.0, MAX_ZONE_SIZE / 1048576.0);
				flush();
			}
			const auto n = g_paths.size();
			g_current.assign(n, stamp{});
			const auto threads = std::min<std::size_t>(16, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
			std::atomic<std::size_t> next{0};
			std::vector<std::thread> pool;
			for (std::size_t t = 0; t < threads; t++)
			{
				pool.emplace_back([&]
				{
					for (std::size_t i; (i = next.fetch_add(256)) < n;)
					{
						for (std::size_t j = i; j < std::min(n, i + 256); j++) g_current[j] = stat_of(g_paths[j]);
					}
				});
			}
			for (auto& t : pool) t.join();
		}

		int path_id(const std::wstring& p)
		{
			const auto it = g_path_id.find(p);
			if (it != g_path_id.end()) return it->second;
			const auto id = static_cast<int>(g_paths.size());
			g_paths.push_back(p);
			g_path_id.emplace(p, id);
			return id;
		}

		bool files_unchanged(const entry& e)
		{
			for (const auto& [id, seen] : e.files)
			{
				if (!(g_current[id] == seen)) return false;
			}
			return true;
		}

		// an entry is usable when its own files are unchanged and every dependency entry is usable
		bool usable(const std::string& key, const std::function<std::string(const std::pair<std::int32_t, std::string>&)>& dep_key)
		{
			const auto ok = g_ok.find(key);
			if (ok != g_ok.end()) return ok->second != 0;
			const auto it = g_table.find(key);
			if (it == g_table.end()) return false;
			g_ok[key] = -1;
			bool good = files_unchanged(it->second);
			for (const auto& d : it->second.deps)
			{
				if (!good) break;
				const auto dk = dep_key(d);
				if (dk.empty() || !g_table.contains(dk)) continue; // nothing was cached for it (a type that adds nothing)
				good = usable(dk, dep_key);
			}
			g_ok[key] = good ? 1 : 0;
			return good;
		}

		struct guard
		{
			frame f;
			bool on;
			explicit guard(bool enable) : on(enable)
			{
				if (on) { f.parent = t_frame; t_frame = &f; }
			}
			~guard()
			{
				if (on) t_frame = f.parent;
			}
		};
	}

	namespace compression::iwc
	{
		size_t align_value(size_t value, unsigned int alignment)
		{
			return (value + alignment - 1) & ~(static_cast<size_t>(alignment) - 1);
		}

		const char* align_value(const char* value, unsigned int alignment)
		{
			return reinterpret_cast<const char*>(align_value(reinterpret_cast<size_t>(value), alignment));
		}

		constexpr auto MAX_BLOCK_SIZE = 0x10000ull;
		constexpr auto BLOCK_SIZE_CHUNK_SIGNED = 0x4000ull;
		constexpr auto BLOCK_SIZE_SIGNED = BLOCK_SIZE_CHUNK_SIGNED - sizeof(XBlockCompressionBlockHeader);
		constexpr auto BLOCK_SIZE_FIRST_SIGNED = BLOCK_SIZE_SIGNED - sizeof(XBlockCompressionDataHeader) - sizeof(XFileCompressorHeader);

		std::vector<std::uint8_t> compress_block_signed(const std::uint8_t* data, const std::size_t size, const int type, std::vector<DB_AuthHash>& chunk_hashes)
		{
			auto* block_buffer = reinterpret_cast<std::uint8_t*>(malloc(BLOCK_SIZE_CHUNK_SIGNED));

			std::vector<std::uint8_t> out_buffer;

			auto bytes_to_compress = size;
			if (bytes_to_compress > std::numeric_limits<unsigned int>::max())
			{
				throw std::runtime_error("cannot compress more than `std::numeric_limits<unsigned int>::max()` bytes");
			}

			auto data_ptr = reinterpret_cast<const char*>(data);

			const auto write = [&](void* data, const size_t len)
			{
				for (auto i = 0ull; i < len; i++)
				{
					out_buffer.push_back(reinterpret_cast<char*>(data)[i]);
				}
			};

			XFileCompressorHeader compress_header{};
			memcpy(compress_header.magic, "IWC", 3);
			compress_header.compressor = DB_COMPRESSOR_BLOCK;
			write(&compress_header, sizeof(XFileCompressorHeader));

			auto first_block = true;

			const auto compress_block_lz4 = [&]()
			{
				memset(block_buffer, 0, sizeof(block_buffer));

				const auto uncompressed_size = static_cast<unsigned int>(std::min(bytes_to_compress, MAX_BLOCK_SIZE));
				memcpy(block_buffer, data_ptr, uncompressed_size);

				const auto block_size = first_block ? BLOCK_SIZE_FIRST_SIGNED : BLOCK_SIZE_SIGNED;
				const auto bound = LZ4_compressBound(static_cast<int>(block_size));
				assert(bound > block_size);

				std::string buffer;
				buffer.resize(block_size);

				const auto compressed_size = LZ4_compress_HC(reinterpret_cast<char*>(block_buffer),
					buffer.data(), uncompressed_size, bound, LZ4HC_CLEVEL_DEFAULT);
				assert(buffer.size() == block_size);

				if (first_block)
				{
					XBlockCompressionDataHeader header{};
					header.uncompressedSize = bytes_to_compress;
					header.blockSizeAndType.blockSize = MAX_BLOCK_SIZE;
					header.blockSizeAndType.compressionType = XBLOCK_COMPRESSION_LZ4;

					write(&header, sizeof(header));
				}

				XBlockCompressionBlockHeader block_header{};
				block_header.compressedSize = compressed_size;
				block_header.uncompressedSize = uncompressed_size;

				write(&block_header, sizeof(block_header));

				write(buffer.data(), buffer.size());

				first_block = false;

				bytes_to_compress -= uncompressed_size;
				data_ptr += uncompressed_size;

				assert(buffer.size() % BLOCK_SIZE_CHUNK_SIGNED);
			};

			const auto compress_block_zlib = [&]()
			{
				__debugbreak();
			};

			while (bytes_to_compress > 0)
			{
				if (type == XBLOCK_COMPRESSION_LZ4)
				{
					compress_block_lz4();
				}
				else
				{
					compress_block_zlib();
				}

				// calc hash
				DB_AuthHash hash{};
				hash_state state{};
				sha256_init(&state);
				sha256_process(&state, out_buffer.data() + out_buffer.size() - BLOCK_SIZE_CHUNK_SIGNED, BLOCK_SIZE_CHUNK_SIGNED);
				sha256_done(&state, hash.bytes);
				chunk_hashes.push_back(hash);
			}

			free(block_buffer);

			return out_buffer;
		}

		std::vector<std::uint8_t> compress_block(const std::uint8_t* data, const std::size_t size, const int type)
		{
			std::vector<std::uint8_t> out_buffer;

			auto bytes_to_compress = size;
			if (bytes_to_compress > std::numeric_limits<unsigned int>::max())
			{
				throw std::runtime_error("cannot compress more than `std::numeric_limits<unsigned int>::max()` bytes");
			}

			auto data_ptr = reinterpret_cast<const char*>(data);

			const auto write = [&](void* data, const size_t len)
			{
				for (auto i = 0ull; i < len; i++)
				{
					out_buffer.push_back(reinterpret_cast<char*>(data)[i]);
				}
			};

			XFileCompressorHeader compress_header{};
			memcpy(compress_header.magic, "IWC", 3);
			compress_header.compressor = DB_COMPRESSOR_BLOCK;
			write(&compress_header, sizeof(XFileCompressorHeader));

			auto first_block = true;

			const auto compress_block_lz4 = [&]()
			{
				const auto block_size = static_cast<unsigned int>(std::min(bytes_to_compress, MAX_BLOCK_SIZE));
				const auto bound = LZ4_compressBound(block_size);

				std::string buffer;
				buffer.resize(bound);

				const auto compressed_size = LZ4_compress_default(data_ptr,
					buffer.data(), block_size, bound);
				buffer.resize(align_value(compressed_size, 4));

				if (first_block)
				{
					XBlockCompressionDataHeader header{};
					header.uncompressedSize = bytes_to_compress;
					header.blockSizeAndType.blockSize = MAX_BLOCK_SIZE;
					header.blockSizeAndType.compressionType = XBLOCK_COMPRESSION_LZ4;

					write(&header, sizeof(header));
				}

				XBlockCompressionBlockHeader block_header{};
				block_header.compressedSize = compressed_size;
				block_header.uncompressedSize = block_size;

				write(&block_header, sizeof(block_header));

				write(buffer.data(), buffer.size());

				first_block = false;

				bytes_to_compress -= block_size;
				data_ptr += block_size;
			};

			const auto compress_block_lz4hc = [&]()
			{
				const auto block_size = static_cast<unsigned int>(std::min(bytes_to_compress, MAX_BLOCK_SIZE));
				const auto bound = LZ4_compressBound(block_size);

				std::string buffer;
				buffer.resize(bound);

				const auto compressed_size = LZ4_compress_HC(data_ptr,
					buffer.data(), block_size, bound, LZ4HC_CLEVEL_DEFAULT);
				buffer.resize(align_value(compressed_size, 4));

				if (first_block)
				{
					XBlockCompressionDataHeader header{};
					header.uncompressedSize = bytes_to_compress;
					header.blockSizeAndType.blockSize = MAX_BLOCK_SIZE;
					header.blockSizeAndType.compressionType = XBLOCK_COMPRESSION_LZ4HC;

					write(&header, sizeof(header));
				}

				XBlockCompressionBlockHeader block_header{};
				block_header.compressedSize = compressed_size;
				block_header.uncompressedSize = block_size;

				write(&block_header, sizeof(block_header));

				write(buffer.data(), buffer.size());

				first_block = false;

				bytes_to_compress -= block_size;
				data_ptr += block_size;
			};

			const auto compress_block_zlib = [&]()
			{
				const auto block_size = static_cast<unsigned int>(std::min(bytes_to_compress, MAX_BLOCK_SIZE));
				auto bound = compressBound(static_cast<unsigned long>(block_size));

				std::string buffer;
				buffer.resize(bound);

				compress2(reinterpret_cast<unsigned char*>(buffer.data()), &bound,
					reinterpret_cast<const unsigned char*>(data_ptr), static_cast<uLong>(size), Z_BEST_COMPRESSION);
				const auto compressed_size = bound;
				buffer.resize(compressed_size);

				if (first_block)
				{
					XBlockCompressionDataHeader header{};
					header.uncompressedSize = bytes_to_compress;
					header.blockSizeAndType.blockSize = MAX_BLOCK_SIZE;
					header.blockSizeAndType.compressionType = XBLOCK_COMPRESSION_ZLIB_SIZE;

					write(&header, sizeof(header));
				}

				XBlockCompressionBlockHeader block_header{};
				block_header.compressedSize = compressed_size;
				block_header.uncompressedSize = block_size;

				write(&block_header, sizeof(block_header));

				write(buffer.data(), buffer.size());

				first_block = false;

				bytes_to_compress -= block_size;
				data_ptr += block_size;
			};

			const auto compress_block_none = [&]()
			{
				const auto block_size = static_cast<unsigned int>(std::min(bytes_to_compress, MAX_BLOCK_SIZE));

				if (first_block)
				{
					XBlockCompressionDataHeader header{};
					header.uncompressedSize = bytes_to_compress;
					header.blockSizeAndType.blockSize = MAX_BLOCK_SIZE;
					header.blockSizeAndType.compressionType = XBLOCK_COMPRESSION_NONE;

					write(&header, sizeof(header));
				}

				XBlockCompressionBlockHeader block_header{};
				block_header.compressedSize = 0;
				block_header.uncompressedSize = block_size;

				write(&block_header, sizeof(block_header));

				write(const_cast<char*>(data_ptr), block_size);

				first_block = false;

				bytes_to_compress -= block_size;
				data_ptr += block_size;
			};

			if (type == XBLOCK_COMPRESSION_LZ4 && bytes_to_compress > 0)
			{
				// the same bytes as the loop below: every 64 KiB block is compressed on its own, so the blocks are compressed
				// by several threads and written in order (a zone of a map is 1 GB or more of input)
				const auto total = static_cast<std::size_t>(bytes_to_compress);
				const auto count = (total + MAX_BLOCK_SIZE - 1) / MAX_BLOCK_SIZE;
				std::vector<std::vector<char>> packed(count);
				std::vector<unsigned int> packed_size(count);
				std::atomic<std::size_t> next{0};
				const auto workers = std::min<std::size_t>(16, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
				std::vector<std::thread> pool;
				for (std::size_t w = 0; w < workers; w++)
				{
					pool.emplace_back([&]
					{
						for (std::size_t i; (i = next.fetch_add(1)) < count;)
						{
							const auto block_size = static_cast<unsigned int>(std::min<std::size_t>(total - i * MAX_BLOCK_SIZE, MAX_BLOCK_SIZE));
							const auto bound = LZ4_compressBound(block_size);
							std::vector<char> buffer(bound);
							const auto compressed_size = LZ4_compress_default(data_ptr + i * MAX_BLOCK_SIZE, buffer.data(), block_size, bound);
							packed_size[i] = static_cast<unsigned int>(compressed_size);
							buffer.resize(align_value(compressed_size, 4));
							packed[i] = std::move(buffer);
						}
					});
				}
				for (auto& t : pool) t.join();

				std::size_t out_size = out_buffer.size() + sizeof(XBlockCompressionDataHeader);
				for (const auto& b : packed) out_size += sizeof(XBlockCompressionBlockHeader) + b.size();
				out_buffer.reserve(out_size);
				const auto append = [&](const void* p, const std::size_t n)
				{
					const auto* bytes = static_cast<const std::uint8_t*>(p);
					out_buffer.insert(out_buffer.end(), bytes, bytes + n);
				};
				XBlockCompressionDataHeader header{};
				header.uncompressedSize = total;
				header.blockSizeAndType.blockSize = MAX_BLOCK_SIZE;
				header.blockSizeAndType.compressionType = XBLOCK_COMPRESSION_LZ4;
				append(&header, sizeof(header));
				for (std::size_t i = 0; i < count; i++)
				{
					XBlockCompressionBlockHeader block_header{};
					block_header.compressedSize = packed_size[i];
					block_header.uncompressedSize = static_cast<std::uint64_t>(std::min<std::size_t>(total - i * MAX_BLOCK_SIZE, MAX_BLOCK_SIZE));
					append(&block_header, sizeof(block_header));
					append(packed[i].data(), packed[i].size());
				}
				return out_buffer;
			}

			while (bytes_to_compress > 0)
			{
				if (type == XBLOCK_COMPRESSION_LZ4)
				{
					compress_block_lz4();
				}
				else if (type == XBLOCK_COMPRESSION_LZ4HC)
				{
					compress_block_lz4hc();
				}
				else if (type == XBLOCK_COMPRESSION_ZLIB_SIZE)
				{
					compress_block_zlib();
				}
				else
				{
					compress_block_none(); // not working
				}
			}

			return out_buffer;
		}
	}

	namespace imagefile
	{
		void compress_images(const std::vector<gfx_image*>& images)
		{
			ZONETOOL_INFO("Compressing images...");

			const auto max_threads = std::thread::hardware_concurrency() * 2;
			const auto images_per_thread = std::max(1, static_cast<int>(images.size() / max_threads));

			std::vector<std::thread> threads;

			const auto start_thread = [&](const int index, const int count)
			{
				threads.emplace_back([&, index, count]
				{
					for (auto i = index; i < index + count; i++)
					{
						const auto image = images[i];
						for (auto o = 0; o < 4; o++)
						{
							auto& path = image->image_stream_blocks_paths[o];
							if (!path.has_value())
							{
								continue;
							}

							auto block = utils::io::read_file(path.value());
							const auto compressed = compression::iwc::compress_block(reinterpret_cast<std::uint8_t*>(block.data()), block.size(), XBLOCK_COMPRESSION_LZ4);
							const std::string compressed_str = { compressed.begin(), compressed.end() };
							image->image_stream_blocks[o].emplace(compressed_str);
						}
					}
				});
			};

			auto images_left = static_cast<int>(images.size());
			auto index = 0;
			while (images_left > 0)
			{
				const auto count = std::min(images_left, images_per_thread);
				start_thread(index, count);
				index += count;
				images_left -= count;
			}

			for (auto& thread : threads)
			{
				if (thread.joinable())
				{
					thread.join();
				}
			}
		}

		void generate(const std::string& fastfile, std::uint16_t index, int ff_version, const std::string& ff_magic,
			std::vector<gfx_image*> images, zone_memory* mem)
		{
			compress_images(images);

			if (images.size() == 0)
			{
				return;
			}

			std::string image_file_buffer;

			ZONETOOL_INFO("Writing imagefile...");

			XPakHeader header{};
			std::memcpy(&header.magic, ff_magic.data(), ff_magic.size());
			header.version = ff_version;

			image_file_buffer.append(reinterpret_cast<char*>(&header), sizeof(XPakHeader));

			const auto write_image_file = [&]
			{
				const auto header = reinterpret_cast<XPakHeader*>(image_file_buffer.data());

				const auto hash_start = reinterpret_cast<std::uint8_t*>(image_file_buffer.data() + sizeof(XPakHeader));
				const auto len = image_file_buffer.size() - sizeof(XPakHeader);

				const auto hash = utils::cryptography::sha256::compute(hash_start, len, false);
				std::memcpy(header->hash.bytes, hash.data(), sizeof(header->hash));

				const auto save_path = utils::io::directory_exists("zone") ? "zone/" : "";
				const auto name = utils::string::va("%s%s.pak", save_path, fastfile.data(), index);
				utils::io::write_file(name, image_file_buffer);
			};

			for (auto i = 0; i < 4; i++)
			{
				for (const auto& image : images)
				{
					if (image->image_stream_files[i])
					{
						continue;
					}

					image->image_stream_files[i] = mem->allocate<XStreamFile>();

					if (!image->image_stream_blocks[i].has_value())
					{
						continue;
					}

					const auto& data = image->image_stream_blocks[i].value();
					const auto offset = image_file_buffer.size();
					image_file_buffer.append(reinterpret_cast<const char*>(data.data()), data.size());
					const auto offset_end = image_file_buffer.size();
					image->image_stream_blocks[i].reset();

					image->image_stream_files[i]->fileIndex = index;
					image->image_stream_files[i]->offset = offset;
					image->image_stream_files[i]->offsetEnd = offset_end;
				}
			}

			write_image_file();
		}
	}

	std::string zone_interface::asset_key(std::int32_t type, const std::string& name)
	{
		return std::to_string(type) + '' + name;
	}

	void zone_interface::push_asset(const std::shared_ptr<asset_interface>& asset)
	{
		m_asset_index.try_emplace(asset_key(asset->type(), asset->name()), m_assets.size());
		m_assets.push_back(asset);
	}

	// Position of the first asset of this type and name, or m_assets.size(). Asset names are fixed once an asset is
	// pushed (init sets them before), so the index answers what the linear scan answered.
	std::size_t zone_interface::find_asset_index(std::int32_t type, const std::string& name)
	{
		const auto it = m_asset_index.find(asset_key(type, name));
		return it == m_asset_index.end() ? m_assets.size() : it->second;
	}

	asset_interface* zone_interface::find_asset(std::int32_t type, const std::string& name)
	{
		if (name.empty())
		{
			return nullptr;
		}

		const auto idx = find_asset_index(type, name);
		return idx < m_assets.size() ? m_assets[idx].get() : nullptr;
	}

	void* zone_interface::get_asset_pointer(std::int32_t type, const std::string& name)
	{
		if (name.empty())
		{
			return nullptr;
		}

		std::string ref_name = "," + name;
		if (name.starts_with(","))
		{
			ref_name = name;
			ref_name.erase(0, 1);
		}

		const std::uint64_t mask = 0x0000000000000000;
		const auto idx = std::min(find_asset_index(type, name), find_asset_index(type, ref_name));
		if (idx < m_assets.size())
		{
			auto ptr = mask | (static_cast<std::uint64_t>(XFILE_BLOCK_VIRTUAL) & 0x0F) << 32; // add stream index
			ptr = (ptr + static_cast<std::uint32_t>((this->m_assetbase + ((16 * idx) + 8) + 1))); // add offset
			return reinterpret_cast<void*>(ptr);
		}

		return nullptr;
	}

	void zone_interface::add_asset_of_type_by_pointer(std::int32_t type, void* pointer)
	{
		if (!pointer)
		{
			return;
		}

		const std::string& name = get_asset_name(XAssetType(type), pointer);
		if (asset_cache::t_frame) asset_cache::t_frame->unsafe = true;

		// don't add asset if it already exists
		if (find_asset_index(type, name) < m_assets.size())
		{
			return;
		}

#define ADD_ASSET_PTR(__type__, ___) \
		if (type == __type__) \
		{ \
			auto asset = std::make_shared < ___ >(); \
			asset->init(pointer, this->m_zonemem.get()); \
			asset->load_depending(this); \
			push_asset(asset); \
		}

		try
		{
			// declare asset interfaces
			ADD_ASSET_PTR(ASSET_TYPE_LOCALIZE_ENTRY, localize);
		}
		catch (std::exception& ex)
		{
			ZONETOOL_FATAL("A fatal exception occured while adding asset \"%s\" of type %s, exception was: \n%s",
				name.data(), type_to_string(XAssetType(type)), ex.what());
		}
	}


	void zone_interface::add_asset_of_type(std::int32_t type, const std::string& _name)
	{
		std::string name = _name;

		if (name.empty())
		{
			return;
		}

		if (asset_cache::t_frame && !asset_cache::t_quiet)
		{
			asset_cache::t_frame->deps.emplace_back(type, _name);
		}

		// add ignore assets as referenced
		if (ignore_assets.find(std::make_pair(static_cast<std::uint32_t>(type), name)) != ignore_assets.end())
		{
			if (!name.starts_with(","))
			{
				name = ","s + name;
			}
		}

		// don't add asset if it already exists
		if (get_asset_pointer(type, name))
		{
			return;
		}

		// the cached add of one asset (see asset_cache): a hit replays the dependency additions and pushes the cached asset
		auto add_cached = [&](auto* tag)
		{
			using T = std::remove_pointer_t<decltype(tag)>;
			// the world assets (the generated map: aipaths, clipmap, ...) are parsed from the compile's output on every build and some
			// of them rewrite their own source tree when they are written (path_data::write: access violation on a second write of the
			// same parsed object, measured), so they are never taken from the cache
			const auto world_type = type == ASSET_TYPE_PATHDATA || type == ASSET_TYPE_CLIPMAP || type == ASSET_TYPE_COMWORLD || type == ASSET_TYPE_FXWORLD ||
				type == ASSET_TYPE_GFXWORLD || type == ASSET_TYPE_GFXWORLD_TRANSIENT_ZONE || type == ASSET_TYPE_GLASSWORLD ||
				type == ASSET_TYPE_MAP_ENTS || type == ASSET_TYPE_NAVMESH;
			const auto cached = asset_cache::active() && !world_type && !name_.starts_with("paris_enemy_");
			const auto key = asset_key(type, name);
			if (cached)
			{
				const auto dep_key = [this](const std::pair<std::int32_t, std::string>& d)
				{
					auto n = d.second;
					if (n.empty()) return std::string{};
					if (ignore_assets.find(std::make_pair(static_cast<std::uint32_t>(d.first), n)) != ignore_assets.end() && !n.starts_with(",")) n = ","s + n;
					return asset_key(d.first, n);
				};
				if (asset_cache::usable(key, dep_key))
				{
					const auto entry = asset_cache::g_table.at(key);
					for (const auto& d : entry.deps) add_asset_of_type(d.first, d.second);
					asset_cache::g_hits++;
					push_asset(entry.asset);
					return;
				}
				if (asset_cache::g_table.contains(key)) asset_cache::g_invalid++;
				asset_cache::g_misses++;
			}

			auto asset = std::make_shared<T>();
			{
				asset_cache::guard scope(cached);
				asset->init(name, cached ? asset_cache::arena() : this->m_zonemem.get());
				dump_enemy_pack_asset(static_cast<XAssetType>(type), asset->pointer());
				asset->load_depending(this);
				if (cached)
				{
					// an asset that took a game zone's memory (the default asset of a missing file) dies with that zone
					if (scope.f.unsafe || (asset->pointer() && !asset_cache::in_arena(asset->pointer())))
					{
						asset_cache::g_uncacheable++;
						asset_cache::g_table.erase(key);
					}
					else
					{
						asset_cache::entry e;
						e.asset = asset;
						e.deps = std::move(scope.f.deps);
						for (const auto& p : scope.f.files) e.files.emplace_back(asset_cache::path_id(p), asset_cache::stat_of(p));
						asset_cache::g_table[key] = std::move(e);
						asset_cache::g_ok[key] = 1;
					}
				}
			}
			push_asset(asset);
		};

#define ADD_ASSET(__type__, ___) \
		if (type == __type__) \
		{ \
			add_cached(static_cast< ___ *>(nullptr)); \
		}

		try
		{
			// declare asset interfaces
			if (name_.starts_with("paris_enemy_"))
			{
				ADD_ASSET(ASSET_TYPE_ANIMCLASS, iw7_anim_class);
				ADD_ASSET(ASSET_TYPE_BEHAVIOR_TREE, iw7_behavior_tree);
				ADD_ASSET(ASSET_TYPE_XANIM_PROCEDURALBONES, iw7_procedural_bones);
			}
			ADD_ASSET(ASSET_TYPE_DDL, ddl);
			ADD_ASSET(ASSET_TYPE_FX, fx_effect_def);
			ADD_ASSET(ASSET_TYPE_PARTICLE_SIM_ANIMATION, fx_particle_sim_animation);
			ADD_ASSET(ASSET_TYPE_GESTURE, gesture);
			ADD_ASSET(ASSET_TYPE_PLAYERANIM, player_anim);
			ADD_ASSET(ASSET_TYPE_IMAGE, gfx_image);
			ADD_ASSET(ASSET_TYPE_LIGHT_DEF, gfx_light_def);
			ADD_ASSET(ASSET_TYPE_GFXLIGHTMAP, gfx_light_map);
			ADD_ASSET(ASSET_TYPE_IMPACT_FX, impact_fx);
			ADD_ASSET(ASSET_TYPE_LASER, laser);
			ADD_ASSET(ASSET_TYPE_LOCALIZE_ENTRY, localize);
			ADD_ASSET(ASSET_TYPE_LUA_FILE, lua_file);
			ADD_ASSET(ASSET_TYPE_MATERIAL, material);
			ADD_ASSET(ASSET_TYPE_NET_CONST_STRINGS, net_const_strings);
			ADD_ASSET(ASSET_TYPE_VFX, particle_system);
			ADD_ASSET(ASSET_TYPE_RAWFILE, rawfile);
			ADD_ASSET(ASSET_TYPE_RETICLE, reticle);
			ADD_ASSET(ASSET_TYPE_RUMBLE, rumble);
			ADD_ASSET(ASSET_TYPE_RUMBLE_GRAPH, rumble_graph);
			ADD_ASSET(ASSET_TYPE_BEHAVIOR_TREE, behavior_tree);
			ADD_ASSET(ASSET_TYPE_ANIMCLASS, anim_class);
			ADD_ASSET(ASSET_TYPE_SCRIPTABLE, scriptable_def);
			ADD_ASSET(ASSET_TYPE_SCRIPTFILE, scriptfile);
			ADD_ASSET(ASSET_TYPE_STREAMING_INFO, streaming_info);
			ADD_ASSET(ASSET_TYPE_STRINGTABLE, string_table);
			ADD_ASSET(ASSET_TYPE_TRACER, tracer);
			ADD_ASSET(ASSET_TYPE_TTF, ttf_def);
			ADD_ASSET(ASSET_TYPE_VECTORFIELD, vector_field);
			ADD_ASSET(ASSET_TYPE_ATTACHMENT, weapon_attachment);
			ADD_ASSET(ASSET_TYPE_ANIM_PACKAGE, weapon_anim_package);
			ADD_ASSET(ASSET_TYPE_SFX_PACKAGE, weapon_sfx_package);
			ADD_ASSET(ASSET_TYPE_VFX_PACKAGE, weapon_vfx_package);
			ADD_ASSET(ASSET_TYPE_WEAPON, weapon_def);
			ADD_ASSET(ASSET_TYPE_XANIMPARTS, xanim_parts);
			ADD_ASSET(ASSET_TYPE_XMODEL, xmodel);
			ADD_ASSET(ASSET_TYPE_XMODEL_SURFS, xsurface);
			
			//ADD_ASSET(ASSET_TYPE_SOUND_GLOBALS, sound_globals);
			ADD_ASSET(ASSET_TYPE_SOUND_BANK, sound_bank);
			//ADD_ASSET(ASSET_TYPE_SOUND_BANK_TRANSIENT, sound_bank_transient);

			ADD_ASSET(ASSET_TYPE_PHYSICSASSET, physics_asset);
			ADD_ASSET(ASSET_TYPE_PHYSICS_FX_PIPELINE, physics_fx_pipeline);
			ADD_ASSET(ASSET_TYPE_PHYSICS_FX_SHAPE, physics_fx_shape);
			ADD_ASSET(ASSET_TYPE_PHYSICSLIBRARY, physics_library);
			ADD_ASSET(ASSET_TYPE_PHYSICS_SFX_EVENT_ASSET, physics_sfx_event);
			ADD_ASSET(ASSET_TYPE_PHYSICS_VFX_EVENT_ASSET, physics_vfx_event);

			ADD_ASSET(ASSET_TYPE_COMPUTESHADER, compute_shader);
			ADD_ASSET(ASSET_TYPE_DOMAINSHADER, domain_shader);
			ADD_ASSET(ASSET_TYPE_HULLSHADER, hull_shader);
			ADD_ASSET(ASSET_TYPE_PIXELSHADER, pixel_shader);
			//ADD_ASSET(ASSET_TYPE_VERTEXDECL, vertex_decl);
			ADD_ASSET(ASSET_TYPE_VERTEXSHADER, vertex_shader);

			ADD_ASSET(ASSET_TYPE_TECHNIQUE_SET, techset);

			ADD_ASSET(ASSET_TYPE_PATHDATA, path_data);
			ADD_ASSET(ASSET_TYPE_CLIPMAP, clip_map);
			ADD_ASSET(ASSET_TYPE_COMWORLD, com_world);
			ADD_ASSET(ASSET_TYPE_FXWORLD, fx_world);
			ADD_ASSET(ASSET_TYPE_GFXWORLD, gfx_world);
			ADD_ASSET(ASSET_TYPE_GFXWORLD_TRANSIENT_ZONE, gfx_world_tr);
			ADD_ASSET(ASSET_TYPE_GLASSWORLD, glass_world);
			ADD_ASSET(ASSET_TYPE_MAP_ENTS, map_ents);
			ADD_ASSET(ASSET_TYPE_NAVMESH, nav_mesh);
			if (name_.starts_with("paris_enemy_") && !get_asset_pointer(type, name))
				throw std::runtime_error("Unsupported dependency in IW7 enemy pack");
		}
		catch (std::exception& ex)
		{
			ZONETOOL_FATAL("A fatal exception occured while adding asset \"%s\" of type %s, exception was: \n%s",
				name.data(), type_to_string(XAssetType(type)), ex.what());
		}
	}

	std::int32_t zone_interface::get_type_by_name(const std::string& type)
	{
		return type_to_int(type);
	}

	void zone_interface::add_asset_of_type(const std::string& type, const std::string& name)
	{
		std::int32_t itype = type_to_int(type);
		this->add_asset_of_type(itype, name);
	}

	void zone_interface::build(zone_buffer* buf)
	{
		if (name_.starts_with("paris_enemy_"))
		{
			const auto suffix = name_.substr(std::string("paris_enemy_").size());
			if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), [](unsigned char c)
				{ return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
				throw std::runtime_error("Unsafe IW7 enemy pack name");

			const bool script_pack = suffix.ends_with("_scripts");
			const std::unordered_set<int> forbidden_world{ASSET_TYPE_CLIPMAP, ASSET_TYPE_COMWORLD,
				ASSET_TYPE_GFXWORLD, ASSET_TYPE_GFXWORLD_TRANSIENT_ZONE, ASSET_TYPE_FXWORLD,
				ASSET_TYPE_GLASSWORLD, ASSET_TYPE_MAP_ENTS, ASSET_TYPE_ADDON_MAP_ENTS,
				ASSET_TYPE_PATHDATA, ASSET_TYPE_NAVMESH};
			std::string inventory;
			for (auto& asset : m_assets)
			{
				const auto type = asset->type();
				const auto asset_name = asset->name();
				if (forbidden_world.contains(type)) throw std::runtime_error("Map/world asset forbidden in IW7 enemy pack");
				if (script_pack)
				{
					if (type != ASSET_TYPE_SCRIPTFILE && type != ASSET_TYPE_RAWFILE)
						throw std::runtime_error("Non-script dependency entered IW7 enemy script pack");
					if (type == ASSET_TYPE_RAWFILE && asset_name != name_)
						throw std::runtime_error("Unexpected rawfile entered IW7 enemy script pack");
				}
				else
				{
					if (type == ASSET_TYPE_SCRIPTFILE)
						throw std::runtime_error("Scriptfile entered IW7 enemy client pack");
					if (type == ASSET_TYPE_RAWFILE)
					{
						if (asset_name != name_)
						{
							auto rawfile_name = asset_name;
							if (rawfile_name.starts_with(",")) rawfile_name.erase(0, 1);
							std::replace(rawfile_name.begin(), rawfile_name.end(), '\\', '/');
							const std::filesystem::path rawfile_path(rawfile_name);
							if (!rawfile_name.starts_with("animtrees/") || !rawfile_name.ends_with(".atr") ||
								rawfile_path.is_absolute() || std::any_of(rawfile_path.begin(), rawfile_path.end(),
									[](const auto& part) { return part == "." || part == ".."; }))
							throw std::runtime_error("Unexpected rawfile entered IW7 enemy client pack: " + asset_name);
						}
					}
					if (type == ASSET_TYPE_STRINGTABLE && asset_name != "mp/" + name_ + "_definition.csv")
						throw std::runtime_error("Unexpected stringtable entered IW7 enemy client pack");
				}
				inventory += std::string(type_to_string(static_cast<XAssetType>(type))) + "," + asset_name + "\n";
			}
			utils::io::write_file(name_ + ".assets.csv", inventory);
		}

		buf->init_streams(MAX_XFILE_COUNT);

		[[maybe_unused]] const auto start_time = GetTickCount64();
		phase_clock clock;

		ZONETOOL_INFO("Compiling fastfile \"%s\"...", this->name_.data());

		{
			std::vector<gfx_image*> images;
			for (std::size_t i = 0; i < m_assets.size(); i++)
			{
				if (m_assets[i]->type() == ASSET_TYPE_IMAGE)
				{
					const auto image = static_cast<gfx_image*>(m_assets[i].get());
					if (image->custom_streamed_image)
					{
						images.emplace_back(image);
					}
				}
			}

			if (images.size() > 0)
			{
				imagefile::generate(filesystem::get_fastfile(),
					CUSTOM_IMAGEFILE_INDEX, FF_VERSION, FF_MAGIC_UNSIGNED, images, this->m_zonemem.get());
			}
		}

		std::uintptr_t following = static_cast<std::uintptr_t>(buf->data_following);
		std::uintptr_t zero = 0; // data_none

		// write asset types to header
		for (std::size_t i = 0; i < m_assets.size(); i++)
		{
			m_assets[i]->prepare(buf, this->m_zonemem.get());
		}
		ZONETOOL_INFO("[ TIMING ] zone \"%s\": images + prepare of %zu assets %.3f s", this->name_.data(), m_assets.size(), clock.lap());

		buf->push_stream(XFILE_BLOCK_TEMP);

		// write scriptstring count
		std::size_t stringcount = buf->scriptstring_count();
		buf->write<std::uintptr_t>(&stringcount);
		buf->write<std::uintptr_t>(stringcount > 0 ? (&following) : (&zero)); // pointer to scriptstrings

		// write asset count
		std::size_t asset_count = m_assets.size();
		buf->write<std::uintptr_t>(&asset_count);
		buf->write<std::uintptr_t>(asset_count > 0 ? (&following) : (&zero)); // pointer to assets

		bool write_globals = (buf->depthstencilstatebit_count() + buf->blendstatebits_count()) > 0;
		buf->write<std::uintptr_t>(write_globals ? (&following) : (&zero)); // pointer to globals

		buf->pop_stream();

		buf->push_stream(XFILE_BLOCK_VIRTUAL);

		// write scriptstrings
		buf->push_stream(XFILE_BLOCK_VIRTUAL);
		if (stringcount)
		{
			buf->align(7);

			// write pointer for every scriptstring
			for (std::size_t idx = 0; idx < stringcount; idx++)
			{
				const auto str = buf->get_scriptstring(idx);
				if (str == nullptr)
				{
					buf->write<std::uintptr_t>(&zero);
				}
				else
				{
					buf->write<std::uintptr_t>(&following);
				}
			}

			// write scriptstrings
			for (std::size_t idx = 0; idx < stringcount; idx++)
			{
				const auto str = buf->get_scriptstring(idx);
				if (str != nullptr)
				{
					buf->write_str(str);
				}
			}
		}
		buf->pop_stream();

		// write globals
		if (write_globals)
		{
			buf->align(7);
			buf->write<std::uintptr_t>(&following); // pointer to gfxglobals

			// parse gfxglobals
			auto mem_ = this->m_zonemem.get();
			auto globals = mem_->allocate<XGfxGlobals>();

			globals->depthStencilStateCount = static_cast<unsigned int>(buf->depthstencilstatebit_count());
			globals->depthStencilStateBits = mem_->allocate<std::uint64_t>(globals->depthStencilStateCount);
			globals->depthStencilStates = mem_->allocate<GfxZoneTableEntry>(globals->depthStencilStateCount);

			for (unsigned int i = 0; i < globals->depthStencilStateCount; i++)
			{
				globals->depthStencilStateBits[i] = buf->get_depthstencilstatebit(i);
			}

			globals->blendStateCount = static_cast<unsigned int>(buf->blendstatebits_count());
			globals->blendStateBits = mem_->allocate<GfxBlendStateBits>(globals->blendStateCount);
			globals->blendStates = mem_->allocate<GfxZoneTableEntry>(globals->blendStateCount);

			for (unsigned int i = 0; i < globals->blendStateCount; i++)
			{
				for (auto j = 0; j < 4; j++)
				{
					globals->blendStateBits[i][j] = buf->get_blendstatebits(i)[j];
				}
			}

			globals->perPrimConstantBufferCount = static_cast<unsigned int>(buf->ppas_count());
			globals->perPrimConstantBufferSizes = mem_->allocate<unsigned int>(globals->perPrimConstantBufferCount);
			globals->perPrimConstantBuffers = mem_->allocate<GfxZoneTableEntry>(globals->perPrimConstantBufferCount);

			for (unsigned int i = 0; i < globals->perPrimConstantBufferCount; i++)
			{
				globals->perPrimConstantBufferSizes[i] = buf->get_ppas(i);
			}

			globals->perObjConstantBufferCount = static_cast<unsigned int>(buf->poas_count());
			globals->perObjConstantBufferSizes = mem_->allocate<unsigned int>(globals->perObjConstantBufferCount);
			globals->perObjConstantBuffers = mem_->allocate<GfxZoneTableEntry>(globals->perObjConstantBufferCount);

			for (unsigned int i = 0; i < globals->perObjConstantBufferCount; i++)
			{
				globals->perObjConstantBufferSizes[i] = buf->get_poas(i);
			}

			globals->stableConstantBufferCount = static_cast<unsigned int>(buf->sas_count());
			globals->stableConstantBufferSizes = mem_->allocate<unsigned int>(globals->stableConstantBufferCount);
			globals->stableConstantBuffers = mem_->allocate<GfxZoneTableEntry>(globals->stableConstantBufferCount);

			for (unsigned int i = 0; i < globals->stableConstantBufferCount; i++)
			{
				globals->stableConstantBufferSizes[i] = buf->get_sas(i);
			}

			// write gfxglobals
			auto dest = buf->write(globals);
			if (globals->depthStencilStateBits)
			{
				buf->align(3);
				buf->write(globals->depthStencilStateBits, globals->depthStencilStateCount);
				buf->clear_pointer(&dest->depthStencilStateBits);
			}
			if (globals->blendStateBits)
			{
				buf->align(3);
				buf->write(globals->blendStateBits, globals->blendStateCount);
				buf->clear_pointer(&dest->blendStateBits);
			}
			buf->push_stream(XFILE_BLOCK_RUNTIME);
			if (globals->depthStencilStates)
			{
				buf->align(7);
				auto dest_0 = buf->write(globals->depthStencilStates, globals->depthStencilStateCount);
				for (unsigned int i = 0; i < globals->depthStencilStateCount; i++)
				{
					if (globals->depthStencilStates[i].voidPtr)
					{
						buf->align(0);
						buf->write(globals->depthStencilStates[i].dataPtr, 1);
						buf->clear_pointer(&dest_0->dataPtr);
					}
				}
				buf->clear_pointer(&dest->depthStencilStates);
			}
			buf->pop_stream();
			buf->push_stream(XFILE_BLOCK_RUNTIME);
			if (globals->blendStates)
			{
				buf->align(7);
				auto dest_0 = buf->write(globals->blendStates, globals->blendStateCount);
				for (unsigned int i = 0; i < globals->blendStateCount; i++)
				{
					if (globals->blendStates[i].voidPtr)
					{
						buf->align(0);
						buf->write(globals->blendStates[i].dataPtr, 1);
						buf->clear_pointer(&dest_0->dataPtr);
					}
				}
				buf->clear_pointer(&dest->blendStates);
			}
			buf->pop_stream();
			if (globals->perPrimConstantBufferSizes)
			{
				buf->align(3);
				buf->write(globals->perPrimConstantBufferSizes, globals->perPrimConstantBufferCount);
				buf->clear_pointer(&dest->perPrimConstantBufferSizes);
			}
			if (globals->perObjConstantBufferSizes)
			{
				buf->align(3);
				buf->write(globals->perObjConstantBufferSizes, globals->perObjConstantBufferCount);
				buf->clear_pointer(&dest->perObjConstantBufferSizes);
			}
			if (globals->stableConstantBufferSizes)
			{
				buf->align(3);
				buf->write(globals->stableConstantBufferSizes, globals->stableConstantBufferCount);
				buf->clear_pointer(&dest->stableConstantBufferSizes);
			}
			buf->push_stream(XFILE_BLOCK_RUNTIME);
			if (globals->perPrimConstantBuffers)
			{
				buf->align(7);
				auto dest_0 = buf->write(globals->perPrimConstantBuffers, globals->perPrimConstantBufferCount);
				for (unsigned int i = 0; i < globals->perPrimConstantBufferCount; i++)
				{
					if (globals->perPrimConstantBuffers[i].voidPtr)
					{
						buf->align(0);
						buf->write(globals->perPrimConstantBuffers[i].dataPtr, 1);
						buf->clear_pointer(&dest_0->dataPtr);
					}
				}
				buf->clear_pointer(&dest->perPrimConstantBuffers);
			}
			buf->pop_stream();
			buf->push_stream(XFILE_BLOCK_RUNTIME);
			if (globals->perObjConstantBuffers)
			{
				buf->align(3);
				auto dest_0 = buf->write(globals->perObjConstantBuffers, globals->perObjConstantBufferCount);
				for (unsigned int i = 0; i < globals->perObjConstantBufferCount; i++)
				{
					if (globals->perObjConstantBuffers[i].voidPtr)
					{
						buf->align(0);
						buf->write(globals->perObjConstantBuffers[i].dataPtr, 1);
						buf->clear_pointer(&dest_0->dataPtr);
					}
				}
				buf->clear_pointer(&dest->perObjConstantBuffers);
			}
			buf->pop_stream();
			buf->push_stream(XFILE_BLOCK_RUNTIME);
			if (globals->stableConstantBuffers)
			{
				buf->align(7);
				auto dest_0 = buf->write(globals->stableConstantBuffers, globals->stableConstantBufferCount);
				for (unsigned int i = 0; i < globals->stableConstantBufferCount; i++)
				{
					if (globals->stableConstantBuffers[i].voidPtr)
					{
						buf->align(0);
						buf->write(globals->stableConstantBuffers[i].dataPtr, 1);
						buf->clear_pointer(&dest_0->dataPtr);
					}
				}
				buf->clear_pointer(&dest->stableConstantBuffers);
			}
			buf->pop_stream();
		}

		buf->pop_stream();
		buf->push_stream(XFILE_BLOCK_VIRTUAL);

		// align buffer
		buf->align(7);

		// set asset ptr base
		this->m_assetbase = buf->stream_offset(XFILE_BLOCK_VIRTUAL);
		ZONETOOL_INFO("m_assetbase: %llu", this->m_assetbase);

		// write asset types to header
		for (std::size_t i = 0; i < asset_count; i++)
		{
			// write asset data to zone
			auto type = static_cast<uintptr_t>(m_assets[i]->type());

			buf->write(&type);
			buf->write(&following);
		}

		ZONETOOL_INFO("[ TIMING ] zone \"%s\": header and globals %.3f s", this->name_.data(), clock.lap());

		// write assets
		const char* const write_log = std::getenv("ZT_WRITE_LOG");
		std::string write_log_text;
		std::map<int, std::pair<double, std::size_t>> write_by_type;
		for (auto& asset : m_assets)
		{
#ifdef DEBUG
			ZONETOOL_INFO("writing asset \"%s\" of type %s...", asset->name().data(), type_to_string(XAssetType(asset->type())));
#endif
			phase_clock one;

			// push stream
			buf->push_stream(XFILE_BLOCK_TEMP);
			buf->align(7);

			const auto write_from = write_log ? buf->size() : 0;

			// write asset
			asset->write(this, buf);

			// pop stream
			buf->pop_stream();

			if (write_log)
			{
				// ZT_WRITE_LOG=<file>: one line per asset (type, name, bytes, hash of the bytes), then the script strings:
				// the comparison of two builds names the assets whose output differs (cold against warm server, A against A)
				std::uint64_t h = 1469598103934665603ull;
				for (auto at = write_from; at < buf->size(); at++) h = (h ^ buf->buffer()[at]) * 1099511628211ull;
				write_log_text += utils::string::va("A\t%d\t%s\t%zu\t%016llx\n", asset->type(), asset->name().data(), buf->size() - write_from, h);
			}

			auto& slot = write_by_type[asset->type()];
			slot.first += one.lap();
			slot.second++;
		}
		if (write_log)
		{
			for (std::size_t i = 0; i < buf->scriptstring_count(); i++)
			{
				write_log_text += utils::string::va("S\t%zu\t%s\n", i, buf->get_scriptstring(i));
			}
			utils::io::write_file(write_log, write_log_text);
		}
		ZONETOOL_INFO("[ TIMING ] zone \"%s\": asset write loop %.3f s", this->name_.data(), clock.lap());
		for (const auto& [type, v] : write_by_type)
		{
			if (v.first >= 0.05)
			{
				ZONETOOL_INFO("[ TIMING ] zone \"%s\":   write %s: %zu assets %.3f s", this->name_.data(), type_to_string(XAssetType(type)), v.second, v.first);
			}
		}

		// pop stream
		buf->pop_stream();

#ifdef DEBUG
		// Dump zone to disk (for debugging)
		buf->save("zonetool\\_debug\\" + this->name_ + ".zone", false);
#endif

		ZONETOOL_INFO("[ TIMING ] zone \"%s\": zone buffer %zu bytes", this->name_.data(), static_cast<std::size_t>(buf->size()));
		clock.lap();
		// Compress buffer
#if (COMPRESSOR == COMPRESSOR_BLOCK)
#ifdef FF_SIGNED
		std::vector<DB_AuthHash> chunk_hashes{};
		const auto buf_compressed = compression::iwc::compress_block_signed(buf->buffer(), buf->size(), COMPRESS_BLOCK_TYPE, chunk_hashes);
		const auto buf_output = buf_compressed.data();
		const auto buf_output_size = buf_compressed.size();
#else
		const auto buf_compressed = compression::iwc::compress_block(buf->buffer(), buf->size(), COMPRESS_BLOCK_TYPE);
		const auto buf_output = buf_compressed.data();
		const auto buf_output_size = buf_compressed.size();
#endif
#elif (COMPRESSOR == COMPRESSOR_PASSTHROUGH)
		const auto buf_output = buf->buffer();
		const auto buf_output_size = buf->size();

		XFileCompressorHeader compress_header{};
		memcpy(compress_header.magic, "IWC", 3);
		compress_header.compressor = DB_COMPRESSOR_PASSTHROUGH;
#endif

		ZONETOOL_INFO("[ TIMING ] zone \"%s\": compression %.3f s", this->name_.data(), clock.lap());
		// Generate FF header
		XFileHeader header{};
		strcat(header.magic, FF_MAGIC);
		header.version = FF_VERSION;
		header.unused = 0;
		header.has_no_image_fastfile = 1;
		header.has_no_shared_fastfile = 1;
		header.unk1 = 1;
		header.fileTimeLow = 0;
		header.fileTimeHigh = 0;
		header.shared_ff_hash = 0;
		header.shared_ff_count = 0;
		header.image_ff_hash = 0;
		header.image_ff_count = 0;
		header.fileLen = buf_output_size + sizeof(XFileHeader);
		header.fileLenUnk1 = 0; // imagefile?
		header.fileLenUnk2 = 0; // shared?

		const auto image_streamfiles_count = buf->streamfile_count();
		if (image_streamfiles_count)
		{
			header.image_ff_count = static_cast<std::uint32_t>(image_streamfiles_count);
		}

		{
			header.stream_data.unk1 = 0;
			header.stream_data.unk2 = 0;

			std::uint64_t total_block_size = 0;
			memset(header.stream_data.block_size, 0, sizeof(header.stream_data.block_size));

			header.stream_data.block_size[XFILE_BLOCK_IMAGE_STREAM] = sizeof(XStreamFile) * image_streamfiles_count;

			for (auto i = 0; i < MAX_XFILE_COUNT; i++)
			{
				header.stream_data.block_size[i] += buf->stream_offset(static_cast<std::uint8_t>(i));
				total_block_size += header.stream_data.block_size[i];
			}

			header.stream_data.block_size[XFILE_BLOCK_TEMP_PRELOAD] = header.stream_data.block_size[XFILE_BLOCK_TEMP];
			total_block_size += header.stream_data.block_size[XFILE_BLOCK_TEMP_PRELOAD];

			header.stream_data.block_size[XFILE_BLOCK_TEMP_POSTLOAD] = header.stream_data.block_size[XFILE_BLOCK_TEMP];
			total_block_size += header.stream_data.block_size[XFILE_BLOCK_TEMP_POSTLOAD];

			header.stream_data.size = total_block_size; // not correct
			memset(header.stream_data.unk_arr, 0, sizeof(header.stream_data.unk_arr));
		}

#ifdef FF_SIGNED
		header.fileLen += sizeof(DB_MasterBlock);

		DB_MasterBlock master_block{};
		memset(&master_block, 0, sizeof(DB_MasterBlock));

		// process_master_blocks()
		{
			for (auto i = 0; i < chunk_hashes.size(); i++)
			{
				memcpy(master_block.chunkHashes[i].bytes, chunk_hashes[i].bytes, sizeof(DB_AuthHash));
			}
		}

		header.fileLen += sizeof(DB_AuthHeader);

		DB_AuthHeader auth_header{};
		memset(&auth_header, 0, sizeof(DB_AuthHeader));
		strcat(auth_header.magic, "IWffs100");
		auth_header.reserved = 0;
		strcat(auth_header.subheader.fastfileName, this->name_.data());
		auth_header.subheader.reserved = 0;

		// process_sub_header_master_block_hashes()
		{
			///for (std::uint8_t i = 0; i < 192; i++)
			{
				hash_state state{};
				sha256_init(&state);
				sha256_process(&state, reinterpret_cast<unsigned char*>(&master_block), sizeof(DB_MasterBlock));
				sha256_done(&state, auth_header.subheader.masterBlockHashes[0].bytes);
			}
		}

		// process_sub_header()
		{
			hash_state state{};
			sha256_init(&state);
			sha256_process(&state, reinterpret_cast<unsigned char*>(&auth_header.subheader), sizeof(DB_AuthSubHeader));
			sha256_done(&state, auth_header.subheaderHash.bytes);
		}

		// process_signature
		{
			// we don't know the private key so we can't generate signature?
			constexpr auto DEADBEEF = 0xDEADBEEF;
			memcpy_s(auth_header.signedSubheaderHash.bytes, sizeof(DB_AuthSignature), &DEADBEEF, sizeof(DEADBEEF));
		}
#endif

		header.fileLen += sizeof(XStreamFile) * header.image_ff_count;
		header.fileLen += sizeof(XStreamFile) * header.shared_ff_count;

		zone_buffer fastfile(header.fileLen);

		// Do streamfile stuff
		if (image_streamfiles_count > 0)
		{
			const auto offset = offsetof(XFileHeader, fileLen);

			// Generate fastfile
			fastfile.init_streams(1);
			fastfile.write_stream(&header, offset);

			// Write stream files
			for (std::size_t i = 0; i < image_streamfiles_count; i++)
			{
				auto* stream = reinterpret_cast<XStreamFile*>(buf->get_streamfile(i));
				fastfile.write_stream(stream, sizeof(XStreamFile));
			}

			fastfile.write_stream(reinterpret_cast<std::uint8_t*>(&header) + offset, sizeof(XFileHeader) - offset);
		}
		else
		{
			// Generate fastfile
			fastfile.init_streams(1);
			fastfile.write_stream(&header, sizeof(XFileHeader));
		}
#ifdef FF_SIGNED
		fastfile.write_stream(&auth_header, sizeof(DB_AuthHeader));
		fastfile.write_stream(&master_block, sizeof(DB_MasterBlock));
#endif
#if (COMPRESSOR == COMPRESSOR_PASSTHROUGH)
		fastfile.write_stream(&compress_header, sizeof(XFileCompressorHeader));
#endif

		fastfile.write(buf_output, buf_output_size);
		assert(fastfile.size() == header.fileLen);
		ZONETOOL_INFO("[ TIMING ] zone \"%s\": fastfile assembly (copy) %.3f s", this->name_.data(), clock.lap());

		std::string path = this->name_ + ".ff";
		fastfile.save(path);
		ZONETOOL_INFO("[ TIMING ] zone \"%s\": file write %.3f s", this->name_.data(), clock.lap());

		ZONETOOL_INFO("Successfully compiled fastfile \"%s\"!", this->name_.data());
		ZONETOOL_INFO("Compiling took %llu msec.", (GetTickCount64() - start_time));
	}

	zone_interface::zone_interface(std::string name)
	{
		this->name_ = name;

		this->m_assetbase = 0;

		this->m_zonemem = std::make_shared<zone_memory>(MAX_ZONE_SIZE);
	}

	zone_interface::~zone_interface()
	{
		// wipe all assets
		m_assets.clear();
		m_asset_index.clear();
	}
}
