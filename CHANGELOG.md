# Changelog

All changes below are relative to [BadassBaboon/Texture-Toolkit](https://github.com/BadassBaboon/Texture-Toolkit)
at commit `796c446`.

> Written for **Kingdoms of Amalur: Re-Reckoning** (32-bit, Direct3D 11) and not tested on any other
> game yet. The Direct3D 9 path was kept working and follows the same design, but no Direct3D 9
> title has been run against it since the rework.

Much of what follows is inspired by [Special K](https://github.com/SpecialKO/SpecialK).

## Replacement at texture creation

- Upstream substitutes at draw time: a couple of thousand binds a frame, each resolving a hash and
  looking for a replacement, for a decision that cannot change. Replacement now happens in
  `CreateTexture2D`, where the pixels are known exactly once, before the original reaches the GPU.
- Work that repeated per bind per frame now runs once per texture. It shows worst where the same
  art is redrawn constantly: a menu went from single-digit fps to the frame cap.
- Bound resources are still tracked for the panel, but the hash is memoised on the shader resource
  view rather than resolved through COM calls and a shared lock on every bind.
- Building a replacement -- DDS read, texture create -- moved off the render thread, where it was a
  visible hitch during a level load.

## Hashing

- CRC-32C on the CPU's own instruction where available, roughly an order of magnitude faster than
  the byte-at-a-time loop it replaces. The hash is taken inside the game's own `CreateTexture2D`,
  so a 33 MB texture arriving during streaming used to stall that frame outright.
- It is the value Special K names its packs with, so an SK pack loads here unchanged -- and every
  file is named differently than upstream.

## Fixes

- **The cursor vanished after closing the panel.** The overlay fought the OS cursor counter instead
  of letting the ImGui backend handle `WM_SETCURSOR`, and each open/close cycle made it worse.
- **Replacement did nothing with the overlay disabled**, because the graphics device was only ever
  captured while initialising the overlay.
- **`CreateDXGIFactory2` was not hooked** -- what a modern game actually calls. Missing it sent the
  tool down a fallback that created a throwaway device and swapchain, visible to other overlays.
- **Legacy 32-bit DDS files ignored their channel masks**, so every BGRA file loaded with red and
  blue swapped.
- **Dumps now carry every mip level and array slice**, rather than the top level alone.
- **A texture showed as dumped the moment the job was queued**, before the writer had written
  anything, and as injected whenever a DDS merely sat in `inject/` -- which may be corrupt, refused
  by the device, or ignored with injection off. Both follow what actually happened now.
- **Re-tracking the same content wiped what was known about the hash**, losing the dump an earlier
  instance had written.
- **The inject scan held the manager lock** while walking the directory, and walked it with no
  error handling at all, so a directory error threw out into whatever game call it was made from.
- **Two files naming one hash** (`5D3E2CCE.dds`, `0x5d3e2cce.dds`) resolved by directory order;
  the plain form now wins, alphabetically among equals.
- **The file preview cached its key before loading**, so a DDS that was half-written when the panel
  first looked at it was never retried; the key now includes the file's timestamp, so a replacement
  edited in place is picked up too.
- **D3D9 lock pitches are signed and were taken as unsigned**, so a negative one became an enormous
  length instead of a refused lock -- in one place a write past the end of a mapped rectangle.

## Options

- **`TrackMapUnmap`** (on): covers games that create empty textures and fill them afterwards. The
  one expensive thing left -- every `Unmap` hashes the whole surface -- so a game that uploads
  through `CreateTexture2D` can switch the detour off entirely.
- **`EnableOverlay`** (on): off leaves a texture replacer and nothing else, with none of the
  process-wide input hooks a panel needs.

## Dependencies and build

- DirectXTex replaces the hand-written DDS handling: parsing, channel masks, mip generation, DDS
  writing. It also builds mip chains for block-compressed formats, which the previous code
  declined to do.
- The ReShade dependency is gone -- one enum and two pitch functions, for a large checkout beside
  this one.
- CMake fetches what it needs, so a bare clone builds; `vcpkg.json` lists the same dependencies for
  anyone who would rather supply them.

## Diagnostics

- Behind `TT_DIAGNOSTICS` and off by default: periodic timings per hook, and a line for every frame
  slower than 20 ms. The per-frame lines are the useful half -- a three-second average barely moves
  for a hitch the player sees plainly.

## Modernisation

- C++20: `std::jthread` for the workers, `Microsoft::WRL::ComPtr` for owned interfaces,
  compile-time hash tables.
- Sources laid out by subsystem -- `core`, `input`, `render/{d3d9,d3d11,dxgi}`, `texture`, `ui` --
  with the texture manager split per graphics API.
