# Changelog

All changes below are relative to [BadassBaboon/Texture-Toolkit](https://github.com/BadassBaboon/Texture-Toolkit)
at commit `796c446`.

> Written for **Kingdoms of Amalur: Re-Reckoning** (32-bit, Direct3D 11) and not tested on any other
> game yet. The Direct3D 9 path was kept working, but it still substitutes at bind time the way
> upstream did; only Direct3D 11 moved to creation-time replacement. No Direct3D 9 title has been
> run against this fork.

## Replacement at texture creation (Direct3D 11)

- Upstream substituted at bind time: every texture of every draw call resolved a hash and looked up
  a replacement. Replacement now happens in `CreateTexture2D`, the one moment a texture's pixels
  are in hand and nothing has drawn with them, and binds cost a lock-free pointer lookup. A menu
  that redraws the same art every frame went from single-digit fps to the frame cap.
- The shader resource view the game asks for is corrected to the replacement's format and mip
  count, keeping the game's sRGB intent rather than the file's.
- A file that turns up for a texture the game has already created is applied by putting a view of
  our own in front of it at bind time, leaving the original untouched -- which is what makes it
  work at all, since game art is usually created immutable and cannot be copied into. Size, format
  and mip count are free to differ. It is built one per frame off the draw call, and costs an
  atomic load in a table lookup the bind hook was already doing.

## What that design gives up

- **On Direct3D 11, a texture is replaced at creation only if it arrives with its pixels** -- as a
  shader resource, with default or immutable usage. One created empty and filled through
  `Map`/`Unmap` misses that moment; Reload still reaches it, and the game rewriting it takes the
  replacement back off.
- **Only the pixel, vertex and compute stages are watched.** A texture bound to no other stage than
  geometry or tessellation never appears at all.
- **D3D9 auto-dump carries the top mip only.** The game delivers one level per `LockRect` and
  nothing says when the chain is complete -- unless the game has `d3dx9_43.dll`, in which case the
  texture comes from D3DX complete and the whole chain is written.

## Hashing

- CRC-32C, on the CPU's own instruction where available, in place of upstream's table-driven
  CRC-32.
- It is the hash Special K names its packs with, so an SK pack drops in unchanged for ordinary 2D
  textures. It is not upstream's value, so an existing `dump/` or `inject/` folder has to be
  renamed.

## D3DX, where the game has it

- `D3DXCreateTextureFromFile*` is watched in whatever `d3dx9_43.dll` the game already loaded, never
  one we load. A texture that arrives that way is complete, so its dump carries every mip level.
- A D3D9 replacement in a format with no `D3DFORMAT` equivalent is built by D3DX instead of being
  refused, and dumps go through `D3DXSaveTextureToFileW`, which also writes formats our own writer
  has to decline.
- None of this happens in a game that does not ship D3DX; those keep the lock-based path, which is
  what covers engines with their own asset formats.

## The panel

- The texture list sorts by hash, size, mips, format or status.
- Reload rescans `TT/inject` and applies what changed, including files added since the game
  started. No restart, whatever the new file's size or format.
- The panel, the startup banner and the log name the key `HotKey` is actually set to, instead of
  saying INSERT regardless.
- On Direct3D 11, with neither panel nor banner on screen, the overlay skips the ImGui frame and
  the back-buffer fetch and render target view that upstream created every presented frame.

## Input

- The exemption that lets our own code read real key state is thread-local and scoped. Upstream
  held one global flag across the whole ImGui build, so any other game thread polling key state
  during that window read through the mask and kept acting on it.
- Coverage, unchanged from upstream and worth knowing: DirectInput 8, the window message queue
  including `WM_INPUT`, and the polled `user32` key-state calls. XInput, HID read directly, and raw
  input read outside the message queue are not.

## Fixes

- **The cursor vanished after closing the panel.** The overlay drove the OS `ShowCursor` counter
  itself and never put it back, and it discarded the ImGui backend's `WM_SETCURSOR` answer, so the
  game re-asserted its own cursor on every pointer move. The backend owns the cursor now.
- **`CreateDXGIFactory2` was not hooked**, so a game that asks for its factory that way was never
  seen creating a swapchain, and the Present hook came from the bootstrap fallback instead: a
  throwaway device and swapchain that ReShade and Special K notice and build against.
- **Legacy 32-bit DDS files loaded with red and blue swapped.** The channel masks were parsed but
  applied only to 24-bit files; a 32-bit `A8R8G8B8` went to the GPU byte-for-byte as RGBA.
- **A texture showed as dumped the moment the job was queued**, before the writer had written
  anything, and as injected whenever a DDS merely sat in `inject/` -- which may be corrupt, refused
  by the device, or ignored with injection off. Both badges now report what the tool actually did.
- **The inject scan ran under the manager lock and could throw out through the game's `Present`**,
  so an unreadable inject folder took the game down on Reload.
- **Two files naming one hash** (`5D3E2CCE.dds`, `0x5d3e2cce.dds`) resolved by directory order, so
  a restart could load the other one. The plain form wins now, filename order settles the rest.
- **The file preview cached its key before loading**, so a DDS still being written when the panel
  first looked at it stayed blank for as long as that texture stayed selected. The key carries the
  file's timestamp now, so a dump rewritten under the same name is picked up too.
- **D3D9 lock pitches are signed and were taken as unsigned**, so a negative pitch became an
  enormous stride instead of a mapping the tool declines to touch.
- **Toggling a checkbox rewrote `TextureToolkit.ini` from scratch**, discarding anything edited by
  hand. Values are written key by key now.
- **An install path over `MAX_PATH` killed the game at startup.**
- **Unload ran in the wrong order**, tearing down the texture managers while their detours were
  still live.

## Options

- **`TrackMapUnmap`** (on): tracks D3D11 textures the game creates empty and fills through
  `Map`/`Unmap`. Every `Unmap` then hashes the whole surface, so a game that uploads through
  `CreateTexture2D` can turn the detour off. The D3D9 lock path, and D3DX creation where the game
  uses it, are always tracked.
- **`EnableOverlay`** (on): off skips the panel and the process-wide input detours it needs,
  leaving a texture replacer and nothing else.

## Dependencies and build

- DirectXTex replaces the hand-written DDS handling: parsing, channel masks, mip generation, DDS
  writing, and the readback that dumps every mip and array slice. Mip synthesis is still
  uncompressed-only -- a block-compressed replacement has to ship its own chain.
- The sibling ReShade checkout is no longer needed. CMake fetches Dear ImGui, MinHook and
  DirectXTex at configure time, pinned to overridable tags, so a bare clone builds, and the `.asi`
  links the static CRT so it needs no Visual C++ redistributable.
- GitHub Actions builds Win32 and x64 on pushes to `main` and on pull requests, and publishes both
  binaries as a release when a version tag is pushed.

## Diagnostics

- Behind `TT_DIAGNOSTICS` and off by default, on the Direct3D 11 path: per-hook timings every three
  seconds, and a line for the first 60 frames over 20 ms.
