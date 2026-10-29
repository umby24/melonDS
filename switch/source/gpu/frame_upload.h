#pragma once
// GPU-side updates of a NanoVG image. nvgUpdateImage records a one-off copy and then
// waits for borealis' whole queue to go idle, every call; this queues the copy on that
// queue ahead of the frame's NanoVG drawing and only waits when it reuses a staging
// buffer that is still in flight (three are kept).
//
// UI thread only. Construct after borealis is up and destroy before it exits (a view
// member is fine: borealis deletes activities before its video context).

#include <cstddef>
#include <memory>

struct NVGcontext;

namespace gpu {

class FrameUploader {
  public:
    FrameUploader();
    ~FrameUploader();  // waits for its copies in flight

    // Replaces the contents of NanoVG RGBA image `image` (w x h) with `rgba` (tightly
    // packed RGBA8). false if that isn't possible; use nvgUpdateImage instead.
    bool upload(NVGcontext* vg, int image, const void* rgba, unsigned w, unsigned h);

    // Waits for the copies in flight. Call before deleting an image uploaded to.
    void finish();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu
