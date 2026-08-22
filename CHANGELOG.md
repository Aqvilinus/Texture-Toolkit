# Changelog

All changes below are relative to [BadassBaboon/Texture-Toolkit](https://github.com/BadassBaboon/Texture-Toolkit)
at commit `796c446`.

> These changes were written for **Kingdoms of Amalur: Re-Reckoning** (32-bit, Direct3D 11) and have
> not been tested on any other game yet. The Direct3D 9 path was kept working and follows the same
> design, but no Direct3D 9 title has been run against it since the rework.

Much of what follows is owed to [Special K](https://github.com/SpecialKO/SpecialK), which solved
these problems first and in the open: replacing at creation time rather than at draw time, naming
textures by CRC-32C, scoping the re-entrancy guards per thread, leaving D3D9 file loading to D3DX.
Its source was read closely and its reasoning argued with; the code here is our own, the approaches
are theirs.

## Replacement moved to texture creation

Upstream substitutes textures at draw time. That is the hottest hook there is: a busy frame binds
a couple of thousand textures, and each bind had to resolve a hash and look for a replacement --
several thousand times a second, forever, for a decision that cannot change. A texture's pixels are
known exactly once, when the game creates it.

Replacement now happens there, in `CreateTexture2D`, before the original ever reaches the GPU, and
the draw-time substitution is gone. The saving is not a percentage but a change of scale: work that
repeated per bind per frame now runs once per texture in the life of the process. It shows worst
where the same art is drawn over and over -- a menu that redraws its icons every frame went from
single-digit fps to the frame cap.

The panel still has to know what is on screen, so bound resources are tracked. Tracking is a
fraction of the old cost, and the answer is memoised on the shader resource view itself instead of
resolved through COM calls and a shared lock on every bind.

Building a replacement -- reading a DDS from disk, creating a texture -- was also moved off the
render thread. Neither belongs inside a frame: disk and driver take as long as they take, and
during a level load that is a hitch the player sees, not a cost an average can absorb.

## Hashing

Hashes are CRC-32C, computed with the CPU's own instruction where available -- roughly an order of
magnitude faster than the byte-at-a-time loop it replaces. That matters because the hash is taken
inside the game's own `CreateTexture2D` call: a 33 MB texture arriving during level streaming used
to stall that frame outright, with the game waiting the whole time.

It is also the value Special K names its texture packs with, so an SK pack loads here unchanged --
and it renames every file compared to upstream.

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

## Options

`TrackMapUnmap` and `EnableOverlay` were added, both on by default. The first covers games that
create empty textures and fill them afterwards, and it is the one genuinely expensive thing left:
every `Unmap` hashes the whole surface, so a game whose menu rewrites a large dynamic texture each
frame pays for it every frame -- easily more than everything else the tool does put together. Games
that upload through `CreateTexture2D` can switch the detour off entirely. The second, turned off, leaves a texture replacer and nothing
else -- no panel, and none of the process-wide input hooks that a panel needs.

## Dependencies and build

DirectXTex replaces the hand-written DDS handling: parsing, channel masks, mip generation and DDS
writing. It also builds mip chains for block-compressed formats, which the previous code declined
to do.

The ReShade dependency is gone. It was used in a single file, for one enum and two pitch
functions, and required a large checkout beside this one.

CMake fetches what it needs, so a bare clone builds; `vcpkg.json` lists the same dependencies for
anyone who would rather supply them.

## Diagnostics

Behind `TT_DIAGNOSTICS` and off by default: periodic timings per hook, and a line for every frame
slower than 20 ms. The second matters more than it sounds -- an average over three seconds barely
moves for a hitch that the player sees plainly, and several wrong diagnoses in this codebase were
argued at length before per-frame lines named the real one.

## Modernisation

C++20 throughout. `std::jthread` for the workers, `Microsoft::WRL::ComPtr` for owned interfaces,
compile-time hash tables, and sources laid out by subsystem -- `core`, `input`,
`render/{d3d9,d3d11,dxgi}`, `texture`, `ui` -- with the texture manager split per graphics API.
