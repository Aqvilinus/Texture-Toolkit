# Changelog

All changes below are relative to [BadassBaboon/Texture-Toolkit](https://github.com/BadassBaboon/Texture-Toolkit)
at commit `796c446`.

> Written for **Kingdoms of Amalur: Re-Reckoning** (32-bit, Direct3D 11) and not tested on any other
> game yet. The Direct3D 9 path was kept working and follows the same design, but no Direct3D 9
> title has been run against it since the rework.

Much of what follows is inspired by [Special K](https://github.com/SpecialKO/SpecialK).

## Replacement at texture creation

- Upstream substituted at bind time: every texture of every draw call resolved a hash and looked up
  a replacement. Replacement now happens in `CreateTexture2D`, the one moment a texture's pixels
  are in hand and nothing has drawn with them; the bind hook is left with recording what is on
  screen. A menu that redraws the same art every frame went from single-digit fps to the frame cap.
- That recording resolves a texture through a lock-free pointer table published once per rebuild,
  rather than COM calls and a shared lock on every bind.
- The shader resource view the game asks for is corrected to the replacement's format and mip
  count, keeping the game's sRGB intent. Upstream built its own view from the file's format, so a
  non-sRGB replacement over an sRGB-sampled texture came out gamma-wrong.

## What that design gives up

- **Only textures created with their pixels can be replaced.** A game that creates an empty texture
  and fills it through `Map`/`Unmap` is tracked and dumped, but not replaced; upstream, deciding at
  bind time, replaced those too.
- **A DDS added while the game runs reaches only hashes that already have a replacement** -- Reload
  copies the new contents into the textures the game is holding. A hash that had no replacement
  yet, or a file whose size, format or mip count changed, needs a restart.
- **D3D9 auto-dump carries the top mip only.** The game delivers one level per `LockRect` and
  nothing says when the chain is complete, so only level 0 is kept; dumping from the panel walks
  the texture on the device and gets every level.

## Hashing

- CRC-32C, on the CPU's own instruction where available, in place of upstream's table-driven
  CRC-32. The hash runs inside the game's own `CreateTexture2D`, so its cost lands in that frame:
  a 33 MB texture arriving during streaming used to stall the frame outright.
- It is the hash Special K names its packs with, so an SK pack drops in unchanged for ordinary 2D
  textures. It is not upstream's value, so an existing `dump/` or `inject/` folder has to be
  renamed.

## The panel

- The texture list sorts by hash, size, mips, format or status.
- Reload refreshes the textures the game already holds, in place, and reports how many it reached.
- The header counts changed meaning: *dumped* counts every texture with a file on disk and overlaps
  the other two, while *injected* and *original* split the whole list.
- The panel, the startup banner and the log name the key `HotKey` is actually set to, instead of
  saying INSERT regardless.
- With neither panel nor banner on screen the overlay does nothing at all. Upstream built an ImGui
  frame, fetched the back buffer and created a render target view every presented frame to draw an
  empty list into.

## Input

- The exemption that lets our own code read real key state is thread-local and scoped. Upstream
  held one global flag across the whole ImGui build, so any other game thread polling key state
  during that window read through the mask and kept acting on it.
- Unchanged from upstream, and worth knowing: only DirectInput 8 and the window message queue are
  covered. A game reading Raw Input, XInput or HID directly keeps receiving input while the panel
  is open.

## Fixes

- **The cursor vanished after closing the panel.** The overlay drove the OS `ShowCursor` counter
  itself and never put it back, and it discarded the ImGui backend's `WM_SETCURSOR` answer, so the
  game re-asserted its own cursor on every pointer move. The backend owns the cursor now.
- **`CreateDXGIFactory2` was not hooked** -- what anything modern actually calls. Missing it meant
  the game's swapchain was never seen, so the Present hook came from the bootstrap fallback
  instead: a throwaway device and swapchain that ReShade and Special K notice and build against.
- **Legacy 32-bit DDS files loaded with red and blue swapped.** The channel masks were parsed but
  applied only to 24-bit files; a 32-bit `A8R8G8B8` went to the GPU byte-for-byte as RGBA.
- **A texture showed as dumped the moment the job was queued**, before the writer had written
  anything, and as injected whenever a DDS merely sat in `inject/` -- which may be corrupt, refused
  by the device, or ignored with injection off. Both badges now report what the tool actually did.
- **Re-tracking the same content dropped the dump path**, so a texture the game uploaded twice
  forgot the `.dds` an earlier upload had already written.
- **The inject scan held the manager lock** for the whole directory walk and used the throwing
  filesystem calls, so an unreadable inject folder unwound out through the game's `Present` -- the
  Reload button runs inside it. The scan happens off the lock now and reports through `error_code`.
- **Two files naming one hash** (`5D3E2CCE.dds`, `0x5d3e2cce.dds`) resolved by directory order, so
  a restart could load the other one. The plain form wins now, filename order settles the rest.
- **The file preview cached its key before loading**, so a DDS still being written when the panel
  first looked at it stayed blank for as long as that texture stayed selected. The key is committed
  on a successful upload now, and carries the file's timestamp, so a dump rewritten under the same
  name is picked up.
- **D3D9 lock pitches are signed and were taken as unsigned**, turning a negative one into a huge
  stride: an out-of-bounds read on the hashing path, and a write past the end of the rectangle on
  both upload paths.
- **Toggling a checkbox rewrote `TextureToolkit.ini` from scratch**, discarding anything edited by
  hand. Values are written key by key now.
- **An install path over `MAX_PATH` aborted the game at startup** on a buffer copy whose overflow
  path terminates the process.
- **Unload ran in the wrong order**, tearing down the texture managers while their detours were
  still live. Detours come down first.

## Options

- **`TrackMapUnmap`** (on): tracks D3D11 textures the game creates empty and fills through
  `Map`/`Unmap`. Every `Unmap` then hashes the whole surface, so a game that uploads through
  `CreateTexture2D` can turn the detour off -- it is never installed, rather than returning early.
  The D3D9 lock path is that branch's only source of textures and is always tracked.
- **`EnableOverlay`** (on): off skips the panel and the process-wide input detours it needs
  (`PeekMessage`, `GetMessage`, `GetKeyState`, `ClipCursor`, the window procedure), leaving a
  texture replacer and nothing else.

## Dependencies and build

- DirectXTex replaces the hand-written DDS handling: parsing, channel masks, mip generation, DDS
  writing, and the readback that dumps every mip and array slice. Mip synthesis is still
  uncompressed-only -- a block-compressed replacement has to ship its own chain.
- The sibling ReShade checkout is gone. It supplied MinHook, Dear ImGui and stb out of
  `../reshade/deps`, plus one header for a format enum, two pitch helpers and two descriptor
  structs.
- CMake fetches Dear ImGui, MinHook and DirectXTex at configure time, pinned to overridable tags,
  so a bare clone builds. The `.asi` links the static CRT and needs no Visual C++ redistributable.
- GitHub Actions builds Win32 and x64 on every push, and publishes both binaries as a release when
  a version tag is pushed.

## Diagnostics

- Behind `TT_DIAGNOSTICS` and off by default, on the D3D11 path: per-hook timings every three
  seconds, and a line for every frame over 20 ms. The per-frame lines are the useful half -- a
  30 ms hitch moves a three-second average by 0.07 ms and is exactly what the player sees.

## Layout

- Sources by subsystem -- `core`, `input`, `render/{d3d9,d3d11,dxgi}`, `texture`, `ui` -- with the
  texture manager split into a shared base and a per-API implementation, in place of one flat
  folder and one 1,400-line manager.
- `std::jthread` and a stop token for the dump writer, `Microsoft::WRL::ComPtr` for owned
  interfaces (upstream had none), and a CRC table built by `constexpr` and checked against the
  published CRC-32C value at compile time instead of 256 pasted literals.
