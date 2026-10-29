#pragma once
// melonDS renderers that use the Switch GPU (deko3d), installed with NDS::SetRenderer.
//
// Stage R1 of docs/GPU_RENDERER.md: upstream's software renderer (2D engines, compositing
// and display capture on the CPU) with its 3D engine replaced by a deko3d port of
// upstream's compute-shader renderer (ComputeRenderer3D). The 3D image is read back for
// the CPU 2D engines one frame at a time. Native resolution only.

#include <memory>

namespace melonDS {
class NDS;
class Renderer;
}  // namespace melonDS

namespace gpu {

// nullptr if deko3d isn't available (no borealis video context, shaders missing from
// RomFS). NDS::SetRenderer calls Init() and falls back to the software renderer if it
// fails. Create on any thread; the renderer then runs on whichever thread runs the core.
// `delay`: show each 3D frame one frame later than hardware does, so the emulation never
// waits for the GPU (the 3D then lags the 2D layers by a frame).
std::unique_ptr<melonDS::Renderer> make_hybrid_renderer(melonDS::NDS& nds, bool delay);

}  // namespace gpu
