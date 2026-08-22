#pragma once

#include <d3d9.h>
#include <dxgi.h>
#include <string>
#include <windows.h>

namespace TextureToolkit
{
    DXGI_FORMAT to_dxgi(D3DFORMAT format);
    D3DFORMAT to_d3d9(DXGI_FORMAT format);
    std::string format_name(D3DFORMAT format);

    // D3D9 surfaces are hashed from the locked rectangle, so the pitch is the runtime's, not ours.
    uint32_t hash_pixels(const void *pixel_data, UINT width, UINT height, D3DFORMAT format, UINT pitch);
}
