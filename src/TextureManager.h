#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <filesystem>
#include <atomic>
#include "TextureHash.h"

namespace TextureToolkit
{
    enum class TextureStatus
    {
        ORIGINAL = 0,
        INJECTED = 1,   // a replacement is built AND bound in place of the original
        DUMPED = 2,
        PENDING = 3     // an inject file exists for this hash but is not applied (yet, or at all)
    };

    struct TextureDetails
    {
        uint64_t hash = 0;
        std::string hash_hex;

        // Special K's name for the same texture (CRC-32C of mip 0). Kept so an SK-named pack can
        // be matched, and so the panel can say which naming a replacement came from.
        uint32_t sk_hash = 0;
        bool injected_via_sk_name = false;

        uint32_t width = 0;          // original game-texture dimensions
        uint32_t height = 0;
        uint32_t mip_levels = 1;

        uint32_t repl_width = 0;     // injected replacement dimensions (0 if not injected)
        uint32_t repl_height = 0;

        uint32_t data_size = 0;      // GPU byte size of the full mip chain

        uint32_t format_id = 0;      // native DXGI_FORMAT / D3DFORMAT value
        std::string format_str;      // full name, e.g. "DXGI_FORMAT_BC3_UNORM"
        std::string format_short;    // list label, e.g. "DX11_BC3_UNORM"
        bool is_compressed = false;
        bool is_srgb = false;
        bool is_dx11 = false;

        // How the game actually samples the texture (the SRV's view format). For a TYPELESS
        // resource this is where the concrete format and sRGB intent live. 0 until first drawn.
        uint32_t view_format_id = 0;
        std::string view_format_str;

        // DX11 resource description (0 / defaults on DX9).
        uint32_t array_size = 1;
        uint32_t bind_flags = 0;
        uint32_t misc_flags = 0;
        uint32_t cpu_access = 0;
        uint32_t usage = 0;

        TextureStatus status = TextureStatus::ORIGINAL;
        std::string filepath_dumped;
        std::string filepath_injected;

        uint64_t replacement_handle = 0;
        uint64_t last_seen_frame = 0;   // scene-visibility test (per-frame)
        uint64_t last_seen_ticks = 0;   // eviction age (wall clock, so it is framerate independent)
    };

    class TextureManager
    {
    public:
        static TextureManager &get();

        void init();
        void shutdown();
        void rescan_injected();
        void on_frame();

        std::filesystem::path get_dump_dir() const { return m_dump_dir; }
        std::filesystem::path get_inject_dir() const { return m_inject_dir; }

        // Settings
        bool auto_dump = false;
        bool enable_injection = true;
        bool filter_small_textures = true;
        bool show_current_frame_only = false;
        bool accept_sk_names = true;   // also resolve Special K-named files in inject/

        // Active Texture Queries
        std::vector<TextureDetails> get_active_textures();

        // Injection health, for the panel: how many DDS files were found, how many are currently
        // applied, and how many were rejected. Surfacing "failed" is what stops a user having to
        // read the log to find out their file was never loaded.
        struct InjectionStats
        {
            size_t files_found = 0;
            size_t applied = 0;
            size_t failed = 0;
        };
        InjectionStats get_injection_stats();

        // Live original-texture preview. The UI names one hash as the preview target; the
        // next time that texture is bound we take a COM reference to the exact resource the
        // game is using, so the preview stays valid even under pointer reuse and cannot be
        // freed while shown. At most one original is pinned at a time. Returns 0 until the
        // target is bound (i.e. visible in the scene).
        void set_preview_target(uint64_t hash);
        uint64_t get_original_preview_handle();

        // Loads a dumped .dds from disk into a GPU texture for preview and caches it (one
        // at a time, released when the selection changes). Lets us show textures that are
        // not currently on screen. Returns a native texture id, or 0 on failure.
        uint64_t get_file_preview_handle(uint64_t hash, const std::string &dds_path, bool is_dx11);

        // Virtual Replacements for DX9
        IDirect3DBaseTexture9 *get_replacement_texture9(IDirect3DBaseTexture9 *orig);
        void register_unmap_texture9(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch);

