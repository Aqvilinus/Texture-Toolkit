# Texture Toolkit

Texture Toolkit is an in-game texture dumping and dynamic texture replacement plugin for 32-bit and 64-bit DirectX 9 and DirectX 11 Windows games. It runs as an `.asi` plugin (loaded via Ultimate ASI Loader) or as a proxy DLL (`dinput8.dll`, `d3d9.dll`, or `dxgi.dll`).

## Features

- **DirectX 9 & DirectX 11 Interception**: Intercepts texture creation and surface locks (`IDirect3DTexture9`, `IDirect3DSurface9`, and `ID3D11Device::CreateTexture2D`).
- **CRC32 Hashing**: Computes unique pixel-data CRC32 hashes for active scene textures.
- **Dynamic Texture Injection**: Replaces textures in memory on the fly when replacement files (`.dds` or `.png`) are placed in the `TT/inject` directory without restarting the game.
- **DDS & PNG Support**: Supports raw GPU-compressed formats (BC1 through BC7) and standard `.png` images.
- **ImGui Control Panel**: In-game overlay toggled with the `INSERT` key to inspect tracked scene textures, filter by hash or resolution, search, copy hex hashes to the clipboard, and trigger manual texture dumps.
- **Input Isolation**: Freezes game keyboard and mouse inputs while the menu is open to prevent accidental character movement or menu clicks in-game.

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
4. When textures load, Texture Toolkit scans `TT/inject/` for replacement files named after the texture's hex hash (e.g., `0x5D3E2CCE.dds`, `5D3E2CCE.png`).
5. Dumped textures are saved asynchronously into `TT/dump/`.

## Configuration

A `TextureToolkit.ini` configuration file is automatically created in `TT/` on first run:

```ini
[Settings]
AutoDump=0
EnableInjection=1
FilterSmallTextures=1
DumpFormat=0
LoadFormat=0
```

- `AutoDump`: Set to `1` to dump every loaded scene texture automatically.
- `EnableInjection`: Set to `1` to enable live texture replacements.
- `FilterSmallTextures`: Set to `1` to ignore textures smaller than 16x16 pixels.

## License

MIT License.
