#pragma once

#include "texture/texture_manager.h"

namespace TextureToolkit
{
    class D3D11TextureManager : public TextureManagerBase
    {
    public:
        static D3D11TextureManager &get();

        // Hands the game a replacement instead of the texture it asked for, and rewrites its own
        // descriptor to match: the engine builds its shader resource view from that same struct.
        ID3D11Texture2D *create_replacement_texture11(ID3D11Device *device,
                                                      D3D11_TEXTURE2D_DESC &desc,
                                                      const D3D11_SUBRESOURCE_DATA &initial_data);

        // initial_data/mip_levels come from CreateTexture2D: the one moment every mip level is in
        // hand, so an auto-dump taken there matches what the game loaded.
        void register_texture11(ID3D11Device *device, ID3D11Resource *resource, const void *pixel_data,
                                UINT width, UINT height, DXGI_FORMAT format, UINT pitch,
                                const D3D11_SUBRESOURCE_DATA *initial_data = nullptr, UINT mip_levels = 1);
        uint32_t note_referenced(void *resource);   // 0 when the pointer is not one of ours
        void pin_preview_view(ID3D11ShaderResourceView *view);
        void note_dump_candidate(ID3D11ShaderResourceView *view, uint32_t hash);
        void register_owned_view(void *view, uint32_t hash);
        bool owns_resource(ID3D11Resource *resource) const;
        uint32_t resource_hash(ID3D11Resource *resource) const;   // 0 when the resource is not ours
        size_t refresh_injected_contents();
        void insert_owned(void *key, uint32_t hash);
        void insert_owned_locked(void *key, uint32_t hash);
        void reset_owned();
        void grow_locked(size_t capacity, bool carry_over);
        std::string dump_resource11(uint32_t hash, ID3D11Resource *res);

    protected:
        size_t refresh_branch() override { return refresh_injected_contents(); }
        void collect_scene_hashes(uint64_t now, std::unordered_set<uint32_t> &out) override;
        uint64_t branch_file_preview_handle() const override;
        std::string dump_readback(const PendingReadback &rb) override;
        std::string dump_selected(uint32_t hash) override;
        uint64_t branch_preview_handle() const override;
        uint64_t upload_file_preview(const DirectX::Image &image) override;
        void release_branch_replacements() override;
        void release_branch_preview() override;
        void release_branch_file_preview() override;

    private:
        struct D3D11State
        {
            // Replacements already built. Games recreate the same texture often, and without this
            // every one would re-read the DDS from disk inside the game's own CreateTexture2D.
            std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<ID3D11Texture2D>> injected;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preview;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> file_preview;

            // Pointer -> hash table for everything of ours the render thread might name: the injected
            // textures and the views onto them. Open addressing over a fixed allocation, so a reader
            // never sees a half-grown table; growth publishes a new one and retires the old.
            struct OwnedSet
            {
                std::unique_ptr<std::atomic<void *>[]> slots;
                std::unique_ptr<uint32_t[]> hashes;
                std::unique_ptr<std::atomic<uint64_t>[]> last_used;
                size_t mask = 0;
                size_t count = 0;
            };
            std::unordered_map<void *, uint32_t> views;
            std::atomic<const OwnedSet *> snapshot{nullptr};
            std::vector<std::unique_ptr<OwnedSet>> history;

            // The table has a lock of its own because it is written from inside CreateShaderResourceView,
            // which can fire while this thread already holds m_mutex -- creating a preview does exactly
            // that. Order is always m_mutex then this one; nothing takes them the other way round.
            std::mutex mutex;
        };
        D3D11State m_d3d11;
    };
}