        // Copies our content-hash tag from one D3D9 texture to another. Used when the game
        // copies a tagged SYSTEMMEM texture into the DEFAULT texture it actually renders, so
        // the bound texture becomes tracked, previewable and injectable.
        void copy_tag9(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst);

        // Virtual Replacements for DX11
        ID3D11ShaderResourceView *get_replacement_srv11(ID3D11ShaderResourceView *orig);
    private:
        ID3D11ShaderResourceView *resolve_srv11_slow(ID3D11ShaderResourceView *orig, uint32_t generation, uint64_t frame);
    public:
        // initial_levels/initial_level_count are the CreateTexture2D initial-data array when the
        // game supplied one. That is the only moment the whole mip chain is visible at once, so it
        // is the only path where auto-dump can capture more than the top level.
        void register_unmap_texture11(ID3D11Device *device, ID3D11Resource *resource, const void *pixel_data, UINT width, UINT height, DXGI_FORMAT format, UINT pitch,
                                      const D3D11_SUBRESOURCE_DATA *initial_levels = nullptr, UINT initial_level_count = 0);

        // Dumps a texture to TT/dump immediately (synchronously, on the calling thread) and
        // returns true on success. Reads back the live GPU resource: the pinned preview
        // reference when the texture is the current selection, otherwise the tracked handle.
        bool request_dump(uint64_t hash);

        // Queues a bulk dump. scene_only limits it to textures active this scene, otherwise
        // every tracked texture. Each is written the next time the game draws it (using the
        // live handle, so it is safe against pointer reuse). Returns the number queued.
        size_t dump_all(bool scene_only);

        // Queues an async dump from CPU pixel data already in hand (used by auto-dump). `levels`
        // holds tightly-packed data per mip level; most upload paths only ever expose level 0.
        bool dump_texture(uint64_t hash, UINT width, UINT height, DXGI_FORMAT format, std::vector<std::vector<uint8_t>> levels);

    private:
        TextureManager() = default;

        std::filesystem::path m_game_dir;
        std::filesystem::path m_dump_dir;
        std::filesystem::path m_inject_dir;

        mutable std::mutex m_mutex;
        uint64_t m_frame_count = 0;

        // Read by the bind hook without the lock (see get_replacement_srv11's fast path).
        std::atomic<uint64_t> m_frame_count_atomic{0};

        // Bumped whenever a cached bind decision could have gone stale: the inject folder was
        // rescanned, a replacement was built, the preview selection moved, or a bulk dump was
        // queued. A cache entry from an older generation is simply ignored and rebuilt.
        std::atomic<uint32_t> m_bind_generation{1};

        // Hashes seen this frame, buffered per thread so the bind hook does not take the manager
        // lock for every bound texture; merged under the lock on flush and in on_frame.
        void flush_seen_locked();
        uint64_t m_next_eviction_ticks = 0;

        // Active-scene tracking is keyed by content hash (immune to driver pointer reuse).
        std::unordered_set<uint64_t> m_current_frame_hashes;
        std::unordered_set<uint64_t> m_active_frame_hashes;
        std::unordered_map<uint64_t, std::filesystem::path> m_injected_files;
        std::unordered_map<uint32_t, std::filesystem::path> m_sk_injected_files; // Special K naming

        // True while any SK-named file is loaded. Read on the upload path without the lock: with
        // no such file present the Special K hash cannot match anything, and computing it means a
        // second full pass over every texture the game uploads.
        std::atomic<bool> m_have_sk_files{false};
        std::unordered_map<uint64_t, TextureDetails> m_tracked_textures;

        // Replacement maps are keyed by content hash, not by raw resource pointer.
        // The original resource is tagged with its hash via D3D private data
        // (TT_HASH_GUID), which the driver clears when the object is destroyed, so a
        // reused pointer can never inherit a stale replacement.
        // We own exactly one COM reference for each stored replacement.
        std::unordered_map<uint64_t, IDirect3DBaseTexture9 *> m_d3d9_replacements;
        std::unordered_map<uint64_t, ID3D11ShaderResourceView *> m_d3d11_replacements;

        // `defer` moves the COM references onto m_retired_replacements instead of releasing them
        // straight away. The bind fast path reads a cached replacement pointer without the lock, so
        // releasing under the lock can still free a view a render thread is about to hand the game.
        // Retired views are dropped a couple of frames later, once no bind can still be holding one.
        void release_replacements(bool defer = false);
        std::vector<IUnknown *> m_retired_replacements;
        uint64_t m_retire_after_frame = 0;
        void drain_retired_replacements();

