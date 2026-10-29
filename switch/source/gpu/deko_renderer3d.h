#pragma once
// Internal to gpu/: the deko3d port of upstream's ComputeRenderer3D. Use
// gpu/deko_renderer.h from outside.

#include <memory>

namespace melonDS {
class GPU3D;
class Renderer3D;
}  // namespace melonDS

namespace gpu {

// The device and the shaders are checked here, the rest in Init(). nullptr if deko3d
// can't be used. `delay`: see make_hybrid_renderer.
std::unique_ptr<melonDS::Renderer3D> make_deko_renderer3d(melonDS::GPU3D& gpu3d, bool delay);

}  // namespace gpu
