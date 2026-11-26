# Design notes

Groundwork for running an Xbox Series game natively on Windows. It's set up now so the work can start
as soon as `game_scarlett_master.exe` is available.

## Ground rules

- Everything here runs from **your own copy** and stays on **your PC**. Game files, generated code, shader caches and
  reports that quote game data are never committed, uploaded or shared. `reports/` is git-ignored for this reason.
- Nothing in this folder decrypts, unpacks or works around Xbox package protection. The tools only read files that
  are already plain on disk.

## Why a loader before a recompiler

The Xbox Series CPU is x86-64, the same as a PC. Unlike Hookshot, where ARM64 code has to be
translated, most of the game's code can run as-is. The work is everything around it:

| Layer | Approach | Folder |
|---|---|---|
| CPU code | Validate and map the PE image, then run it natively. Patch only what can't run on desktop (privileged instructions, console-only intrinsics). | `runtime/src` |
| Imports (GameOS / GDK / Xbox DLLs) | Every imported `dll!function` goes to a shim. Unimplemented ones log once and return a failure, so the missing surface shows up at runtime. | `runtime/shims` |
| Graphics | Xbox D3D12 (`d3d12_xs`) to Vulkan. Shaders ship as compiled RDNA2 machine code, so they need a translator from RDNA2 ISA to SPIR-V. **Largest single piece.** | `tools/rdna2spirv.py`, `runtime/gpu` |
| Assets | `.rpf` archives (RPF9) read through the runtime's file layer. | later |

## Layout

```
tools/       Python analysis tools (no game data inside)
  pe_inspect.py   PE headers, sections, entropy, imports sorted by where they resolve, .pdata, TLS
  rpf_probe.py    RPF header survey of a game folder
  manifest.py     SHA-256 manifest of a game folder, plus a --verify mode
scripts/
  first_look.ps1  run once the exe arrives: the full report set for it
runtime/     C++ native loader (CMake + Ninja, MSVC)
  src/            PE mapper, relocations, import binding, TLS
  shims/          per-DLL shim tables
  gpu/            Vulkan layer for translated shaders: device, GPU heap, descriptor hash table, bindless set
  test/           GPU harnesses and the runtime tests
tests/       translator regression tests on hand-written shaders (no game data)
reports/     generated output (git-ignored)
docs/        notes
```

## Day one, when the exe is here

```powershell
.\scripts\first_look.ps1 -Exe "<game>\game_scarlett_master.exe"
```

This writes `reports/exe/` (JSON plus a readable summary). Check it in this order:

1. **Section entropy.** Sections near 8.0 bits/byte are still encrypted or compressed. If `.text` is one of them,
   nothing else can be done yet.
2. **Import table.** Each DLL is marked `bundled` (in the game folder), `system` (present on this Windows), or
   `shim` (needs a reimplementation). The `shim` list is the real to-do list.
3. **.pdata function count.** This gives function boundaries for patching and later analysis.

Then `runtime\build\FlamingoPort.exe --dry-run <exe>` maps the image and binds every import to the shim table without
running any game code. It reports what it couldn't bind.

The mapper bounds-checks PE headers, sections, relocation targets, imports and exports before accessing them.
Forwarded exports are rejected until forwarding is implemented.

## Running game code (`--run`)

`FlamingoPort.exe --run [--trace-host] <exe> --game-dir <dir>` runs the game the way Windows would start it: DllMain of the
bundled DLLs, the exe's TLS callbacks, then its entry point. Unimplemented imports log once and return 0, and an
unhandled exception prints the code, `module+rva`, registers and the unimplemented imports called so far.
`--trace-host` also logs the first call to every Windows function and shim. It's for finding the next thing to
implement, not for playing. What it took to get the 1.0.2.1 exe through its CRT start-up:

- **Windows maps the images** (`LoadLibraryExW` with `DONT_RESOLVE_DLL_REFERENCES`), falling back to a hand mapping.
  ntdll's own stack walks (`RtlCaptureStackBackTrace`, and one inside `GetModuleHandleW`) ignore
  `RtlAddFunctionTable` tables and crashed walking through hand-mapped game frames.
- **Static TLS.** Mapped this way, a module gets no TLS slot. The exe takes TLS index 0: MSVC compiles EXEs
  assuming index 0 (`/GA`), and the game reads `gs:[0x58][0]` directly in places, so it uses the exe's own
  block (a 16 KiB reserve the runtime never touches; it is built with `/Zc:threadSafeInit-` so its statics use no
  TLS), refilled with the game's template by a TLS callback of the exe on every thread. Before this, those writes landed
  in another module's TLS block and corrupted the process heap. Bundled DLLs get the index of one of the
  `rt_tls_slot<N>.dll` stubs (`src/tls_slot.cpp`): CRT-free DLLs whose whole TLS block is free. A stub needs at least
  one import, or ntdll skips its TLS when it's loaded dynamically, and its template is real data, not zero fill.
