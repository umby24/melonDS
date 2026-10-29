#include "gpu/deko_renderer.h"

#include <GPU_Soft.h>
#include <NDS.h>

#include "gpu/deko_renderer3d.h"

namespace gpu {

namespace {

// SoftRenderer with a different Rend3D. SoftRenderer's 2D engines only call
// Rend3D->GetLine(); its savestate and settings hooks assume a SoftRenderer3D, so they
// are replaced (no render thread to stop or restart; the 3D engine has no settings at
// native resolution).
class HybridRenderer : public melonDS::SoftRenderer {
  public:
    HybridRenderer(melonDS::NDS& nds, std::unique_ptr<melonDS::Renderer3D> rend3d)
        : SoftRenderer(nds) {
        Rend3D = std::move(rend3d);
    }

    bool Init() override { return Rend3D->Init(); }
    void PreSavestate() override {}
    void PostSavestate() override {}
    void SetRenderSettings(melonDS::RendererSettings&) override {}
};

}  // namespace

std::unique_ptr<melonDS::Renderer> make_hybrid_renderer(melonDS::NDS& nds, bool delay) {
    auto rend3d = make_deko_renderer3d(nds.GPU.GPU3D, delay);
    if (!rend3d)
        return nullptr;
    return std::make_unique<HybridRenderer>(nds, std::move(rend3d));
}

}  // namespace gpu
