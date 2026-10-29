#include "gpu/frame_upload.h"

#include <borealis.hpp>
#include <borealis/platforms/switch/switch_video.hpp>
#include <deko3d.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gpu {

namespace {

constexpr int kSlots = 3;
constexpr uint32_t kCmdSize = DK_MEMBLOCK_ALIGNMENT;  // per slot; a copy is a few words

uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

brls::SwitchVideoContext* video() {
    return static_cast<brls::SwitchVideoContext*>(brls::Application::getPlatform()->getVideoContext());
}

}  // namespace

struct FrameUploader::Impl {
    dk::Device device;
    dk::Queue queue;
    dk::UniqueMemBlock cmd_mem;
    dk::UniqueMemBlock staging;  // kSlots * slot_size, CPU-written, GPU-read
    uint32_t slot_size = 0;
    dk::UniqueCmdBuf cmd[kSlots];
    dk::Fence done[kSlots];
    bool in_flight[kSlots] = {};
    int next = 0;

    void wait_all() {
        queue.flush();  // borealis flushes when it presents; a wait may come before that
        for (int i = 0; i < kSlots; ++i) {
            if (in_flight[i]) {
                done[i].wait();
                in_flight[i] = false;
            }
        }
    }

    bool ensure_staging(uint32_t bytes) {
        uint32_t size = align_up(bytes, DK_MEMBLOCK_ALIGNMENT);
        if (size <= slot_size)
            return true;
        wait_all();
        staging = dk::MemBlockMaker{device, size * kSlots}
                      .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                      .create();
        slot_size = staging ? size : 0;
        return slot_size != 0;
    }
};

FrameUploader::FrameUploader() {
    auto* v = video();
    if (!v)
        return;
    auto impl = std::make_unique<Impl>();
    impl->device = v->getDeko3dDevice();
    impl->queue = v->getQueue();
    impl->cmd_mem = dk::MemBlockMaker{impl->device, kCmdSize * kSlots}
                        .setFlags(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)
                        .create();
    if (!impl->cmd_mem) {
        fprintf(stderr, "[gpu] no command memory; frames go through nvgUpdateImage\n");
        return;
    }
    for (auto& c : impl->cmd)
        c = dk::CmdBufMaker{impl->device}.create();
    impl_ = std::move(impl);
}

FrameUploader::~FrameUploader() {
    finish();
}

void FrameUploader::finish() {
    if (impl_)
        impl_->wait_all();
}

bool FrameUploader::upload(NVGcontext* vg, int image, const void* rgba, unsigned w, unsigned h) {
    if (!impl_ || !vg || !image)
        return false;
    auto* ctx = static_cast<DKNVGcontext*>(nvgInternalParams(vg)->userPtr);
    dk::Image* target = ctx && ctx->renderer ? ctx->renderer->GetTextureImage(image) : nullptr;
    if (!target)
        return false;

    Impl& d = *impl_;
    uint32_t bytes = uint32_t(w) * h * 4;
    if (!d.ensure_staging(bytes))
        return false;

    int s = d.next;
    d.next = (s + 1) % kSlots;
    if (d.in_flight[s]) {
        d.queue.flush();
        d.done[s].wait();
        d.in_flight[s] = false;
    }

    uint32_t offset = uint32_t(s) * d.slot_size;
    memcpy(static_cast<uint8_t*>(d.staging.getCpuAddr()) + offset, rgba, bytes);

    dk::CmdBuf cmd = d.cmd[s];
    cmd.clear();
    cmd.addMemory(d.cmd_mem, uint32_t(s) * kCmdSize, kCmdSize);
    // Earlier draws sampling the image must be done before it is overwritten, and the
    // texture cache must not serve the old contents afterwards.
    cmd.barrier(DkBarrier_Fragments, 0);
    cmd.copyBufferToImage({d.staging.getGpuAddr() + offset}, dk::ImageView{*target},
                          {0, 0, 0, w, h, 1});
    cmd.barrier(DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
    cmd.signalFence(d.done[s]);
    d.queue.submitCommands(cmd.finishList());
    d.in_flight[s] = true;
    return true;
}

}  // namespace gpu
