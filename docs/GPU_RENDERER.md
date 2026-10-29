# deko3d renderer plan

Today the DS GPU is emulated on the CPU with upstream's software renderers
(`SoftRenderer`: `SoftRenderer2D` x2 + `SoftRenderer3D`). deko3d only draws the borealis UI.
This file is the plan for moving the emulated GPU onto the Switch GPU with deko3d. It
records what the old Switch port did, what upstream offers now, and the stages.

## What the old port had

umby24/melonDS (`switch-new`) carries deko3d renderers by RSDuck, written in 2021 against
the melonDS of that time (global `GPU::`, `GPU2D::`, `GPU3D::` namespaces):

- `src/GPU3D_Deko.cpp` (~1,450 lines): the compute-shader 3D rasteriser (the design that
  became upstream's `ComputeRenderer3D`), with shaders in `src/shaders/GPU3D_Comp.glsl`
  (GLSL 4.60, compiled offline to `.dksh` per variant by `compile_shader_instance` in
  `src/frontend/switch/CMakeLists.txt`).
- `src/GPU2D_Deko.cpp` (~2,000 lines): BG/OBJ/compositing/capture on the GPU, with
  `src/shaders/{BGText,BGAffine,OBJ,ComposeBGOBJ,...}_*.glsl`.
- `src/frontend/switch/{Gfx,GpuMemHeap,CmdMemRing,UploadBuffer}`: device, memory heap and
  command ring helpers. The port owned the display, so it presented directly.

None of it builds against today's core. It is a reference for the deko3d side (shaders
already adapted to deko3d's binding rules, memory and fence handling), not code to copy.

## What upstream offers now (mds_upstream @ 7d8dea42)

- `melonDS::Renderer` (`GPU.h`) is the whole-GPU interface: `DrawScanline`, `DrawSprites`,
  `VBlank`, `VBlankEnd`, `AllocCapture` / `SyncVRAMCapture`, `GetFramebuffers`,
  `SetRenderSettings` (`ScaleFactor`, `HiresCoordinates`, `Threaded`), savestate hooks. It
  owns `Rend2D_A`, `Rend2D_B` (`Renderer2D`) and `Rend3D` (`Renderer3D`: `RenderFrame`,
  `FinishRendering`, `RestartFrame`, `GetLine`). The frontend installs one with
  `NDS::SetRenderer` (see `frontend/qt_sdl/EmuThread.cpp`).
- `GLRenderer` (`GPU_OpenGL.cpp`, ~900 lines) + `GLRenderer2D` (`GPU2D_OpenGL.cpp`, ~1,900)
  does 2D, compositing and display capture on the GPU, with either `GLRenderer3D` or
  `ComputeRenderer3D` (`GPU3D_Compute.cpp`, ~1,300; shaders as strings in
  `GPU3D_Compute_shaders.h`, variants chosen with `#define`s at runtime). Shaders for 2D are
  in `OpenGL_shaders/*.glsl` (GLSL 1.40).
- `Texcache<Loader, Handle>` (`GPU3D_Texcache.h`) is generic; the GL instance is 35 lines.

So the structure to mirror is upstream's GL renderer, class for class, with deko3d calls
in place of GL and offline-compiled shaders.

## Constraints on the Switch

- **One device, borealis owns it.** `brls::SwitchVideoContext` exposes `getDeko3dDevice()`,
  `getQueue()`, image descriptor slots, and fence helpers. The renderer uses that device
  and creates its **own queue** (deko3d queues aren't thread-safe and the renderer runs on
  the emu thread). Hand-off to the UI is by fences.
- **Output into NanoVG.** NanoVG only draws its own textures. The borealis patch
  `DkRenderer::GetTextureImage` gives the `dk::Image` behind one, so the UI can copy or
  blit the renderer's output into the game image on borealis' queue
  (`gpu::FrameUploader` already does this for CPU frames).
- **Shaders compile offline.** deko3d has no runtime compiler. `uam` is in the devkitPro
  image (`/opt/devkitpro/tools/bin/uam`); each variant becomes a `.dksh` in RomFS. GLSL needs
  explicit `layout(binding = N)` for every resource.
- **Patches stay small.** New renderer files live in `switch/source/gpu/`, not in
  `mds_upstream/`. They include core headers and subclass core classes; core edits are only
  for access (e.g. a `friend` or a `protected`) and go in `patches/melonDS.patch`.

## Stages

**R0: plumbing (done in code, untested on hardware).**
- `gpu::FrameUploader`: CPU frames reach the game image by a GPU copy on borealis' queue,
  without the per-frame `waitIdle` of `nvgUpdateImage`.
- borealis patch `GetTextureImage`.

**R1: compute 3D on the GPU, 2D stays on the CPU (code exists, builds, untested on hardware).**
- Done as planned: `switch/source/gpu/deko_renderer.cpp` (`HybridRenderer`) and
  `deko_renderer3d.cpp` (`DekoRenderer3D`), option `renderer` (Settings → Emulation,
  applies to the next game).
- Shaders: `switch/tools/gen_compute_shaders.cpp` is a host tool, built and run at CMake
  configure time, that includes upstream's `GPU3D_Compute_shaders.h` and writes all 33
  variants exactly as `ComputeRenderer3D::CompileShader` assembles them, with three
  text adaptations (loose uniforms → a uniform block; the `SetupIndices` texel buffer → a
  storage buffer; the final pass writes packed RGB6A5 to a storage buffer instead of a
  normalised image), plus a faster `Div64_32_32` (below). `uam` compiles each into `romfs:/shaders/compute3d/`. If upstream
  changes a replaced line, the tool fails the configure step with the pattern's name.
- Display captures used as textures come from VRAM like any texture (the CPU 2D engines
  write captures to VRAM), so upstream's capture-texture path is not used.
- Synchronisation: `RenderFrame` waits for the previous frame's fence, records
  everything into one command list on the renderer's own queue and submits it;
  `GetLine` waits for that fence once per frame. Texture uploads go through an 8 MB
  staging buffer per frame.

The original plan:
- `gpu::DekoRenderer : SoftRenderer`, which swaps in `gpu::DekoRenderer3D` as `Rend3D`.
  Override `PreSavestate`, `PostSavestate` and `SetRenderSettings`: the base versions
  `dynamic_cast` to `SoftRenderer3D`.
- `DekoRenderer3D` ports `ComputeRenderer3D`:
  - `RenderFrame` records the compute passes on the renderer's queue.
  - `GetLine` waits on the frame fence once, then returns lines from a CPU-cached readback
    buffer, applying `RenderXPos` scrolling as `SoftRenderer3D::GetLine` does.
  - This is how desktop melonDS used its GL 3D renderer before the GL 2D renderer existed.
- Shader variants come from upstream's `GPU3D_Compute_shaders.h`, extracted to `.glsl` at
  build time and given deko3d bindings. The old port's `GPU3D_Comp.glsl` shows the binding
  changes.
- Texture cache: `Texcache<DekoTexLoader, ...>` modelled on `GPU3D_TexcacheOpenGL`.
- Option `renderer = software | deko3d`, read at game load, with a fallback to software if
  init fails.
- Gain: the 3D rasteriser, the most expensive part for 3D games, leaves the CPU.
- Cost: a 256x192 readback per frame (unified memory, so it's cheap; the risk is the
  latency of waiting on the fence).

**R2: 2D and compositing on the GPU.**
- Port `GLRenderer` + `GLRenderer2D` as `DekoRenderer` / `DekoRenderer2D`, including
  display capture.
- `GetFramebuffers` is replaced by a GPU output image that the UI blits into the game image
  after waiting on its fence. The CPU frame path and `acquire_frame` stay for the software
  renderer.
- Gain: no readback; the CPU only runs the ARM cores.

**R3: upscaling.**
- `ScaleFactor` / `HiresCoordinates` already flow through `SetRenderSettings`.
- The game image grows to `256*N x 384*N`, and an option is added.

## Open questions

- ~~Does `uam` accept upstream's compute shaders?~~ Yes: all 33 variants compile with
  only the three adaptations above (2026-10-29).
- First hardware test (2026-10-29, SM64DS): slower than the software renderer with
  threaded 3D (fast forward reaches less), and the image looked less stable. Likely
  causes:
  - The threaded software renderer runs all of the 3D on another core. R1 runs
    upstream's CPU setup (span setup, texture decoding) on the emu thread, and the
    emu thread then waits for the GPU at line 0, only ~48 scanlines after line 215.
  - Stats are now logged every 600 frames (`[gpu] 3D over ...`): CPU setup time vs GPU
    wait time.
  - Next steps depend on the split:
    - setup dominates: move it to a worker thread, as SoftRenderer3D does;
    - the wait dominates: an option for one frame of 3D latency, or R2, where nothing
      waits on the CPU.
- Fixed after that test:
  - Span limits that upstream only asserts could overrun the arrays (and the heap) in
    big scenes; polygons that don't fit are now dropped and logged.
  - The output buffer is GPU-uncached, so the CPU can't read GPU-cached stale data.
- Second test (same day): flicker gone. GPU timestamps show the frame waits ~0.3-0.9 ms
  in the queue and the GPU then works 5-9 ms for 400-650 polygons, so the compute
  renderer itself is the cost on this GPU, not scheduling or the CPU setup (~0.5 ms).
  Changes:
  - CPU per-frame data now goes through an uncached staging buffer that the GPU copies
    into cached buffers. That keeps it coherent without making the shaders read
    uncached memory.
  - Two frame slots, and option `gpu_3d_delay`: show each 3D frame one frame late, so the
    emu thread doesn't wait for the GPU (the GPU gets a whole frame).
  - Making the compute renderer itself cheaper on Tegra (fewer barriers, profiling the
    passes) is the other lever, and still open.
- Third test (2026-10-29), `gpu_3d_delay = on`:
  - The emu thread waits 0.00 ms; the GPU still works ~4-5.5 ms per frame for ~250
    polygons (Pokémon Platinum).
  - The SM64DS title-screen slowdown (Mario's face) is gone, and fast forward reaches
    about what the software renderer does.
  - So R1 now matches software speed and frees the 3D thread's CPU core. Beating software
    needs a cheaper GPU frame: per-pass timestamps to find the expensive passes; ~4 ms for
    250 polygons suggests fixed per-pass cost (barriers, full-screen passes) dominates.
- Per-pass GPU timestamps (logged as `[gpu] 3D passes, ...`), 2026-10-29:
  - Rasterise is 55-65% of the frame in every scene (SM64DS title, ~1,100 polygons:
    4.4-5.0 of 7.4-8.1 ms; Platinum, ~300 polygons: 2.4-3.0 of 4.3-5.0 ms).
  - Depth blend is ~0.45 ms even with 6 polygons. Clear+spans ~1 ms, bin ~0.55 ms at
    1,100 polygons.
  - Only 1-16 variants per frame, so dispatch overhead isn't the problem; per-pixel cost
    is.
- Changes from that:
  - The per-pixel `Div64_32_32` (a software 64/32 division) is replaced in the generator
    by a float estimate plus an exact `umulExtended` correction. For its callers the
    quotient is <= 512, so the result is identical: checked against exact division over
    160M random inputs in the callers' ranges, including a float divide made 3 ulps
    worse than IEEE.
  - Option `gpu_clock` (stock / 460.8 / 614.4 / 768 MHz, never below the clock the app
    started with): handheld apps run the GPU at 384 MHz by default.
- Result (2026-10-29, GPU frame time, before -> after the division change):
  - SM64DS title, ~1,100 polygons: 7.4-8.1 -> 4.1-4.4 ms handheld; 2.2-3.0 ms docked.
    Rasterise 4.4-5.0 -> 2.2-2.4 ms; clear+spans ~1.0 -> ~0.6 ms.
  - Platinum, ~300 polygons: 4.3-5.0 -> 2.4-2.5 ms handheld; 1.7-1.9 ms docked.
  - With the GPU this fast, the emu thread's own cost (~0.4-1 ms of CPU setup per frame)
    is what's left between R1 and "free 3D".
- Indirect dispatch and atomics: the old port's compute renderer used them under deko3d,
  so they should work, but upstream's newer shaders may differ.
- Save states mid-frame and `RestartFrame`: the GL renderer's behaviour is the reference.
