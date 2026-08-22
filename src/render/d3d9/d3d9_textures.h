#pragma once

#include "texture/texture_manager.h"

namespace TextureToolkit
{
    // Draw-time substitution: D3D9 has no initial data at creation, so a replacement can only be
    // handed over when the game binds the texture.
    class D3D9TextureManager : public TextureManagerBase
    {
    public:
        static D3D9TextureManager &get();

        IDirect3DBaseTexture9 *get_replacement_texture9(IDirect3DBaseTexture9 *orig);
        void register_texture9(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch);
        void copy_tag9(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst);
        bool build_replacement9(IDirect3DDevice9 *device, uint32_t hash, const std::filesystem::path &inject_path, UINT original_levels, TextureDetails &details);
        std::string dump_base_texture9(uint32_t hash, IDirect3DBaseTexture9 *base);

    protected:
        size_t refresh_branch() override;
        void process_branch_injections() override;
        uint64_t branch_file_preview_handle() const override;
        std::string dump_readback(const PendingReadback &rb) override;
        std::string dump_selected(uint32_t hash) override;
        uint64_t branch_preview_handle() const override;
        uint64_t upload_file_preview(const DirectX::Image &image) override;
        void release_branch_replacements() override;
        bool branch_has_replacement(uint32_t hash) const override { return m_d3d9.replacements.contains(hash); }
        void release_branch_preview() override;
        void release_branch_file_preview() override;

    private:
        void note_pending_injection(uint32_t hash, bool is_dx11);
        void process_pending_injections();

        struct D3D9State
        {
            // Keyed by content hash: a reused resource pointer can never inherit a stale replacement.
            std::unordered_map<uint32_t, Microsoft::WRL::ComPtr<IDirect3DBaseTexture9>> replacements;
            Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> preview;
            Microsoft::WRL::ComPtr<IDirect3DBaseTexture9> file_preview;
        };
        D3D9State m_d3d9;
    };
}