- **Virtual drives** (`shims/files.cpp`): every path-taking import (kernel32, the CRT, `std::_Fiopen`) rewrites
  `G:\` (the package: the game folder) and `T:\` (temporary space: `%LOCALAPPDATA%\Flamingo\temp`) to host folders.
  The g: device's own file methods are patched to the same mapping (`src/game_patches.cpp`, 1.0.2.1 only): on the
  console they answer from an index that nothing fills here. With that the exe mounts its 12 archives.
- **Persistent local storage** (`shims/storage.cpp`): `%LOCALAPPDATA%\Flamingo\storage`, mounted on a free drive
  letter (the title gets `X:\`, as on the console) with the quota from `MicrosoftGame.Config` (2048 MiB for
  1.0.2.1). Game saves (XGameSave) come later: the title only asks for them with a signed-in user (XUser).
- **Offline by design** (`shims/net.cpp`): WinHTTP and Winsock fail as an unplugged network would, also when looked
  up with `GetProcAddress`. The game's crash reporter (Backtrace/Breakpad) packs the crash log and a minidump of the
  process (16 MB) and tries to upload them; it must never reach the network. Dumps stay in the `T:` folder.
- **Runtime services**: our own `xgameruntime.dll` (loaded by name as a pseudo handle, so the desktop runtime of
  Gaming Services is never used) with XTaskQueue/XAsync (`shims/xtaskqueue.cpp`); other areas are logged and refused.
- **Graphics: D3D12.X on the PC's D3D12** (decided 2026-11-24; both projects that got furthest with this title,
  VIctory and XCL, do the same). The console API is close to the PC's (root signatures, descriptor heaps, PSOs,
  queues, fences), so the translation is direct and keeps PIX for measuring; shaders go RDNA2 -> DXIL instead of
  SPIR-V. Console objects keep the console header (table, bits, refs) because the title inlines parts of D3D12.X
  and reads them; the device's out-of-line flag sends would-be-inlined calls to the table. Unwritten methods are
  probes that log their slot (`shims/d3d12_xs.cpp`, `shims/d3d12x.cpp`). Written so far: device creation
  (ID3D12Device11, version 0x20004), command queues (8, and 48 CreateCommandQueueX), fences (36), custom heap
  properties (26). Note for NVIDIA GPUs: they only run 32-lane waves, and console shaders use 64.
  Then: command allocators (9), command lists (12, and 73 CreateCommandListX: params `{ type @0; node mask @8 }`,
  allocator, initial pipeline) with GetType / Close / Reset (8, 9, 10 and 139); the device's QueryInterface (itself
  for every device level, IDXGIDevice whose GetAdapter is the host adapter); descriptor increments (15: views 32,
  samplers 16, as the console's; RTV / DSV 32 is a guess). 1.0.2.1 makes 581 command lists at start.
- **Frame clock** (`shims/frame.cpp`, device 78-81): SetFrameIntervalX / ScheduleFrameEventX / WaitFrameEventX /
  GetFrameStatisticsX on a clock thread (high-resolution timer, last 0.6 ms spun; `frame_test` measures 4
  intervals of 16 667 us within 0.1 ms). 1.0.2.1 asks for 33 333 us (30 fps) with origin and event 2 at +0.
- **D3D12.X objects since** (details in `gpu-memory.md`): descriptor heaps, views (T# / V# / S# written into heap
  memory, `shims/d3d12x_views.cpp`), resources with the console's sizes (`gpu/gfx10_layout.cpp`), heaps and placed
  resources, root signatures, pipelines (kept as console binaries: their shaders are DXBC containers, and slot 57's
  descs point at code already in GPU memory), state blocks (55), query heaps, command signatures, `xg_xs.dll`.
  Command lists carry the console's inline-writer fields with no room, so every command arrives as a method; they
  aren't replayed yet (their methods are still probes). Fences keep their value in memory (slot 12).
- **DirectStorage** (`shims/xcrdapi.cpp`, our own inflate in `shims/inflate.cpp`): queues run requests in order on a
  thread each; the G: device's handles carry the IDStorageFile (+0) and an empty extents map (+32 / +40), where the
  console's package files keep theirs. BCPack (options bits 1-4 = the BC format, zlib first if bit 0) is decoded by
  `shims/bcpack.cpp` for BC1, BC3, BC4, BC5, BC6H and BC7: 12 or 16 rANS lanes run on the hardware's byte schedule
  (as rpf9-explorer's decoder models it), then per block an endpoint and an index reference (the last 256 blocks, or
  a dictionary entry before the first) plus residuals. The format's constant tables (models, index ranks, BC7
  control words, dictionaries) are generated into `bcpack_tables.h` by `tools/gen_bcpack_tables.py` from a table
  file. rpf9-explorer's built-in history agrees with them where it marks entries confirmed and often not where it
  marks them provisional. `bcpack_test` checks the decoder byte for byte on chunks of the title's texture pools
  (`tools/bcpack_samples.py`, data kept in `temp/`). A stream that doesn't decode is filled with zeros.
- **Runtime services** (`shims/services.cpp`): unknown areas get stubs that log the GDK function by name
  (`shims/gdk_api.inc`, from `tools/gen_gdk_api.py`). Written: XPackage, XNetworking (offline), XUser change events,
  XDisplay (SDR), XGame title id, XSystem (sandbox `RETAIL`, which 1.0.2.1 checks; a neutral console id),
  analytics info.
- **Audio hardware** (`shims/audio_hw.cpp`): AcpHal / xapu / chad answer as hardware with little to do (APU memory,
  audio-frame messages every 128 samples, decoder results of silence). No sound yet.
- **XUser**: one offline local user (signed in, adult, every privilege; tokens fail as offline), as VIctory serves it.
- **Swizzle** (`gpu/gfx10_swizzle.cpp`, equations from addrlib's tables via `tools/gen_swizzle.py`, Series X: 32
  pipes / 8 packers): element offsets, tile / detile of subresources; `xg_xs` slots 6, 8, 9, 10 use it.
- **Two call conventions**: methods returning a struct through a buffer are called (buffer, this) by some call sites and
  (this, buffer) by others (resource GetDesc); ours tell which argument is the object. Writing the desc over our own
  resource object was what broke the slot-70 views and later called a null method.
- **The PC device**: views of PC textures always get explicit, viewable descs (typeless and depth formats made typed);
  depth textures lose UAV use on the PC. An invalid view had removed the device (887A0001).
- **R** is the largest of 256 / 192 / 128 / 96 / 64 GiB that fits from below 4 GiB; under 192 GiB, the runtime starts itself
  again (up to 3 times, the new process in a job that ends with this one): something Windows places by ASLR at process
  start can be in the way, and 1.0.2.1 reserves 3+ GiB ranges.
- **Where it stops now:** nowhere: the render loop runs until stopped (three 300 s runs, ~11 000 submissions each),
  nothing drawn yet (command lists aren't replayed, shaders aren't translated). Next asked for: XGameSave.

## Build the runtime

```powershell
.\scripts\build_runtime.ps1
```

It builds `runtime\build\FlamingoPort.exe` (the launcher, the preparation and the runtime: one program, see "One
program" below) and the runtime's tests. It sets up MSVC through `vcvars64.bat` and builds at BelowNormal priority on
half the cores, with the CMake and Ninja of VS's "C++ CMake tools" component (the devkitPro MSYS `cmake` on PATH can't
build MSVC projects). A runtime source file (`src\`, `shims\`, `gpu\`) is added to `RT_SOURCES` in
`runtime\CMakeLists.txt`; the launcher's and the preparation's folders (`app\`, `core\`, `ui\`, `prep\`, `shaders\`,
`xlat\`, `cache\`) are globbed. The Vulkan layer and its tests need the Vulkan SDK headers.

`.\scripts\test_runtime.ps1` builds and runs the runtime tests (see "Vulkan layer" below).

## One program: launcher, tools and runtime (`FlamingoPort.exe`)

`.\scripts\build_runtime.ps1` builds `runtime\build\FlamingoPort.exe`, which is three things in one
Windows-subsystem program:

- the launcher;
- the preparation's tools, which replace the Python scripts and `make_game.py`;
- the runtime.

`runtime\CMakeLists.txt` lists the runtime's sources (`src/`, `shims/`, `gpu/`) and globs the seven folders below,
with `runtime\` as the include directory (`"core/fs.h"`, `"xlat/gen.h"`). The same build makes the tests. The old
`FlamingoPort.pyw`, `make_game.py`, `Build.bat` and `runtime-v2\` are gone (copies in `temp/removed/`).

### Layout (by responsibility)

| Folder | What it holds |
|---|---|
| `app/` | `entry.cpp` (`wWinMain`: launcher, `--run`, or a tool command); `tools_cli.cpp` (the tool commands); `cli.h` |
| `core/` | No knowledge of the game: `text` (UTF-8/16, formats), `fs` (paths, files, disks), `system` (environment, repository, SDK path), `hash` (SHA-1/256), `process` (job objects, child processes with captured output and a time limit), `json` |
| `ui/` | The launcher, laid out as the "Flamingo — pronto" design. `canvas` (Direct2D/DirectWrite, images from the exe's resources), `theme.h` (the design's palette and type, sizes, enums), `widgets` (buttons, icons, switches, choice cards, the stage bar), and `App` split by concern: `launcher_window` (resizable, sized to the screen, per-monitor DPI), `launcher_settings`, `launcher_prepare`, `launcher_play` (game process and splash), `launcher_page` (header with the cover, title and tools), `launcher_panels` (status card, folders, log), `launcher_drawer` (the side panel: settings and monitoring) |
| `prep/` | The preparation as stages. `versions` (supported versions, finding copies), `toolchain` (programs it needs), `preparation` (running stages, preflight, cache builds), `stages.cpp` (check, shaders, heavy), `assets.cpp` (models, texture dictionaries, texture pools and audio banks through `tools\rpf9dump`) |
| `shaders/` | Containers and native code. `dxbc` (parts, HASH digest, ISG1, stage), `carve`, `unpack`, `disasm` |
| `xlat/` | The RDNA → SPIR-V translator (below) |
| `cache/` | The DXIL cache (below) |

### The launcher's look

The launcher follows the user's design ("Flamingo — pronto (config e monitoramento)").

- **Art and icon.** The cover, the splash and the app icon are our own drawings: `assets/launcher/*.svg`, rasterized by `tools/gen_launcher_art.py` (Microsoft Edge, headless) into `cover.png`, `splash.png` and `flamingo.ico`. `runtime/app/flamingo.rc` puts them in the exe. Nothing is read from the game's copy: no logo, splash or icon of the title.
- **Layout.** It is worked out in DIPs from the window's width, following the design's flex rules:
  - a column at most 1100 wide;
  - the four tiles go 2x2 below 560;
  - the two folder cards stack below 656;
  - chips and action buttons wrap;
  - paths end in an ellipsis;
  - the page scrolls.
- **Window.** It is resizable down to 600x480 and opens at the design's 1280x880 where the screen has room (else 90% of the work area). It scales per monitor.
- **Side panel.** Monitoring and settings sit in a panel over the page, which scrolls on its own; Esc or a click outside closes it.
- **Fonts.** The design's Inter, Space Grotesk and JetBrains Mono are replaced by Segoe UI, Bahnschrift and Consolas, so no font is embedded.

### The translator (`xlat/`)

`xlat/` is `tools/rdna2spirv.py` and `tools/shader_ir.py` in C++. The output is meant to be the same text, byte for
byte, so the two can be compared over the corpus while both exist (see "Checking the port").

| File | Part |
|---|---|
| `ir.*` | The listing parser. The regular expressions are matched by hand: `std::regex` would take hours over the corpus. |
| `gen.h` | The generator's state, plus the helpers that keep Python's semantics |
| `gen_regs.cpp` | Operands, registers, EXEC, destinations, modifiers, SDWA, and the d3d12 rewrites in `emit` |
| `gen_memory.cpp` | Buffers, global and scalar memory, LDS |
| `gen_image.cpp` | Descriptor lookup, sampling, image loads, stores, atomics and queries |
| `gen_export.cpp` | Compares, exports, attributes |
| `ops.cpp` | The instruction dispatch: the Python's `elif` chain, in its order |
| `ops_vector_float.cpp`, `ops_vector_int.cpp`, `ops_half.cpp`, `ops_scalar.cpp`, `ops_lanes.cpp` | The instruction families |
| `assemble.cpp` | The module around the body |
| `program.cpp` | `translate()`: stage, inputs, reachable code, basic blocks |
| `formats.cpp` | GFX10 formats (Mesa's table) |

Porting rules, stated at the top of `gen.h`:

- **Evaluation order.** Lines are built from braced lists, which C++ evaluates left to right, as Python evaluates an
  f-string. Function arguments are evaluated in an unspecified order, and the order of `tmp()` calls is the output.
- **Errors.** Python's `IndexError`, `KeyError` and `ValueError` come back as `PyError`. A shader that made the
  Python fail fails the same way.
- **No function-local statics.** Tables sit at namespace scope, because the exe builds with `/Zc:threadSafeInit-`
  and the translator runs on many threads.

### The cache (`cache/`)

`cache/` is `tools/dxil_cache.py`, `pending_shaders.py` and `literal_samplers.py`, with the same files written:

- `<digest>.dxil`, `.fail`, `.vsin`;
- `index.json`, `translation.json`;
- `literal_samplers.txt`.

| File | Part |
|---|---|
| `toolchain` | The Vulkan SDK programs (spirv-as/val, spirv-cross, dxc), the scratch folders, `first_line` |
| `compile` | `to_hlsl` (translate, assemble, validate, SPIRV-Cross) and `run_dxc` (with its time limit) |
| `queue` | HLSL waiting for DXC, with leases for helpers |
| `server` | The helpers' HTTP protocol "1", unchanged, so `tools/worker` works as before |
| `progress` | Events for the launcher, `translation.json` |
| `builder` | The build |
| `pending` | The run-time shaders |
| `literal_samplers` | The literal S# scan |

Differences from the Python:

- **Threads, not processes.** Translation runs on threads in the one process.
- **Failure identity.** The identity on kept failures is `FP_TOOLS_ID`, a hash of the translator's and the cache's
  sources that CMake generates (`cmake/tools_id.cmake`). Before, it was a hash of the two `.py` files.
- **Firewall.** The helper server is `FlamingoPort.exe` itself. Windows asks once to let it listen on the local
  network.

### Other parts

- **Address space.** The launcher's DLLs (Direct2D, DirectWrite, WIC, shell32, bcrypt, iphlpapi, dwmapi) are
  delay-loaded, so a `--run` process gets the 256 GiB at `0x80000000` the runtime reserves. Linked normally, something
  they load at process start sat in the range `uma.cpp` reserves from below 4 GiB: "can't reserve even 64 GiB
  starting below 4 GiB".
- **Playing.** The game runs in a second process of the same exe, because the title takes over its process (its TLS
  block, its memory layout). Its window lives inside the launcher's, so the two show as one window with one taskbar
  button.
  - **Starting.** The launcher passes `RT_CONSOLE`, `RT_SHADER_CACHE`, `RT_SPLASH_EVENT` and `RT_PARENT_WINDOW` (its
    own window), and sends stdout and stderr to `logs\game.log`. The splash (our own art) fills the launcher's
    window meanwhile.
  - **First frame.** With `RT_PARENT_WINDOW`, `shims/window.cpp` makes the game's window a hidden `WS_CHILD` of the
    launcher's, per-monitor DPI aware like it. After the first `Present` it shows that window, posts `WM_GAME_SHOWN`
    (`WM_APP+3`, with the window) to the launcher, and then signals the event.
  - **Size.** The launcher keeps the game's window at the largest 16:9 that fits, centred, with black bars beside
    it. It moves it with `SWP_ASYNCWINDOWPOS`, so a game thread that stops answering doesn't hold the launcher.
  - **Full screen.** F11 or Alt+Enter, in the game (`WM_GAME_FULLSCREEN`) or in the launcher, switch the launcher's
    window to borderless full screen. DXGI's own Alt+Enter is turned off (`MakeWindowAssociation`).
  - **Leaving.** The close button ends the game (`WM_CLOSE` to its window) and brings the page back in the same
    place, as soon as the game's window is gone. The process lingers about two minutes handing memory back; the
    launcher shows "Encerrando…" meanwhile. If the launcher's window goes, the game's window gets `WM_DESTROY` and
    the game ends.
  - **Tools while playing.** A translation the launcher runs while the game is up starts its tools at idle priority.
  - **Without the launcher.** `FlamingoPort.exe --run` alone, without the variable, makes a window of its own as
    before.
- **Pipelines ahead of the game.** The preparation's stages are `check`, `shaders`, `pipelines` and the heavy
  shaders.
  - **What it does.** `pipelines` runs `FlamingoPort.exe --warm-pipelines` (with `RT_SHADER_CACHE`, as Jogar passes)
    and reads its "warm: N of M" lines. It makes every pipeline the game has asked for (`<cache>\pipelines.txt`,
    written by the runtime).
  - **Cost.** The driver keeps what it made, so a second run passes in milliseconds. Only new ones cost: 660
    pipelines took 19 min with 5 threads after a retranslation.
  - **When it runs.** Never while the game runs. The list exists once the game has run, so after each game session
    the ready card offers "Compilar N pipelines novos" (the lines the session added). A failure there doesn't stop
    the preparation: the game makes what's missing as it goes.
- **Tool commands.** Each runs one step on its own:
  - `--prep carve|unpack|disasm`
  - `--prep assets <game> <work> [--no-audio]` (`make_game.py`'s assets stage: builds `tools\rpf9dump` with the
    .NET 8 SDK the first time; an export whose manifest exists is skipped, so a stopped run continues)
  - `--xlat <.s or folder> [--target d3d12] [--hashes F]`
  - `--build-cache <disasm> <dxbc> <out> ...` (the arguments of `dxil_cache.py`)
  - `--pending <cache>`
  - `--literal-samplers <src> --out F`

### Checking the port

The carve, unpack and disassembly steps were checked against the Python outputs, and all were identical:

- the whole carve index and 2,000 random `.dxbc`;
- the whole `fssm_index.json` and 2,000 random `.fssm`;
- 300 random `.s`.

For the translator, `--xlat <disasm> --target d3d12 --cs-regs <cache>\cs --hashes F` writes, for each shader, either
`name<TAB>sha1<TAB>length` or `name<TAB>ERR<TAB>reason`. A script runs the Python snapshot the port was made from
(`temp/port_base`) over the same folder and writes the same lines; the two files are then compared. Changes made to
`tools/rdna2spirv.py` after that snapshot are ported by diffing against it.

## Asset status (no exe needed)

| Asset | State | Where |
|---|---|---|
| Archive data | Carved without the encrypted tables of contents. Most archives are 93–99% plain zlib (`levels.rpf` 98.3%, 1.1M files). Not zlib: `audio.rpf` (99.9%), about half of `story000.rpf` and `shared_minigame000.rpf`. | `<work>\carve\<archive>\`, `coverage.json` |
| Videos | 797 Bink 2 files (`KB2n`), cut out whole. | `*.bk2` in the carve folders |
| Shaders | 26,503 carved from `shaders.rpf`, native RDNA2 code unpacked (`FSSM`), and disassembly working. | `<work>\shaders\` (`.dxbc`, `fssm\`, `disasm\`) |
| Textures | **Solved.** Headers are in CPU segments: GPU pointer at q[k], u16 width/height/depth and DXGI format at q[k+4], mip count at byte 2 of q[k+5]. Data is AMD `SW_4K_S` tiled, largest mip first. The GPU segment is the blob run after the CPU blob. On 200 samples, 173 decode at least 1.5× smoother detiled than untiled (median 2.56×). Exports the top mip, slice 0, as DDS. **22,342 exported (1.23 GiB)**: every header whose GPU data follows its CPU blob. World and model textures in `levels.rpf` have the same blocker as meshes. GPU offsets must be 256-byte aligned, which drops the false headers in `navmeshes`/`anim`. | `tools\tex_headers.py`, `tools\tex_export.py`, `<work>\textures\` |
| Models | **Exporting** with `tools\rpf9dump` (built on breadbbbb/rpf9-explorer's Core; see below). Resources are found at page offset +16 after 16 zero bytes, followed by a zlib system blob with a drawable/fragment header and then the following zlib blocks as graphics data. Positions are quantized: `bias + scale * ((q >> 8) & 0xFFFFFF)`. Output is OBJ (positions, UVs, indices) plus a manifest with real model names (e.g. `cvar_int_grg2b_toys_1`), bounds, per-mesh shader index and material texture references (parameter hash, name hash, 64-bit asset key). My earlier `0x1380`/`0x1601` descriptor notes stand, but the "data not located" blocker was wrong. | `<work>\assets\<archive>\models\` |
| Texture dictionaries | Same tool: TXD resources become DDS per texture. | `<work>\assets\<archive>\textures\` |
| World textures (TTPF) | `data\ttdb\**\*.ttpf` (57 pools, 58 GiB) are loose RDS5 texture pools, **not** inside RPFs. rpf9-explorer's `TtpfReader` reads the catalog and detiles them; checked visually (`ttdb_graphics`: scope reticles, clean lines). Names are generated (`texture_N_<assetkey>`); models reference textures by asset key. Some rows are skipped (BCPack or unmapped shards). | `<work>\assets\ttpf\<pool>\` + `manifest.json` |
| Audio | `audio.rpf` holds raw (not zlib) **AWC** banks: Opus (codec 0x0B) and XMA2 streams. Exported as whole `.awc` files plus `audio_manifest.json` (stream ids, codec, channels, rate, duration). **Done: 7,162 banks, 27 GiB, 1,603,231 streams, about 1,324 hours** (codec 0x0B Opus 1,601,167; 5: 1,818; 2: 227; 0 PCM: 19). Decoding to WAV is a later step. | `<work>\assets\audio\awc\` |

### rpf9dump (bulk export)

`tools\rpf9dump` is a small CLI over the Core library from https://github.com/breadbbbb/rpf9-explorer (a friend's tool; its `Core` is copied into `tools\rpf9-explorer` at commit `af54933`, not a submodule, see `SOURCE.md` there; audited: no keys, no network, no custom build targets; one NuGet package, `BCnEncoder.Net 2.3.0`). It decrypts nothing. It carves validated resources, as `rpf9_carve.py` does, but with working model and texture decoders.

```
dotnet build tools\rpf9dump\rpf9dump.csproj -c Release -o <work>\tools\rpf9dump
rpf9dump models|textures <archive.rpf> <work>\assets
rpf9dump ttpf <pool.ttpf> <work>\assets\ttpf
rpf9dump audio <audio.rpf> <work>\assets
bash scripts/dump_assets.sh      # everything, log in <work>\assets\dump.log
```

Full-scan indexes are also saved to `%LOCALAPPDATA%\RpfPreview\Indexes`, so the rpf9-explorer GUI opens these archives with complete listings instead of its 16 MiB preview scan.

## Other prepared pieces

- `tools\shader_profile.py`: across the corpus there are 551 distinct RDNA mnemonics, and 204 of them cover 99% of
  all instructions (`<work>\shaders\profile.json`).
- `tools\shader_ir.py`: parses every disassembled instruction into opcode, typed operands (s/v registers and
  ranges, NSA register lists, constants, literals, export targets, interpolation attributes), modifiers and
  encoding bytes. On 1,000 shaders, 12 of 2.2M operands were left untyped before the last fix. This is the front end
  for an RDNA→SPIR-V translator.
- `tools\rtti_scan.py`: MSVC x64 RTTI scanner (class names, bases, vtables). Tested on the bundled DLLs; run it on
  the exe first.
- `tools\hashdict.py`: joaat hash→name dictionary (`<work>\hashes.json`, 1.9M entries, checked against
  `adder` = 0xB779A091). On day one, rerun with `--extra <exe>` so PSO field and type names resolve. Without the exe
  only about 1% do.
- Runtime shims: all 13 `xmem.dll` imports of the exe and bundled DLLs are shimmed, with signatures read off the
  exe's call sites (`runtime/shims/xmem.cpp` notes which fields are still guesses). Physical memory uses 64 KiB
  pages: `XMemAllocatePhysicalPages` makes a pagefile-backed section, `MEM_PHYSICAL` regions are Windows
  placeholders, and `XMemMapPhysicalPages[Scatter]` maps page views into them (`MapViewOfFile3`), so one page can
  show at several addresses with no AWE privilege. The exe already uses `MEM_RESERVE_PLACEHOLDER` /
  `MEM_REPLACE_PLACEHOLDER` with the desktop values. `runtime/test/xmem_test.cpp` covers the call patterns. The
  `FlamingoPort.exe --dry-run` of 1.0.2.1 leaves 24 imports unimplemented: `d3d12_xs` (2), `xg_xs` (2), `XCRDAPI`
  (`DStorageGetFactory`), and the audio hardware DLLs `AcpHal`, `xapu`, `chad` (19).
- PSO metadata is a new variant (`PSIN`, `LSCH` schema, `LENM`, `LSNS`, `LGID`, `LRSC`, `CHKS`, little-endian).
  CodeWalker's parser reads the container but not the new schema; dump it once the exe gives the field names.

## Shader translation (RDNA → SPIR-V)

`tools\rdna2spirv.py` (needs `spirv-as`/`spirv-val` on PATH: Vulkan SDK, or `spirv-tools` in WSL) translates parsed RDNA
code to SPIR-V assembly, assembles it with `spirv-as`, and validates it with `spirv-val` (Vulkan 1.2). Version 0
runs one lane per invocation, keeps VGPRs and SGPRs as uint arrays with exact bit patterns, treats EXEC as
all-ones, and covers branch-free vertex and pixel shaders with no memory loads: common float and integer ALU ops
and exports to `gl_Position`, params and MRTs. Unsupported instructions are reported by name, never guessed.
`VS_Passthrough` and `VS_BlurFilter` translate and validate. Coverage across the corpus and the blocking
instructions are in `<work>\shaders\spirv_run.log`. Branches (EXEC masks) and the descriptor/binding model for
`s_load`, `s_buffer_load`, `buffer_*` and `image_*` have since been added; current status is below.

Also handled now: 64-bit scalar moves, EXEC reads (all-ones), whole-quad mode on EXEC, `m0`, SCC, vector and scalar
compares, `v_mul_hi_u32`, `v_cvt_pkrtz_f16_f32` (rounds to nearest instead of toward zero, so it can be off by one
f16 ulp), and pixel interpolation (`v_interp_p1/p2/mov` become fragment inputs at their attribute's location). The
float MODE register (`s_setreg`) is skipped, so Vulkan's default denormal and rounding behaviour applies.

### GPU tests (RTX 4060 Ti)

`runtime\test\vk_harness.cpp` (build: `runtime\build\test\build_harness.cmd`, MSVC + the Vulkan 1.4 headers from
Hookshot's CPM cache, loading `vulkan-1.dll` at runtime) draws one triangle with a given VS + FS into an RGBA32F
target and dumps the floats. Test shaders are in `<work>\gputest`.

- **`VS_Passthrough` (translated) is bit-exact.** All 64×64 pixels are written, and `param0.xy` = (u, 1 − v) with max
  error **0**, with zw = (0, 1) as exported. This covers the VertexIndex input, integer/float ALU, inline constants,
  and position/param exports.
- **400 arithmetic-only translated pixel shaders** (no buffers, descriptors or subgroup ops), driven by a generic
  full-screen VS feeding locations 0–15: the driver compiled every pipeline, every draw completed, and every output
  was finite. This is a stability check, not a value check (there's no reference for these).
- **Texture path is exact** (`runtime\test\vk_texture_test.cpp`): translated `PS_PassthroughCopy` gives max error
  **0.0** over all 64×64 pixels against the expected nearest-sampled 8×8 test texture. That covers push-constant
  user data, a heap-relative `s_load_dwordx8` of the T#, the hash-table lookup of the T# and of the shader's
  literal S#, bindless sampling, `v_interp`, and the f16 compressed export. **It also caught a translator bug**
  that validation can't: compressed exports read the wrong register pair (LLVM prints `v4, v4, v5, v5`), giving
  (r,g,r,g). That's fixed.
- After the export fix, 2 of the 400 smoke-test shaders (`PS_ImposterShadow` variants) gave non-finite red values.
  The cause was round-to-nearest `pkrtz` overflowing to inf, fixed with exact RTZ. One variant is now fully finite.
  The other still has 1 inf and 4 NaN in red, and RTZ can't create those from finite values, so they come from the
  shader's own maths on the synthetic test inputs (hardware would pass them through too). That's consistent with
  hardware, but unconfirmed without a reference.

### Translator status and runtime contract

Full corpus (verified pixel-shader inputs incl. front face, ACO-confirmed 16-bit semantics, `vcc_hi` as an SGPR,
code-embedded data via `s_getpc_b64`, global memory, exact packed-f16 modifiers, SDWA selects): **13,971 of 26,503
shaders translated, all passing `spirv-val`** (Vulkan 1.2). That's **97.4% of all vertex and pixel shaders** (13,971
of 14,337).

The remaining 366 VS/PS:

| Count | Reason |
|---|---|
| 321 | PS that read unexplained system-input registers (raw barycentrics and so on), rejected on purpose |
| 10 | use `src_pops_exiting_wave_id` |
| 10 | embedded data that can't be re-anchored |
| 8 | `v_fma_f16` with op_sel |
| 4 | `image_sample_o` |
| rest | rare operands |

Mesh (9,282), compute (2,689) and geometry (195) shaders aren't attempted yet.

GPU re-test against the final SPIR-V: `VS_Passthrough` exact, texture test within RTZ f16 rounding (1.3e-5), and
**400/400** arithmetic-only PS finite and identical across two runs. Output: `<work>\shaders\spirv\` (`.spvasm` +
`.spv`), and the log is `<work>\shaders\spirv_run.log`.

How the generated SPIR-V expects the runtime to work:

| Piece | Contract |
|---|---|
| Execution model | One invocation = one lane. A wave = a subgroup, so cross-lane ops (`ds_swizzle`, `v_permlane*`, `v_readlane`/`v_writelane`, `v_mbcnt`) become subgroup shuffles. **Requires subgroup size == wave size** (wave32 maps directly onto NVIDIA GPUs; wave64 shaders need a 64-wide subgroup or emulation). |
| EXEC | A per-invocation `active` flag. VALU writes and exports are masked by it, scalar ops aren't, and `v_cmpx`, saveexec and EXEC writes update it. A pixel lane still inactive at the end is discarded. |
| Control flow | Any CFG becomes a dispatch loop (`pc` variable + `switch`), which is valid structured SPIR-V. |
| User data | **Push constants, 144 bytes:** dwords 0–31 = initial s0–s31 (the user SGPRs the console driver loads from the root signature: descriptor-table pointers, constants and so on), dwords 32–33 = the 64-bit **heap base**, dwords 34–35 = the **code base**: the Xbox VA where the runtime uploaded this shader's whole FSSM blob. The runtime has to reproduce the console driver's root-signature-to-user-SGPR mapping; that comes from the exe or driver behaviour. |
| Xbox GPU addresses | Shaders build Xbox GPU virtual addresses, and some force the high dword to 0 (descriptor tables in the low 4 GB). Every memory access goes to **heap base + Xbox VA**, so the runtime maps the console's GPU address window into one host buffer-device-address allocation. |
| Code-embedded data | 155 VS/PS had "undecodable words". They're **constant tables inside the code range** (float values like 0x3EF7…, plus zeros), read through `s_getpc_b64` + `s_add_u32`/`s_addc_u32` + `s_load` (147 of 155). The translator re-anchors each instruction to its true FSSM offset by matching encodings, translates only code reachable from entry (a branch into data is rejected), and turns `s_getpc_b64` into code base + next-instruction offset. **The runtime uploads each shader's FSSM blob into the heap and passes that VA as the code base.** 132 of 155 now translate. |
| Global memory | `global_load/store_dword*`: address = (64-bit SGPR base + zero-extended 32-bit VGPR offset, or a 64-bit VGPR address) + signed 12-bit offset, translated through the heap base. Inactive lanes don't access memory, and loads merge their previous values with OpPhi. |
| Literal samplers | Some shaders build their S# from immediates (for example `0x92, 0xC00000, 0x14500000, 0`). The runtime registers those in the descriptor hash table too, not only S#s the game binds: `tools/literal_samplers.py` finds them (forward dataflow of `s_mov` constants over each shader's basic blocks; a literal is listed only when all 4 dwords are the same constant on every path) and writes `<work>\shaders\literal_samplers.txt`, which `rt::gpu::register_literal_samplers` loads. S#s read from code-embedded constant tables (`s_getpc_b64` + `s_load`) aren't found by this scan yet. |
| Scalar/buffer memory | `s_load*`, `s_buffer_load*`, `buffer_load*` and `buffer_store*` use buffer device addresses (`PhysicalStorageBuffer64`). The base comes from the 64-bit SGPR pointer or the V# (dword0 \| dword1[15:0] << 32), and the stride is V# dword1[29:16]. **The runtime must put the game's GPU heap in buffer-device-addressable memory at the same addresses the game writes into descriptors** (or remap them). `num_records` bounds checks aren't modelled. |
| Image stores | `image_store` (1D, 2D, 2D array, 3D) writes through typed UAVs of the texture in its own format: bindless arrays of `RWTexture*<float4>` (spaces 12-15) and `RWTexture*<uint4>` (16-19) over the views' UAV half; the shader picks uint4 when the T#'s format (word 1 bits 20-28) is an integer one. dmask names the components vdata holds; the rest are 0. Image atomics stay on the r32ui arrays (spaces 5, 8). |
| LDS | `ds_*` on a `groupshared uint[]` sized from LDS_SIZE (at most 32 KiB, the PC's limit): read/write b32-b128, read2/write2 (st64), 8/16-bit reads and writes (d16 halves; sub-dword writes as two atomics), the add/sub/and/or/xor/min/max/wrxchg/cmpst atomics, ds_bpermute as a subgroup shuffle. Out-of-range addresses read 0 and drop writes, as the hardware's range check does. `s_barrier` is `GroupMemoryBarrierWithGroupSync`. |
| Re-anchoring | LLVM prints its own encoding, not the bytes it decoded: unused VOP3 SRC1/SRC2 fields come back as 0x80 where the console compiler left 0. Re-anchoring instructions past embedded constants ignores that difference. |
| Typed buffer loads | The format is V# word3[18:12] (GFX10_FORMAT, from Mesa's `gfx10-rsrc.json`, saved as `tools\gfx10_formats.json`). Supported: f32 ×1–4, u32 ×1/2/4, f16 ×1/2/4, unorm8 ×1/2/4, snorm8 ×4, uint8 ×4, unorm16 ×1/2/4, snorm16 ×2/4, 10-10-10-2 and 2-10-10-10 unorm. Others give 0. DST_SEL swizzle is applied. |
| Images and samplers | Bindless. Set 0: binding 0 = 2D sampled images, 1 = samplers, 2 = descriptor hash table (storage buffer), 3 = 2D-array images (**cube textures are bound here as 6-layer 2D-array views**: the shader already selected the face with `v_cube*`), 4 = 2D r32ui storage images, 5 = 1D, 6 = 3D, 7 = 3D r32ui storage images. |
| Descriptor hash table | `uint mask; entries[mask+1] of 10 uints: key[0..7], index, pad`. Key = the raw T# (8 dwords), or the S# (4 dwords) followed by four 0xFFFFFFFF. Hash = FNV-1a over the 8 key dwords, probed linearly for 8 slots. A miss gives index 0, so **the runtime keeps a fallback texture and sampler at index 0**. As the game binds descriptors, the runtime inserts each T#/S# with the bindless index of the Vulkan image view or sampler it created for it. |
| Vertex fetch | `s_swappc_b64` at the start of a VS calls a driver-generated fetch program that isn't in the blob. It's replaced by Vulkan vertex inputs: each `ISG1` input-signature element (DXBC container) becomes an input at **location = its D3D input register**, typed by its component type, and is written to **v(4 + 4·register + component)**. That rule was checked over all 788 fetch-calling shaders: in 783 every read-before-write VGPR is predicted by it (715 exact). The other 5 are rejected. **The runtime must bind vertex buffers by the game's D3D12 input layout, location = input register.** |
| PS system inputs | v0–v1 = barycentrics (consumed by `v_interp`, which becomes attribute inputs). **v2.. = the `SV_Position` components the shader reads, packed in order**. Verified on 2,531 of 3,000 PS: the VGPRs they read before writing equal exactly the position components their input signature reads. Filled from `FragCoord`: xy/z directly, and **w directly too**, because AMD's position-w register holds 1/w (Mesa lowers `load_frag_coord_w_rcp` straight to `frag_pos[3]`, `ac_nir_lower_intrinsics_to_args.c`), the same as Vulkan's `FragCoord.w`. **Front face** follows the used position components when the signature declares `SV_IsFrontFace`, as ±1.0 (Mesa: `load_front_face` = `front_face > 0`). Only enabled inputs are packed, in hardware order (Mesa `radv_shader_args.c`: barycentric pairs, POS_X/Y/Z/W, FRONT_FACE, ANCILLARY, SAMPLE_COVERAGE, POS_FIXED_PT). That explains the earlier "unverified" shaders: 102 read pos.xy + front face, and 76 read pos.x/y/w + front face. A PS that reads an unexplained register *inside* the system-input range (for example a raw barycentric) is **rejected** (about 21 of 3,000). Reads past the last hardware input are values undefined on some path, so zero stands in for them. All register arrays start at zero, so translated shaders are deterministic. |
| Inputs and outputs | VS: `v0` = `VertexIndex` (**unverified**: if the console runs vertex shaders as NGG, the input VGPR layout differs). `exp pos0` → `Position`, `exp paramN` → location N. PS: `v_interp_*` attribute N → input location N. `exp mrtN` → location N, `exp mrtz` x → `FragDepth`. |
| 16-bit results | From Mesa ACO (`aco_ir.cpp`: `instr_is_16bit`, `can_use_opsel`) for GFX10: **f16 VOP1/VOP2 ops** (`v_add/sub/mul/max/min_f16`, `v_fmac_f16`, `v_cvt_f16_f32`, f16 unary) and **opsel-capable VOP3 ops** (`v_fma_f16`, `_e64` forms of the 16-bit integer ops) are **partial writes that keep the high half**. The **VOP2 (`_e32`) 16-bit integer ops** (`v_lshrrev_b16`…) define the whole register, so the high half is modelled as zero. An op_sel destination bit writes the high half. The translator used to clear the high half on f16 ops; that's fixed. |
| SDWA | `_sdwa` forms: each source is narrowed to `src0_sel`/`src1_sel` (BYTE_n/WORD_n, zero-extended, or sign-extended under `sext()`) **before** neg/abs and the op. A VGPR result goes to `dst_sel`, with `dst_unused` setting the other bits: PAD = 0, SEXT = sign copies above the field, PRESERVE = old bits kept. VOPC has no `dst_sel`, and GFX10 `v_cmpx` has no written destination. **Until 2026-11-20 the selects were silently ignored.** That affected 3,826 of the 13,926 translated shaders: they passed `spirv-val` but computed wrong values. GPU-checked with `runtime/test/shaders/ps_sdwa.asm` and `ps_ops.asm` (also `v_pk_fma_f16`, `v_min3_u32`, `v_max3_i32`, `v_bfe_i32`): byte/word source selects, `sext()`, `UNUSED_PRESERVE` into WORD_1 and `UNUSED_SEXT` at BYTE_2 all match on every pixel. |
| Known approximations | `v_cvt_pkrtz_f16_f32` is now **exact round-toward-zero** (finite overflow gives ±65504, not inf). The GPU smoke test caught the old round-to-nearest version producing inf in `PS_ImposterShadow`. The other f16 packs (`v_cvt_f16_f32`, d16 results, mix ops) round to nearest, which matches the default mode. The MODE register (`s_setreg`) is ignored, so denormal and rounding behaviour is Vulkan's default. Whole-quad mode on non-EXEC masks is the identity. `v_mul_legacy_f32` is a plain multiply, while `v_mad_legacy_f32` keeps the 0×anything = 0 rule. |
| Compute input registers | The register block a compute pipeline is made with (slot 67 desc +8, 0x1c bytes) holds them; the runtime saves it per shader digest as `<cache>/cs/<digest>.regs` and the translator reads it. Layout, checked against every container's `PSV0` NumThreads (1,949 shaders): dword 3 = NUM_THREAD_X | Y << 16, dword 4 = NUM_THREAD_Z, dword 5 = COMPUTE_PGM_RSRC1, dword 6 = COMPUTE_PGM_RSRC2 (USER_SGPR bits 1-5, TGID_X/Y/Z_EN bits 7-9, TG_SIZE_EN bit 10, TIDIG_COMP_CNT bits 11-12, LDS_SIZE bits 15-23 in 512-byte units). So v0-v2 = local thread ID, then after the user SGPRs the enabled workgroup IDs and TG_SIZE (waves in the group bits 0-5, this wave bits 6-11; modelled as wave32). A compute shader whose pipeline the game hasn't made yet stays untranslated ("compute registers unknown") until a run saves its block. **Mesh:** every one of 2,969 reads exactly v5, plus s3, a pointer pair s[16:17] and s19-s23; not translated. |
| Not yet translated | Mesh (9,282), geometry and compute stages; `tbuffer_*`; a few rare ops. Mesh, geometry and compute need the per-stage hardware register setup (user-SGPR layout, system-value VGPRs). Get that from the FSSM metadata or the exe before translating them. The vertex-fetch call (`s_swappc_b64`) is handled: see Vertex fetch above. |

**Main blocker (before descriptor support was added): descriptor loads.** On a 3,000-shader sample, `s_load_dwordx4`/`x8` stop over half of them. Shaders
load texture, buffer and sampler descriptors through user-data SGPR pointers, and how the game's root signatures
map onto those SGPRs is set up by game code. Read that mapping from the exe (root signature creation plus the
FSSM user-data metadata) before building the binding layer; guessing it would give silent rendering bugs. The
other blockers are mesh shaders (9,282, which need a mesh-stage entry model), the vertex-fetch subroutine call
(`s_swappc_b64`), geometry and compute stages, and compressed exports.

### Translator regression checks

- `python -I tests/test_translator.py` translates the hand-written shaders in `tests/translator/cases/` and
  `runtime/test/shaders/*.asm` and compares them with `tests/translator/expected/`. It also validates them when
  `spirv-as`/`spirv-val` are on PATH. After an intended change, run it with `--update` and commit the new references
  with the change. It needs no game data, so run it before every translator commit.
- On the full corpus, `tools/spirv_snapshot.py save` before a change and `diff` after it lists every shader whose
  output changed, newly translates or stopped translating. A change meant to affect one instruction should only
  change shaders that use it. The SDWA bug (3,826 shaders valid but wrong) is the kind of change this exposes.
- Neither checks values. Value checks stay on the GPU harness.
- `python -I tests/test_literal_samplers.py` checks the literal-sampler scan on hand-written control flow
  (straight line, loaded words, overwrites, VOP3b carry-outs, if/else joins, loop back edges).

## Vulkan layer (`runtime/gpu`)

The runtime side of the contract above, as a static library (`rt_gpu`). It needs only the Vulkan headers to build
(`$VULKAN_SDK/Include`); `vulkan-1.dll` is loaded at runtime.

| Piece | File | What it does |
|---|---|---|
| Device | `device.cpp` | Vulkan 1.2 device with every feature the generated SPIR-V declares (Int64, BDA, descriptor indexing with update-after-bind, MinLod, subgroup shuffle in VS/PS, mirror-clamp). Lists everything missing at once. Records the subgroup size (32 on NVIDIA, which matches wave32 shaders). |
| GPU heap | `heap.cpp` | The Xbox GPU VA window as one buffer: `[0x10000, 0x10000 + size)`, below 4 GiB. Persistently mapped (`cpu(va)`), since the game writes descriptors and constants through CPU pointers. Device-local host-visible memory when resizable BAR allows. `heap_base()` = BDA − base VA goes to push-constant dwords 32–33. `upload()` places a shader's FSSM blob and returns the code base. |
| Hash table | `desc_table.cpp` | T#/S# → bindless index, the same layout, hash and probing as `desc_lookup`. Grows by doubling when an 8-slot window is full. Slot occupancy is tracked on the CPU because the shader compares every probed slot. |
| Bindless set | `bindless.cpp` | Bindings 0–7, partially bound, updatable after bind and while unused-but-pending. Image indices are shared across the six image bindings: each index holds the real view in its own binding and that binding's fallback in the others, so sampling with the wrong dimensionality reads a fallback. Index 0 is the fallback everywhere (magenta, zero for storage, nearest/clamp sampler). Also creates the pipeline layout (set 0 + 144 bytes of push constants). |
| Samplers | `ssharp.cpp` | GFX10 S# → `VkSamplerCreateInfo`, from Mesa's `sid.h` layout. `bind_ssharp` creates one sampler per distinct S#. Register border colours, half-border modes and min/max reduction are approximated and logged once. **Not checked against game captures yet.** |
| Literal samplers | `literal_samplers.cpp` | Loads `literal_samplers.txt` (one S# per line) and `bind_ssharp`s each entry, so samplers built from immediates resolve. |
| Vertex input | `vertex_input.cpp` | D3D12 input layout + the shader's ISG1 → Vulkan attributes at location = input register, with append-aligned offsets resolved. Strides are left to dynamic state. Reports unmatched elements, unmapped formats, float/int mismatches and step rates above 1. |

Tests (`scripts\test_runtime.ps1`, add `-Validation` for the Khronos validation layer):

- `gpu_unit_test`: CPU-only checks of the allocator (random churn, coalescing), the hash table (growth to 3,000
  keys, removal, dirty ranges), S# decoding (including the literal S# above) and ISG1/vertex-input building.
- `gpu_layer_test`: draws translated hand-written shaders through the layer. A T# and S# written into the heap at
  allocated VAs are read by `PS_SampleHeapDescriptors` through s4, looked up in a hash table that was started at 8
  slots and forced to grow five times, and sampled: every pixel equals the expected texel. An unregistered T# gives
  the fallback on every pixel. `ps_sdwa` and `ps_ops` produce their documented bits. Passes on an RTX 3070
  (driver Vulkan 1.4.341) with no validation messages.

Not done yet: S#s that shaders load from their own embedded constant tables (only immediates are listed), creating
images from T# (format, dimensions, tiling: textures in the heap are AMD-tiled, see
`tools\amd_swizzle.py`), render targets and PSO creation from the game's D3D12 calls, and command submission. Those
need the exe's D3D12 usage.

## Day one, in order

1. `scripts\first_look.ps1 -Exe ...` checks the exe is plain, lists the imports to shim, and does a dry run.
2. Find the RPF9 table-of-contents key and decryption in the exe, then add a table reader to `rpf9_carve.py`. Carved
   blobs then get real names and types, and the remaining unread data shows what it is.
3. Find the texture and mesh resource structures in the exe (their loaders give exact field layouts), which turns
   carved blobs into named textures and meshes.
4. Shader translation: RDNA2 disassembly to SPIR-V, starting with the most-used shaders.

## Disassembling shaders

The AMDGPU disassembler needs LLVM 21+ (`llvm-mc`). The official Windows LLVM installer has no AMDGPU target, but
conda-forge's Windows `llvm-tools` package does (checked with 23.1.6: `llvm-mc --version` lists `amdgcn`). Its
`llvm-mc.exe` is static apart from `zlib.dll` and `zstd.dll`, which come from conda-forge's `libzlib` and `zstd`
packages. The `.conda` files are zip archives holding a `pkg-*.tar.zst`, so the four files can be extracted without
installing conda (Python 3.14 reads zstd). Put them in one folder and set `LLVM_MC` to the `.exe`:

```
set LLVM_MC=C:\tools\llvm-23.1.6\llvm-mc.exe
python -I tools\shader_disasm.py <work>\shaders\dxbc\fssm --out <work>\shaders\disasm --mcpu gfx1013
```

Or in WSL with the `llvm-21` package (`/usr/lib/llvm-21/bin/llvm-mc`):

```
LLVM_MC=/usr/lib/llvm-21/bin/llvm-mc python3 -I <repo>/tools/shader_disasm.py <work>/shaders/fssm
```

Code starts at the u32 at FSSM offset 0xC0, plus 0x28, and ends at the last `s_endpgm`. Use `--mcpu gfx1013`. On
300 random shaders it decoded 284 cleanly (134 invalid words in total), against 201 clean and 6,560 invalid for
gfx1030 and 283 clean for gfx1010–1012.

## Baseline already recorded

- `reports/manifest.json`: SHA-256 of all 108 files in the game folder (208.5 GiB). Run
  `python -I tools\manifest.py verify <game>` to check a copy against it.
- `reports/rpf_probe.json`: all 22 archives are **RPF9**. Five have tables that look packed or encrypted (`levels`,
  `movies`, `core_bvh4`, `navmeshes`, `data`).
- `reports/pe/`: the 16 bundled DLLs are plain x86-64 PE with no encrypted sections. Through the loader they need
  only `xmem.dll` (`XMemAlloc` and `XMemFree` are shimmed; `XMemVirtualQuery` and `XMemGetWorkingSetStatistics` aren't
  yet) and two `d3d12_xs.dll` root-signature calls. Every other import resolves on desktop Windows.

## Mesh shaders: what the entry looks like (to be checked against the title's shaders)

The open world's geometry is drawn by indirect DispatchMesh calls (list slot 61, one-argument signatures of type 10,
count 1) into six targets. The 9,282 mesh-stage shaders aren't translated yet. The VIctory project (GPL-3; used as a
description of the format, none of its code is here) translates them, and describes the stage like this:

- A mesh program runs as one NGG stage. A thread's index in its group is the wave's index (s3 bits 24-27) times the
  wave size plus the lane (`v_mbcnt` of the lanes below it). The group's id comes from v5 of the first lane
  (`v_readfirstlane`), as group index times group size. This agrees with what was measured here: every one of 2,969
  mesh shaders reads exactly v5, plus s3, a pointer pair s[16:17] and s19-s23.
- Lane n exports vertex n (`exp pos`, `exp param`) and primitive n (`exp prim`: three 9-bit indices, each with an edge flag in the bit above, at bits 0, 10
  and 20, bit 31 culls the primitive). Bit 7 of a param marks a per-primitive output.
- The output counts come with `s_sendmsg` GS_ALLOC_REQ (id 9): m0 = vertices | primitives << 12, which is
  SetMeshOutputCounts.
- Threads, maximum vertices and primitives and the topology are in the stage data (at 0x448 there; clip control at
  0x194).
- The title's mesh programs are one-dimensional: DispatchMesh(x, 1, 1). In pipeline streams the console numbers the
  amplification and mesh subobjects 23 and 24 (the PC's are 24 and 25).
- On the PC it makes a DXIL mesh pipeline with the title's pixel shader. A console wave is 64 lanes and NVIDIA's are
  32: one wave is run as two halves of a group, with cross-lane operations (ballots, readlane, mbcnt, divergent
  branches) going through group-shared memory.

The translator here already has the output side in a test mode (`MS_TEST`: exports to mesh outputs through SPIR-V
MeshEXT, HLSL and DXIL ms_6_5, with every input register zero).