        // Replacement construction. Split out of the register_unmap_* paths so a replacement can
        // also be built later, when a DDS is added while the game is running (the texture is never
        // re-uploaded, so hot reload has to build it after the fact). Both require m_mutex.
        bool build_replacement9(IDirect3DDevice9 *device, uint64_t hash, const std::filesystem::path &inject_path, UINT original_levels, TextureDetails &details);
        bool build_replacement11(ID3D11Device *device, uint64_t hash, const std::filesystem::path &inject_path, UINT original_levels, TextureDetails &details);

        // Hot reload. The bind hooks only FLAG a hash (note_pending_injection, cheap, runs inside
        // the game's draw call); the actual DDS read and texture creation happen in on_frame via
        // process_pending_injections, a couple per frame. Both require m_mutex.
        void note_pending_injection(uint64_t hash, bool is_dx11);
        void process_pending_injections();

        std::unordered_map<uint64_t, bool> m_pending_injections; // hash -> is_dx11

        // Hashes whose inject file failed to load/create, so we do not retry a broken file every
        // frame. Cleared by rescan_injected.
        std::unordered_set<uint64_t> m_failed_injections;

        // Live original-texture preview (see set_preview_target). We hold one COM reference.
        uint64_t m_preview_target_hash = 0;
        IDirect3DBaseTexture9 *m_preview_tex9 = nullptr;
        ID3D11ShaderResourceView *m_preview_srv11 = nullptr;

        // Dumped-file preview, loaded on demand (see get_file_preview_handle).
        uint64_t m_file_preview_hash = 0;
        IDirect3DBaseTexture9 *m_file_preview_tex9 = nullptr;
        ID3D11ShaderResourceView *m_file_preview_srv11 = nullptr;

        void release_preview();

        // Background dump worker
        struct DumpRequest
        {
            uint64_t hash = 0;
            UINT width = 0;
            UINT height = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::vector<std::vector<uint8_t>> levels; // tightly packed, level 0 first
        };

        std::vector<DumpRequest> m_dump_queue;
        std::thread m_dump_thread;
        std::condition_variable m_dump_cv;
        std::mutex m_dump_mutex;
        bool m_dump_thread_running = false;

        void dump_worker_loop();

        // Writes a full mip chain (tightly-packed data per level), so a dump can be edited and
        // injected straight back without losing its mips.
        // `levels` is slice-major: mip_levels entries per array slice (cubemaps are six slices).
        std::string write_dump_dds_mips(uint64_t hash, UINT width, UINT height, DXGI_FORMAT format,
                                        const std::vector<std::vector<uint8_t>> &levels,
                                        UINT array_size = 1);

        // GPU readback + write for one live resource. Return the file path or empty.
        std::string dump_resource11(uint64_t hash, ID3D11Resource *res);
        std::string dump_base_texture9(uint64_t hash, IDirect3DBaseTexture9 *base);

        // Bulk-dump plumbing (see dump_all). m_pending_dumps holds hashes waiting to be
        // drawn; when drawn we take a reference into m_readback_queue and drain it a few per
        // frame in on_frame. Caller of process_readback_queue MUST hold m_mutex.
        struct PendingReadback
        {
            uint64_t hash = 0;
            IDirect3DBaseTexture9 *tex9 = nullptr;
            ID3D11ShaderResourceView *srv11 = nullptr;
        };
        std::unordered_set<uint64_t> m_pending_dumps;
        std::vector<PendingReadback> m_readback_queue;
        void process_readback_queue();

        // Drops long-unseen tracked textures so the map does not grow without bound over a
        // long session. Caller MUST hold m_mutex.
        void evict_stale_textures(uint64_t now_ticks);

        // Resolves the file for a hash: our own 16-hex name first, then Special K's 8-hex top-CRC
        // name when AcceptSpecialKNames is on. via_sk_name reports which naming matched.
        std::filesystem::path find_injection_path(uint64_t hash, uint32_t sk_hash = 0, bool *via_sk_name = nullptr);
    };
}
