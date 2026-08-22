<p align="center">
  <img src="texture-toolkit.png" alt="Texture Toolkit" width="640">
</p>

> **Fork of [BadassBaboon/Texture-Toolkit](https://github.com/BadassBaboon/Texture-Toolkit).** Same tool. What is different:
>
> - Textures are replaced when the game creates them, not when it draws them.
> - Hashes are CRC-32C, the value Special K names its packs with, so an SK pack loads here unchanged -- and every dump is named differently than upstream.
> - Dumps carry every mip level and array slice.
>
> Many of these approaches are borrowed from [Special K](https://github.com/SpecialKO/SpecialK), which solved the same problems first. Written for Kingdoms of Amalur: Re-Reckoning and not tested on any other game yet; [CHANGELOG.md](CHANGELOG.md) has the rest.

Texture Toolkit dumps and replaces textures at runtime in 32-bit and 64-bit Direct3D 9 and Direct3D 11 games on Windows. It loads as an `.asi` plugin through Ultimate ASI Loader, or as a proxy DLL renamed to `dinput8.dll`, `d3d9.dll`, or `dxgi.dll`. An in-game panel lists the textures in the current scene, shows their format and memory size, and lets you dump or replace them without restarting. It has been tested against Bully: Scholarship Edition (Direct3D 9), Grand Theft Auto IV: Complete Edition (Direct3D 9), Total Overdose (Direct3D 9), Spec Ops: The Line (Direct3D 11), and Need for Speed: The Run (Direct3D 11).

## How it works

Texture Toolkit hooks the calls that create and upload textures: `LockRect`/`UnlockRect` on D3D9, and `Map`/`Unmap` plus `CreateTexture2D` on D3D11. When a texture's pixels are uploaded it computes a CRC-32C over that data and writes the hash onto the resource as D3D private data. On D3D11 the pixels arrive with the creation call, so a replacement is substituted there and the original never reaches the GPU. On D3D9 the texture is filled after it is created, so the hash is read back when the game binds it with `SetTexture` and the replacement takes its place before the draw.

Storing the hash on the resource, instead of tracking raw pointers, keeps a replacement attached to the right texture after the driver frees an address and reuses it for something else. On D3D9 the tool also follows `UpdateTexture`, so art that the game loads into a `SYSTEMMEM` texture and copies into a `DEFAULT`-pool texture is matched by the copy the game actually renders.

## Features

- Direct3D 9 and Direct3D 11, both x86 and x64.
- DDS replacement: put `<hash>.dds` in `TT/inject` and it loads without a restart.
- Mip handling: a replacement is created with the original texture's mip count. Missing mips are generated for uncompressed formats; a compressed replacement without a full chain loads at its top level and logs a warning.
- Dumping to `TT/dump` as `.dds`: automatically on load, one at a time from the panel, or every tracked texture at once.
- CRC-32C content hashing, so two identical textures share one hash and one replacement, and Special K texture packs load unchanged.
- Input isolation and a software cursor, so the game stops reading the mouse and keyboard while the panel is open.

## The in-game panel

Press `INSERT` to open it. The left pane lists tracked textures with hash, size, mip count, format, and status (original, dumped, or injected). The filter box matches on hash, dimensions, or format. Hover the list and press `[` or `]` to step through it.

The right pane inspects the selected texture. The preview shows the injected replacement, the live original while it is on screen, or the dumped `.dds` read back from disk. Below it are the dimensions, mip count, data size, format, the compressed and sRGB flags, and the D3D11 bind, usage, and misc flags. You can copy the hash or dump the texture from here, and drag the divider to resize the two panes.

## Building

Requires CMake 3.20 or newer and a Visual Studio toolchain with the Windows SDK. Dependencies (Dear ImGui, MinHook, DirectXTex) are fetched at configure time, so a bare clone builds. `vcpkg.json` lists the same three for anyone who would rather supply them.

### 32-bit (x86)

```cmd
cmake -B build32 -A Win32
cmake --build build32 --config Release
```

Output: `build32/Release/TextureToolkit.asi`.

### 64-bit (x64)

```cmd
cmake -B build64 -A x64
cmake --build build64 --config Release
```

Output: `build64/Release/TextureToolkit.asi`.

Match the build to the game: a 32-bit game needs the x86 build.

## Installing

1. Copy `TextureToolkit.asi` into the game folder, or into a `plugins/` or `scripts/` folder when using Ultimate ASI Loader. To load it as a proxy instead, rename it to `dinput8.dll`, `d3d9.dll`, or `dxgi.dll`.
2. Launch the game. Texture Toolkit writes `TextureToolkit.ini` and its log next to the `.asi`, and creates a `TT/` folder next to the executable containing `dump/`, `inject/`, and `imgui.ini`.
3. Press `INSERT` to open the panel.

To replace a texture, read its hash from the panel (or dump it first), edit the `.dds`, and place it in `TT/inject` named after the hash, for example `5D3E2CCE.dds` or `0x5D3E2CCE.dds`. Export it with a full mip chain if it is block-compressed. The `TT` folder name can be changed with `ResourceRoot` in the ini.

## Configuration

`TextureToolkit.ini` is created next to the `.asi` on first run:

```ini
[TextureToolkit]
HotKey=0x2D
ResourceRoot=TT
EnableInjection=1
AutoDump=0
FilterSmallTextures=1
ShowCurrentFrameOnly=1
TrackMapUnmap=1
EnableOverlay=1
ShowOSDBanner=1
Verbose=0
```

- `HotKey`: virtual-key code that toggles the panel (`0x2D` INSERT, `0x24` HOME, `0x74` F5, `0x79` F10).
- `ResourceRoot`: folder holding `dump/`, `inject/`, and `imgui.ini`; relative to the game folder, or an absolute path.
- `EnableInjection`: load replacements from the `inject/` folder.
- `AutoDump`: dump every texture to the `dump/` folder as it loads.
- `FilterSmallTextures`: ignore textures under 16x16.
- `ShowCurrentFrameOnly`: list only textures drawn in the current scene.
- `TrackMapUnmap`: track textures the game fills through `Map`/`Unmap` instead of handing the pixels to `CreateTexture2D`. Every `Unmap` then costs a hash over the whole surface, so turn it off if the log shows textures arriving through `CreateTexture2D`.
- `EnableOverlay`: the panel and the process-wide input hooks it needs. Off leaves texture replacement and nothing else.
- `ShowOSDBanner`: show the startup banner.
- `Verbose`: write per-texture debug lines to the log; leave off for normal use, since it slows the game.

Toggling a checkbox in the panel writes its new value back to this file.

## Limitations

- Direct3D 9 and Direct3D 11 only. DirectX 8, 10, 12, and Vulkan are not hooked.
- A DirectX 8 game run through a `d3d8to9` wrapper renders as Direct3D 9, so the overlay appears, but its textures stay invisible. The wrapper feeds pixel data into the D3D9 textures through an internal path that never calls a `LockRect`, `UpdateSurface`, `UpdateTexture`, or `StretchRect` we can hook, so there is nothing to hash. Capturing those would require hooking Direct3D 8 directly, which is not implemented. With `Verbose=1` the log fills with `Hooked_CreateTexture` lines and never a `Tracked` line.
- Injection reads `.dds` only. Dumps are written as `.dds`.
- A D3D9 texture in the default pool cannot be read back with `LockRect`, so the panel's Dump button fails on those; Auto-dump captures them from the upload instead.
- A compressed replacement without a full mip chain samples its top level at every distance, which aliases in motion. The missing mips cannot be regenerated from compressed data, so export those with mipmaps.

## License

MIT. See [LICENSE](LICENSE). Copyright for the original work remains with BadassBaboon; the changes in this fork are offered under the same licence.
