# Texture Toolkit

Texture Toolkit is an in-game texture dumping and dynamic texture replacement plugin for 32-bit and 64-bit DirectX 9 and DirectX 11 Windows games. It runs as an `.asi` plugin (loaded via Ultimate ASI Loader) or as a proxy DLL (`dinput8.dll`, `d3d9.dll`, or `dxgi.dll`).

## Features

- **DirectX 9 & DirectX 11 Interception**: Intercepts texture creation and surface locks (`IDirect3DTexture9`, `IDirect3DSurface9`, and `ID3D11Device::CreateTexture2D`).
- **CRC32 Hashing**: Computes unique pixel-data CRC32 hashes for active scene textures.
- **Dynamic Texture Injection**: Replaces textures in memory on the fly when `.dds` replacement files are placed in the `TT/inject` directory without restarting the game.
- **DDS-only pipeline**: Injection and export both use `.dds` (raw GPU-compressed BC1–BC7 plus the mapped uncompressed formats). This keeps replacement byte-for-byte faithful to the GPU format and maximises compatibility.
- **Mip-safe injection**: A replacement is created to match the original texture's mip topology. Missing mip levels are auto-generated (box filter) for uncompressed formats; compressed replacements should ship a full mip chain (a warning is logged if they don't).
- **Pointer-reuse-safe replacement**: Replacements are matched by content hash tagged onto the original resource via D3D private data, so driver pointer reuse can never apply a replacement to the wrong texture.
- **ImGui Control Panel**: In-game overlay toggled with the `INSERT` key to inspect tracked scene textures, filter by hash or resolution, search, copy hex hashes to the clipboard, and trigger manual texture dumps.
- **Input Isolation**: Freezes game keyboard and mouse inputs while the menu is open, and draws a software cursor so the pointer stays visible even in fullscreen titles that hide the OS cursor.

## Building

### 32-bit (x86)

```cmd
cmake -B build32 -A Win32
cmake --build build32 --config Release
```

The output binary will be generated at `build32/Release/TextureToolkit.asi`.

### 64-bit (x64)

```cmd
cmake -B build64 -A x64
cmake --build build64 --config Release
```

The output binary will be generated at `build64/Release/TextureToolkit.asi`.

## Usage

1. Copy `TextureToolkit.asi` to your game directory (or inside a `plugins/` or `scripts/` folder if using Ultimate ASI Loader). Alternatively, rename it to `dinput8.dll`, `d3d9.dll`, or `dxgi.dll` for direct proxy loading.
2. Launch the game.
3. Press `INSERT` to open the Texture Toolkit control panel overlay.
4. When textures load, Texture Toolkit scans `TT/inject/` for `.dds` replacement files named after the texture's hex hash (e.g., `0x5D3E2CCE.dds` or `5D3E2CCE.dds`). For best results, export replacements with a full mip chain.
5. Dumped textures are saved asynchronously as `.dds` into `TT/dump/`.

## Configuration

A `TextureToolkit.ini` configuration file is automatically created in `TT/` on first run:

```ini
[TextureToolkit]
HotKey=0x2D
DumpDir=TT/dump
InjectDir=TT/inject
EnableInjection=1
AutoDump=0
FilterSmallTextures=1
ShowCurrentFrameOnly=0
ShowOSDBanner=1
Verbose=0
```

- `HotKey`: Virtual-key code for the UI toggle (`0x2D` = INSERT, `0x24` = HOME, `0x74` = F5).
- `EnableInjection`: Set to `1` to enable live texture replacements.
- `AutoDump`: Set to `1` to dump every loaded scene texture automatically.
- `FilterSmallTextures`: Set to `1` to ignore textures smaller than 16x16 pixels.
- `ShowCurrentFrameOnly`: Set to `1` to list only textures seen in the current scene.
- `ShowOSDBanner`: Set to `1` to show the startup on-screen banner.
- `Verbose`: Set to `1` for chatty per-texture/per-hook debug logging (slow; leave off for normal use).

## License

MIT License.
